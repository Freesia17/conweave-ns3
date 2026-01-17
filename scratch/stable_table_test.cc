#include <iostream>
#include <vector>
#include <unordered_map>
#include <cassert>
#include <algorithm>
#include <map>
#include <functional>
#include <random>
#include <deque>
#include <memory>
#include <set>

// Include ns-3 core headers
#include "ns3/ptr.h"
#include "ns3/packet.h"
#include "ns3/callback.h"
#include "ns3/custom-header.h"
#include "ns3/assert.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

// Wrapper namespace
namespace test_context {

#define private public
#define protected public

using namespace ::ns3;
using namespace std;

#ifdef OOO_SYSTEM_H
#undef OOO_SYSTEM_H
#endif

// Avoid LogComponent collision
#undef NS_LOG_COMPONENT_DEFINE
#define NS_LOG_COMPONENT_DEFINE(name) \
    static ::ns3::LogComponent g_log = ::ns3::LogComponent("MockStable_" name)

#define OOO_SYSTEM_TEST_INCLUSION
#include "src/point-to-point/model/ooo-system.cc"

#undef private
#undef protected

} // namespace test_context

// Aliases
using namespace ::ns3; // For global Packet, Ptr, etc.
using TestImpl = test_context::ns3::OooSystemAdapter::Impl;
using TestStableTable = test_context::ns3::OooSystemAdapter::Impl::StableTableState;
using TestConfig = test_context::ns3::OooSystemAdapter::Config;
using TestUpdateType = test_context::ns3::UpdateType;
using TestPacketCtx = test_context::ns3::PacketCtx;

// =========================================================================
// TESTING FRAMEWORK
// =========================================================================

#define ASSERT_EQ(val1, val2) \
    do { \
        if ((val1) != (val2)) { \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__ \
                      << ". Expected " << (val2) << ", got " << (val1) << std::endl; \
            std::exit(1); \
        } \
    } while (0)

#define ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__ \
                      << ". Condition is false." << std::endl; \
            std::exit(1); \
        } \
    } while (0)

#define TEST(name) \
    void name(); \
    static void (*name##_ptr)() = name; \
    void name()

// =========================================================================
// TEST CASES
// =========================================================================

TEST(TestInitialization) {
    std::cout << "[RUNNING] TestInitialization..." << std::endl;
    TestConfig cfg;
    TestImpl impl(cfg); // Impl creates StableTableState
    
    // Check initial state via impl.stable
    ASSERT_EQ(impl.stable.table.size(), 0);
    ASSERT_EQ(impl.stable.counters.size(), 0);
    
    std::cout << "[PASSED] TestInitialization" << std::endl;
}

TEST(TestInsert) {
    std::cout << "[RUNNING] TestInsert..." << std::endl;
    TestConfig cfg;
    cfg.stable_table_volume = 10;
    TestImpl impl(cfg);
    
    // Insert new entry
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 1001, -1);
    
    ASSERT_EQ(impl.stable.table.size(), 1);
    ASSERT_TRUE(impl.stable.table.find(1001) != impl.stable.table.end());
    ASSERT_EQ(impl.stable.counters[1001], 0);

    // Insert another
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 1002, -1);
    ASSERT_EQ(impl.stable.table.size(), 2);
    ASSERT_EQ(impl.stable.counters[1002], 0);
    
    std::cout << "[PASSED] TestInsert" << std::endl;
}

TEST(TestDelete) {
    std::cout << "[RUNNING] TestDelete..." << std::endl;
    TestConfig cfg;
    TestImpl impl(cfg);
    
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 1001, -1);
    ASSERT_EQ(impl.stable.table.size(), 1);
    
    impl.stable.ApplyUpdate(TestUpdateType::DELETE, 1001, -1);
    ASSERT_EQ(impl.stable.table.size(), 0);
    ASSERT_TRUE(impl.stable.table.find(1001) == impl.stable.table.end());
    ASSERT_TRUE(impl.stable.counters.find(1001) == impl.stable.counters.end());
    
    std::cout << "[PASSED] TestDelete" << std::endl;
}

TEST(TestReplace) {
    std::cout << "[RUNNING] TestReplace..." << std::endl;
    TestConfig cfg;
    TestImpl impl(cfg);
    
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 1001, -1);
    ASSERT_EQ(impl.stable.table.size(), 1);
    
    // Replace 1001 with 1002
    impl.stable.ApplyUpdate(TestUpdateType::REPLACE, 1002, 1001);
    
    ASSERT_EQ(impl.stable.table.size(), 1);
    ASSERT_TRUE(impl.stable.table.find(1001) == impl.stable.table.end());
    ASSERT_TRUE(impl.stable.table.find(1002) != impl.stable.table.end());
    ASSERT_TRUE(impl.stable.counters.find(1002) != impl.stable.counters.end());
    
    std::cout << "[PASSED] TestReplace" << std::endl;
}

TEST(TestFullTableBehavior) {
    std::cout << "[RUNNING] TestFullTableBehavior..." << std::endl;
    TestConfig cfg;
    cfg.stable_table_volume = 2;
    TestImpl impl(cfg);
    
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 10, -1);
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 20, -1);
    ASSERT_EQ(impl.stable.table.size(), 2);
    
    // Attempt Insert when full -> Should do nothing (check ApplyUpdate implementation)
    // implementation: if (table.size() < owner->cfg.stable_table_volume) { insert... }
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 30, -1);
    
    ASSERT_EQ(impl.stable.table.size(), 2);
    ASSERT_TRUE(impl.stable.table.find(30) == impl.stable.table.end());
    
    // Replace should work: remove one, insert another
    // REPLACE 20 with 30
    impl.stable.ApplyUpdate(TestUpdateType::REPLACE, 30, 20);
    ASSERT_EQ(impl.stable.table.size(), 2);
    ASSERT_TRUE(impl.stable.table.find(20) == impl.stable.table.end());
    ASSERT_TRUE(impl.stable.table.find(30) != impl.stable.table.end());
    
    std::cout << "[PASSED] TestFullTableBehavior" << std::endl;
}

TEST(TestFlushCounters) {
    std::cout << "[RUNNING] TestFlushCounters..." << std::endl;
    TestConfig cfg;
    TestImpl impl(cfg);
    
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 100, -1);
    impl.stable.counters[100] = 50; // Simulate counts
    
    std::map<int32_t, int32_t> snapshot = impl.stable.FlushCounters();
    
    ASSERT_EQ(snapshot.size(), 1);
    ASSERT_EQ(snapshot[100], 50);
    
    // Verify counters reset to 0
    ASSERT_EQ(impl.stable.counters[100], 0);
    // Verify table entry remains
    ASSERT_TRUE(impl.stable.table.find(100) != impl.stable.table.end());
    
    std::cout << "[PASSED] TestFlushCounters" << std::endl;
}

TEST(TestOnPacketHit) {
    std::cout << "[RUNNING] TestOnPacketHit..." << std::endl;
    TestConfig cfg;
    TestImpl impl(cfg);
    
    // Create Dummy Packet
    TestPacketCtx ctx;
    ctx.flow_id = 99;
    ctx.pkt = Create<Packet>(100);
    
    // Without rule: Miss
    // We can't easily verify Miss logic (it calls flow.OnPacket), but we verify counter NOT chaning.
    impl.stable.OnPacket(ctx);
    ASSERT_TRUE(impl.stable.counters.find(99) == impl.stable.counters.end());
    
    // Insert Rule
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 99, -1);
    ASSERT_EQ(impl.stable.counters[99], 0);
    
    // Hit
    impl.stable.OnPacket(ctx);
    ASSERT_EQ(impl.stable.counters[99], 1);
    
    impl.stable.OnPacket(ctx);
    ASSERT_EQ(impl.stable.counters[99], 2);
    
    std::cout << "[PASSED] TestOnPacketHit" << std::endl;
}

TEST(TestOnPacketClearSignal) {
    std::cout << "[RUNNING] TestOnPacketClearSignal..." << std::endl;
    TestConfig cfg;
    TestImpl impl(cfg);
    
    TestPacketCtx ctx;
    ctx.flow_id = -1;
    ctx.burst_table_index = 5;
    
    // Should pass to FlowTracker, NOT touch counters or table
    impl.stable.OnPacket(ctx);
    
    ASSERT_EQ(impl.stable.table.size(), 0);
    
    std::cout << "[PASSED] TestOnPacketClearSignal" << std::endl;
}

TEST(TestInsertExisting) {
    std::cout << "[RUNNING] TestInsertExisting..." << std::endl;
    TestConfig cfg;
    TestImpl impl(cfg);
    
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 100, -1);
    impl.stable.counters[100] = 50; // Simulate counts
    
    // Insert existing flow -> should reset counters
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 100, -1);
    
    ASSERT_EQ(impl.stable.table.size(), 1);
    ASSERT_EQ(impl.stable.counters[100], 0); // Reset to 0
    
    std::cout << "[PASSED] TestInsertExisting" << std::endl;
}

TEST(TestDeleteNonExistent) {
    std::cout << "[RUNNING] TestDeleteNonExistent..." << std::endl;
    TestConfig cfg;
    TestImpl impl(cfg);
    
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 100, -1);
    
    // Delete non-existent
    impl.stable.ApplyUpdate(TestUpdateType::DELETE, 200, -1);
    
    ASSERT_EQ(impl.stable.table.size(), 1);
    ASSERT_TRUE(impl.stable.table.find(100) != impl.stable.table.end());
    
    std::cout << "[PASSED] TestDeleteNonExistent" << std::endl;
}

TEST(TestReplaceNonExistent) {
    std::cout << "[RUNNING] TestReplaceNonExistent..." << std::endl;
    TestConfig cfg;
    cfg.stable_table_volume = 10;
    TestImpl impl(cfg);
    
    // Replace old=200 (not exists) with new=100
    impl.stable.ApplyUpdate(TestUpdateType::REPLACE, 100, 200);
    
    ASSERT_EQ(impl.stable.table.size(), 1);
    ASSERT_TRUE(impl.stable.table.find(100) != impl.stable.table.end());
    ASSERT_EQ(impl.stable.counters[100], 0);
    
    std::cout << "[PASSED] TestReplaceNonExistent" << std::endl;
}

TEST(TestReplaceFull) {
    std::cout << "[RUNNING] TestReplaceFull..." << std::endl;
    TestConfig cfg;
    cfg.stable_table_volume = 1; // capacity 1
    TestImpl impl(cfg);
    
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 10, -1);
    ASSERT_EQ(impl.stable.table.size(), 1);
    
    // Table full. Try Insert 20 -> Fail
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 20, -1);
    ASSERT_EQ(impl.stable.table.size(), 1);
    ASSERT_TRUE(impl.stable.table.find(10) != impl.stable.table.end());
    
    // Try Replace 10 with 20 -> Should succeed
    // Logic: Erase(10) first (size becomes 0), then Insert(20) (size becomes 1)
    impl.stable.ApplyUpdate(TestUpdateType::REPLACE, 20, 10);
    
    ASSERT_EQ(impl.stable.table.size(), 1);
    ASSERT_TRUE(impl.stable.table.find(20) != impl.stable.table.end());
    
    std::cout << "[PASSED] TestReplaceFull" << std::endl;
}

TEST(TestFlushCountersComplex) {
    std::cout << "[RUNNING] TestFlushCountersComplex..." << std::endl;
    TestConfig cfg;
    TestImpl impl(cfg);
    
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 10, -1);
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 20, -1);
    
    impl.stable.counters[10] = 5;
    impl.stable.counters[20] = 10;
    // Simulate garbage in counters that is NOT in table (should be ignored/dropped?)
    // Actually FlushCounters copies `counters` map. If map has extra entries, they are copied.
    // However, implementation says:
    // counters.clear(); for(auto flow : table) counters[flow] = 0;
    // So garbage is cleared out.
    impl.stable.counters[99] = 100; 
    
    std::map<int32_t, int32_t> snapshot = impl.stable.FlushCounters();
    
    // Snapshot should have 10, 20, AND 99 because it copies current counters map.
    ASSERT_EQ(snapshot.count(10), 1);
    ASSERT_EQ(snapshot.count(20), 1);
    ASSERT_EQ(snapshot.count(99), 1);
    ASSERT_EQ(snapshot[10], 5);
    ASSERT_EQ(snapshot[20], 10);
    ASSERT_EQ(snapshot[99], 100);
    
    // But AFTER flush, counters is rebuilt from table only.
    ASSERT_EQ(impl.stable.counters.size(), 2);
    ASSERT_EQ(impl.stable.counters.count(10), 1);
    ASSERT_EQ(impl.stable.counters.count(20), 1);
    ASSERT_EQ(impl.stable.counters.count(99), 0); // Garbage gone
    ASSERT_EQ(impl.stable.counters[10], 0);
    
    std::cout << "[PASSED] TestFlushCountersComplex" << std::endl;
}

TEST(TestReplaceSameEntryResetsCounter) {
    std::cout << "[RUNNING] TestReplaceSameEntryResetsCounter..." << std::endl;
    TestConfig cfg;
    cfg.stable_table_volume = 10;
    TestImpl impl(cfg);

    // Insert and set non-zero counter
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 100, -1);
    impl.stable.counters[100] = 7;

    // Replace with itself should reset counter to 0 (erase then insert)
    impl.stable.ApplyUpdate(TestUpdateType::REPLACE, 100, 100);

    ASSERT_EQ(impl.stable.table.size(), 1);
    ASSERT_TRUE(impl.stable.table.find(100) != impl.stable.table.end());
    ASSERT_EQ(impl.stable.counters[100], 0);

    std::cout << "[PASSED] TestReplaceSameEntryResetsCounter" << std::endl;
}

TEST(TestInsertExistingWhenFull) {
    std::cout << "[RUNNING] TestInsertExistingWhenFull..." << std::endl;
    TestConfig cfg;
    cfg.stable_table_volume = 1; // capacity 1
    TestImpl impl(cfg);

    // Fill table
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 10, -1);
    impl.stable.counters[10] = 5;

    // Insert same entry when full should NOT reset counters (no actual insert)
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 10, -1);

    ASSERT_EQ(impl.stable.table.size(), 1);
    ASSERT_TRUE(impl.stable.table.find(10) != impl.stable.table.end());
    ASSERT_EQ(impl.stable.counters[10], 5);

    std::cout << "[PASSED] TestInsertExistingWhenFull" << std::endl;
}

TEST(TestFlushCountersEmptyTable) {
    std::cout << "[RUNNING] TestFlushCountersEmptyTable..." << std::endl;
    TestConfig cfg;
    TestImpl impl(cfg);

    // Counters contain garbage while table is empty
    impl.stable.counters[999] = 42;

    auto snapshot = impl.stable.FlushCounters();

    ASSERT_EQ(snapshot.size(), 1);
    ASSERT_EQ(snapshot[999], 42);
    // After flush, counters should be rebuilt from empty table -> empty
    ASSERT_EQ(impl.stable.counters.size(), 0);

    std::cout << "[PASSED] TestFlushCountersEmptyTable" << std::endl;
}

TEST(TestOnPacketClearSignalCountersNoChange) {
    std::cout << "[RUNNING] TestOnPacketClearSignalCountersNoChange..." << std::endl;
    TestConfig cfg;
    TestImpl impl(cfg);

    // Prepare some counters
    impl.stable.ApplyUpdate(TestUpdateType::INSERT, 10, -1);
    impl.stable.counters[10] = 3;

    TestPacketCtx ctx;
    ctx.flow_id = -1;
    ctx.burst_table_index = 5;
    ctx.pkt = Create<Packet>(10);

    // Set slowpath to busy so we don't process immediately
    Simulator::Destroy();
    impl.slow.start_scheduled = true;
    impl.stable.OnPacket(ctx);
    Simulator::Stop(NanoSeconds(200));
    Simulator::Run();
    Simulator::Destroy();

    // Counters and table should remain unchanged by clear signal at StableTable level
    ASSERT_EQ(impl.stable.table.size(), 1);
    ASSERT_EQ(impl.stable.counters[10], 3);

    std::cout << "[PASSED] TestOnPacketClearSignalCountersNoChange" << std::endl;
}

void DebugEvent() {
    std::cout << "[DEBUG] Simulator Event Fired at 50ns" << std::endl;
}

TEST(TestClearSignalForwarding) {
    std::cout << "[RUNNING] TestClearSignalForwarding..." << std::endl;
    TestConfig cfg;
    cfg.pcie_latency_ns = 100;
    
    // Need a simulator for this one
    Simulator::Destroy();
    TestImpl impl(cfg); 
    
    // Clear signal: flow_id -1, burst_index >= 0
    TestPacketCtx ctx;
    ctx.flow_id = -1;
    ctx.burst_table_index = 5;
    ctx.pkt = Create<Packet>(10);
    ctx.packet_id = 123;
    
    // Initial: Slowpath buffer empty
    ASSERT_EQ(impl.slow.buffer.size(), 0);
    
    // Hack: Force Slowpath to think it is busy/scheduled.
    // This prevents OnPacket -> ScheduleStart -> ProcessOne loop which would drain the buffer immediately.
    // We want to verify packet arrived in buffer.
    impl.slow.start_scheduled = true;

    impl.stable.OnPacket(ctx);
    
    Simulator::Schedule(NanoSeconds(50), &DebugEvent);

    // Run simulator past pcie_latency (100ns)
    Simulator::Stop(NanoSeconds(200)); 
    std::cout << "[DEBUG] Starting Simulator::Run" << std::endl;
    Simulator::Run(); 
    std::cout << "[DEBUG] Finished Simulator::Run" << std::endl;
    
    // Now verification
    std::cout << "[DEBUG] Slowpath buffer size: " << impl.slow.buffer.size() << std::endl;
    ASSERT_EQ(impl.slow.buffer.size(), 1);
    ASSERT_EQ(impl.slow.buffer.front().first.packet_id, 123);
    
    Simulator::Destroy(); // Clean up
    
    std::cout << "[PASSED] TestClearSignalForwarding" << std::endl;
}

int main() {
    std::cout << "Starting StableTable Standalone Tests..." << std::endl;
    
    // Ensure simulator environment is clean although we mainly test logic
    Simulator::Destroy(); 

    TestInitialization();
    TestInsert();
    TestDelete();
    TestReplace();
    TestFullTableBehavior();
    TestFlushCounters();
    TestOnPacketHit();
    TestOnPacketClearSignal();
    // New cases
    TestInsertExisting();
    TestDeleteNonExistent();
    TestReplaceNonExistent();
    TestReplaceFull();
    TestFlushCountersComplex();
    TestClearSignalForwarding();
    TestReplaceSameEntryResetsCounter();
    TestInsertExistingWhenFull();
    TestFlushCountersEmptyTable();
    TestOnPacketClearSignalCountersNoChange();

    std::cout << "All StableTable tests passed!" << std::endl;
    return 0;
}
