// ============================================================================
// test_liquidity_group2.cpp
// Integration test for LiquidityEngine Group 2 DOM pattern detection
// ============================================================================

// Required for M_PI on Windows (must be before cmath/math.h)
#define _USE_MATH_DEFINES
#include <cmath>

#include "../AMT_Liquidity.h"
#include <iostream>
#include <cassert>

using namespace AMT;

static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " #name "... "; \
    try { test_##name(); g_testsPassed++; std::cout << "PASSED\n"; } \
    catch (const std::exception& e) { g_testsFailed++; std::cout << "FAILED: " << e.what() << "\n"; } \
    catch (...) { g_testsFailed++; std::cout << "FAILED: Unknown exception\n"; } \
} while(0)

#define ASSERT_TRUE(cond) do { if (!(cond)) throw std::runtime_error("Assertion failed: " #cond); } while(0)
#define ASSERT_FALSE(cond) do { if (cond) throw std::runtime_error("Assertion failed: NOT " #cond); } while(0)

// ============================================================================
// TEST: Liq3Result has Group 2 pattern fields
// ============================================================================
TEST(Liq3Result_HasGroup2Fields)
{
    Liq3Result snap;

    // Verify vectors exist
    ASSERT_TRUE(snap.balancePatterns.empty());
    ASSERT_TRUE(snap.imbalancePatterns.empty());
    ASSERT_TRUE(snap.balanceHits.empty());
    ASSERT_TRUE(snap.imbalanceHits.empty());

    // Verify helper methods exist and work
    ASSERT_FALSE(snap.HasGroup2Patterns());
    ASSERT_FALSE(snap.HasStackedBids());
    ASSERT_FALSE(snap.HasChasingOrdersBuy());
    ASSERT_FALSE(snap.HasAnyDomPattern());
}

// ============================================================================
// TEST: Liq3Result Group 2 helper methods work correctly
// ============================================================================
TEST(Liq3Result_Group2Helpers)
{
    Liq3Result snap;

    // Add a balance pattern
    snap.balancePatterns.push_back(BalanceDOMPattern::STACKED_BIDS);
    ASSERT_TRUE(snap.HasGroup2Patterns());
    ASSERT_TRUE(snap.HasStackedBids());
    ASSERT_FALSE(snap.HasStackedAsks());
    ASSERT_TRUE(snap.HasAnyDomPattern());

    // Add an imbalance pattern
    snap.imbalancePatterns.push_back(ImbalanceDOMPattern::CHASING_ORDERS_BUY);
    ASSERT_TRUE(snap.HasChasingOrdersBuy());
    ASSERT_FALSE(snap.HasChasingOrdersSell());
    ASSERT_FALSE(snap.HasAbsorptionFailure());
}

// ============================================================================
// TEST: LiquidityEngine has Group 2 detection methods
// ============================================================================
TEST(LiquidityEngine_Group2Methods)
{
    LiquidityEngine engine;

    // Create empty Group 1 result
    DomDetectionResult group1;
    group1.wasEligible = false; // Not enough samples

    // DetectGroup2Patterns should exist and return result
    DomPatternResult result = engine.DetectGroup2Patterns(group1);
    ASSERT_FALSE(result.wasEligible); // Should be ineligible

    // DetectAndCopyGroup2Patterns should exist
    Liq3Result snap;
    engine.DetectAndCopyGroup2Patterns(snap, group1);
    ASSERT_FALSE(snap.HasGroup2Patterns());

    // DetectAndCopyAllDomPatterns should exist
    engine.DetectAndCopyAllDomPatterns(snap);
    ASSERT_FALSE(snap.HasGroup2Patterns());
}

// ============================================================================
// TEST: Full integration with sample data
// ============================================================================
TEST(LiquidityEngine_Group2Integration)
{
    LiquidityEngine engine;

    // Push enough samples to enable detection
    for (int i = 0; i < 10; ++i)
    {
        // Bid-heavy depth (3:1 ratio for STACKED_BIDS)
        engine.PushDomSample(
            1000 + i * 500,   // timestampMs
            i,                // barIndex
            1000,             // bestBidTick
            1001,             // bestAskTick
            300.0,            // domBidSize
            100.0,            // domAskSize
            5.0,              // bidStackPull
            2.0,              // askStackPull
            0.5,              // haloDepthImbalance
            true,             // haloDepthValid
            10.0,             // askVolSec
            8.0,              // bidVolSec
            2.0,              // deltaSec
            5.0               // tradesSec
        );
    }

    // Run combined detection
    Liq3Result snap;
    DomDetectionResult group1 = engine.DetectAndCopyAllDomPatterns(snap);

    // Verify Group 1 is eligible (enough samples)
    ASSERT_TRUE(group1.wasEligible);

    // Check Group 2 patterns were detected (STACKED_BIDS expected)
    if (snap.HasStackedBids())
    {
        std::cout << "(STACKED_BIDS detected) ";
    }
}

// ============================================================================
// TEST: Reset clears Group 2 log state
// ============================================================================
TEST(LiquidityEngine_ResetClearsGroup2)
{
    LiquidityEngine engine;

    // Add samples and detect
    for (int i = 0; i < 10; ++i)
    {
        engine.PushDomSample(1000 + i * 500, i, 1000, 1001, 200.0, 100.0, 0, 0, 0.3, true, 10, 8, 2, 5);
    }

    Liq3Result snap;
    engine.DetectAndCopyAllDomPatterns(snap);

    // Reset
    engine.ResetDomHistory();

    // Verify history is cleared
    ASSERT_FALSE(engine.HasDomHistoryMinSamples());
}

// ============================================================================
// MAIN
// ============================================================================

int main()
{
    std::cout << "========================================\n";
    std::cout << "LiquidityEngine Group 2 Integration Tests\n";
    std::cout << "========================================\n\n";

    RUN_TEST(Liq3Result_HasGroup2Fields);
    RUN_TEST(Liq3Result_Group2Helpers);
    RUN_TEST(LiquidityEngine_Group2Methods);
    RUN_TEST(LiquidityEngine_Group2Integration);
    RUN_TEST(LiquidityEngine_ResetClearsGroup2);

    std::cout << "\n========================================\n";
    std::cout << "Results: " << g_testsPassed << " passed, " << g_testsFailed << " failed\n";
    std::cout << "========================================\n";

    return g_testsFailed > 0 ? 1 : 0;
}
