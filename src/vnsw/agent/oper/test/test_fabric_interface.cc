/*
 * Copyright (c) 2014 Juniper Networks, Inc. All rights reserved.
 */

#include <sys/socket.h>
#include <linux/netlink.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <linux/if_packet.h>

#include "testing/gunit.h"

#include <boost/uuid/string_generator.hpp>

#include <base/logging.h>
#include <io/event_manager.h>
#include <tbb/task.h>
#include <base/task.h>

#include <cmn/agent_cmn.h>

#include "cfg/cfg_init.h"
#include "cfg/cfg_interface.h"
#include "oper/operdb_init.h"
#include "controller/controller_init.h"
#include "pkt/pkt_init.h"
#include "services/services_init.h"
#include "ksync/ksync_init.h"
#include "oper/interface_common.h"
#include "oper/nexthop.h"
#include "route/route.h"
#include "oper/vrf.h"
#include "oper/mpls.h"
#include "oper/vm.h"
#include "oper/vn.h"
#include "filter/acl.h"
#include "openstack/instance_service_server.h"
#include "test_cmn_util.h"
#include "vr_types.h"
#include <controller/controller_export.h> 
#include <ksync/ksync_sock_user.h> 

void RouterIdDepInit(Agent *agent) {
}

class FabricInterfaceTest : public ::testing::Test {
public:
    virtual void SetUp() {
        agent_ = Agent::GetInstance();
        interface_table_ = agent_->GetInterfaceTable();
        nh_table_ = agent_->GetNextHopTable();
        vrf_table_ = agent_->GetVrfTable();
        fabric_rt_table_ = agent_->GetDefaultInet4UnicastRouteTable();

        intf_count_ = agent_->GetInterfaceTable()->Size();
        nh_count_ = agent_->GetNextHopTable()->Size();
        vrf_count_ = agent_->GetVrfTable()->Size();

        strcpy(fabric_vrf_name_, agent_->GetDefaultVrf().c_str());
        VmAddReq(1);
        VmAddReq(2);
    }

    virtual void TearDown() {
        VmDelReq(1);
        VmDelReq(2);
        WAIT_FOR(100, 1000, (interface_table_->Size() == intf_count_));
        WAIT_FOR(100, 1000, (agent_->GetVrfTable()->Size() == vrf_count_));
        WAIT_FOR(100, 1000, (agent_->GetNextHopTable()->Size() == nh_count_));
        WAIT_FOR(100, 1000, (agent_->GetVmTable()->Size() == 0U));
        WAIT_FOR(100, 1000, (agent_->GetVnTable()->Size() == 0U));
    }

    int intf_count_;
    int nh_count_;
    int vrf_count_;
    Agent *agent_;
    InterfaceTable *interface_table_;
    NextHopTable *nh_table_;
    Inet4UnicastAgentRouteTable *fabric_rt_table_;
    VrfTable *vrf_table_;
    char fabric_vrf_name_[64];
};

static void DhcpInterfaceSync(FabricInterfaceTest *t, int id,
                              const char *ifname) {
    DBRequest req(DBRequest::DB_ENTRY_ADD_CHANGE);
    req.key.reset(new VmInterfaceKey(AgentKey::RESYNC, MakeUuid(id), ifname));
    req.data.reset(new VmInterfaceIpAddressData());
    t->interface_table_->Enqueue(&req);
}

static void CfgIntfSync(FabricInterfaceTest *t, int id, const char *cfg_name,
                        int vn, int vm, const char *vrf_name, const char *ip,
                        bool fab_port) {
    DBRequest req(DBRequest::DB_ENTRY_ADD_CHANGE);
    req.key.reset(new VmInterfaceKey(AgentKey::RESYNC, MakeUuid(id), cfg_name));

    VmInterfaceConfigData *cfg_data = new VmInterfaceConfigData();
    req.data.reset(cfg_data);
    InterfaceData *data = static_cast<InterfaceData *>(cfg_data);
    data->VmPortInit();

    cfg_data->cfg_name_ = cfg_name;
    cfg_data->vn_uuid_ = MakeUuid(vn);
    cfg_data->vm_uuid_ = MakeUuid(vm);
    cfg_data->vrf_name_ = vrf_name;
    cfg_data->addr_ = Ip4Address::from_string(ip);
    cfg_data->fabric_port_ = fab_port;
    t->interface_table_->Enqueue(&req);
}

static void NovaDel(FabricInterfaceTest *t, int id) {
    VmInterface::Delete(t->interface_table_, MakeUuid(id));
}

static void NovaIntfAdd(FabricInterfaceTest *t, int id, const char *name,
                        const char *addr, const char *mac) {
    IpAddress ip = Ip4Address::from_string(addr);
    VmInterface::Add(t->interface_table_, MakeUuid(id), name, ip.to_v4(), mac,
                     "", MakeUuid(kProjectUuid),
                     VmInterface::kInvalidVlanId, Agent::NullString());
}

// Fabric port with IP Address 0.0.0.0. Needs dhcp_relay
TEST_F(FabricInterfaceTest, zero_ip_1) {
    VnAddReq(1, "vn1", 0, fabric_vrf_name_);
    NovaIntfAdd(this, 1, "fabric1", "0.0.0.0", "00:00:00:00:00:01");
    client->WaitForIdle();
    WAIT_FOR(1000, 100, (VmPortInactive(1) == true));

    CfgIntfSync(this, 1, "fabric1", 1, 1, fabric_vrf_name_, "0.0.0.0", true);
    client->WaitForIdle();
    VmInterface *intf = static_cast<VmInterface *>(VmPortGet(1));
    EXPECT_TRUE(intf->do_dhcp_relay());

    NovaDel(this, 1);
    CfgIntfSync(this, 1, "fabric1", 0, 0, "", "0.0.0.0", true);
    VnDelReq(1);
    client->WaitForIdle();
    EXPECT_FALSE(VmPortFind(1));
}

// Fabric port with IP Address 0.0.0.0. Needs dhcp_relay
TEST_F(FabricInterfaceTest, zero_ip_2) {
    VnAddReq(1, "vn1", 0, fabric_vrf_name_);
    NovaIntfAdd(this, 1, "fabric1", "1.1.1.1", "00:00:00:00:00:01");
    client->WaitForIdle();
    WAIT_FOR(1000, 100, (VmPortInactive(1) == true));

    CfgIntfSync(this, 1, "fabric1", 1, 1, fabric_vrf_name_, "0.0.0.0", true);
    client->WaitForIdle();
    VmInterface *intf = static_cast<VmInterface *>(VmPortGet(1));
    EXPECT_TRUE(intf->do_dhcp_relay());

    NovaDel(this, 1);
    CfgIntfSync(this, 1, "fabric1", 0, 0, "", "0.0.0.0", true);
    VnDelReq(1);
    client->WaitForIdle();
    EXPECT_FALSE(VmPortFind(1));
}

// Fabric port with IP Address != 0.0.0.0. Does not need dhcp_relay
TEST_F(FabricInterfaceTest, non_zero_ip_1) {
    VnAddReq(1, "vn1", 0, fabric_vrf_name_);
    NovaIntfAdd(this, 1, "fabric1", "0.0.0.0", "00:00:00:00:00:01");
    client->WaitForIdle();
    WAIT_FOR(1000, 100, (VmPortInactive(1) == true));

    CfgIntfSync(this, 1, "fabric1", 1, 1, fabric_vrf_name_, "1.1.1.1", true);
    client->WaitForIdle();
    VmInterface *intf = static_cast<VmInterface *>(VmPortGet(1));
    EXPECT_TRUE(intf->do_dhcp_relay() == false);

    NovaDel(this, 1);
    CfgIntfSync(this, 1, "fabric1", 0, 0, "", "0.0.0.0", true);
    VnDelReq(1);
    client->WaitForIdle();
    EXPECT_FALSE(VmPortFind(1));
}

// Fabric port with IP Address != 0.0.0.0. Does not need dhcp_relay
TEST_F(FabricInterfaceTest, non_zero_ip_2) {
    VnAddReq(1, "vn1", 0, fabric_vrf_name_);
    NovaIntfAdd(this, 1, "fabric1", "1.1.1.1", "00:00:00:00:00:01");
    client->WaitForIdle();
    WAIT_FOR(1000, 100, (VmPortInactive(1) == true));

    CfgIntfSync(this, 1, "fabric1", 1, 1, fabric_vrf_name_, "1.1.1.1", true);
    client->WaitForIdle();
    VmInterface *intf = static_cast<VmInterface *>(VmPortGet(1));
    EXPECT_TRUE(intf->do_dhcp_relay() == false);

    NovaDel(this, 1);
    CfgIntfSync(this, 1, "fabric1", 0, 0, "", "0.0.0.0", true);
    VnDelReq(1);
    client->WaitForIdle();
    EXPECT_FALSE(VmPortFind(1));
}

// Interface with IP == 0.0.0.0 on non-fabric VRF. Should not need dncp_relay
TEST_F(FabricInterfaceTest, non_fabric_vrf_1) {
    VrfAddReq("vrf1");
    VnAddReq(1, "vn1", 0, "vrf1");
    NovaIntfAdd(this, 1, "fabric1", "0.0.0.0", "00:00:00:00:00:01");
    client->WaitForIdle();
    WAIT_FOR(1000, 100, (VmPortInactive(1) == true));

    CfgIntfSync(this, 1, "fabric1", 1, 1, "vrf1", "0.0.0.0", true);
    client->WaitForIdle();
    VmInterface *intf = static_cast<VmInterface *>(VmPortGet(1));
    EXPECT_TRUE(intf->do_dhcp_relay() == false);

    NovaDel(this, 1);
    CfgIntfSync(this, 1, "fabric1", 0, 0, "", "0.0.0.0", true);
    VnDelReq(1);
    VrfDelReq("vrf1");
    client->WaitForIdle();
    EXPECT_FALSE(VmPortFind(1));
}

// Interface with IP != 0.0.0.0 on non-fabric VRF. Should not need dncp_relay
TEST_F(FabricInterfaceTest, non_fabric_vrf_2) {
    VrfAddReq("vrf1");
    VnAddReq(1, "vn1", 0, "vrf1");
    NovaIntfAdd(this, 1, "fabric1", "1.1.1.1", "00:00:00:00:00:01");
    client->WaitForIdle();
    WAIT_FOR(1000, 100, (VmPortInactive(1) == true));

    CfgIntfSync(this, 1, "fabric1", 1, 1, "vrf1", "1.1.1.1", true);
    client->WaitForIdle();
    VmInterface *intf = static_cast<VmInterface *>(VmPortGet(1));
    EXPECT_TRUE(intf->do_dhcp_relay() == false);

    NovaDel(this, 1);
    CfgIntfSync(this, 1, "fabric1", 0, 0, "", "0.0.0.0", true);
    VnDelReq(1);
    VrfDelReq("vrf1");
    client->WaitForIdle();
    EXPECT_FALSE(VmPortFind(1));
}

// port with IP Address 0.0.0.0 learning from DHCP Relay
TEST_F(FabricInterfaceTest, dhcp_snoop_1) {
    VnAddReq(1, "vn1", 0, fabric_vrf_name_);
    NovaIntfAdd(this, 1, "fabric1", "0.0.0.0", "00:00:00:00:00:01");
    client->WaitForIdle();
    WAIT_FOR(1000, 100, (VmPortInactive(1) == true));

    CfgIntfSync(this, 1, "fabric1", 1, 1, fabric_vrf_name_, "0.0.0.0", true);
    client->WaitForIdle();
    VmInterface *intf = static_cast<VmInterface *>(VmPortGet(1));
    EXPECT_TRUE(intf->do_dhcp_relay());

    interface_table_->AddDhcpSnoopEntry("fabric1",
                                        Ip4Address::from_string("1.1.1.1"));
    DhcpInterfaceSync(this, 1, "fabric1");
    client->WaitForIdle();

    EXPECT_TRUE(intf->do_dhcp_relay());
    EXPECT_TRUE(intf->ip_addr().to_string() == "1.1.1.1");
    Inet4UnicastRouteEntry *rt;
    rt = RouteGet(fabric_vrf_name_, Ip4Address::from_string("1.1.1.1"), 32);
    EXPECT_TRUE(rt != NULL);

    interface_table_->AddDhcpSnoopEntry("fabric1",
                                        Ip4Address::from_string("2.2.2.2"));
    DhcpInterfaceSync(this, 1, "fabric1");
    client->WaitForIdle();
    rt = RouteGet(fabric_vrf_name_, Ip4Address::from_string("2.2.2.2"), 32);
    EXPECT_TRUE(rt != NULL);
    rt = RouteGet(fabric_vrf_name_, Ip4Address::from_string("1.1.1.1"), 32);
    EXPECT_TRUE(rt == NULL);

    NovaDel(this, 1);
    CfgIntfSync(this, 1, "fabric1", 0, 0, "", "0.0.0.0", true);
    VnDelReq(1);
    client->WaitForIdle();
    EXPECT_FALSE(VmPortFind(1));
}

int main(int argc, char **argv) {
    GETUSERARGS();

    client = TestInit(init_file, ksync_init, false, false, false);

    int ret = RUN_ALL_TESTS();

    usleep(10000);
    client->WaitForIdle();
    usleep(10000);

    return ret;
}
