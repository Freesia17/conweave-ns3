// -----------------------------------------------------------------------------
// RuleAllocation Unit Test
// -----------------------------------------------------------------------------

#include "ns3/test.h"
#include "ns3/log.h"
#include "ns3/abort.h"

#include <vector>
#include <iostream>
#include <algorithm>
#include <random>
#include <set>

// --- Access Internal Members ---
#define private public
#define protected public

// Patch NS_LOG_COMPONENT_DEFINE to avoid double registration
#include "ns3/log.h"
#undef NS_LOG_COMPONENT_DEFINE
#define NS_LOG_COMPONENT_DEFINE(name) static ns3::LogComponent g_log = ns3::LogComponent(name "_RuleAllocTest")

// Include source file directly to access internal types
#define OOO_SYSTEM_TEST_INCLUSION
#include "src/point-to-point/model/ooo-system.cc"

using namespace ns3;

// Aliases
using TestRuleAllocation = RuleAllocation;

#define ASSERT_EQ(a,b) if((a)!=(b)) { std::cout << "Assertion failed at " << __LINE__ << ": " << #a << " (" << (a) << ") != " << #b << " (" << (b) << ")" << std::endl; exit(1); }
#define ASSERT_TRUE(a) if(!(a)) { std::cout << "Assertion failed at " << __LINE__ << ": " << #a << " is false." << std::endl; exit(1); }

// -----------------------------------------------------------------------------
// Test Cases
// -----------------------------------------------------------------------------

void TestBasicAllocation() {
    std::cout << "[RUNNING] TestBasicAllocation..." << std::endl;
    
    // Create allocator with size 10
    TestRuleAllocation alloc(10);
    ASSERT_EQ(alloc.Size(), 10u);
    
    // Allocate first
    int idx1 = alloc.Allocate();
    ASSERT_EQ(idx1, 0);
    
    // Allocate second
    int idx2 = alloc.Allocate();
    ASSERT_EQ(idx2, 1);
    
    std::cout << "[PASSED] TestBasicAllocation" << std::endl;
}

void TestFullAllocation() {
    std::cout << "[RUNNING] TestFullAllocation..." << std::endl;
    
    size_t size = 5;
    TestRuleAllocation alloc(size);
    
    // Fill it up
    for (size_t i = 0; i < size; i++) {
        int idx = alloc.Allocate();
        ASSERT_EQ(idx, (int)i);
    }
    
    // Try one more
    int idx = alloc.Allocate();
    ASSERT_EQ(idx, -1);
    
    std::cout << "[PASSED] TestFullAllocation" << std::endl;
}

void TestReleaseAndReuse() {
    std::cout << "[RUNNING] TestReleaseAndReuse..." << std::endl;
    
    TestRuleAllocation alloc(5);
    
    // Allocated: 0, 1, 2
    alloc.Allocate();
    alloc.Allocate();
    int idx2 = alloc.Allocate();
    ASSERT_EQ(idx2, 2);
    
    // Release 1
    alloc.Release(1);
    
    // Next allocation should reuse 1 (implementation iterates from 0)
    int idxNew = alloc.Allocate();
    ASSERT_EQ(idxNew, 1);
    
    // Next should be 3
    int idx3 = alloc.Allocate();
    ASSERT_EQ(idx3, 3);
    
    std::cout << "[PASSED] TestReleaseAndReuse" << std::endl;
}

void TestInvalidRelease() {
    std::cout << "[RUNNING] TestInvalidRelease..." << std::endl;
    
    TestRuleAllocation alloc(5);
    int idx0 = alloc.Allocate();
    
    // Release out of bounds (negative)
    alloc.Release(-1);
    ASSERT_EQ(alloc.slots_[0], true); // Should still be allocated
    
    // Release out of bounds (too large)
    alloc.Release(100);
    ASSERT_EQ(alloc.slots_[0], true);
    
    // Correct release
    alloc.Release(idx0);
    ASSERT_EQ(alloc.slots_[0], false);
    
    std::cout << "[PASSED] TestInvalidRelease" << std::endl;
}

void TestDoubleRelease() {
    std::cout << "[RUNNING] TestDoubleRelease..." << std::endl;
    
    TestRuleAllocation alloc(5);
    int idx = alloc.Allocate();
    
    alloc.Release(idx);
    ASSERT_EQ(alloc.slots_[0], false);
    
    // Release again - should be harmless / idempotent for boolean map
    alloc.Release(idx);
    ASSERT_EQ(alloc.slots_[0], false);
    
    // Re-allocate to confirm it's usable
    int newIdx = alloc.Allocate();
    ASSERT_EQ(newIdx, 0);
    
    std::cout << "[PASSED] TestDoubleRelease" << std::endl;
}

void TestReset() {
    std::cout << "[RUNNING] TestReset..." << std::endl;
    
    TestRuleAllocation alloc(5);
    // Fill all
    for(int i=0; i<5; ++i) alloc.Allocate();
    ASSERT_EQ(alloc.Allocate(), -1);
    
    // Reset
    alloc.Reset(5);
    
    // Should be empty
    ASSERT_EQ(alloc.Allocate(), 0);
    
    std::cout << "[PASSED] TestReset" << std::endl;
}

void TestStressCombined() {
    std::cout << "[RUNNING] TestStressCombined..." << std::endl;
    
    int size = 100;
    TestRuleAllocation alloc(size);
    std::vector<int> active_allocations;
    std::vector<bool> shadow_slots(size, false);
    
    // Random operations
    for (int i = 0; i < 1000; i++) {
        int op = rand() % 2; // 0=Allocate, 1=Release
        
        if (op == 0) { // Allocate
            int idx = alloc.Allocate();
            if (idx != -1) {
                ASSERT_EQ(shadow_slots[idx], false);
                shadow_slots[idx] = true;
                active_allocations.push_back(idx);
            } else {
                // Determine if really full
                bool full = true;
                for(bool b : shadow_slots) if(!b) full = false;
                ASSERT_TRUE(full);
            }
        } else { // Release
            if (!active_allocations.empty()) {
                // Pick random index to release
                int rand_vec_idx = rand() % active_allocations.size();
                int idx_to_release = active_allocations[rand_vec_idx];
                
                // Swap remove
                active_allocations[rand_vec_idx] = active_allocations.back();
                active_allocations.pop_back();
                
                alloc.Release(idx_to_release);
                shadow_slots[idx_to_release] = false;
            }
        }
    }
    
    // Verify final state consistency
    for (int i = 0; i < size; i++) {
        ASSERT_EQ(alloc.slots_[i], shadow_slots[i]);
    }
    
    std::cout << "[PASSED] TestStressCombined" << std::endl;
}

int main(int argc, char *argv[]) {
    TestBasicAllocation();
    TestFullAllocation();
    TestReleaseAndReuse();
    TestInvalidRelease();
    TestDoubleRelease();
    TestReset();
    TestStressCombined();
    
    std::cout << "\n=======================================" << std::endl;
    std::cout << "All RuleAllocation tests passed!" << std::endl;
    std::cout << "=======================================" << std::endl;
    
    return 0;
}
