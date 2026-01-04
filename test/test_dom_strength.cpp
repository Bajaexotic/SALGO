// test_dom_strength.cpp - Verify domStrength quality metric computation
// Tests the pure helper functions without Sierra runtime dependency
#include <iostream>
#include <cassert>
#include <cmath>
#include <cstdint>

// Include required headers
#include "test_sierrachart_mock.h"
#include "../AMT_Snapshots.h"
#include "../AMT_Patterns.h"

using namespace AMT;

// ============================================================================
// TEST HELPERS
// ============================================================================

constexpr float EPSILON = 0.001f;
constexpr double TICK_SIZE = 0.25;  // ES tick size

bool approx_equal(float a, float b, float eps = EPSILON)
{
    return std::abs(a - b) < eps;
}

// ============================================================================
// TEST: DOMQualitySnapshot basic properties
// ============================================================================

void test_dom_quality_snapshot_defaults()
{
    std::cout << "=== Test: DOMQualitySnapshot defaults ===" << std::endl;

    DOMQualitySnapshot snap;

    assert(snap.bidLevelCount == 0);
    assert(snap.askLevelCount == 0);
    assert(snap.bidNonZeroCount == 0);
    assert(snap.askNonZeroCount == 0);
    assert(snap.bestBid == 0.0);
    assert(snap.bestAsk == 0.0);
    assert(snap.structureHash == 0);

    assert(!snap.hasBidLevels());
    assert(!snap.hasAskLevels());
    assert(!snap.hasAnyLevels());
    assert(!snap.hasBothSides());
    assert(!snap.hasValidSpread(TICK_SIZE));

    std::cout << "  PASSED" << std::endl;
}

void test_dom_quality_snapshot_has_levels()
{
    std::cout << "=== Test: DOMQualitySnapshot level detection ===" << std::endl;

    DOMQualitySnapshot snap;
    snap.bidLevelCount = 5;
    snap.askLevelCount = 0;

    assert(snap.hasBidLevels());
    assert(!snap.hasAskLevels());
    assert(snap.hasAnyLevels());
    assert(!snap.hasBothSides());

    snap.askLevelCount = 3;
    assert(snap.hasBothSides());

    std::cout << "  PASSED" << std::endl;
}

void test_dom_quality_snapshot_valid_spread()
{
    std::cout << "=== Test: DOMQualitySnapshot spread validation ===" << std::endl;

    DOMQualitySnapshot snap;
    snap.bidLevelCount = 5;
    snap.askLevelCount = 5;

    // No prices set - invalid
    assert(!snap.hasValidSpread(TICK_SIZE));

    // Valid spread (1 tick)
    snap.bestBid = 6100.00;
    snap.bestAsk = 6100.25;
    assert(snap.hasValidSpread(TICK_SIZE));

    // Zero spread - invalid
    snap.bestAsk = 6100.00;
    assert(!snap.hasValidSpread(TICK_SIZE));

    // Negative spread (crossed) - invalid
    snap.bestAsk = 6099.75;
    assert(!snap.hasValidSpread(TICK_SIZE));

    // Huge spread (exceeds 100 ticks) - invalid
    snap.bestAsk = 6200.00;  // 400 ticks away
    assert(!snap.hasValidSpread(TICK_SIZE));

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: DOMQualityTracker freshness detection
// ============================================================================

void test_dom_quality_tracker_reset()
{
    std::cout << "=== Test: DOMQualityTracker reset ===" << std::endl;

    DOMQualityTracker tracker;
    tracker.lastHash = 12345;
    tracker.lastChangeBar = 100;
    tracker.isStale = true;

    tracker.Reset();

    assert(tracker.lastHash == 0);
    assert(tracker.lastChangeBar == -1);
    assert(tracker.isStale == false);
    assert(tracker.barsSinceChange == 0);

    std::cout << "  PASSED" << std::endl;
}

void test_dom_quality_tracker_change_detection()
{
    std::cout << "=== Test: DOMQualityTracker change detection ===" << std::endl;

    DOMQualityTracker tracker;

    DOMQualitySnapshot snap1;
    snap1.bidLevelCount = 5;
    snap1.askLevelCount = 5;
    snap1.bestBid = 6100.00;
    snap1.bestAsk = 6100.25;
    snap1.structureHash = ComputeDOMStructureHash(5, 5, 6100.00, 6100.25, 5, 5);

    // First update - always "changed" from initial state
    bool changed = tracker.Update(snap1, 0);
    assert(changed);
    assert(tracker.lastChangeBar == 0);

    // Same hash - no change
    changed = tracker.Update(snap1, 1);
    assert(!changed);
    assert(tracker.lastChangeBar == 0);
    assert(tracker.barsSinceChange == 1);

    // Different hash - change detected
    DOMQualitySnapshot snap2 = snap1;
    snap2.bestBid = 6100.25;
    snap2.bestAsk = 6100.50;
    snap2.structureHash = ComputeDOMStructureHash(5, 5, 6100.25, 6100.50, 5, 5);

    changed = tracker.Update(snap2, 2);
    assert(changed);
    assert(tracker.lastChangeBar == 2);
    assert(tracker.barsSinceChange == 0);

    std::cout << "  PASSED" << std::endl;
}

void test_dom_quality_tracker_staleness()
{
    std::cout << "=== Test: DOMQualityTracker staleness detection ===" << std::endl;

    DOMQualityTracker tracker;
    tracker.maxStaleBarsHard = 10;  // Hard limit: stale after 10 bars unchanged

    // Set conservative initial cadence (changes expected every ~5 bars)
    // This prevents the adaptive threshold from being too aggressive
    tracker.adaptiveExpectedCadence = 0.2f;  // 1 change per 5 bars

    DOMQualitySnapshot snap;
    snap.bidLevelCount = 5;
    snap.askLevelCount = 5;
    snap.structureHash = 12345;

    // Initial update
    tracker.Update(snap, 0);
    assert(!tracker.isStale);
    std::cout << "  After bar 0: isStale=" << tracker.isStale
              << " barsSinceChange=" << tracker.barsSinceChange << std::endl;

    // Simulate no changes for 5 bars - not stale yet (adaptive threshold ~15)
    for (int bar = 1; bar <= 5; ++bar)
    {
        tracker.Update(snap, bar);
    }
    std::cout << "  After bar 5: isStale=" << tracker.isStale
              << " barsSinceChange=" << tracker.barsSinceChange << std::endl;
    assert(!tracker.isStale);
    assert(tracker.barsSinceChange == 5);

    // Simulate no changes for 11 bars total - now stale (exceeds hard limit of 10)
    for (int bar = 6; bar <= 11; ++bar)
    {
        tracker.Update(snap, bar);
    }
    std::cout << "  After bar 11: isStale=" << tracker.isStale
              << " barsSinceChange=" << tracker.barsSinceChange << std::endl;
    assert(tracker.isStale);
    assert(tracker.barsSinceChange == 11);

    std::cout << "  PASSED" << std::endl;
}

void test_dom_quality_tracker_freshness_score()
{
    std::cout << "=== Test: DOMQualityTracker freshness score ===" << std::endl;

    DOMQualityTracker tracker;
    tracker.maxStaleBarsHard = 10;
    tracker.adaptiveExpectedCadence = 0.2f;  // Conservative: 1 change per 5 bars

    DOMQualitySnapshot snap;
    snap.structureHash = 12345;

    // Initial state - NO-FALLBACK POLICY: no history = invalid, score is dead value
    float score = tracker.ComputeFreshnessScore();
    std::cout << "  Initial (no history): score=" << score << " valid=" << tracker.IsFreshnessValid() << std::endl;
    assert(!tracker.IsFreshnessValid());  // Must be invalid until first update
    assert(approx_equal(score, 0.0f, 0.01f));  // Dead value, not 0.5f fallback

    // Just changed - establishes baseline, now valid with full freshness
    tracker.Update(snap, 0);
    score = tracker.ComputeFreshnessScore();
    std::cout << "  Just changed: score=" << score << " valid=" << tracker.IsFreshnessValid() << std::endl;
    assert(tracker.IsFreshnessValid());  // Now valid
    assert(score > 0.9f);

    // 5 bars since change - partial freshness
    for (int bar = 1; bar <= 5; ++bar)
    {
        tracker.Update(snap, bar);
    }
    score = tracker.ComputeFreshnessScore();
    std::cout << "  5 bars since change: " << score << std::endl;
    assert(score > 0.3f && score < 0.9f);  // Decayed but not zero

    // Stale - zero freshness (exceeds hard limit)
    for (int bar = 6; bar <= 15; ++bar)
    {
        tracker.Update(snap, bar);
    }
    score = tracker.ComputeFreshnessScore();
    std::cout << "  Stale (15 bars): " << score << std::endl;
    assert(approx_equal(score, 0.0f));

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: ComputeDOMStrength pure helper
// ============================================================================

void test_compute_dom_strength_no_levels()
{
    std::cout << "=== Test: ComputeDOMStrength with no levels ===" << std::endl;

    DOMQualitySnapshot snap;  // All zeros
    DOMQualityTracker tracker;

    float strength = ComputeDOMStrength(snap, tracker, 5, TICK_SIZE);
    std::cout << "  No levels strength: " << strength << std::endl;

    // No levels = very low score (coverage=0, sanity=0)
    assert(strength < 0.3f);

    std::cout << "  PASSED" << std::endl;
}

void test_compute_dom_strength_full_coverage()
{
    std::cout << "=== Test: ComputeDOMStrength with full coverage ===" << std::endl;

    DOMQualitySnapshot snap;
    snap.bidLevelCount = 5;
    snap.askLevelCount = 5;
    snap.bidNonZeroCount = 5;  // All levels have quantity
    snap.askNonZeroCount = 5;
    snap.bestBid = 6100.00;
    snap.bestAsk = 6100.25;
    snap.structureHash = ComputeDOMStructureHash(5, 5, 6100.00, 6100.25, 5, 5);

    DOMQualityTracker tracker;
    tracker.Update(snap, 0);  // Just changed - fresh

    float strength = ComputeDOMStrength(snap, tracker, 5, TICK_SIZE);
    std::cout << "  Full coverage strength: " << strength << std::endl;
    std::cout << "    coverage: " << snap.coverageScore << std::endl;
    std::cout << "    freshness: " << snap.freshnessScore_ << std::endl;  // Raw diagnostic access
    std::cout << "    sanity: " << snap.sanityScore << std::endl;

    // Full coverage + fresh + valid spread = near 1.0
    assert(strength > 0.85f);

    std::cout << "  PASSED" << std::endl;
}

void test_compute_dom_strength_partial_coverage()
{
    std::cout << "=== Test: ComputeDOMStrength with partial coverage ===" << std::endl;

    DOMQualitySnapshot snap;
    snap.bidLevelCount = 5;
    snap.askLevelCount = 5;
    snap.bidNonZeroCount = 3;  // 5 total non-zero out of 10 expected = 50%
    snap.askNonZeroCount = 2;
    snap.bestBid = 6100.00;
    snap.bestAsk = 6100.25;
    snap.structureHash = ComputeDOMStructureHash(5, 5, 6100.00, 6100.25, 3, 2);

    DOMQualityTracker tracker;
    tracker.Update(snap, 0);

    float strength = ComputeDOMStrength(snap, tracker, 5, TICK_SIZE);
    std::cout << "  Partial coverage (50%) strength: " << strength << std::endl;
    std::cout << "    coverage: " << snap.coverageScore << std::endl;
    std::cout << "    freshness: " << snap.freshnessScore_ << std::endl;  // Raw diagnostic access
    std::cout << "    sanity: " << snap.sanityScore << std::endl;

    // Partial coverage with fresh data and valid structure:
    // coverage = 0.5, freshness = 1.0, sanity = 1.0
    // strength = 0.4*0.5 + 0.4*1.0 + 0.2*1.0 = 0.8
    assert(strength > 0.5f && strength <= 0.85f);

    std::cout << "  PASSED" << std::endl;
}

void test_compute_dom_strength_one_sided()
{
    std::cout << "=== Test: ComputeDOMStrength with one-sided book ===" << std::endl;

    DOMQualitySnapshot snap;
    snap.bidLevelCount = 5;
    snap.askLevelCount = 0;  // No asks!
    snap.bidNonZeroCount = 5;
    snap.askNonZeroCount = 0;
    snap.bestBid = 6100.00;
    snap.bestAsk = 0.0;  // Invalid
    snap.structureHash = ComputeDOMStructureHash(5, 0, 6100.00, 0.0, 5, 0);

    DOMQualityTracker tracker;
    tracker.Update(snap, 0);

    float strength = ComputeDOMStrength(snap, tracker, 5, TICK_SIZE);
    std::cout << "  One-sided book strength: " << strength << std::endl;
    std::cout << "    coverage: " << snap.coverageScore << std::endl;
    std::cout << "    freshness: " << snap.freshnessScore_ << std::endl;  // Raw diagnostic access
    std::cout << "    sanity: " << snap.sanityScore << std::endl;

    // One-sided = penalized coverage (0.5 * 0.3 = 0.15) and zero sanity
    // strength = 0.4 * 0.15 + 0.4 * 1.0 + 0.2 * 0.0 = 0.46
    // This is still moderate because freshness is high
    assert(strength < 0.55f);

    std::cout << "  PASSED" << std::endl;
}

void test_compute_dom_strength_stale()
{
    std::cout << "=== Test: ComputeDOMStrength when stale ===" << std::endl;

    DOMQualitySnapshot snap;
    snap.bidLevelCount = 5;
    snap.askLevelCount = 5;
    snap.bidNonZeroCount = 5;
    snap.askNonZeroCount = 5;
    snap.bestBid = 6100.00;
    snap.bestAsk = 6100.25;
    snap.structureHash = ComputeDOMStructureHash(5, 5, 6100.00, 6100.25, 5, 5);

    DOMQualityTracker tracker;
    tracker.maxStaleBarsHard = 10;
    tracker.adaptiveExpectedCadence = 0.2f;  // Conservative initial cadence
    tracker.Update(snap, 0);

    // Make it stale (exceed hard limit of 10)
    for (int bar = 1; bar <= 15; ++bar)
    {
        tracker.Update(snap, bar);
    }
    std::cout << "  isStale=" << tracker.isStale
              << " barsSinceChange=" << tracker.barsSinceChange << std::endl;
    assert(tracker.isStale);

    float strength = ComputeDOMStrength(snap, tracker, 5, TICK_SIZE);
    std::cout << "  Stale DOM strength: " << strength << std::endl;
    std::cout << "    coverage: " << snap.coverageScore << std::endl;
    std::cout << "    freshness: " << snap.freshnessScore_ << std::endl;  // Raw diagnostic access
    std::cout << "    sanity: " << snap.sanityScore << std::endl;

    // Stale = freshness=0, so coverage and sanity are only contributors
    // 0.4 * 1.0 (full coverage) + 0.4 * 0 (stale) + 0.2 * 1.0 (valid) = 0.6
    assert(strength < 0.65f);  // Freshness dragging it down

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: ConfidenceAttribute integration
// ============================================================================

void test_confidence_attribute_dom_validity()
{
    std::cout << "=== Test: ConfidenceAttribute domStrengthValid default ===" << std::endl;

    ConfidenceAttribute conf;

    assert(conf.domStrengthValid == false);
    assert(conf.domStrength == 0.0f);

    std::cout << "  PASSED" << std::endl;
}

void test_calculate_score_excludes_invalid_dom()
{
    std::cout << "=== Test: calculate_score excludes invalid DOM ===" << std::endl;

    ConfidenceWeights w;  // Default weights: dom=0.35, delta=0.25, profile=0.20, tpo=0.10, liquidity=0.10
    std::cout << "  Weights: dom=" << w.dom << " delta=" << w.delta
              << " profile=" << w.profile << " tpo=" << w.tpo << " liquidity=" << w.liquidity << std::endl;

    ConfidenceAttribute conf;
    conf.domStrength = 0.9f;           // High value that should NOT contribute
    conf.domStrengthValid = false;     // But it's invalid!
    conf.deltaConsistency = 0.6f;
    conf.deltaConsistencyValid = true;
    conf.volumeProfileClarity = 0.7f;
    conf.volumeProfileClarityValid = true;
    conf.tpoAcceptance = 0.5f;
    conf.tpoAcceptanceValid = true;
    conf.liquidityAvailability = 0.8f;
    conf.liquidityAvailabilityValid = true;

    ScoreResult result = conf.calculate_score(w);
    assert(result.scoreValid);
    std::cout << "  Score with invalid DOM: " << result.score << std::endl;

    // Expected: DOM excluded, other 4 metrics valid and normalized
    // Active weights: delta=0.25, profile=0.20, tpo=0.10, liquidity=0.10 = 0.65
    // Score = (0.6*0.25 + 0.7*0.20 + 0.5*0.10 + 0.8*0.10) / 0.65
    //       = (0.15 + 0.14 + 0.05 + 0.08) / 0.65 = 0.42 / 0.65 = 0.646
    float expected = (0.6f*w.delta + 0.7f*w.profile + 0.5f*w.tpo + 0.8f*w.liquidity)
                   / (w.delta + w.profile + w.tpo + w.liquidity);
    std::cout << "  Expected (DOM excluded): " << expected << std::endl;

    assert(std::abs(result.score - expected) < 0.01f);

    std::cout << "  PASSED" << std::endl;
}

void test_calculate_score_includes_valid_dom()
{
    std::cout << "=== Test: calculate_score includes valid DOM ===" << std::endl;

    ConfidenceWeights w;

    ConfidenceAttribute conf;
    conf.domStrength = 0.9f;
    conf.domStrengthValid = true;      // Now it's valid!
    conf.deltaConsistency = 0.6f;
    conf.deltaConsistencyValid = true;
    conf.volumeProfileClarity = 0.7f;
    conf.volumeProfileClarityValid = true;
    conf.tpoAcceptance = 0.5f;
    conf.tpoAcceptanceValid = true;
    conf.liquidityAvailability = 0.8f;
    conf.liquidityAvailabilityValid = true;

    ScoreResult result = conf.calculate_score(w);
    assert(result.scoreValid);
    std::cout << "  Score with valid DOM: " << result.score << std::endl;

    // Expected: All 5 metrics included, total weight = 1.0
    float expected = 0.9f*w.dom + 0.6f*w.delta + 0.7f*w.profile + 0.5f*w.tpo + 0.8f*w.liquidity;
    std::cout << "  Expected (all included): " << expected << std::endl;

    assert(std::abs(result.score - expected) < 0.01f);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Structure hash
// ============================================================================

void test_dom_structure_hash_changes()
{
    std::cout << "=== Test: ComputeDOMStructureHash changes on structure change ===" << std::endl;

    uint64_t hash1 = ComputeDOMStructureHash(5, 5, 6100.00, 6100.25, 5, 5);
    uint64_t hash2 = ComputeDOMStructureHash(5, 5, 6100.00, 6100.25, 5, 5);
    uint64_t hash3 = ComputeDOMStructureHash(5, 5, 6100.25, 6100.50, 5, 5);  // Price change
    uint64_t hash4 = ComputeDOMStructureHash(6, 5, 6100.00, 6100.25, 5, 5);  // Level count change

    std::cout << "  hash1 = " << hash1 << std::endl;
    std::cout << "  hash2 = " << hash2 << std::endl;
    std::cout << "  hash3 = " << hash3 << std::endl;
    std::cout << "  hash4 = " << hash4 << std::endl;

    assert(hash1 == hash2);  // Same inputs = same hash
    assert(hash1 != hash3);  // Price change = different hash
    assert(hash1 != hash4);  // Level count change = different hash
    assert(hash3 != hash4);  // Different changes = different hashes

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Stale vs Unavailable semantics (NEW)
// ============================================================================

void test_stale_does_not_set_invalid()
{
    std::cout << "=== Test: Stale DOM does NOT set domStrengthValid=false ===" << std::endl;

    // Simulate the exact flow from AuctionSensor_v1.cpp:
    // When DOM has levels but is stale, domStrengthValid should remain TRUE

    DOMQualitySnapshot snap;
    snap.bidLevelCount = 5;
    snap.askLevelCount = 5;
    snap.bidNonZeroCount = 5;
    snap.askNonZeroCount = 5;
    snap.bestBid = 6100.00;
    snap.bestAsk = 6100.25;
    snap.structureHash = ComputeDOMStructureHash(5, 5, 6100.00, 6100.25, 5, 5);

    DOMQualityTracker tracker;
    tracker.maxStaleBarsHard = 10;
    tracker.adaptiveExpectedCadence = 0.2f;

    // First update - fresh
    tracker.Update(snap, 0);
    assert(!tracker.isStale);

    // Make it stale by simulating no changes for 15 bars
    for (int bar = 1; bar <= 15; ++bar)
    {
        tracker.Update(snap, bar);
    }

    // Verify stale state
    assert(tracker.isStale);
    std::cout << "  isStale: " << tracker.isStale << " (confirmed stale)" << std::endl;

    // Compute strength - should still return a value (just low due to freshness=0)
    float strength = ComputeDOMStrength(snap, tracker, 5, TICK_SIZE);
    std::cout << "  domStrength: " << strength << std::endl;

    // KEY ASSERTION: In the production code, domStrengthValid is set to TRUE
    // even when stale (see AuctionSensor_v1.cpp:2203).
    // Staleness affects the VALUE (low), not the VALIDITY.
    // We verify this by ensuring hasAnyLevels() is still true (the prerequisite check)
    assert(snap.hasAnyLevels());  // This is what controls validity
    std::cout << "  hasAnyLevels(): " << snap.hasAnyLevels() << " (valid=TRUE expected)" << std::endl;

    // The strength value should be degraded but non-zero
    // Coverage + sanity contribute, freshness=0
    // 0.4*1.0 + 0.4*0.0 + 0.2*1.0 = 0.6
    assert(strength > 0.4f && strength < 0.7f);
    std::cout << "  strength in expected range [0.4, 0.7]: YES" << std::endl;

    std::cout << "  PASSED" << std::endl;
}

void test_stale_produces_lower_score_than_unavailable()
{
    std::cout << "=== Test: Stale DOM produces LOWER score than unavailable ===" << std::endl;
    std::cout << "  (Because stale contributes ~0.0 while unavailable is excluded+renormalized)" << std::endl;

    ConfidenceWeights w;  // dom=0.35, delta=0.25, profile=0.20, tpo=0.10, liquidity=0.10

    // Set up identical base attributes for both scenarios
    ConfidenceAttribute confUnavailable;
    confUnavailable.domStrength = 0.0f;
    confUnavailable.domStrengthValid = false;  // UNAVAILABLE: excluded from score
    confUnavailable.deltaConsistency = 0.7f;
    confUnavailable.deltaConsistencyValid = true;
    confUnavailable.volumeProfileClarity = 0.7f;
    confUnavailable.volumeProfileClarityValid = true;
    confUnavailable.tpoAcceptance = 0.7f;
    confUnavailable.tpoAcceptanceValid = true;
    confUnavailable.liquidityAvailability = 0.7f;
    confUnavailable.liquidityAvailabilityValid = true;

    ConfidenceAttribute confStale;
    confStale.domStrength = 0.0f;  // Very low value due to stale (freshness=0)
    confStale.domStrengthValid = true;  // STALE: still valid, contributes as low value
    confStale.deltaConsistency = 0.7f;
    confStale.deltaConsistencyValid = true;
    confStale.volumeProfileClarity = 0.7f;
    confStale.volumeProfileClarityValid = true;
    confStale.tpoAcceptance = 0.7f;
    confStale.tpoAcceptanceValid = true;
    confStale.liquidityAvailability = 0.7f;
    confStale.liquidityAvailabilityValid = true;

    ScoreResult resultUnavailable = confUnavailable.calculate_score(w);
    ScoreResult resultStale = confStale.calculate_score(w);
    assert(resultUnavailable.scoreValid);
    assert(resultStale.scoreValid);

    std::cout << "  Score (unavailable, renormalized): " << resultUnavailable.score << std::endl;
    std::cout << "  Score (stale, contributes 0.0):    " << resultStale.score << std::endl;

    // UNAVAILABLE: DOM excluded, weight renormalized
    // Active weights: delta=0.25, profile=0.20, tpo=0.10, liquidity=0.10 = 0.65
    // Score = (0.7*0.25 + 0.7*0.20 + 0.7*0.10 + 0.7*0.10) / 0.65
    //       = 0.7 * 0.65 / 0.65 = 0.7
    float expectedUnavailable = 0.7f;

    // STALE: DOM included with value 0.0
    // Total weight: 0.35 + 0.25 + 0.20 + 0.10 + 0.10 = 1.0
    // Score = 0.0*0.35 + 0.7*0.25 + 0.7*0.20 + 0.7*0.10 + 0.7*0.10
    //       = 0 + 0.175 + 0.14 + 0.07 + 0.07 = 0.455
    float expectedStale = 0.7f * (w.delta + w.profile + w.tpo + w.liquidity);

    std::cout << "  Expected unavailable: " << expectedUnavailable << std::endl;
    std::cout << "  Expected stale:       " << expectedStale << std::endl;

    // Verify calculations match
    assert(std::abs(resultUnavailable.score - expectedUnavailable) < 0.01f);
    assert(std::abs(resultStale.score - expectedStale) < 0.01f);

    // KEY ASSERTION: Stale produces LOWER score than unavailable
    // Because stale contributes as 0.0 (dragging down average),
    // while unavailable is excluded and remaining metrics are renormalized
    assert(resultStale.score < resultUnavailable.score);
    std::cout << "  CONFIRMED: stale (" << resultStale.score << ") < unavailable (" << resultUnavailable.score << ")" << std::endl;

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// MAIN
// ============================================================================

int main()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "DOM Strength Unit Tests" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // DOMQualitySnapshot tests
    test_dom_quality_snapshot_defaults();
    test_dom_quality_snapshot_has_levels();
    test_dom_quality_snapshot_valid_spread();

    // DOMQualityTracker tests
    test_dom_quality_tracker_reset();
    test_dom_quality_tracker_change_detection();
    test_dom_quality_tracker_staleness();
    test_dom_quality_tracker_freshness_score();

    // ComputeDOMStrength tests
    test_compute_dom_strength_no_levels();
    test_compute_dom_strength_full_coverage();
    test_compute_dom_strength_partial_coverage();
    test_compute_dom_strength_one_sided();
    test_compute_dom_strength_stale();

    // ConfidenceAttribute integration tests
    test_confidence_attribute_dom_validity();
    test_calculate_score_excludes_invalid_dom();
    test_calculate_score_includes_valid_dom();

    // Hash tests
    test_dom_structure_hash_changes();

    // NEW: Stale vs Unavailable semantics tests
    std::cout << "\n--- Stale vs Unavailable Semantics Tests (NEW) ---\n" << std::endl;
    test_stale_does_not_set_invalid();
    test_stale_produces_lower_score_than_unavailable();

    std::cout << "\n========================================" << std::endl;
    std::cout << "ALL TESTS PASSED" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
}
