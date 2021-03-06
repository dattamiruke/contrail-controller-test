/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fstream>
#include <boost/asio/ip/host_name.hpp>
#include <boost/foreach.hpp>
#include <boost/program_options.hpp>
#include <boost/tokenizer.hpp>
#include "base/logging.h"
#include "base/cpuinfo.h"
#include "base/task.h"
#include "base/task_trigger.h"
#include "base/timer.h"
#include "io/event_manager.h"
#include "QEOpServerProxy.h"
#include <boost/bind.hpp>
#include <boost/assign/list_of.hpp>
#include <sandesh/common/vns_types.h>
#include <sandesh/common/vns_constants.h>
#include "analytics_cpuinfo_types.h"
#include "query_engine/options.h"
#include "query.h"
#include <base/misc_utils.h>
#include <query_engine/buildinfo.h>
#include <sandesh/sandesh_http.h>
using std::auto_ptr;
using std::string;
using std::vector;
using std::map;
using std::vector;
using boost::assign::list_of;
using boost::system::error_code;
using namespace boost::asio;
using namespace std;
// This is to force qed to wait for a gdbattach
// before proceeding.
// It will make it easier to debug qed during systest
volatile int gdbhelper = 1;

TaskTrigger *qe_info_trigger;
Timer *qe_info_log_timer;

bool QEInfoLogTimer() {
    qe_info_trigger->Set();
    return false;
}

bool QEInfoLogger(const string &hostname) {

    CpuLoadInfo cpu_load_info;
    CpuLoadData::FillCpuInfo(cpu_load_info, false);

    ModuleCpuState state;
    state.set_name(hostname);

    ModuleCpuInfo cinfo;
    cinfo.set_module_id(Sandesh::module());
    cinfo.set_instance_id(Sandesh::instance_id());
    cinfo.set_cpu_info(cpu_load_info);
    vector<ModuleCpuInfo> cciv;
    cciv.push_back(cinfo);

    // At some point, the following attributes will be deprecated
    // in favor of AnalyticsCpuState
    state.set_module_cpu_info(cciv);
    state.set_queryengine_cpu_share(cpu_load_info.get_cpu_share());
    state.set_queryengine_mem_virt(cpu_load_info.get_meminfo().get_virt());

    ModuleCpuStateTrace::Send(state);

    AnalyticsCpuState  astate;
    astate.set_name(hostname);

    ProcessCpuInfo ainfo;
    ainfo.set_module_id(Sandesh::module());
    ainfo.set_inst_id(Sandesh::instance_id());
    ainfo.set_cpu_share(cpu_load_info.get_cpu_share());
    ainfo.set_mem_virt(cpu_load_info.get_meminfo().get_virt());
    vector<ProcessCpuInfo> aciv;
    aciv.push_back(ainfo);
    astate.set_cpu_info(aciv);
    AnalyticsCpuStateTrace::Send(astate);

    qe_info_log_timer->Cancel();
    qe_info_log_timer->Start(60*1000, boost::bind(&QEInfoLogTimer),
                               NULL);
    return true;
}

bool QedVersion(std::string &version) {
    return MiscUtils::GetBuildInfo(MiscUtils::Analytics, BuildInfo, version);
}

#include <csignal>

static EventManager * pevm = NULL;
static void terminate_qe (int param)
{
  pevm->Shutdown();
}

static void WaitForIdle() {
    static const int kTimeout = 15;
    TaskScheduler *scheduler = TaskScheduler::GetInstance();

    for (int i = 0; i < (kTimeout * 1000); i++) {
        if (scheduler->IsEmpty()) {
            break;
        }
        usleep(1000);
    }    
}

static bool OptionsParse(Options &options, EventManager &evm,
                         int argc, char *argv[]) {
    try {
        options.Parse(evm, argc, argv);
        return true;
    } catch (boost::program_options::error &e) {
        cout << "Error " << e.what() << endl;
    } catch (...) {
        cout << "Options Parser: Caught fatal unknown exception" << endl;
    }

    return false;
}

int
main(int argc, char *argv[]) {
    EventManager evm;
    pevm = &evm;
    Options options;

    if (!OptionsParse(options, evm, argc, argv)) {
        exit(-1);
    }

    while (gdbhelper==0) {
        usleep(1000);
    }

    if (options.log_file() == "<stdout>") {
        LoggingInit();
    } else {
        LoggingInit(options.log_file(), options.log_file_size(),
                    options.log_files_count());
    }

    error_code error;
    DiscoveryServiceClient *ds_client = NULL;
    ip::tcp::endpoint dss_ep;
    Sandesh::CollectorSubFn csf = 0;
    Module::type module = Module::QUERY_ENGINE;
    string module_name = g_vns_constants.ModuleNames.find(module)->second;
    if (!options.discovery_server().empty()) {
        error_code error;
        dss_ep.address(
            ip::address::from_string(options.discovery_server(), error));
        if (error) {
            LOG(ERROR, __func__ << ": Invalid discovery-server: " << 
                    options.discovery_server());
        } else {
            dss_ep.port(options.discovery_port());
            string subscriber_name =
                g_vns_constants.ModuleNames.find(Module::QUERY_ENGINE)->second;
            ds_client = new DiscoveryServiceClient(&evm, dss_ep, 
                                                   subscriber_name);
            ds_client->Init();
            csf = boost::bind(&DiscoveryServiceClient::Subscribe, 
                              ds_client, _1, _2, _3);
        }
    }

    LOG(INFO, "http-server-port " << options.http_server_port());
    LOG(INFO, "Endpoint " << dss_ep);
    LOG(INFO, "Max-tasks " << options.max_tasks());
    LOG(INFO, "Max-slice " << options.max_slice());
    BOOST_FOREACH(std::string collector_ip, options.collector_server_list()) {
        LOG(INFO, "Collectors  " << collector_ip);
    }

    // Initialize Sandesh
    NodeType::type node_type = 
        g_vns_constants.Module2NodeType.find(module)->second;
    Sandesh::InitGenerator(
            module_name,
            options.hostname(),
            g_vns_constants.NodeTypeNames.find(node_type)->second,
            g_vns_constants.INSTANCE_ID_DEFAULT,
            &evm,
            options.http_server_port(),
            csf,
            options.collector_server_list(),
            NULL);
    Sandesh::SetLoggingParams(options.log_local(), options.log_category(),
                              options.log_level());

    // XXX Disable logging -- for test purposes only
    if (options.log_disable()) {
        SetLoggingDisabled(true);
    }

    string cassandra_server = options.cassandra_server_list()[0];
    typedef boost::tokenizer<boost::char_separator<char> > tokenizer;
    boost::char_separator<char> sep(":");
    tokenizer tokens(cassandra_server, sep);
    tokenizer::iterator it = tokens.begin();
    std::string cassandra_ip(*it);
    ++it;
    std::string port(*it);
    int cassandra_port;
    stringToInteger(port, cassandra_port);

    QueryEngine *qe;
    if (cassandra_port == 0) {
        qe = new QueryEngine(&evm,
            options.redis_server(),
            options.redis_port(),
            options.max_tasks(),
            options.max_slice(),
            options.analytics_data_ttl());
    } else {
        qe = new QueryEngine(&evm,
            cassandra_ip,
            cassandra_port,
            options.redis_server(),
            options.redis_port(),
            options.max_tasks(),
            options.max_slice(),
            options.analytics_data_ttl(),
            options.start_time());
    }
    (void) qe;

    CpuLoadData::Init();
    qe_info_trigger =
        new TaskTrigger(boost::bind(&QEInfoLogger, options.hostname()),
                    TaskScheduler::GetInstance()->GetTaskId("qe::Stats"), 0);
    qe_info_log_timer = TimerManager::CreateTimer(*evm.io_service(),
                                                    "Collector Info log timer");
    qe_info_log_timer->Start(5*1000, boost::bind(&QEInfoLogTimer), NULL);    
    signal(SIGTERM, terminate_qe);
    evm.Run();

    if (ds_client) {
        ds_client->Shutdown();
        delete ds_client;
    }
    qe_info_log_timer->Cancel();
    TimerManager::DeleteTimer(qe_info_log_timer);
    WaitForIdle();
    delete qe;
    Sandesh::Uninit();
    WaitForIdle();
    return 0;
}
