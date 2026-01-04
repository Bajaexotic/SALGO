// test_baseline_readiness_stage2.cpp
// Stage 3 Tests: All ConfidenceAttribute metrics have validity flags
// Stage 2.1: deltaConsistency, liquidityAvailability
// Stage 3: domStrength, tpoAcceptance, volumeProfileClarity (unimplemented, default invalid)
//
// Build: g++ -std=c++17 -I.. -o test_baseline_readiness_stage2.exe test_baseline_readiness_stage2.cpp
// Run: ./test_baseline_readiness_stage2.exe

#include <iostream>
#include <cassert>
#include <cmath>
#include "test_sierrachart_mock.h"
#include "../AMT_Snapshots.h"
#include "../AMT_config.h"
#include "../AMT_Patterns.h"

using namespace AMT;

// Test helper
#define TEST(name) std::cout << "=== Test: " << name << " ===" << std::endl
#define PASS() std::cout << "  PASSED" << std::endl

//------------------------------------------------------------------------------
// Test 1: ConfidenceAttribute validity flags exist
//------------------------------------------------------------------------------
void test_confidence_validity_flags_exist() {
    TEST("ConfidenceAttribute validity flags exist");

    ConfidenceAttribute conf;

    // Check default validity state is false
    assert(!conf.deltaConsistencyValid && "deltaConsistencyValid should default to false");
    assert(!conf.deltaStrengthValid && "deltaStrengthValid should default to false");
    assert(!conf.liquidityAvailabilityValid && "liquidityAvailabilityValid should default to false");

    // Check numeric values default correctly
    // deltaConsistency defaults to 0.5 (neutral fraction), deltaStrength to 0.0 (no signal)
    assert(conf.deltaConsistency == 0.5f);  // Neutral fraction (was 0.0f - BUG)
    assert(conf.deltaStrength == 0.0f);     // No signal
    assert(conf.liquidityAvailability == 0.0f);

    std::cout << "  deltaConsistencyValid default: false - OK" << std::endl;
    std::cout << "  deltaStrengthValid default: false - OK" << std::endl;
    std::cout << "  liquidityAvailabilityValid default: false - OK" << std::endl;

    PASS();
}

//------------------------------------------------------------------------------
// Test 2: calculate_score() excludes invalid components
//------------------------------------------------------------------------------
void test_calculate_score_excludes_invalid() {
    TEST("calculate_score() excludes invalid components");

    ConfidenceAttribute conf;
    ConfidenceWeights w;

    // Set all weights equal for easy math
    w.dom = 0.2f;
    w.delta = 0.2f;
    w.profile = 0.2f;
    w.tpo = 0.2f;
    w.liquidity = 0.2f;

    // Scenario 1: All components valid with value 1.0
    // Stage 3: Must set ALL validity flags
    // Note: Score uses deltaStrength (magnitude), not deltaConsistency (fraction)
    conf.domStrength = 1.0f;
    conf.deltaStrength = 1.0f;  // Score uses deltaStrength, not deltaConsistency
    conf.volumeProfileClarity = 1.0f;
    conf.tpoAcceptance = 1.0f;
    conf.liquidityAvailability = 1.0f;
    conf.domStrengthValid = true;
    conf.volumeProfileClarityValid = true;
    conf.tpoAcceptanceValid = true;
    conf.deltaStrengthValid = true;  // Score uses deltaStrengthValid
    conf.liquidityAvailabilityValid = true;

    ScoreResult result1 = conf.calculate_score(w);
    assert(result1.scoreValid);
    assert(std::abs(result1.score - 1.0f) < 0.001f);
    std::cout << "  All valid, all 1.0: score=" << result1.score << " (expected 1.0) - OK" << std::endl;

    // Scenario 2: deltaStrength INVALID - should be excluded
    conf.deltaStrengthValid = false;
    // Note: deltaStrength numeric is still 1.0, but should be IGNORED
    ScoreResult result2 = conf.calculate_score(w);
    // Expected: (1*0.2 + 1*0.2 + 1*0.2 + 1*0.2) / 0.8 = 0.8/0.8 = 1.0 (renormalized)
    assert(result2.scoreValid);
    assert(std::abs(result2.score - 1.0f) < 0.001f);
    std::cout << "  deltaStrength INVALID: score=" << result2.score << " (expected 1.0 renormalized) - OK" << std::endl;

    // Scenario 3: Both deltaStrength and liquidity INVALID
    conf.liquidityAvailabilityValid = false;
    ScoreResult result3 = conf.calculate_score(w);
    // Expected: (1*0.2 + 1*0.2 + 1*0.2) / 0.6 = 0.6/0.6 = 1.0 (renormalized)
    assert(result3.scoreValid);
    assert(std::abs(result3.score - 1.0f) < 0.001f);
    std::cout << "  Both deltaStrength/liquidity INVALID: score=" << result3.score << " (expected 1.0 renormalized) - OK" << std::endl;

    // Scenario 4: Mixed values with one invalid
    conf.domStrength = 0.5f;
    conf.volumeProfileClarity = 0.5f;
    conf.tpoAcceptance = 0.5f;
    conf.deltaStrengthValid = true;
    conf.deltaStrength = 1.0f;  // High delta strength but...
    conf.liquidityAvailabilityValid = false;  // Liquidity invalid
    conf.liquidityAvailability = 0.0f;  // This should be IGNORED

    ScoreResult result4 = conf.calculate_score(w);
    // Expected: (0.5*0.2 + 1.0*0.2 + 0.5*0.2 + 0.5*0.2) / 0.8 = 0.5/0.8 = 0.625
    assert(result4.scoreValid);
    assert(std::abs(result4.score - 0.625f) < 0.001f);
    std::cout << "  Mixed with one INVALID: score=" << result4.score << " (expected 0.625) - OK" << std::endl;

    PASS();
}

//------------------------------------------------------------------------------
// Test 3: AuctionFacilitation::UNKNOWN exists
//------------------------------------------------------------------------------
void test_facilitation_unknown_exists() {
    TEST("AuctionFacilitation::UNKNOWN exists");

    AuctionFacilitation unknown = AuctionFacilitation::UNKNOWN;
    assert(static_cast<int>(unknown) == 0);
    std::cout << "  UNKNOWN = 0 - OK" << std::endl;

    // Verify other values unchanged
    assert(static_cast<int>(AuctionFacilitation::EFFICIENT) == 1);
    assert(static_cast<int>(AuctionFacilitation::INEFFICIENT) == 2);
    assert(static_cast<int>(AuctionFacilitation::LABORED) == 3);
    assert(static_cast<int>(AuctionFacilitation::FAILED) == 4);
    std::cout << "  Other values unchanged - OK" << std::endl;

    PASS();
}

//------------------------------------------------------------------------------
// Test 4: Validity flag propagation pattern
//------------------------------------------------------------------------------
void test_validity_propagation_pattern() {
    TEST("Validity flag propagation pattern");

    // Simulate the pattern from AuctionSensor_v1.cpp
    RollingDist baseline;
    baseline.reset(100);

    ConfidenceAttribute conf;

    // Simulate: baseline not ready
    bool baselineReady = baseline.IsReady(BaselineMinSamples::TOTAL_VOL);
    assert(!baselineReady);

    if (!baselineReady) {
        // Stage 2.1 pattern: set validity false, do NOT write numeric
        conf.deltaConsistencyValid = false;
        // Note: conf.deltaConsistency is NOT written
    }

    assert(!conf.deltaConsistencyValid);
    std::cout << "  Baseline not ready -> valid=false - OK" << std::endl;

    // Now populate baseline
    for (int i = 0; i < 25; i++) {
        baseline.push(100.0 + i);
    }

    baselineReady = baseline.IsReady(BaselineMinSamples::TOTAL_VOL);
    assert(baselineReady);

    if (baselineReady) {
        // Stage 2.1 pattern: compute and set validity true
        conf.deltaConsistency = 0.75f;  // Some computed value
        conf.deltaConsistencyValid = true;
    }

    assert(conf.deltaConsistencyValid);
    assert(conf.deltaConsistency == 0.75f);
    std::cout << "  Baseline ready -> valid=true, value=0.75 - OK" << std::endl;

    PASS();
}

//------------------------------------------------------------------------------
// Test 5: checkExtremes baselinesReady flag (unchanged from Stage 2)
//------------------------------------------------------------------------------
void test_checkExtremes_readiness_flag() {
    TEST("checkExtremes baselinesReady flag");

    BaselineEngine be;
    be.reset(100);

    // With empty baselines, baselinesReady should be false
    BaselineEngine::ExtremeCheck result = be.checkExtremes(50.0, 50.0, 10.0, 0.0, 0.0, 5.0);
    assert(!result.baselinesReady && "Empty baselines should set baselinesReady=false");
    std::cout << "  Empty baselines: baselinesReady=false - OK" << std::endl;

    // Add samples to all required baselines
    for (int i = 0; i < 25; i++) {
        be.vol_sec.push(100.0);
        be.delta_pct.push(0.5);
        be.trades_sec.push(10.0);
        be.stack_rate.push(5.0);
        be.pull_rate.push(3.0);
        be.depth_mass_core.push(100.0);
    }

    result = be.checkExtremes(50.0, 50.0, 10.0, 0.0, 0.0, 5.0);
    assert(result.baselinesReady && "Full baselines should set baselinesReady=true");
    std::cout << "  Full baselines: baselinesReady=true - OK" << std::endl;

    PASS();
}

//------------------------------------------------------------------------------
// Test 6: Fallback values NOT used when invalid
//------------------------------------------------------------------------------
void test_no_fallback_when_invalid() {
    TEST("Fallback values NOT used when invalid");

    ConfidenceAttribute conf;
    ConfidenceWeights w;
    w.dom = 0.2f;
    w.delta = 0.2f;
    w.profile = 0.2f;
    w.tpo = 0.2f;
    w.liquidity = 0.2f;

    // Stage 3: Set valid components to 0.5 with validity=true
    conf.domStrength = 0.5f;
    conf.domStrengthValid = true;
    conf.volumeProfileClarity = 0.5f;
    conf.volumeProfileClarityValid = true;
    conf.tpoAcceptance = 0.5f;
    conf.tpoAcceptanceValid = true;

    // Set invalid components to 0.0 with INVALID flag
    // This simulates the OLD Stage 2 behavior that we're fixing
    // Note: Score uses deltaStrength (magnitude), not deltaConsistency (fraction)
    conf.deltaStrength = 0.0f;  // Would have been 0.0f "fallback"
    conf.liquidityAvailability = 0.0f;  // Would have been 0.0f "fallback"
    conf.deltaStrengthValid = false;  // Marked INVALID
    conf.liquidityAvailabilityValid = false;

    ScoreResult result = conf.calculate_score(w);
    // Expected: (0.5*0.2 + 0.5*0.2 + 0.5*0.2) / 0.6 = 0.3/0.6 = 0.5
    // NOT: (0.5*0.2 + 0.0*0.2 + 0.5*0.2 + 0.5*0.2 + 0.0*0.2) / 1.0 = 0.3/1.0 = 0.3
    assert(result.scoreValid);
    assert(std::abs(result.score - 0.5f) < 0.001f);
    std::cout << "  Invalid components excluded from score: " << result.score << " (expected 0.5) - OK" << std::endl;
    std::cout << "  (Old behavior would have given 0.3 - that's wrong)" << std::endl;

    PASS();
}

//------------------------------------------------------------------------------
// Test 7: Unimplemented metrics default to invalid
//------------------------------------------------------------------------------
void test_unimplemented_metrics_default_invalid() {
    TEST("Unimplemented metrics (dom/tpo/profile) default to invalid");

    ConfidenceAttribute conf;  // Fresh instance with defaults

    // Stage 3: Unimplemented metrics default to invalid
    assert(!conf.domStrengthValid);
    assert(!conf.tpoAcceptanceValid);
    assert(!conf.volumeProfileClarityValid);
    std::cout << "  domStrengthValid default: false - OK" << std::endl;
    std::cout << "  tpoAcceptanceValid default: false - OK" << std::endl;
    std::cout << "  volumeProfileClarityValid default: false - OK" << std::endl;

    // Implemented metrics also default to invalid (until set by computation)
    assert(!conf.deltaConsistencyValid);
    assert(!conf.deltaStrengthValid);
    assert(!conf.liquidityAvailabilityValid);
    std::cout << "  deltaConsistencyValid default: false - OK" << std::endl;
    std::cout << "  deltaStrengthValid default: false - OK" << std::endl;
    std::cout << "  liquidityAvailabilityValid default: false - OK" << std::endl;

    // With all defaults (all invalid), scoreValid should be FALSE
    // This is the key NO-FALLBACK POLICY test: all-invalid = explicit invalid score
    ConfidenceWeights w;
    ScoreResult result = conf.calculate_score(w);
    assert(!result.scoreValid);  // Must be invalid, not 0.0f sentinel
    std::cout << "  All invalid -> scoreValid=false - OK" << std::endl;

    PASS();
}

//------------------------------------------------------------------------------
// Main
//------------------------------------------------------------------------------
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Baseline Readiness Tests (Stage 3)" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    test_confidence_validity_flags_exist();
    test_calculate_score_excludes_invalid();
    test_facilitation_unknown_exists();
    test_validity_propagation_pattern();
    test_checkExtremes_readiness_flag();
    test_no_fallback_when_invalid();
    test_unimplemented_metrics_default_invalid();

    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "All Stage 3 tests PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
