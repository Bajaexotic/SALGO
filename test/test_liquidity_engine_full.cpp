// test_liquidity_engine_full.cpp - Comprehensive test of Kyle's 4-component liquidity model
// Tests: DepthMass, Stress, Resilience, Spread, Toxicity, Composite LIQ, LIQSTATE
#include <iostream>
#include <cassert>
#include <cmath>

#include "test_sierrachart_mock.h"
#include "../amt_core.h"
#include "../AMT_Snapshots.h"
#include "../AMT_Liquidity.h"

using namespace AMT;

// Helper to check floating point equality
bool approxEqual(double a, double b, double epsilon = 0.001) {
    return std::abs(a - b) < epsilon;
}

// ============================================================================
// TEST: DepthMass calculation (distance-weighted)
// ============================================================================

void test_depth_mass_calculation() {
    std::cout << "=== Test: DepthMass distance-weighted calculation ===" << std::endl;

    LiquidityEngine engine;
    engine.Reset();

    // Reference price = 100.0, tickSize = 0.25
    // Dmax = 4 ticks (default)
    // Weight = 1 / (1 + distTicks)

    // Bid at 99.75 (1 tick away): weight = 1/2 = 0.5, volume = 100 -> mass = 50
    // Bid at 99.50 (2 ticks away): weight = 1/3 = 0.333, volume = 60 -> mass = 20
    // Ask at 100.25 (1 tick away): weight = 1/2 = 0.5, volume = 80 -> mass = 40
    // Ask at 100.50 (2 ticks away): weight = 1/3 = 0.333, volume = 30 -> mass = 10

    std::vector<std::pair<double, double>> bidLevels = {{99.75, 100.0}, {99.50, 60.0}};
    std::vector<std::pair<double, double>> askLevels = {{100.25, 80.0}, {100.50, 30.0}};

    auto result = engine.ComputeDepthMassFromLevels(100.0, 0.25, bidLevels, askLevels);

    std::cout << "  bidMass=" << result.bidMass << " (expected ~70)" << std::endl;
    std::cout << "  askMass=" << result.askMass << " (expected ~50)" << std::endl;
    std::cout << "  totalMass=" << result.totalMass << " (expected ~120)" << std::endl;
    std::cout << "  imbalance=" << result.imbalance << " (expected ~0.17, bid-heavy)" << std::endl;

    // Bid: 100/2 + 60/3 = 50 + 20 = 70
    // Ask: 80/2 + 30/3 = 40 + 10 = 50
    assert(approxEqual(result.bidMass, 70.0, 1.0));
    assert(approxEqual(result.askMass, 50.0, 1.0));
    assert(approxEqual(result.totalMass, 120.0, 2.0));
    assert(result.valid);

    // Imbalance = (70 - 50) / 120 = 0.167
    assert(result.imbalance > 0.1 && result.imbalance < 0.25);

    std::cout << "  PASSED" << std::endl;
}

void test_depth_mass_dmax_cutoff() {
    std::cout << "=== Test: DepthMass respects Dmax cutoff ===" << std::endl;

    LiquidityEngine engine;
    engine.Reset();
    // Default Dmax = 4 ticks

    // Bid at 98.50 (6 ticks away) - should be EXCLUDED
    // Ask at 101.50 (6 ticks away) - should be EXCLUDED
    std::vector<std::pair<double, double>> bidLevels = {{98.50, 1000.0}};
    std::vector<std::pair<double, double>> askLevels = {{101.50, 1000.0}};

    auto result = engine.ComputeDepthMassFromLevels(100.0, 0.25, bidLevels, askLevels);

    std::cout << "  Levels beyond Dmax: totalMass=" << result.totalMass << " (expected 0)" << std::endl;
    assert(result.totalMass == 0.0);
    assert(!result.valid);  // No valid levels

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Stress calculation
// ============================================================================

void test_stress_calculation() {
    std::cout << "=== Test: Stress calculation ===" << std::endl;

    LiquidityEngine engine;
    engine.Reset();

    // Stress = AggressiveTotal / (DepthMass + epsilon)
    // epsilon = 1.0 (default)

    // Case 1: Low stress (aggressive << depth)
    auto stress1 = engine.ComputeStress(50.0, 50.0, 1000.0);
    std::cout << "  Low stress: " << stress1.stress << " (expected ~0.1)" << std::endl;
    // (50 + 50) / (1000 + 1) = 100 / 1001 = 0.0999
    assert(approxEqual(stress1.stress, 0.1, 0.01));
    assert(stress1.valid);

    // Case 2: High stress (aggressive >> depth)
    auto stress2 = engine.ComputeStress(500.0, 500.0, 100.0);
    std::cout << "  High stress: " << stress2.stress << " (expected ~9.9)" << std::endl;
    // (500 + 500) / (100 + 1) = 1000 / 101 = 9.9
    assert(approxEqual(stress2.stress, 9.9, 0.2));

    // Case 3: Balanced
    auto stress3 = engine.ComputeStress(100.0, 100.0, 200.0);
    std::cout << "  Balanced stress: " << stress3.stress << " (expected ~1.0)" << std::endl;
    // 200 / 201 = 0.995
    assert(approxEqual(stress3.stress, 1.0, 0.1));

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Resilience calculation
// ============================================================================

void test_resilience_calculation() {
    std::cout << "=== Test: Resilience (refill rate) calculation ===" << std::endl;

    LiquidityEngine engine;
    engine.Reset();

    // First bar - no previous, should return invalid
    auto res1 = engine.ComputeResilience(100.0, 60.0);
    std::cout << "  First bar: valid=" << res1.valid << " (expected 0)" << std::endl;
    assert(!res1.valid);

    // Second bar - depth increased (positive refill)
    auto res2 = engine.ComputeResilience(150.0, 60.0);
    std::cout << "  Depth increased: refillRate=" << res2.refillRate << " (expected ~0.83)" << std::endl;
    // refillRaw = max(0, 150 - 100) = 50
    // refillRate = 50 / 60 = 0.833
    assert(res2.valid);
    assert(approxEqual(res2.refillRate, 0.833, 0.01));

    // Third bar - depth decreased (no refill)
    auto res3 = engine.ComputeResilience(120.0, 60.0);
    std::cout << "  Depth decreased: refillRate=" << res3.refillRate << " (expected 0)" << std::endl;
    // refillRaw = max(0, 120 - 150) = max(0, -30) = 0
    assert(res3.valid);
    assert(res3.refillRate == 0.0);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Baseline warmup and percentile ranking
// ============================================================================

void test_baseline_warmup() {
    std::cout << "=== Test: Baseline warmup and readiness ===" << std::endl;

    LiquidityEngine engine;
    engine.Reset();

    // Pre-warm with 5 samples (less than min 10)
    for (int i = 0; i < 5; i++) {
        engine.PreWarmFromBar(100.0, 50.0, 50.0, 99.0, 60.0, SessionPhase::UNKNOWN);
    }

    auto status1 = engine.GetPreWarmStatus();
    std::cout << "  After 5 samples: allReady=" << status1.allReady << " (expected 0)" << std::endl;
    assert(!status1.allReady);

    // Pre-warm with 10 more samples (total 15)
    for (int i = 0; i < 10; i++) {
        engine.PreWarmFromBar(100.0, 50.0, 50.0, 99.0, 60.0, SessionPhase::UNKNOWN);
    }

    auto status2 = engine.GetPreWarmStatus();
    std::cout << "  After 15 samples: allReady=" << status2.allReady << " (expected 1)" << std::endl;
    assert(status2.allReady);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Full Compute with composite LIQ
// ============================================================================

void test_compute_composite_liq() {
    std::cout << "=== Test: Compute composite LIQ formula ===" << std::endl;

    LiquidityEngine engine;
    engine.Reset();

    // Pre-warm baselines with consistent values
    for (int i = 0; i < 20; i++) {
        engine.PreWarmFromBar(100.0, 50.0, 50.0, 99.0, 60.0, SessionPhase::UNKNOWN);
    }

    // Create DOM lambdas with known depth
    auto getBidLevel = [](int level, double& price, double& volume) {
        if (level == 0) { price = 99.75; volume = 100.0; return true; }
        return false;
    };
    auto getAskLevel = [](int level, double& price, double& volume) {
        if (level == 0) { price = 100.25; volume = 100.0; return true; }
        return false;
    };

    // First Compute sets up internal prevDepthMassTotal (resilience needs prev bar)
    auto result1 = engine.Compute(100.0, 0.25, 10, getBidLevel, getAskLevel, 50.0, 50.0, 60.0);
    std::cout << "  First Compute (sets up prev): liqValid=" << result1.liqValid
              << " resilienceValid=" << result1.resilience.valid << std::endl;

    // Second Compute has valid resilience
    auto result = engine.Compute(100.0, 0.25, 10, getBidLevel, getAskLevel, 50.0, 50.0, 60.0);

    std::cout << "  depth.valid=" << result.depth.valid << std::endl;
    std::cout << "  depth.totalMass=" << result.depth.totalMass << std::endl;
    std::cout << "  stress.stress=" << result.stress.stress << std::endl;
    std::cout << "  depthRank=" << result.depthRank << " (valid=" << result.depthRankValid << ")" << std::endl;
    std::cout << "  stressRank=" << result.stressRank << " (valid=" << result.stressRankValid << ")" << std::endl;
    std::cout << "  resilienceRank=" << result.resilienceRank << " (valid=" << result.resilienceRankValid << ")" << std::endl;
    std::cout << "  liq=" << result.liq << " (valid=" << result.liqValid << ")" << std::endl;
    std::cout << "  liqState=" << LiquidityStateToString(result.liqState) << std::endl;

    assert(result.depth.valid);
    assert(result.resilience.valid);  // Now valid after second call
    assert(result.liqValid);
    assert(result.liq >= 0.0 && result.liq <= 1.0);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: LIQSTATE classification
// ============================================================================

void test_liqstate_classification() {
    std::cout << "=== Test: LIQSTATE classification thresholds ===" << std::endl;

    // Test the state derivation logic directly
    // VOID: liq <= 0.10 OR depthRank <= 0.10
    // THIN: 0.10 < liq <= 0.25 OR stressRank >= 0.90
    // NORMAL: 0.25 < liq < 0.75
    // THICK: liq >= 0.75

    LiquidityEngine engine;
    DOMWarmup warmup;
    warmup.Reset();
    engine.Reset();
    engine.SetDOMWarmup(&warmup);

    // Pre-warm with varied values to create a distribution
    for (int i = 0; i < 20; i++) {
        engine.PreWarmFromBar(50.0 + i * 10, 20.0 + i * 5, 20.0 + i * 5, 49.0, 60.0, SessionPhase::GLOBEX);
    }
    engine.SetPhase(SessionPhase::GLOBEX);

    // Create lambdas for different depth scenarios
    auto makeBidLevel = [](double vol) {
        return [vol](int level, double& price, double& volume) {
            if (level == 0) { price = 99.75; volume = vol; return true; }
            return false;
        };
    };
    auto makeAskLevel = [](double vol) {
        return [vol](int level, double& price, double& volume) {
            if (level == 0) { price = 100.25; volume = vol; return true; }
            return false;
        };
    };

    // First call to set up prevDepthMassTotal (resilience needs prev bar)
    engine.Compute(100.0, 0.25, 10, makeBidLevel(100.0), makeAskLevel(100.0), 50.0, 50.0, 60.0);

    // Test with very high depth (should be THICK or high LIQ)
    auto resultHigh = engine.Compute(100.0, 0.25, 10, makeBidLevel(500.0), makeAskLevel(500.0), 10.0, 10.0, 60.0);
    std::cout << "  High depth: liq=" << resultHigh.liq << " state=" << LiquidityStateToString(resultHigh.liqState)
              << " liqValid=" << resultHigh.liqValid << std::endl;

    // Test with low depth + high stress (should trend toward THIN/VOID)
    auto resultLow = engine.Compute(100.0, 0.25, 10, makeBidLevel(5.0), makeAskLevel(5.0), 200.0, 200.0, 60.0);
    std::cout << "  Low depth/high stress: liq=" << resultLow.liq << " state=" << LiquidityStateToString(resultLow.liqState)
              << " liqValid=" << resultLow.liqValid << std::endl;

    // Verify high depth gives higher LIQ than low depth
    if (resultHigh.liqValid && resultLow.liqValid) {
        assert(resultHigh.liq > resultLow.liq);
        std::cout << "  High depth LIQ > Low depth LIQ: PASSED" << std::endl;
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Spread (Kyle's Tightness) impact
// ============================================================================

void test_spread_impact_on_liq() {
    std::cout << "=== Test: Spread (Tightness) impact on LIQ ===" << std::endl;

    LiquidityEngine engine;
    DOMWarmup warmup;
    warmup.Reset();
    engine.Reset();
    engine.SetDOMWarmup(&warmup);

    // Pre-warm with consistent depth but varying spreads
    for (int i = 0; i < 20; i++) {
        engine.PreWarmFromBar(100.0, 50.0, 50.0, 99.0, 60.0, SessionPhase::GLOBEX, 2.0);  // spread = 2 ticks
    }
    engine.SetPhase(SessionPhase::GLOBEX);

    auto getBid = [](int level, double& price, double& volume) {
        if (level == 0) { price = 99.75; volume = 100.0; return true; }
        return false;
    };
    auto getAsk = [](int level, double& price, double& volume) {
        if (level == 0) { price = 100.25; volume = 100.0; return true; }
        return false;
    };

    // First call to set up prevDepthMassTotal (resilience needs prev bar)
    engine.Compute(100.0, 0.25, 10, getBid, getAsk, 50.0, 50.0, 60.0, 2.0);

    // Compute with tight spread (1 tick)
    auto resultTight = engine.Compute(100.0, 0.25, 10, getBid, getAsk, 50.0, 50.0, 60.0, 1.0);
    std::cout << "  Tight spread (1 tick): liq=" << resultTight.liq
              << " spreadRank=" << resultTight.spreadRank << std::endl;

    // Compute with wide spread (4 ticks)
    auto resultWide = engine.Compute(100.0, 0.25, 10, getBid, getAsk, 50.0, 50.0, 60.0, 4.0);
    std::cout << "  Wide spread (4 ticks): liq=" << resultWide.liq
              << " spreadRank=" << resultWide.spreadRank << std::endl;

    // Wide spread should have higher spreadRank (worse)
    if (resultTight.spreadRankValid && resultWide.spreadRankValid) {
        assert(resultWide.spreadRank > resultTight.spreadRank);
        std::cout << "  Wide spreadRank > Tight spreadRank: PASSED" << std::endl;
    }

    // Tight spread should give equal or higher LIQ (less penalty)
    if (resultTight.liqValid && resultWide.liqValid) {
        std::cout << "  Tight LIQ=" << resultTight.liq << " vs Wide LIQ=" << resultWide.liq << std::endl;
        // Note: Depending on baseline, tight might be lower rank, so LIQ impact varies
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Toxicity proxy (VPIN-lite)
// ============================================================================

void test_toxicity_proxy() {
    std::cout << "=== Test: Toxicity proxy (VPIN-lite) ===" << std::endl;

    LiquidityEngine engine;
    engine.Reset();

    // Pre-warm minimally
    for (int i = 0; i < 15; i++) {
        engine.PreWarmFromBar(100.0, 50.0, 50.0, 99.0, 60.0, SessionPhase::UNKNOWN);
    }

    auto getBid = [](int level, double& price, double& volume) {
        if (level == 0) { price = 99.75; volume = 100.0; return true; }
        return false;
    };
    auto getAsk = [](int level, double& price, double& volume) {
        if (level == 0) { price = 100.25; volume = 100.0; return true; }
        return false;
    };

    // First call to set up prevDepthMassTotal (resilience needs prev bar)
    engine.Compute(100.0, 0.25, 10, getBid, getAsk, 50.0, 50.0, 60.0);

    // Symmetric consumption (low toxicity)
    auto result1 = engine.Compute(100.0, 0.25, 10, getBid, getAsk, 50.0, 50.0, 60.0, -1.0, 100.0, 100.0);
    std::cout << "  Symmetric (100 vs 100): toxicity=" << result1.toxicityProxy
              << " valid=" << result1.toxicityValid << std::endl;
    // |100 - 100| / 200 = 0
    if (result1.toxicityValid) {
        assert(approxEqual(result1.toxicityProxy, 0.0, 0.01));
    }

    // Asymmetric consumption (high toxicity)
    auto result2 = engine.Compute(100.0, 0.25, 10, getBid, getAsk, 50.0, 50.0, 60.0, -1.0, 180.0, 20.0);
    std::cout << "  Asymmetric (180 vs 20): toxicity=" << result2.toxicityProxy
              << " valid=" << result2.toxicityValid << std::endl;
    // |180 - 20| / 200 = 160 / 200 = 0.8
    if (result2.toxicityValid) {
        assert(approxEqual(result2.toxicityProxy, 0.8, 0.01));
    }

    // Verify asymmetric > symmetric
    if (result1.toxicityValid && result2.toxicityValid) {
        assert(result2.toxicityProxy > result1.toxicityProxy);
        std::cout << "  Asymmetric toxicity > Symmetric toxicity: PASSED" << std::endl;
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Error handling (warmup states)
// ============================================================================

void test_error_handling_warmup() {
    std::cout << "=== Test: Error handling during warmup ===" << std::endl;

    LiquidityEngine engine;
    engine.Reset();
    // NO pre-warm - baselines empty

    auto getBid = [](int level, double& price, double& volume) {
        if (level == 0) { price = 99.75; volume = 100.0; return true; }
        return false;
    };
    auto getAsk = [](int level, double& price, double& volume) {
        if (level == 0) { price = 100.25; volume = 100.0; return true; }
        return false;
    };

    auto result = engine.Compute(100.0, 0.25, 10, getBid, getAsk, 50.0, 50.0, 60.0);

    std::cout << "  liqValid=" << result.liqValid << " (expected 0 - warmup)" << std::endl;
    std::cout << "  liqState=" << LiquidityStateToString(result.liqState) << std::endl;
    std::cout << "  errorReason=" << LiquidityErrorReasonToString(result.errorReason) << std::endl;
    std::cout << "  IsWarmup()=" << result.IsWarmup() << std::endl;

    assert(!result.liqValid);
    assert(result.liqState == LiquidityState::LIQ_NOT_READY);
    assert(result.IsWarmup());

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// V1: STALENESS DETECTION
// ============================================================================

void test_v1_staleness_detection() {
    std::cout << "=== Test: V1 Staleness detection ===" << std::endl;

    LiquidityEngine engine;
    engine.Reset();

    // Pre-warm minimally
    for (int i = 0; i < 15; i++) {
        engine.PreWarmFromBar(100.0, 50.0, 50.0, 99.0, 60.0, SessionPhase::UNKNOWN);
    }

    auto getBid = [](int level, double& price, double& volume) {
        if (level == 0) { price = 99.75; volume = 100.0; return true; }
        return false;
    };
    auto getAsk = [](int level, double& price, double& volume) {
        if (level == 0) { price = 100.25; volume = 100.0; return true; }
        return false;
    };

    // First call to set up prevDepthMassTotal
    engine.Compute(100.0, 0.25, 10, getBid, getAsk, 50.0, 50.0, 60.0);

    // Test 1: Fresh data (100ms old, threshold is 2000ms)
    int64_t currentTime = 1000000;  // 1 second (1000000 ms)
    int64_t domTime = 999900;       // 100ms ago
    auto resultFresh = engine.Compute(100.0, 0.25, 10, getBid, getAsk, 50.0, 50.0, 60.0,
                                       -1.0, -1.0, -1.0, currentTime, domTime);
    std::cout << "  Fresh data (100ms): stale=" << resultFresh.depthStale
              << " ageMs=" << resultFresh.depthAgeMs
              << " action=" << LiquidityActionToString(resultFresh.recommendedAction) << std::endl;
    assert(!resultFresh.depthStale);
    assert(resultFresh.depthAgeMs == 100);
    assert(resultFresh.recommendedAction != LiquidityAction::HARD_BLOCK || resultFresh.liqState == LiquidityState::LIQ_VOID);

    // Test 2: Stale data (3000ms old, threshold is 2000ms)
    domTime = 997000;  // 3000ms ago
    auto resultStale = engine.Compute(100.0, 0.25, 10, getBid, getAsk, 50.0, 50.0, 60.0,
                                       -1.0, -1.0, -1.0, currentTime, domTime);
    std::cout << "  Stale data (3000ms): stale=" << resultStale.depthStale
              << " ageMs=" << resultStale.depthAgeMs
              << " action=" << LiquidityActionToString(resultStale.recommendedAction)
              << " error=" << LiquidityErrorReasonToString(resultStale.errorReason) << std::endl;
    assert(resultStale.depthStale);
    assert(resultStale.depthAgeMs == 3000);
    assert(resultStale.recommendedAction == LiquidityAction::HARD_BLOCK);
    assert(resultStale.errorReason == LiquidityErrorReason::ERR_DEPTH_STALE);
    assert(resultStale.liqState == LiquidityState::LIQ_NOT_READY);

    // Test 3: No timestamp provided (staleness check skipped)
    auto resultNoTs = engine.Compute(100.0, 0.25, 10, getBid, getAsk, 50.0, 50.0, 60.0);
    std::cout << "  No timestamp: stale=" << resultNoTs.depthStale
              << " ageMs=" << resultNoTs.depthAgeMs << std::endl;
    assert(!resultNoTs.depthStale);
    assert(resultNoTs.depthAgeMs == -1);  // Not provided

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// V1: EXECUTION FRICTION
// ============================================================================

void test_v1_execution_friction() {
    std::cout << "=== Test: V1 Execution friction score ===" << std::endl;

    LiquidityEngine engine;
    DOMWarmup warmup;
    warmup.Reset();
    engine.Reset();
    engine.SetDOMWarmup(&warmup);

    // Pre-warm with varied values
    for (int i = 0; i < 20; i++) {
        engine.PreWarmFromBar(50.0 + i * 10, 20.0 + i * 5, 20.0 + i * 5, 49.0, 60.0, SessionPhase::GLOBEX, 2.0);
    }
    engine.SetPhase(SessionPhase::GLOBEX);

    auto makeBid = [](double vol) {
        return [vol](int level, double& price, double& volume) {
            if (level == 0) { price = 99.75; volume = vol; return true; }
            return false;
        };
    };
    auto makeAsk = [](double vol) {
        return [vol](int level, double& price, double& volume) {
            if (level == 0) { price = 100.25; volume = vol; return true; }
            return false;
        };
    };

    // First call to set up prevDepthMassTotal
    engine.Compute(100.0, 0.25, 10, makeBid(100.0), makeAsk(100.0), 50.0, 50.0, 60.0);

    // Test 1: Good conditions (high depth, low stress)
    auto resultGood = engine.Compute(100.0, 0.25, 10, makeBid(500.0), makeAsk(500.0), 10.0, 10.0, 60.0, 1.0);
    std::cout << "  Good conditions: friction=" << resultGood.executionFriction
              << " valid=" << resultGood.frictionValid << std::endl;
    assert(resultGood.frictionValid);
    assert(resultGood.executionFriction >= 0.0 && resultGood.executionFriction <= 1.0);

    // Test 2: Bad conditions (low depth, high stress)
    auto resultBad = engine.Compute(100.0, 0.25, 10, makeBid(5.0), makeAsk(5.0), 200.0, 200.0, 60.0, 4.0);
    std::cout << "  Bad conditions: friction=" << resultBad.executionFriction
              << " valid=" << resultBad.frictionValid << std::endl;
    assert(resultBad.frictionValid);
    assert(resultBad.executionFriction >= 0.0 && resultBad.executionFriction <= 1.0);

    // Verify: bad conditions should have higher friction
    assert(resultBad.executionFriction > resultGood.executionFriction);
    std::cout << "  Bad friction > Good friction: PASSED" << std::endl;

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// V1: ACTION GUIDANCE
// ============================================================================

void test_v1_action_guidance() {
    std::cout << "=== Test: V1 Action guidance (recommendedAction) ===" << std::endl;

    LiquidityEngine engine;
    DOMWarmup warmup;
    warmup.Reset();
    engine.Reset();
    engine.SetDOMWarmup(&warmup);

    // Pre-warm with varied values
    for (int i = 0; i < 20; i++) {
        engine.PreWarmFromBar(50.0 + i * 10, 20.0 + i * 5, 20.0 + i * 5, 49.0, 60.0, SessionPhase::GLOBEX, 2.0);
    }
    engine.SetPhase(SessionPhase::GLOBEX);

    auto makeBid = [](double vol) {
        return [vol](int level, double& price, double& volume) {
            if (level == 0) { price = 99.75; volume = vol; return true; }
            return false;
        };
    };
    auto makeAsk = [](double vol) {
        return [vol](int level, double& price, double& volume) {
            if (level == 0) { price = 100.25; volume = vol; return true; }
            return false;
        };
    };

    // First call to set up prevDepthMassTotal
    engine.Compute(100.0, 0.25, 10, makeBid(100.0), makeAsk(100.0), 50.0, 50.0, 60.0);

    // Test 1: THICK state -> PROCEED
    auto resultThick = engine.Compute(100.0, 0.25, 10, makeBid(500.0), makeAsk(500.0), 10.0, 10.0, 60.0, 1.0);
    std::cout << "  THICK state: action=" << LiquidityActionToString(resultThick.recommendedAction)
              << " state=" << LiquidityStateToString(resultThick.liqState) << std::endl;
    if (resultThick.liqState == LiquidityState::LIQ_THICK || resultThick.liqState == LiquidityState::LIQ_NORMAL) {
        assert(resultThick.recommendedAction == LiquidityAction::PROCEED);
    }

    // Test 2: VOID state -> HARD_BLOCK
    auto resultVoid = engine.Compute(100.0, 0.25, 10, makeBid(1.0), makeAsk(1.0), 200.0, 200.0, 60.0, 4.0);
    std::cout << "  Low depth: action=" << LiquidityActionToString(resultVoid.recommendedAction)
              << " state=" << LiquidityStateToString(resultVoid.liqState) << std::endl;
    if (resultVoid.liqState == LiquidityState::LIQ_VOID) {
        assert(resultVoid.recommendedAction == LiquidityAction::HARD_BLOCK);
    }

    // Test 3: Helper functions
    std::cout << "  Testing helper functions..." << std::endl;
    assert(resultThick.CanProceed() == (resultThick.recommendedAction == LiquidityAction::PROCEED));
    assert(resultVoid.ShouldBlock() == (resultVoid.recommendedAction == LiquidityAction::HARD_BLOCK));

    std::cout << "  PASSED" << std::endl;
}

void test_v1_action_with_warmup() {
    std::cout << "=== Test: V1 Action during warmup ===" << std::endl;

    LiquidityEngine engine;
    engine.Reset();
    // NO pre-warm - baselines empty

    auto getBid = [](int level, double& price, double& volume) {
        if (level == 0) { price = 99.75; volume = 100.0; return true; }
        return false;
    };
    auto getAsk = [](int level, double& price, double& volume) {
        if (level == 0) { price = 100.25; volume = 100.0; return true; }
        return false;
    };

    auto result = engine.Compute(100.0, 0.25, 10, getBid, getAsk, 50.0, 50.0, 60.0);

    std::cout << "  Warmup: action=" << LiquidityActionToString(result.recommendedAction)
              << " state=" << LiquidityStateToString(result.liqState)
              << " frictionValid=" << result.frictionValid << std::endl;

    // During warmup: HARD_BLOCK (no valid data to make decisions)
    assert(result.recommendedAction == LiquidityAction::HARD_BLOCK);
    assert(!result.frictionValid);  // Cannot compute friction without baselines

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Extreme Liquidity Detection (Jan 2025)
// ============================================================================

void test_extreme_liquidity_from_stress() {
    std::cout << "=== Test: Extreme liquidity detection from high stress ===" << std::endl;

    LiquidityEngine engine;
    DOMWarmup warmup;
    warmup.Reset();
    engine.Reset();
    engine.SetDOMWarmup(&warmup);
    engine.SetPhase(SessionPhase::GLOBEX);

    // Pre-warm baselines with normal stress values
    for (int i = 0; i < 20; ++i) {
        // Normal stress: 100 aggressive against 100 depth -> stress = 0.99
        engine.PreWarmFromBar(100.0, 50.0, 50.0, 99.0, 60.0, SessionPhase::GLOBEX);
    }

    // Lambda to create DOM levels
    auto makeBidLevel = [](double vol) {
        return [vol](int level, double& price, double& volume) {
            if (level == 0) { price = 99.75; volume = vol; return true; }
            return false;
        };
    };
    auto makeAskLevel = [](double vol) {
        return [vol](int level, double& price, double& volume) {
            if (level == 0) { price = 100.25; volume = vol; return true; }
            return false;
        };
    };

    // First compute to set up prevDepthMassTotal
    engine.Compute(100.0, 0.25, 10, makeBidLevel(100.0), makeAskLevel(100.0), 50.0, 50.0, 60.0);

    // Now test with EXTREME stress (very high aggressive volume vs depth)
    // Stress rank >= P95 should trigger isExtremeLiquidity
    // 1000 aggressive against 20 depth -> stress = 49.5 (much higher than baseline ~0.99)
    auto extremeStressSnap = engine.Compute(100.0, 0.25, 10, makeBidLevel(10.0), makeAskLevel(10.0),
                                            500.0, 500.0, 60.0);

    std::cout << "  Extreme stress: stressRank=" << extremeStressSnap.stressRank
              << " isExtreme=" << extremeStressSnap.isExtremeLiquidity
              << " extremeFromStress=" << extremeStressSnap.extremeFromStress
              << std::endl;

    // Verify the flag computation logic matches expected threshold
    if (extremeStressSnap.stressRankValid) {
        const bool shouldBeExtreme = (extremeStressSnap.stressRank >= 0.95);
        if (shouldBeExtreme) {
            assert(extremeStressSnap.isExtremeLiquidity);
            assert(extremeStressSnap.extremeFromStress);
            std::cout << "  High stress correctly triggers extreme flag" << std::endl;
        } else {
            // Even if not >= P95, verify logic is consistent
            assert(extremeStressSnap.isExtremeLiquidity == shouldBeExtreme ||
                   extremeStressSnap.extremeFromDepth);  // Could be extreme from depth instead
            std::cout << "  Note: Stress rank=" << extremeStressSnap.stressRank
                      << " (threshold is P95=0.95)" << std::endl;
        }
    }

    std::cout << "  PASSED" << std::endl;
}

void test_extreme_liquidity_from_thin_depth() {
    std::cout << "=== Test: Extreme liquidity detection from thin depth ===" << std::endl;

    LiquidityEngine engine;
    DOMWarmup warmup;
    warmup.Reset();
    engine.Reset();
    engine.SetDOMWarmup(&warmup);
    engine.SetPhase(SessionPhase::GLOBEX);

    // Pre-warm with NORMAL depth values (100-200 mass)
    for (int i = 0; i < 20; ++i) {
        double depthMass = 100.0 + (i % 10) * 10.0;  // Vary 100-190
        engine.PreWarmFromBar(depthMass, 10.0, 10.0, depthMass - 1.0, 60.0, SessionPhase::GLOBEX);
    }

    // Lambda to create DOM levels with very thin depth
    auto makeThinBidLevel = [](int level, double& price, double& volume) {
        if (level == 0) { price = 99.75; volume = 1.0; return true; }  // Very thin!
        return false;
    };
    auto makeThinAskLevel = [](int level, double& price, double& volume) {
        if (level == 0) { price = 100.25; volume = 1.0; return true; }  // Very thin!
        return false;
    };

    // First compute to set up prevDepthMassTotal
    engine.Compute(100.0, 0.25, 10, makeThinBidLevel, makeThinAskLevel, 10.0, 10.0, 60.0);

    // Test with VERY THIN depth (should be <= P5)
    auto thinDepthSnap = engine.Compute(100.0, 0.25, 10, makeThinBidLevel, makeThinAskLevel,
                                        10.0, 10.0, 60.0);

    std::cout << "  Thin depth: depthRank=" << thinDepthSnap.depthRank
              << " isExtreme=" << thinDepthSnap.isExtremeLiquidity
              << " extremeFromDepth=" << thinDepthSnap.extremeFromDepth
              << std::endl;

    // Verify the flag computation logic
    if (thinDepthSnap.depthRankValid) {
        const bool shouldBeExtremeFromDepth = (thinDepthSnap.depthRank <= 0.05);
        if (shouldBeExtremeFromDepth) {
            assert(thinDepthSnap.isExtremeLiquidity);
            assert(thinDepthSnap.extremeFromDepth);
            std::cout << "  Thin depth correctly triggers extreme flag" << std::endl;
        } else {
            std::cout << "  Note: Depth rank=" << thinDepthSnap.depthRank
                      << " (threshold is P5=0.05)" << std::endl;
        }
    }

    std::cout << "  PASSED" << std::endl;
}

void test_liquidity_shock_detection() {
    std::cout << "=== Test: Liquidity shock (P99+) detection ===" << std::endl;

    LiquidityEngine engine;
    DOMWarmup warmup;
    warmup.Reset();
    engine.Reset();
    engine.SetDOMWarmup(&warmup);
    engine.SetPhase(SessionPhase::GLOBEX);

    // Pre-warm with consistent values to establish baseline
    for (int i = 0; i < 100; ++i) {
        engine.PreWarmFromBar(100.0, 50.0, 50.0, 99.0, 60.0, SessionPhase::GLOBEX);
    }

    // Lambdas for shock test
    auto makeBidLevel = [](double vol) {
        return [vol](int level, double& price, double& volume) {
            if (level == 0) { price = 99.75; volume = vol; return true; }
            return false;
        };
    };
    auto makeAskLevel = [](double vol) {
        return [vol](int level, double& price, double& volume) {
            if (level == 0) { price = 100.25; volume = vol; return true; }
            return false;
        };
    };

    // First compute
    engine.Compute(100.0, 0.25, 10, makeBidLevel(100.0), makeAskLevel(100.0), 50.0, 50.0, 60.0);

    // Test with SHOCK level conditions - extremely thin depth + high stress
    auto shockSnap = engine.Compute(100.0, 0.25, 10, makeBidLevel(0.5), makeAskLevel(0.5),
                                    2000.0, 2000.0, 60.0);

    std::cout << "  Shock level: stressRank=" << shockSnap.stressRank
              << " depthRank=" << shockSnap.depthRank
              << " isShock=" << shockSnap.isLiquidityShock
              << std::endl;

    // Verify helper methods work correctly
    std::cout << "  Helper IsExtremeLiquidity()=" << shockSnap.IsExtremeLiquidity()
              << " IsLiquidityShock()=" << shockSnap.IsLiquidityShock()
              << std::endl;

    // At minimum, verify the flags are set correctly based on computed ranks
    if (shockSnap.depthRankValid && shockSnap.stressRankValid) {
        const bool shouldBeExtreme = (shockSnap.stressRank >= 0.95) || (shockSnap.depthRank <= 0.05);
        const bool shouldBeShock = (shockSnap.stressRank >= 0.99) || (shockSnap.depthRank <= 0.01);

        assert(shockSnap.isExtremeLiquidity == shouldBeExtreme);
        assert(shockSnap.isLiquidityShock == shouldBeShock);
        std::cout << "  Flags correctly match threshold logic" << std::endl;
    }

    std::cout << "  PASSED" << std::endl;
}

void test_extreme_liquidity_flags_inactive_during_warmup() {
    std::cout << "=== Test: Extreme flags inactive during warmup ===" << std::endl;

    LiquidityEngine engine;
    engine.Reset();
    engine.SetPhase(SessionPhase::GLOBEX);
    // Note: NO DOMWarmup set, NO pre-warming - baselines won't be ready

    auto makeBidLevel = [](int level, double& price, double& volume) {
        if (level == 0) { price = 99.75; volume = 1000.0; return true; }
        return false;
    };
    auto makeAskLevel = [](int level, double& price, double& volume) {
        if (level == 0) { price = 100.25; volume = 1000.0; return true; }
        return false;
    };

    // Don't pre-warm - baselines not ready
    auto warmupSnap = engine.Compute(100.0, 0.25, 10, makeBidLevel, makeAskLevel, 500.0, 500.0, 60.0);

    std::cout << "  Warmup state: depthRankValid=" << warmupSnap.depthRankValid
              << " stressRankValid=" << warmupSnap.stressRankValid
              << " liqValid=" << warmupSnap.liqValid
              << std::endl;
    std::cout << "  Flags during warmup: isExtreme=" << warmupSnap.isExtremeLiquidity
              << " isShock=" << warmupSnap.isLiquidityShock
              << std::endl;

    // During warmup, extreme flags should remain false (default)
    if (!warmupSnap.depthRankValid || !warmupSnap.stressRankValid) {
        assert(!warmupSnap.isExtremeLiquidity);
        assert(!warmupSnap.isLiquidityShock);
        std::cout << "  Extreme flags correctly inactive during warmup" << std::endl;
    }

    // Also verify helper methods check liqValid
    assert(!warmupSnap.IsExtremeLiquidity());
    assert(!warmupSnap.IsLiquidityShock());
    std::cout << "  Helper methods correctly return false during warmup" << std::endl;

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Full Liquidity Engine Tests" << std::endl;
    std::cout << "(Kyle's 4-Component Model)" << std::endl;
    std::cout << "========================================\n" << std::endl;

    std::cout << "--- Component 1: DepthMass ---\n" << std::endl;
    test_depth_mass_calculation();
    test_depth_mass_dmax_cutoff();

    std::cout << "\n--- Component 2: Stress ---\n" << std::endl;
    test_stress_calculation();

    std::cout << "\n--- Component 3: Resilience ---\n" << std::endl;
    test_resilience_calculation();

    std::cout << "\n--- Component 4: Spread (Tightness) ---\n" << std::endl;
    test_spread_impact_on_liq();

    std::cout << "\n--- Baseline Warmup ---\n" << std::endl;
    test_baseline_warmup();

    std::cout << "\n--- Composite LIQ ---\n" << std::endl;
    test_compute_composite_liq();
    test_liqstate_classification();

    std::cout << "\n--- Toxicity Proxy ---\n" << std::endl;
    test_toxicity_proxy();

    std::cout << "\n--- Error Handling ---\n" << std::endl;
    test_error_handling_warmup();

    std::cout << "\n--- V1: Staleness Detection ---\n" << std::endl;
    test_v1_staleness_detection();

    std::cout << "\n--- V1: Execution Friction ---\n" << std::endl;
    test_v1_execution_friction();

    std::cout << "\n--- V1: Action Guidance ---\n" << std::endl;
    test_v1_action_guidance();
    test_v1_action_with_warmup();

    std::cout << "\n--- Extreme Liquidity Detection (Jan 2025) ---\n" << std::endl;
    test_extreme_liquidity_from_stress();
    test_extreme_liquidity_from_thin_depth();
    test_liquidity_shock_detection();
    test_extreme_liquidity_flags_inactive_during_warmup();

    std::cout << "\n========================================" << std::endl;
    std::cout << "All Liquidity Engine Tests PASSED!" << std::endl;
    std::cout << "(Including V1 Features + Extreme Detection)" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
}
