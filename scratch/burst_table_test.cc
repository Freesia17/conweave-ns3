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

// Include ns-3 core headers that might be needed by ooo-system.cc
// to ensure they are defined in the global namespace, not our wrapper.
#include "ns3/ptr.h"
#include "ns3/packet.h"
#include "ns3/callback.h"
#include "ns3/custom-header.h"
#include "ns3/assert.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

// Define a test context to wrap the source code inclusions.
// This allows us to compile the source file again without linking errors (mostly),
// and allows us to access private members via preprocessor ticks.
namespace test_context {

// 1. Force private/protected members to be public for testing
#define private public
#define protected public

// Import global ns3 symbols so they are visible inside test_context
// This allows usages of Simulator, Ptr, Packet etc. to resolve to ::ns3::*
using namespace ::ns3;
using namespace std; // Safety measure for std types if needed unqualified

namespace ns3 {
    using LogComponent = ::ns3::LogComponent;
    using Time = ::ns3::Time; // Just in case
}

// 2. We need to ensure ooo-system.h is re-processed inside this namespace
// so that OooSystemAdapter is defined within test_context::ns3.
// If it was already included by the global headers above, we need to undefine the guard.
#ifdef OOO_SYSTEM_H
#undef OOO_SYSTEM_H
#endif

// 3. Avoid LogComponent name collision with the library
#undef NS_LOG_COMPONENT_DEFINE
#define NS_LOG_COMPONENT_DEFINE(name) \
    static ::ns3::LogComponent g_log = ::ns3::LogComponent("Mock" name)

// 4. Include the HEADER first (conceptually) inside the namespace.
// Actually, ooo-system.cc includes ooo-system.h.
// But we need to make sure the relative path works.
// We are in scratch/ (or top level when running waf).
// The compiler includes -I. -I.. 
// We will include the .cc file directly.

#define OOO_SYSTEM_TEST_INCLUSION
#include "src/point-to-point/model/ooo-system.cc"

// Clean up macros
#undef private
#undef protected

} // namespace test_context

// Shorten the long type name for convenience
using TestBurstTable = test_context::ns3::OooSystemAdapter::Impl::BurstTableState;
using TestImpl = test_context::ns3::OooSystemAdapter::Impl;
using TestConfig = test_context::ns3::OooSystemAdapter::Config;

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

// Helper: Consistency Check
void CheckConsistency(const TestBurstTable& table) {
    int flow_count = 0;
    // 1. Check index_to_flow -> flow_to_index consistency
    for (size_t i = 0; i < table.index_to_flow.size(); ++i) {
        int32_t flow_id = table.index_to_flow[i];
        if (flow_id >= 0) {
            flow_count++;
            auto it = table.flow_to_index.find(flow_id);
            if (it == table.flow_to_index.end()) {
                 std::cerr << "Consistency Error: Flow " << flow_id << " at index " << i 
                           << " not found in flow_to_index map." << std::endl;
                 std::exit(1);
            }
            if (it->second != (int)i) {
                 std::cerr << "Consistency Error: Flow " << flow_id << " at index " << i 
                           << " maps to index " << it->second << " in map." << std::endl;
                 std::exit(1);
            }
        }
    }

    // 2. Check flow_to_index -> index_to_flow consistency
    if (table.flow_to_index.size() != (size_t)flow_count) {
         std::cerr << "Consistency Error: Map size (" << table.flow_to_index.size() 
                   << ") does not match valid entries in vector (" << flow_count << ")." << std::endl;
         std::exit(1);
    }
    
    for (auto const& [flow_id, index] : table.flow_to_index) {
        if (index < 0 || (size_t)index >= table.index_to_flow.size()) {
             std::cerr << "Consistency Error: Map contains invalid index " << index << std::endl;
             std::exit(1);
        }
        if (table.index_to_flow[index] != flow_id) {
             std::cerr << "Consistency Error: Flow " << flow_id << " maps to index " << index 
                       << ", but vector at " << index << " contains " << table.index_to_flow[index] << std::endl;
             std::exit(1);
        }
    }
}

// =========================================================================
// TEST CASES
// =========================================================================

TEST(TestResetBehavior) {
    std::cout << "[RUNNING] TestResetBehavior..." << std::endl;
    TestConfig cfg;
    cfg.burst_table_volume = 10;
    TestImpl impl(cfg);
    TestBurstTable table(&impl);
    table.Reset();
    
    // Fill some data
    table.Insert(1001, 1);
    table.Insert(1002, 5);
    ASSERT_EQ(table.flow_to_index.size(), 2);
    CheckConsistency(table);

    // Verify Reset clears everything
    table.Reset();
    ASSERT_EQ(table.flow_to_index.size(), 0);
    ASSERT_EQ(table.index_to_flow.size(), 10);
    for (int flow : table.index_to_flow) {
        ASSERT_EQ(flow, -1);
    }
    CheckConsistency(table);

    std::cout << "[PASSED] TestResetBehavior" << std::endl;
}

TEST(TestIdempotency) {
    std::cout << "[RUNNING] TestIdempotency..." << std::endl;
    TestConfig cfg;
    TestImpl impl(cfg);
    TestBurstTable table(&impl);
    table.Reset();

    table.Insert(1001, 2);
    size_t map_size_before = table.flow_to_index.size();
    
    // Re-insert same flow same index
    table.Insert(1001, 2);
    
    ASSERT_EQ(table.Lookup(1001), 2);
    ASSERT_EQ(table.flow_to_index.size(), map_size_before);
    CheckConsistency(table);
    
    std::cout << "[PASSED] TestIdempotency" << std::endl;
}

TEST(TestZeroCapacity) {
    std::cout << "[RUNNING] TestZeroCapacity..." << std::endl;
    TestConfig cfg;
    cfg.burst_table_volume = 0;
    TestImpl impl(cfg);
    TestBurstTable table(&impl);
    table.Reset(); // Should handle size 0 resize

    ASSERT_EQ(table.index_to_flow.size(), 0);
    
    // Insert should be safe no-op
    table.Insert(1001, 0); 
    ASSERT_EQ(table.flow_to_index.size(), 0);
    
    // Delete should be safe no-op
    table.DeleteByIndex(0);
    
    std::cout << "[PASSED] TestZeroCapacity" << std::endl;
}

TEST(TestInitialization) {
    std::cout << "[RUNNING] TestInitialization..." << std::endl;
    TestConfig cfg;
    cfg.burst_table_volume = 10;
    TestImpl impl(cfg);
    TestBurstTable table(&impl);
    
    table.Reset();
    
    ASSERT_EQ(table.index_to_flow.size(), 10);
    ASSERT_EQ(table.flow_to_index.size(), 0);
    
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(table.index_to_flow[i], -1);
    }
    std::cout << "[PASSED] TestInitialization" << std::endl;
}

TEST(TestBasicInsertAndLookup) {
    std::cout << "[RUNNING] TestBasicInsertAndLookup..." << std::endl;
    TestConfig cfg;
    cfg.burst_table_volume = 5;
    TestImpl impl(cfg);
    TestBurstTable table(&impl);
    table.Reset();

    table.Insert(1001, 2);
    ASSERT_EQ(table.Lookup(1001), 2);
    ASSERT_EQ(table.index_to_flow[2], 1001);
    ASSERT_EQ(table.flow_to_index.size(), 1);

    table.Insert(1002, 0);
    ASSERT_EQ(table.Lookup(1002), 0);
    ASSERT_EQ(table.index_to_flow[0], 1002);
    ASSERT_EQ(table.flow_to_index.size(), 2);
    std::cout << "[PASSED] TestBasicInsertAndLookup" << std::endl;
}

TEST(TestOverwriteIndex) {
    std::cout << "[RUNNING] TestOverwriteIndex..." << std::endl;
    TestConfig cfg;
    cfg.burst_table_volume = 5;
    TestImpl impl(cfg);
    TestBurstTable table(&impl);
    table.Reset();

    // Insert Flow A at Index 2
    table.Insert(1001, 2);
    ASSERT_EQ(table.Lookup(1001), 2);

    // Insert Flow B at Index 2 (Overwrite)
    table.Insert(1002, 2);
    
    // Check Flow A is gone
    ASSERT_EQ(table.Lookup(1001), -1);
    // Check Flow B is at Index 2
    ASSERT_EQ(table.Lookup(1002), 2);
    ASSERT_EQ(table.index_to_flow[2], 1002);
    ASSERT_EQ(table.flow_to_index.size(), 1);
    std::cout << "[PASSED] TestOverwriteIndex" << std::endl;
}

TEST(TestUpdateFlowPosition) {
    std::cout << "[RUNNING] TestUpdateFlowPosition..." << std::endl;
    TestConfig cfg;
    cfg.burst_table_volume = 5;
    TestImpl impl(cfg);
    TestBurstTable table(&impl);
    table.Reset();

    // Insert Flow A at Index 2
    table.Insert(1001, 2);
    
    // Move Flow A from Index 2 to Index 4
    table.Insert(1001, 4);

    // Old index should be cleared
    ASSERT_EQ(table.index_to_flow[2], -1);
    // New index should be set
    ASSERT_EQ(table.index_to_flow[4], 1001);
    ASSERT_EQ(table.Lookup(1001), 4);
    ASSERT_EQ(table.flow_to_index.size(), 1);
    std::cout << "[PASSED] TestUpdateFlowPosition" << std::endl;
}

TEST(TestOverwriteWithMove) {
    std::cout << "[RUNNING] TestOverwriteWithMove..." << std::endl;
    TestConfig cfg;
    cfg.burst_table_volume = 5;
    TestImpl impl(cfg);
    TestBurstTable table(&impl);
    table.Reset();

    // Flow A at 1
    table.Insert(1001, 1);
    // Flow B at 2
    table.Insert(1002, 2);

    // Move Flow A to 2 (Overwrite B)
    table.Insert(1001, 2);

    // Flow B should be gone
    ASSERT_EQ(table.Lookup(1002), -1);
    // Flow A should be at 2
    ASSERT_EQ(table.Lookup(1001), 2);
    // Index 1 should be clear
    ASSERT_EQ(table.index_to_flow[1], -1);
    ASSERT_EQ(table.flow_to_index.size(), 1);
    std::cout << "[PASSED] TestOverwriteWithMove" << std::endl;
}

TEST(TestFullTableBehavior) {
    std::cout << "[RUNNING] TestFullTableBehavior..." << std::endl;
    TestConfig cfg;
    cfg.burst_table_volume = 2; // Small volume
    TestImpl impl(cfg);
    TestBurstTable table(&impl);
    table.Reset();

    table.Insert(10, 0);
    table.Insert(11, 1);
    
    ASSERT_EQ(table.flow_to_index.size(), 2);

    // Try to insert new Flow C at Index 0 (Collision/Swap logic)
    // The Insert logic checks `flow_to_index.size() >= index_to_flow.size()` FIRST.
    // map size (2) >= vector size (2), AND flow 12 is new. 
    // It should RETURN EARLY (reject insert).
    table.Insert(12, 0);

    // Verify Flow C was NOT inserted
    ASSERT_EQ(table.Lookup(12), -1);
    // Verify Flow A is still there
    ASSERT_EQ(table.Lookup(10), 0);
    
    // NOW: Test 1. Existing flow migration when full
    // Flow 10 is at 0, Flow 11 is at 1. Table is FULL (size 2).
    // We want to move Flow 10 to index 1 (Occupied by 11).
    // Logic:
    // 1. Check full? Yes.
    // 2. Is flow already present? Yes (Flow 10). -> Proceed.
    // 3. Clear old pos (0).
    // 4. Clear target pos (1) -> wipes Flow 11.
    // 5. Update maps.
    
    table.Insert(10, 1);
    
    CheckConsistency(table);

    // Flow 10 should be at 1
    ASSERT_EQ(table.Lookup(10), 1);
    ASSERT_EQ(table.index_to_flow[1], 10);
    
    // Flow 11 should be gone (overwritten)
    ASSERT_EQ(table.Lookup(11), -1);
    
    // Index 0 should be empty (old position cleared)
    ASSERT_EQ(table.index_to_flow[0], -1);

    // Verify size decreased (2 -> 1) because we overwrote one and vacated another
    ASSERT_EQ(table.flow_to_index.size(), 1);

    std::cout << "[PASSED] TestFullTableBehavior" << std::endl;
}

TEST(TestRepeatedDeleteNoop) {
    std::cout << "[RUNNING] TestRepeatedDeleteNoop..." << std::endl;
    TestConfig cfg;
    TestImpl impl(cfg);
    TestBurstTable table(&impl);
    table.Reset();
    
    table.Insert(10, 1);
    ASSERT_EQ(table.Lookup(10), 1);
    
    table.DeleteByIndex(1);
    ASSERT_EQ(table.Lookup(10), -1);
    
    // Repeat delete
    table.DeleteByIndex(1);
    // Delete unrelated
    table.DeleteByIndex(2);
    
    CheckConsistency(table);
    std::cout << "[PASSED] TestRepeatedDeleteNoop" << std::endl;
}

TEST(TestMoveExistingOutOfBounds) {
    std::cout << "[RUNNING] TestMoveExistingOutOfBounds..." << std::endl;
    TestConfig cfg;
    cfg.burst_table_volume = 5;
    TestImpl impl(cfg);
    TestBurstTable table(&impl);
    table.Reset();
    
    table.Insert(10, 0);
    
    // Try move to out of bounds
    table.Insert(10, 100);
    
    // Should remain at 0
    ASSERT_EQ(table.Lookup(10), 0);
    ASSERT_EQ(table.index_to_flow[0], 10);
    
    CheckConsistency(table);
    std::cout << "[PASSED] TestMoveExistingOutOfBounds" << std::endl;
}

TEST(TestComplexSequenceConsistency) {
    std::cout << "[RUNNING] TestComplexSequenceConsistency..." << std::endl;
    TestConfig cfg;
    cfg.burst_table_volume = 3;
    TestImpl impl(cfg);
    TestBurstTable table(&impl);
    table.Reset();
    
    // 1. Fill
    table.Insert(1, 0);
    table.Insert(2, 1);
    table.Insert(3, 2);
    CheckConsistency(table);
    
    // 2. Rotate/Swap like ops
    // Move 1 to 1 (overwrite 2) -> 1 at 1, 0 is empty, 2 is gone, 3 at 2
    table.Insert(1, 1);
    CheckConsistency(table);
    ASSERT_EQ(table.Lookup(1), 1);
    ASSERT_EQ(table.Lookup(2), -1);
    ASSERT_EQ(table.index_to_flow[0], -1);
    
    // 3. Insert new to vacated 0
    table.Insert(4, 0);
    CheckConsistency(table);
    ASSERT_EQ(table.Lookup(4), 0);
    
    // 4. Delete middle
    table.DeleteByIndex(1);
    CheckConsistency(table);
    ASSERT_EQ(table.Lookup(1), -1);
    
    std::cout << "[PASSED] TestComplexSequenceConsistency" << std::endl;
}

TEST(TestDeleteByIndex) {
    std::cout << "[RUNNING] TestDeleteByIndex..." << std::endl;
    TestConfig cfg;
    cfg.burst_table_volume = 5;
    TestImpl impl(cfg);
    TestBurstTable table(&impl);
    table.Reset();

    table.Insert(100, 3);
    ASSERT_EQ(table.Lookup(100), 3);

    table.DeleteByIndex(3);
    ASSERT_EQ(table.Lookup(100), -1);
    ASSERT_EQ(table.index_to_flow[3], -1);
    ASSERT_EQ(table.flow_to_index.size(), 0);

    // Delete empty index (should define no-op)
    table.DeleteByIndex(0);
    // Delete out of bounds
    table.DeleteByIndex(100);
    table.DeleteByIndex(-1);

    std::cout << "[PASSED] TestDeleteByIndex" << std::endl;
}

TEST(TestOutOfBoundsInsert) {
    std::cout << "[RUNNING] TestOutOfBoundsInsert..." << std::endl;
    TestConfig cfg;
    cfg.burst_table_volume = 3;
    TestImpl impl(cfg);
    TestBurstTable table(&impl);
    table.Reset();

    table.Insert(1, -1);
    ASSERT_EQ(table.Lookup(1), -1);

    table.Insert(1, 3); // Size is 3, valid indices 0,1,2
    ASSERT_EQ(table.Lookup(1), -1);
    
    std::cout << "[PASSED] TestOutOfBoundsInsert" << std::endl;
}


int main() {
    std::cout << "Starting BurstTable Standalone Tests (Testing Actual Source)..." << std::endl;
    TestInitialization();
    TestResetBehavior();
    TestBasicInsertAndLookup();
    TestIdempotency();
    TestOverwriteIndex();
    TestUpdateFlowPosition();
    TestOverwriteWithMove();
    TestFullTableBehavior();
    TestDeleteByIndex();
    TestOutOfBoundsInsert();
    TestZeroCapacity();
    // Additional edge and sequence tests
    TestRepeatedDeleteNoop();
    TestMoveExistingOutOfBounds();
    TestComplexSequenceConsistency();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
