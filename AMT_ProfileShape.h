// ============================================================================
// AMT_ProfileShape.h
// Deterministic profile shape classification using HVN/LVN cluster detection
// Pure classifier module - no Sierra Chart dependencies
//
// SSOT: ProfileShape enum in amt_core.h is the canonical shape representation.
// This module provides:
//   1. ProfileFeatures DTO (extracted from histogram + VA inputs)
//   2. ShapeError enum with explicit failure reasons
//   3. ShapeClassificationResult (shape + error + reason)
//   4. ExtractProfileFeatures() - pure feature extraction
//   5. ClassifyProfileShape() - deterministic decision tree
//
// NO FALLBACKS: If classification fails or is ambiguous, returns UNDEFINED
// with a specific ShapeError code. Never assigns a shape "just to have one".
//
// ADAPTIVE THRESHOLDS: All thresholds are derived from VA/range proportions,
// not instrument-specific tick counts.
// ============================================================================

#ifndef AMT_PROFILESHAPE_H
#define AMT_PROFILESHAPE_H

#include "amt_core.h"     // ProfileShape, VolumeThresholds, PriceToTicks
#include "AMT_config.h"   // PriceToTicks (canonical)
#include "AMT_Patterns.h" // BalanceProfileShape, ImbalanceProfileShape (legacy enums)
#include <cmath>
#include <algorithm>
#include <vector>
#include <cassert>
#include <type_traits>

namespace AMT {

// ============================================================================
// TYPE TRAIT: Detect if a type has a .Volume member (for duck-typing volume maps)
// Used by ValidateDoubleDistribution to handle different volume profile types
// ============================================================================
template<typename T, typename = void>
struct has_volume_member : std::false_type {};

template<typename T>
struct has_volume_member<T, std::void_t<decltype(std::declval<T>().Volume)>> : std::true_type {};

// ============================================================================
// ADAPTIVE THRESHOLD CONFIGURATION
// All thresholds are proportional to VA width or range - NO instrument-specific values
// ============================================================================

namespace ProfileShapeConfig {
    // =========================================================================
    // POC POSITION BANDS (x_poc: POC position in full profile range [0,1])
    // =========================================================================
    // x_poc = (POC - P_lo) / R, where R = profile range
    // POC in center band = balance family (NORMAL, D, or BALANCED)
    // POC outside center band = imbalance family (P or B shaped)
    constexpr float C_MIN = 0.35f;   // x_poc < C_MIN = B territory (POC low in range)
    constexpr float C_MAX = 0.65f;   // x_poc > C_MAX = P territory (POC high in range)

    // =========================================================================
    // BREADTH THRESHOLDS (w = W_va / R = VA width fraction)
    // =========================================================================
    // w measures acceptance breadth: higher = wider acceptance, lower = trend-like
    // These thresholds are ordered: W_THIN < W_BAL to guarantee non-overlap
    constexpr float W_THIN = 0.40f;  // w <= W_THIN = THIN_VERTICAL (narrow acceptance)
    constexpr float W_BAL = 0.50f;   // w >= W_BAL required for BALANCED (wide acceptance)
    // Note: W_BAL > W_THIN guarantees BALANCED and THIN_VERTICAL never overlap

    // =========================================================================
    // PEAKINESS THRESHOLDS (k = POC volume / VA mean volume)
    // =========================================================================
    // k measures single-node dominance: higher = sharper peak
    // Ordered: K_MOD < K_SHARP to guarantee non-overlap
    constexpr float K_MOD = 1.5f;    // k >= K_MOD = moderate peak (D_SHAPED candidate)
    constexpr float K_SHARP = 2.0f;  // k >= K_SHARP = sharp peak (NORMAL_DISTRIBUTION)
    // BALANCED requires k < K_MOD (no dominant peak)

    // =========================================================================
    // ASYMMETRY THRESHOLDS (a = POC offset from VA midpoint / W_va)
    // =========================================================================
    // a ranges [-0.5, 0.5], |a| measures how far POC is from VA center
    // Ordered: A_BAL < A_D to create intentional ambiguity gap
    constexpr float A_BAL = 0.10f;   // |a| <= A_BAL = symmetric (NORMAL or BALANCED)
    constexpr float A_D = 0.15f;     // |a| >= A_D = asymmetric (D_SHAPED)
    // Gap (A_BAL, A_D) is intentional no-man's-land → UNDEFINED

    // =========================================================================
    // BIMODAL DETECTION (DOUBLE_DISTRIBUTION)
    // Uses HVN clusters separated by LVN valley
    // =========================================================================
    constexpr float CLUSTER_SEP_VA_RATIO = 0.25f;    // Min separation: 25% of VA width
    constexpr int CLUSTER_SEP_MIN_ABS_TICKS = 3;     // Min absolute separation
    constexpr float VALLEY_WIDTH_MIN_RATIO = 0.2f;   // Valley >= 20% of gap width
    constexpr float CLUSTER_DOMINANCE_MIN = 0.25f;   // Each cluster >= 25% of mass
    constexpr float MIN_TOTAL_HVN_MASS_RATIO = 0.40f; // Combined >= 40% of total

    // =========================================================================
    // THIN VERTICAL (legacy compatibility - prefer W_THIN)
    // =========================================================================
    constexpr float ELONGATION_MIN = 2.5f;    // e >= 2.5 = thin (equivalent to w <= 0.4)
    constexpr float POC_FLATNESS_MAX = 1.5f;  // Optional: exclude spike-peaked from thin

    // =========================================================================
    // VOLUME SKEW (optional P/B confirmation)
    // =========================================================================
    constexpr float MASS_SKEW_THRESHOLD = 1.5f;  // > 1.5 or < 0.67 = significant skew

    // =========================================================================
    // MINIMUM DATA REQUIREMENTS
    // =========================================================================
    constexpr int MIN_HISTOGRAM_BINS = 5;
    constexpr int MIN_VA_WIDTH_TICKS = 2;
    constexpr int MIN_HVN_CLUSTER_BINS = 2;
}

// ============================================================================
// SHAPE ERROR UTILITIES
// ShapeError enum is defined in amt_core.h (SSOT for core enums)
// ============================================================================

inline const char* ShapeErrorToString(ShapeError e) {
    switch (e) {
        case ShapeError::NONE: return "NONE";
        case ShapeError::INVALID_VA: return "INVALID_VA";
        case ShapeError::HISTOGRAM_EMPTY: return "HISTOGRAM_EMPTY";
        case ShapeError::INSUFFICIENT_DATA: return "INSUFFICIENT_DATA";
        case ShapeError::THRESHOLDS_INVALID: return "THRESHOLDS_INVALID";
        case ShapeError::AMBIGUOUS_BIMODAL: return "AMBIGUOUS_BIMODAL";
        case ShapeError::INCONCLUSIVE_BALANCE: return "INCONCLUSIVE_BALANCE";
        case ShapeError::VA_TOO_NARROW: return "VA_TOO_NARROW";
        case ShapeError::INSUFFICIENT_CLUSTERS: return "INSUFFICIENT_CLUSTERS";
    }
    return "UNKNOWN";
}

// ============================================================================
// HVN CLUSTER (internal representation for bimodal detection)
// ============================================================================

struct HVNCluster {
    int startTick = 0;          // First tick in cluster
    int endTick = 0;            // Last tick in cluster
    int centerTick = 0;         // Volume-weighted center tick
    double totalVolume = 0.0;   // Sum of volume in cluster
    int binCount = 0;           // Number of bins in cluster

    int Width() const { return endTick - startTick + 1; }
};

// ============================================================================
// PROFILE FEATURES DTO
// Extracted once from histogram - passed to classifier
// ============================================================================

struct ProfileFeatures {
    // =========================================================================
    // CORE LEVELS (tick-based, using canonical PriceToTicks)
    // =========================================================================
    int pocTick = 0;
    int vahTick = 0;
    int valTick = 0;
    int profileHighTick = 0;
    int profileLowTick = 0;

    // =========================================================================
    // DERIVED SCALARS
    // =========================================================================
    int vaWidthTicks = 0;       // W_va = VAH - VAL
    int rangeTicks = 0;         // R = profileHigh - profileLow
    float pocInVa01 = 0.0f;     // (POC - VAL) / W_va, clamped [0,1] (legacy)

    // =========================================================================
    // NORMALIZED METRICS (per formal specification)
    // =========================================================================
    // x_poc: POC position in full profile range [0,1]
    // x_poc = (POC - P_lo) / R
    float pocInRange = 0.0f;

    // w: Value Area width fraction (breadth of acceptance) (0,1]
    // w = W_va / R = 1/elongation
    // Higher w = wider acceptance, lower w = narrower (trend-like)
    float breadth = 0.0f;

    // a: VA asymmetry - signed POC offset from VA midpoint [-0.5, 0.5]
    // a = (POC - (VAH + VAL)/2) / W_va
    // |a| = 0 means POC at VA center, |a| = 0.5 means POC at VA edge
    float asymmetry = 0.0f;

    // =========================================================================
    // VOLUME STATISTICS
    // =========================================================================
    double pocVolume = 0.0;     // Volume at POC tick
    double totalVolume = 0.0;   // Total profile volume
    double vaVolume = 0.0;      // Volume within VA
    double vaMean = 0.0;        // Mean volume within VA
    float vaMassRatio = 0.0f;   // vaVolume / totalVolume

    // =========================================================================
    // PEAKINESS / FLATNESS
    // =========================================================================
    float peakiness = 0.0f;     // pocVolume / vaMean (within VA)
    float flatness = 0.0f;      // maxVolume / profileMean (whole profile)
    float elongation = 0.0f;    // rangeTicks / vaWidthTicks

    // =========================================================================
    // VOLUME SKEW (for P/B confirmation)
    // =========================================================================
    double volumeAbovePOC = 0.0;  // Volume in VA above POC
    double volumeBelowPOC = 0.0;  // Volume in VA below POC
    float massSkewRatio = 1.0f;   // volumeAbovePOC / volumeBelowPOC

    // =========================================================================
    // HVN/LVN CLUSTER DETECTION (using thresholds)
    // =========================================================================
    std::vector<HVNCluster> hvnClusters;  // Contiguous HVN regions
    int lvnValleyWidth = 0;               // Width of LVN region between two largest clusters

    // =========================================================================
    // THRESHOLDS (copied from VolumeThresholds for reference)
    // =========================================================================
    double hvnThreshold = 0.0;
    double lvnThreshold = 0.0;
    double mean = 0.0;

    // =========================================================================
    // VALIDATION
    // =========================================================================
    bool valid = false;
    int binCount = 0;
    ShapeError extractionError = ShapeError::NONE;  // Specific error from extraction phase

    // =========================================================================
    // ADAPTIVE THRESHOLDS (computed from VA/range)
    // =========================================================================
    int minClusterSeparationTicks = 0;  // Derived: max(MIN_ABS, VA * ratio)
};

// ============================================================================
// CLASSIFICATION RESULT
// ============================================================================

struct ShapeClassificationResult {
    ProfileShape shape = ProfileShape::UNDEFINED;
    ShapeError error = ShapeError::NONE;
    float confidence01 = 0.0f;    // [0,1] deterministic confidence
    const char* reason = "";      // Short reason string for logging

    bool ok() const {
        return error == ShapeError::NONE && shape != ProfileShape::UNDEFINED;
    }
};

// ============================================================================
// FEATURE EXTRACTION
// Extracts ProfileFeatures from histogram array using VolumeThresholds
// Template to work with s_VolumeAtPriceV2 or test mocks
// ============================================================================

template<typename VolumeAtPriceT>
inline ProfileFeatures ExtractProfileFeatures(
    const VolumeAtPriceT* histogram,
    int numPrices,
    int pocTick,
    int vahTick,
    int valTick,
    const VolumeThresholds& thresholds)
{
    using namespace ProfileShapeConfig;
    ProfileFeatures f;

    // =========================================================================
    // VALIDATION GATES (set specific error, not just valid=false)
    // =========================================================================
    if (!histogram || numPrices < MIN_HISTOGRAM_BINS) {
        f.valid = false;
        f.extractionError = ShapeError::INSUFFICIENT_DATA;
        return f;
    }

    if (vahTick <= valTick) {
        f.valid = false;
        f.extractionError = ShapeError::INVALID_VA;
        return f;
    }

    if (!thresholds.valid) {
        f.valid = false;
        f.extractionError = ShapeError::THRESHOLDS_INVALID;
        return f;
    }

    // =========================================================================
    // STORE INPUTS
    // =========================================================================
    f.pocTick = pocTick;
    f.vahTick = vahTick;
    f.valTick = valTick;
    f.vaWidthTicks = vahTick - valTick;
    f.binCount = numPrices;

    // Copy thresholds
    f.hvnThreshold = thresholds.hvnThreshold;
    f.lvnThreshold = thresholds.lvnThreshold;
    f.mean = thresholds.mean;

    // Compute adaptive cluster separation threshold
    const int adaptiveSep = static_cast<int>(std::ceil(f.vaWidthTicks * CLUSTER_SEP_VA_RATIO));
    f.minClusterSeparationTicks = (std::max)(CLUSTER_SEP_MIN_ABS_TICKS, adaptiveSep);

    // =========================================================================
    // PASS 1: Profile bounds, total volume, VA volume, POC volume
    // =========================================================================
    int minTick = INT_MAX, maxTick = INT_MIN;
    double totalVol = 0.0;
    double maxVol = 0.0;
    double vaVol = 0.0;
    int vaBinCount = 0;
    double volAbovePOC = 0.0;
    double volBelowPOC = 0.0;
    double pocVol = 0.0;

    // Build sorted bin list for cluster detection
    std::vector<std::pair<int, double>> sortedBins;
    sortedBins.reserve(numPrices);

    for (int i = 0; i < numPrices; ++i) {
        const int tick = histogram[i].PriceInTicks;
        const double vol = static_cast<double>(histogram[i].Volume);

        sortedBins.push_back({tick, vol});

        if (tick < minTick) minTick = tick;
        if (tick > maxTick) maxTick = tick;
        totalVol += vol;
        if (vol > maxVol) maxVol = vol;

        // Within VA?
        if (tick >= valTick && tick <= vahTick) {
            vaVol += vol;
            vaBinCount++;

            // Skew calculation
            if (tick > pocTick) {
                volAbovePOC += vol;
            } else if (tick < pocTick) {
                volBelowPOC += vol;
            }
            // POC tick itself doesn't count for skew
        }

        // POC volume
        if (tick == pocTick) {
            pocVol = vol;
        }
    }

    if (totalVol <= 0.0 || maxVol <= 0.0) {
        f.valid = false;
        return f;
    }

    f.profileHighTick = maxTick;
    f.profileLowTick = minTick;
    f.rangeTicks = maxTick - minTick;
    f.totalVolume = totalVol;
    f.vaVolume = vaVol;
    f.pocVolume = pocVol;

    // POC position within VA [0,1] (legacy metric)
    if (f.vaWidthTicks > 0) {
        float raw = static_cast<float>(pocTick - valTick) / static_cast<float>(f.vaWidthTicks);
        f.pocInVa01 = (std::max)(0.0f, (std::min)(1.0f, raw));
    }

    // =========================================================================
    // NORMALIZED METRICS (per formal specification)
    // =========================================================================

    // x_poc: POC position in full profile range [0,1]
    if (f.rangeTicks > 0) {
        f.pocInRange = static_cast<float>(pocTick - f.profileLowTick) /
                       static_cast<float>(f.rangeTicks);
        f.pocInRange = (std::max)(0.0f, (std::min)(1.0f, f.pocInRange));
    }

    // w: Breadth = VA width / Range (0,1]
    if (f.rangeTicks > 0) {
        f.breadth = static_cast<float>(f.vaWidthTicks) / static_cast<float>(f.rangeTicks);
    }

    // a: Asymmetry = (POC - VA_midpoint) / W_va, range [-0.5, 0.5]
    if (f.vaWidthTicks > 0) {
        const float vaMidpoint = static_cast<float>(valTick + vahTick) / 2.0f;
        f.asymmetry = (static_cast<float>(pocTick) - vaMidpoint) /
                      static_cast<float>(f.vaWidthTicks);
        // Clamp to valid range
        f.asymmetry = (std::max)(-0.5f, (std::min)(0.5f, f.asymmetry));
    }

    // Volume mass ratio
    f.vaMassRatio = static_cast<float>(vaVol / totalVol);

    // VA mean
    f.vaMean = (vaBinCount > 0) ? (vaVol / vaBinCount) : 1.0;

    // Peakiness within VA
    f.peakiness = (f.vaMean > 0) ? static_cast<float>(pocVol / f.vaMean) : 0.0f;

    // Flatness (whole profile)
    const double profileMean = totalVol / numPrices;
    f.flatness = static_cast<float>(maxVol / profileMean);

    // Elongation
    if (f.vaWidthTicks > 0) {
        f.elongation = static_cast<float>(f.rangeTicks) / static_cast<float>(f.vaWidthTicks);
    }

    // Skew ratio
    f.volumeAbovePOC = volAbovePOC;
    f.volumeBelowPOC = volBelowPOC;
    if (volBelowPOC > 0) {
        f.massSkewRatio = static_cast<float>(volAbovePOC / volBelowPOC);
    } else if (volAbovePOC > 0) {
        f.massSkewRatio = 10.0f;  // Cap at high value
    } else {
        f.massSkewRatio = 1.0f;   // No skew data
    }

    // =========================================================================
    // PASS 2: HVN CLUSTER DETECTION (using thresholds)
    // Find contiguous regions where volume >= hvnThreshold
    // =========================================================================
    std::sort(sortedBins.begin(), sortedBins.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    HVNCluster currentCluster;
    bool inCluster = false;
    double clusterWeightedSum = 0.0;

    for (const auto& [tick, vol] : sortedBins) {
        const bool isHVN = (vol >= f.hvnThreshold);

        if (isHVN) {
            if (!inCluster) {
                // Start new cluster
                currentCluster = HVNCluster();
                currentCluster.startTick = tick;
                currentCluster.endTick = tick;
                currentCluster.totalVolume = vol;
                currentCluster.binCount = 1;
                clusterWeightedSum = tick * vol;
                inCluster = true;
            } else {
                // Extend current cluster
                currentCluster.endTick = tick;
                currentCluster.totalVolume += vol;
                currentCluster.binCount++;
                clusterWeightedSum += tick * vol;
            }
        } else {
            if (inCluster) {
                // End current cluster
                if (currentCluster.binCount >= MIN_HVN_CLUSTER_BINS) {
                    currentCluster.centerTick = static_cast<int>(
                        std::round(clusterWeightedSum / currentCluster.totalVolume));
                    f.hvnClusters.push_back(currentCluster);
                }
                inCluster = false;
            }
        }
    }

    // Don't forget last cluster if still in one
    if (inCluster && currentCluster.binCount >= MIN_HVN_CLUSTER_BINS) {
        currentCluster.centerTick = static_cast<int>(
            std::round(clusterWeightedSum / currentCluster.totalVolume));
        f.hvnClusters.push_back(currentCluster);
    }

    // =========================================================================
    // PASS 3: VALLEY DETECTION (if 2+ clusters)
    // Find LVN region between the two largest HVN clusters
    // =========================================================================
    if (f.hvnClusters.size() >= 2) {
        // Sort clusters by volume (descending)
        std::vector<size_t> clusterIndices(f.hvnClusters.size());
        for (size_t i = 0; i < clusterIndices.size(); ++i) clusterIndices[i] = i;

        std::sort(clusterIndices.begin(), clusterIndices.end(),
                  [&f](size_t a, size_t b) {
                      return f.hvnClusters[a].totalVolume > f.hvnClusters[b].totalVolume;
                  });

        const HVNCluster& cluster1 = f.hvnClusters[clusterIndices[0]];
        const HVNCluster& cluster2 = f.hvnClusters[clusterIndices[1]];

        // Order by tick position
        int lowClusterEnd = (std::min)(cluster1.endTick, cluster2.endTick);
        int highClusterStart = (std::max)(cluster1.startTick, cluster2.startTick);

        if (lowClusterEnd < highClusterStart) {
            // Count LVN bins in the valley
            int lvnCount = 0;
            for (const auto& [tick, vol] : sortedBins) {
                if (tick > lowClusterEnd && tick < highClusterStart) {
                    if (vol <= f.lvnThreshold) {
                        lvnCount++;
                    }
                }
            }
            f.lvnValleyWidth = lvnCount;
        }
    }

    f.valid = true;
    return f;
}

// ============================================================================
// CLASSIFIER (pure decision tree, deterministic, no fallbacks)
// ============================================================================

inline ShapeClassificationResult ClassifyProfileShape(const ProfileFeatures& f)
{
    using namespace ProfileShapeConfig;
    ShapeClassificationResult r;

    // =========================================================================
    // VALIDATION GATES (propagate extraction error if set)
    // =========================================================================
    if (!f.valid) {
        // Propagate specific extraction error, don't collapse to INSUFFICIENT_DATA
        r.error = (f.extractionError != ShapeError::NONE)
                  ? f.extractionError
                  : ShapeError::INSUFFICIENT_DATA;
        r.reason = (f.extractionError == ShapeError::INVALID_VA) ? "VAH <= VAL (invalid VA)" :
                   (f.extractionError == ShapeError::THRESHOLDS_INVALID) ? "VolumeThresholds not valid" :
                   "Features invalid or insufficient data";
        return r;
    }

    if (f.binCount == 0) {
        r.error = ShapeError::HISTOGRAM_EMPTY;
        r.reason = "Histogram empty";
        return r;
    }

    if (f.binCount < MIN_HISTOGRAM_BINS) {
        r.error = ShapeError::INSUFFICIENT_DATA;
        r.reason = "Too few bins";
        return r;
    }

    if (f.vaWidthTicks < MIN_VA_WIDTH_TICKS) {
        r.error = ShapeError::VA_TOO_NARROW;
        r.reason = "VA width < minimum";
        return r;
    }

    if (f.hvnThreshold <= 0.0 || f.lvnThreshold < 0.0) {
        r.error = ShapeError::THRESHOLDS_INVALID;
        r.reason = "HVN/LVN thresholds not computed";
        return r;
    }

    // =========================================================================
    // DECISION TREE (per formal specification)
    // Priority: THIN_VERTICAL → DOUBLE_DISTRIBUTION → Imbalance (P/B) → Balance family
    // =========================================================================

    // Precompute key metrics for readability
    const float x = f.pocInRange;     // POC position in range [0,1]
    const float w = f.breadth;        // VA width / Range (0,1]
    const float a = f.asymmetry;      // POC offset from VA midpoint [-0.5,0.5]
    const float k = f.peakiness;      // POC vol / VA mean

    // -------------------------------------------------------------------------
    // 1. THIN_VERTICAL (structural: narrow acceptance / fast auction)
    // Fires first - takes priority over all other classifications
    // Condition: w <= W_THIN (equivalently e >= e_thin)
    // -------------------------------------------------------------------------
    if (w <= W_THIN) {
        // Optional: exclude spike-peaked profiles from thin classification
        if (k <= POC_FLATNESS_MAX) {
            r.shape = ProfileShape::THIN_VERTICAL;
            r.confidence01 = (std::min)(1.0f, (W_THIN - w) / W_THIN + 0.5f);
            r.reason = "Narrow acceptance (w <= 0.4)";
            return r;
        }
        // Spike-peaked thin profile - fall through to other classifications
    }

    // -------------------------------------------------------------------------
    // 2. DOUBLE_DISTRIBUTION (bimodal: two HVN clusters with LVN valley)
    // -------------------------------------------------------------------------
    if (f.hvnClusters.size() >= 2) {
        std::vector<size_t> indices(f.hvnClusters.size());
        for (size_t i = 0; i < indices.size(); ++i) indices[i] = i;
        std::sort(indices.begin(), indices.end(),
                  [&f](size_t ai, size_t bi) {
                      return f.hvnClusters[ai].totalVolume > f.hvnClusters[bi].totalVolume;
                  });

        const HVNCluster& c1 = f.hvnClusters[indices[0]];
        const HVNCluster& c2 = f.hvnClusters[indices[1]];
        const int separation = std::abs(c1.centerTick - c2.centerTick);

        if (separation >= f.minClusterSeparationTicks) {
            const double combinedMass = c1.totalVolume + c2.totalVolume;
            const float c1Ratio = static_cast<float>(c1.totalVolume / combinedMass);
            const float c2Ratio = static_cast<float>(c2.totalVolume / combinedMass);
            const bool c1Dominant = (c1Ratio >= CLUSTER_DOMINANCE_MIN);
            const bool c2Dominant = (c2Ratio >= CLUSTER_DOMINANCE_MIN);
            const float totalHvnMassRatio = (f.totalVolume > 0.0)
                ? static_cast<float>(combinedMass / f.totalVolume) : 0.0f;
            const bool hvnMassSignificant = (totalHvnMassRatio >= MIN_TOTAL_HVN_MASS_RATIO);
            const int minValleyWidth = static_cast<int>(std::ceil(separation * VALLEY_WIDTH_MIN_RATIO));

            if (c1Dominant && c2Dominant && hvnMassSignificant && f.lvnValleyWidth >= minValleyWidth) {
                r.shape = ProfileShape::DOUBLE_DISTRIBUTION;
                r.confidence01 = (std::min)(1.0f,
                    static_cast<float>(separation) / (f.minClusterSeparationTicks * 2.0f) * 0.5f +
                    static_cast<float>(f.lvnValleyWidth) / (minValleyWidth * 2.0f) * 0.5f);
                r.reason = "Two HVN clusters with LVN valley";
                return r;
            } else if (c1Dominant && c2Dominant && hvnMassSignificant) {
                r.error = ShapeError::AMBIGUOUS_BIMODAL;
                r.reason = "Two HVN clusters but valley unclear";
                return r;
            }
        }
    }

    // -------------------------------------------------------------------------
    // 3. IMBALANCE FAMILY (P/B): POC outside center band of range
    // Uses x_poc (POC position in full range), NOT pocInVa01
    // -------------------------------------------------------------------------

    // P-SHAPED: POC high in range (x_poc > C_MAX) - fat top, thin bottom
    if (x > C_MAX) {
        r.shape = ProfileShape::P_SHAPED;
        r.confidence01 = (x - C_MAX) / (1.0f - C_MAX);
        // Boost for confirming skew
        if (f.massSkewRatio > 1.0f) {
            r.confidence01 = (std::min)(1.0f, r.confidence01 + 0.2f);
        }
        r.reason = "POC high in range (fat top)";
        return r;
    }

    // B-SHAPED: POC low in range (x_poc < C_MIN) - fat bottom, thin top
    if (x < C_MIN) {
        r.shape = ProfileShape::B_SHAPED;
        r.confidence01 = (C_MIN - x) / C_MIN;
        // Boost for confirming skew
        if (f.massSkewRatio < 1.0f) {
            r.confidence01 = (std::min)(1.0f, r.confidence01 + 0.2f);
        }
        r.reason = "POC low in range (fat bottom)";
        return r;
    }

    // -------------------------------------------------------------------------
    // 4. BALANCE FAMILY: POC in center band (C_MIN <= x_poc <= C_MAX)
    // Non-overlapping classification via exact inequalities:
    //   NORMAL: k >= K_SHARP AND |a| <= A_BAL
    //   D_SHAPED: K_MOD <= k < K_SHARP AND |a| >= A_D
    //   BALANCED: k < K_MOD AND w >= W_BAL AND |a| <= A_BAL
    //   UNDEFINED: falls in intentional gaps
    // -------------------------------------------------------------------------

    const float absA = std::abs(a);

    // 4a. NORMAL_DISTRIBUTION: Sharp peak AND symmetric
    if (k >= K_SHARP && absA <= A_BAL) {
        r.shape = ProfileShape::NORMAL_DISTRIBUTION;
        r.confidence01 = (std::min)(1.0f, k / (K_SHARP * 1.5f));
        r.reason = "Sharp symmetric peak";
        return r;
    }

    // 4b. D_SHAPED: Moderate peak AND asymmetric (one-sided rejection)
    if (k >= K_MOD && k < K_SHARP && absA >= A_D) {
        r.shape = ProfileShape::D_SHAPED;
        r.confidence01 = (k - K_MOD) / (K_SHARP - K_MOD);
        // Boost for stronger asymmetry
        const float asymBoost = (absA - A_D) * 0.5f;
        r.confidence01 = (std::min)(1.0f, r.confidence01 + asymBoost);
        r.reason = (a > 0) ? "Rejection below (D-shape)" : "Rejection above (D-shape)";
        return r;
    }

    // 4c. BALANCED: Low peak AND wide acceptance AND symmetric
    // Requires ALL three conditions to prevent overlap
    if (k < K_MOD && w >= W_BAL && absA <= A_BAL) {
        r.shape = ProfileShape::BALANCED;
        // Confidence: flatter = more clearly balanced
        r.confidence01 = 1.0f - (k - 1.0f) / (K_MOD - 1.0f);
        r.confidence01 = (std::max)(0.0f, (std::min)(1.0f, r.confidence01));
        r.reason = "Wide acceptance, no dominant POC";
        return r;
    }

    // -------------------------------------------------------------------------
    // 5. UNDEFINED: Profile falls in intentional gap regions
    // This is NOT a fallback - it means the profile is genuinely ambiguous
    // Gap regions:
    //   - k >= K_SHARP but asymmetric (sharp but not symmetric bell)
    //   - K_MOD <= k < K_SHARP but |a| in (A_BAL, A_D) (moderate, weakly asymmetric)
    //   - k < K_MOD but w < W_BAL (low peak but narrow - not equilibrium)
    //   - k < K_MOD but |a| > A_BAL (low peak but asymmetric)
    // -------------------------------------------------------------------------
    r.error = ShapeError::INCONCLUSIVE_BALANCE;
    r.reason = "Profile in ambiguity gap";
    return r;
}

// ============================================================================
// LEGACY ENUM MAPPING (unified -> legacy, one direction only)
// These functions derive legacy enum values from the unified ProfileShape.
// The unified ProfileShape is SSOT - legacy enums are derived views only.
// ============================================================================

inline BalanceProfileShape ToBalanceProfileShape(ProfileShape shape)
{
    switch (shape) {
        case ProfileShape::NORMAL_DISTRIBUTION:
            return BalanceProfileShape::NORMAL_DISTRIBUTION;
        case ProfileShape::D_SHAPED:
            return BalanceProfileShape::D_SHAPED;
        case ProfileShape::BALANCED:
            return BalanceProfileShape::BALANCED;
        default:
            return BalanceProfileShape::UNDEFINED;
    }
}

inline ImbalanceProfileShape ToImbalanceProfileShape(ProfileShape shape)
{
    switch (shape) {
        case ProfileShape::P_SHAPED:
            return ImbalanceProfileShape::P_SHAPED;
        case ProfileShape::B_SHAPED:
            return ImbalanceProfileShape::B_SHAPED_LOWER;  // Map to LOWER (single-mode B)
        case ProfileShape::DOUBLE_DISTRIBUTION:
            return ImbalanceProfileShape::B_SHAPED_BIMODAL;
        case ProfileShape::THIN_VERTICAL:
            return ImbalanceProfileShape::THIN_VERTICAL;
        default:
            return ImbalanceProfileShape::UNDEFINED;
    }
}

// ============================================================================
// CONVENIENCE: Check if shape indicates balance vs imbalance
// ============================================================================

inline bool IsBalanceShape(ProfileShape shape)
{
    switch (shape) {
        case ProfileShape::NORMAL_DISTRIBUTION:
        case ProfileShape::D_SHAPED:
        case ProfileShape::BALANCED:
            return true;
        default:
            return false;
    }
}

inline bool IsImbalanceShape(ProfileShape shape)
{
    switch (shape) {
        case ProfileShape::P_SHAPED:
        case ProfileShape::B_SHAPED:
        case ProfileShape::THIN_VERTICAL:
        case ProfileShape::DOUBLE_DISTRIBUTION:
            return true;
        default:
            return false;
    }
}

// ============================================================================
// SHAPE RESOLUTION WITH DAY STRUCTURE CONSTRAINT
// ============================================================================
// Resolves final shape by applying DayStructure as family constraint.
// STRICT MODE: If rawShape conflicts with dayStructure family, returns UNDEFINED.
//
// SSOT Contract:
// - DayStructure is SSOT for shape family (session-level, from DayTypeClassifier)
// - RawShape is geometric only (from ClassifyProfileShape)
// - No circularity: DayStructure must NOT depend on shape
// ============================================================================

struct ShapeResolutionResult {
    ProfileShape rawShape = ProfileShape::UNDEFINED;      // Geometric classification
    ProfileShape finalShape = ProfileShape::UNDEFINED;    // After family constraint
    bool conflict = false;                                // RawShape was outside allowed family
    const char* resolution = "PENDING";                   // "ACCEPTED" | "CONFLICT" | "PENDING"
};

// Family membership check
inline bool IsShapeInBalanceFamily(ProfileShape shape) {
    return shape == ProfileShape::NORMAL_DISTRIBUTION ||
           shape == ProfileShape::D_SHAPED ||
           shape == ProfileShape::BALANCED;
}

inline bool IsShapeInImbalanceFamily(ProfileShape shape) {
    return shape == ProfileShape::P_SHAPED ||
           shape == ProfileShape::B_SHAPED ||
           shape == ProfileShape::THIN_VERTICAL ||
           shape == ProfileShape::DOUBLE_DISTRIBUTION;
}

// Main resolution function (STRICT MODE - no remapping)
// Pre-condition: Only call when BOTH rawShape is valid AND dayStructure is classified
inline ShapeResolutionResult ResolveShapeWithDayStructure(
    ProfileShape rawShape,
    DayStructure dayStructure)
{
    ShapeResolutionResult result;
    result.rawShape = rawShape;

    // Gate: rawShape must be valid
    if (rawShape == ProfileShape::UNDEFINED) {
        result.finalShape = ProfileShape::UNDEFINED;
        result.conflict = false;
        result.resolution = "RAW_UNDEFINED";
        return result;
    }

    // Gate: dayStructure must be classified
    if (dayStructure == DayStructure::UNDEFINED) {
        // Cannot resolve without structure - should not happen if caller follows contract
        result.finalShape = ProfileShape::UNDEFINED;
        result.conflict = false;
        result.resolution = "STRUCTURE_UNDEFINED";
        return result;
    }

    // Determine allowed family based on DayStructure
    const bool rawIsBalance = IsShapeInBalanceFamily(rawShape);
    const bool rawIsImbalance = IsShapeInImbalanceFamily(rawShape);
    const bool structureIsBalanced = (dayStructure == DayStructure::BALANCED);

    // Check family membership
    if (structureIsBalanced) {
        // DayStructure = BALANCED → only balance shapes allowed
        if (rawIsBalance) {
            result.finalShape = rawShape;
            result.conflict = false;
            result.resolution = "ACCEPTED";
        } else {
            // CONFLICT: imbalance shape in balanced day
            result.finalShape = ProfileShape::UNDEFINED;
            result.conflict = true;
            result.resolution = "CONFLICT";
        }
    } else {
        // DayStructure = IMBALANCED → only imbalance shapes allowed
        if (rawIsImbalance) {
            result.finalShape = rawShape;
            result.conflict = false;
            result.resolution = "ACCEPTED";
        } else {
            // CONFLICT: balance shape in imbalanced day
            result.finalShape = ProfileShape::UNDEFINED;
            result.conflict = true;
            result.resolution = "CONFLICT";
        }
    }

    return result;
}

// ============================================================================
// SHAPE CONFIRMATION GATE HELPERS
// ============================================================================
// These functions support the 6-gate shape confirmation system.
// They validate geometric shapes with auction evidence.
// ============================================================================

/**
 * HasSinglePrints - Detect thin structure (single prints) in a price range
 *
 * @param volumeProfile  Map of price_tick -> VolumeAtPrice
 * @param fromPriceTicks Lower bound of range to check (in ticks)
 * @param toPriceTicks   Upper bound of range to check (in ticks)
 * @param avgVolumePerLevel  Average volume per price level (for "thin" threshold)
 * @param thinThreshold  What fraction of avg constitutes "thin" (default 0.30 = 30%)
 * @return true if > 30% of levels in range are "thin" (single print-like)
 *
 * P-shaped profiles should have single prints BELOW POC (tail/excess)
 * b-shaped profiles should have single prints ABOVE POC (tail/excess)
 */
template<typename VolumeMapT>
inline bool HasSinglePrints(
    const VolumeMapT& volumeProfile,
    int fromPriceTicks,
    int toPriceTicks,
    double avgVolumePerLevel,
    double thinThreshold = 0.30)
{
    if (volumeProfile.empty() || avgVolumePerLevel <= 0.0) return false;
    if (fromPriceTicks > toPriceTicks) std::swap(fromPriceTicks, toPriceTicks);

    const double thinCutoff = avgVolumePerLevel * thinThreshold;

    int thinLevelCount = 0;
    int totalLevelsInRange = 0;

    for (const auto& kv : volumeProfile) {
        const int priceTick = kv.first;
        if (priceTick >= fromPriceTicks && priceTick <= toPriceTicks) {
            totalLevelsInRange++;
            // Access volume - works with s_VolumeAtPriceV2 (.Volume) or similar types
            const double levelVolume = kv.second.Volume;
            if (levelVolume < thinCutoff) {
                thinLevelCount++;
            }
        }
    }

    if (totalLevelsInRange == 0) return false;

    // More than 30% thin levels = has single prints
    return (static_cast<double>(thinLevelCount) / totalLevelsInRange) > 0.30;
}

/**
 * GetVolumeInRange - Calculate total volume in a price range
 *
 * @param volumeProfile  Map of price_tick -> VolumeAtPrice
 * @param fromPriceTicks Lower bound of range (in ticks)
 * @param toPriceTicks   Upper bound of range (in ticks)
 * @return Total volume in the range
 */
template<typename VolumeMapT>
inline double GetVolumeInRange(
    const VolumeMapT& volumeProfile,
    int fromPriceTicks,
    int toPriceTicks)
{
    if (volumeProfile.empty()) return 0.0;
    if (fromPriceTicks > toPriceTicks) std::swap(fromPriceTicks, toPriceTicks);

    double totalVolume = 0.0;
    for (const auto& kv : volumeProfile) {
        const int priceTick = kv.first;
        if (priceTick >= fromPriceTicks && priceTick <= toPriceTicks) {
            totalVolume += kv.second.Volume;
        }
    }
    return totalVolume;
}

/**
 * ValidateVolumeDistribution - Check if volume distribution matches geometric shape
 *
 * @param volumeProfile  Map of price_tick -> VolumeAtPrice
 * @param rawShape       The geometric shape to validate
 * @param profileHighTicks  Session high in ticks
 * @param profileLowTicks   Session low in ticks
 * @param[out] upperThirdRatio  Ratio of upper/lower third volume (for diagnostics)
 * @return true if volume distribution confirms the shape
 *
 * P-shape: volume concentrated in upper third (ratio > 2.0)
 * b-shape: volume concentrated in lower third (ratio < 0.5)
 * Balance shapes: evenly distributed (0.67 < ratio < 1.5)
 */
template<typename VolumeMapT>
inline bool ValidateVolumeDistribution(
    const VolumeMapT& volumeProfile,
    ProfileShape rawShape,
    int profileHighTicks,
    int profileLowTicks,
    double& upperThirdRatio)
{
    upperThirdRatio = 1.0;  // Default to balanced
    if (volumeProfile.empty()) return false;

    const int rangeTicks = profileHighTicks - profileLowTicks;
    if (rangeTicks < 3) return false;  // Need at least 3 levels for thirds

    const int thirdSize = rangeTicks / 3;
    const int upperThirdStart = profileHighTicks - thirdSize;
    const int lowerThirdEnd = profileLowTicks + thirdSize;

    const double volumeUpperThird = GetVolumeInRange(volumeProfile, upperThirdStart, profileHighTicks);
    const double volumeLowerThird = GetVolumeInRange(volumeProfile, profileLowTicks, lowerThirdEnd);

    const double epsilon = 1.0;  // Prevent division by zero
    upperThirdRatio = volumeUpperThird / (volumeLowerThird + epsilon);

    // Validate based on shape
    switch (rawShape) {
        case ProfileShape::P_SHAPED:
            // P-shape: volume concentrated in upper third
            return (upperThirdRatio > 2.0);

        case ProfileShape::B_SHAPED:
            // b-shape: volume concentrated in lower third
            return (upperThirdRatio < 0.5);

        case ProfileShape::NORMAL_DISTRIBUTION:
        case ProfileShape::D_SHAPED:
        case ProfileShape::BALANCED:
            // Balance shapes: evenly distributed
            return (upperThirdRatio > 0.67 && upperThirdRatio < 1.5);

        default:
            // Other shapes: no volume validation required
            return true;
    }
}

/**
 * GetTimeConfidenceMultiplier - Scale shape confidence based on session progress
 *
 * @param sessionMinutes  Minutes since session start
 * @param isRTH           true for RTH session, false for Globex
 * @return Confidence multiplier [0.3, 1.0]
 *
 * RTH: IB = 60 min, full confidence at 180+ min
 * Globex: Opening range = 90 min (lower volume), full confidence at 300+ min
 */
inline double GetTimeConfidenceMultiplier(int sessionMinutes, bool isRTH) {
    if (isRTH) {
        if (sessionMinutes < 60) return 0.3;   // IB forming
        if (sessionMinutes < 90) return 0.5;   // IB just complete
        if (sessionMinutes < 120) return 0.7;  // Early mid-session
        if (sessionMinutes < 180) return 0.85; // Mid-session
        return 1.0;  // Late session - shape well established
    } else {
        // Globex - longer development times due to lower volume
        if (sessionMinutes < 90) return 0.3;   // Opening range forming
        if (sessionMinutes < 120) return 0.5;  // Opening range just complete
        if (sessionMinutes < 180) return 0.7;  // Developing
        if (sessionMinutes < 300) return 0.85; // Established
        return 1.0;  // Mature overnight profile
    }
}

// ============================================================================
// DOUBLE DISTRIBUTION VALIDATION (4-criteria independent confirmation)
// ============================================================================
// Validates DD classification with multiple independent criteria to prevent
// false positives from noise or close volume peaks.
//
// Criteria:
// 1. Cluster separation: HVN clusters must be > 8 ticks apart
// 2. Genuine LVN: Valley volume < 30% of cluster average
// 3. Volume balance: Neither cluster > 3x the other
// 4. Time split: Price spent meaningful time in both (optional)
// ============================================================================

struct DDValidation {
    // === THRESHOLDS ===
    static constexpr int MIN_CLUSTER_SEPARATION_TICKS = 8;   // ES: 2 points
    static constexpr double LVN_VOLUME_THRESHOLD = 0.30;     // <30% of cluster avg
    static constexpr double VOLUME_BALANCE_RATIO = 3.0;      // Neither >3x other
    static constexpr double MIN_TIME_SPLIT_RATIO = 0.15;     // 15% time in each

    // === CLUSTER METRICS ===
    int cluster1CenterTicks = 0;
    int cluster2CenterTicks = 0;
    int lvnCenterTicks = 0;
    double cluster1Volume = 0.0;
    double cluster2Volume = 0.0;
    double lvnVolume = 0.0;
    int separationTicks = 0;
    double volumeBalanceRatio = 1.0;
    double lvnVolumeRatio = 0.0;

    // === VALIDATION CRITERIA ===
    bool hasSufficientSeparation = false;  // Clusters > 8 ticks apart
    bool hasGenuineLVN = false;            // Valley volume < 30% of cluster avg
    bool hasBalancedVolume = false;        // Neither cluster > 3x the other
    bool hasTimeSplit = false;             // Price spent >=15% time in each

    // === FAILURE REASON ===
    const char* failedCriterion = nullptr;

    bool IsValidDD() const {
        return hasSufficientSeparation && hasGenuineLVN && hasBalancedVolume;
    }

    float GetDDConfidence() const {
        int criteria = 0;
        if (hasSufficientSeparation) criteria++;
        if (hasGenuineLVN) criteria++;
        if (hasBalancedVolume) criteria++;
        if (hasTimeSplit) criteria++;
        return criteria / 4.0f;
    }

    int GetCriteriaPassCount() const {
        int count = 0;
        if (hasSufficientSeparation) count++;
        if (hasGenuineLVN) count++;
        if (hasBalancedVolume) count++;
        if (hasTimeSplit) count++;
        return count;
    }

    const char* GetFailedCriteria() const {
        if (!hasSufficientSeparation) return "SEPARATION";
        if (!hasGenuineLVN) return "LVN_VOLUME";
        if (!hasBalancedVolume) return "VOLUME_BALANCE";
        if (!hasTimeSplit) return "TIME_SPLIT";
        return "NONE";
    }

    // Format DD validation log string
    // Output: "DD_CHECK: SEP=12t(OK) LVN=18%(OK) BAL=2.1x(OK) TIME=22%/19%(OK) | VALID=YES CONF=1.00"
    std::string FormatLogString() const {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "DD_CHECK: SEP=%dt(%s) LVN=%.0f%%(%s) BAL=%.1fx(%s) TIME(%s) | VALID=%s CONF=%.2f",
            separationTicks,
            hasSufficientSeparation ? "OK" : "FAIL",
            lvnVolumeRatio * 100.0,
            hasGenuineLVN ? "OK" : "FAIL",
            volumeBalanceRatio,
            hasBalancedVolume ? "OK" : "FAIL",
            hasTimeSplit ? "OK" : "FAIL",
            IsValidDD() ? "YES" : "NO",
            GetDDConfidence());
        return std::string(buf);
    }
};

// Validate Double Distribution with multiple independent criteria
// Returns DDValidation with all criteria results and overall validity
template<typename VolumeMapT>
inline DDValidation ValidateDoubleDistribution(
    const VolumeMapT& volumeProfile,
    const std::vector<HVNCluster>& clusters,
    int barsAboveVA = 0,
    int barsBelowVA = 0,
    int totalBars = 0)
{
    DDValidation result;

    // Need at least 2 clusters for DD
    if (clusters.size() < 2) {
        result.failedCriterion = "INSUFFICIENT_CLUSTERS";
        return result;
    }

    // Sort clusters by price (tick position)
    std::vector<HVNCluster> sortedClusters = clusters;
    std::sort(sortedClusters.begin(), sortedClusters.end(),
              [](const HVNCluster& a, const HVNCluster& b) {
                  return a.centerTick < b.centerTick;
              });

    // Use the two most prominent clusters (by volume if more than 2)
    if (sortedClusters.size() > 2) {
        std::sort(sortedClusters.begin(), sortedClusters.end(),
                  [](const HVNCluster& a, const HVNCluster& b) {
                      return a.totalVolume > b.totalVolume;
                  });
        sortedClusters.resize(2);
        // Re-sort by price
        std::sort(sortedClusters.begin(), sortedClusters.end(),
                  [](const HVNCluster& a, const HVNCluster& b) {
                      return a.centerTick < b.centerTick;
                  });
    }

    result.cluster1CenterTicks = sortedClusters[0].centerTick;
    result.cluster2CenterTicks = sortedClusters[1].centerTick;
    result.cluster1Volume = sortedClusters[0].totalVolume;
    result.cluster2Volume = sortedClusters[1].totalVolume;

    // === CRITERION 1: Cluster Separation ===
    result.separationTicks = result.cluster2CenterTicks - result.cluster1CenterTicks;
    result.hasSufficientSeparation = (result.separationTicks >= DDValidation::MIN_CLUSTER_SEPARATION_TICKS);

    // === CRITERION 2: Genuine LVN (valley between clusters) ===
    // Find minimum volume level between the two cluster centers
    double minVolInGap = std::numeric_limits<double>::max();
    int lvnTick = (result.cluster1CenterTicks + result.cluster2CenterTicks) / 2;

    for (const auto& kv : volumeProfile) {
        const int tick = kv.first;
        if (tick > result.cluster1CenterTicks && tick < result.cluster2CenterTicks) {
            double vol = 0.0;
            // Handle different volume profile types (duck-typing via has_volume_member trait)
            if constexpr (has_volume_member<typename VolumeMapT::mapped_type>::value) {
                vol = kv.second.Volume;
            } else {
                vol = static_cast<double>(kv.second);
            }
            if (vol < minVolInGap) {
                minVolInGap = vol;
                lvnTick = tick;
            }
        }
    }

    if (minVolInGap == std::numeric_limits<double>::max()) {
        minVolInGap = 0.0;  // No levels between clusters
    }

    result.lvnCenterTicks = lvnTick;
    result.lvnVolume = minVolInGap;

    const double avgClusterVol = (result.cluster1Volume + result.cluster2Volume) / 2.0;
    result.lvnVolumeRatio = (avgClusterVol > 0.0) ? (result.lvnVolume / avgClusterVol) : 1.0;
    result.hasGenuineLVN = (result.lvnVolumeRatio < DDValidation::LVN_VOLUME_THRESHOLD);

    // === CRITERION 3: Volume Balance ===
    const double minClusterVol = (std::min)(result.cluster1Volume, result.cluster2Volume);
    const double maxClusterVol = (std::max)(result.cluster1Volume, result.cluster2Volume);
    result.volumeBalanceRatio = (minClusterVol > 0.0) ? (maxClusterVol / minClusterVol) : 999.0;
    result.hasBalancedVolume = (result.volumeBalanceRatio <= DDValidation::VOLUME_BALANCE_RATIO);

    // === CRITERION 4: Time Split (if bar data provided) ===
    if (totalBars > 0) {
        const float upperRatio = static_cast<float>(barsAboveVA) / totalBars;
        const float lowerRatio = static_cast<float>(barsBelowVA) / totalBars;
        result.hasTimeSplit = (upperRatio >= DDValidation::MIN_TIME_SPLIT_RATIO &&
                               lowerRatio >= DDValidation::MIN_TIME_SPLIT_RATIO);
    } else {
        result.hasTimeSplit = false;  // Unknown - can't validate
    }

    return result;
}

} // namespace AMT

#endif // AMT_PROFILESHAPE_H
