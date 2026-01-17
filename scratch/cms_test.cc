// -----------------------------------------------------------------------------
// CountMinSketch Unit Test
// -----------------------------------------------------------------------------

#include "ns3/test.h"
#include "ns3/log.h"
#include "ns3/abort.h"

#include <vector>
#include <iostream>
#include <algorithm>
#include <random>
#include <limits>

// --- Access Internal Members ---
#define private public
#define protected public

// Patch NS_LOG_COMPONENT_DEFINE to avoid double registration
#include "ns3/log.h"
#undef NS_LOG_COMPONENT_DEFINE
#define NS_LOG_COMPONENT_DEFINE(name) static ns3::LogComponent g_log = ns3::LogComponent(name "_CMSTest")

// Include source file directly to access internal types
#define OOO_SYSTEM_TEST_INCLUSION
#include "src/point-to-point/model/ooo-system.cc"

using namespace ns3;

// Aliases
using TestCMS = CountMinSketch;

#define ASSERT_EQ(a,b) if((a)!=(b)) { std::cout << "Assertion failed at " << __LINE__ << ": " << #a << " (" << (a) << ") != " << #b << " (" << (b) << ")" << std::endl; exit(1); }
#define ASSERT_TRUE(a) if(!(a)) { std::cout << "Assertion failed at " << __LINE__ << ": " << #a << " is false." << std::endl; exit(1); }
#define ASSERT_GE(a,b) if((a)<(b)) { std::cout << "Assertion failed at " << __LINE__ << ": " << #a << " (" << (a) << ") < " << #b << " (" << (b) << ")" << std::endl; exit(1); }

// -----------------------------------------------------------------------------
// Test Cases
// -----------------------------------------------------------------------------

void TestBasicUpdateQuery() {
    std::cout << "[RUNNING] TestBasicUpdateQuery..." << std::endl;
    
    // Width 100, Depth 4
    TestCMS cms(4, 100);
    
    // Count should be 0 initially
    ASSERT_EQ(cms.UpdateAndQuery(12345, 0), 0u);
    
    // Add 1
    ASSERT_EQ(cms.UpdateAndQuery(12345, 1), 1u);
    
    // Add 10 more
    ASSERT_EQ(cms.UpdateAndQuery(12345, 10), 11u);
    
    // Another key should be distinct (mostly)
    ASSERT_EQ(cms.UpdateAndQuery(67890, 5), 5u);
    
    // Verify 12345 still 11
    ASSERT_EQ(cms.UpdateAndQuery(12345, 0), 11u);
    
    std::cout << "[PASSED] TestBasicUpdateQuery" << std::endl;
}

void TestReset() {
    std::cout << "[RUNNING] TestReset..." << std::endl;
    
    TestCMS cms(4, 100);
    cms.UpdateAndQuery(1, 100);
    cms.UpdateAndQuery(2, 200);
    
    ASSERT_EQ(cms.UpdateAndQuery(1, 0), 100u);
    
    cms.Reset();
    
    ASSERT_EQ(cms.UpdateAndQuery(1, 0), 0u);
    ASSERT_EQ(cms.UpdateAndQuery(2, 0), 0u);
    
    std::cout << "[PASSED] TestReset" << std::endl;
}

void TestSaturation() {
    std::cout << "[RUNNING] TestSaturation..." << std::endl;
    
    TestCMS cms(2, 10);
    uint64_t max = UINT64_MAX;
    
    // Add nearly max
    cms.UpdateAndQuery(555, max - 10);
    ASSERT_EQ(cms.UpdateAndQuery(555, 0), max - 10);
    
    // Add 5 -> max - 5
    cms.UpdateAndQuery(555, 5);
    ASSERT_EQ(cms.UpdateAndQuery(555, 0), max - 5);
    
    // Add 10 -> saturates at max (no overflow wrap around)
    cms.UpdateAndQuery(555, 10);
    ASSERT_EQ(cms.UpdateAndQuery(555, 0), max);
    
    // Add more -> stays max
    cms.UpdateAndQuery(555, 100);
    ASSERT_EQ(cms.UpdateAndQuery(555, 0), max);
    
    std::cout << "[PASSED] TestSaturation" << std::endl;
}

void TestSeedConsistency() {
    std::cout << "[RUNNING] TestSeedConsistency..." << std::endl;
    
    // Two CMS with same explicit seed
    TestCMS cms1(4, 100, 9999);
    TestCMS cms2(4, 100, 9999);
    
    // Third with different seed
    TestCMS cms3(4, 100, 1111);
    
    uint64_t key = 0xDEADBEEF;
    
    // Fill with some data
    cms1.UpdateAndQuery(key, 50);
    cms2.UpdateAndQuery(key, 50);
    cms3.UpdateAndQuery(key, 50);
    
    // CMS1 and CMS2 should have identical internal tables
    ASSERT_TRUE(cms1.seeds_ == cms2.seeds_);
    ASSERT_TRUE(cms1.table_ == cms2.table_);
    
    // CMS3 should be different (seeds likely different, so table indices different)
    // Note: Use 'likely' since small width could collide, but here we check internal seeds too.
    ASSERT_TRUE(cms1.seeds_ != cms3.seeds_);
    
    std::cout << "[PASSED] TestSeedConsistency" << std::endl;
}

void TestAccuracy() {
    std::cout << "[RUNNING] TestAccuracy..." << std::endl;
    
    // Standard CMS guarantee: Point query estimate >= True count
    // Error probability depends on width/depth.
    
    TestCMS cms(5, 1000); // Reasonable size
    
    std::map<uint64_t, uint64_t> ground_truth;
    
    // Inject 1000 items
    for(int i=0; i<1000; ++i) {
        uint64_t key = i % 100; // Recur keys
        cms.UpdateAndQuery(key, 1);
        ground_truth[key]++;
    }
    
    // Verify
    for(auto const& [k, v] : ground_truth) {
        uint64_t est = cms.UpdateAndQuery(k, 0);
        ASSERT_GE(est, v); // Overestimation allowed, underestimation NOT allowed
    }
    
    std::cout << "[PASSED] TestAccuracy" << std::endl;
}

void TestHeavyHitter() {
    std::cout << "[RUNNING] TestHeavyHitter..." << std::endl;
    
    // Detect heavy hitter amidst noise
    TestCMS cms(4, 200);
    
    uint64_t heavy = 777;
    cms.UpdateAndQuery(heavy, 1000);
    
    // Noise
    for(int i=0; i<500; ++i) {
        cms.UpdateAndQuery(i, 1);
    }
    
    uint64_t est = cms.UpdateAndQuery(heavy, 0);
    ASSERT_GE(est, 1000u);
    // Ideally shouldn't be too huge if dimensions are ok
    // With w=200, 500 noise items -> roughly 2.5 per bucket per row expected collision.
    // So 1000 + small noise
    if (est > 1200) {
        std::cout << "  Warning: Heavy Hitter estimate " << est << " seems high (expected ~1000)" << std::endl;
    }
    
    std::cout << "[PASSED] TestHeavyHitter" << std::endl;
}

int main(int argc, char *argv[]) {
    TestBasicUpdateQuery();
    TestReset();
    TestSaturation();
    TestSeedConsistency();
    TestAccuracy();
    TestHeavyHitter();
    
    std::cout << "\n=======================================" << std::endl;
    std::cout << "All CountMinSketch tests passed!" << std::endl;
    std::cout << "=======================================" << std::endl;
    
    return 0;
}
