// test_liquidity_phase_aware.cpp - Verify phase-aware liquidity engine baselines
// Tests that depth/spread use DOMWarmup (phase-bucketed) while stress/resilience stay local
#include <iostream>
#include <cassert>
#include <cmath>

// Include required headers
#include "test_sierrachart_mock.h"
#include "../amt_core.h"
#include "../AMT_Snapshots.h"
#include "../AMT_Liquidity.h"

using namespace AMT;

// ============================================================================
// TEST: HasPhaseAwareBaselines() logic
// ============================================================================

void test_has_phase_aware_baselines_no_warmup() {
    std::cout << "=== Test: HasPhaseAwareBaselines without DOMWarmup ===" << std::endl;

    LiquidityEngine engine;
    engine.Reset();

    // No DOMWarmup set - should return false regardless of phase
    engine.SetPhase(SessionPhase::MID_SESSION);
    assert(engine.HasPhaseAwareBaselines() == false);
    std::cout << "  No DOMWarmup + valid phase = false: PASSED" << std::endl;

    engine.SetPhase(SessionPhase::GLOBEX);
    assert(engine.HasPhaseAwareBaselines() == false);
    std::cout << "  No DOMWarmup + GLOBEX = false: PASSED" << std::endl;

    std::cout << "  PASSED" << std::endl;
}

void test_has_phase_aware_baselines_with_warmup() {
    std::cout << "=== Test: HasPhaseAwareBaselines with DOMWarmup ===" << std::endl;

    LiquidityEngine engine;
    DOMWarmup warmup;
    warmup.Reset();
    engine.Reset();
    engine.SetDOMWarmup(&warmup);

    // Valid tradeable phases should return true
    engine.SetPhase(SessionPhase::GLOBEX);
    assert(engine.HasPhaseAwareBaselines() == true);
    std::cout << "  GLOBEX = true: PASSED" << std::endl;

    engine.SetPhase(SessionPhase::LONDON_OPEN);
    assert(engine.HasPhaseAwareBaselines() == true);
    std::cout << "  LONDON_OPEN = true: PASSED" << std::endl;

    engine.SetPhase(SessionPhase::PRE_MARKET);
    assert(engine.HasPhaseAwareBaselines() == true);
    std::cout << "  PRE_MARKET = true: PASSED" << std::endl;

    engine.SetPhase(SessionPhase::INITIAL_BALANCE);
    assert(engine.HasPhaseAwareBaselines() == true);
    std::cout << "  INITIAL_BALANCE = true: PASSED" << std::endl;

    engine.SetPhase(SessionPhase::MID_SESSION);
    assert(engine.HasPhaseAwareBaselines() == true);
    std::cout << "  MID_SESSION = true: PASSED" << std::endl;

    engine.SetPhase(SessionPhase::CLOSING_SESSION);
    assert(engine.HasPhaseAwareBaselines() == true);
    std::cout << "  CLOSING_SESSION = true: PASSED" << std::endl;

    engine.SetPhase(SessionPhase::POST_CLOSE);
    assert(engine.HasPhaseAwareBaselines() == true);
    std::cout << "  POST_CLOSE = true: PASSED" << std::endl;

    // Non-tradeable phases should return false
    engine.SetPhase(SessionPhase::UNKNOWN);
    assert(engine.HasPhaseAwareBaselines() == false);
    std::cout << "  UNKNOWN = false: PASSED" << std::endl;

    engine.SetPhase(SessionPhase::MAINTENANCE);
    assert(engine.HasPhaseAwareBaselines() == false);
    std::cout << "  MAINTENANCE = false: PASSED" << std::endl;

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: PreWarmFromBar routes depth to correct location
// ============================================================================

void test_prewarm_routes_depth_to_domwarmup() {
    std::cout << "=== Test: PreWarmFromBar routes depth to DOMWarmup ===" << std::endl;

    LiquidityEngine engine;
    DOMWarmup warmup;
    warmup.Reset();
    engine.Reset();
    engine.SetDOMWarmup(&warmup);

    // Pre-warm with GLOBEX phase
    for (int i = 0; i < 15; i++) {
        engine.PreWarmFromBar(100.0 + i, 50.0, 50.0, 99.0, 60.0, SessionPhase::GLOBEX);
    }

    // Check that depth went to DOMWarmup's GLOBEX bucket
    size_t globexDepth = warmup.Get(SessionPhase::GLOBEX).depthMassCore.size();
    std::cout << "  GLOBEX bucket depth samples: " << globexDepth << std::endl;
    assert(globexDepth == 15);

    // Check that other buckets are empty
    size_t ibDepth = warmup.Get(SessionPhase::INITIAL_BALANCE).depthMassCore.size();
    assert(ibDepth == 0);
    std::cout << "  INITIAL_BALANCE bucket depth samples: " << ibDepth << " (expected 0)" << std::endl;

    // Pre-warm with INITIAL_BALANCE phase
    for (int i = 0; i < 10; i++) {
        engine.PreWarmFromBar(200.0 + i, 60.0, 60.0, 199.0, 60.0, SessionPhase::INITIAL_BALANCE);
    }

    ibDepth = warmup.Get(SessionPhase::INITIAL_BALANCE).depthMassCore.size();
    std::cout << "  INITIAL_BALANCE bucket after prewarm: " << ibDepth << std::endl;
    assert(ibDepth == 10);

    // GLOBEX should still have 15
    globexDepth = warmup.Get(SessionPhase::GLOBEX).depthMassCore.size();
    assert(globexDepth == 15);

    std::cout << "  PASSED" << std::endl;
}

void test_prewarm_stress_resilience_stay_local() {
    std::cout << "=== Test: PreWarmFromBar keeps stress/resilience local ===" << std::endl;

    LiquidityEngine engine;
    DOMWarmup warmup;
    warmup.Reset();
    engine.Reset();
    engine.SetDOMWarmup(&warmup);

    // Pre-warm with mixed phases - stress/resilience should accumulate locally
    for (int i = 0; i < 10; i++) {
        engine.PreWarmFromBar(100.0, 50.0, 50.0, 99.0, 60.0, SessionPhase::GLOBEX);
    }
    for (int i = 0; i < 10; i++) {
        engine.PreWarmFromBar(100.0, 50.0, 50.0, 99.0, 60.0, SessionPhase::MID_SESSION);
    }

    // Stress and resilience should be local (20 total samples)
    size_t stressSamples = engine.stressBaseline.Size();
    size_t resSamples = engine.resilienceBaseline.Size();

    std::cout << "  Local stress samples: " << stressSamples << " (expected 20)" << std::endl;
    std::cout << "  Local resilience samples: " << resSamples << " (expected 20)" << std::endl;

    assert(stressSamples == 20);
    assert(resSamples == 20);

    std::cout << "  PASSED" << std::endl;
}

void test_prewarm_fallback_without_domwarmup() {
    std::cout << "=== Test: PreWarmFromBar uses fallback without DOMWarmup ===" << std::endl;

    LiquidityEngine engine;
    engine.Reset();
    // NOT setting DOMWarmup

    for (int i = 0; i < 15; i++) {
        engine.PreWarmFromBar(100.0 + i, 50.0, 50.0, 99.0, 60.0, SessionPhase::GLOBEX);
    }

    // Depth should go to local fallback
    size_t fallbackDepth = engine.depthBaselineFallback.Size();
    std::cout << "  Fallback depth samples: " << fallbackDepth << std::endl;
    assert(fallbackDepth == 15);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: GetDiagnostics returns phase-aware counts
// ============================================================================

void test_get_diagnostics_phase_aware() {
    std::cout << "=== Test: GetDiagnostics returns phase-aware counts ===" << std::endl;

    LiquidityEngine engine;
    DOMWarmup warmup;
    warmup.Reset();
    engine.Reset();
    engine.SetDOMWarmup(&warmup);

    // Pre-warm GLOBEX phase
    for (int i = 0; i < 12; i++) {
        engine.PreWarmFromBar(100.0, 50.0, 50.0, 99.0, 60.0, SessionPhase::GLOBEX);
    }

    // Set phase to GLOBEX and check diagnostics
    engine.SetPhase(SessionPhase::GLOBEX);
    size_t depthSamples, stressSamples, resSamples, spreadSamples;
    engine.GetDiagnostics(depthSamples, stressSamples, resSamples, spreadSamples);

    std::cout << "  GLOBEX phase - depth=" << depthSamples << " stress=" << stressSamples
              << " res=" << resSamples << " spread=" << spreadSamples << std::endl;
    assert(depthSamples == 12);  // From DOMWarmup GLOBEX bucket
    assert(stressSamples == 12); // Local
    assert(resSamples == 12);    // Local

    // Pre-warm MID_SESSION phase
    for (int i = 0; i < 8; i++) {
        engine.PreWarmFromBar(200.0, 60.0, 60.0, 199.0, 60.0, SessionPhase::MID_SESSION);
    }

    // Switch phase to MID_SESSION - should see different depth count
    engine.SetPhase(SessionPhase::MID_SESSION);
    engine.GetDiagnostics(depthSamples, stressSamples, resSamples, spreadSamples);

    std::cout << "  MID_SESSION phase - depth=" << depthSamples << " stress=" << stressSamples
              << " res=" << resSamples << std::endl;
    assert(depthSamples == 8);   // From DOMWarmup MID_SESSION bucket
    assert(stressSamples == 20); // Local (12 + 8)
    assert(resSamples == 20);    // Local (12 + 8)

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: GetPreWarmStatus reports phase-aware readiness
// ============================================================================

void test_get_prewarm_status_phase_aware() {
    std::cout << "=== Test: GetPreWarmStatus reports phase-aware readiness ===" << std::endl;

    LiquidityEngine engine;
    DOMWarmup warmup;
    warmup.Reset();
    engine.Reset();
    engine.SetDOMWarmup(&warmup);

    // Pre-warm GLOBEX with enough samples (>= 10)
    for (int i = 0; i < 15; i++) {
        engine.PreWarmFromBar(100.0, 50.0, 50.0, 99.0, 60.0, SessionPhase::GLOBEX);
    }

    // Set phase to GLOBEX - should be ready
    engine.SetPhase(SessionPhase::GLOBEX);
    auto status = engine.GetPreWarmStatus();

    std::cout << "  GLOBEX phase status: depth=" << status.depthSamples
              << " ready=" << status.depthReady << " allReady=" << status.allReady << std::endl;
    assert(status.depthSamples == 15);
    assert(status.depthReady == true);
    assert(status.allReady == true);

    // Switch to MID_SESSION - no samples there yet
    engine.SetPhase(SessionPhase::MID_SESSION);
    status = engine.GetPreWarmStatus();

    std::cout << "  MID_SESSION phase status: depth=" << status.depthSamples
              << " ready=" << status.depthReady << " allReady=" << status.allReady << std::endl;
    assert(status.depthSamples == 0);
    assert(status.depthReady == false);
    assert(status.allReady == false);  // Depth not ready for this phase

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Compute uses phase-aware percentiles
// ============================================================================

void test_compute_uses_phase_aware_percentiles() {
    std::cout << "=== Test: Compute uses phase-aware percentiles ===" << std::endl;

    LiquidityEngine engine;
    DOMWarmup warmup;
    warmup.Reset();
    engine.Reset();
    engine.SetDOMWarmup(&warmup);

    // Pre-warm GLOBEX with LOW depth values (10-25)
    for (int i = 0; i < 15; i++) {
        engine.PreWarmFromBar(10.0 + i, 50.0, 50.0, 9.0, 60.0, SessionPhase::GLOBEX);
    }

    // Pre-warm MID_SESSION with HIGH depth values (500-515)
    for (int i = 0; i < 15; i++) {
        engine.PreWarmFromBar(500.0 + i, 50.0, 50.0, 499.0, 60.0, SessionPhase::MID_SESSION);
    }

    // Lambdas that produce depth ~100 (50 bid + 50 ask at near-touch)
    // With tickSize=0.25 and dmax=4, levels at distance 0 have weight 1.0
    auto getBidLevel = [](int level, double& price, double& volume) {
        if (level == 0) { price = 100.0; volume = 50.0; return true; }  // At reference = 0 ticks distance
        return false;
    };
    auto getAskLevel = [](int level, double& price, double& volume) {
        if (level == 0) { price = 100.0; volume = 50.0; return true; }  // At reference = 0 ticks distance
        return false;
    };

    // Compute in GLOBEX phase (depth ~100 vs baseline 10-25 -> should be 100%)
    engine.SetPhase(SessionPhase::GLOBEX);
    auto resultGlobex = engine.Compute(100.0, 0.25, 10, getBidLevel, getAskLevel, 50.0, 50.0, 60.0);

    std::cout << "  GLOBEX phase: depthRank=" << resultGlobex.depthRank
              << " valid=" << resultGlobex.depthRankValid
              << " baselineReady=" << resultGlobex.depthBaselineReady
              << " depth.valid=" << resultGlobex.depth.valid
              << " depth.totalMass=" << resultGlobex.depth.totalMass
              << std::endl;

    // Compute in MID_SESSION phase (depth ~100 vs baseline 500-515 -> should be 0%)
    engine.SetPhase(SessionPhase::MID_SESSION);
    auto resultMid = engine.Compute(100.0, 0.25, 10, getBidLevel, getAskLevel, 50.0, 50.0, 60.0);

    std::cout << "  MID_SESSION phase: depthRank=" << resultMid.depthRank
              << " valid=" << resultMid.depthRankValid
              << " baselineReady=" << resultMid.depthBaselineReady
              << " depth.valid=" << resultMid.depth.valid
              << " depth.totalMass=" << resultMid.depth.totalMass
              << std::endl;

    // Verify both computed depths successfully
    assert(resultGlobex.depth.valid);
    assert(resultMid.depth.valid);
    assert(resultGlobex.depthBaselineReady);
    assert(resultMid.depthBaselineReady);
    assert(resultGlobex.depthRankValid);
    assert(resultMid.depthRankValid);

    // Verify ranks are different based on phase
    // GLOBEX: 100 vs 10-25 baseline -> high rank (~100%)
    // MID_SESSION: 100 vs 500-515 baseline -> low rank (~0%)
    std::cout << "  Comparing: GLOBEX rank=" << resultGlobex.depthRank
              << " vs MID_SESSION rank=" << resultMid.depthRank << std::endl;
    assert(resultGlobex.depthRank > resultMid.depthRank);
    std::cout << "  GLOBEX rank > MID_SESSION rank: PASSED" << std::endl;

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Spread also uses phase-aware baselines
// ============================================================================

void test_spread_uses_phase_aware_baselines() {
    std::cout << "=== Test: Spread uses phase-aware baselines ===" << std::endl;

    LiquidityEngine engine;
    DOMWarmup warmup;
    warmup.Reset();
    engine.Reset();
    engine.SetDOMWarmup(&warmup);

    // Pre-warm GLOBEX with wide spreads (3-4 ticks)
    for (int i = 0; i < 15; i++) {
        engine.PreWarmFromBar(100.0, 50.0, 50.0, 99.0, 60.0, SessionPhase::GLOBEX, 3.5);
    }

    // Pre-warm MID_SESSION with tight spreads (1 tick)
    for (int i = 0; i < 15; i++) {
        engine.PreWarmFromBar(100.0, 50.0, 50.0, 99.0, 60.0, SessionPhase::MID_SESSION, 1.0);
    }

    // Check spread samples in each bucket
    size_t globexSpread = warmup.Get(SessionPhase::GLOBEX).spreadTicks.size();
    size_t midSpread = warmup.Get(SessionPhase::MID_SESSION).spreadTicks.size();

    std::cout << "  GLOBEX spread samples: " << globexSpread << std::endl;
    std::cout << "  MID_SESSION spread samples: " << midSpread << std::endl;

    assert(globexSpread == 15);
    assert(midSpread == 15);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Phase-Aware Liquidity Engine Tests" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // HasPhaseAwareBaselines tests
    test_has_phase_aware_baselines_no_warmup();
    test_has_phase_aware_baselines_with_warmup();

    std::cout << "\n--- PreWarmFromBar Routing Tests ---\n" << std::endl;
    test_prewarm_routes_depth_to_domwarmup();
    test_prewarm_stress_resilience_stay_local();
    test_prewarm_fallback_without_domwarmup();

    std::cout << "\n--- Diagnostics Tests ---\n" << std::endl;
    test_get_diagnostics_phase_aware();
    test_get_prewarm_status_phase_aware();

    std::cout << "\n--- Compute Phase-Aware Tests ---\n" << std::endl;
    test_compute_uses_phase_aware_percentiles();
    test_spread_uses_phase_aware_baselines();

    std::cout << "\n========================================" << std::endl;
    std::cout << "All tests PASSED!" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
}
