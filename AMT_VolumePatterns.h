// ============================================================================
// AMT_VolumePatterns.h
// Deterministic volume profile pattern detection from profile structure only
// Pure classifier module - no Sierra Chart dependencies
//
// SSOT: VolumeProfilePattern and TPOMechanics enums in AMT_Patterns.h
// This module provides:
//   1. VolumePatternHit / TPOMechanicsHit structs (hit metadata)
//   2. VolumePatternConfig namespace (adaptive thresholds)
//   3. VolumePatternFeatures DTO (extracted from histogram + VA inputs)
//   4. IsPatternEligible() - eligibility gate
//   5. Pattern detector functions (pure, no fallbacks)
//   6. DetectAllPatterns() - orchestrator returning all hits
//
// NO FALLBACKS: If detection is ambiguous or data insufficient, emit nothing.
// ADAPTIVE THRESHOLDS: Derived from VA/range proportions, not hardcoded.
// ============================================================================

#ifndef AMT_VOLUMEPATTERNS_H
#define AMT_VOLUMEPATTERNS_H

#include "amt_core.h"
#include "AMT_config.h"
#include "AMT_Patterns.h"
#include "AMT_ProfileShape.h"  // Reuse ProfileShapeConfig::MIN_* constants
#include <vector>
#include <algorithm>
#include <cmath>
#include <optional>
#include <map>
#include <set>
#include <climits>

namespace AMT {

// ============================================================================
// HIT METADATA STRUCTS
// VolumePatternHit and TPOMechanicsHit are defined in AMT_Patterns.h
// ============================================================================

// ============================================================================
// VOLUME PATTERN CONFIGURATION
// All thresholds are proportional to VA width or range - NO hardcoded values
// ============================================================================

namespace VolumePatternConfig {
    // =========================================================================
    // ELIGIBILITY REQUIREMENTS (reuse from ProfileShapeConfig)
    // =========================================================================
    // MIN_HISTOGRAM_BINS and MIN_VA_WIDTH_TICKS come from ProfileShapeConfig

    // =========================================================================
    // GAP DETECTION (LVN corridor between meaningful volume regions)
    // =========================================================================
    constexpr float GAP_WIDTH_VA_RATIO = 0.08f;    // minGapWidth = max(3, VA * 0.08)
    constexpr int GAP_WIDTH_MIN_ABS = 3;           // Absolute minimum gap width
    constexpr float GAP_VOL_RATIO_MAX = 0.4f;      // LVN vol <= 40% of median

    // =========================================================================
    // VACUUM DETECTION (stricter LVN corridor, potential slippage zone)
    // =========================================================================
    constexpr float VACUUM_WIDTH_VA_RATIO = 0.12f; // minVacuumWidth = max(4, VA * 0.12)
    constexpr int VACUUM_WIDTH_MIN_ABS = 4;        // Absolute minimum vacuum width
    constexpr float VACUUM_VOL_RATIO_MAX = 0.25f;  // LVN vol <= 25% of median

    // =========================================================================
    // SHELF DETECTION (HVN plateau with edge drop-off)
    // =========================================================================
    constexpr float SHELF_WIDTH_VA_RATIO = 0.10f;  // minShelfWidth = max(3, VA * 0.10)
    constexpr int SHELF_WIDTH_MIN_ABS = 3;         // Absolute minimum shelf width
    constexpr float SHELF_FLATNESS_MAX = 0.4f;     // (p90-p10)/p50 <= 0.4 for flatness
    constexpr float SHELF_EDGE_DROP_MIN = 2.0f;    // Edge vol / outside vol >= 2.0

    // =========================================================================
    // LEDGE DETECTION (significant step-change in volume density)
    // =========================================================================
    constexpr float LEDGE_GRADIENT_SIGMA = 2.0f;   // Gradient must exceed 2 sigma
    constexpr int LEDGE_PERSISTENCE_BINS = 2;      // Must persist over N neighbors

    // =========================================================================
    // CLUSTER DETECTION (concentrated HVN mass in VA, no LVN corridors)
    // =========================================================================
    constexpr float CLUSTER_HVN_MASS_MIN = 0.5f;   // HVN mass >= 50% of VA volume
    constexpr float CLUSTER_LVN_GAP_MAX_RATIO = 0.05f; // Max LVN gap width / VA width

    // =========================================================================
    // MIGRATION DETECTION (POC drift over time)
    // =========================================================================
    constexpr int MIGRATION_HISTORY_SIZE = 8;      // Track last N POC positions
    constexpr float MIGRATION_NET_DRIFT_MIN = 0.15f; // Net drift >= 15% of VA
    constexpr int MIGRATION_MAX_REVERSALS = 2;     // Max direction changes allowed

    // =========================================================================
    // TPO MECHANICS (overlap vs separation)
    // =========================================================================
    constexpr float OVERLAP_MIN = 0.6f;            // overlap >= 60% = TPO_OVERLAP
    constexpr float SEPARATION_MAX = 0.3f;         // overlap <= 30% = TPO_SEPARATION

    // =========================================================================
    // BREAKOUT/TRAP DETECTION (profile structure + mechanics-gated)
    // =========================================================================
    // Breach thresholds (adaptive to VA width)
    constexpr float BREACH_VA_RATIO = 0.05f;       // minBreachTicks = max(2, VA * 0.05)
    constexpr int BREACH_MIN_ABS = 2;              // Absolute minimum breach ticks

    // Outside mass thresholds for breach/acceptance classification
    constexpr float OUTSIDE_MASS_BREACH_MIN = 0.05f;   // >= 5% outside = breach
    constexpr float OUTSIDE_MASS_ACCEPT_MIN = 0.15f;   // >= 15% outside = acceptance
    constexpr float OUTSIDE_HVN_MASS_MIN = 0.08f;      // >= 8% HVN outside = acceptance

    // Trap classification band: breach present but acceptance weak
    constexpr float TRAP_MASS_MAX = 0.12f;             // Outside mass < 12% = trap territory

    // Ambiguity guard for both-sides breach
    constexpr float BOTH_SIDES_BREACH_GUARD = 0.03f;   // If both sides > 3%, ambiguous

    // Minimum VA width for breakout detection
    constexpr int BREAKOUT_MIN_VA_WIDTH = 8;           // Need >= 8 tick VA for breakout
}

// ============================================================================
// BALANCE SNAPSHOT (session-scoped, mechanics-gated reference boundary)
// Updated only when TPO_OVERLAP is detected and stable levels are valid
// ============================================================================

struct BalanceSnapshot {
    bool valid = false;
    int vahTick = 0;
    int valTick = 0;
    int pocTick = 0;
    int capturedAtBar = -1;
    double tickSize = 0.0;

    void Reset() {
        valid = false;
        vahTick = valTick = pocTick = 0;
        capturedAtBar = -1;
        tickSize = 0.0;
    }

    /**
     * Check if snapshot is coherent for breakout detection.
     * @return true if VAH > VAL and width is meaningful
     */
    bool IsCoherent() const {
        if (!valid) return false;
        if (vahTick <= valTick) return false;
        const int width = vahTick - valTick;
        return width >= VolumePatternConfig::BREAKOUT_MIN_VA_WIDTH;
    }

    /**
     * Check tick size compatibility with current profile.
     */
    bool IsCompatible(double currentTickSize) const {
        if (!valid) return false;
        return std::abs(tickSize - currentTickSize) < 1e-9;
    }

    /**
     * Update from stable levels (mechanics-gated).
     * @param stableVah VAH tick from stable levels
     * @param stableVal VAL tick from stable levels
     * @param stablePoc POC tick from stable levels
     * @param bar Current bar index
     * @param ts Tick size
     */
    void UpdateFrom(int stableVah, int stableVal, int stablePoc, int bar, double ts) {
        vahTick = stableVah;
        valTick = stableVal;
        pocTick = stablePoc;
        capturedAtBar = bar;
        tickSize = ts;
        valid = (stableVah > stableVal && ts > 0.0);
    }

    int WidthTicks() const { return vahTick - valTick; }
};

// ============================================================================
// CONTIGUOUS RUN DETECTION (internal helper)
// Used for finding LVN corridors and HVN plateaus
// ============================================================================

struct VolumeRun {
    int startTick = 0;
    int endTick = 0;
    double totalVolume = 0.0;
    double meanVolume = 0.0;
    int binCount = 0;

    int WidthTicks() const { return endTick - startTick + 1; }
};

// ============================================================================
// VOLUME PATTERN FEATURES DTO
// Extracted once from histogram - passed to detectors
// ============================================================================

struct VolumePatternFeatures {
    // =========================================================================
    // CORE TICK-BASED LEVELS
    // =========================================================================
    int pocTick = 0;
    int vahTick = 0;
    int valTick = 0;
    int profileHighTick = 0;
    int profileLowTick = 0;

    // =========================================================================
    // DERIVED SCALARS
    // =========================================================================
    int vaWidthTicks = 0;        // VAH - VAL
    int rangeTicks = 0;          // profileHigh - profileLow

    // =========================================================================
    // VOLUME STATISTICS
    // =========================================================================
    double totalVolume = 0.0;
    double maxVolume = 0.0;
    double medianVolume = 0.0;
    double hvnThreshold = 0.0;
    double lvnThreshold = 0.0;

    // =========================================================================
    // CONTIGUOUS RUNS (computed from histogram)
    // =========================================================================
    std::vector<VolumeRun> hvnRuns;   // Contiguous HVN regions (vol >= hvnThreshold)
    std::vector<VolumeRun> lvnRuns;   // Contiguous LVN regions (vol <= lvnThreshold)

    // =========================================================================
    // GRADIENT DATA (for ledge detection)
    // =========================================================================
    std::vector<double> gradients;    // Volume gradient at each bin
    double gradientMean = 0.0;
    double gradientMAD = 0.0;         // Median Absolute Deviation

    // =========================================================================
    // HISTOGRAM ACCESSOR
    // =========================================================================
    std::vector<std::pair<int, double>> sortedBins;  // (tick, volume) sorted by tick

    // =========================================================================
    // ADAPTIVE THRESHOLDS (computed from VA/range)
    // =========================================================================
    int minGapWidthTicks = 0;
    int minVacuumWidthTicks = 0;
    int minShelfWidthTicks = 0;

    // =========================================================================
    // VALIDATION
    // =========================================================================
    bool valid = false;
    int binCount = 0;
};

// ============================================================================
// MIGRATION HISTORY (session-scoped state for POC drift tracking)
// ============================================================================

struct MigrationHistory {
    static constexpr int MAX_HISTORY = VolumePatternConfig::MIGRATION_HISTORY_SIZE;

    int pocHistory[MAX_HISTORY] = {0};
    int count = 0;
    int head = 0;  // Ring buffer head

    void Reset() {
        count = 0;
        head = 0;
        for (int i = 0; i < MAX_HISTORY; ++i) pocHistory[i] = 0;
    }

    void AddPOC(int pocTick) {
        pocHistory[head] = pocTick;
        head = (head + 1) % MAX_HISTORY;
        if (count < MAX_HISTORY) ++count;
    }

    // Returns true if monotonic drift detected with low reversals
    bool HasMonotonicDrift(int vaWidthTicks, int& netDriftTicks, int& reversalCount) const {
        if (count < 3) return false;

        // Get samples in chronological order
        int samples[MAX_HISTORY];
        int n = 0;
        for (int i = 0; i < count; ++i) {
            int idx = (head - count + i + MAX_HISTORY) % MAX_HISTORY;
            samples[n++] = pocHistory[idx];
        }

        // Count direction changes and compute net drift
        reversalCount = 0;
        int prevDir = 0;
        for (int i = 1; i < n; ++i) {
            int diff = samples[i] - samples[i-1];
            if (diff == 0) continue;
            int dir = (diff > 0) ? 1 : -1;
            if (prevDir != 0 && dir != prevDir) {
                ++reversalCount;
            }
            prevDir = dir;
        }

        netDriftTicks = samples[n-1] - samples[0];

        // Check if drift is significant relative to VA
        const int minDrift = (std::max)(2, static_cast<int>(vaWidthTicks * VolumePatternConfig::MIGRATION_NET_DRIFT_MIN));
        return (std::abs(netDriftTicks) >= minDrift) &&
               (reversalCount <= VolumePatternConfig::MIGRATION_MAX_REVERSALS);
    }
};

// ============================================================================
// IB DISTRIBUTION SNAPSHOT (session-scoped state for TPO mechanics)
// Captures volume distribution at IB freeze for later overlap comparison
// ============================================================================

struct IBDistSnapshot {
    bool valid = false;                                // True if snapshot captured this session
    double tickSize = 0.0;                             // Tick size at capture (for alignment check)
    std::vector<std::pair<int, double>> dist;          // (tick, volume) pairs from volume_profile
    int capturedAtBar = -1;                            // Bar index when snapshot was captured

    void Reset() {
        valid = false;
        tickSize = 0.0;
        dist.clear();
        capturedAtBar = -1;
    }

    /**
     * Capture snapshot from volume profile map.
     * @param volumeProfile The current session volume profile (tick -> VolumeAtPrice)
     * @param ts Tick size for alignment validation
     * @param bar Current bar index
     */
    template<typename VolumeProfileMapT>
    void CaptureFrom(const VolumeProfileMapT& volumeProfile, double ts, int bar) {
        dist.clear();
        dist.reserve(volumeProfile.size());

        for (const auto& [tick, vap] : volumeProfile) {
            if (vap.Volume > 0.0) {
                dist.push_back({tick, vap.Volume});
            }
        }

        // Sort by tick ascending (map iteration is already sorted, but be explicit)
        std::sort(dist.begin(), dist.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

        tickSize = ts;
        capturedAtBar = bar;
        valid = !dist.empty();
    }

    /**
     * Check if snapshot is compatible with current profile for overlap computation.
     * @param currentTickSize Current profile tick size
     * @return True if tick sizes match and snapshot is valid
     */
    bool IsCompatible(double currentTickSize) const {
        if (!valid || dist.empty()) return false;
        // Tick size must match exactly (float comparison with small tolerance)
        return std::abs(tickSize - currentTickSize) < 1e-9;
    }
};

// ============================================================================
// ELIGIBILITY GATE
// Returns true if data is sufficient for pattern detection
// ============================================================================

inline bool IsPatternEligible(const VolumePatternFeatures& f) {
    if (!f.valid) return false;
    if (f.binCount < ProfileShapeConfig::MIN_HISTOGRAM_BINS) return false;
    if (f.vahTick <= f.valTick) return false;
    if (f.vaWidthTicks < ProfileShapeConfig::MIN_VA_WIDTH_TICKS) return false;
    if (f.hvnThreshold <= 0.0 || f.lvnThreshold < 0.0) return false;
    return true;
}

// ============================================================================
// FEATURE EXTRACTION
// Extracts VolumePatternFeatures from histogram array
// Template to work with s_VolumeAtPriceV2 or test mocks
// ============================================================================

template<typename VolumeAtPriceT>
inline VolumePatternFeatures ExtractVolumePatternFeatures(
    const VolumeAtPriceT* histogram,
    int numPrices,
    int pocTick,
    int vahTick,
    int valTick,
    const VolumeThresholds& thresholds)
{
    using namespace VolumePatternConfig;
    VolumePatternFeatures f;

    // =========================================================================
    // VALIDATION GATES
    // =========================================================================
    if (!histogram || numPrices < ProfileShapeConfig::MIN_HISTOGRAM_BINS) {
        f.valid = false;
        return f;
    }

    if (vahTick <= valTick) {
        f.valid = false;
        return f;
    }

    if (!thresholds.valid) {
        f.valid = false;
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
    f.hvnThreshold = thresholds.hvnThreshold;
    f.lvnThreshold = thresholds.lvnThreshold;
    f.totalVolume = thresholds.totalVolume;
    f.maxVolume = thresholds.maxLevelVolume;

    // Compute adaptive thresholds
    f.minGapWidthTicks = (std::max)(GAP_WIDTH_MIN_ABS,
        static_cast<int>(std::ceil(f.vaWidthTicks * GAP_WIDTH_VA_RATIO)));
    f.minVacuumWidthTicks = (std::max)(VACUUM_WIDTH_MIN_ABS,
        static_cast<int>(std::ceil(f.vaWidthTicks * VACUUM_WIDTH_VA_RATIO)));
    f.minShelfWidthTicks = (std::max)(SHELF_WIDTH_MIN_ABS,
        static_cast<int>(std::ceil(f.vaWidthTicks * SHELF_WIDTH_VA_RATIO)));

    // =========================================================================
    // BUILD SORTED BIN LIST
    // =========================================================================
    f.sortedBins.reserve(numPrices);
    std::vector<double> volumes;
    volumes.reserve(numPrices);

    for (int i = 0; i < numPrices; ++i) {
        const int tick = histogram[i].PriceInTicks;
        const double vol = static_cast<double>(histogram[i].Volume);
        f.sortedBins.push_back({tick, vol});
        volumes.push_back(vol);
    }

    // Sort by tick
    std::sort(f.sortedBins.begin(), f.sortedBins.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Compute profile bounds
    if (!f.sortedBins.empty()) {
        f.profileLowTick = f.sortedBins.front().first;
        f.profileHighTick = f.sortedBins.back().first;
        f.rangeTicks = f.profileHighTick - f.profileLowTick;
    }

    // Compute median volume
    std::sort(volumes.begin(), volumes.end());
    if (!volumes.empty()) {
        size_t mid = volumes.size() / 2;
        f.medianVolume = (volumes.size() % 2 == 0)
            ? (volumes[mid-1] + volumes[mid]) / 2.0
            : volumes[mid];
    }

    // =========================================================================
    // DETECT CONTIGUOUS HVN RUNS
    // =========================================================================
    {
        VolumeRun currentRun;
        bool inRun = false;

        for (const auto& [tick, vol] : f.sortedBins) {
            const bool isHVN = (vol >= f.hvnThreshold);

            if (isHVN) {
                if (!inRun) {
                    currentRun = VolumeRun();
                    currentRun.startTick = tick;
                    currentRun.endTick = tick;
                    currentRun.totalVolume = vol;
                    currentRun.binCount = 1;
                    inRun = true;
                } else {
                    currentRun.endTick = tick;
                    currentRun.totalVolume += vol;
                    currentRun.binCount++;
                }
            } else {
                if (inRun) {
                    currentRun.meanVolume = currentRun.totalVolume / currentRun.binCount;
                    f.hvnRuns.push_back(currentRun);
                    inRun = false;
                }
            }
        }
        // Don't forget last run
        if (inRun) {
            currentRun.meanVolume = currentRun.totalVolume / currentRun.binCount;
            f.hvnRuns.push_back(currentRun);
        }
    }

    // =========================================================================
    // DETECT CONTIGUOUS LVN RUNS
    // =========================================================================
    {
        VolumeRun currentRun;
        bool inRun = false;

        for (const auto& [tick, vol] : f.sortedBins) {
            const bool isLVN = (vol <= f.lvnThreshold && vol > 0);

            if (isLVN) {
                if (!inRun) {
                    currentRun = VolumeRun();
                    currentRun.startTick = tick;
                    currentRun.endTick = tick;
                    currentRun.totalVolume = vol;
                    currentRun.binCount = 1;
                    inRun = true;
                } else {
                    currentRun.endTick = tick;
                    currentRun.totalVolume += vol;
                    currentRun.binCount++;
                }
            } else {
                if (inRun) {
                    currentRun.meanVolume = currentRun.totalVolume / currentRun.binCount;
                    f.lvnRuns.push_back(currentRun);
                    inRun = false;
                }
            }
        }
        if (inRun) {
            currentRun.meanVolume = currentRun.totalVolume / currentRun.binCount;
            f.lvnRuns.push_back(currentRun);
        }
    }

    // =========================================================================
    // COMPUTE GRADIENTS (for ledge detection)
    // =========================================================================
    if (f.sortedBins.size() >= 2) {
        f.gradients.reserve(f.sortedBins.size() - 1);
        std::vector<double> absGradients;
        absGradients.reserve(f.sortedBins.size() - 1);

        for (size_t i = 1; i < f.sortedBins.size(); ++i) {
            double grad = f.sortedBins[i].second - f.sortedBins[i-1].second;
            f.gradients.push_back(grad);
            absGradients.push_back(std::abs(grad));
        }

        // Compute gradient mean
        double sum = 0.0;
        for (double g : f.gradients) sum += g;
        f.gradientMean = sum / f.gradients.size();

        // Compute MAD (Median Absolute Deviation)
        std::sort(absGradients.begin(), absGradients.end());
        size_t mid = absGradients.size() / 2;
        double median = (absGradients.size() % 2 == 0)
            ? (absGradients[mid-1] + absGradients[mid]) / 2.0
            : absGradients[mid];

        std::vector<double> deviations;
        deviations.reserve(absGradients.size());
        for (double g : absGradients) {
            deviations.push_back(std::abs(g - median));
        }
        std::sort(deviations.begin(), deviations.end());
        mid = deviations.size() / 2;
        f.gradientMAD = (deviations.size() % 2 == 0)
            ? (deviations[mid-1] + deviations[mid]) / 2.0
            : deviations[mid];
    }

    f.valid = true;
    return f;
}

// ============================================================================
// PATTERN DETECTORS (pure functions, no fallbacks)
// Each returns std::optional<VolumePatternHit> - empty if not detected
// ============================================================================

// ---------------------------------------------------------------------------
// VOLUME_GAP: LVN corridor between meaningful volume regions
// ---------------------------------------------------------------------------
inline std::optional<VolumePatternHit> DetectVolumeGap(const VolumePatternFeatures& f) {
    using namespace VolumePatternConfig;

    if (f.lvnRuns.empty()) return std::nullopt;

    // HARDENING: Require meaningful median volume to avoid false positives
    if (f.medianVolume <= 0) return std::nullopt;

    // Find the widest LVN run that meets gap criteria
    const VolumeRun* bestGap = nullptr;
    float bestStrength = 0.0f;

    for (const auto& run : f.lvnRuns) {
        // Must be wide enough
        if (run.WidthTicks() < f.minGapWidthTicks) continue;

        // Must be sufficiently low volume
        if (run.meanVolume > f.medianVolume * GAP_VOL_RATIO_MAX) continue;

        // Must be bounded by meaningful volume regions (HVN or VA boundary)
        bool lowerBound = (run.startTick <= f.valTick + 1);
        bool upperBound = (run.endTick >= f.vahTick - 1);

        // Check for HVN clusters on either side
        for (const auto& hvn : f.hvnRuns) {
            if (hvn.endTick <= run.startTick && hvn.endTick >= run.startTick - 3) {
                lowerBound = true;
            }
            if (hvn.startTick >= run.endTick && hvn.startTick <= run.endTick + 3) {
                upperBound = true;
            }
        }

        if (!lowerBound && !upperBound) continue;

        // Calculate strength based on width and volume emptiness
        float widthFactor = static_cast<float>(run.WidthTicks()) / f.vaWidthTicks;
        float emptyFactor = (f.medianVolume > 0)
            ? 1.0f - static_cast<float>(run.meanVolume / f.medianVolume)
            : 0.5f;
        float strength = (std::min)(1.0f, (widthFactor + emptyFactor) / 2.0f);

        if (strength > bestStrength) {
            bestStrength = strength;
            bestGap = &run;
        }
    }

    if (!bestGap) return std::nullopt;

    VolumePatternHit hit;
    hit.type = VolumeProfilePattern::VOLUME_GAP;
    hit.lowTick = bestGap->startTick;
    hit.highTick = bestGap->endTick;
    hit.anchorTick = (bestGap->startTick + bestGap->endTick) / 2;
    hit.strength01 = bestStrength;
    return hit;
}

// ---------------------------------------------------------------------------
// VOLUME_VACUUM: Stricter LVN corridor (potential slippage zone)
// ---------------------------------------------------------------------------
inline std::optional<VolumePatternHit> DetectVolumeVacuum(const VolumePatternFeatures& f) {
    using namespace VolumePatternConfig;

    if (f.lvnRuns.empty()) return std::nullopt;

    // HARDENING: Require meaningful median volume to avoid false positives
    // on sparse/early-session profiles where median collapses toward 0
    if (f.medianVolume <= 0) return std::nullopt;

    // Find the most extreme vacuum (widest + emptiest)
    const VolumeRun* bestVacuum = nullptr;
    float bestStrength = 0.0f;

    for (const auto& run : f.lvnRuns) {
        // Must be wider than gap threshold
        if (run.WidthTicks() < f.minVacuumWidthTicks) continue;

        // Must be very low volume (stricter than gap)
        if (run.meanVolume > f.medianVolume * VACUUM_VOL_RATIO_MAX) continue;

        // Calculate strength
        float widthFactor = static_cast<float>(run.WidthTicks()) / f.vaWidthTicks;
        float emptyFactor = (f.medianVolume > 0)
            ? 1.0f - static_cast<float>(run.meanVolume / f.medianVolume)
            : 0.5f;
        float strength = (std::min)(1.0f, (widthFactor * 0.4f + emptyFactor * 0.6f));

        if (strength > bestStrength) {
            bestStrength = strength;
            bestVacuum = &run;
        }
    }

    if (!bestVacuum) return std::nullopt;

    VolumePatternHit hit;
    hit.type = VolumeProfilePattern::VOLUME_VACUUM;
    hit.lowTick = bestVacuum->startTick;
    hit.highTick = bestVacuum->endTick;
    hit.anchorTick = (bestVacuum->startTick + bestVacuum->endTick) / 2;
    hit.strength01 = bestStrength;
    return hit;
}

// ---------------------------------------------------------------------------
// VOLUME_SHELF: HVN plateau with sharp edge drop-off
// ---------------------------------------------------------------------------
inline std::optional<VolumePatternHit> DetectVolumeShelf(const VolumePatternFeatures& f) {
    using namespace VolumePatternConfig;

    if (f.hvnRuns.empty()) return std::nullopt;

    const VolumeRun* bestShelf = nullptr;
    float bestStrength = 0.0f;
    bool hasEdgeDrop = false;

    for (const auto& run : f.hvnRuns) {
        // Must be wide enough for a shelf
        if (run.WidthTicks() < f.minShelfWidthTicks) continue;

        // Check flatness within the run
        // We need to look at individual bin volumes within the run
        std::vector<double> runVolumes;
        for (const auto& [tick, vol] : f.sortedBins) {
            if (tick >= run.startTick && tick <= run.endTick) {
                runVolumes.push_back(vol);
            }
        }

        if (runVolumes.size() < 3) continue;

        // Sort to get percentiles
        std::sort(runVolumes.begin(), runVolumes.end());
        size_t n = runVolumes.size();
        double p10 = runVolumes[n / 10];
        double p50 = runVolumes[n / 2];
        double p90 = runVolumes[n * 9 / 10];

        if (p50 <= 0) continue;
        double flatness = (p90 - p10) / p50;
        if (flatness > SHELF_FLATNESS_MAX) continue;

        // Check for edge drop-off (at least one edge must have sharp drop)
        double edgeVol = run.meanVolume;
        double outsideVolLow = 0.0, outsideVolHigh = 0.0;
        int outsideCountLow = 0, outsideCountHigh = 0;

        for (const auto& [tick, vol] : f.sortedBins) {
            if (tick < run.startTick && tick >= run.startTick - 3) {
                outsideVolLow += vol;
                outsideCountLow++;
            }
            if (tick > run.endTick && tick <= run.endTick + 3) {
                outsideVolHigh += vol;
                outsideCountHigh++;
            }
        }

        double avgOutsideLow = (outsideCountLow > 0) ? outsideVolLow / outsideCountLow : edgeVol;
        double avgOutsideHigh = (outsideCountHigh > 0) ? outsideVolHigh / outsideCountHigh : edgeVol;

        bool lowEdgeDrop = (avgOutsideLow > 0 && edgeVol / avgOutsideLow >= SHELF_EDGE_DROP_MIN);
        bool highEdgeDrop = (avgOutsideHigh > 0 && edgeVol / avgOutsideHigh >= SHELF_EDGE_DROP_MIN);

        if (!lowEdgeDrop && !highEdgeDrop) continue;

        hasEdgeDrop = true;

        // Calculate strength based on width, flatness, and edge drop
        float widthFactor = (std::min)(1.0f, static_cast<float>(run.WidthTicks()) / (f.vaWidthTicks * 0.3f));
        float flatFactor = 1.0f - static_cast<float>(flatness / SHELF_FLATNESS_MAX);
        float edgeFactor = (lowEdgeDrop && highEdgeDrop) ? 1.0f : 0.7f;
        float strength = (widthFactor * 0.3f + flatFactor * 0.3f + edgeFactor * 0.4f);

        if (strength > bestStrength) {
            bestStrength = strength;
            bestShelf = &run;
        }
    }

    if (!bestShelf) return std::nullopt;

    VolumePatternHit hit;
    hit.type = VolumeProfilePattern::VOLUME_SHELF;
    hit.lowTick = bestShelf->startTick;
    hit.highTick = bestShelf->endTick;
    hit.anchorTick = (bestShelf->startTick + bestShelf->endTick) / 2;
    hit.strength01 = bestStrength;
    return hit;
}

// ---------------------------------------------------------------------------
// LEDGE_PATTERN: Significant step-change in volume density
// ---------------------------------------------------------------------------
inline std::optional<VolumePatternHit> DetectLedgePattern(const VolumePatternFeatures& f) {
    using namespace VolumePatternConfig;

    if (f.gradients.size() < 5 || f.gradientMAD <= 0) return std::nullopt;

    // Find significant gradient steps using robust threshold
    const double sigmaThreshold = f.gradientMAD * LEDGE_GRADIENT_SIGMA * 1.4826;  // MAD to sigma

    int bestLedgeTick = 0;
    float bestStrength = 0.0f;
    int bestIdx = -1;

    for (size_t i = LEDGE_PERSISTENCE_BINS; i < f.gradients.size() - LEDGE_PERSISTENCE_BINS; ++i) {
        double grad = std::abs(f.gradients[i]);
        if (grad < sigmaThreshold) continue;

        // Check persistence: gradient should maintain direction over neighbors
        bool persistent = true;
        int dir = (f.gradients[i] > 0) ? 1 : -1;
        for (int j = 1; j <= LEDGE_PERSISTENCE_BINS; ++j) {
            if (i >= static_cast<size_t>(j)) {
                int prevDir = (f.gradients[i-j] > 0) ? 1 : -1;
                if (prevDir != dir && std::abs(f.gradients[i-j]) > sigmaThreshold * 0.5) {
                    persistent = false;
                    break;
                }
            }
        }

        if (!persistent) continue;

        float strength = static_cast<float>((std::min)(grad / (sigmaThreshold * 3.0), 1.0));
        if (strength > bestStrength) {
            bestStrength = strength;
            bestIdx = static_cast<int>(i);
            bestLedgeTick = f.sortedBins[i].first;
        }
    }

    if (bestIdx < 0) return std::nullopt;

    VolumePatternHit hit;
    hit.type = VolumeProfilePattern::LEDGE_PATTERN;
    hit.lowTick = (bestIdx > 0) ? f.sortedBins[bestIdx - 1].first : bestLedgeTick;
    hit.highTick = (bestIdx < static_cast<int>(f.sortedBins.size()) - 1)
        ? f.sortedBins[bestIdx + 1].first : bestLedgeTick;
    hit.anchorTick = bestLedgeTick;
    hit.strength01 = bestStrength;
    return hit;
}

// ---------------------------------------------------------------------------
// VOLUME_CLUSTER: Concentrated HVN mass in VA, no LVN corridors
// ---------------------------------------------------------------------------
inline std::optional<VolumePatternHit> DetectVolumeCluster(const VolumePatternFeatures& f) {
    using namespace VolumePatternConfig;

    if (f.hvnRuns.empty()) return std::nullopt;

    // Calculate HVN mass within VA
    double hvnMassInVA = 0.0;
    double totalVAVolume = 0.0;

    for (const auto& [tick, vol] : f.sortedBins) {
        if (tick >= f.valTick && tick <= f.vahTick) {
            totalVAVolume += vol;
            if (vol >= f.hvnThreshold) {
                hvnMassInVA += vol;
            }
        }
    }

    if (totalVAVolume <= 0) return std::nullopt;

    float hvnMassRatio = static_cast<float>(hvnMassInVA / totalVAVolume);
    if (hvnMassRatio < CLUSTER_HVN_MASS_MIN) return std::nullopt;

    // Check for absence of significant LVN corridors within VA
    int maxLvnGapWidth = 0;
    for (const auto& lvn : f.lvnRuns) {
        // Check if LVN is within VA
        if (lvn.startTick >= f.valTick && lvn.endTick <= f.vahTick) {
            maxLvnGapWidth = (std::max)(maxLvnGapWidth, lvn.WidthTicks());
        }
    }

    float lvnGapRatio = (f.vaWidthTicks > 0)
        ? static_cast<float>(maxLvnGapWidth) / f.vaWidthTicks
        : 0.0f;

    if (lvnGapRatio > CLUSTER_LVN_GAP_MAX_RATIO) return std::nullopt;

    // Calculate strength based on HVN concentration
    float strength = hvnMassRatio * (1.0f - lvnGapRatio * 2.0f);
    strength = (std::max)(0.0f, (std::min)(1.0f, strength));

    VolumePatternHit hit;
    hit.type = VolumeProfilePattern::VOLUME_CLUSTER;
    hit.lowTick = f.valTick;
    hit.highTick = f.vahTick;
    hit.anchorTick = f.pocTick;
    hit.strength01 = strength;
    return hit;
}

// ---------------------------------------------------------------------------
// VOLUME_MIGRATION: POC drift over time (requires MigrationHistory)
// ---------------------------------------------------------------------------
inline std::optional<VolumePatternHit> DetectVolumeMigration(
    const VolumePatternFeatures& f,
    const MigrationHistory& history)
{
    if (!f.valid || f.vaWidthTicks <= 0) return std::nullopt;

    int netDrift = 0;
    int reversals = 0;

    if (!history.HasMonotonicDrift(f.vaWidthTicks, netDrift, reversals)) {
        return std::nullopt;
    }

    // Calculate strength based on drift magnitude
    float driftRatio = static_cast<float>(std::abs(netDrift)) / f.vaWidthTicks;
    float strength = (std::min)(1.0f, driftRatio * 2.0f);

    VolumePatternHit hit;
    hit.type = VolumeProfilePattern::VOLUME_MIGRATION;
    hit.lowTick = (netDrift > 0) ? (f.pocTick - netDrift) : f.pocTick;
    hit.highTick = (netDrift > 0) ? f.pocTick : (f.pocTick - netDrift);
    hit.anchorTick = f.pocTick;
    hit.strength01 = strength;
    return hit;
}

// ============================================================================
// TPO MECHANICS DETECTION
// Compares IB distribution snapshot vs current session distribution using
// overlap metric: sum(min(A,B)) / sum(max(A,B))
// ============================================================================

/**
 * ComputeDistributionOverlap: Calculate overlap between two sorted volume distributions
 * Formula: sum(min(A[i], B[i])) / sum(max(A[i], B[i]))
 *
 * Uses two-pointer walk for O(n+m) efficiency on sorted vectors.
 * Both distA and distB MUST be sorted by tick ascending.
 *
 * @param distA First distribution (tick -> volume), sorted by tick ascending
 * @param distB Second distribution (tick -> volume), sorted by tick ascending
 * @return Overlap ratio [0, 1], or nullopt if invalid (empty/degenerate)
 */
inline std::optional<float> ComputeDistributionOverlap(
    const std::vector<std::pair<int, double>>& distA,
    const std::vector<std::pair<int, double>>& distB)
{
    if (distA.empty() || distB.empty()) return std::nullopt;

    double sumMin = 0.0;
    double sumMax = 0.0;

    // Two-pointer walk over sorted distributions
    size_t i = 0, j = 0;
    while (i < distA.size() || j < distB.size()) {
        int tickA = (i < distA.size()) ? distA[i].first : INT_MAX;
        int tickB = (j < distB.size()) ? distB[j].first : INT_MAX;

        double volA = 0.0, volB = 0.0;

        if (tickA < tickB) {
            // Tick only in A
            volA = distA[i].second;
            ++i;
        } else if (tickB < tickA) {
            // Tick only in B
            volB = distB[j].second;
            ++j;
        } else {
            // Tick in both
            volA = distA[i].second;
            volB = distB[j].second;
            ++i;
            ++j;
        }

        sumMin += (std::min)(volA, volB);
        sumMax += (std::max)(volA, volB);
    }

    if (sumMax <= 0.0) return std::nullopt;
    return static_cast<float>(sumMin / sumMax);
}

/**
 * DetectTPOMechanics: Classify overlap as TPO_OVERLAP or TPO_SEPARATION
 *
 * @param ibSnapshot The IB distribution snapshot (captured at IB freeze)
 * @param currentDist Current session distribution (sorted by tick ascending)
 * @param currentTickSize Current profile tick size (for compatibility check)
 * @return TPOMechanicsHit if classification succeeds, nullopt otherwise
 */
inline std::optional<TPOMechanicsHit> DetectTPOMechanics(
    const IBDistSnapshot& ibSnapshot,
    const std::vector<std::pair<int, double>>& currentDist,
    double currentTickSize)
{
    // Compatibility check
    if (!ibSnapshot.IsCompatible(currentTickSize)) {
        return std::nullopt;
    }

    if (currentDist.empty()) {
        return std::nullopt;
    }

    // Compute overlap
    auto overlapOpt = ComputeDistributionOverlap(ibSnapshot.dist, currentDist);
    if (!overlapOpt) {
        return std::nullopt;
    }

    const float overlap01 = *overlapOpt;

    // Classification thresholds
    using namespace VolumePatternConfig;

    TPOMechanicsHit hit;
    hit.overlap01 = overlap01;

    if (overlap01 >= OVERLAP_MIN) {
        hit.type = TPOMechanics::TPO_OVERLAP;
        return hit;
    } else if (overlap01 <= SEPARATION_MAX) {
        hit.type = TPOMechanics::TPO_SEPARATION;
        return hit;
    }

    // Mid-range: no classification
    return std::nullopt;
}

// ============================================================================
// BREAKOUT METRICS (structural breach + acceptance computation)
// ============================================================================

struct BreakoutMetrics {
    // Mass ratios (relative to totalVolume)
    float massAboveVAH = 0.0f;      // Volume above reference VAH / total
    float massBelowVAL = 0.0f;      // Volume below reference VAL / total
    float hvnMassAbove = 0.0f;      // HVN volume above VAH / total
    float hvnMassBelow = 0.0f;      // HVN volume below VAL / total

    // HVN cluster counts outside boundary
    int hvnClustersAbove = 0;
    int hvnClustersBelow = 0;

    // Tick ranges
    int outsideAboveHighTick = 0;   // Highest tick with volume above VAH
    int outsideBelowLowTick = 0;    // Lowest tick with volume below VAL

    bool valid = false;
};

/**
 * ComputeBreakoutMetrics: Compute mass and HVN metrics outside balance boundary
 * @param f Current VolumePatternFeatures (with sortedBins and thresholds)
 * @param ref BalanceSnapshot reference boundary
 * @return BreakoutMetrics with outside mass ratios and HVN cluster info
 */
inline BreakoutMetrics ComputeBreakoutMetrics(
    const VolumePatternFeatures& f,
    const BalanceSnapshot& ref)
{
    BreakoutMetrics m;

    if (!ref.IsCoherent() || f.sortedBins.empty() || f.totalVolume <= 0.0) {
        return m;
    }

    double volAbove = 0.0, volBelow = 0.0;
    double hvnAbove = 0.0, hvnBelow = 0.0;
    int highestAbove = ref.vahTick, lowestBelow = ref.valTick;

    // Count volume and HVN mass outside boundary
    for (const auto& [tick, vol] : f.sortedBins) {
        if (tick > ref.vahTick) {
            volAbove += vol;
            if (vol >= f.hvnThreshold) {
                hvnAbove += vol;
            }
            if (tick > highestAbove) highestAbove = tick;
        } else if (tick < ref.valTick) {
            volBelow += vol;
            if (vol >= f.hvnThreshold) {
                hvnBelow += vol;
            }
            if (tick < lowestBelow) lowestBelow = tick;
        }
    }

    // Count HVN clusters (contiguous runs) outside boundary
    for (const auto& run : f.hvnRuns) {
        if (run.startTick > ref.vahTick) {
            m.hvnClustersAbove++;
        } else if (run.endTick < ref.valTick) {
            m.hvnClustersBelow++;
        }
    }

    m.massAboveVAH = static_cast<float>(volAbove / f.totalVolume);
    m.massBelowVAL = static_cast<float>(volBelow / f.totalVolume);
    m.hvnMassAbove = static_cast<float>(hvnAbove / f.totalVolume);
    m.hvnMassBelow = static_cast<float>(hvnBelow / f.totalVolume);
    m.outsideAboveHighTick = highestAbove;
    m.outsideBelowLowTick = lowestBelow;
    m.valid = true;

    return m;
}

/**
 * DetectBreakoutOrTrap: Classify breakout vs trap based on structural metrics
 * @param f Current VolumePatternFeatures
 * @param ref BalanceSnapshot reference boundary
 * @param mechanics Current TPOMechanics (used to gate acceptance)
 * @return VolumePatternHit for VOLUME_BREAKOUT or LOW_VOLUME_BREAKOUT, or nullopt
 */
inline std::optional<VolumePatternHit> DetectBreakoutOrTrap(
    const VolumePatternFeatures& f,
    const BalanceSnapshot& ref,
    const std::vector<TPOMechanics>& mechanics)
{
    using namespace VolumePatternConfig;

    // Eligibility gate
    if (!ref.IsCoherent() || !f.valid) {
        return std::nullopt;
    }

    // Check tick size compatibility
    // (Note: ref.tickSize should match the profile's tick size)

    // Compute structural metrics
    BreakoutMetrics m = ComputeBreakoutMetrics(f, ref);
    if (!m.valid) {
        return std::nullopt;
    }

    // Determine mechanics context
    bool isSeparation = false;
    bool isOverlap = false;
    for (const auto& mech : mechanics) {
        if (mech == TPOMechanics::TPO_SEPARATION) isSeparation = true;
        if (mech == TPOMechanics::TPO_OVERLAP) isOverlap = true;
    }

    // Check for breach on each side
    const bool breachAbove = (m.massAboveVAH >= OUTSIDE_MASS_BREACH_MIN);
    const bool breachBelow = (m.massBelowVAL >= OUTSIDE_MASS_BREACH_MIN);

    // Ambiguity guard: both sides breach significantly
    if (breachAbove && breachBelow) {
        if (m.massAboveVAH >= BOTH_SIDES_BREACH_GUARD &&
            m.massBelowVAL >= BOTH_SIDES_BREACH_GUARD) {
            // Ambiguous - emit nothing
            return std::nullopt;
        }
    }

    // Determine dominant side (if any)
    enum class BreachSide { NONE, ABOVE, BELOW };
    BreachSide side = BreachSide::NONE;

    if (breachAbove && !breachBelow) {
        side = BreachSide::ABOVE;
    } else if (breachBelow && !breachAbove) {
        side = BreachSide::BELOW;
    } else if (breachAbove && breachBelow) {
        // Pick dominant side by mass
        side = (m.massAboveVAH > m.massBelowVAL) ? BreachSide::ABOVE : BreachSide::BELOW;
    }

    if (side == BreachSide::NONE) {
        return std::nullopt;
    }

    // Get metrics for the dominant side
    float outsideMass = (side == BreachSide::ABOVE) ? m.massAboveVAH : m.massBelowVAL;
    float hvnMass = (side == BreachSide::ABOVE) ? m.hvnMassAbove : m.hvnMassBelow;
    int hvnClusters = (side == BreachSide::ABOVE) ? m.hvnClustersAbove : m.hvnClustersBelow;
    int boundaryTick = (side == BreachSide::ABOVE) ? ref.vahTick : ref.valTick;
    int outsideTick = (side == BreachSide::ABOVE) ? m.outsideAboveHighTick : m.outsideBelowLowTick;

    // Check acceptance conditions
    bool accepted = false;
    if (outsideMass >= OUTSIDE_MASS_ACCEPT_MIN) {
        accepted = true;
    } else if (hvnMass >= OUTSIDE_HVN_MASS_MIN && hvnClusters >= 1) {
        accepted = true;
    }

    // Decision logic:
    // - If separation + accepted -> VOLUME_BREAKOUT
    // - If overlap/none + breach but not accepted -> LOW_VOLUME_BREAKOUT (trap)
    // - Otherwise -> nothing

    VolumePatternHit hit;
    hit.anchorTick = boundaryTick;
    hit.lowTick = (side == BreachSide::ABOVE) ? boundaryTick : outsideTick;
    hit.highTick = (side == BreachSide::ABOVE) ? outsideTick : boundaryTick;
    hit.strength01 = outsideMass;  // Use outside mass as strength indicator

    if (isSeparation && accepted) {
        // Valid breakout
        hit.type = VolumeProfilePattern::VOLUME_BREAKOUT;
        return hit;
    } else if (!accepted && outsideMass >= OUTSIDE_MASS_BREACH_MIN && outsideMass <= TRAP_MASS_MAX) {
        // Trap: breach present but acceptance weak, and mechanics suggests balance/unclear
        // Only emit trap if NOT in separation mode (separation without acceptance = ambiguous, not trap)
        if (!isSeparation) {
            hit.type = VolumeProfilePattern::LOW_VOLUME_BREAKOUT;
            return hit;
        }
    }

    return std::nullopt;
}

// ============================================================================
// PATTERN DETECTION RESULT
// ============================================================================

struct VolumePatternResult {
    std::vector<VolumeProfilePattern> patterns;  // Unique enum types
    std::vector<VolumePatternHit> hits;          // Detailed hit metadata
    std::vector<TPOMechanics> tpoMechanics;      // TPO mechanics (unique enums)
    std::vector<TPOMechanicsHit> tpoHits;        // TPO hit metadata
    bool eligible = false;
};

// ============================================================================
// ORCHESTRATOR: Detect all patterns from features
// ============================================================================

inline VolumePatternResult DetectAllPatterns(
    const VolumePatternFeatures& f,
    const MigrationHistory* migrationHistory = nullptr,
    const IBDistSnapshot* ibSnapshot = nullptr,
    double currentTickSize = 0.0,
    const BalanceSnapshot* balanceRef = nullptr)
{
    VolumePatternResult result;

    if (!IsPatternEligible(f)) {
        return result;
    }

    result.eligible = true;

    // Helper to add hit and enum
    auto addHit = [&result](std::optional<VolumePatternHit> hit) {
        if (hit) {
            result.hits.push_back(*hit);
            // Add enum if not already present
            auto it = std::find(result.patterns.begin(), result.patterns.end(), hit->type);
            if (it == result.patterns.end()) {
                result.patterns.push_back(hit->type);
            }
        }
    };

    // Run all detectors
    // Note: VACUUM is stricter than GAP. If VACUUM fires, suppress GAP for same run
    // to avoid "vacuum is also a gap" redundancy.
    auto vacuumHit = DetectVolumeVacuum(f);
    auto gapHit = DetectVolumeGap(f);

    // If both fire for overlapping regions, prefer VACUUM (stricter)
    bool suppressGap = false;
    if (vacuumHit && gapHit) {
        // Check if they're the same or overlapping run
        if (gapHit->lowTick >= vacuumHit->lowTick && gapHit->highTick <= vacuumHit->highTick) {
            suppressGap = true;  // Vacuum subsumes gap
        }
    }

    if (gapHit && !suppressGap) {
        addHit(gapHit);
    }
    addHit(vacuumHit);
    addHit(DetectVolumeShelf(f));
    addHit(DetectLedgePattern(f));
    addHit(DetectVolumeCluster(f));

    if (migrationHistory) {
        addHit(DetectVolumeMigration(f, *migrationHistory));
    }

    // TPO Mechanics detection
    // Requires IB snapshot and current distribution from features
    if (ibSnapshot && ibSnapshot->valid && currentTickSize > 0.0 && !f.sortedBins.empty()) {
        auto tpoHit = DetectTPOMechanics(*ibSnapshot, f.sortedBins, currentTickSize);
        if (tpoHit) {
            result.tpoHits.push_back(*tpoHit);
            // Add enum if not already present
            auto it = std::find(result.tpoMechanics.begin(), result.tpoMechanics.end(), tpoHit->type);
            if (it == result.tpoMechanics.end()) {
                result.tpoMechanics.push_back(tpoHit->type);
            }
        }
    }

    // Breakout/Trap detection
    // Requires balance reference snapshot and mechanics context
    if (balanceRef && balanceRef->IsCoherent()) {
        addHit(DetectBreakoutOrTrap(f, *balanceRef, result.tpoMechanics));
    }

    return result;
}

// ============================================================================
// STRING CONVERSION FOR VOLUME PATTERN
// ============================================================================

inline const char* VolumeProfilePatternToString(VolumeProfilePattern p) {
    switch (p) {
        case VolumeProfilePattern::VOLUME_SHELF: return "SHELF";
        case VolumeProfilePattern::VOLUME_CLUSTER: return "CLUSTER";
        case VolumeProfilePattern::VOLUME_GAP: return "GAP";
        case VolumeProfilePattern::VOLUME_VACUUM: return "VACUUM";
        case VolumeProfilePattern::LEDGE_PATTERN: return "LEDGE";
        case VolumeProfilePattern::VOLUME_MIGRATION: return "MIGRATION";
        case VolumeProfilePattern::VOLUME_BREAKOUT: return "BREAKOUT";
        case VolumeProfilePattern::LOW_VOLUME_BREAKOUT: return "LV_BREAKOUT";
    }
    return "UNK";
}

inline const char* TPOMechanicsToString(TPOMechanics m) {
    switch (m) {
        case TPOMechanics::TPO_OVERLAP: return "OVERLAP";
        case TPOMechanics::TPO_SEPARATION: return "SEPARATION";
    }
    return "UNK";
}

} // namespace AMT

#endif // AMT_VOLUMEPATTERNS_H
