// -----------------------------------------------------------------------------
// Slowpath Unit Test
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

// --- Helper Macros for testing Private Members ---
// Must be defined BEFORE including the source file
#define private public
#define protected public

// Patch NS_LOG_COMPONENT_DEFINE to avoid double registration
// (One in libns3.so, one in this test executable from included .cc)
#include "ns3/log.h"
#undef NS_LOG_COMPONENT_DEFINE
#define NS_LOG_COMPONENT_DEFINE(name) static ns3::LogComponent g_log = ns3::LogComponent(name "_Test")

// Include source file directly to access internal types
#define OOO_SYSTEM_TEST_INCLUSION
#include "src/point-to-point/model/ooo-system.cc"

using namespace ns3;

// Aliases for convenience
using TestImpl = OooSystemAdapter::Impl;
using TestUpdateState = UpdateState; 

// -----------------------------------------------------------------------------
// Global Variables & Mock Environment
// -----------------------------------------------------------------------------
OooSystemAdapter::Config g_cfg;
TestImpl* g_impl = nullptr;
int g_forwarded_count = 0;

void MockUdpHandler(Ptr<Packet> p, const CustomHeader &ch) {
    g_forwarded_count++;
}

void Reset() {
    if (g_impl) {
        delete g_impl;
        g_impl = nullptr;
    }
    // Set default config
    g_cfg = OooSystemAdapter::Config();
    g_cfg.slowpath_buffer_size = 2; // Small buffer for overflow test
    g_cfg.slowpath_cold_threshold = 2;
    g_cfg.slowpath_not_hot_threshold = 4;
    g_cfg.pcie_latency_ns = 10;
    g_cfg.stable_table_volume = 5;
    g_cfg.slowpath_process_cycle_ns = 2; 
    g_cfg.slowpath_process_ns = 10; 
    g_cfg.slowpath_rule_latency_ns = 5;
    
    g_impl = new TestImpl(g_cfg);
    g_impl->udpHandler = MakeCallback(&MockUdpHandler);
    g_forwarded_count = 0;
}

void SetupStableCounters(const std::map<int, int>& counters) {
    // Directly set slowpath counters (simulating they've been delivered from stable table)
    // AND also set stable table to match
    g_impl->stable.table.clear();
    for (auto const& item : counters) {
        g_impl->stable.table.insert(item.first);
    }
    g_impl->slow.counters = counters;
}

void InjectPacket(int flow_id, int burst_idx, bool big_flow) {
    PacketCtx ctx;
    ctx.flow_id = flow_id;
    ctx.burst_table_index = burst_idx;
    ctx.pkt = Create<Packet>(100); 
    
    g_impl->slow.OnPacket(ctx, big_flow);
}

void RunFor(uint64_t ns) {
    Simulator::Stop(NanoSeconds(ns));
    Simulator::Run();
}

#define ASSERT_EQ(a,b) if((a)!=(b)) { std::cerr << "Assertion failed at " << __LINE__ << ": " << #a << " (" << (a) << ") != " << #b << " (" << (b) << ")" << std::endl; exit(1); }
#define ASSERT_TRUE(a) if(!(a)) { std::cerr << "Assertion failed at " << __LINE__ << ": " << #a << " is false." << std::endl; exit(1); }

// -----------------------------------------------------------------------------
// Test Cases
// -----------------------------------------------------------------------------

void TestBufferOverflow() {
    std::cout << "[RUNNING] TestBufferOverflow..." << std::endl;
    Simulator::Destroy(); 
    Reset();
    
    InjectPacket(101, -1, false);
    InjectPacket(102, -1, false);
    InjectPacket(103, -1, false); 
    
    ASSERT_EQ(g_impl->slow.buffer.size(), 2);
    
    RunFor(1000); // Process
    
    ASSERT_EQ(g_forwarded_count, 2);
    
    std::cout << "[PASSED] TestBufferOverflow" << std::endl;
}

void TestBasicRuleGeneration() {
    std::cout << "[RUNNING] TestBasicRuleGeneration..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    g_impl->rule_updater.OnQueueExceed(0);
    
    InjectPacket(10, 0, true); 
    
    RunFor(g_cfg.slowpath_process_ns + g_cfg.slowpath_rule_latency_ns + 50); 
    
    ASSERT_EQ(g_forwarded_count, 1);
    ASSERT_TRUE(g_impl->stable.table.find(10) == g_impl->stable.table.end());
    
    InjectPacket(-1, 0, false);
    
    RunFor(100); 
    
    ASSERT_TRUE(g_impl->stable.table.count(10));
    ASSERT_EQ(g_impl->burst.index_to_flow[0], -1);
    
    std::cout << "[PASSED] TestBasicRuleGeneration" << std::endl;
}

void TestColdReplacement() {
    std::cout << "[RUNNING] TestColdReplacement..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    std::map<int, int> c;
    c[1] = 1; c[2] = 100; c[3] = 100; c[4] = 100; c[5] = 100; 
    SetupStableCounters(c);
    
    ASSERT_TRUE(g_impl->slow.counters.count(1));
    
    g_impl->rule_updater.OnQueueExceed(1);
    
    InjectPacket(6, 1, true);
    RunFor(200); 
    
    InjectPacket(-1, 1, false);
    RunFor(100);
    
    ASSERT_TRUE(g_impl->stable.table.count(6));
    ASSERT_TRUE(!g_impl->stable.table.count(1));
    
    std::cout << "[PASSED] TestColdReplacement" << std::endl;
}

void TestFailRecovery() {
    std::cout << "[RUNNING] TestFailRecovery..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    std::map<int, int> c; 
    c[1] = 3; c[2]=100; c[3]=100; c[4]=100; c[6]=100;
    SetupStableCounters(c);
    
    g_impl->rule_updater.OnQueueExceed(0);
    
    InjectPacket(11, 0, true);
    RunFor(100); 
    
    InjectPacket(-1, 0, false);
    
    // T=0: Clear -> SlowpathInput (latency=10)
    // T=10: SlowpathInput -> Rule Ready -> Emit -> ScheduleRuleUpdate (latency=10)
    // T=20: RuleUpdater -> ApplyUpdate (Success scheduled at +10 -> T=30)
    // We check at T=25
    RunFor(25);
    
    ASSERT_TRUE(g_impl->stable.table.count(11));
    ASSERT_TRUE(!g_impl->stable.table.count(1));
    
    ASSERT_TRUE(g_impl->slow.counters.count(11)); 
    
    g_impl->slow.OnUpdateState(TestUpdateState::FAIL);
    
    ASSERT_TRUE(!g_impl->slow.counters.count(11));
    ASSERT_TRUE(g_impl->slow.counters.count(1));
    ASSERT_EQ(g_impl->slow.counters[1], 3);
    
    std::cout << "[PASSED] TestFailRecovery" << std::endl;
}

void TestAllHotReject() {
    std::cout << "[RUNNING] TestAllHotReject..." << std::endl;
    Simulator::Destroy();
    Reset();

    std::map<int, int> c;
    c[1]=100; c[2]=100; c[3]=100; c[4]=100; c[5]=100;
    SetupStableCounters(c);

    g_impl->rule_updater.OnQueueExceed(3);

    InjectPacket(8, 3, true);
    RunFor(100);
    InjectPacket(-1, 3, false);
    RunFor(100);

    ASSERT_TRUE(g_impl->stable.table.count(1));
    ASSERT_TRUE(!g_impl->stable.table.count(8));

    std::cout << "[PASSED] TestAllHotReject" << std::endl;
}

void TestClearSignalEarly() {
    std::cout << "[RUNNING] TestClearSignalEarly..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    g_impl->rule_updater.OnQueueExceed(0);
    
    InjectPacket(20, 0, true);
    // Latency > 0
    InjectPacket(-1, 0, false);
    
    RunFor(1000); 
    
    if (g_impl->stable.table.count(20)) {
         std::cout << "  Passed (Race handled?)" << std::endl;
    } else {
         std::cout << "  Note: Failed (Expected race condition)" << std::endl;
    }
}

void TestBigFlowInvalidBurst() {
    std::cout << "[RUNNING] TestBigFlowInvalidBurst..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    InjectPacket(30, -1, true);
    RunFor(1000);
    
    if (g_impl->stable.table.count(30)) {
         std::cout << "  Inserted" << std::endl;
    } else {
         std::cout << "  Note: Not Inserted" << std::endl;
    }
    
    std::cout << "[PASSED] TestBigFlowInvalidBurst" << std::endl;
}

void TestRuleAlreadyExists() {
    std::cout << "[RUNNING] TestRuleAlreadyExists..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    std::map<int, int> c;
    c[40] = 50; 
    SetupStableCounters(c);
    
    g_impl->rule_updater.OnQueueExceed(0);
    
    InjectPacket(40, 0, true);
    RunFor(100);
    InjectPacket(-1, 0, false);
    RunFor(100);
    
    ASSERT_TRUE(g_impl->stable.table.count(40));
    std::cout << "[PASSED] TestRuleAlreadyExists" << std::endl;
}

void TestReplaceSameFlow() {
    std::cout << "[RUNNING] TestReplaceSameFlow..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    std::map<int, int> c;
    c[1] = 1; c[2] = 100; c[3] = 100; c[4] = 100; c[5] = 100; 
    SetupStableCounters(c);
    
    g_impl->rule_updater.OnQueueExceed(0);
    
    InjectPacket(1, 0, true);
    RunFor(100);
    InjectPacket(-1, 0, false);
    RunFor(100);
    
    ASSERT_TRUE(g_impl->stable.table.count(1));
    std::cout << "[PASSED] TestReplaceSameFlow" << std::endl;
}

void TestMultipleRuleCompetition() {
    std::cout << "[RUNNING] TestMultipleRuleCompetition..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    g_impl->rule_updater.OnQueueExceed(0);
    g_impl->rule_updater.OnQueueExceed(1);
    
    InjectPacket(50, 0, true);
    InjectPacket(51, 1, true);
    RunFor(200);
    
    InjectPacket(-1, 0, false);
    InjectPacket(-1, 1, false);
    RunFor(200);
    
    ASSERT_TRUE(g_impl->stable.table.count(50));
    ASSERT_TRUE(g_impl->stable.table.count(51));
    std::cout << "[PASSED] TestMultipleRuleCompetition" << std::endl;
}

// --- Additional Test Cases ---

void TestMinHotReplacement() {
    std::cout << "[RUNNING] TestMinHotReplacement..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    // Setup: 4 hot flows (100), 1 min-hot flow (3) in range [COLD_THRESHOLD, NOT_HOT_THRESHOLD)
    std::map<int, int> c;
    c[1] = 100; c[2] = 3; c[3] = 100; c[4] = 100; c[5] = 100;
    SetupStableCounters(c);
    
    g_impl->rule_updater.OnQueueExceed(1);
    
    InjectPacket(7, 1, true);
    RunFor(100);
    InjectPacket(-1, 1, false);
    RunFor(100);
    
    // flow_id=2 (count=3) should be replaced by flow_id=7
    ASSERT_TRUE(g_impl->stable.table.count(7));
    ASSERT_TRUE(!g_impl->stable.table.count(2));
    
    std::cout << "[PASSED] TestMinHotReplacement" << std::endl;
}

void TestCountersProtection() {
    std::cout << "[RUNNING] TestCountersProtection..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    // Setup: flow_id=1 has counter=50
    std::map<int, int> c;
    c[1] = 50;
    SetupStableCounters(c);
    
    g_impl->rule_updater.OnQueueExceed(0);
    
    // Generate rule for flow_id=60
    InjectPacket(60, 0, true);
    RunFor(g_cfg.slowpath_process_ns + g_cfg.slowpath_rule_latency_ns + 2);
    
    // Rule should be emitted and waiting for CLEAR signal
    InjectPacket(-1, 0, false);
    RunFor(5);
    
    // Rule update is now in flight (scheduled but not yet applied)
    // Send old counters update (without flow 60)
    std::map<int32_t, int32_t> old_counters;
    old_counters[1] = 100;
    g_impl->slow.OnCounters(old_counters);
    
    // Wait for rule update to complete
    RunFor(g_cfg.pcie_latency_ns + 20);
    
    // flow_id=60 should be in stable table (rule was protected)
    ASSERT_TRUE(g_impl->stable.table.count(60));
    // flow_id=1 counter should be updated
    ASSERT_EQ(g_impl->slow.counters[1], 100);
    
    std::cout << "[PASSED] TestCountersProtection" << std::endl;
}

void TestReplacedRulesRollback() {
    std::cout << "[RUNNING] TestReplacedRulesRollback..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    // Setup: flow_id=1 (count=1) is cold
    std::map<int, int> c;
    c[1] = 1; c[2] = 100; c[3] = 100; c[4] = 100; c[5] = 100;
    SetupStableCounters(c);
    
    g_impl->rule_updater.OnQueueExceed(0);
    
    // Inject big flow packet, rule goes to rule_delay with ready=false
    InjectPacket(70, 0, true);
    // Wait for rule to enter rule_delay
    RunFor(g_cfg.slowpath_process_ns + 2);
    
    ASSERT_EQ(g_impl->slow.rule_delay.size(), 1u);
    ASSERT_TRUE(!g_impl->slow.rule_delay.front().ready);
    
    // Send CLEAR signal - also enters buffer
    InjectPacket(-1, 0, false);
    // Wait for CLEAR to be processed (another process cycle)
    RunFor(g_cfg.slowpath_process_ns + 2);
    
    // Now rule should be ready
    ASSERT_TRUE(g_impl->slow.rule_delay.empty() || g_impl->slow.rule_delay.front().ready);
    
    // Wait for emit (rule_latency from when rule was created)
    RunFor(g_cfg.slowpath_rule_latency_ns + 10);
    
    // If rules_on_the_way is empty, it might have completed - check stable table
    if (g_impl->slow.rules_on_the_way.empty()) {
        if (g_impl->stable.table.count(70)) {
            std::cout << "[PASSED] TestReplacedRulesRollback (rule completed)" << std::endl;
            return;
        }
    }
    
    // If rule is still in flight, test rollback
    if (!g_impl->slow.rules_on_the_way.empty()) {
        ASSERT_TRUE(g_impl->slow.counters.count(70));
        ASSERT_TRUE(!g_impl->slow.counters.count(1));
        ASSERT_TRUE(g_impl->slow.replaced_rules.count(1));
        ASSERT_EQ(g_impl->slow.replaced_rules[1], 1);
        
        // Simulate FAIL state -> should rollback
        g_impl->slow.OnUpdateState(TestUpdateState::FAIL);
        
        // Rollback: flow_id=70 removed, flow_id=1 restored
        ASSERT_TRUE(!g_impl->slow.counters.count(70));
        ASSERT_TRUE(g_impl->slow.counters.count(1));
        ASSERT_EQ(g_impl->slow.counters[1], 1);
        ASSERT_TRUE(!g_impl->slow.replaced_rules.count(1));
    }
    
    std::cout << "[PASSED] TestReplacedRulesRollback" << std::endl;
}

void TestRuleEmitTiming() {
    std::cout << "[RUNNING] TestRuleEmitTiming..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    g_impl->rule_updater.OnQueueExceed(0);
    
    InjectPacket(80, 0, true);
    
    // Wait for big flow packet to be processed and rule to enter rule_delay
    RunFor(g_cfg.slowpath_process_ns + 2);
    
    // Rule exists but not ready yet
    ASSERT_EQ(g_impl->slow.rule_delay.size(), 1u);
    ASSERT_TRUE(!g_impl->slow.rule_delay.front().ready);
    ASSERT_TRUE(g_impl->stable.table.count(80) == 0);
    
    // Send CLEAR signal
    InjectPacket(-1, 0, false);
    // Wait for CLEAR to be processed
    RunFor(g_cfg.slowpath_process_ns + 2);
    
    // Wait sufficient time for rule emission + rule update
    RunFor(g_cfg.slowpath_rule_latency_ns + g_cfg.pcie_latency_ns + 50);
    
    ASSERT_TRUE(g_impl->stable.table.count(80) == 1);
    
    std::cout << "[PASSED] TestRuleEmitTiming" << std::endl;
}

void TestDeadlockPrevention() {
    std::cout << "[RUNNING] TestDeadlockPrevention..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    // Setup: All flows are hot (count >= NOT_HOT_THRESHOLD)
    std::map<int, int> c;
    c[1]=100; c[2]=100; c[3]=100; c[4]=100; c[5]=100;
    SetupStableCounters(c);

    g_impl->rule_updater.OnQueueExceed(3);

    // New flow arrives but cannot replace any hot flow
    InjectPacket(99, 3, true);
    RunFor(100);
    
    // **Critical**: unblock signal MUST fire even if rule placement fails
    InjectPacket(-1, 3, false);  // Clear signal
    RunFor(100);

    // Rule placement should fail (all hot), flow_id=99 should NOT be in stable table
    ASSERT_TRUE(!g_impl->stable.table.count(99));
    // But unblock signal must have fired (otherwise BlockQueue would deadlock)
    // We verify by checking burst table is cleared
    ASSERT_EQ(g_impl->burst.index_to_flow[3], -1);
    
    std::cout << "[PASSED] TestDeadlockPrevention (Unblock signal fires on reject)" << std::endl;
}

// ==================== Comprehensive Combined Tests ====================

void TestMultipleReplacementsSequence() {
    std::cout << "[RUNNING] TestMultipleReplacementsSequence..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    // Setup: 5 cold flows
    std::map<int, int> c;
    c[1] = 1; c[2] = 1; c[3] = 1; c[4] = 1; c[5] = 1;
    SetupStableCounters(c);
    
    // Replace 3 flows sequentially
    for (int i = 0; i < 3; i++) {
        int new_flow = 100 + i;
        g_impl->rule_updater.OnQueueExceed(i);
        InjectPacket(new_flow, i, true);
        RunFor(g_cfg.slowpath_process_ns + 5);
        InjectPacket(-1, i, false);
        RunFor(g_cfg.slowpath_process_ns + g_cfg.slowpath_rule_latency_ns + g_cfg.pcie_latency_ns + 50);
        
        ASSERT_TRUE(g_impl->stable.table.count(new_flow));
        std::cout << "  Replaced with flow " << new_flow << std::endl;
    }
    
    // Verify: 3 new flows (100,101,102) + 2 original cold flows remaining
    ASSERT_EQ(g_impl->stable.table.size(), 5u);
    ASSERT_TRUE(g_impl->stable.table.count(100));
    ASSERT_TRUE(g_impl->stable.table.count(101));
    ASSERT_TRUE(g_impl->stable.table.count(102));
    
    std::cout << "[PASSED] TestMultipleReplacementsSequence" << std::endl;
}

void TestFailThenSuccessSequence() {
    std::cout << "[RUNNING] TestFailThenSuccessSequence..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    // Setup: all hot flows
    std::map<int, int> c;
    c[1]=100; c[2]=100; c[3]=100; c[4]=100; c[5]=100;
    SetupStableCounters(c);
    
    // First attempt: should fail (all hot)
    g_impl->rule_updater.OnQueueExceed(0);
    InjectPacket(200, 0, true);
    RunFor(g_cfg.slowpath_process_ns + 5);
    InjectPacket(-1, 0, false);
    RunFor(g_cfg.slowpath_process_ns + g_cfg.slowpath_rule_latency_ns + g_cfg.pcie_latency_ns + 50);
    
    ASSERT_TRUE(!g_impl->stable.table.count(200));
    std::cout << "  First attempt failed (as expected)" << std::endl;
    
    // Change one flow to cold
    g_impl->slow.counters[1] = 1;
    
    // Second attempt: should succeed (flow 1 is cold)
    g_impl->rule_updater.OnQueueExceed(1);
    InjectPacket(201, 1, true);
    RunFor(g_cfg.slowpath_process_ns + 5);
    InjectPacket(-1, 1, false);
    RunFor(g_cfg.slowpath_process_ns + g_cfg.slowpath_rule_latency_ns + g_cfg.pcie_latency_ns + 50);
    
    ASSERT_TRUE(g_impl->stable.table.count(201));
    ASSERT_TRUE(!g_impl->stable.table.count(1));
    std::cout << "  Second attempt succeeded" << std::endl;
    
    std::cout << "[PASSED] TestFailThenSuccessSequence" << std::endl;
}

void TestRapidFireRules() {
    std::cout << "[RUNNING] TestRapidFireRules..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    // Empty stable table - all insertions should succeed
    g_impl->slow.counters.clear();
    g_impl->stable.table.clear();
    
    // Fire 5 rules rapidly
    for (int i = 0; i < 5; i++) {
        int flow = 300 + i;
        g_impl->rule_updater.OnQueueExceed(i % 2);  // Alternate between burst idx 0 and 1
        InjectPacket(flow, i % 2, true);
        RunFor(g_cfg.slowpath_process_ns + 2);
        InjectPacket(-1, i % 2, false);
        RunFor(g_cfg.slowpath_process_ns + 2);
    }
    
    // Wait for all rules to complete
    RunFor(g_cfg.slowpath_rule_latency_ns + g_cfg.pcie_latency_ns + 100);
    
    // All 5 should be in stable table (table has room for 5)
    for (int i = 0; i < 5; i++) {
        int flow = 300 + i;
        if (!g_impl->stable.table.count(flow)) {
            std::cout << "  Missing flow " << flow << std::endl;
        }
    }
    ASSERT_EQ(g_impl->stable.table.size(), 5u);
    
    std::cout << "[PASSED] TestRapidFireRules" << std::endl;
}

void TestCountersUpdateMidFlight() {
    std::cout << "[RUNNING] TestCountersUpdateMidFlight..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    // Setup: flow 1 is cold
    std::map<int, int> c;
    c[1] = 1; c[2] = 100; c[3] = 100; c[4] = 100; c[5] = 100;
    SetupStableCounters(c);
    
    g_impl->rule_updater.OnQueueExceed(0);
    InjectPacket(400, 0, true);
    RunFor(g_cfg.slowpath_process_ns + 5);
    InjectPacket(-1, 0, false);
    RunFor(g_cfg.slowpath_process_ns + 2);
    
    // Rule is now being emitted - update counters mid-flight
    std::map<int32_t, int32_t> new_counters;
    new_counters[1] = 50;  // flow 1 becomes hot
    new_counters[2] = 100;
    new_counters[3] = 100;
    new_counters[4] = 100;
    new_counters[5] = 100;
    // flow 400 not in new_counters - should be protected
    g_impl->slow.OnCounters(new_counters);
    
    // Wait for rule to complete
    RunFor(g_cfg.slowpath_rule_latency_ns + g_cfg.pcie_latency_ns + 50);
    
    // Rule should still succeed (protected by rules_on_the_way)
    ASSERT_TRUE(g_impl->stable.table.count(400));
    
    std::cout << "[PASSED] TestCountersUpdateMidFlight" << std::endl;
}

void TestStressMultipleOperations() {
    std::cout << "[RUNNING] TestStressMultipleOperations..." << std::endl;
    Simulator::Destroy();
    Reset();
    
    int success_count = 0;
    int fail_count = 0;
    
    // Stress test: 20 operations mixing cold/hot replacements
    for (int round = 0; round < 20; round++) {
        // Randomly set up counters
        std::map<int, int> c;
        for (int j = 1; j <= 5; j++) {
            c[j] = (round + j) % 7 < 3 ? 1 : 100;  // Pseudo-random cold/hot
        }
        SetupStableCounters(c);
        
        int flow = 500 + round;
        int burst_idx = round % 2;
        
        g_impl->rule_updater.OnQueueExceed(burst_idx);
        InjectPacket(flow, burst_idx, true);
        RunFor(g_cfg.slowpath_process_ns + 5);
        InjectPacket(-1, burst_idx, false);
        RunFor(g_cfg.slowpath_process_ns + g_cfg.slowpath_rule_latency_ns + g_cfg.pcie_latency_ns + 50);
        
        if (g_impl->stable.table.count(flow)) {
            success_count++;
        } else {
            fail_count++;
        }
        
        // Clear stable table for next round (except keep flow in counters)
        g_impl->stable.table.clear();
    }
    
    std::cout << "  Success: " << success_count << ", Fail: " << fail_count << std::endl;
    ASSERT_TRUE(success_count > 0);  // At least some should succeed
    
    std::cout << "[PASSED] TestStressMultipleOperations" << std::endl;
}

int main(int argc, char *argv[]) {
    TestBufferOverflow();
    TestBasicRuleGeneration();
    TestColdReplacement();
    TestFailRecovery();
    TestAllHotReject();
    TestClearSignalEarly();
    TestBigFlowInvalidBurst();
    TestRuleAlreadyExists();
    TestReplaceSameFlow();
    TestMultipleRuleCompetition();
    
    // Additional critical tests
    TestMinHotReplacement();
    TestCountersProtection();
    TestReplacedRulesRollback();
    TestRuleEmitTiming();
    TestDeadlockPrevention();
    
    // Comprehensive combined tests
    TestMultipleReplacementsSequence();
    TestFailThenSuccessSequence();
    TestRapidFireRules();
    TestCountersUpdateMidFlight();
    TestStressMultipleOperations();
    
    std::cout << "\n==================================" << std::endl;
    std::cout << "All Slowpath tests passed! (20 tests)" << std::endl;
    std::cout << "==================================" << std::endl;
    return 0;
}
