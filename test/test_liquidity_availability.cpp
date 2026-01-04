// test_liquidity_availability.cpp - Verify liquidityAvailability no-fallback policy
// Tests the baseline readiness gate, validity flag, and calculate_score behavior
#include <iostream>
#include <cassert>
#include <cmath>

// Include required headers
#include "test_sierrachart_mock.h"
#include "../AMT_Patterns.h"
#include "../AMT_Snapshots.h"
#include "../AMT_config.h"

using namespace AMT;

void test_confidence_attribute_validity_default() {
    std::cout << "=== Test: ConfidenceAttribute validity default ===" << std::endl;

    ConfidenceAttribute conf;

    // Default should be invalid (no baseline computed yet)
    std::cout << "  liquidityAvailabilityValid (default): " << conf.liquidityAvailabilityValid << std::endl;
    assert(conf.liquidityAvailabilityValid == false);
    assert(conf.liquidityAvailability == 0.0f);

    std::cout << "  PASSED" << std::endl;
}

void test_calculate_score_with_invalid_liquidity() {
    std::cout << "=== Test: calculate_score excludes invalid liquidity ===" << std::endl;

    ConfidenceWeights w;  // Default weights
    std::cout << "  Weights: dom=" << w.dom << " delta=" << w.delta
              << " profile=" << w.profile << " tpo=" << w.tpo << " liquidity=" << w.liquidity << std::endl;

    ConfidenceAttribute conf;
    conf.domStrength = 0.8f;
    conf.domStrengthValid = true;  // DOM is valid
    conf.deltaConsistency = 0.6f;  // DEPRECATED but kept for compatibility
    conf.deltaSignal.strength = 0.6;  // NEW: Set deltaSignal properly
    conf.deltaSignal.signedProportion = 0.6;
    conf.deltaSignal.reliability = 1.0;
    conf.deltaSignal.isAvailable = true;
    conf.deltaAvailabilityValid = true;  // NEW: Delta IS available
    conf.volumeProfileClarity = 0.7f;
    conf.volumeProfileClarityValid = true;  // NEW: Profile clarity is available
    conf.tpoAcceptance = 0.5f;
    conf.liquidityAvailability = 0.0f;  // This value should NOT matter when invalid
    conf.liquidityAvailabilityValid = false;

    float score = conf.calculate_score(w);
    std::cout << "  Score with invalid liquidity: " << score << std::endl;

    // Expected: (0.8*0.35 + 0.6*0.25 + 0.7*0.20 + 0.5*0.10) / (0.35+0.25+0.20+0.10)
    // Note: deltaSignal.strength is used now, not deltaConsistency
    // = (0.28 + 0.15 + 0.14 + 0.05) / 0.90 = 0.62 / 0.90 = 0.6889
    float expected = (0.8f*w.dom + 0.6f*w.delta + 0.7f*w.profile + 0.5f*w.tpo)
                   / (w.dom + w.delta + w.profile + w.tpo);
    std::cout << "  Expected (normalized without liquidity): " << expected << std::endl;

    assert(std::abs(score - expected) < 0.001f);

    std::cout << "  PASSED" << std::endl;
}

void test_calculate_score_with_valid_liquidity() {
    std::cout << "=== Test: calculate_score includes valid liquidity ===" << std::endl;

    ConfidenceWeights w;  // Default weights

    ConfidenceAttribute conf;
    conf.domStrength = 0.8f;
    conf.domStrengthValid = true;
    conf.deltaConsistency = 0.6f;  // DEPRECATED
    conf.deltaSignal.strength = 0.6;
    conf.deltaSignal.signedProportion = 0.6;
    conf.deltaSignal.reliability = 1.0;
    conf.deltaSignal.isAvailable = true;
    conf.deltaAvailabilityValid = true;
    conf.volumeProfileClarity = 0.7f;
    conf.volumeProfileClarityValid = true;
    conf.tpoAcceptance = 0.5f;
    conf.liquidityAvailability = 0.9f;  // High liquidity
    conf.liquidityAvailabilityValid = true;

    float score = conf.calculate_score(w);
    std::cout << "  Score with valid liquidity: " << score << std::endl;

    // Expected: (0.8*0.35 + 0.6*0.25 + 0.7*0.20 + 0.5*0.10 + 0.9*0.10) / (0.35+0.25+0.20+0.10+0.10)
    // = (0.28 + 0.15 + 0.14 + 0.05 + 0.09) / 1.0 = 0.71
    float expected = (0.8f*w.dom + 0.6f*w.delta + 0.7f*w.profile + 0.5f*w.tpo + 0.9f*w.liquidity)
                   / (w.dom + w.delta + w.profile + w.tpo + w.liquidity);
    std::cout << "  Expected (with liquidity): " << expected << std::endl;

    assert(std::abs(score - expected) < 0.001f);

    std::cout << "  PASSED" << std::endl;
}

void test_baseline_minimum_samples_config() {
    std::cout << "=== Test: ZoneConfig liquidityBaselineMinSamples ===" << std::endl;

    ZoneConfig cfg;
    std::cout << "  liquidityBaselineMinSamples (default): " << cfg.liquidityBaselineMinSamples << std::endl;

    assert(cfg.liquidityBaselineMinSamples == 10);
    assert(cfg.liquidityBaselineMinSamples > 0);

    std::cout << "  PASSED" << std::endl;
}

void test_baseline_sample_count_gate() {
    std::cout << "=== Test: Baseline sample count gate ===" << std::endl;

    ZoneConfig cfg;
    BaselineEngine be;
    be.reset(300);

    // Case 1: Empty baseline - should be insufficient
    size_t samples = be.depth_mass_core.size();
    bool baselineAvailable = (samples >= static_cast<size_t>(cfg.liquidityBaselineMinSamples));
    std::cout << "  Empty baseline: samples=" << samples << ", available=" << baselineAvailable << std::endl;
    assert(samples == 0);
    assert(baselineAvailable == false);

    // Case 2: Partial baseline (< minSamples) - should be insufficient
    for (int i = 0; i < 5; i++) {
        be.depth_mass_core.push(100.0 + i);
    }
    samples = be.depth_mass_core.size();
    baselineAvailable = (samples >= static_cast<size_t>(cfg.liquidityBaselineMinSamples));
    std::cout << "  Partial baseline: samples=" << samples << ", available=" << baselineAvailable << std::endl;
    assert(samples == 5);
    assert(baselineAvailable == false);

    // Case 3: Sufficient baseline (>= minSamples) - should be available
    for (int i = 0; i < 10; i++) {
        be.depth_mass_core.push(100.0 + i);
    }
    samples = be.depth_mass_core.size();
    baselineAvailable = (samples >= static_cast<size_t>(cfg.liquidityBaselineMinSamples));
    std::cout << "  Sufficient baseline: samples=" << samples << ", available=" << baselineAvailable << std::endl;
    assert(samples == 15);
    assert(baselineAvailable == true);

    std::cout << "  PASSED" << std::endl;
}

void test_normalized_liquidity_calculation() {
    std::cout << "=== Test: Normalized liquidity calculation ===" << std::endl;

    BaselineEngine be;
    be.reset(300);

    // Populate with consistent values
    for (int i = 0; i < 20; i++) {
        be.depth_mass_core.push(500.0);  // baseline median = 500
    }

    double baselineDepth = be.depth_mass_core.median();
    std::cout << "  Baseline median: " << baselineDepth << std::endl;
    assert(std::abs(baselineDepth - 500.0) < 0.001);

    // Test cases: curDepth -> normalizedLiq
    // Formula: min(1.0, curDepth / (baselineDepth * 2.0))

    // Case 1: curDepth = baseline (500) -> 500/1000 = 0.5
    double curDepth1 = 500.0;
    double liq1 = std::min(1.0, curDepth1 / (baselineDepth * 2.0));
    std::cout << "  curDepth=" << curDepth1 << " -> liq=" << liq1 << " (expected 0.5)" << std::endl;
    assert(std::abs(liq1 - 0.5) < 0.001);

    // Case 2: curDepth = 2x baseline (1000) -> 1000/1000 = 1.0
    double curDepth2 = 1000.0;
    double liq2 = std::min(1.0, curDepth2 / (baselineDepth * 2.0));
    std::cout << "  curDepth=" << curDepth2 << " -> liq=" << liq2 << " (expected 1.0)" << std::endl;
    assert(std::abs(liq2 - 1.0) < 0.001);

    // Case 3: curDepth = 3x baseline (1500) -> capped at 1.0
    double curDepth3 = 1500.0;
    double liq3 = std::min(1.0, curDepth3 / (baselineDepth * 2.0));
    std::cout << "  curDepth=" << curDepth3 << " -> liq=" << liq3 << " (expected 1.0, capped)" << std::endl;
    assert(std::abs(liq3 - 1.0) < 0.001);

    // Case 4: curDepth = 0.5x baseline (250) -> 250/1000 = 0.25
    double curDepth4 = 250.0;
    double liq4 = std::min(1.0, curDepth4 / (baselineDepth * 2.0));
    std::cout << "  curDepth=" << curDepth4 << " -> liq=" << liq4 << " (expected 0.25)" << std::endl;
    assert(std::abs(liq4 - 0.25) < 0.001);

    // Case 5: curDepth = 0 -> 0/1000 = 0.0
    double curDepth5 = 0.0;
    double liq5 = std::min(1.0, curDepth5 / (baselineDepth * 2.0));
    std::cout << "  curDepth=" << curDepth5 << " -> liq=" << liq5 << " (expected 0.0)" << std::endl;
    assert(std::abs(liq5 - 0.0) < 0.001);

    std::cout << "  PASSED" << std::endl;
}

void test_rolling_dist_size_vs_mean() {
    std::cout << "=== Test: RollingDist.size() vs mean() for empty check ===" << std::endl;

    RollingDist rd;
    rd.reset(100);

    // Empty case: size() should be 0, mean() returns 1.0 (the bug we're fixing)
    std::cout << "  Empty: size()=" << rd.size() << ", mean()=" << rd.mean() << std::endl;
    assert(rd.size() == 0);
    assert(rd.mean() == 1.0);  // This is why we can't use mean() > 0 as gate!

    // Single value: size() = 1, mean() = that value
    rd.push(500.0);
    std::cout << "  After 1 push: size()=" << rd.size() << ", mean()=" << rd.mean() << std::endl;
    assert(rd.size() == 1);
    assert(std::abs(rd.mean() - 500.0) < 0.001);

    std::cout << "  PASSED" << std::endl;
    std::cout << "  NOTE: This confirms why we use size() >= minSamples, NOT mean() > 0" << std::endl;
}

// ============================================================================
// SCORING INTEGRITY: Renormalization Edge Cases
// ============================================================================

void test_score_renormalization_comparison() {
    std::cout << "=== Test: Score renormalization vs prior behavior ===" << std::endl;

    ConfidenceWeights w;  // Default weights: dom=0.35, delta=0.25, profile=0.20, tpo=0.10, liquidity=0.10

    // Scenario: All other metrics at 0.7
    ConfidenceAttribute confValid;
    confValid.domStrength = 0.7f;
    confValid.domStrengthValid = true;
    confValid.deltaConsistency = 0.7f;
    confValid.deltaSignal.strength = 0.7;
    confValid.deltaAvailabilityValid = true;
    confValid.volumeProfileClarity = 0.7f;
    confValid.volumeProfileClarityValid = true;
    confValid.tpoAcceptance = 0.7f;
    confValid.liquidityAvailability = 0.7f;
    confValid.liquidityAvailabilityValid = true;

    ConfidenceAttribute confInvalid;
    confInvalid.domStrength = 0.7f;
    confInvalid.domStrengthValid = true;
    confInvalid.deltaConsistency = 0.7f;
    confInvalid.deltaSignal.strength = 0.7;
    confInvalid.deltaAvailabilityValid = true;
    confInvalid.volumeProfileClarity = 0.7f;
    confInvalid.volumeProfileClarityValid = true;
    confInvalid.tpoAcceptance = 0.7f;
    confInvalid.liquidityAvailability = 0.0f;  // Invalid - will be excluded
    confInvalid.liquidityAvailabilityValid = false;

    float scoreValid = confValid.calculate_score(w);
    float scoreInvalid = confInvalid.calculate_score(w);

    std::cout << "  All metrics at 0.7:" << std::endl;
    std::cout << "    Valid liquidity score: " << scoreValid << " (expected: 0.7)" << std::endl;
    std::cout << "    Invalid liquidity score: " << scoreInvalid << " (expected: 0.7, renormalized)" << std::endl;

    // Both should be 0.7 because renormalization preserves the "true" score
    // when all other metrics are equal
    assert(std::abs(scoreValid - 0.7f) < 0.001f);
    assert(std::abs(scoreInvalid - 0.7f) < 0.001f);

    std::cout << "  PASSED - Renormalization preserves score when metrics are uniform" << std::endl;
}

void test_score_no_divide_by_zero() {
    std::cout << "=== Test: No divide-by-zero when all metrics zero weight ===" << std::endl;

    // Edge case: what if we had a scenario where all weights could be zero?
    // This shouldn't happen with default weights, but test the guard
    ConfidenceWeights w;
    w.dom = 0.0f;
    w.delta = 0.0f;
    w.profile = 0.0f;
    w.tpo = 0.0f;
    w.liquidity = 0.0f;

    ConfidenceAttribute conf;
    conf.liquidityAvailabilityValid = false;

    float score = conf.calculate_score(w);
    std::cout << "  Score with all zero weights: " << score << " (expected: 0.0, not NaN)" << std::endl;

    // Should return 0, not NaN or crash
    assert(score == 0.0f);
    assert(!std::isnan(score));

    std::cout << "  PASSED" << std::endl;
}

void test_score_with_extreme_values() {
    std::cout << "=== Test: Score with extreme values ===" << std::endl;

    ConfidenceWeights w;

    // Scenario: Perfect scores everywhere
    ConfidenceAttribute confPerfect;
    confPerfect.domStrength = 1.0f;
    confPerfect.domStrengthValid = true;
    confPerfect.deltaConsistency = 1.0f;
    confPerfect.deltaSignal.strength = 1.0;
    confPerfect.deltaAvailabilityValid = true;
    confPerfect.volumeProfileClarity = 1.0f;
    confPerfect.volumeProfileClarityValid = true;
    confPerfect.tpoAcceptance = 1.0f;
    confPerfect.liquidityAvailability = 1.0f;
    confPerfect.liquidityAvailabilityValid = true;

    float scorePerfect = confPerfect.calculate_score(w);
    std::cout << "  Perfect valid score: " << scorePerfect << " (expected: 1.0)" << std::endl;
    assert(std::abs(scorePerfect - 1.0f) < 0.001f);

    // Scenario: Perfect scores but liquidity invalid
    confPerfect.liquidityAvailabilityValid = false;
    float scorePerfectNoLiq = confPerfect.calculate_score(w);
    std::cout << "  Perfect invalid-liq score: " << scorePerfectNoLiq << " (expected: 1.0, renormalized)" << std::endl;
    assert(std::abs(scorePerfectNoLiq - 1.0f) < 0.001f);

    // Scenario: Zero scores everywhere (with all validity flags set to include metrics)
    ConfidenceAttribute confZero;
    confZero.domStrengthValid = true;
    confZero.deltaAvailabilityValid = true;
    confZero.liquidityAvailabilityValid = true;
    float scoreZero = confZero.calculate_score(w);
    std::cout << "  Zero valid score: " << scoreZero << " (expected: 0.0)" << std::endl;
    assert(scoreZero == 0.0f);

    std::cout << "  PASSED" << std::endl;
}

void test_score_transition_invalid_to_valid() {
    std::cout << "=== Test: Score transition when liquidity becomes valid ===" << std::endl;

    ConfidenceWeights w;

    ConfidenceAttribute conf;
    conf.domStrength = 0.8f;
    conf.domStrengthValid = true;
    conf.deltaConsistency = 0.6f;  // DEPRECATED
    conf.deltaSignal.strength = 0.6;
    conf.deltaSignal.signedProportion = 0.6;
    conf.deltaSignal.reliability = 1.0;
    conf.deltaSignal.isAvailable = true;
    conf.deltaAvailabilityValid = true;
    conf.volumeProfileClarity = 0.7f;
    conf.volumeProfileClarityValid = true;
    conf.tpoAcceptance = 0.5f;

    // Start invalid
    conf.liquidityAvailability = 0.0f;
    conf.liquidityAvailabilityValid = false;
    float scoreBefore = conf.calculate_score(w);

    // Transition to valid with low liquidity
    conf.liquidityAvailability = 0.3f;  // Low but valid
    conf.liquidityAvailabilityValid = true;
    float scoreAfterLow = conf.calculate_score(w);

    // Transition to valid with high liquidity
    conf.liquidityAvailability = 0.9f;  // High
    float scoreAfterHigh = conf.calculate_score(w);

    std::cout << "  Score before (invalid): " << scoreBefore << std::endl;
    std::cout << "  Score after (valid, low=0.3): " << scoreAfterLow << std::endl;
    std::cout << "  Score after (valid, high=0.9): " << scoreAfterHigh << std::endl;

    // The renormalized score (before) should be between the two valid cases
    // since it's effectively the "true" performance without liquidity contributing
    std::cout << "  Verifying: low < renormalized < high? "
              << (scoreAfterLow < scoreBefore && scoreBefore < scoreAfterHigh ? "YES" : "NO") << std::endl;

    // Actually, with renormalization the invalid score represents the true performance
    // of the other metrics. Adding low liquidity will drag it down, high will pull up.
    // So: scoreAfterLow < scoreBefore && scoreBefore < scoreAfterHigh
    // ... but only if liquidity weight is significant enough

    // For w.liquidity = 0.10:
    // scoreBefore = 0.6889 (from earlier test)
    // scoreAfterLow = (0.28 + 0.15 + 0.14 + 0.05 + 0.03) / 1.0 = 0.65
    // scoreAfterHigh = (0.28 + 0.15 + 0.14 + 0.05 + 0.09) / 1.0 = 0.71

    std::cout << "  Expected: low(~0.65) < invalid(~0.69) < high(~0.71)" << std::endl;
    assert(scoreAfterLow < scoreBefore);
    assert(scoreBefore < scoreAfterHigh);

    std::cout << "  PASSED - Transition behavior is correct" << std::endl;
}

void test_delta_unavailable_renormalization() {
    std::cout << "=== Test: Delta unavailable triggers renormalization (new feature) ===" << std::endl;

    ConfidenceWeights w;

    // Case 1: Both delta and liquidity available
    ConfidenceAttribute confBothValid;
    confBothValid.domStrength = 0.8f;
    confBothValid.domStrengthValid = true;
    confBothValid.deltaSignal.strength = 0.6;
    confBothValid.deltaSignal.isAvailable = true;
    confBothValid.deltaAvailabilityValid = true;
    confBothValid.volumeProfileClarity = 0.7f;
    confBothValid.volumeProfileClarityValid = true;
    confBothValid.tpoAcceptance = 0.5f;
    confBothValid.liquidityAvailability = 0.9f;
    confBothValid.liquidityAvailabilityValid = true;

    float scoreBothValid = confBothValid.calculate_score(w);
    std::cout << "  Both valid score: " << scoreBothValid << std::endl;

    // Case 2: Delta unavailable, liquidity valid
    ConfidenceAttribute confDeltaInvalid;
    confDeltaInvalid.domStrength = 0.8f;
    confDeltaInvalid.domStrengthValid = true;
    confDeltaInvalid.deltaSignal.strength = 0.6;  // High strength but...
    confDeltaInvalid.deltaSignal.isAvailable = false;  // ...not trusted
    confDeltaInvalid.deltaAvailabilityValid = false;  // Thin bar
    confDeltaInvalid.volumeProfileClarity = 0.7f;
    confDeltaInvalid.volumeProfileClarityValid = true;
    confDeltaInvalid.tpoAcceptance = 0.5f;
    confDeltaInvalid.liquidityAvailability = 0.9f;
    confDeltaInvalid.liquidityAvailabilityValid = true;

    float scoreDeltaInvalid = confDeltaInvalid.calculate_score(w);
    std::cout << "  Delta invalid score: " << scoreDeltaInvalid << std::endl;

    // Expected for delta invalid:
    // (0.8*0.35 + 0.7*0.20 + 0.5*0.10 + 0.9*0.10) / (0.35 + 0.20 + 0.10 + 0.10)
    // = (0.28 + 0.14 + 0.05 + 0.09) / 0.75 = 0.56 / 0.75 = 0.7467
    float expectedDeltaInvalid = (0.8f*w.dom + 0.7f*w.profile + 0.5f*w.tpo + 0.9f*w.liquidity)
                                / (w.dom + w.profile + w.tpo + w.liquidity);
    std::cout << "  Expected delta invalid: " << expectedDeltaInvalid << std::endl;

    assert(std::abs(scoreDeltaInvalid - expectedDeltaInvalid) < 0.001f);

    // Verify: delta invalid score should be different from both-valid score
    // because delta is excluded from calculation
    std::cout << "  Scores differ (expected): " << (std::abs(scoreBothValid - scoreDeltaInvalid) > 0.01 ? "YES" : "NO") << std::endl;
    assert(std::abs(scoreBothValid - scoreDeltaInvalid) > 0.01f);

    std::cout << "  PASSED - Delta unavailable renormalizes correctly" << std::endl;
}

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Liquidity Availability Tests" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // Core functionality tests
    test_confidence_attribute_validity_default();
    test_calculate_score_with_invalid_liquidity();
    test_calculate_score_with_valid_liquidity();
    test_baseline_minimum_samples_config();
    test_baseline_sample_count_gate();
    test_normalized_liquidity_calculation();
    test_rolling_dist_size_vs_mean();

    // Scoring integrity / renormalization edge cases
    std::cout << "\n--- Scoring Integrity Tests ---\n" << std::endl;
    test_score_renormalization_comparison();
    test_score_no_divide_by_zero();
    test_score_with_extreme_values();
    test_score_transition_invalid_to_valid();

    // NEW: Delta availability renormalization tests
    std::cout << "\n--- Delta Availability Tests (New Feature) ---\n" << std::endl;
    test_delta_unavailable_renormalization();

    std::cout << "\n========================================" << std::endl;
    std::cout << "All tests PASSED!" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
}
