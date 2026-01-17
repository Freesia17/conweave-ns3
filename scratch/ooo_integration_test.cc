// -----------------------------------------------------------------------------
// OooSystem Integration Test
// -----------------------------------------------------------------------------

#include "ns3/test.h"
#include "ns3/simulator.h"
#include "ns3/packet.h"
#include "ns3/ptr.h"
#include "ns3/callback.h"
#include "ns3/custom-header.h"
#include "ns3/log.h"
#include "ns3/nstime.h"
#include "ns3/uinteger.h"
#include "ns3/boolean.h"
#include "ns3/string.h"
#include "ns3/double.h"
#include "ns3/simple-ref-count.h"
#include "ns3/object-base.h"
#include "ns3/object.h"
#include "ns3/type-id.h"

#include <vector>
#include <deque>
#include <map>
#include <functional>
#include <algorithm>
#include <iostream>
#include <memory>
#include <unordered_map>

// --- Helper Macros for testing Private Members ---
#define private public
#define protected public

// Patch NS_LOG_COMPONENT_DEFINE
#include "ns3/log.h"
#undef NS_LOG_COMPONENT_DEFINE
#define NS_LOG_COMPONENT_DEFINE(name) static ns3::LogComponent g_log = ns3::LogComponent(name "_IntegrationTest")

// Include source file directly for white-box access
#define OOO_SYSTEM_TEST_INCLUSION
#include "src/point-to-point/model/ooo-system.cc"

using namespace ns3;

// Aliases
using TestImpl = OooSystemAdapter::Impl;

// -----------------------------------------------------------------------------
// Global Variables & Test Environment
// -----------------------------------------------------------------------------
OooSystemAdapter::Config g_cfg;
TestImpl* g_impl = nullptr;

// Packet Tag for verification
struct TestPacketTag : public Tag {
  static TypeId GetTypeId(void) {
    static TypeId tid = TypeId("ns3::IntegrationTestPacketTag")
                            .SetParent<Tag>()
                            .AddConstructor<TestPacketTag>();
    return tid;
  }
  virtual TypeId GetInstanceTypeId(void) const { return GetTypeId(); }
  virtual uint32_t GetSerializedSize(void) const { return sizeof(int) * 3; }
  virtual void Serialize(TagBuffer i) const {
    i.Write((const uint8_t *)&flow_id, sizeof(int));
    i.Write((const uint8_t *)&seq, sizeof(int));
    i.Write((const uint8_t *)&timestamp, sizeof(int)); // Sim time in micros/unit?
  }
  virtual void Deserialize(TagBuffer i) {
    i.Read((uint8_t *)&flow_id, sizeof(int));
    i.Read((uint8_t *)&seq, sizeof(int));
    i.Read((uint8_t *)&timestamp, sizeof(int));
  }
  virtual void Print(std::ostream &os) const {
    os << "Flow=" << flow_id << " Seq=" << seq;
  }
  
  int flow_id = -1;
  int seq = -1;
  int timestamp = 0;
};

// Global recording
struct FlowStats {
    int last_seq = -1;
    int received_count = 0;
    bool out_of_order_detected = false;
};

std::unordered_map<int, FlowStats> g_flow_stats;
int g_udp_recv_count = 0;

void MockUdpHandler(Ptr<Packet> p, const CustomHeader &ch) {
    g_udp_recv_count++;
    
    TestPacketTag tag;
    if (p->PeekPacketTag(tag)) {
        int flow = tag.flow_id;
        int seq = tag.seq;
        
        FlowStats &stats = g_flow_stats[flow];
        stats.received_count++;
        
        if (seq <= stats.last_seq) {
            std::cout << "OOO Detected! Flow=" << flow << " Seq=" << seq 
                      << " Last=" << stats.last_seq << std::endl;
            stats.out_of_order_detected = true;
        }
        stats.last_seq = seq;
    }
}

void Reset() {
    if (g_impl) {
        delete g_impl;
        g_impl = nullptr;
    }
    
    g_cfg = OooSystemAdapter::Config();
    
    // Default config (customizable per test)
    g_cfg.burst_table_volume = 128; // Large enough
    g_cfg.big_flow_threshold = 5;
    g_cfg.blockqueue_pool_size = 100;
    
    // Deterministic latencies
    g_cfg.pcie_latency_ns = 10;
    g_cfg.slowpath_process_ns = 100;
    g_cfg.slowpath_process_cycle_ns = 10;
    g_cfg.slowpath_rule_latency_ns = 50;
    
    g_impl = new TestImpl(g_cfg);
    g_impl->udpHandler = MakeCallback(&MockUdpHandler);
    
    g_udp_recv_count = 0;
    g_flow_stats.clear();
}

void EnqueuePacket(int flow_id, int seq, int burst_idx = -1) {
    // If burst_idx is not forced (-1), look it up!
    if (burst_idx == -1) {
        burst_idx = g_impl->burst.Lookup(flow_id);
    }
    
    // PacketCtx setup...
    PacketCtx ctx;
    ctx.flow_id = flow_id;
    ctx.burst_table_index = burst_idx; 
    
    ctx.packet_id = (int64_t)seq; // Use packet_id
    ctx.pkt = Create<Packet>(100);
    
    // Tagging
    TestPacketTag tag;
    tag.flow_id = flow_id;
    tag.seq = seq;
    tag.timestamp = Simulator::Now().GetNanoSeconds();
    ctx.pkt->AddPacketTag(tag);
    
    g_impl->blockq.OnPacket(ctx, burst_idx);
}

void RunFor(uint64_t ns) {
    Simulator::Stop(Simulator::Now() + NanoSeconds(ns));
    Simulator::Run();
}

#define ASSERT_EQ(a,b) if((a)!=(b)) { std::cout << "Assertion failed at " << __LINE__ << ": " << #a << " (" << (a) << ") != " << #b << " (" << (b) << ")" << std::endl; exit(1); }
#define ASSERT_TRUE(a) if(!(a)) { std::cout << "Assertion failed at " << __LINE__ << ": " << #a << " is false." << std::endl; exit(1); }
#define ASSERT_FALSE(a) ASSERT_TRUE(!(a))

// -----------------------------------------------------------------------------
// Test Cases
// -----------------------------------------------------------------------------

void TestInitialization() {
    std::cout << "[RUNNING] TestInitialization..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    RunFor(100);
    ASSERT_EQ(g_udp_recv_count, 0);
    std::cout << "[PASSED] TestInitialization" << std::endl;
}

void TestSteadyStateBigFlow() {
    std::cout << "[RUNNING] TestSteadyStateBigFlow..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    // 1. Establish a "Big Flow" in Stable Table manually.
    int flow_id = 99;
    Rule r;
    r.update_entry = 99; // Assume entry matches flow_id for simplicity?
    // Wait, StableTable uses `table` map: unordered_map<int32_t, int>.
    // Key=FlowID, Value=Index in burst table? Or Rule Index?
    // Actually StableTable definition:
    // std::vector<int32_t> table_; // If array based?
    // struct StableTableState {
    //   std::unordered_map<int32_t, int> table; 
    //   ...
    // }
    
    // Let's manually insert into `g_impl->stable.table`.
    // Key: flow_id (99). Value: -2 (Fast Path, invalid burst index?)
    // StableTable logic:
    // if (table.count(fid)) { idx = table[fid]; ... }
    // If idx >= 0 -> Slow Path (Burst Table Index).
    // If idx == -1? -> Miss?
    // If idx == -2 (or specialized)? 
    // Wait, `StableTableState` stores `burst_table_index`.
    // BUT FastPath flows usually DON'T have a burst table index?
    // Actually `OooSystem` assumes "Stable" means "Has a Rule"?
    // If Has Rule, where does it go?
    
    // Let's check `Active` flows logic.
    // If `stable.table` has entry, it forwards to `ForwardFast`.
    // Wait, `OnPacket` in `StableTable`:
    /*
       if (table.count(ctx.flow_id)) {
          // Found.
          int idx = table[ctx.flow_id];
          // ...
          owner->ForwardFast(ctx);
       }
    */
    // So just being in the table implies forwarding.
    // StableTable uses std::set<int32_t> table; (Key-only)
    // Just presence implies FastPath forwarding.
    g_impl->stable.table.insert(flow_id);
    
    // 2. Send packets
    for (int i=0; i<10; ++i) {
        EnqueuePacket(flow_id, i, -1);
    }
    
    // 3. Process
    // FastPath: PCIe latency * 2 (Flow -> Forward).
    RunFor(500);
    
    // 4. Verify
    ASSERT_EQ(g_udp_recv_count, 10);
    ASSERT_EQ(g_flow_stats[flow_id].received_count, 10);
    ASSERT_FALSE(g_flow_stats[flow_id].out_of_order_detected);
    
    std::cout << "[PASSED] TestSteadyStateBigFlow" << std::endl;
}

// test function setup
void TestColdToHotTransition() {
    std::cout << "[RUNNING] TestColdToHotTransition..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    // Config: Threshold 3 (Easier). Rule Latency 50ns.
    delete g_impl;
    g_cfg.big_flow_threshold = 3;
    g_impl = new TestImpl(g_cfg);
    g_impl->udpHandler = MakeCallback(&MockUdpHandler);
    g_udp_recv_count = 0;
    g_flow_stats.clear();
    
    int flow_id = 101;
    
    // 1. Send burst of packets (100 to ensure Queue fill)
    for (int i=0; i<100; ++i) {
        EnqueuePacket(flow_id, i, -1);
        if (i > 0 && i % 10 == 0) {
            RunFor(500); // 500ns pacing (allow burst update, but keep inside rule latency)
        }
    }
    
    // 2. Allow processing
    RunFor(100000); // 100us plenty
    
    // 3. Verify
    ASSERT_EQ(g_udp_recv_count, 100);
    ASSERT_EQ(g_flow_stats[flow_id].received_count, 100);
    ASSERT_FALSE(g_flow_stats[flow_id].out_of_order_detected);
    
    // 4. Verify Rule was installed
    int burst_idx = g_impl->burst.Lookup(flow_id);
    std::cout << "Debug: Flow " << flow_id << " BurstIdx=" << burst_idx 
              << " Stable=" << g_impl->stable.table.count(flow_id) << std::endl;
    ASSERT_TRUE(g_impl->stable.table.count(flow_id) > 0);
    
    std::cout << "[PASSED] TestColdToHotTransition" << std::endl;
}

void TestStableTableContention() {
    std::cout << "[RUNNING] TestStableTableContention..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    // 1. Shrink Stable Table
    // Use smaller volume for test
    // Need to restart Impl to take effect? Config passed to Constructor.
    g_cfg.stable_table_volume = 3; // Very small
    delete g_impl;
    g_impl = new TestImpl(g_cfg);
    g_impl->udpHandler = MakeCallback(&MockUdpHandler);
    g_udp_recv_count = 0;
    g_flow_stats.clear();
    
    // 2. Generate 5 Big Flows (exceeds capacity 3)
    // Threshold is 5. Send 10 packets each.
    // Flows 0, 1, 2 should install rule.
    // Flow 3 should kick out someone (LRU or Random? Implementation dependent).
    // Flow 4 kicks out another.
    
    for (int flow=0; flow<5; ++flow) {
        for (int i=0; i<10; ++i) {
            EnqueuePacket(flow, i, -1);
            RunFor(1);
        }
    }
    
    RunFor(5000);
    
    // 3. Verify delivery
    // Some might go Slowpath (if rule displaced).
    // Some Fastpath.
    // ALL must be delivered.
    int total_expected = 5 * 10;
    ASSERT_EQ(g_udp_recv_count, total_expected);
    
    // Verify no OOO
    for (int flow=0; flow<5; ++flow) {
        ASSERT_FALSE(g_flow_stats[flow].out_of_order_detected);
        ASSERT_EQ(g_flow_stats[flow].received_count, 10);
    }
    
    // Verify Table Full (Size == 3)
    ASSERT_EQ(g_impl->stable.table.size(), 3u);
    
    std::cout << "[PASSED] TestStableTableContention" << std::endl;
}

void TestBurstTableContention() {
    std::cout << "[RUNNING] TestBurstTableContention..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    // 1. Shrink Burst Table Volume
    // "burst_table_volume" controls RuleAllocation size and queues vector size?
    g_cfg.burst_table_volume = 5; 
    // And pool size?
    g_cfg.blockqueue_pool_size = 10; 
    
    delete g_impl;
    g_impl = new TestImpl(g_cfg);
    g_impl->udpHandler = MakeCallback(&MockUdpHandler);
    g_udp_recv_count = 0;
    g_flow_stats.clear();
    
    // 2. Start more flows than burst table slots
    // Send 10 distinct flows. Each triggers allocation.
    // After 5, allocation fails.
    // Packets should be queued in MissQueue? Or dropped if MissQueue full?
    // Or Slowpath just drops?
    // BlockQueue logic: if alloc -1, sends to miss queue (index -1).
    // So packets SHOULD be delivered via Miss Queue Round Robin.
    
    for (int flow=0; flow<10; ++flow) {
        EnqueuePacket(flow, 0, -1); // Just 1 packet to allocate
        RunFor(1);
    }
    
    RunFor(2000);
    
    // 3. Verify delivery of all 10 packets
    ASSERT_EQ(g_udp_recv_count, 10);
    
    // Verify RuleAllocation is full (5 slots used)
    // Need access to flow.rule_alloc.Size() or similar.
    // Or just check that we survived.
    // The test ensures "Crash Freedom" and "Delivery" under contention.

    std::cout << "[PASSED] TestBurstTableContention" << std::endl;
}

void TestMixedTraffic() {
    std::cout << "[RUNNING] TestMixedTraffic..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    // Complex Scenario
    // 5 Hot Flows (Pre-installed)
    // 5 New Flows (Cold triggering rules)
    // 5 Background Small Flows (Below threshold)
    
    // 1. Install Hot Flows
    for(int i=0; i<5; ++i) {
        g_impl->stable.table.insert(i);
    }
    
    int pkt_per_flow = 50;
    
    // 2. Generator Loop
    // Shuffle send order to mix traffic
    struct PktReq { int flow; int seq; };
    std::vector<PktReq> reqs;
    
    // Hot flows 0-4
    for(int f=0; f<5; ++f) {
        for(int s=0; s<pkt_per_flow; ++s) reqs.push_back({f, s});
    }
    // Cold Flows 5-9
    for(int f=5; f<10; ++f) {
        for(int s=0; s<pkt_per_flow; ++s) reqs.push_back({f, s});
    }
    // Small Flows 10-14 (Only 3 packets each -> < 5 threshold)
    for(int f=10; f<15; ++f) {
        for(int s=0; s<3; ++s) reqs.push_back({f, s});
    }
    
    // Shuffle
    // (Simple pseudo-shuffle or just interleaved)
    // std::shuffle needs random generator.
    // Deterministic verify:
    // Just interleave manually or use simple deterministic pattern
    std::vector<PktReq> shuffled;
    size_t n = reqs.size();
    size_t prime = 1009; 
    for(size_t i=0; i<n; ++i) {
        shuffled.push_back(reqs[(i * prime) % n]);
    }
    
    // 3. Send
    for(const auto& req : shuffled) {
        EnqueuePacket(req.flow, req.seq, -1);
        RunFor(1);
    }
    
    // 4. Drain Integration
    RunFor(50000); 
    
    // 5. Verify
    ASSERT_EQ((int)g_udp_recv_count, (int)reqs.size());
    
    for(const auto& req : reqs) {
        // Just check flows generally
        int f = req.flow;
        ASSERT_FALSE(g_flow_stats[f].out_of_order_detected);
        if (f < 10) {
            ASSERT_EQ(g_flow_stats[f].received_count, pkt_per_flow);
        } else {
            ASSERT_EQ(g_flow_stats[f].received_count, 3);
        }
    }

    std::cout << "[PASSED] TestMixedTraffic" << std::endl;
}

int main(int argc, char *argv[]) {
    TestInitialization();
    TestSteadyStateBigFlow();
    TestColdToHotTransition();
    TestStableTableContention();
    TestBurstTableContention();
    TestMixedTraffic();
    
    std::cout << "\n===========================================" << std::endl;
    std::cout << "All Integration tests passed!" << std::endl;
    std::cout << "===========================================" << std::endl;
    return 0;
}
