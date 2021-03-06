/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/routing-instance/path_resolver.h"

#include <boost/assign/list_of.hpp>
#include <boost/foreach.hpp>

#include "bgp/bgp_factory.h"
#include "bgp/bgp_session_manager.h"
#include "bgp/extended-community/load_balance.h"
#include "bgp/security_group/security_group.h"
#include "bgp/tunnel_encap/tunnel_encap.h"
#include "bgp/inet/inet_table.h"
#include "bgp/inet6/inet6_table.h"
#include "bgp/test/bgp_server_test_util.h"
#include "control-node/control_node.h"

using boost::assign::list_of;
using std::set;
using std::string;
using std::vector;

class PeerMock : public IPeer {
public:
    PeerMock(bool is_xmpp, const string &address_str)
        : is_xmpp_(is_xmpp) {
        boost::system::error_code ec;
        address_ = Ip4Address::from_string(address_str, ec);
        assert(ec.value() == 0);
    }
    virtual ~PeerMock() { }
    virtual std::string ToString() const { return address_.to_string(); }
    virtual std::string ToUVEKey() const { return address_.to_string(); }
    virtual bool SendUpdate(const uint8_t *msg, size_t msgsize) { return true; }
    virtual BgpServer *server() { return NULL; }
    virtual IPeerClose *peer_close() { return NULL; }
    virtual IPeerDebugStats *peer_stats() { return NULL; }
    virtual const IPeerDebugStats *peer_stats() const { return NULL; }
    virtual bool IsReady() const { return true; }
    virtual bool IsXmppPeer() const { return is_xmpp_; }
    virtual void Close() {
    }
    BgpProto::BgpPeerType PeerType() const {
        return is_xmpp_ ? BgpProto::XMPP : BgpProto::EBGP;
    }
    virtual uint32_t bgp_identifier() const {
        return htonl(address_.to_ulong());
    }
    virtual const std::string GetStateName() const { return ""; }
    virtual void UpdateRefCount(int count) const { }
    virtual tbb::atomic<int> GetRefCount() const {
        tbb::atomic<int> count;
        count = 0;
        return count;
    }
    virtual void UpdatePrimaryPathCount(int count) const { }
    virtual int GetPrimaryPathCount() const { return 0; }

private:
    bool is_xmpp_;
    Ip4Address address_;
};

static const char *config = "\
<config>\
    <bgp-router name=\'localhost\'>\
        <identifier>192.168.0.100</identifier>\
        <address>192.168.0.100</address>\
        <autonomous-system>64512</autonomous-system>\
    </bgp-router>\
    <routing-instance name='blue'>\
        <vrf-target>target:64512:1</vrf-target>\
    </routing-instance>\
    <routing-instance name='pink'>\
        <vrf-target>target:64512:2</vrf-target>\
    </routing-instance>\
</config>\
";

//
// Template structure to pass to fixture class template. Needed because
// gtest fixture class template can accept only one template parameter.
//
template <typename T1, typename T2, typename T3>
struct TypeDefinition {
  typedef T1 TableT;
  typedef T2 PrefixT;
  typedef T3 AddressT;
};

// TypeDefinitions that we want to test.
typedef TypeDefinition<InetTable, Ip4Prefix, Ip4Address> InetDefinition;
typedef TypeDefinition<Inet6Table, Inet6Prefix, Ip6Address> Inet6Definition;

//
// Fixture class template - instantiated later for each TypeDefinition.
//
template <typename T>
class PathResolverTest : public ::testing::Test {
protected:
    typedef typename T::TableT TableT;
    typedef typename T::PrefixT PrefixT;
    typedef typename T::AddressT AddressT;

    PathResolverTest()
        : bgp_server_(new BgpServerTest(&evm_, "localhost")),
          family_(GetFamily()),
          ipv6_prefix_("::ffff:"),
          bgp_peer1_(new PeerMock(false, "192.168.1.1")),
          bgp_peer2_(new PeerMock(false, "192.168.1.2")),
          xmpp_peer1_(new PeerMock(true, "172.16.1.1")),
          xmpp_peer2_(new PeerMock(true, "172.16.1.2")) {
        bgp_server_->session_manager()->Initialize(0);
    }
    ~PathResolverTest() {
        delete bgp_peer1_;
        delete bgp_peer2_;
        delete xmpp_peer1_;
        delete xmpp_peer2_;
    }

    virtual void SetUp() {
        bgp_server_->Configure(config);
        task_util::WaitForIdle();
    }

    virtual void TearDown() {
        task_util::WaitForIdle();
        bgp_server_->Shutdown();
        task_util::WaitForIdle();
    }

    Address::Family GetFamily() const {
        assert(false);
        return Address::UNSPEC;
    }

    string BuildHostAddress(const string &ipv4_addr) const {
        if (family_ == Address::INET) {
            return ipv4_addr;
        }
        if (family_ == Address::INET6) {
            return ipv6_prefix_ + ipv4_addr;
        }
        assert(false);
        return "";
    }

    string BuildPrefix(const string &ipv4_prefix, uint8_t ipv4_plen) const {
        if (family_ == Address::INET) {
            return ipv4_prefix + "/" + integerToString(ipv4_plen);
        }
        if (family_ == Address::INET6) {
            return ipv6_prefix_ + ipv4_prefix + "/" +
                integerToString(96 + ipv4_plen);
        }
        assert(false);
        return "";
    }

    string BuildPrefix(int index) const {
        assert(index <= 65535);
        string ipv4_prefix("10.1.");
        uint8_t ipv4_plen = Address::kMaxV4PrefixLen;
        string byte3 = integerToString(index / 256);
        string byte4 = integerToString(index % 256);
        if (family_ == Address::INET) {
            return ipv4_prefix + byte3 + "." + byte4 + "/" +
                integerToString(ipv4_plen);
        }
        if (family_ == Address::INET6) {
            return ipv6_prefix_ + ipv4_prefix + byte3 + "." + byte4 + "/" +
                integerToString(96 + ipv4_plen);
        }
        assert(false);
        return "";
    }

    string BuildNextHopAddress(const string &ipv4_addr) const {
        return ipv4_addr;
    }

    string GetTableName(const string &instance) const {
        if (family_ == Address::INET) {
            return instance + ".inet.0";
        }
        if (family_ == Address::INET6) {
            return instance + ".inet6.0";
        }
        assert(false);
        return "";
    }

    BgpTable *GetTable(const string &instance) {
        return static_cast<BgpTable *>(
            bgp_server_->database()->FindTable(GetTableName(instance)));
    }

    // Add a BgpPath that requires resolution.
    void AddBgpPath(IPeer *bgp_peer, const string &instance,
        const string &prefix_str, const string &nexthop_str, uint32_t med = 0,
        const vector<uint32_t> &comm_list = vector<uint32_t>(),
        const vector<uint16_t> &as_list = vector<uint16_t>()) {
        assert(!bgp_peer->IsXmppPeer());

        boost::system::error_code ec;
        PrefixT prefix = PrefixT::FromString(prefix_str, &ec);
        EXPECT_FALSE(ec);
        DBRequest request;
        request.oper = DBRequest::DB_ENTRY_ADD_CHANGE;
        request.key.reset(new typename TableT::RequestKey(prefix, bgp_peer));

        BgpTable *table = GetTable(instance);
        BgpAttrSpec attr_spec;

        AddressT nh_addr = AddressT::from_string(nexthop_str, ec);
        EXPECT_FALSE(ec);
        BgpAttrNextHop nh_spec(nh_addr);
        attr_spec.push_back(&nh_spec);

        BgpAttrMultiExitDisc med_spec(med);
        if (med)
            attr_spec.push_back(&med_spec);

        CommunitySpec comm_spec;
        if (!comm_list.empty()) {
            comm_spec.communities = comm_list;
            attr_spec.push_back(&comm_spec);
        }

        AsPathSpec aspath_spec;
        AsPathSpec::PathSegment *ps = new AsPathSpec::PathSegment;
        ps->path_segment_type = AsPathSpec::PathSegment::AS_SEQUENCE;
        BOOST_FOREACH(uint16_t asn, as_list) {
            ps->path_segment.push_back(asn);
        }
        aspath_spec.path_segments.push_back(ps);
        attr_spec.push_back(&aspath_spec);

        BgpAttrPtr attr = bgp_server_->attr_db()->Locate(attr_spec);
        request.data.reset(
            new BgpTable::RequestData(attr, BgpPath::ResolveNexthop, 0));
        table->Enqueue(&request);
    }

    void AddBgpPathWithMed(IPeer *bgp_peer, const string &instance,
        const string &prefix_str, const string &nexthop_str, uint32_t med) {
        AddBgpPath(bgp_peer, instance, prefix_str, nexthop_str, med);
    }

    void AddBgpPathWithCommunities(IPeer *bgp_peer, const string &instance,
        const string &prefix_str, const string &nexthop_str,
        const vector<uint32_t> &comm_list) {
        AddBgpPath(bgp_peer, instance, prefix_str, nexthop_str, 0, comm_list);
    }

    void AddBgpPathWithAsList(IPeer *bgp_peer, const string &instance,
        const string &prefix_str, const string &nexthop_str,
        const vector<uint16_t> &as_list) {
        AddBgpPath(bgp_peer, instance, prefix_str, nexthop_str, 0,
            vector<uint32_t>(), as_list);
    }

    // Add a BgpPath that that can be used to resolve other paths.
    void AddXmppPath(IPeer *xmpp_peer, const string &instance,
        const string &prefix_str, const string &nexthop_str1,
        int label, const string &nexthop_str2 = string(),
        vector<uint32_t> sgid_list = vector<uint32_t>(),
        set<string> encap_list = set<string>(),
        const LoadBalance &lb = LoadBalance()) {
        assert(xmpp_peer->IsXmppPeer());
        assert(!nexthop_str1.empty());
        assert(label);

        boost::system::error_code ec;
        PrefixT prefix = PrefixT::FromString(prefix_str, &ec);
        EXPECT_FALSE(ec);
        DBRequest request;
        request.oper = DBRequest::DB_ENTRY_ADD_CHANGE;
        request.key.reset(new typename TableT::RequestKey(prefix, xmpp_peer));

        BgpTable *table = GetTable(instance);
        int index = table->routing_instance()->index();
        BgpAttrSpec attr_spec;

        Ip4Address nh_addr1 = Ip4Address::from_string(nexthop_str1, ec);
        EXPECT_FALSE(ec);
        BgpAttrNextHop nh_spec(nh_addr1);
        attr_spec.push_back(&nh_spec);

        BgpAttrSourceRd source_rd_spec(
            RouteDistinguisher(nh_addr1.to_ulong(), index));
        attr_spec.push_back(&source_rd_spec);

        ExtCommunitySpec extcomm_spec;
        for (vector<uint32_t>::iterator it = sgid_list.begin();
             it != sgid_list.end(); ++it) {
            SecurityGroup sgid(64512, *it);
            uint64_t value = sgid.GetExtCommunityValue();
            extcomm_spec.communities.push_back(value);
        }
        for (set<string>::iterator it = encap_list.begin();
             it != encap_list.end(); ++it) {
            TunnelEncap encap(*it);
            uint64_t value = encap.GetExtCommunityValue();
            extcomm_spec.communities.push_back(value);
        }
        if (!lb.IsDefault()) {
            uint64_t value = lb.GetExtCommunityValue();
            extcomm_spec.communities.push_back(value);
        }
        if (!extcomm_spec.communities.empty())
            attr_spec.push_back(&extcomm_spec);

        BgpAttrPtr attr = bgp_server_->attr_db()->Locate(attr_spec);

        typename TableT::RequestData::NextHops nexthops;
        typename TableT::RequestData::NextHop nexthop1;
        nexthop1.flags_ = 0;
        nexthop1.address_ = nh_addr1;
        nexthop1.label_ = label;
        nexthop1.source_rd_ = RouteDistinguisher(nh_addr1.to_ulong(), index);
        nexthops.push_back(nexthop1);

        typename TableT::RequestData::NextHop nexthop2;
        if (!nexthop_str2.empty()) {
            Ip4Address nh_addr2 = Ip4Address::from_string(nexthop_str2, ec);
            EXPECT_FALSE(ec);
            nexthop2.flags_ = 0;
            nexthop2.address_ = nh_addr2;
            nexthop2.label_ = label;
            nexthop2.source_rd_ =
                RouteDistinguisher(nh_addr2.to_ulong(), index);
            nexthops.push_back(nexthop2);
        }

        request.data.reset(new BgpTable::RequestData(attr, nexthops));
        table->Enqueue(&request);
    }

    void AddXmppPathWithSecurityGroups(IPeer *xmpp_peer,
        const string &instance, const string &prefix_str,
        const string &nexthop_str, int label, vector<uint32_t> sgid_list) {
        AddXmppPath(xmpp_peer, instance, prefix_str, nexthop_str, label,
            string(), sgid_list);
    }

    void AddXmppPathWithTunnelEncapsulations(IPeer *xmpp_peer,
        const string &instance, const string &prefix_str,
        const string &nexthop_str, int label, set<string> encap_list) {
        AddXmppPath(xmpp_peer, instance, prefix_str, nexthop_str, label,
            string(), vector<uint32_t>(), encap_list);
    }

    void AddXmppPathWithLoadBalance(IPeer *xmpp_peer,
        const string &instance, const string &prefix_str,
        const string &nexthop_str, int label, const LoadBalance &lb) {
        AddXmppPath(xmpp_peer, instance, prefix_str, nexthop_str, label,
            string(), vector<uint32_t>(), set<string>(), lb);
    }

    void DeletePath(IPeer *peer, const string &instance,
        const string &prefix_str) {
        boost::system::error_code ec;
        PrefixT prefix = PrefixT::FromString(prefix_str, &ec);
        EXPECT_FALSE(ec);

        DBRequest request;
        request.oper = DBRequest::DB_ENTRY_DELETE;
        request.key.reset(new typename TableT::RequestKey(prefix, peer));

        BgpTable *table = GetTable(instance);
        table->Enqueue(&request);
    }

    void DeleteBgpPath(IPeer *bgp_peer, const string &instance,
        const string &prefix_str) {
        DeletePath(bgp_peer, instance, prefix_str);
    }

    void DeleteXmppPath(IPeer *xmpp_peer, const string &instance,
        const string &prefix_str) {
        DeletePath(xmpp_peer, instance, prefix_str);
    }

    void DisableResolverNexthopRegUnregProcessing(const string &instance) {
        BgpTable *table = GetTable(instance);
        table->path_resolver()->DisableResolverNexthopRegUnregProcessing();
    }

    void EnableResolverNexthopRegUnregProcessing(const string &instance) {
        BgpTable *table = GetTable(instance);
        table->path_resolver()->EnableResolverNexthopRegUnregProcessing();
    }

    size_t ResolverNexthopRegUnregListSize(const string &instance) {
        BgpTable *table = GetTable(instance);
        return table->path_resolver()->GetResolverNexthopRegUnregListSize();
    }

    void DisableResolverNexthopUpdateProcessing(const string &instance) {
        BgpTable *table = GetTable(instance);
        table->path_resolver()->DisableResolverNexthopUpdateProcessing();
    }

    void EnableResolverNexthopUpdateProcessing(const string &instance) {
        BgpTable *table = GetTable(instance);
        table->path_resolver()->EnableResolverNexthopUpdateProcessing();
    }

    size_t ResolverNexthopUpdateListSize(const string &instance) {
        BgpTable *table = GetTable(instance);
        return table->path_resolver()->GetResolverNexthopUpdateListSize();
    }

    void DisableResolverPathUpdateProcessing(const string &instance) {
        BgpTable *table = GetTable(instance);
        table->path_resolver()->DisableResolverPathUpdateProcessing();
    }

    void EnableResolverPathUpdateProcessing(const string &instance) {
        BgpTable *table = GetTable(instance);
        table->path_resolver()->EnableResolverPathUpdateProcessing();
    }

    size_t ResolverPathUpdateListSize(const string &instance) {
        BgpTable *table = GetTable(instance);
        return table->path_resolver()->GetResolverPathUpdateListSize();
    }

    BgpRoute *RouteLookup(const string &instance_name, const string &prefix) {
        BgpTable *bgp_table = GetTable(instance_name);
        TableT *table = dynamic_cast<TableT *>(bgp_table);
        EXPECT_TRUE(table != NULL);
        if (table == NULL) {
            return NULL;
        }
        boost::system::error_code error;
        PrefixT nlri = PrefixT::FromString(prefix, &error);
        EXPECT_FALSE(error);
        typename TableT::RequestKey key(nlri, NULL);
        BgpRoute *rt = dynamic_cast<BgpRoute *>(table->Find(&key));
        return rt;
    }

    BgpRoute *VerifyRouteExists(const string &instance, const string &prefix) {
        TASK_UTIL_EXPECT_TRUE(RouteLookup(instance, prefix) != NULL);
        BgpRoute *rt = RouteLookup(instance, prefix);
        if (rt == NULL) {
            return NULL;
        }
        TASK_UTIL_EXPECT_TRUE(rt->BestPath() != NULL);
        return rt;
    }

    void VerifyRouteNoExists(const string &instance, const string &prefix) {
        TASK_UTIL_EXPECT_TRUE(RouteLookup(instance, prefix) == NULL);
    }

    vector<uint32_t> GetSGIDListFromPath(const BgpPath *path) {
        const ExtCommunity *ext_comm = path->GetAttr()->ext_community();
        vector<uint32_t> list;
        if (!ext_comm)
            return list;
        BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &comm,
                      ext_comm->communities()) {
            if (!ExtCommunity::is_security_group(comm))
                continue;
            SecurityGroup security_group(comm);
            list.push_back(security_group.security_group_id());
        }
        sort(list.begin(), list.end());
        return list;
    }

    set<string> GetTunnelEncapListFromPath(const BgpPath *path) {
        const ExtCommunity *ext_comm = path->GetAttr()->ext_community();
        set<string> list;
        if (!ext_comm)
            return list;
        BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &comm,
                      ext_comm->communities()) {
            if (!ExtCommunity::is_tunnel_encap(comm))
                continue;
            TunnelEncap encap(comm);
            list.insert(encap.ToXmppString());
        }
        return list;
    }

    LoadBalance GetLoadBalanceFromPath(const BgpPath *path) {
        const ExtCommunity *ext_comm = path->GetAttr()->ext_community();
        if (!ext_comm)
            return LoadBalance();
        BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &comm,
                      ext_comm->communities()) {
            if (!ExtCommunity::is_load_balance(comm))
                continue;
            return LoadBalance(comm);
        }
        return LoadBalance();
    }

    vector<uint32_t> GetCommunityListFromPath(const BgpPath *path) {
        const Community *comm = path->GetAttr()->community();
        vector<uint32_t> list = comm ? comm->communities() : vector<uint32_t>();
        sort(list.begin(), list.end());
        return list;
    }

    vector<uint16_t> GetAsListFromPath(const BgpPath *path) {
        vector<uint16_t> list;
        const AsPath *aspath = path->GetAttr()->as_path();
        const AsPathSpec &aspath_spec = aspath->path();
        if (aspath_spec.path_segments.size() != 1)
            return list;
        if (aspath_spec.path_segments[0] == NULL)
            return list;
        if (aspath_spec.path_segments[0]->path_segment_type !=
            AsPathSpec::PathSegment::AS_SEQUENCE)
            return list;
        list = aspath_spec.path_segments[0]->path_segment;
        return list;
    }

    bool MatchPathAttributes(const BgpPath *path, const string &path_id,
        uint32_t label, const vector<uint32_t> &sgid_list,
        const set<string> &encap_list, const LoadBalance &lb, uint32_t med,
        const CommunitySpec &comm_spec, const vector<uint16_t> &as_list) {
        const BgpAttr *attr = path->GetAttr();
        if (attr->nexthop().to_v4().to_string() != path_id)
            return false;
        if (label && path->GetLabel() != label)
            return false;
        if (sgid_list.size()) {
            vector<uint32_t> path_sgid_list = GetSGIDListFromPath(path);
            if (path_sgid_list.size() != sgid_list.size())
                return false;
            for (vector<uint32_t>::const_iterator
                it1 = path_sgid_list.begin(), it2 = sgid_list.begin();
                it1 != path_sgid_list.end() && it2 != sgid_list.end();
                ++it1, ++it2) {
                if (*it1 != *it2)
                    return false;
            }
        }
        if (encap_list.size()) {
            set<string> path_encap_list = GetTunnelEncapListFromPath(path);
            if (path_encap_list != encap_list)
                return false;
        }
        if (!lb.IsDefault()) {
            LoadBalance path_lb = GetLoadBalanceFromPath(path);
            if (path_lb != lb)
                return false;
        }
        if (attr->med() != med) {
            return false;
        }
        vector<uint32_t> path_comm_list = GetCommunityListFromPath(path);
        if (path_comm_list != comm_spec.communities) {
            return false;
        }
        vector<uint16_t> path_as_list = GetAsListFromPath(path);
        if (path_as_list != as_list) {
            return false;
        }

        return true;
    }

    bool CheckPathAttributes(const string &instance, const string &prefix,
        const string &path_id, int label, const vector<uint32_t> &sgid_list,
        const set<string> &encap_list, const LoadBalance &lb, uint32_t med,
        const CommunitySpec &comm_spec, const vector<uint16_t> &as_list) {
        task_util::TaskSchedulerLock lock;
        BgpRoute *route = RouteLookup(instance, prefix);
        if (!route)
            return false;
        for (Route::PathList::iterator it = route->GetPathList().begin();
             it != route->GetPathList().end(); ++it) {
            const BgpPath *path = static_cast<const BgpPath *>(it.operator->());
            if (path->GetSource() != BgpPath::ResolvedRoute)
                continue;
            if (BgpPath::PathIdString(path->GetPathId()) != path_id)
                continue;
            if (MatchPathAttributes(path, path_id, label, sgid_list,
                encap_list, lb, med, comm_spec, as_list)) {
                return true;
            }
            return false;
        }

        return false;
    }

    bool CheckPathNoExists(const string &instance, const string &prefix,
        const string &path_id) {
        task_util::TaskSchedulerLock lock;
        BgpRoute *route = RouteLookup(instance, prefix);
        if (!route)
            return true;
        for (Route::PathList::iterator it = route->GetPathList().begin();
             it != route->GetPathList().end(); ++it) {
            const BgpPath *path = static_cast<const BgpPath *>(it.operator->());
            if (path->GetSource() != BgpPath::ResolvedRoute)
                continue;
            if (BgpPath::PathIdString(path->GetPathId()) == path_id)
                return false;
        }

        return true;
    }

    void VerifyPathAttributes(const string &instance, const string &prefix,
        const string &path_id, uint32_t label) {
        TASK_UTIL_EXPECT_TRUE(CheckPathAttributes(instance, prefix,
            path_id, label, vector<uint32_t>(), set<string>(), LoadBalance(),
            0, CommunitySpec(), vector<uint16_t>()));
    }

    void VerifyPathAttributes(const string &instance, const string &prefix,
        const string &path_id, uint32_t label, const set<string> &encap_list) {
        TASK_UTIL_EXPECT_TRUE(CheckPathAttributes(instance, prefix,
            path_id, label, vector<uint32_t>(), encap_list, LoadBalance(),
            0, CommunitySpec(), vector<uint16_t>()));
    }

    void VerifyPathAttributes(const string &instance, const string &prefix,
        const string &path_id, uint32_t label,
        const vector<uint32_t> &sgid_list) {
        TASK_UTIL_EXPECT_TRUE(CheckPathAttributes(instance, prefix,
            path_id, label, sgid_list, set<string>(), LoadBalance(),
            0, CommunitySpec(), vector<uint16_t>()));
    }

    void VerifyPathAttributes(const string &instance, const string &prefix,
        const string &path_id, uint32_t label, const LoadBalance &lb) {
        TASK_UTIL_EXPECT_TRUE(CheckPathAttributes(instance, prefix,
            path_id, label, vector<uint32_t>(), set<string>(), lb,
            0, CommunitySpec(), vector<uint16_t>()));
    }

    void VerifyPathAttributes(const string &instance, const string &prefix,
        const string &path_id, uint32_t label, uint32_t med) {
        TASK_UTIL_EXPECT_TRUE(CheckPathAttributes(instance, prefix,
            path_id, label, vector<uint32_t>(), set<string>(), LoadBalance(),
            med, CommunitySpec(), vector<uint16_t>()));
    }

    void VerifyPathAttributes(const string &instance, const string &prefix,
        const string &path_id, uint32_t label, const CommunitySpec &comm_spec) {
        TASK_UTIL_EXPECT_TRUE(CheckPathAttributes(instance, prefix,
            path_id, label, vector<uint32_t>(), set<string>(), LoadBalance(),
            0, comm_spec, vector<uint16_t>()));
    }

    void VerifyPathAttributes(const string &instance, const string &prefix,
        const string &path_id, uint32_t label,
        const vector<uint16_t> &as_list) {
        TASK_UTIL_EXPECT_TRUE(CheckPathAttributes(instance, prefix,
            path_id, label, vector<uint32_t>(), set<string>(), LoadBalance(),
            0, CommunitySpec(), as_list));
    }

    void VerifyPathNoExists(const string &instance, const string &prefix,
        const string &path_id) {
        TASK_UTIL_EXPECT_TRUE(CheckPathNoExists(instance, prefix, path_id));
    }

    EventManager evm_;
    BgpServerTestPtr bgp_server_;
    Address::Family family_;
    string ipv6_prefix_;
    PeerMock *bgp_peer1_;
    PeerMock *bgp_peer2_;
    PeerMock *xmpp_peer1_;
    PeerMock *xmpp_peer2_;
};

// Specialization of GetFamily for INET.
template<>
Address::Family PathResolverTest<InetDefinition>::GetFamily() const {
    return Address::INET;
}

// Specialization of GetFamily for INET6.
template<>
Address::Family PathResolverTest<Inet6Definition>::GetFamily() const {
    return Address::INET6;
}

// Instantiate fixture class template for each TypeDefinition.
typedef ::testing::Types <InetDefinition, Inet6Definition> TypeDefinitionList;
TYPED_TEST_CASE(PathResolverTest, TypeDefinitionList);

//
// Add BGP path before XMPP path.
//
TYPED_TEST(PathResolverTest, SinglePrefix1) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
}

//
// Add BGP path after XMPP path.
//
TYPED_TEST(PathResolverTest, SinglePrefix2) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
}

//
// Add BGP path after XMPP path.
// Delete and add BGP path multiple times when path update list processing is
// disabled.
//
TYPED_TEST(PathResolverTest, SinglePrefixAddDelete) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    TASK_UTIL_EXPECT_EQ(0, this->ResolverPathUpdateListSize("blue"));
    this->DisableResolverPathUpdateProcessing("blue");

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
    TASK_UTIL_EXPECT_EQ(1, this->ResolverPathUpdateListSize("blue"));

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));
    TASK_UTIL_EXPECT_EQ(2, this->ResolverPathUpdateListSize("blue"));

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(2, this->ResolverPathUpdateListSize("blue"));

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));
    TASK_UTIL_EXPECT_EQ(3, this->ResolverPathUpdateListSize("blue"));

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(3, this->ResolverPathUpdateListSize("blue"));

    this->EnableResolverPathUpdateProcessing("blue");
    TASK_UTIL_EXPECT_EQ(0, this->ResolverPathUpdateListSize("blue"));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
}

//
// Change BGP path med after BGP path has been resolved.
//
TYPED_TEST(PathResolverTest, SinglePrefixChangeBgpPath1) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->AddBgpPathWithMed(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()), 100);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000, 100);

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->AddBgpPathWithMed(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()), 200);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000, 200);

    this->AddBgpPathWithMed(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()), 300);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000, 300);

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
}

//
// Change BGP path communities after BGP path has been resolved.
//
TYPED_TEST(PathResolverTest, SinglePrefixChangeBgpPath2) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    vector<uint32_t> comm_list1 = list_of(0xFFFFA101)(0xFFFFA102)(0xFFFFA103);
    CommunitySpec comm_spec1;
    comm_spec1.communities = comm_list1;
    this->AddBgpPathWithCommunities(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()), comm_list1);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000, comm_spec1);

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    vector<uint32_t> comm_list2 = list_of(0xFFFFA201)(0xFFFFA202)(0xFFFFA203);
    CommunitySpec comm_spec2;
    comm_spec2.communities = comm_list2;
    this->AddBgpPathWithCommunities(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()), comm_list2);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000, comm_spec2);

    vector<uint32_t> comm_list3 = list_of(0xFFFFA301)(0xFFFFA302)(0xFFFFA303);
    CommunitySpec comm_spec3;
    comm_spec3.communities = comm_list3;
    this->AddBgpPathWithCommunities(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()), comm_list3);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000, comm_spec3);

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
}

//
// Change BGP path aspath after BGP path has been resolved.
//
TYPED_TEST(PathResolverTest, SinglePrefixChangeBgpPath3) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    vector<uint16_t> as_list1 = list_of(64512)(64513)(64514);
    this->AddBgpPathWithAsList(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()), as_list1);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000, as_list1);

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    vector<uint16_t> as_list2 = list_of(64522)(64523)(64524);
    this->AddBgpPathWithAsList(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()), as_list2);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000, as_list2);

    vector<uint16_t> as_list3 = list_of(64532)(64533)(64534);
    this->AddBgpPathWithAsList(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()), as_list3);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000, as_list3);

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
}

//
// Change XMPP path label after BGP path has been resolved.
//
TYPED_TEST(PathResolverTest, SinglePrefixChangeXmppPath1) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10001);

    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10001);

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
}

//
// Change XMPP path nexthop after BGP path has been resolved.
//
TYPED_TEST(PathResolverTest, SinglePrefixChangeXmppPath2) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.2.1"), 10000);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.1"), 10000);

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
}

//
// Change XMPP path multiple times when nh update list processing is disabled.
//
TYPED_TEST(PathResolverTest, SinglePrefixChangeXmppPath3) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    TASK_UTIL_EXPECT_EQ(0, this->ResolverNexthopUpdateListSize("blue"));
    this->DisableResolverNexthopUpdateProcessing("blue");

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10001);
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(1, this->ResolverNexthopUpdateListSize("blue"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10002);
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(1, this->ResolverNexthopUpdateListSize("blue"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10003);
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(1, this->ResolverNexthopUpdateListSize("blue"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->EnableResolverNexthopUpdateProcessing("blue");
    TASK_UTIL_EXPECT_EQ(0, this->ResolverNexthopUpdateListSize("blue"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10003);

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
}

//
// Change and delete XMPP path when nh update list processing is disabled.
//
TYPED_TEST(PathResolverTest, SinglePrefixChangeXmppPath4) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    TASK_UTIL_EXPECT_EQ(0, this->ResolverNexthopUpdateListSize("blue"));
    this->DisableResolverNexthopUpdateProcessing("blue");

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10001);
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(1, this->ResolverNexthopUpdateListSize("blue"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(1, this->ResolverNexthopUpdateListSize("blue"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->EnableResolverNexthopUpdateProcessing("blue");
    TASK_UTIL_EXPECT_EQ(0, this->ResolverNexthopUpdateListSize("blue"));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
}

//
// Delete and resurrect XMPP path when nh update list processing is disabled.
//
TYPED_TEST(PathResolverTest, SinglePrefixChangeXmppPath5) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    TASK_UTIL_EXPECT_EQ(0, this->ResolverNexthopUpdateListSize("blue"));
    this->DisableResolverNexthopUpdateProcessing("blue");

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(1, this->ResolverNexthopUpdateListSize("blue"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10001);
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(1, this->ResolverNexthopUpdateListSize("blue"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->EnableResolverNexthopUpdateProcessing("blue");
    TASK_UTIL_EXPECT_EQ(0, this->ResolverNexthopUpdateListSize("blue"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10001);

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
}

//
// Change XMPP path sgid list.
//
TYPED_TEST(PathResolverTest, SinglePrefixChangeXmppPath6) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    vector<uint32_t> sgid_list1 = list_of(1)(2)(3)(4);
    this->AddXmppPathWithSecurityGroups(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000, sgid_list1);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000, sgid_list1);

    vector<uint32_t> sgid_list2 = list_of(3)(4)(5)(6);
    this->AddXmppPathWithSecurityGroups(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000, sgid_list2);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000, sgid_list2);

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
}

//
// Change XMPP path encap list.
//
TYPED_TEST(PathResolverTest, SinglePrefixChangeXmppPath7) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    set<string> encap_list1 = list_of("gre")("udp");
    this->AddXmppPathWithTunnelEncapsulations(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000, encap_list1);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000, encap_list1);

    set<string> encap_list2 = list_of("udp");
    this->AddXmppPathWithTunnelEncapsulations(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000, encap_list2);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000, encap_list2);

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
}

//
// Change XMPP path load balance property.
//
TYPED_TEST(PathResolverTest, SinglePrefixChangeXmppPath8) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);

    LoadBalance::bytes_type data1 = { { BgpExtendedCommunityType::Opaque,
        BgpExtendedCommunityOpaqueSubType::LoadBalance,
        0xFE, 0x00, 0x80, 0x00, 0x00, 0x00 } };
    LoadBalance lb1(data1);
    this->AddXmppPathWithLoadBalance(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000, lb1);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000, lb1);

    LoadBalance::bytes_type data2 = { { BgpExtendedCommunityType::Opaque,
        BgpExtendedCommunityOpaqueSubType::LoadBalance,
        0xaa, 0x00, 0x80, 0x00, 0x00, 0x00 } };
    LoadBalance lb2(data2);
    this->AddXmppPathWithLoadBalance(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000, lb2);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000, lb2);

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
}

//
// XMPP path has multiple ECMP nexthops.
// Change XMPP route from ECMP to non-ECMP and back.
//
TYPED_TEST(PathResolverTest, SinglePrefixWithEcmp) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000,
        this->BuildNextHopAddress("172.16.2.1"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.1"), 10000);

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.2.1"), 10000);
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.1"), 10000);

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000,
        this->BuildNextHopAddress("172.16.2.1"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.1"), 10000);

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.1"));

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000,
        this->BuildNextHopAddress("172.16.2.1"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.1"), 10000);

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.1"));

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
}

//
// BGP has multiple paths for same prefix, each with a different nexthop.
// Add and remove paths for the prefix.
//
TYPED_TEST(PathResolverTest, SinglePrefixWithMultipath) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *bgp_peer2 = this->bgp_peer2_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;
    PeerMock *xmpp_peer2 = this->xmpp_peer2_;

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));
    this->AddBgpPath(bgp_peer2, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer2->ToString()));

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10001);
    this->AddXmppPath(xmpp_peer2, "blue",
        this->BuildPrefix(bgp_peer2->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.2"), 10002);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10001);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.2"), 10002);

    this->DeleteBgpPath(bgp_peer2, "blue", this->BuildPrefix(1));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10001);
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.2"));

    this->AddBgpPath(bgp_peer2, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer2->ToString()));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10001);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.2"), 10002);

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.2"), 10002);

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10001);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.2"), 10002);

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    this->DeleteXmppPath(xmpp_peer2, "blue",
        this->BuildPrefix(bgp_peer2->ToString(), 32));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.2"));

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
    this->DeleteBgpPath(bgp_peer2, "blue", this->BuildPrefix(1));
}

//
// BGP has multiple paths for same prefix, each with a different nexthop.
// XMPP route for the nexthop is ECMP.
//
TYPED_TEST(PathResolverTest, SinglePrefixWithMultipathAndEcmp) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *bgp_peer2 = this->bgp_peer2_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;
    PeerMock *xmpp_peer2 = this->xmpp_peer2_;

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));
    this->AddBgpPath(bgp_peer2, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer2->ToString()));

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10001,
        this->BuildNextHopAddress("172.16.2.1"));
    this->AddXmppPath(xmpp_peer2, "blue",
        this->BuildPrefix(bgp_peer2->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.2"), 10002,
        this->BuildNextHopAddress("172.16.2.2"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10001);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.1"), 10001);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.2"), 10002);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.2"), 10002);

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.2.1"), 10001);
    this->AddXmppPath(xmpp_peer2, "blue",
        this->BuildPrefix(bgp_peer2->ToString(), 32),
        this->BuildNextHopAddress("172.16.2.2"), 10002);
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.1"), 10001);
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.2"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.2"), 10002);

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10001,
        this->BuildNextHopAddress("172.16.2.1"));
    this->AddXmppPath(xmpp_peer2, "blue",
        this->BuildPrefix(bgp_peer2->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.2"), 10002,
        this->BuildNextHopAddress("172.16.2.2"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10001);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.1"), 10001);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.2"), 10002);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.2"), 10002);

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10001);
    this->AddXmppPath(xmpp_peer2, "blue",
        this->BuildPrefix(bgp_peer2->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.2"), 10002);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10001);
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.1"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.2"), 10002);
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.2"));

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10003,
        this->BuildNextHopAddress("172.16.2.1"));
    this->AddXmppPath(xmpp_peer2, "blue",
        this->BuildPrefix(bgp_peer2->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.2"), 10004,
        this->BuildNextHopAddress("172.16.2.2"));
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"), 10003);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.1"), 10003);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.2"), 10004);
    this->VerifyPathAttributes("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.2"), 10004);

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    this->DeleteXmppPath(xmpp_peer2, "blue",
        this->BuildPrefix(bgp_peer2->ToString(), 32));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.1"));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.1"));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.1.2"));
    this->VerifyPathNoExists("blue", this->BuildPrefix(1),
        this->BuildNextHopAddress("172.16.2.2"));

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
    this->DeleteBgpPath(bgp_peer2, "blue", this->BuildPrefix(1));
}

//
// BGP has multiple prefixes, each with the same nexthop.
//
TYPED_TEST(PathResolverTest, MultiplePrefix) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(idx),
            this->BuildHostAddress(bgp_peer1->ToString()));
    }

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->VerifyPathAttributes("blue", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.1"), 10000);
    }

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->VerifyPathNoExists("blue", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.1"));
    }

    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(idx));
    }
}

//
// BGP has multiple prefixes, each with the same nexthop.
// Change XMPP path multiple times when path update list processing is disabled.
//
TYPED_TEST(PathResolverTest, MultiplePrefixChangeXmppPath1) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(idx),
            this->BuildHostAddress(bgp_peer1->ToString()));
    }

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->VerifyPathAttributes("blue", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.1"), 10000);
    }

    TASK_UTIL_EXPECT_EQ(0, this->ResolverPathUpdateListSize("blue"));
    this->DisableResolverPathUpdateProcessing("blue");

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10001);
    TASK_UTIL_EXPECT_EQ(DB::PartitionCount() * 2,
        this->ResolverPathUpdateListSize("blue"));
    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->VerifyPathAttributes("blue", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.1"), 10000);
    }

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10002);
    TASK_UTIL_EXPECT_EQ(DB::PartitionCount() * 2,
        this->ResolverPathUpdateListSize("blue"));
    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->VerifyPathAttributes("blue", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.1"), 10000);
    }

    this->EnableResolverPathUpdateProcessing("blue");
    TASK_UTIL_EXPECT_EQ(0, this->ResolverPathUpdateListSize("blue"));
    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->VerifyPathAttributes("blue", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.1"), 10002);
    }

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->VerifyPathNoExists("blue", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.1"));
    }

    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(idx));
    }
}

//
// BGP has multiple prefixes, each with the same nexthop.
// Change XMPP path and delete it path update list processing is disabled.
//
TYPED_TEST(PathResolverTest, MultiplePrefixChangeXmppPath2) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(idx),
            this->BuildHostAddress(bgp_peer1->ToString()));
    }

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->VerifyPathAttributes("blue", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.1"), 10000);
    }

    TASK_UTIL_EXPECT_EQ(0, this->ResolverPathUpdateListSize("blue"));
    this->DisableResolverPathUpdateProcessing("blue");

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10001);
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(DB::PartitionCount() * 2,
        this->ResolverPathUpdateListSize("blue"));
    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->VerifyPathAttributes("blue", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.1"), 10000);
    }

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(DB::PartitionCount() * 2,
        this->ResolverPathUpdateListSize("blue"));
    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->VerifyPathAttributes("blue", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.1"), 10000);
    }

    this->EnableResolverPathUpdateProcessing("blue");
    TASK_UTIL_EXPECT_EQ(0, this->ResolverPathUpdateListSize("blue"));
    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->VerifyPathNoExists("blue", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.1"));
    }

    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(idx));
    }
}

//
// BGP has 2 paths for all prefixes.
//
TYPED_TEST(PathResolverTest, MultiplePrefixWithMultipath) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *bgp_peer2 = this->bgp_peer2_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;
    PeerMock *xmpp_peer2 = this->xmpp_peer2_;

    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(idx),
            this->BuildHostAddress(bgp_peer1->ToString()));
        this->AddBgpPath(bgp_peer2, "blue", this->BuildPrefix(idx),
            this->BuildHostAddress(bgp_peer2->ToString()));
    }

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10001);
    this->AddXmppPath(xmpp_peer2, "blue",
        this->BuildPrefix(bgp_peer2->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.2"), 10002);
    task_util::WaitForIdle();

    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->VerifyPathAttributes("blue", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.1"), 10001);
        this->VerifyPathAttributes("blue", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.2"), 10002);
    }

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    this->DeleteXmppPath(xmpp_peer2, "blue",
        this->BuildPrefix(bgp_peer2->ToString(), 32));

    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->VerifyPathNoExists("blue", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.1"));
        this->VerifyPathNoExists("blue", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.2"));
    }

    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(idx));
        this->DeleteBgpPath(bgp_peer2, "blue", this->BuildPrefix(idx));
    }
}

//
// BGP has multiple prefixes in blue and pink tables, each with same nexthop.
//
TYPED_TEST(PathResolverTest, MultipleTableMultiplePrefix) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *xmpp_peer1 = this->xmpp_peer1_;

    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(idx),
            this->BuildHostAddress(bgp_peer1->ToString()));
        this->AddBgpPath(bgp_peer1, "pink", this->BuildPrefix(idx),
            this->BuildHostAddress(bgp_peer1->ToString()));
    }

    this->AddXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    this->AddXmppPath(xmpp_peer1, "pink",
        this->BuildPrefix(bgp_peer1->ToString(), 32),
        this->BuildNextHopAddress("172.16.1.1"), 10000);
    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->VerifyPathAttributes("blue", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.1"), 10000);
        this->VerifyPathAttributes("pink", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.1"), 10000);
    }

    this->DeleteXmppPath(xmpp_peer1, "blue",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    this->DeleteXmppPath(xmpp_peer1, "pink",
        this->BuildPrefix(bgp_peer1->ToString(), 32));
    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->VerifyPathNoExists("blue", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.1"));
        this->VerifyPathNoExists("pink", this->BuildPrefix(idx),
            this->BuildNextHopAddress("172.16.1.1"));
    }

    for (int idx = 1; idx <= DB::PartitionCount() * 2; ++idx) {
        this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(idx));
        this->DeleteBgpPath(bgp_peer1, "pink", this->BuildPrefix(idx));
    }
}

//
// Delete BGP path before it's nexthop is registered to condition listener.
//
TYPED_TEST(PathResolverTest, StopResolutionBeforeRegister) {
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *bgp_peer2 = this->bgp_peer2_;

    this->DisableResolverNexthopRegUnregProcessing("blue");
    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));
    this->AddBgpPath(bgp_peer2, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer2->ToString()));
    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
    this->DeleteBgpPath(bgp_peer2, "blue", this->BuildPrefix(1));
    this->EnableResolverNexthopRegUnregProcessing("blue");
}

//
// Shutdown server and resolver before all BGP paths are deleted.
// Deletion does not finish till BGP paths are deleted.
//
TYPED_TEST(PathResolverTest, Shutdown1) {
    BgpServerTestPtr bgp_server = this->bgp_server_;
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *bgp_peer2 = this->bgp_peer2_;

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));
    this->AddBgpPath(bgp_peer2, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer2->ToString()));

    bgp_server->Shutdown(false);
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(1, bgp_server->routing_instance_mgr()->count());

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
    this->DeleteBgpPath(bgp_peer2, "blue", this->BuildPrefix(1));
    TASK_UTIL_EXPECT_EQ(0, bgp_server->routing_instance_mgr()->count());
}

//
// Shutdown server and resolver before all BGP paths are deleted.
// Deletion does not finish till BGP paths are deleted and nexthop gets
// unregistered from condition listener.
//
TYPED_TEST(PathResolverTest, Shutdown2) {
    BgpServerTestPtr bgp_server = this->bgp_server_;
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *bgp_peer2 = this->bgp_peer2_;

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));
    this->AddBgpPath(bgp_peer2, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer2->ToString()));

    bgp_server->Shutdown(false);
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(1, bgp_server->routing_instance_mgr()->count());

    this->DisableResolverNexthopRegUnregProcessing("blue");
    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
    this->DeleteBgpPath(bgp_peer2, "blue", this->BuildPrefix(1));
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(1, bgp_server->routing_instance_mgr()->count());

    this->EnableResolverNexthopRegUnregProcessing("blue");
    TASK_UTIL_EXPECT_EQ(0, bgp_server->routing_instance_mgr()->count());
}

//
// Shutdown server and resolver before all resolver paths are deleted.
// Deletion does not finish till resolver paths are deleted and nexthop
// gets unregistered from condition listener.
//
TYPED_TEST(PathResolverTest, Shutdown3) {
    BgpServerTestPtr bgp_server = this->bgp_server_;
    PeerMock *bgp_peer1 = this->bgp_peer1_;
    PeerMock *bgp_peer2 = this->bgp_peer2_;

    this->AddBgpPath(bgp_peer1, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer1->ToString()));
    this->AddBgpPath(bgp_peer2, "blue", this->BuildPrefix(1),
        this->BuildHostAddress(bgp_peer2->ToString()));

    this->DisableResolverPathUpdateProcessing("blue");

    this->DeleteBgpPath(bgp_peer1, "blue", this->BuildPrefix(1));
    this->DeleteBgpPath(bgp_peer2, "blue", this->BuildPrefix(1));
    TASK_UTIL_EXPECT_EQ(2, this->ResolverPathUpdateListSize("blue"));

    bgp_server->Shutdown(false);
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(1, bgp_server->routing_instance_mgr()->count());
    TASK_UTIL_EXPECT_EQ(2, this->ResolverPathUpdateListSize("blue"));

    this->EnableResolverPathUpdateProcessing("blue");
    TASK_UTIL_EXPECT_EQ(0, bgp_server->routing_instance_mgr()->count());
}

class TestEnvironment : public ::testing::Environment {
    virtual ~TestEnvironment() { }
};

static void SetUp() {
    ControlNode::SetDefaultSchedulingPolicy();
    BgpServerTest::GlobalSetUp();
}

static void TearDown() {
    task_util::WaitForIdle();
    TaskScheduler *scheduler = TaskScheduler::GetInstance();
    scheduler->Terminate();
}

int main(int argc, char **argv) {
    bgp_log_test::init();
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new TestEnvironment());
    SetUp();
    int result = RUN_ALL_TESTS();
    TearDown();
    return result;
}
