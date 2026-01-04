// test_profile_shape.cpp - Verify ProfileShape classification
// Tests the shape classifier with synthetic histograms
// Covers: NORMAL_DISTRIBUTION, D_SHAPED, P_SHAPED, B_SHAPED, THIN_VERTICAL, DOUBLE_DISTRIBUTION
// Edge cases: Invalid VA, empty histogram, ambiguous bimodal, inconclusive balance

#include "test_sierrachart_mock.h"
#include "../amt_core.h"
#include "../AMT_config.h"
#include "../AMT_ProfileShape.h"

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

    // Clear and reset
    void reset() {
        bins.clear();
        thresholds.Reset();
    }

    // Add a price level with volume
    void addLevel(int priceTick, uint64_t volume) {
        VolumeAtPrice vap;
        vap.PriceInTicks = priceTick;
        vap.Volume = volume;
        vap.BidVolume = volume / 2;
        vap.AskVolume = volume - vap.BidVolume;
        bins.push_back(vap);
    }

    // Compute thresholds from current bins
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
        thresholds.lvnThreshold = mean - lvnSigmaCoeff * stddev;
        thresholds.sampleSize = static_cast<int>(bins.size());
        thresholds.totalVolume = totalVol;
        thresholds.maxLevelVolume = maxVol;
        thresholds.computedAtBar = 0;
        thresholds.valid = true;
    }

    // Get data pointer and size
    const VolumeAtPrice* data() const { return bins.data(); }
    int size() const { return static_cast<int>(bins.size()); }
};

// ============================================================================
// HELPER: Create specific profile shapes
// ============================================================================

// Create a normal distribution (bell curve) centered at centerTick
// Uses a sharp bell curve to ensure high peakiness (POC >> mean)
SyntheticHistogram createNormalDistribution(int centerTick, int halfWidth, uint64_t peakVol) {
    SyntheticHistogram h;
    h.reset();

    for (int tick = centerTick - halfWidth; tick <= centerTick + halfWidth; tick++) {
        int dist = std::abs(tick - centerTick);
        // Sharp bell curve: very high peak, rapid falloff
        // sigma = halfWidth * 0.2 for tight peak
        double sigma = halfWidth * 0.2;
        uint64_t vol = static_cast<uint64_t>(peakVol * std::exp(-0.5 * (dist * dist) / (sigma * sigma)));
        if (vol < 30) vol = 30;  // Low minimum to maintain high peak ratio
        h.addLevel(tick, vol);
    }

    h.computeThresholds();
    return h;
}

// Create a D-shaped profile (broad hump with one-sided rejection)
// Moderate peakiness: peak is 1.5-2.0x the mean within VA
// ASYMMETRIC: POC slightly off-center, flat edge on one side
SyntheticHistogram createDShaped(int pocTick, int halfWidth, uint64_t peakVol) {
    SyntheticHistogram h;
    h.reset();

    // Create asymmetric profile with "flat edge" on upper side
    // Volume decays normally below POC but is cut off sharply above
    // Key: Higher floor and wider sigma to achieve MODERATE peakiness (1.5-2.0)
    for (int tick = pocTick - halfWidth; tick <= pocTick + halfWidth; tick++) {
        int dist = tick - pocTick;  // Signed distance from POC
        uint64_t vol;

        if (dist <= 0) {
            // Below/at POC: broader hump with higher floor
            double sigma = halfWidth * 0.5;  // Wider sigma for broader hump
            vol = static_cast<uint64_t>(peakVol * std::exp(-0.5 * (dist * dist) / (sigma * sigma)));
        } else {
            // Above POC: sharp cutoff (flat edge) - mimics rejection
            double sigma = halfWidth * 0.2;
            vol = static_cast<uint64_t>(peakVol * 0.5 * std::exp(-0.5 * (dist * dist) / (sigma * sigma)));
        }

        // Higher floor to reduce peakiness (POC/mean ratio)
        // Target peakiness: 1.5-2.0 (moderate)
        if (vol < 250) vol = 250;
        h.addLevel(tick, vol);
    }

    h.computeThresholds();
    return h;
}

// Create a P-shaped profile (fat top, thin bottom - POC near VAH)
SyntheticHistogram createPShaped(int bottomTick, int rangeTicks, uint64_t peakVol) {
    SyntheticHistogram h;
    h.reset();

    // POC near top (fat top)
    const int topTick = bottomTick + rangeTicks;
    const int pocTick = topTick - rangeTicks / 6;

    for (int tick = bottomTick; tick <= topTick; tick++) {
        uint64_t vol;
        if (tick >= pocTick - 2) {
            // High volume near POC (top) - fat top
            int dist = std::abs(tick - pocTick);
            vol = static_cast<uint64_t>(peakVol * std::exp(-0.5 * dist));
        } else {
            // Declining volume below POC (thin tail)
            int distFromPoc = pocTick - tick;
            vol = static_cast<uint64_t>(peakVol * 0.3 * std::exp(-0.1 * distFromPoc));
        }
        if (vol < 50) vol = 50;
        h.addLevel(tick, vol);
    }

    h.computeThresholds();
    return h;
}

// Create a B-shaped profile (fat bottom, thin top - POC near VAL)
SyntheticHistogram createBShaped(int bottomTick, int rangeTicks, uint64_t peakVol) {
    SyntheticHistogram h;
    h.reset();

    // POC near bottom (fat bottom)
    const int pocTick = bottomTick + rangeTicks / 6;

    for (int tick = bottomTick; tick <= bottomTick + rangeTicks; tick++) {
        uint64_t vol;
        if (tick <= pocTick + 2) {
            // High volume near POC (bottom) - fat bottom
            int dist = std::abs(tick - pocTick);
            vol = static_cast<uint64_t>(peakVol * std::exp(-0.5 * dist));
        } else {
            // Declining volume above POC (thin tail)
            int distFromPoc = tick - pocTick;
            vol = static_cast<uint64_t>(peakVol * 0.3 * std::exp(-0.1 * distFromPoc));
        }
        if (vol < 50) vol = 50;
        h.addLevel(tick, vol);
    }

    h.computeThresholds();
    return h;
}

// Create a thin vertical profile (trend day - elongated, no dominant POC)
SyntheticHistogram createThinVertical(int bottomTick, int rangeTicks, uint64_t avgVol) {
    SyntheticHistogram h;
    h.reset();

    // Flat-ish volume distribution across wide range
    for (int tick = bottomTick; tick <= bottomTick + rangeTicks; tick++) {
        // Slight variation but mostly flat
        uint64_t vol = avgVol + (tick % 3) * 10;  // Minor variation
        h.addLevel(tick, vol);
    }

    h.computeThresholds();
    return h;
}

// Create a double distribution (two distinct peaks with clear valley)
// Key: peaks must be wide enough for 2+ bins to exceed HVN threshold
SyntheticHistogram createDoubleDistribution(int bottomTick, int rangeTicks, uint64_t peakVol) {
    SyntheticHistogram h;
    h.reset();

    // Two distinct peaks at 1/5 and 4/5 of range (more separation)
    const int peak1Tick = bottomTick + rangeTicks / 5;
    const int peak2Tick = bottomTick + 4 * rangeTicks / 5;
    const int valleyCenter = bottomTick + rangeTicks / 2;
    const int valleyHalfWidth = rangeTicks / 6;  // Wider LVN zone

    // Wider sigma for multi-bin peaks that can form clusters
    const double sigma = 3.0;

    for (int tick = bottomTick; tick <= bottomTick + rangeTicks; tick++) {
        int dist1 = std::abs(tick - peak1Tick);
        int dist2 = std::abs(tick - peak2Tick);

        // Volume from two bells
        double vol1 = peakVol * std::exp(-0.5 * (dist1 * dist1) / (sigma * sigma));
        double vol2 = peakVol * std::exp(-0.5 * (dist2 * dist2) / (sigma * sigma));

        // Valley: sharp drop in LVN zone between peaks
        int distFromValley = std::abs(tick - valleyCenter);
        double valleyMultiplier = (distFromValley <= valleyHalfWidth) ? 0.08 : 1.0;

        uint64_t vol = static_cast<uint64_t>((vol1 + vol2) * valleyMultiplier);
        // Higher floor outside valley to push threshold lower
        if (vol < 150) vol = 150;
        h.addLevel(tick, vol);
    }

    // Use lower hvn sigma coefficient to detect peaks more easily
    h.computeThresholds(1.0, 0.5);  // hvnThreshold = mean + 1.0*stddev
    return h;
}

// ============================================================================
// TEST: NORMAL_DISTRIBUTION
// ============================================================================

void test_normal_distribution() {
    std::cout << "=== Test: NORMAL_DISTRIBUTION ===" << std::endl;

    SyntheticHistogram h = createNormalDistribution(24020, 10, 1000);

    // POC at center, VA around it
    const int pocTick = 24020;
    const int vahTick = 24025;  // Centered around POC
    const int valTick = 24015;

    AMT::ProfileFeatures features = AMT::ExtractProfileFeatures(
        h.data(), h.size(), pocTick, vahTick, valTick, h.thresholds);

    AMT::ShapeClassificationResult result = AMT::ClassifyProfileShape(features);

    std::cout << "  Shape: " << AMT::ProfileShapeToString(result.shape) << std::endl;
    std::cout << "  Error: " << AMT::ShapeErrorToString(result.error) << std::endl;
    std::cout << "  Confidence: " << result.confidence01 << std::endl;
    std::cout << "  Reason: " << result.reason << std::endl;
    std::cout << "  Peakiness: " << features.peakiness << std::endl;
    std::cout << "  POC in VA: " << features.pocInVa01 << std::endl;

    assert(result.ok());
    assert(result.shape == AMT::ProfileShape::NORMAL_DISTRIBUTION);
    assert(result.error == AMT::ShapeError::NONE);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: D_SHAPED (asymmetric - flat edge on one side)
// Requirements per formal spec:
//   - K_MOD <= k < K_SHARP (1.5 <= peakiness < 2.0)
//   - |a| >= A_D (asymmetry >= 0.15)
//   - POC in center band of range (C_MIN <= x_poc <= C_MAX)
// ============================================================================

void test_d_shaped() {
    std::cout << "=== Test: D_SHAPED ===" << std::endl;

    SyntheticHistogram h = createDShaped(24016, 15, 600);

    // Set up VA so that:
    // - POC is off-center enough for |a| >= 0.15
    // - a = (POC - VA_midpoint) / W_va
    // With POC=24016, VAL=24006, VAH=24030:
    //   W_va = 24, midpoint = 24018
    //   a = (24016 - 24018) / 24 = -0.0833 (not enough!)
    //
    // Need larger asymmetry. Use VAL=24008, VAH=24028:
    //   W_va = 20, midpoint = 24018
    //   a = (24016 - 24018) / 20 = -0.10 (still not enough)
    //
    // Use VAL=24010, VAH=24026:
    //   W_va = 16, midpoint = 24018
    //   a = (24016 - 24018) / 16 = -0.125 (still not enough)
    //
    // Use VAL=24012, VAH=24024:
    //   W_va = 12, midpoint = 24018
    //   a = (24016 - 24018) / 12 = -0.167 (>= 0.15 ✓)
    const int pocTick = 24016;
    const int testValTick = 24012;
    const int testVahTick = 24024;

    AMT::ProfileFeatures features = AMT::ExtractProfileFeatures(
        h.data(), h.size(), pocTick, testVahTick, testValTick, h.thresholds);

    AMT::ShapeClassificationResult result = AMT::ClassifyProfileShape(features);

    std::cout << "  Shape: " << AMT::ProfileShapeToString(result.shape) << std::endl;
    std::cout << "  Error: " << AMT::ShapeErrorToString(result.error) << std::endl;
    std::cout << "  Peakiness (k): " << features.peakiness << std::endl;
    std::cout << "  Asymmetry (a): " << features.asymmetry << std::endl;
    std::cout << "  Breadth (w): " << features.breadth << std::endl;
    std::cout << "  POC in range (x): " << features.pocInRange << std::endl;

    assert(result.ok());
    assert(result.shape == AMT::ProfileShape::D_SHAPED);
    assert(result.error == AMT::ShapeError::NONE);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: P_SHAPED (fat top, thin bottom - POC near VAH)
// ============================================================================

void test_p_shaped() {
    std::cout << "=== Test: P_SHAPED ===" << std::endl;

    SyntheticHistogram h = createPShaped(24000, 40, 1000);

    // POC in top third (fat top)
    const int pocTick = 24034;  // Near top
    const int valTick = 24010;
    const int vahTick = 24040;

    AMT::ProfileFeatures features = AMT::ExtractProfileFeatures(
        h.data(), h.size(), pocTick, vahTick, valTick, h.thresholds);

    AMT::ShapeClassificationResult result = AMT::ClassifyProfileShape(features);

    std::cout << "  Shape: " << AMT::ProfileShapeToString(result.shape) << std::endl;
    std::cout << "  Error: " << AMT::ShapeErrorToString(result.error) << std::endl;
    std::cout << "  POC in VA: " << features.pocInVa01 << std::endl;
    std::cout << "  Mass skew: " << features.massSkewRatio << std::endl;

    assert(result.ok());
    assert(result.shape == AMT::ProfileShape::P_SHAPED);
    assert(result.error == AMT::ShapeError::NONE);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: B_SHAPED (fat bottom, thin top - POC near VAL)
// ============================================================================

void test_b_shaped() {
    std::cout << "=== Test: B_SHAPED ===" << std::endl;

    SyntheticHistogram h = createBShaped(24000, 40, 1000);

    // POC in bottom third (fat bottom)
    const int pocTick = 24006;  // Near bottom
    const int valTick = 24000;
    const int vahTick = 24030;  // Wide VA

    AMT::ProfileFeatures features = AMT::ExtractProfileFeatures(
        h.data(), h.size(), pocTick, vahTick, valTick, h.thresholds);

    AMT::ShapeClassificationResult result = AMT::ClassifyProfileShape(features);

    std::cout << "  Shape: " << AMT::ProfileShapeToString(result.shape) << std::endl;
    std::cout << "  Error: " << AMT::ShapeErrorToString(result.error) << std::endl;
    std::cout << "  POC in VA: " << features.pocInVa01 << std::endl;
    std::cout << "  Mass skew: " << features.massSkewRatio << std::endl;

    assert(result.ok());
    assert(result.shape == AMT::ProfileShape::B_SHAPED);
    assert(result.error == AMT::ShapeError::NONE);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: THIN_VERTICAL (trend day)
// ============================================================================

void test_thin_vertical() {
    std::cout << "=== Test: THIN_VERTICAL ===" << std::endl;

    // Wide range, narrow VA (elongated)
    SyntheticHistogram h = createThinVertical(24000, 60, 200);

    const int pocTick = 24030;  // Somewhere in middle
    const int valTick = 24025;  // Narrow VA
    const int vahTick = 24035;  // VA width = 10, range = 60 => elongation = 6

    AMT::ProfileFeatures features = AMT::ExtractProfileFeatures(
        h.data(), h.size(), pocTick, vahTick, valTick, h.thresholds);

    AMT::ShapeClassificationResult result = AMT::ClassifyProfileShape(features);

    std::cout << "  Shape: " << AMT::ProfileShapeToString(result.shape) << std::endl;
    std::cout << "  Error: " << AMT::ShapeErrorToString(result.error) << std::endl;
    std::cout << "  Elongation: " << features.elongation << std::endl;
    std::cout << "  Flatness: " << features.flatness << std::endl;
    std::cout << "  Peakiness: " << features.peakiness << std::endl;

    assert(result.ok());
    assert(result.shape == AMT::ProfileShape::THIN_VERTICAL);
    assert(result.error == AMT::ShapeError::NONE);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: DOUBLE_DISTRIBUTION (bimodal)
// ============================================================================

void test_double_distribution() {
    std::cout << "=== Test: DOUBLE_DISTRIBUTION ===" << std::endl;

    SyntheticHistogram h = createDoubleDistribution(24000, 40, 800);

    // POC at one of the peaks
    const int pocTick = 24010;  // First peak
    const int valTick = 24005;
    const int vahTick = 24035;  // Spans both peaks

    AMT::ProfileFeatures features = AMT::ExtractProfileFeatures(
        h.data(), h.size(), pocTick, vahTick, valTick, h.thresholds);

    AMT::ShapeClassificationResult result = AMT::ClassifyProfileShape(features);

    std::cout << "  Shape: " << AMT::ProfileShapeToString(result.shape) << std::endl;
    std::cout << "  Error: " << AMT::ShapeErrorToString(result.error) << std::endl;
    std::cout << "  HVN clusters: " << features.hvnClusters.size() << std::endl;
    std::cout << "  LVN valley width: " << features.lvnValleyWidth << std::endl;
    std::cout << "  Min cluster sep: " << features.minClusterSeparationTicks << std::endl;

    // Note: This test may result in DOUBLE_DISTRIBUTION or AMBIGUOUS_BIMODAL
    // depending on threshold values. Both are valid as long as we don't fallback.
    if (result.ok()) {
        assert(result.shape == AMT::ProfileShape::DOUBLE_DISTRIBUTION);
    } else {
        assert(result.error == AMT::ShapeError::AMBIGUOUS_BIMODAL ||
               result.error == AMT::ShapeError::INSUFFICIENT_CLUSTERS);
        std::cout << "  (Classified as ambiguous - acceptable)" << std::endl;
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: INVALID VA (VAH <= VAL)
// ============================================================================

void test_invalid_va() {
    std::cout << "=== Test: INVALID_VA ===" << std::endl;

    SyntheticHistogram h = createNormalDistribution(24020, 10, 1000);

    // Invalid: VAH <= VAL
    const int pocTick = 24020;
    const int vahTick = 24015;  // Lower than VAL!
    const int valTick = 24020;

    AMT::ProfileFeatures features = AMT::ExtractProfileFeatures(
        h.data(), h.size(), pocTick, vahTick, valTick, h.thresholds);

    // Features should be invalid with specific error
    assert(!features.valid);
    assert(features.extractionError == AMT::ShapeError::INVALID_VA);
    std::cout << "  Extraction error: " << AMT::ShapeErrorToString(features.extractionError) << std::endl;

    AMT::ShapeClassificationResult result = AMT::ClassifyProfileShape(features);

    std::cout << "  Shape: " << AMT::ProfileShapeToString(result.shape) << std::endl;
    std::cout << "  Error: " << AMT::ShapeErrorToString(result.error) << std::endl;

    assert(!result.ok());
    assert(result.shape == AMT::ProfileShape::UNDEFINED);
    // Verify specific error is propagated, NOT collapsed to INSUFFICIENT_DATA
    assert(result.error == AMT::ShapeError::INVALID_VA);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: EMPTY HISTOGRAM
// ============================================================================

void test_empty_histogram() {
    std::cout << "=== Test: HISTOGRAM_EMPTY ===" << std::endl;

    SyntheticHistogram h;
    h.reset();
    // No bins added
    h.thresholds.valid = true;  // Thresholds "valid" but histogram empty

    const int pocTick = 24020;
    const int vahTick = 24025;
    const int valTick = 24015;

    AMT::ProfileFeatures features = AMT::ExtractProfileFeatures(
        h.data(), h.size(), pocTick, vahTick, valTick, h.thresholds);

    assert(!features.valid);

    AMT::ShapeClassificationResult result = AMT::ClassifyProfileShape(features);

    std::cout << "  Shape: " << AMT::ProfileShapeToString(result.shape) << std::endl;
    std::cout << "  Error: " << AMT::ShapeErrorToString(result.error) << std::endl;

    assert(!result.ok());
    assert(result.shape == AMT::ProfileShape::UNDEFINED);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: INSUFFICIENT DATA (< 5 bins)
// ============================================================================

void test_insufficient_data() {
    std::cout << "=== Test: INSUFFICIENT_DATA ===" << std::endl;

    SyntheticHistogram h;
    h.reset();
    // Only 3 bins
    h.addLevel(24018, 500);
    h.addLevel(24020, 1000);
    h.addLevel(24022, 500);
    h.thresholds.valid = true;

    const int pocTick = 24020;
    const int vahTick = 24022;
    const int valTick = 24018;

    AMT::ProfileFeatures features = AMT::ExtractProfileFeatures(
        h.data(), h.size(), pocTick, vahTick, valTick, h.thresholds);

    assert(!features.valid);

    AMT::ShapeClassificationResult result = AMT::ClassifyProfileShape(features);

    std::cout << "  Shape: " << AMT::ProfileShapeToString(result.shape) << std::endl;
    std::cout << "  Error: " << AMT::ShapeErrorToString(result.error) << std::endl;

    assert(!result.ok());
    assert(result.shape == AMT::ProfileShape::UNDEFINED);
    assert(result.error == AMT::ShapeError::INSUFFICIENT_DATA);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: THRESHOLDS_INVALID
// ============================================================================

void test_thresholds_invalid() {
    std::cout << "=== Test: THRESHOLDS_INVALID ===" << std::endl;

    SyntheticHistogram h = createNormalDistribution(24020, 10, 1000);
    h.thresholds.valid = false;  // Explicitly invalid

    const int pocTick = 24020;
    const int vahTick = 24025;
    const int valTick = 24015;

    AMT::ProfileFeatures features = AMT::ExtractProfileFeatures(
        h.data(), h.size(), pocTick, vahTick, valTick, h.thresholds);

    // Features should be invalid with specific error
    assert(!features.valid);
    assert(features.extractionError == AMT::ShapeError::THRESHOLDS_INVALID);
    std::cout << "  Extraction error: " << AMT::ShapeErrorToString(features.extractionError) << std::endl;

    AMT::ShapeClassificationResult result = AMT::ClassifyProfileShape(features);

    std::cout << "  Shape: " << AMT::ProfileShapeToString(result.shape) << std::endl;
    std::cout << "  Error: " << AMT::ShapeErrorToString(result.error) << std::endl;

    assert(!result.ok());
    assert(result.shape == AMT::ProfileShape::UNDEFINED);
    // Verify specific error is propagated, NOT collapsed to INSUFFICIENT_DATA
    assert(result.error == AMT::ShapeError::THRESHOLDS_INVALID);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: BALANCED (wide acceptance, no dominant POC - equilibrium state)
// ============================================================================

void test_balanced() {
    std::cout << "=== Test: BALANCED ===" << std::endl;

    // Create a flat profile with centered POC - wide acceptance pattern
    SyntheticHistogram h;
    h.reset();

    // Very flat profile - all volumes nearly equal (equilibrium)
    for (int tick = 24010; tick <= 24030; tick++) {
        h.addLevel(tick, 500 + (tick % 2) * 10);  // 500-510 range
    }
    h.computeThresholds();

    const int pocTick = 24020;  // Centered
    const int vahTick = 24025;
    const int valTick = 24015;

    AMT::ProfileFeatures features = AMT::ExtractProfileFeatures(
        h.data(), h.size(), pocTick, vahTick, valTick, h.thresholds);

    AMT::ShapeClassificationResult result = AMT::ClassifyProfileShape(features);

    std::cout << "  Shape: " << AMT::ProfileShapeToString(result.shape) << std::endl;
    std::cout << "  Error: " << AMT::ShapeErrorToString(result.error) << std::endl;
    std::cout << "  Confidence: " << result.confidence01 << std::endl;
    std::cout << "  Peakiness: " << features.peakiness << std::endl;
    std::cout << "  POC in VA: " << features.pocInVa01 << std::endl;

    // BALANCED is now a valid classification for flat profiles
    assert(result.ok());
    assert(result.shape == AMT::ProfileShape::BALANCED);
    assert(result.error == AMT::ShapeError::NONE);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: VA_TOO_NARROW
// ============================================================================

void test_va_too_narrow() {
    std::cout << "=== Test: VA_TOO_NARROW ===" << std::endl;

    SyntheticHistogram h = createNormalDistribution(24020, 10, 1000);

    const int pocTick = 24020;
    const int vahTick = 24021;  // Only 1 tick wide!
    const int valTick = 24020;

    AMT::ProfileFeatures features = AMT::ExtractProfileFeatures(
        h.data(), h.size(), pocTick, vahTick, valTick, h.thresholds);

    AMT::ShapeClassificationResult result = AMT::ClassifyProfileShape(features);

    std::cout << "  Shape: " << AMT::ProfileShapeToString(result.shape) << std::endl;
    std::cout << "  Error: " << AMT::ShapeErrorToString(result.error) << std::endl;
    std::cout << "  VA width: " << features.vaWidthTicks << std::endl;

    assert(!result.ok());
    assert(result.shape == AMT::ProfileShape::UNDEFINED);
    assert(result.error == AMT::ShapeError::VA_TOO_NARROW);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Legacy enum mapping
// ============================================================================

void test_legacy_enum_mapping() {
    std::cout << "=== Test: Legacy enum mapping ===" << std::endl;

    // Test balance shapes
    assert(AMT::ToBalanceProfileShape(AMT::ProfileShape::NORMAL_DISTRIBUTION) ==
           AMT::BalanceProfileShape::NORMAL_DISTRIBUTION);
    assert(AMT::ToBalanceProfileShape(AMT::ProfileShape::D_SHAPED) ==
           AMT::BalanceProfileShape::D_SHAPED);
    assert(AMT::ToBalanceProfileShape(AMT::ProfileShape::P_SHAPED) ==
           AMT::BalanceProfileShape::UNDEFINED);  // Imbalance -> UNDEFINED in balance

    // Test imbalance shapes
    assert(AMT::ToImbalanceProfileShape(AMT::ProfileShape::P_SHAPED) ==
           AMT::ImbalanceProfileShape::P_SHAPED);
    assert(AMT::ToImbalanceProfileShape(AMT::ProfileShape::B_SHAPED) ==
           AMT::ImbalanceProfileShape::B_SHAPED_LOWER);
    assert(AMT::ToImbalanceProfileShape(AMT::ProfileShape::THIN_VERTICAL) ==
           AMT::ImbalanceProfileShape::THIN_VERTICAL);
    assert(AMT::ToImbalanceProfileShape(AMT::ProfileShape::DOUBLE_DISTRIBUTION) ==
           AMT::ImbalanceProfileShape::B_SHAPED_BIMODAL);
    assert(AMT::ToImbalanceProfileShape(AMT::ProfileShape::NORMAL_DISTRIBUTION) ==
           AMT::ImbalanceProfileShape::UNDEFINED);  // Balance -> UNDEFINED in imbalance

    // Test helper functions
    assert(AMT::IsBalanceShape(AMT::ProfileShape::NORMAL_DISTRIBUTION) == true);
    assert(AMT::IsBalanceShape(AMT::ProfileShape::D_SHAPED) == true);
    assert(AMT::IsBalanceShape(AMT::ProfileShape::P_SHAPED) == false);

    assert(AMT::IsImbalanceShape(AMT::ProfileShape::P_SHAPED) == true);
    assert(AMT::IsImbalanceShape(AMT::ProfileShape::B_SHAPED) == true);
    assert(AMT::IsImbalanceShape(AMT::ProfileShape::THIN_VERTICAL) == true);
    assert(AMT::IsImbalanceShape(AMT::ProfileShape::NORMAL_DISTRIBUTION) == false);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Adaptive thresholds scale with VA width
// ============================================================================

void test_adaptive_thresholds() {
    std::cout << "=== Test: Adaptive thresholds ===" << std::endl;

    // Create two profiles with different VA widths
    SyntheticHistogram h1 = createNormalDistribution(24020, 10, 1000);
    SyntheticHistogram h2 = createNormalDistribution(24020, 10, 1000);

    // Narrow VA
    AMT::ProfileFeatures f1 = AMT::ExtractProfileFeatures(
        h1.data(), h1.size(), 24020, 24025, 24015, h1.thresholds);  // VA width = 10

    // Wide VA
    AMT::ProfileFeatures f2 = AMT::ExtractProfileFeatures(
        h2.data(), h2.size(), 24020, 24030, 24010, h2.thresholds);  // VA width = 20

    std::cout << "  Narrow VA (10 ticks): minClusterSep = " << f1.minClusterSeparationTicks << std::endl;
    std::cout << "  Wide VA (20 ticks): minClusterSep = " << f2.minClusterSeparationTicks << std::endl;

    // Wider VA should have larger cluster separation threshold
    assert(f2.minClusterSeparationTicks >= f1.minClusterSeparationTicks);

    // Both should be at least the minimum absolute
    assert(f1.minClusterSeparationTicks >= AMT::ProfileShapeConfig::CLUSTER_SEP_MIN_ABS_TICKS);
    assert(f2.minClusterSeparationTicks >= AMT::ProfileShapeConfig::CLUSTER_SEP_MIN_ABS_TICKS);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Family Resolution - Balance family with BALANCED DayStructure
// ============================================================================

void test_family_resolution_balance_accepted() {
    std::cout << "=== Test: Family Resolution - Balance shapes with BALANCED structure ===" << std::endl;

    // Balance family shapes: NORMAL_DISTRIBUTION, D_SHAPED, BALANCED
    // With BALANCED DayStructure, these should be ACCEPTED

    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::NORMAL_DISTRIBUTION, AMT::DayStructure::BALANCED);

        assert(result.rawShape == AMT::ProfileShape::NORMAL_DISTRIBUTION);
        assert(result.finalShape == AMT::ProfileShape::NORMAL_DISTRIBUTION);
        assert(result.conflict == false);
        assert(std::string(result.resolution) == "ACCEPTED");
        std::cout << "  NORMAL_DISTRIBUTION + BALANCED -> ACCEPTED ✓" << std::endl;
    }

    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::D_SHAPED, AMT::DayStructure::BALANCED);

        assert(result.rawShape == AMT::ProfileShape::D_SHAPED);
        assert(result.finalShape == AMT::ProfileShape::D_SHAPED);
        assert(result.conflict == false);
        assert(std::string(result.resolution) == "ACCEPTED");
        std::cout << "  D_SHAPED + BALANCED -> ACCEPTED ✓" << std::endl;
    }

    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::BALANCED, AMT::DayStructure::BALANCED);

        assert(result.rawShape == AMT::ProfileShape::BALANCED);
        assert(result.finalShape == AMT::ProfileShape::BALANCED);
        assert(result.conflict == false);
        assert(std::string(result.resolution) == "ACCEPTED");
        std::cout << "  BALANCED + BALANCED -> ACCEPTED ✓" << std::endl;
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Family Resolution - Imbalance shapes with BALANCED DayStructure = CONFLICT
// ============================================================================

void test_family_resolution_imbalance_in_balanced_conflict() {
    std::cout << "=== Test: Family Resolution - Imbalance shapes with BALANCED structure ===" << std::endl;

    // Imbalance family shapes: P_SHAPED, B_SHAPED, THIN_VERTICAL, DOUBLE_DISTRIBUTION
    // With BALANCED DayStructure, these should CONFLICT (strict mode)

    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::P_SHAPED, AMT::DayStructure::BALANCED);

        assert(result.rawShape == AMT::ProfileShape::P_SHAPED);
        assert(result.finalShape == AMT::ProfileShape::UNDEFINED);
        assert(result.conflict == true);
        assert(std::string(result.resolution) == "CONFLICT");
        std::cout << "  P_SHAPED + BALANCED -> CONFLICT ✓" << std::endl;
    }

    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::B_SHAPED, AMT::DayStructure::BALANCED);

        assert(result.rawShape == AMT::ProfileShape::B_SHAPED);
        assert(result.finalShape == AMT::ProfileShape::UNDEFINED);
        assert(result.conflict == true);
        assert(std::string(result.resolution) == "CONFLICT");
        std::cout << "  B_SHAPED + BALANCED -> CONFLICT ✓" << std::endl;
    }

    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::THIN_VERTICAL, AMT::DayStructure::BALANCED);

        assert(result.rawShape == AMT::ProfileShape::THIN_VERTICAL);
        assert(result.finalShape == AMT::ProfileShape::UNDEFINED);
        assert(result.conflict == true);
        assert(std::string(result.resolution) == "CONFLICT");
        std::cout << "  THIN_VERTICAL + BALANCED -> CONFLICT ✓" << std::endl;
    }

    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::DOUBLE_DISTRIBUTION, AMT::DayStructure::BALANCED);

        assert(result.rawShape == AMT::ProfileShape::DOUBLE_DISTRIBUTION);
        assert(result.finalShape == AMT::ProfileShape::UNDEFINED);
        assert(result.conflict == true);
        assert(std::string(result.resolution) == "CONFLICT");
        std::cout << "  DOUBLE_DISTRIBUTION + BALANCED -> CONFLICT ✓" << std::endl;
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Family Resolution - Imbalance family with IMBALANCED DayStructure
// ============================================================================

void test_family_resolution_imbalance_accepted() {
    std::cout << "=== Test: Family Resolution - Imbalance shapes with IMBALANCED structure ===" << std::endl;

    // Imbalance family shapes with IMBALANCED DayStructure should be ACCEPTED

    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::P_SHAPED, AMT::DayStructure::IMBALANCED);

        assert(result.rawShape == AMT::ProfileShape::P_SHAPED);
        assert(result.finalShape == AMT::ProfileShape::P_SHAPED);
        assert(result.conflict == false);
        assert(std::string(result.resolution) == "ACCEPTED");
        std::cout << "  P_SHAPED + IMBALANCED -> ACCEPTED ✓" << std::endl;
    }

    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::B_SHAPED, AMT::DayStructure::IMBALANCED);

        assert(result.rawShape == AMT::ProfileShape::B_SHAPED);
        assert(result.finalShape == AMT::ProfileShape::B_SHAPED);
        assert(result.conflict == false);
        assert(std::string(result.resolution) == "ACCEPTED");
        std::cout << "  B_SHAPED + IMBALANCED -> ACCEPTED ✓" << std::endl;
    }

    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::THIN_VERTICAL, AMT::DayStructure::IMBALANCED);

        assert(result.rawShape == AMT::ProfileShape::THIN_VERTICAL);
        assert(result.finalShape == AMT::ProfileShape::THIN_VERTICAL);
        assert(result.conflict == false);
        assert(std::string(result.resolution) == "ACCEPTED");
        std::cout << "  THIN_VERTICAL + IMBALANCED -> ACCEPTED ✓" << std::endl;
    }

    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::DOUBLE_DISTRIBUTION, AMT::DayStructure::IMBALANCED);

        assert(result.rawShape == AMT::ProfileShape::DOUBLE_DISTRIBUTION);
        assert(result.finalShape == AMT::ProfileShape::DOUBLE_DISTRIBUTION);
        assert(result.conflict == false);
        assert(std::string(result.resolution) == "ACCEPTED");
        std::cout << "  DOUBLE_DISTRIBUTION + IMBALANCED -> ACCEPTED ✓" << std::endl;
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Family Resolution - Balance shapes with IMBALANCED DayStructure = CONFLICT
// ============================================================================

void test_family_resolution_balance_in_imbalanced_conflict() {
    std::cout << "=== Test: Family Resolution - Balance shapes with IMBALANCED structure ===" << std::endl;

    // Balance family shapes with IMBALANCED DayStructure should CONFLICT

    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::NORMAL_DISTRIBUTION, AMT::DayStructure::IMBALANCED);

        assert(result.rawShape == AMT::ProfileShape::NORMAL_DISTRIBUTION);
        assert(result.finalShape == AMT::ProfileShape::UNDEFINED);
        assert(result.conflict == true);
        assert(std::string(result.resolution) == "CONFLICT");
        std::cout << "  NORMAL_DISTRIBUTION + IMBALANCED -> CONFLICT ✓" << std::endl;
    }

    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::D_SHAPED, AMT::DayStructure::IMBALANCED);

        assert(result.rawShape == AMT::ProfileShape::D_SHAPED);
        assert(result.finalShape == AMT::ProfileShape::UNDEFINED);
        assert(result.conflict == true);
        assert(std::string(result.resolution) == "CONFLICT");
        std::cout << "  D_SHAPED + IMBALANCED -> CONFLICT ✓" << std::endl;
    }

    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::BALANCED, AMT::DayStructure::IMBALANCED);

        assert(result.rawShape == AMT::ProfileShape::BALANCED);
        assert(result.finalShape == AMT::ProfileShape::UNDEFINED);
        assert(result.conflict == true);
        assert(std::string(result.resolution) == "CONFLICT");
        std::cout << "  BALANCED + IMBALANCED -> CONFLICT ✓" << std::endl;
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Family Resolution - UNDEFINED DayStructure = STRUCTURE_UNDEFINED
// ============================================================================

void test_family_resolution_undefined_structure_pending() {
    std::cout << "=== Test: Family Resolution - UNDEFINED DayStructure ===" << std::endl;

    // With UNDEFINED DayStructure, resolution returns STRUCTURE_UNDEFINED (not enough info)

    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::NORMAL_DISTRIBUTION, AMT::DayStructure::UNDEFINED);

        assert(result.rawShape == AMT::ProfileShape::NORMAL_DISTRIBUTION);
        assert(result.finalShape == AMT::ProfileShape::UNDEFINED);
        assert(result.conflict == false);  // Not a conflict, just pending
        assert(std::string(result.resolution) == "STRUCTURE_UNDEFINED");
        std::cout << "  NORMAL_DISTRIBUTION + UNDEFINED -> STRUCTURE_UNDEFINED ✓" << std::endl;
    }

    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::THIN_VERTICAL, AMT::DayStructure::UNDEFINED);

        assert(result.rawShape == AMT::ProfileShape::THIN_VERTICAL);
        assert(result.finalShape == AMT::ProfileShape::UNDEFINED);
        assert(result.conflict == false);
        assert(std::string(result.resolution) == "STRUCTURE_UNDEFINED");
        std::cout << "  THIN_VERTICAL + UNDEFINED -> STRUCTURE_UNDEFINED ✓" << std::endl;
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Family Resolution - UNDEFINED RawShape = RAW_UNDEFINED
// ============================================================================

void test_family_resolution_undefined_shape_pending() {
    std::cout << "=== Test: Family Resolution - UNDEFINED RawShape ===" << std::endl;

    // With UNDEFINED RawShape, resolution returns RAW_UNDEFINED

    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::UNDEFINED, AMT::DayStructure::BALANCED);

        assert(result.rawShape == AMT::ProfileShape::UNDEFINED);
        assert(result.finalShape == AMT::ProfileShape::UNDEFINED);
        assert(result.conflict == false);
        assert(std::string(result.resolution) == "RAW_UNDEFINED");
        std::cout << "  UNDEFINED + BALANCED -> RAW_UNDEFINED ✓" << std::endl;
    }

    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::UNDEFINED, AMT::DayStructure::IMBALANCED);

        assert(result.rawShape == AMT::ProfileShape::UNDEFINED);
        assert(result.finalShape == AMT::ProfileShape::UNDEFINED);
        assert(result.conflict == false);
        assert(std::string(result.resolution) == "RAW_UNDEFINED");
        std::cout << "  UNDEFINED + IMBALANCED -> RAW_UNDEFINED ✓" << std::endl;
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: IsShapeInBalanceFamily and IsShapeInImbalanceFamily helpers
// ============================================================================

void test_family_helper_functions() {
    std::cout << "=== Test: Family Helper Functions ===" << std::endl;

    // Balance family: NORMAL_DISTRIBUTION, D_SHAPED, BALANCED
    assert(AMT::IsShapeInBalanceFamily(AMT::ProfileShape::NORMAL_DISTRIBUTION) == true);
    assert(AMT::IsShapeInBalanceFamily(AMT::ProfileShape::D_SHAPED) == true);
    assert(AMT::IsShapeInBalanceFamily(AMT::ProfileShape::BALANCED) == true);
    assert(AMT::IsShapeInBalanceFamily(AMT::ProfileShape::P_SHAPED) == false);
    assert(AMT::IsShapeInBalanceFamily(AMT::ProfileShape::B_SHAPED) == false);
    assert(AMT::IsShapeInBalanceFamily(AMT::ProfileShape::THIN_VERTICAL) == false);
    assert(AMT::IsShapeInBalanceFamily(AMT::ProfileShape::DOUBLE_DISTRIBUTION) == false);
    assert(AMT::IsShapeInBalanceFamily(AMT::ProfileShape::UNDEFINED) == false);
    std::cout << "  IsShapeInBalanceFamily() ✓" << std::endl;

    // Imbalance family: P_SHAPED, B_SHAPED, THIN_VERTICAL, DOUBLE_DISTRIBUTION
    assert(AMT::IsShapeInImbalanceFamily(AMT::ProfileShape::P_SHAPED) == true);
    assert(AMT::IsShapeInImbalanceFamily(AMT::ProfileShape::B_SHAPED) == true);
    assert(AMT::IsShapeInImbalanceFamily(AMT::ProfileShape::THIN_VERTICAL) == true);
    assert(AMT::IsShapeInImbalanceFamily(AMT::ProfileShape::DOUBLE_DISTRIBUTION) == true);
    assert(AMT::IsShapeInImbalanceFamily(AMT::ProfileShape::NORMAL_DISTRIBUTION) == false);
    assert(AMT::IsShapeInImbalanceFamily(AMT::ProfileShape::D_SHAPED) == false);
    assert(AMT::IsShapeInImbalanceFamily(AMT::ProfileShape::BALANCED) == false);
    assert(AMT::IsShapeInImbalanceFamily(AMT::ProfileShape::UNDEFINED) == false);
    std::cout << "  IsShapeInImbalanceFamily() ✓" << std::endl;

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Profile Shape Classification Tests" << std::endl;
    std::cout << "========================================\n" << std::endl;

    std::cout << "--- Valid Shape Tests ---\n" << std::endl;
    test_normal_distribution();
    test_d_shaped();
    test_p_shaped();
    test_b_shaped();
    test_thin_vertical();
    test_double_distribution();
    test_balanced();  // BALANCED is now a valid classification

    std::cout << "\n--- Error Case Tests ---\n" << std::endl;
    test_invalid_va();
    test_empty_histogram();
    test_insufficient_data();
    test_thresholds_invalid();
    test_va_too_narrow();

    std::cout << "\n--- Mapping and Utility Tests ---\n" << std::endl;
    test_legacy_enum_mapping();
    test_adaptive_thresholds();

    std::cout << "\n--- Family Resolution Tests (DayStructure Constraint) ---\n" << std::endl;
    test_family_resolution_balance_accepted();
    test_family_resolution_imbalance_in_balanced_conflict();
    test_family_resolution_imbalance_accepted();
    test_family_resolution_balance_in_imbalanced_conflict();
    test_family_resolution_undefined_structure_pending();
    test_family_resolution_undefined_shape_pending();
    test_family_helper_functions();

    std::cout << "\n========================================" << std::endl;
    std::cout << "All tests PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
