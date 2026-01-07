// Define standalone test mode to exclude SC-dependent functions
#define AMT_STANDALONE_TEST

// test_extremes.cpp - Verify RollingDist and extreme detection logic
#include <iostream>
#include <cassert>
#include <cmath>
#include <vector>

// Use the proper mock
#include "test_sierrachart_mock.h"
#include "../AMT_Snapshots.h"
#include "../AMT_Helpers.h"
#include "../AMT_Patterns.h"

using namespace AMT;

void test_rolling_dist_basic() {
    std::cout << "=== Test: RollingDist Basic Operations ===" << std::endl;

    RollingDist rd;
    rd.reset(100);

    // Push some values
    for (int i = 0; i < 20; i++) {
        rd.push(10.0);
    }

    std::cout << "  Size after 20 pushes: " << rd.size() << std::endl;
    std::cout << "  Median: " << rd.median() << std::endl;
    std::cout << "  Mean: " << rd.mean() << std::endl;
    std::cout << "  MAD: " << rd.mad() << std::endl;

    assert(rd.size() == 20);
    assert(std::abs(rd.median() - 10.0) < 0.001);
    assert(std::abs(rd.mean() - 10.0) < 0.001);

    std::cout << "  PASSED" << std::endl;
}

void test_rolling_dist_mad() {
    std::cout << "=== Test: RollingDist MAD Calculation ===" << std::endl;

    RollingDist rd;
    rd.reset(100);

    // Push values with known distribution: 1,2,3,4,5,6,7,8,9,10
    for (int i = 1; i <= 10; i++) {
        rd.push(static_cast<double>(i));
    }

    double med = rd.median();  // Should be 5.5
    double m = rd.mad();

    std::cout << "  Values: 1-10" << std::endl;
    std::cout << "  Median: " << med << " (expected ~5.5)" << std::endl;
    std::cout << "  MAD: " << m << std::endl;

    // MAD for 1-10: deviations from 5.5 are 4.5, 3.5, 2.5, 1.5, 0.5, 0.5, 1.5, 2.5, 3.5, 4.5
    // Sorted: 0.5, 0.5, 1.5, 1.5, 2.5, 2.5, 3.5, 3.5, 4.5, 4.5
    // Median of deviations = (2.5 + 2.5) / 2 = 2.5
    std::cout << "  Expected MAD: ~2.5" << std::endl;

    assert(std::abs(med - 5.5) < 0.001);
    assert(std::abs(m - 2.5) < 0.001);

    std::cout << "  PASSED" << std::endl;
}

void test_is_extreme_requires_min_size() {
    std::cout << "=== Test: isExtreme Requires Min Size (10) ===" << std::endl;

    RollingDist rd;
    rd.reset(100);

    // With fewer than 10 values, isExtreme should always return false
    for (int i = 0; i < 9; i++) {
        rd.push(10.0);
    }

    std::cout << "  Size: " << rd.size() << std::endl;
    std::cout << "  isExtreme(1000.0) with 9 values: " << rd.isExtreme(1000.0) << std::endl;

    assert(rd.isExtreme(1000.0) == false);  // Should be false due to size < 10

    // Now add one more
    rd.push(10.0);
    std::cout << "  Size after 10th push: " << rd.size() << std::endl;
    std::cout << "  isExtreme(1000.0) with 10 values: " << rd.isExtreme(1000.0) << std::endl;

    // Now it should detect extreme... BUT all values are 10.0, so MAD is 0!
    std::cout << "  MAD with identical values: " << rd.mad() << std::endl;

    // MAD is 0, so isExtreme returns false (m < 1e-9 check)
    assert(rd.isExtreme(1000.0) == false);  // MAD is 0, so no extreme detection

    std::cout << "  PASSED (extreme detection disabled when MAD ~= 0)" << std::endl;
}

void test_is_extreme_with_variance() {
    std::cout << "=== Test: isExtreme With Variance ===" << std::endl;

    RollingDist rd;
    rd.reset(100);

    // Push values with some variance: alternating 8 and 12
    for (int i = 0; i < 20; i++) {
        rd.push((i % 2 == 0) ? 8.0 : 12.0);
    }

    double med = rd.median();
    double m = rd.mad();

    std::cout << "  Values: alternating 8 and 12" << std::endl;
    std::cout << "  Median: " << med << " (expected 10.0)" << std::endl;
    std::cout << "  MAD: " << m << " (expected 2.0)" << std::endl;

    // Extreme threshold = median ± k * MAD * 1.4826
    // With k=2.5: threshold = 10 ± 2.5 * 2.0 * 1.4826 = 10 ± 7.413
    // So values outside [2.587, 17.413] are extreme
    double threshold = 2.5 * m * 1.4826;
    std::cout << "  Extreme threshold: ±" << threshold << " from median" << std::endl;
    std::cout << "  Extreme range: [" << (med - threshold) << ", " << (med + threshold) << "]" << std::endl;

    std::cout << "  isExtreme(10.0): " << rd.isExtreme(10.0) << " (expected: false)" << std::endl;
    std::cout << "  isExtreme(15.0): " << rd.isExtreme(15.0) << " (expected: false)" << std::endl;
    std::cout << "  isExtreme(20.0): " << rd.isExtreme(20.0) << " (expected: true)" << std::endl;
    std::cout << "  isExtreme(0.0): " << rd.isExtreme(0.0) << " (expected: true)" << std::endl;

    assert(rd.isExtreme(10.0) == false);
    assert(rd.isExtreme(15.0) == false);
    assert(rd.isExtreme(20.0) == true);
    assert(rd.isExtreme(0.0) == true);

    std::cout << "  PASSED" << std::endl;
}

void test_baseline_engine_check_extremes() {
    std::cout << "=== Test: BaselineEngine.checkExtremes ===" << std::endl;

    BaselineEngine be;
    be.reset(300);

    // Populate baselines with realistic data
    for (int i = 0; i < 50; i++) {
        be.vol_sec.push(100.0 + (i % 10));       // 100-109
        be.delta_pct.push(-0.05 + (i % 10) * 0.01);  // -0.05 to 0.04
        be.trades_sec.push(20.0 + (i % 5));      // 20-24
        be.stack_rate.push(50.0 + (i % 20));     // 50-69
        be.pull_rate.push(10.0 + (i % 10));      // 10-19
        be.depth_mass_core.push(500.0 + (i % 50)); // 500-549
    }

    std::cout << "  Populated 50 values per baseline" << std::endl;
    std::cout << "  vol_sec: median=" << be.vol_sec.median() << " MAD=" << be.vol_sec.mad() << std::endl;
    std::cout << "  delta_pct: median=" << be.delta_pct.median() << " MAD=" << be.delta_pct.mad() << std::endl;
    std::cout << "  trades_sec: median=" << be.trades_sec.median() << " MAD=" << be.trades_sec.mad() << std::endl;
    std::cout << "  stack_rate: median=" << be.stack_rate.median() << " MAD=" << be.stack_rate.mad() << std::endl;
    std::cout << "  pull_rate: median=" << be.pull_rate.median() << " MAD=" << be.pull_rate.mad() << std::endl;
    std::cout << "  depth_mass_core: median=" << be.depth_mass_core.median() << " MAD=" << be.depth_mass_core.mad() << std::endl;

    // Test with normal values
    auto normalCheck = be.checkExtremes(105.0, 0.0, 22.0, 60.0, 15.0, 525.0);
    std::cout << "\n  Normal values check:" << std::endl;
    std::cout << "    anyExtreme: " << normalCheck.anyExtreme() << " (expected: false)" << std::endl;

    // Test with extreme values
    auto extremeCheck = be.checkExtremes(500.0, 0.5, 100.0, 200.0, 100.0, 2000.0);
    std::cout << "\n  Extreme values check:" << std::endl;
    std::cout << "    anyExtreme: " << extremeCheck.anyExtreme() << " (expected: true)" << std::endl;
    std::cout << "    extremeCount: " << extremeCheck.extremeCount() << std::endl;
    std::cout << "    volExtreme: " << extremeCheck.volExtreme << std::endl;
    std::cout << "    deltaExtreme: " << extremeCheck.deltaExtreme << std::endl;
    std::cout << "    tradesExtreme: " << extremeCheck.tradesExtreme << std::endl;
    std::cout << "    stackExtreme: " << extremeCheck.stackExtreme << std::endl;
    std::cout << "    pullExtreme: " << extremeCheck.pullExtreme << std::endl;
    std::cout << "    depthExtreme: " << extremeCheck.depthExtreme << std::endl;

    assert(normalCheck.anyExtreme() == false);
    assert(extremeCheck.anyExtreme() == true);
    assert(extremeCheck.extremeCount() >= 4);  // Most should be extreme

    std::cout << "  PASSED" << std::endl;
}

void test_pull_calculation_logic() {
    std::cout << "=== Test: Pull Calculation Logic ===" << std::endl;

    // Pull = -min(bidStackPull, 0) - min(askStackPull, 0)
    // This captures NEGATIVE stack values (liquidity being removed)

    auto calcPull = [](double bidStack, double askStack) {
        return -std::min(bidStack, 0.0) - std::min(askStack, 0.0);
    };

    // Case 1: Both positive (adding liquidity) -> Pull = 0
    double pull1 = calcPull(10.0, 20.0);
    std::cout << "  bidStack=10, askStack=20 -> Pull=" << pull1 << " (expected: 0)" << std::endl;
    assert(std::abs(pull1 - 0.0) < 0.001);

    // Case 2: Both negative (pulling liquidity)
    double pull2 = calcPull(-15.0, -25.0);
    std::cout << "  bidStack=-15, askStack=-25 -> Pull=" << pull2 << " (expected: 40)" << std::endl;
    assert(std::abs(pull2 - 40.0) < 0.001);

    // Case 3: Mixed - bid pulling, ask stacking
    double pull3 = calcPull(-10.0, 30.0);
    std::cout << "  bidStack=-10, askStack=30 -> Pull=" << pull3 << " (expected: 10)" << std::endl;
    assert(std::abs(pull3 - 10.0) < 0.001);

    // Case 4: All zeros
    double pull4 = calcPull(0.0, 0.0);
    std::cout << "  bidStack=0, askStack=0 -> Pull=" << pull4 << " (expected: 0)" << std::endl;
    assert(std::abs(pull4 - 0.0) < 0.001);

    std::cout << "\n  IMPORTANT: Pull=0 in session stats means:" << std::endl;
    std::cout << "    1. DOM stack values are always >= 0 (no pulls reported by study)" << std::endl;
    std::cout << "    2. OR baseline has MAD ~= 0 (all pull values identical)" << std::endl;
    std::cout << "    3. OR baseline has < 10 samples" << std::endl;

    std::cout << "  PASSED" << std::endl;
}

void test_mad_with_zeros() {
    std::cout << "=== Test: MAD When All Values Are Zero ===" << std::endl;

    RollingDist rd;
    rd.reset(100);

    // Push all zeros (simulating Pull when no negative stack values)
    for (int i = 0; i < 50; i++) {
        rd.push(0.0);
    }

    std::cout << "  50 values of 0.0 pushed" << std::endl;
    std::cout << "  Median: " << rd.median() << std::endl;
    std::cout << "  MAD: " << rd.mad() << std::endl;
    std::cout << "  isExtreme(100.0): " << rd.isExtreme(100.0) << std::endl;

    // MAD is 0 when all values are identical
    assert(std::abs(rd.mad()) < 1e-9);
    // isExtreme returns false when MAD < 1e-9
    assert(rd.isExtreme(100.0) == false);

    std::cout << "  CONCLUSION: Pull extremes won't be detected if all pull values are 0" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

void test_stack_vs_pull_separation() {
    std::cout << "=== Test: Stack vs Pull Are Separate Metrics ===" << std::endl;

    // Simulate what the study does
    struct SimSnapshot {
        double bidStackPull = 0.0;
        double askStackPull = 0.0;
    };

    std::vector<SimSnapshot> bars = {
        {10.0, 20.0},    // Both stacking
        {-5.0, 15.0},    // Bid pull, ask stack
        {30.0, -10.0},   // Bid stack, ask pull
        {-20.0, -30.0},  // Both pulling
        {0.0, 0.0},      // Flat
    };

    RollingDist stackBaseline, pullBaseline;
    stackBaseline.reset(100);
    pullBaseline.reset(100);

    std::cout << "  Processing 5 simulated bars:" << std::endl;
    for (size_t i = 0; i < bars.size(); i++) {
        const auto& snap = bars[i];
        double netStack = snap.bidStackPull + snap.askStackPull;
        double netPull = -std::min(snap.bidStackPull, 0.0) - std::min(snap.askStackPull, 0.0);

        stackBaseline.push(netStack);
        pullBaseline.push(netPull);

        std::cout << "    Bar " << i << ": bid=" << snap.bidStackPull
                  << " ask=" << snap.askStackPull
                  << " -> Stack=" << netStack << " Pull=" << netPull << std::endl;
    }

    std::cout << "\n  Stack baseline: median=" << stackBaseline.median()
              << " MAD=" << stackBaseline.mad() << std::endl;
    std::cout << "  Pull baseline: median=" << pullBaseline.median()
              << " MAD=" << pullBaseline.mad() << std::endl;

    std::cout << "  PASSED" << std::endl;
}

void test_calculate_facilitation() {
    std::cout << "=== Test: CalculateFacilitation Thresholds ===" << std::endl;

    // Default thresholds: highPctl=75, lowPctl=25, extremePctl=10
    const double HIGH = 75.0;
    const double LOW = 25.0;
    const double EXTREME = 10.0;

    // Test LABORED: high vol (>=75) + low range (<=25)
    auto labored = CalculateFacilitation(80.0, 20.0, HIGH, LOW, EXTREME);
    std::cout << "  LABORED (vol=80%, range=20%): " << to_string(labored) << std::endl;
    assert(labored == AuctionFacilitation::LABORED);

    // Test INEFFICIENT: low vol (<=25) + high range (>=75)
    auto inefficient = CalculateFacilitation(20.0, 80.0, HIGH, LOW, EXTREME);
    std::cout << "  INEFFICIENT (vol=20%, range=80%): " << to_string(inefficient) << std::endl;
    assert(inefficient == AuctionFacilitation::INEFFICIENT);

    // Test FAILED: very low vol (<=10) + very low range (<=10)
    auto failed = CalculateFacilitation(5.0, 5.0, HIGH, LOW, EXTREME);
    std::cout << "  FAILED (vol=5%, range=5%): " << to_string(failed) << std::endl;
    assert(failed == AuctionFacilitation::FAILED);

    // Test EFFICIENT: normal conditions (fallthrough)
    auto efficient1 = CalculateFacilitation(50.0, 50.0, HIGH, LOW, EXTREME);
    std::cout << "  EFFICIENT (vol=50%, range=50%): " << to_string(efficient1) << std::endl;
    assert(efficient1 == AuctionFacilitation::EFFICIENT);

    // Edge case: vol=75 (exactly at threshold) with low range -> LABORED
    auto edge1 = CalculateFacilitation(75.0, 25.0, HIGH, LOW, EXTREME);
    std::cout << "  Edge LABORED (vol=75%, range=25%): " << to_string(edge1) << std::endl;
    assert(edge1 == AuctionFacilitation::LABORED);

    // Edge case: vol=25 (exactly at threshold) with high range -> INEFFICIENT
    auto edge2 = CalculateFacilitation(25.0, 75.0, HIGH, LOW, EXTREME);
    std::cout << "  Edge INEFFICIENT (vol=25%, range=75%): " << to_string(edge2) << std::endl;
    assert(edge2 == AuctionFacilitation::INEFFICIENT);

    // Edge case: vol=10, range=10 (exactly at extreme threshold) -> FAILED
    auto edge3 = CalculateFacilitation(10.0, 10.0, HIGH, LOW, EXTREME);
    std::cout << "  Edge FAILED (vol=10%, range=10%): " << to_string(edge3) << std::endl;
    assert(edge3 == AuctionFacilitation::FAILED);

    // Precedence: LABORED checked before FAILED (vol=8, range=8 but doesn't match LABORED first)
    auto prec1 = CalculateFacilitation(8.0, 8.0, HIGH, LOW, EXTREME);
    std::cout << "  Precedence (vol=8%, range=8%): " << to_string(prec1) << " (should be FAILED)" << std::endl;
    assert(prec1 == AuctionFacilitation::FAILED);

    // Low vol, low range but range > extreme -> EFFICIENT (not FAILED)
    auto notFailed = CalculateFacilitation(8.0, 25.0, HIGH, LOW, EXTREME);
    std::cout << "  Not FAILED (vol=8%, range=25%): " << to_string(notFailed) << " (should be EFFICIENT)" << std::endl;
    assert(notFailed == AuctionFacilitation::EFFICIENT);

    std::cout << "  PASSED" << std::endl;
}

void diagnose_real_scenario() {
    std::cout << "\n=== DIAGNOSIS: Why Pull=0 in Session Stats ===" << std::endl;

    std::cout << R"(
  Looking at the log: "Extremes: Vol=715 Delta=11 Trades=448 Stack=577 Pull=0 Depth=231"

  Pull=0 means NO extreme pull events were detected. Possible causes:

  1. DOM study outputs Stack/Pull as NET values (always positive)
     - If bidStackPull and askStackPull from the DOM study are >= 0
     - Then curPull = -min(bid,0) - min(ask,0) = 0 always
     - Baseline would have all 0s -> MAD = 0 -> no extremes detected

  2. DOM data not available on historical bars
     - Code at line 820-831 only pushes to baselines when hasRealDomData
     - Historical bars have no DOM -> no baseline built
     - First live bars may not have enough samples (need 10+)

  3. Stack baseline already captures the variability
     - Stack = bidStackPull + askStackPull (net flow)
     - Pull = sum of negative components only
     - If the DOM study reports NET changes (not signed), Pull is meaningless

  RECOMMENDATION: Check what the DOM study actually outputs for Stack/Pull values.
  If they're always >= 0, the Pull metric is broken by design.
)" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  AMT Extremes Detection Test Suite" << std::endl;
    std::cout << "========================================\n" << std::endl;

    test_rolling_dist_basic();
    std::cout << std::endl;

    test_rolling_dist_mad();
    std::cout << std::endl;

    test_is_extreme_requires_min_size();
    std::cout << std::endl;

    test_is_extreme_with_variance();
    std::cout << std::endl;

    test_baseline_engine_check_extremes();
    std::cout << std::endl;

    test_pull_calculation_logic();
    std::cout << std::endl;

    test_mad_with_zeros();
    std::cout << std::endl;

    test_stack_vs_pull_separation();
    std::cout << std::endl;

    test_calculate_facilitation();
    std::cout << std::endl;

    diagnose_real_scenario();

    std::cout << "\n========================================" << std::endl;
    std::cout << "  ALL TESTS PASSED" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
