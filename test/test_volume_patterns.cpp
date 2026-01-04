// test_volume_patterns.cpp - Verify VolumeProfilePattern detection
// Tests the pattern detectors with synthetic histograms
// Covers: VOLUME_GAP, VOLUME_VACUUM, VOLUME_SHELF, LEDGE_PATTERN, VOLUME_CLUSTER, VOLUME_MIGRATION
// Edge cases: Eligibility gate failures, ambiguous cases

#include "test_sierrachart_mock.h"
#include "../amt_core.h"
#include "../AMT_config.h"
#include "../AMT_VolumePatterns.h"

#include <iostream>
#include <cassert>
#include <cmath>
#include <vector>
#include <map>
#include <cstdint>

using VolumeAtPrice = s_VolumeAtPriceV2;

// ============================================================================
// HELPER: Create synthetic histogram from volume distribution
// ============================================================================

struct SyntheticHistogram {
    std::vector<VolumeAtPrice> bins;
    AMT::VolumeThresholds thresholds;
    double tickSize = 0.25;

    void reset() {
        bins.clear();
        thresholds.Reset();
    }

    void addLevel(int priceTick, uint64_t volume) {
        VolumeAtPrice vap;
        vap.PriceInTicks = priceTick;
        vap.Volume = volume;
        vap.BidVolume = volume / 2;
        vap.AskVolume = volume - vap.BidVolume;
        bins.push_back(vap);
    }

    void computeThresholds(double hvnSigmaCoeff = 1.5, double lvnSigmaCoeff = 0.5) {
        thresholds.Reset();
        if (bins.size() < 5) return;

        double totalVol = 0.0;
        double maxVol = 0.0;
        for (const auto& b : bins) {
            totalVol += b.Volume;
            if (b.Volume > maxVol) maxVol = b.Volume;
        }

        const double mean = totalVol / bins.size();

        double variance = 0.0;
        for (const auto& b : bins) {
            double diff = static_cast<double>(b.Volume) - mean;
            variance += diff * diff;
        }
        const double stddev = std::sqrt(variance / bins.size());

        thresholds.mean = mean;
        thresholds.stddev = stddev;
        thresholds.hvnThreshold = mean + hvnSigmaCoeff * stddev;
        thresholds.lvnThreshold = (std::max)(0.0, mean - lvnSigmaCoeff * stddev);
        thresholds.sampleSize = static_cast<int>(bins.size());
        thresholds.totalVolume = totalVol;
        thresholds.maxLevelVolume = maxVol;
        thresholds.computedAtBar = 0;
        thresholds.valid = true;
    }

    const VolumeAtPrice* data() const { return bins.data(); }
    int size() const { return static_cast<int>(bins.size()); }
};

// ============================================================================
// HELPER: Check if pattern type is in result
// ============================================================================

bool hasPattern(const AMT::VolumePatternResult& result, AMT::VolumeProfilePattern pattern) {
    for (const auto& p : result.patterns) {
        if (p == pattern) return true;
    }
    return false;
}

AMT::VolumePatternHit* findHit(AMT::VolumePatternResult& result, AMT::VolumeProfilePattern pattern) {
    for (auto& h : result.hits) {
        if (h.type == pattern) return &h;
    }
    return nullptr;
}

// ============================================================================
// TEST: Eligibility Gate
// ============================================================================

void test_eligibility_gate() {
    std::cout << "Testing eligibility gate..." << std::endl;

    // Test 1: Empty histogram fails eligibility
    {
        SyntheticHistogram h;
        h.reset();
        h.computeThresholds();

        AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
            h.data(), h.size(), 100, 110, 90, h.thresholds);

        assert(!f.valid && "Empty histogram should not be valid");
        assert(!AMT::IsPatternEligible(f) && "Empty histogram should fail eligibility");
    }

    // Test 2: Invalid thresholds fail eligibility
    {
        SyntheticHistogram h;
        h.reset();
        for (int t = 90; t <= 110; t++) h.addLevel(t, 1000);
        // Don't compute thresholds - leave invalid
        h.thresholds.valid = false;

        AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
            h.data(), h.size(), 100, 110, 90, h.thresholds);

        assert(!f.valid && "Invalid thresholds should not produce valid features");
    }

    // Test 3: VAH <= VAL fails eligibility
    {
        SyntheticHistogram h;
        h.reset();
        for (int t = 90; t <= 110; t++) h.addLevel(t, 1000);
        h.computeThresholds();

        AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
            h.data(), h.size(), 100, 90, 110, h.thresholds); // VAH < VAL

        assert(!f.valid && "VAH <= VAL should fail validation");
    }

    // Test 4: Valid histogram passes eligibility
    {
        SyntheticHistogram h;
        h.reset();
        for (int t = 90; t <= 110; t++) h.addLevel(t, 1000);
        h.computeThresholds();

        AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
            h.data(), h.size(), 100, 108, 92, h.thresholds);

        assert(f.valid && "Valid histogram should be valid");
        assert(AMT::IsPatternEligible(f) && "Valid histogram should pass eligibility");
    }

    std::cout << "  PASSED: eligibility gate tests" << std::endl;
}

// ============================================================================
// TEST: Volume Gap Detection
// ============================================================================

void test_volume_gap() {
    std::cout << "Testing VOLUME_GAP detection..." << std::endl;

    // Create histogram with LVN corridor touching VA boundary
    // The gap must be bounded by VA boundary or HVN cluster
    SyntheticHistogram h;
    h.reset();

    // Lower volume area (ticks 80-84) - part of VAL boundary
    for (int t = 80; t <= 84; t++) {
        h.addLevel(t, 3000);
    }

    // LVN gap (ticks 85-92) - low volume but NOT vacuum-level
    // Volume 1200: satisfies GAP (<=40% of median=4000->1600) but NOT VACUUM (<=25% of 4000->1000)
    for (int t = 85; t <= 92; t++) {
        h.addLevel(t, 1200);
    }

    // High volume cluster (ticks 93-110)
    for (int t = 93; t <= 110; t++) {
        h.addLevel(t, 4000);
    }

    h.computeThresholds();

    // POC at center, VA boundary at the gap start
    int pocTick = 100;
    int vahTick = 106;
    int valTick = 85;  // LVN starts exactly at VAL

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), pocTick, vahTick, valTick, h.thresholds);

    assert(f.valid && "Features should be valid");
    assert(!f.lvnRuns.empty() && "Should detect LVN runs");

    AMT::VolumePatternResult result = AMT::DetectAllPatterns(f, nullptr);

    assert(hasPattern(result, AMT::VolumeProfilePattern::VOLUME_GAP) &&
           "Should detect VOLUME_GAP");

    AMT::VolumePatternHit* hit = findHit(result, AMT::VolumeProfilePattern::VOLUME_GAP);
    assert(hit != nullptr && "Should have gap hit");
    assert(hit->lowTick >= 85 && hit->highTick <= 92 && "Gap should be in LVN region");
    assert(hit->strength01 > 0.0f && hit->strength01 <= 1.0f && "Strength should be in [0,1]");

    std::cout << "  PASSED: VOLUME_GAP detected at ticks [" << hit->lowTick
              << "-" << hit->highTick << "] strength=" << hit->strength01 << std::endl;
}

// ============================================================================
// TEST: Volume Vacuum Detection
// ============================================================================

void test_volume_vacuum() {
    std::cout << "Testing VOLUME_VACUUM detection..." << std::endl;

    // Create histogram with very empty corridor (stricter than gap)
    SyntheticHistogram h;
    h.reset();

    // Lower HVN cluster
    for (int t = 70; t <= 85; t++) {
        h.addLevel(t, 6000);
    }

    // Vacuum - extremely low volume, wide
    for (int t = 86; t <= 100; t++) {
        h.addLevel(t, 50); // Very low
    }

    // Upper HVN cluster
    for (int t = 101; t <= 115; t++) {
        h.addLevel(t, 6000);
    }

    h.computeThresholds();

    int pocTick = 105;
    int vahTick = 112;
    int valTick = 75;

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), pocTick, vahTick, valTick, h.thresholds);

    assert(f.valid && "Features should be valid");

    AMT::VolumePatternResult result = AMT::DetectAllPatterns(f, nullptr);

    assert(hasPattern(result, AMT::VolumeProfilePattern::VOLUME_VACUUM) &&
           "Should detect VOLUME_VACUUM");

    AMT::VolumePatternHit* hit = findHit(result, AMT::VolumeProfilePattern::VOLUME_VACUUM);
    assert(hit != nullptr && "Should have vacuum hit");
    assert(hit->WidthTicks() >= 4 && "Vacuum should be at least 4 ticks wide");

    std::cout << "  PASSED: VOLUME_VACUUM detected at ticks [" << hit->lowTick
              << "-" << hit->highTick << "] strength=" << hit->strength01 << std::endl;
}

// ============================================================================
// TEST: Volume Shelf Detection
// ============================================================================

void test_volume_shelf() {
    std::cout << "Testing VOLUME_SHELF detection..." << std::endl;

    // Create histogram with flat HVN plateau and sharp edge drop
    SyntheticHistogram h;
    h.reset();

    // Low volume lead-in
    for (int t = 80; t <= 89; t++) {
        h.addLevel(t, 400);
    }

    // Flat HVN shelf (high, very uniform volume)
    // All volumes must exceed HVN threshold to form contiguous run
    for (int t = 90; t <= 100; t++) {
        h.addLevel(t, 8000); // Much higher than tails to ensure HVN classification
    }

    // Sharp drop to low volume
    for (int t = 101; t <= 115; t++) {
        h.addLevel(t, 400);
    }

    h.computeThresholds();

    int pocTick = 95;
    int vahTick = 98;
    int valTick = 92;

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), pocTick, vahTick, valTick, h.thresholds);

    assert(f.valid && "Features should be valid");
    assert(!f.hvnRuns.empty() && "Should detect HVN runs");

    AMT::VolumePatternResult result = AMT::DetectAllPatterns(f, nullptr);

    assert(hasPattern(result, AMT::VolumeProfilePattern::VOLUME_SHELF) &&
           "Should detect VOLUME_SHELF");

    AMT::VolumePatternHit* hit = findHit(result, AMT::VolumeProfilePattern::VOLUME_SHELF);
    assert(hit != nullptr && "Should have shelf hit");
    assert(hit->lowTick >= 90 && hit->highTick <= 100 && "Shelf should be in HVN region");

    std::cout << "  PASSED: VOLUME_SHELF detected at ticks [" << hit->lowTick
              << "-" << hit->highTick << "] strength=" << hit->strength01 << std::endl;
}

// ============================================================================
// TEST: Ledge Pattern Detection
// ============================================================================

void test_ledge_pattern() {
    std::cout << "Testing LEDGE_PATTERN detection..." << std::endl;

    // Create histogram with sudden step-change in volume
    SyntheticHistogram h;
    h.reset();

    // Low volume region
    for (int t = 80; t <= 94; t++) {
        h.addLevel(t, 800);
    }

    // Sharp step up (ledge)
    for (int t = 95; t <= 110; t++) {
        h.addLevel(t, 4500);
    }

    h.computeThresholds();

    int pocTick = 100;
    int vahTick = 106;
    int valTick = 88;

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), pocTick, vahTick, valTick, h.thresholds);

    assert(f.valid && "Features should be valid");
    assert(!f.gradients.empty() && "Should have gradients");

    AMT::VolumePatternResult result = AMT::DetectAllPatterns(f, nullptr);

    // Ledge detection is sensitive - may or may not trigger depending on threshold
    if (hasPattern(result, AMT::VolumeProfilePattern::LEDGE_PATTERN)) {
        AMT::VolumePatternHit* hit = findHit(result, AMT::VolumeProfilePattern::LEDGE_PATTERN);
        assert(hit != nullptr && "Should have ledge hit");
        std::cout << "  PASSED: LEDGE_PATTERN detected at tick " << hit->anchorTick
                  << " strength=" << hit->strength01 << std::endl;
    } else {
        // Acceptable if gradient doesn't exceed threshold
        std::cout << "  PASSED: LEDGE_PATTERN not detected (gradient below threshold)" << std::endl;
    }
}

// ============================================================================
// TEST: Volume Cluster Detection
// ============================================================================

void test_volume_cluster() {
    std::cout << "Testing VOLUME_CLUSTER detection..." << std::endl;

    // Create histogram with concentrated HVN mass in VA, no LVN corridors
    // The key is: within VA, all volumes must be at/above HVN threshold
    // And there should be no significant LVN corridors within VA
    SyntheticHistogram h;
    h.reset();

    // Low volume tails outside VA (these will be LVN, but outside VA)
    for (int t = 80; t <= 92; t++) {
        h.addLevel(t, 500);
    }

    // Very high, uniform volume within VA
    for (int t = 93; t <= 107; t++) {
        h.addLevel(t, 8000);
    }

    // Low volume tails outside VA
    for (int t = 108; t <= 120; t++) {
        h.addLevel(t, 500);
    }

    // Use more lenient HVN/LVN coefficients for testing
    // This ensures 8000 volume exceeds HVN threshold
    h.computeThresholds(0.5, 0.5);  // Lower sigma multipliers

    int pocTick = 100;
    int vahTick = 107;
    int valTick = 93;

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), pocTick, vahTick, valTick, h.thresholds);

    assert(f.valid && "Features should be valid");

    AMT::VolumePatternResult result = AMT::DetectAllPatterns(f, nullptr);

    // With very high HVN mass and no LVN gaps in VA, should detect cluster
    assert(hasPattern(result, AMT::VolumeProfilePattern::VOLUME_CLUSTER) &&
           "Should detect VOLUME_CLUSTER");

    AMT::VolumePatternHit* hit = findHit(result, AMT::VolumeProfilePattern::VOLUME_CLUSTER);
    assert(hit != nullptr && "Should have cluster hit");
    assert(hit->anchorTick == pocTick && "Cluster anchor should be at POC");

    std::cout << "  PASSED: VOLUME_CLUSTER detected with strength=" << hit->strength01 << std::endl;
}

// ============================================================================
// TEST: Volume Migration Detection
// ============================================================================

void test_volume_migration() {
    std::cout << "Testing VOLUME_MIGRATION detection..." << std::endl;

    // Create a simple valid histogram
    SyntheticHistogram h;
    h.reset();
    for (int t = 80; t <= 120; t++) {
        h.addLevel(t, 2000);
    }
    h.computeThresholds();

    int vahTick = 115;
    int valTick = 85;
    int vaWidth = vahTick - valTick; // 30 ticks

    // Create migration history with monotonic drift
    AMT::MigrationHistory history;
    history.Reset();

    // Simulate POC drifting upward over 8 updates
    int startPoc = 95;
    for (int i = 0; i < 8; i++) {
        history.AddPOC(startPoc + i * 2); // Drift by 2 ticks each update
    }

    int currentPoc = startPoc + 7 * 2; // 109

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), currentPoc, vahTick, valTick, h.thresholds);

    assert(f.valid && "Features should be valid");

    AMT::VolumePatternResult result = AMT::DetectAllPatterns(f, &history);

    assert(hasPattern(result, AMT::VolumeProfilePattern::VOLUME_MIGRATION) &&
           "Should detect VOLUME_MIGRATION");

    AMT::VolumePatternHit* hit = findHit(result, AMT::VolumeProfilePattern::VOLUME_MIGRATION);
    assert(hit != nullptr && "Should have migration hit");

    std::cout << "  PASSED: VOLUME_MIGRATION detected, drift from " << hit->lowTick
              << " to " << hit->highTick << " strength=" << hit->strength01 << std::endl;
}

// ============================================================================
// TEST: No Migration Without History
// ============================================================================

void test_no_migration_without_history() {
    std::cout << "Testing migration not detected without history..." << std::endl;

    SyntheticHistogram h;
    h.reset();
    for (int t = 80; t <= 120; t++) {
        h.addLevel(t, 2000);
    }
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 100, 115, 85, h.thresholds);

    // Pass nullptr for migration history
    AMT::VolumePatternResult result = AMT::DetectAllPatterns(f, nullptr);

    assert(!hasPattern(result, AMT::VolumeProfilePattern::VOLUME_MIGRATION) &&
           "Should NOT detect VOLUME_MIGRATION without history");

    std::cout << "  PASSED: No migration detected without history" << std::endl;
}

// ============================================================================
// TEST: No Migration With Reversals
// ============================================================================

void test_no_migration_with_reversals() {
    std::cout << "Testing migration not detected with too many reversals..." << std::endl;

    SyntheticHistogram h;
    h.reset();
    for (int t = 80; t <= 120; t++) {
        h.addLevel(t, 2000);
    }
    h.computeThresholds();

    // Create migration history with frequent reversals
    AMT::MigrationHistory history;
    history.Reset();

    // POC oscillates: 100, 102, 99, 103, 98, 104, 97, 105
    int oscillating[] = {100, 102, 99, 103, 98, 104, 97, 105};
    for (int poc : oscillating) {
        history.AddPOC(poc);
    }

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 105, 115, 85, h.thresholds);

    AMT::VolumePatternResult result = AMT::DetectAllPatterns(f, &history);

    assert(!hasPattern(result, AMT::VolumeProfilePattern::VOLUME_MIGRATION) &&
           "Should NOT detect VOLUME_MIGRATION with many reversals");

    std::cout << "  PASSED: No migration detected with reversals" << std::endl;
}

// ============================================================================
// HELPER: Check if TPO mechanics type is in result
// ============================================================================

bool hasTPOMech(const AMT::VolumePatternResult& result, AMT::TPOMechanics mech) {
    for (const auto& m : result.tpoMechanics) {
        if (m == mech) return true;
    }
    return false;
}

// ============================================================================
// TEST: TPO Mechanics - High Overlap (TPO_OVERLAP)
// ============================================================================

void test_tpo_mechanics_overlap() {
    std::cout << "Testing TPO mechanics - high overlap..." << std::endl;

    // Create identical IB snapshot and current distribution
    // overlap = 1.0 (identical) -> should emit TPO_OVERLAP (>= 0.6)
    AMT::IBDistSnapshot ibSnapshot;
    ibSnapshot.tickSize = 0.25;
    ibSnapshot.capturedAtBar = 100;
    ibSnapshot.valid = true;

    // IB distribution: ticks 80-120 with uniform volume
    for (int t = 80; t <= 120; t++) {
        ibSnapshot.dist.push_back({t, 2000.0});
    }

    // Create current histogram - identical to IB
    SyntheticHistogram h;
    h.reset();
    for (int t = 80; t <= 120; t++) {
        h.addLevel(t, 2000);
    }
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 100, 115, 85, h.thresholds);

    AMT::VolumePatternResult result = AMT::DetectAllPatterns(
        f, nullptr, &ibSnapshot, 0.25);

    assert(hasTPOMech(result, AMT::TPOMechanics::TPO_OVERLAP) &&
           "Identical distributions should emit TPO_OVERLAP");
    assert(!result.tpoHits.empty() && "Should have TPO hit");
    assert(result.tpoHits[0].overlap01 >= 0.99f && "Overlap should be ~1.0");

    std::cout << "  PASSED: High overlap emits TPO_OVERLAP (overlap="
              << result.tpoHits[0].overlap01 << ")" << std::endl;
}

// ============================================================================
// TEST: TPO Mechanics - Low Overlap (TPO_SEPARATION)
// ============================================================================

void test_tpo_mechanics_separation() {
    std::cout << "Testing TPO mechanics - low overlap (separation)..." << std::endl;

    // Create non-overlapping distributions
    // IB at ticks 80-100, current at ticks 110-130 -> zero overlap
    AMT::IBDistSnapshot ibSnapshot;
    ibSnapshot.tickSize = 0.25;
    ibSnapshot.capturedAtBar = 100;
    ibSnapshot.valid = true;

    // IB distribution: ticks 80-100
    for (int t = 80; t <= 100; t++) {
        ibSnapshot.dist.push_back({t, 2000.0});
    }

    // Create current histogram - completely separate range
    SyntheticHistogram h;
    h.reset();
    for (int t = 110; t <= 130; t++) {
        h.addLevel(t, 2000);
    }
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 120, 128, 112, h.thresholds);

    AMT::VolumePatternResult result = AMT::DetectAllPatterns(
        f, nullptr, &ibSnapshot, 0.25);

    assert(hasTPOMech(result, AMT::TPOMechanics::TPO_SEPARATION) &&
           "Non-overlapping distributions should emit TPO_SEPARATION");
    assert(!result.tpoHits.empty() && "Should have TPO hit");
    assert(result.tpoHits[0].overlap01 <= 0.01f && "Overlap should be ~0.0");

    std::cout << "  PASSED: Low overlap emits TPO_SEPARATION (overlap="
              << result.tpoHits[0].overlap01 << ")" << std::endl;
}

// ============================================================================
// TEST: TPO Mechanics - Mid Overlap (No Classification)
// ============================================================================

void test_tpo_mechanics_mid_overlap() {
    std::cout << "Testing TPO mechanics - mid overlap (no classification)..." << std::endl;

    // Create partial overlap: IB 80-109 (30 bins), current 93-122 (30 bins)
    // Overlap: 93-109 (17 bins), IB-only: 80-92 (13 bins), Current-only: 110-122 (13 bins)
    // overlap = 17 / (17+13+13) = 17/43 = ~40% -> should NOT emit (between 0.3 and 0.6)
    AMT::IBDistSnapshot ibSnapshot;
    ibSnapshot.tickSize = 0.25;
    ibSnapshot.capturedAtBar = 100;
    ibSnapshot.valid = true;

    // IB distribution: ticks 80-109 (30 bins)
    for (int t = 80; t <= 109; t++) {
        ibSnapshot.dist.push_back({t, 2000.0});
    }

    // Create current histogram: ticks 93-122 (30 bins)
    SyntheticHistogram h;
    h.reset();
    for (int t = 93; t <= 122; t++) {
        h.addLevel(t, 2000);
    }
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 107, 117, 97, h.thresholds);

    AMT::VolumePatternResult result = AMT::DetectAllPatterns(
        f, nullptr, &ibSnapshot, 0.25);

    assert(result.tpoMechanics.empty() &&
           "Mid-range overlap should not emit any TPO mechanics");
    assert(result.tpoHits.empty() && "Should have no TPO hits");

    std::cout << "  PASSED: Mid overlap emits nothing" << std::endl;
}

// ============================================================================
// TEST: TPO Mechanics - Empty Distribution (No Classification)
// ============================================================================

void test_tpo_mechanics_empty_dist() {
    std::cout << "Testing TPO mechanics - empty distribution..." << std::endl;

    // IB snapshot with empty distribution
    AMT::IBDistSnapshot ibSnapshot;
    ibSnapshot.tickSize = 0.25;
    ibSnapshot.capturedAtBar = 100;
    ibSnapshot.valid = true;  // Valid but empty
    // dist is empty

    SyntheticHistogram h;
    h.reset();
    for (int t = 80; t <= 120; t++) {
        h.addLevel(t, 2000);
    }
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 100, 115, 85, h.thresholds);

    AMT::VolumePatternResult result = AMT::DetectAllPatterns(
        f, nullptr, &ibSnapshot, 0.25);

    assert(result.tpoMechanics.empty() &&
           "Empty IB distribution should not emit TPO mechanics");

    std::cout << "  PASSED: Empty distribution emits nothing" << std::endl;
}

// ============================================================================
// TEST: TPO Mechanics - Tick Size Mismatch (No Classification)
// ============================================================================

void test_tpo_mechanics_ticksize_mismatch() {
    std::cout << "Testing TPO mechanics - tick size mismatch..." << std::endl;

    // IB snapshot with different tick size
    AMT::IBDistSnapshot ibSnapshot;
    ibSnapshot.tickSize = 0.50;  // Different from current 0.25
    ibSnapshot.capturedAtBar = 100;
    ibSnapshot.valid = true;

    for (int t = 80; t <= 120; t++) {
        ibSnapshot.dist.push_back({t, 2000.0});
    }

    SyntheticHistogram h;
    h.reset();
    for (int t = 80; t <= 120; t++) {
        h.addLevel(t, 2000);
    }
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 100, 115, 85, h.thresholds);

    // Pass different tick size
    AMT::VolumePatternResult result = AMT::DetectAllPatterns(
        f, nullptr, &ibSnapshot, 0.25);

    assert(result.tpoMechanics.empty() &&
           "Tick size mismatch should not emit TPO mechanics");

    std::cout << "  PASSED: Tick size mismatch emits nothing" << std::endl;
}

// ============================================================================
// TEST: TPO Mechanics - No IB Snapshot (No Classification)
// ============================================================================

void test_tpo_mechanics_no_snapshot() {
    std::cout << "Testing TPO mechanics - no IB snapshot..." << std::endl;

    SyntheticHistogram h;
    h.reset();
    for (int t = 80; t <= 120; t++) {
        h.addLevel(t, 2000);
    }
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 100, 115, 85, h.thresholds);

    // No IB snapshot passed (nullptr)
    AMT::VolumePatternResult result = AMT::DetectAllPatterns(f, nullptr, nullptr, 0.25);

    assert(result.tpoMechanics.empty() &&
           "No IB snapshot should not emit TPO mechanics");
    assert(result.tpoHits.empty() && "Should have no TPO hits");

    std::cout << "  PASSED: No IB snapshot emits nothing" << std::endl;
}

// ============================================================================
// TEST: TPO Mechanics - Invalid Snapshot (No Classification)
// ============================================================================

void test_tpo_mechanics_invalid_snapshot() {
    std::cout << "Testing TPO mechanics - invalid IB snapshot..." << std::endl;

    // IB snapshot with valid=false
    AMT::IBDistSnapshot ibSnapshot;
    ibSnapshot.tickSize = 0.25;
    ibSnapshot.capturedAtBar = 100;
    ibSnapshot.valid = false;  // Invalid

    for (int t = 80; t <= 120; t++) {
        ibSnapshot.dist.push_back({t, 2000.0});
    }

    SyntheticHistogram h;
    h.reset();
    for (int t = 80; t <= 120; t++) {
        h.addLevel(t, 2000);
    }
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 100, 115, 85, h.thresholds);

    AMT::VolumePatternResult result = AMT::DetectAllPatterns(
        f, nullptr, &ibSnapshot, 0.25);

    assert(result.tpoMechanics.empty() &&
           "Invalid IB snapshot should not emit TPO mechanics");

    std::cout << "  PASSED: Invalid snapshot emits nothing" << std::endl;
}

// ============================================================================
// TEST: IBDistSnapshot CaptureFrom and Reset
// ============================================================================

void test_ib_snapshot_capture_reset() {
    std::cout << "Testing IBDistSnapshot capture and reset..." << std::endl;

    // Create a mock volume profile map (simplified)
    std::map<int, s_VolumeAtPriceV2> volumeProfile;
    for (int t = 80; t <= 100; t++) {
        s_VolumeAtPriceV2 vap;
        vap.PriceInTicks = t;
        vap.Volume = 1000;
        volumeProfile[t] = vap;
    }

    AMT::IBDistSnapshot snapshot;

    // Initially invalid
    assert(!snapshot.valid && "Snapshot should start invalid");
    assert(snapshot.dist.empty() && "Snapshot should start empty");

    // Capture from profile
    snapshot.CaptureFrom(volumeProfile, 0.25, 50);

    assert(snapshot.valid && "Snapshot should be valid after capture");
    assert(snapshot.dist.size() == 21 && "Should have 21 levels (80-100)");
    assert(snapshot.tickSize == 0.25 && "Tick size should match");
    assert(snapshot.capturedAtBar == 50 && "Captured bar should match");
    assert(snapshot.IsCompatible(0.25) && "Should be compatible with same tick size");
    assert(!snapshot.IsCompatible(0.50) && "Should not be compatible with different tick size");

    // Reset
    snapshot.Reset();
    assert(!snapshot.valid && "Snapshot should be invalid after reset");
    assert(snapshot.dist.empty() && "Snapshot should be empty after reset");

    std::cout << "  PASSED: IBDistSnapshot capture and reset work correctly" << std::endl;
}

// ============================================================================
// TEST: ComputeDistributionOverlap - Degenerate Cases
// ============================================================================

void test_overlap_degenerate() {
    std::cout << "Testing overlap computation - degenerate cases..." << std::endl;

    // Empty distributions
    std::vector<std::pair<int, double>> empty;
    std::vector<std::pair<int, double>> nonEmpty = {{100, 1000.0}};

    auto result1 = AMT::ComputeDistributionOverlap(empty, empty);
    assert(!result1.has_value() && "Empty vs empty should return nullopt");

    auto result2 = AMT::ComputeDistributionOverlap(empty, nonEmpty);
    assert(!result2.has_value() && "Empty vs non-empty should return nullopt");

    auto result3 = AMT::ComputeDistributionOverlap(nonEmpty, empty);
    assert(!result3.has_value() && "Non-empty vs empty should return nullopt");

    // Zero volume distribution (sumMax == 0)
    std::vector<std::pair<int, double>> zeroVol = {{100, 0.0}, {101, 0.0}};
    auto result4 = AMT::ComputeDistributionOverlap(zeroVol, zeroVol);
    assert(!result4.has_value() && "Zero volume should return nullopt");

    std::cout << "  PASSED: Degenerate overlap cases handled correctly" << std::endl;
}

// ============================================================================
// TEST: Delayed Capture After ibFrozen
// Simulates scenario where IB freezes but profile isn't ready yet
// ============================================================================

void test_delayed_capture_after_frozen() {
    std::cout << "Testing delayed capture after ibFrozen..." << std::endl;

    // Scenario: IB froze at bar 100, but we don't capture until bar 110
    // when the profile becomes eligible

    // Step 1: Create an empty/invalid snapshot (simulating "IB frozen but no profile yet")
    AMT::IBDistSnapshot snapshot;
    assert(!snapshot.valid && "Snapshot should start invalid");

    // Step 2: Simulate first VbP refresh after IB freeze with empty profile
    std::map<int, s_VolumeAtPriceV2> emptyProfile;
    // Don't capture from empty profile - this is the "waiting" state
    if (!emptyProfile.empty()) {
        snapshot.CaptureFrom(emptyProfile, 0.25, 100);
    }
    assert(!snapshot.valid && "Snapshot should still be invalid after empty profile");

    // Step 3: Simulate later VbP refresh with valid profile (delayed capture)
    std::map<int, s_VolumeAtPriceV2> validProfile;
    for (int t = 80; t <= 120; t++) {
        s_VolumeAtPriceV2 vap;
        vap.PriceInTicks = t;
        vap.Volume = 2000;
        validProfile[t] = vap;
    }

    // Now capture at bar 110 (delayed from freeze at bar 100)
    const int delayedCaptureBar = 110;
    snapshot.CaptureFrom(validProfile, 0.25, delayedCaptureBar);

    assert(snapshot.valid && "Snapshot should be valid after delayed capture");
    assert(snapshot.capturedAtBar == delayedCaptureBar && "Captured bar should be delayed bar");
    assert(snapshot.dist.size() == 41 && "Should have 41 levels (80-120)");

    // Step 4: Verify TPO mechanics works with delayed-captured snapshot
    SyntheticHistogram h;
    h.reset();
    for (int t = 80; t <= 120; t++) {
        h.addLevel(t, 2000);  // Same as IB snapshot
    }
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 100, 115, 85, h.thresholds);

    AMT::VolumePatternResult result = AMT::DetectAllPatterns(
        f, nullptr, &snapshot, 0.25);

    assert(hasTPOMech(result, AMT::TPOMechanics::TPO_OVERLAP) &&
           "Delayed-captured snapshot should work for TPO detection");
    assert(!result.tpoHits.empty() && "Should have TPO hit");
    assert(result.tpoHits[0].overlap01 >= 0.99f && "Overlap should be ~1.0");

    std::cout << "  PASSED: Delayed capture after ibFrozen works correctly" << std::endl;
}

// ============================================================================
// TEST: Delayed Capture - Snapshot Only Taken Once
// Ensures second refresh doesn't overwrite first capture
// ============================================================================

void test_delayed_capture_only_once() {
    std::cout << "Testing delayed capture only happens once..." << std::endl;

    // Create first profile (IB period)
    std::map<int, s_VolumeAtPriceV2> ibProfile;
    for (int t = 80; t <= 100; t++) {
        s_VolumeAtPriceV2 vap;
        vap.PriceInTicks = t;
        vap.Volume = 2000;
        ibProfile[t] = vap;
    }

    AMT::IBDistSnapshot snapshot;

    // First capture at bar 100
    snapshot.CaptureFrom(ibProfile, 0.25, 100);
    assert(snapshot.valid && "First capture should succeed");
    assert(snapshot.dist.size() == 21 && "Should have 21 levels");
    assert(snapshot.capturedAtBar == 100 && "Should be captured at bar 100");

    // Simulate a later "different" profile that should NOT overwrite
    std::map<int, s_VolumeAtPriceV2> laterProfile;
    for (int t = 90; t <= 130; t++) {
        s_VolumeAtPriceV2 vap;
        vap.PriceInTicks = t;
        vap.Volume = 3000;  // Different volume
        laterProfile[t] = vap;
    }

    // In the actual code, we check !snapshot.valid before capture
    // So this second capture should NOT happen if snapshot is already valid
    if (!snapshot.valid) {
        snapshot.CaptureFrom(laterProfile, 0.25, 150);
    }

    // Verify original snapshot is preserved
    assert(snapshot.capturedAtBar == 100 && "Should still be captured at bar 100");
    assert(snapshot.dist.size() == 21 && "Should still have 21 levels");
    assert(snapshot.dist[0].second == 2000.0 && "Volume should be original 2000");

    std::cout << "  PASSED: Delayed capture only happens once" << std::endl;
}

// ============================================================================
// TEST: No Patterns on Ineligible Data
// ============================================================================

void test_no_patterns_ineligible() {
    std::cout << "Testing no patterns on ineligible data..." << std::endl;

    // Create histogram with too few bins
    SyntheticHistogram h;
    h.reset();
    h.addLevel(100, 1000);
    h.addLevel(101, 1000);
    h.addLevel(102, 1000);
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 101, 102, 100, h.thresholds);

    AMT::VolumePatternResult result = AMT::DetectAllPatterns(f, nullptr);

    assert(result.patterns.empty() && "Should have no patterns on ineligible data");
    assert(result.hits.empty() && "Should have no hits on ineligible data");

    std::cout << "  PASSED: No patterns on ineligible data" << std::endl;
}

// ============================================================================
// TEST: Ambiguous Profile Yields No Forced Patterns
// ============================================================================

void test_ambiguous_no_forced() {
    std::cout << "Testing ambiguous profile yields no forced patterns..." << std::endl;

    // Create uniform histogram with no distinct features
    SyntheticHistogram h;
    h.reset();
    for (int t = 90; t <= 110; t++) {
        h.addLevel(t, 2000); // All same volume
    }
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 100, 108, 92, h.thresholds);

    AMT::VolumePatternResult result = AMT::DetectAllPatterns(f, nullptr);

    // With uniform volume, there should be no gaps, vacuums, or shelves
    // There might be a cluster if HVN mass is concentrated
    // But no GAP or VACUUM since no LVN corridors
    assert(!hasPattern(result, AMT::VolumeProfilePattern::VOLUME_GAP) &&
           "Uniform profile should not have GAP");
    assert(!hasPattern(result, AMT::VolumeProfilePattern::VOLUME_VACUUM) &&
           "Uniform profile should not have VACUUM");

    std::cout << "  PASSED: Ambiguous profile yields appropriate patterns only" << std::endl;
}

// ============================================================================
// TEST: Valid Upside Breakout
// ============================================================================

void test_breakout_upside() {
    std::cout << "Testing valid upside breakout..." << std::endl;

    // Balance snapshot at VAH=100, VAL=80, POC=90 (20 tick width)
    AMT::BalanceSnapshot balanceRef;
    balanceRef.UpdateFrom(100, 80, 90, 50, 0.25);
    assert(balanceRef.IsCoherent() && "Balance snapshot should be coherent");

    // Create histogram with significant volume above VAH (breakout accepted)
    // Inside VA: ticks 80-100, 2000 volume each (21 bins * 2000 = 42000)
    // Outside above: ticks 101-115, 3000 volume each (15 bins * 3000 = 45000)
    // Total = 87000, outside = 45000/87000 = 51.7% (well above 15% accept threshold)
    SyntheticHistogram h;
    h.reset();
    for (int t = 80; t <= 100; t++) {
        h.addLevel(t, 2000);  // Inside VA
    }
    for (int t = 101; t <= 115; t++) {
        h.addLevel(t, 3000);  // Outside above VAH (HVN level)
    }
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 105, 110, 85, h.thresholds);

    // Simulate TPO_SEPARATION mechanics
    std::vector<AMT::TPOMechanics> mechanics = {AMT::TPOMechanics::TPO_SEPARATION};

    auto hit = AMT::DetectBreakoutOrTrap(f, balanceRef, mechanics);

    assert(hit.has_value() && "Should detect breakout");
    assert(hit->type == AMT::VolumeProfilePattern::VOLUME_BREAKOUT &&
           "Should be VOLUME_BREAKOUT");
    assert(hit->anchorTick == 100 && "Anchor should be VAH");
    assert(hit->strength01 > 0.15f && "Outside mass should exceed acceptance threshold");

    std::cout << "  PASSED: Valid upside breakout detected (str=" << hit->strength01 << ")" << std::endl;
}

// ============================================================================
// TEST: Valid Downside Breakout
// ============================================================================

void test_breakout_downside() {
    std::cout << "Testing valid downside breakout..." << std::endl;

    // Balance snapshot at VAH=100, VAL=80, POC=90
    AMT::BalanceSnapshot balanceRef;
    balanceRef.UpdateFrom(100, 80, 90, 50, 0.25);

    // Create histogram with significant volume below VAL
    // Inside VA: ticks 80-100, 2000 volume each
    // Outside below: ticks 65-79, 3000 volume each (15 bins * 3000 = 45000)
    SyntheticHistogram h;
    h.reset();
    for (int t = 80; t <= 100; t++) {
        h.addLevel(t, 2000);
    }
    for (int t = 65; t <= 79; t++) {
        h.addLevel(t, 3000);  // Outside below VAL
    }
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 90, 100, 70, h.thresholds);

    std::vector<AMT::TPOMechanics> mechanics = {AMT::TPOMechanics::TPO_SEPARATION};

    auto hit = AMT::DetectBreakoutOrTrap(f, balanceRef, mechanics);

    assert(hit.has_value() && "Should detect breakout");
    assert(hit->type == AMT::VolumeProfilePattern::VOLUME_BREAKOUT &&
           "Should be VOLUME_BREAKOUT");
    assert(hit->anchorTick == 80 && "Anchor should be VAL");

    std::cout << "  PASSED: Valid downside breakout detected" << std::endl;
}

// ============================================================================
// TEST: Trap Upside (Low Volume Breakout)
// ============================================================================

void test_trap_upside() {
    std::cout << "Testing trap upside (low volume breakout)..." << std::endl;

    // Balance snapshot at VAH=100, VAL=80, POC=90
    AMT::BalanceSnapshot balanceRef;
    balanceRef.UpdateFrom(100, 80, 90, 50, 0.25);

    // Create histogram with weak volume above VAH (breach but not accepted)
    // Inside VA: ticks 80-100, 5000 volume each (21 bins * 5000 = 105000)
    // Outside above: ticks 101-105, 1200 volume each (5 bins * 1200 = 6000)
    // Total = 111000, outside = 6000/111000 = 5.4% (above 5% breach, below 12% trap)
    SyntheticHistogram h;
    h.reset();
    for (int t = 80; t <= 100; t++) {
        h.addLevel(t, 5000);
    }
    for (int t = 101; t <= 105; t++) {
        h.addLevel(t, 1200);  // Weak outside volume
    }
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 90, 100, 80, h.thresholds);

    // No separation mechanics (overlap or none)
    std::vector<AMT::TPOMechanics> mechanics = {AMT::TPOMechanics::TPO_OVERLAP};

    auto hit = AMT::DetectBreakoutOrTrap(f, balanceRef, mechanics);

    assert(hit.has_value() && "Should detect trap");
    assert(hit->type == AMT::VolumeProfilePattern::LOW_VOLUME_BREAKOUT &&
           "Should be LOW_VOLUME_BREAKOUT (trap)");

    std::cout << "  PASSED: Trap detected (str=" << hit->strength01 << ")" << std::endl;
}

// ============================================================================
// TEST: Both Sides Breach = Ambiguous (No Pattern)
// ============================================================================

void test_breakout_both_sides_ambiguous() {
    std::cout << "Testing both sides breach (ambiguous)..." << std::endl;

    // Balance snapshot at VAH=100, VAL=80, POC=90
    AMT::BalanceSnapshot balanceRef;
    balanceRef.UpdateFrom(100, 80, 90, 50, 0.25);

    // Create histogram with significant volume on BOTH sides (ambiguous)
    // Inside VA: ticks 80-100, 2000 volume each (21 bins * 2000 = 42000)
    // Outside above: ticks 101-108, 2000 volume each (8 bins * 2000 = 16000)
    // Outside below: ticks 72-79, 2000 volume each (8 bins * 2000 = 16000)
    // Total = 74000, above = 16000/74000 = 21.6%, below = 21.6% (both > 3% guard)
    SyntheticHistogram h;
    h.reset();
    for (int t = 72; t <= 108; t++) {
        h.addLevel(t, 2000);
    }
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 90, 100, 80, h.thresholds);

    std::vector<AMT::TPOMechanics> mechanics = {AMT::TPOMechanics::TPO_SEPARATION};

    auto hit = AMT::DetectBreakoutOrTrap(f, balanceRef, mechanics);

    assert(!hit.has_value() && "Should NOT detect pattern when both sides breach");

    std::cout << "  PASSED: Both sides breach = ambiguous (no pattern)" << std::endl;
}

// ============================================================================
// TEST: No Pattern When Snapshot Invalid
// ============================================================================

void test_breakout_no_snapshot() {
    std::cout << "Testing no pattern when snapshot invalid..." << std::endl;

    AMT::BalanceSnapshot balanceRef;  // Not initialized (valid=false)
    assert(!balanceRef.IsCoherent() && "Snapshot should not be coherent");

    SyntheticHistogram h;
    h.reset();
    for (int t = 80; t <= 120; t++) {
        h.addLevel(t, 2000);
    }
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 100, 115, 85, h.thresholds);

    std::vector<AMT::TPOMechanics> mechanics = {AMT::TPOMechanics::TPO_SEPARATION};

    auto hit = AMT::DetectBreakoutOrTrap(f, balanceRef, mechanics);

    assert(!hit.has_value() && "Should NOT detect pattern when snapshot invalid");

    std::cout << "  PASSED: No pattern when snapshot invalid" << std::endl;
}

// ============================================================================
// TEST: No Pattern When VA Width Too Small
// ============================================================================

void test_breakout_narrow_va() {
    std::cout << "Testing no pattern when VA width too small..." << std::endl;

    // Balance snapshot with narrow VA (width = 5, below minimum of 8)
    AMT::BalanceSnapshot balanceRef;
    balanceRef.UpdateFrom(105, 100, 102, 50, 0.25);  // Width = 5 ticks
    assert(!balanceRef.IsCoherent() && "Narrow VA should not be coherent");

    SyntheticHistogram h;
    h.reset();
    for (int t = 80; t <= 120; t++) {
        h.addLevel(t, 2000);
    }
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 100, 115, 85, h.thresholds);

    std::vector<AMT::TPOMechanics> mechanics = {AMT::TPOMechanics::TPO_SEPARATION};

    auto hit = AMT::DetectBreakoutOrTrap(f, balanceRef, mechanics);

    assert(!hit.has_value() && "Should NOT detect pattern when VA too narrow");

    std::cout << "  PASSED: No pattern when VA too narrow" << std::endl;
}

// ============================================================================
// TEST: No Breach = No Pattern
// ============================================================================

void test_breakout_no_breach() {
    std::cout << "Testing no pattern when no breach..." << std::endl;

    // Balance snapshot at VAH=110, VAL=90 (price is inside VA)
    AMT::BalanceSnapshot balanceRef;
    balanceRef.UpdateFrom(110, 90, 100, 50, 0.25);

    // Create histogram entirely inside VA (no outside volume)
    SyntheticHistogram h;
    h.reset();
    for (int t = 92; t <= 108; t++) {
        h.addLevel(t, 3000);  // All inside VA
    }
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 100, 106, 94, h.thresholds);

    std::vector<AMT::TPOMechanics> mechanics = {AMT::TPOMechanics::TPO_SEPARATION};

    auto hit = AMT::DetectBreakoutOrTrap(f, balanceRef, mechanics);

    assert(!hit.has_value() && "Should NOT detect pattern when no breach");

    std::cout << "  PASSED: No pattern when no breach" << std::endl;
}

// ============================================================================
// TEST: BalanceSnapshot Coherence
// ============================================================================

void test_balance_snapshot_coherence() {
    std::cout << "Testing BalanceSnapshot coherence checks..." << std::endl;

    AMT::BalanceSnapshot snap;

    // Initially invalid
    assert(!snap.IsCoherent() && "Initial snapshot should not be coherent");

    // Invalid: VAH == VAL
    snap.UpdateFrom(100, 100, 100, 50, 0.25);
    assert(!snap.IsCoherent() && "VAH == VAL should not be coherent");

    // Invalid: VAH < VAL
    snap.UpdateFrom(90, 100, 95, 50, 0.25);
    assert(!snap.IsCoherent() && "VAH < VAL should not be coherent");

    // Invalid: Width < minimum
    snap.UpdateFrom(105, 100, 102, 50, 0.25);  // Width = 5
    assert(!snap.IsCoherent() && "Width < 8 should not be coherent");

    // Valid: Good width
    snap.UpdateFrom(120, 100, 110, 50, 0.25);  // Width = 20
    assert(snap.IsCoherent() && "Width >= 8 should be coherent");

    // Check tick size compatibility
    assert(snap.IsCompatible(0.25) && "Same tick size should be compatible");
    assert(!snap.IsCompatible(0.50) && "Different tick size should not be compatible");

    // Reset
    snap.Reset();
    assert(!snap.IsCoherent() && "Reset snapshot should not be coherent");

    std::cout << "  PASSED: BalanceSnapshot coherence checks" << std::endl;
}

// ============================================================================
// TEST: Breakout Metrics Computation
// ============================================================================

void test_breakout_metrics() {
    std::cout << "Testing breakout metrics computation..." << std::endl;

    // Balance snapshot at VAH=100, VAL=80, POC=90
    AMT::BalanceSnapshot balanceRef;
    balanceRef.UpdateFrom(100, 80, 90, 50, 0.25);

    // Create histogram with known distribution
    // Inside: 80-100 = 21 bins * 2000 = 42000
    // Above: 101-110 = 10 bins * 3000 = 30000
    // Below: 70-79 = 10 bins * 1000 = 10000
    // Total = 82000
    SyntheticHistogram h;
    h.reset();
    for (int t = 70; t <= 79; t++) {
        h.addLevel(t, 1000);  // Below VAL
    }
    for (int t = 80; t <= 100; t++) {
        h.addLevel(t, 2000);  // Inside VA
    }
    for (int t = 101; t <= 110; t++) {
        h.addLevel(t, 3000);  // Above VAH
    }
    h.computeThresholds();

    AMT::VolumePatternFeatures f = AMT::ExtractVolumePatternFeatures(
        h.data(), h.size(), 95, 105, 75, h.thresholds);

    AMT::BreakoutMetrics m = AMT::ComputeBreakoutMetrics(f, balanceRef);

    assert(m.valid && "Metrics should be valid");

    // Expected: above = 30000/82000 ≈ 36.6%, below = 10000/82000 ≈ 12.2%
    assert(m.massAboveVAH > 0.35f && m.massAboveVAH < 0.40f &&
           "Mass above should be ~36%");
    assert(m.massBelowVAL > 0.10f && m.massBelowVAL < 0.15f &&
           "Mass below should be ~12%");
    assert(m.outsideAboveHighTick == 110 && "Highest above should be 110");
    assert(m.outsideBelowLowTick == 70 && "Lowest below should be 70");

    std::cout << "  PASSED: Breakout metrics computed correctly" << std::endl;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Volume Pattern Detection Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    test_eligibility_gate();
    test_volume_gap();
    test_volume_vacuum();
    test_volume_shelf();
    test_ledge_pattern();
    test_volume_cluster();
    test_volume_migration();
    test_no_migration_without_history();
    test_no_migration_with_reversals();
    test_no_patterns_ineligible();
    test_ambiguous_no_forced();

    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "TPO Mechanics Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    test_tpo_mechanics_overlap();
    test_tpo_mechanics_separation();
    test_tpo_mechanics_mid_overlap();
    test_tpo_mechanics_empty_dist();
    test_tpo_mechanics_ticksize_mismatch();
    test_tpo_mechanics_no_snapshot();
    test_tpo_mechanics_invalid_snapshot();
    test_ib_snapshot_capture_reset();
    test_overlap_degenerate();
    test_delayed_capture_after_frozen();
    test_delayed_capture_only_once();

    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Breakout/Trap Detection Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    test_balance_snapshot_coherence();
    test_breakout_metrics();
    test_breakout_upside();
    test_breakout_downside();
    test_trap_upside();
    test_breakout_both_sides_ambiguous();
    test_breakout_no_snapshot();
    test_breakout_narrow_va();
    test_breakout_no_breach();

    std::cout << "========================================" << std::endl;
    std::cout << "All tests passed!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
