/*
 * Copyright (c) 2015 Juniper Networks, Inc. All rights reserved.
 */

#ifndef vnsw_agent_flow_stats_collector_test_h
#define vnsw_agent_flow_stats_collector_test_h

#include <vector>
#include <vrouter/flow_stats/flow_stats_collector.h>
#include <sandesh/common/flow_types.h>

class FlowStatsCollectorTest : public FlowStatsCollector {
public:
    FlowStatsCollectorTest(boost::asio::io_service &io, int intvl,
                           uint32_t flow_cache_timeout,
                           AgentUveBase *uve);
    virtual ~FlowStatsCollectorTest();
    void DispatchFlowMsg(const std::vector<FlowDataIpv4> &msg_list);
    FlowDataIpv4 last_sent_flow_log() const;
    std::vector<FlowDataIpv4> ingress_flow_log_list() const {
        return ingress_flow_log_list_;
    }
    void ClearList();

private:
    FlowDataIpv4 flow_log_;
    std::vector<FlowDataIpv4> ingress_flow_log_list_;
};

#endif  //  vnsw_agent_flow_stats_collector_test_h
