// ============================================================================
// AMT_VolumeProfile.h
// Session Volume Profile structures and VBP integration
// Extracted from AuctionSensor_v1.cpp for modularization
// ============================================================================

#ifndef AMT_VOLUMEPROFILE_H
#define AMT_VOLUMEPROFILE_H

#include "sierrachart.h"
#include "amt_core.h"
#include "AMT_config.h"
#include "AMT_Levels.h"
#include "AMT_Helpers.h"
#include "AMT_Snapshots.h"  // For RollingDist (profile baselines)
#include "AMT_Logger.h"  // For LogManager integration
#include "AMT_ProfileShape.h"  // For ProfileFeatures, ClassifyProfileShape, ResolveShapeWithDayStructure
#include "AMT_Volatility.h"  // For VolatilityRegime (adaptive break thresholds)
#include <map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <string>

// Sierra Chart VolumeAtPrice alias (s_VolumeAtPriceV2 is the API struct)
using VolumeAtPrice = s_VolumeAtPriceV2;

namespace AMT {

// ============================================================================
// PROFILE PROGRESS BUCKETS (for progress-conditioned baselines)
// ============================================================================
// Historical profile baselines are keyed by (SessionType, ProgressBucket).
// This allows comparing "today at +30m" vs "historical at +30m".
// ============================================================================

enum class ProgressBucket : int {
    BUCKET_15M = 0,    // +15 minutes into session
    BUCKET_30M = 1,    // +30 minutes (pre-IB for RTH)
    BUCKET_60M = 2,    // +60 minutes (IB complete for RTH)
    BUCKET_120M = 3,   // +120 minutes (mid-session)
    BUCKET_EOD = 4,    // End of session (full profile)
    BUCKET_COUNT = 5   // Number of buckets
};

inline const char* ProgressBucketToString(ProgressBucket bucket) {
    switch (bucket) {
        case ProgressBucket::BUCKET_15M:  return "15m";
        case ProgressBucket::BUCKET_30M:  return "30m";
        case ProgressBucket::BUCKET_60M:  return "60m";
        case ProgressBucket::BUCKET_120M: return "120m";
        case ProgressBucket::BUCKET_EOD:  return "EOD";
        default: return "UNK";
    }
}

// Minutes into session for each bucket threshold
inline int ProgressBucketMinutes(ProgressBucket bucket) {
    switch (bucket) {
        case ProgressBucket::BUCKET_15M:  return 15;
        case ProgressBucket::BUCKET_30M:  return 30;
        case ProgressBucket::BUCKET_60M:  return 60;
        case ProgressBucket::BUCKET_120M: return 120;
        case ProgressBucket::BUCKET_EOD:  return 9999;  // Always matches at EOD
        default: return 0;
    }
}

// Given minutes into session, return the appropriate bucket
inline ProgressBucket GetProgressBucket(int minutesIntoSession) {
    if (minutesIntoSession >= 120) return ProgressBucket::BUCKET_120M;
    if (minutesIntoSession >= 60)  return ProgressBucket::BUCKET_60M;
    if (minutesIntoSession >= 30)  return ProgressBucket::BUCKET_30M;
    if (minutesIntoSession >= 15)  return ProgressBucket::BUCKET_15M;
    return ProgressBucket::BUCKET_15M;  // Default to earliest bucket
}

// ============================================================================
// PROFILE FEATURE SNAPSHOT (captured at each bucket boundary)
// ============================================================================
// Stores dimensionless profile metrics at a specific progress point.
// Used for both historical baseline storage and current session comparison.
// ============================================================================

struct ProfileFeatureSnapshot {
    // --- Identity ---
    ProgressBucket bucket = ProgressBucket::BUCKET_15M;
    int minutesIntoSession = 0;

    // --- VA Width (dimensionless) ---
    double vaWidthTicks = 0.0;           // (VAH - VAL) / tickSize
    double sessionRangeTicks = 0.0;      // (SessionHigh - SessionLow) / tickSize
    double vaWidthRatio = 0.0;           // vaWidthTicks / sessionRangeTicks (if range > 0)

    // --- POC Dominance ---
    double pocShare = 0.0;               // volume_at_POC / total_session_volume [0, 1]
    double pocVolume = 0.0;              // Absolute volume at POC (for debugging)
    double totalVolume = 0.0;            // Total session volume (for debugging)

    // --- Volume Sufficiency (for progress-conditioned maturity) ---
    double cumulativeVolume = 0.0;       // Total volume up to this bucket boundary

    // --- Profile Shape (supplementary) ---
    int priceLevelsCount = 0;            // Number of price levels with volume
    double vaVolumeShare = 0.0;          // Volume in VA / total volume (should be ~0.70)

    // --- Validity ---
    bool valid = false;                  // True if VA width data was available
    bool pocShareValid = false;          // True if POC volume data was available (requires VAP access)

    // Compute derived ratios (call after setting raw values)
    void ComputeDerived() {
        if (sessionRangeTicks > 0.0) {
            vaWidthRatio = vaWidthTicks / sessionRangeTicks;
        } else {
            vaWidthRatio = 0.0;
        }
        if (totalVolume > 0.0) {
            pocShare = pocVolume / totalVolume;
        } else {
            pocShare = 0.0;
        }
    }
};

// ============================================================================
// PROFILE MATURITY THRESHOLDS
// ============================================================================
// Minimum requirements before current session profile is "decision-grade".
// Below these thresholds, profile metrics are marked invalid.
// ============================================================================

namespace ProfileMaturity {
    constexpr int MIN_PRICE_LEVELS = 5;         // At least 5 price levels with volume
    constexpr int MIN_BARS = 5;                 // At least 5 bars into session
    constexpr int MIN_MINUTES = 10;             // At least 10 minutes into session

    // VOLUME SUFFICIENCY: Percentile-based (self-calibrating), NO FALLBACK
    // When baseline available: require volume >= Nth percentile of historical at same bucket
    // When baseline unavailable: volumeSufficiencyValid = false, gate is NOT applied
    // NO-FALLBACK POLICY: We do NOT inject absolute volume thresholds when baseline unavailable
    constexpr double VOLUME_SUFFICIENCY_PERCENTILE = 20.0;  // Require >= 20th percentile of historical

    // POC stability: POC must not have moved > N ticks in last M bars
    constexpr int POC_STABILITY_WINDOW = 3;     // Check last 3 bars
    constexpr int POC_STABILITY_MAX_DRIFT = 4;  // Max 4 ticks drift in window
}

// ============================================================================
// PROFILE STRUCTURE ERROR REASONS (for ProfileStructureResult validity)
// ============================================================================
enum class ProfileStructureErrorReason : int {
    NONE = 0,

    // Warmup states (expected, not errors)
    WARMUP_VBP_STUDY = 10,       // VbP study not populated yet
    WARMUP_THRESHOLDS = 11,      // Thresholds not computed
    WARMUP_MATURITY = 12,        // Profile not yet mature

    // Validation errors
    ERR_TICK_SIZE_INVALID = 20,  // tickSize <= 0
    ERR_NO_PRICE_LEVELS = 21,    // Empty volume_profile
    ERR_INVALID_POC = 22,        // POC not valid
    ERR_INVALID_VA = 23,         // VAH/VAL invalid (VAH <= VAL)

    // Session events
    SESSION_RESET = 30           // Just transitioned, no data yet
};

inline bool IsProfileStructureWarmup(ProfileStructureErrorReason r) {
    return r >= ProfileStructureErrorReason::WARMUP_VBP_STUDY &&
           r <= ProfileStructureErrorReason::WARMUP_MATURITY;
}

inline const char* ProfileStructureErrorReasonToString(ProfileStructureErrorReason r) {
    switch (r) {
        case ProfileStructureErrorReason::NONE:                return "NONE";
        case ProfileStructureErrorReason::WARMUP_VBP_STUDY:    return "WARMUP_VBP";
        case ProfileStructureErrorReason::WARMUP_THRESHOLDS:   return "WARMUP_THRESH";
        case ProfileStructureErrorReason::WARMUP_MATURITY:     return "WARMUP_MATURE";
        case ProfileStructureErrorReason::ERR_TICK_SIZE_INVALID: return "ERR_TICK";
        case ProfileStructureErrorReason::ERR_NO_PRICE_LEVELS: return "ERR_NO_LEVELS";
        case ProfileStructureErrorReason::ERR_INVALID_POC:     return "ERR_POC";
        case ProfileStructureErrorReason::ERR_INVALID_VA:      return "ERR_VA";
        case ProfileStructureErrorReason::SESSION_RESET:       return "SESS_RESET";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// PROFILE MATURITY STATE (FSM for profile development tracking)
// ============================================================================
enum class ProfileMaturityState : int {
    IMMATURE = 0,    // Does not meet structural gates
    DEVELOPING = 1,  // Meets some gates, approaching mature
    MATURE = 2       // All gates passed with confirmation
};

inline const char* ProfileMaturityStateToString(ProfileMaturityState s) {
    switch (s) {
        case ProfileMaturityState::IMMATURE:   return "IMMATURE";
        case ProfileMaturityState::DEVELOPING: return "DEVELOPING";
        case ProfileMaturityState::MATURE:     return "MATURE";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// PROFILE STRUCTURE CONFIG (tuning parameters for ComputeStructure)
// ============================================================================
struct ProfileStructureConfig {
    // Maturity FSM hysteresis
    int maturityConfirmationBars = 3;   // Bars required to confirm state change

    // Logging control
    int logIntervalBars = 50;           // Log metrics every N bars
    bool logOnMaturityChange = true;    // Log on state transitions
};

// ============================================================================
// PROFILE STRUCTURE RESULT (SSOT for profile validity, metrics, maturity)
// ============================================================================
struct ProfileStructureResult {
    // === OVERALL VALIDITY ===
    bool IsReady() const {
        return volumeProfilePopulated && pocValid && vaValid &&
               errorReason == ProfileStructureErrorReason::NONE;
    }
    bool IsWarmup() const { return IsProfileStructureWarmup(errorReason); }
    bool IsHardError() const {
        return errorReason >= ProfileStructureErrorReason::ERR_TICK_SIZE_INVALID &&
               errorReason < ProfileStructureErrorReason::SESSION_RESET;
    }

    ProfileStructureErrorReason errorReason = ProfileStructureErrorReason::WARMUP_VBP_STUDY;
    int errorBar = -1;

    // === COMPONENT VALIDITY FLAGS ===
    bool volumeProfilePopulated = false;  // PopulateFromVbPStudy succeeded
    bool peaksValleysLoaded = false;      // PopulatePeaksValleysFromVbP succeeded
    bool thresholdsComputed = false;      // ComputeThresholds succeeded
    bool pocVolumeVerified = false;       // volumeAtPOC == maxLevelVolume
    bool pocValid = false;                // session_poc > 0 and valid
    bool vaValid = false;                 // VAH > VAL and both valid

    // === COMPOSITE METRICS (SSOT) ===
    double pocDominance = 0.0;            // POC volume / total volume [0,1]
    bool pocDominanceValid = false;

    int vaWidthTicks = 0;                 // VAH - VAL in ticks
    double vaWidthRatio = 0.0;            // VA width / session range
    bool vaWidthValid = false;

    double profileCompactness = 0.0;      // 1.0 - (vaWidthRatio / 0.70) clamped [0,1]
    bool compactnessValid = false;

    // Raw values (for diagnostics)
    double sessionPOC = 0.0;
    double sessionVAH = 0.0;
    double sessionVAL = 0.0;
    double totalVolume = 0.0;
    double pocVolume = 0.0;
    int priceLevelCount = 0;
    int hvnCount = 0;
    int lvnCount = 0;

    // === MATURITY FSM WITH HYSTERESIS ===
    ProfileMaturityState maturityState = ProfileMaturityState::IMMATURE;
    ProfileMaturityState rawMaturityState = ProfileMaturityState::IMMATURE;
    ProfileMaturityState candidateState = ProfileMaturityState::IMMATURE;
    int candidateConfirmationBars = 0;
    int barsInMaturityState = 0;
    bool isTransitioning = false;         // candidateState != maturityState

    // Individual gate results
    bool hasMinLevels = false;
    bool hasMinBars = false;
    bool hasMinMinutes = false;
    bool hasMinVolume = false;            // Only meaningful if volumeSufficiencyValid
    bool volumeSufficiencyValid = false;
    double volumePercentile = -1.0;

    // === MATURITY EVENTS (true on transition bar only) ===
    bool becameMature = false;            // Just transitioned to MATURE
    bool becameImmature = false;          // Just transitioned to IMMATURE
    bool maturityChanged = false;         // Any maturity state change

    // === PROFILE SHAPE (unified classification) ===
    // Raw shape: geometric classification from profile features only
    ProfileShape rawShape = ProfileShape::UNDEFINED;
    bool rawShapeValid = false;           // true if classification succeeded
    ShapeError shapeError = ShapeError::NONE;  // Specific error if classification failed
    float shapeConfidence = 0.0f;         // Confidence from classifier [0,1]

    // Resolved shape: after DayStructure constraint applied
    ProfileShape resolvedShape = ProfileShape::UNDEFINED;
    bool shapeConflict = false;           // true if rawShape conflicts with DayStructure
    DayStructure dayStructureUsed = DayStructure::UNDEFINED;  // The constraint applied
    const char* shapeResolution = "PENDING";  // "ACCEPTED" | "CONFLICT" | "PENDING"

    // Shape freeze: once resolved, locks for session
    bool shapeFrozen = false;             // true = don't reclassify this session
    int shapeFrozenBar = -1;              // Bar at which shape was frozen

    // Profile features (for diagnostics/logging)
    float pocInRange = 0.0f;              // x_poc: POC position in range [0,1]
    float breadth = 0.0f;                 // w: VA width / Range (0,1]
    float asymmetry = 0.0f;               // a: POC offset from VA midpoint [-0.5,0.5]
    float peakiness = 0.0f;               // k: POC vol / VA mean
    int hvnClusterCount = 0;              // Number of HVN clusters detected

    // === SHAPE CONFIRMATION GATES (6-gate system) ===
    // Gate 1: Opening range (IB for RTH, SOR for Globex) must be complete
    bool openingRangeComplete = false;

    // Gate 2: POC stability (must be stable for N bars before freeze)
    bool pocStableForShape = false;       // Distinct from maturity POC stability

    // Gate 3: Auction validation (P/b shapes need auction evidence)
    bool auctionValidated = false;        // Range extension + tail confirms shape

    // Gate 4: Failed auction detection (breach + quick return = failed)
    bool noFailedAuction = false;         // true = no failed auction detected

    // Gate 5: Volume distribution confirms geometric shape
    bool volumeConfirmsShape = false;

    // Gate 6: Time-based confidence multiplier
    double timeConfidenceMultiplier = 0.0;

    // Combined gate result
    bool allGatesPass = false;            // All 6 gates passed

    // Opening range tracking (IB for RTH, Session Open Range for Globex)
    double openingRangeHigh = 0.0;
    double openingRangeLow = 0.0;
    bool hasRangeExtensionUp = false;     // Price extended above opening range
    bool hasRangeExtensionDown = false;   // Price extended below opening range
    bool failedAuctionUp = false;         // Breached above, returned quickly
    bool failedAuctionDown = false;       // Breached below, returned quickly

    // Single print detection (tail/excess validation)
    bool hasSinglePrintsAbove = false;    // Thin structure above POC
    bool hasSinglePrintsBelow = false;    // Thin structure below POC

    // Volume distribution metrics
    double volumeUpperThirdRatio = 0.0;   // Volume in upper third / lower third
    double volumeLowerThirdPct = 0.0;     // % of total volume in lower third

    // === CONFIRMED SHAPE STATE (replaces frozen) ===
    bool shapeConfirmed = false;          // Shape passed all gates and is confirmed
    int shapeConfirmedBar = -1;           // Bar when shape was confirmed
    float effectiveConfidence = 0.0f;     // confirmedConfidence * structuralMatchScore
    float structuralMatchScore = 1.0f;    // How well current structure matches confirmed (1.0 = perfect)

    // === BREAK DETECTION ===
    bool breakDetected = false;           // Structural break detected (pending confirmation)
    bool breakConfirmed = false;          // Structural break confirmed (shape will re-evaluate)
    int breakType = 0;                    // ShapeBreakType cast to int (0=NONE, 1=POC_DRIFT, etc.)
    int breakConfirmationBars = 0;        // Bars of break confirmation
    int pocDriftTicks = 0;                // Current POC drift from confirmed position
    int barsAcceptedOutsideVA = 0;        // Consecutive bars accepted outside value area

    // === TRANSITION STATE ===
    bool inTransitionCooldown = false;    // Recently transitioned, in cooldown
    int transitionCount = 0;              // Total shape transitions this session
    int lastTransitionBar = -1;           // Bar of last transition
};

// ============================================================================
// PROFILE MATURITY RESULT (returned by CheckProfileMaturity)
// ============================================================================

struct ProfileMaturityResult {
    bool isMature = false;           // True if profile meets all applied thresholds

    // Individual gate results (structural gates - always applied)
    bool hasMinLevels = false;       // >= MIN_PRICE_LEVELS
    bool hasMinBars = false;         // >= MIN_BARS
    bool hasMinMinutes = false;      // >= MIN_MINUTES

    // Volume sufficiency (progress-conditioned, only applied when baseline ready)
    bool volumeSufficiencyValid = false;  // True if baseline available for volume check
    bool hasMinVolume = false;            // Volume >= Nth percentile (only meaningful if volumeSufficiencyValid)
    double volumePercentile = -1.0;       // Percentile vs historical (-1 = baseline unavailable)

    // Actual values (for diagnostics)
    int priceLevels = 0;
    double totalVolume = 0.0;
    int sessionBars = 0;
    int sessionMinutes = 0;

    // Reason string for logging
    const char* gateFailedReason = nullptr;
};

// Forward declaration for baseline-aware version
struct HistoricalProfileBaseline;

// Simple version (NO baseline available - volume gate NOT applied)
// NO-FALLBACK POLICY: We do NOT inject absolute volume thresholds
inline ProfileMaturityResult CheckProfileMaturity(
    int priceLevels,
    double totalVolume,
    int sessionBars,
    int sessionMinutes)
{
    ProfileMaturityResult result;
    result.priceLevels = priceLevels;
    result.totalVolume = totalVolume;
    result.sessionBars = sessionBars;
    result.sessionMinutes = sessionMinutes;

    // Structural gates (always applied)
    result.hasMinLevels = (priceLevels >= ProfileMaturity::MIN_PRICE_LEVELS);
    result.hasMinBars = (sessionBars >= ProfileMaturity::MIN_BARS);
    result.hasMinMinutes = (sessionMinutes >= ProfileMaturity::MIN_MINUTES);

    // Volume sufficiency NOT AVAILABLE (no baseline)
    result.volumeSufficiencyValid = false;
    result.hasMinVolume = false;  // Meaningless without baseline
    result.volumePercentile = -1.0;

    // Maturity uses ONLY structural gates when volume baseline unavailable
    result.isMature = result.hasMinLevels && result.hasMinBars && result.hasMinMinutes;

    // Set reason for first failed gate (for logging)
    if (!result.hasMinLevels) {
        result.gateFailedReason = "insufficient price levels";
    } else if (!result.hasMinBars) {
        result.gateFailedReason = "insufficient bars";
    } else if (!result.hasMinMinutes) {
        result.gateFailedReason = "insufficient minutes";
    }
    // Note: volume not checked - volumeSufficiencyValid = false indicates this

    return result;
}

// ============================================================================
// HISTORICAL PROFILE BASELINE (progress-conditioned distributions)
// ============================================================================
// Stores RollingDist for each profile feature at each progress bucket.
// Keyed by SessionType (RTH/GBX) - each domain has independent baselines.
// Sessions are the samples (not bars), so N samples = N prior sessions.
// ============================================================================

namespace ProfileBaselineMinSamples {
    constexpr size_t VA_WIDTH = 5;       // Need 5+ prior sessions for VA width baseline
    constexpr size_t POC_DOMINANCE = 5;  // Need 5+ prior sessions for POC dominance baseline
}

struct HistoricalProfileBaseline {
    // --- Per-Bucket Distributions ---
    // vaWidthTicks[bucket] - VA width in ticks at that progress point
    // vaWidthRatio[bucket] - VA width as ratio of session range
    // pocShare[bucket] - POC volume share at that progress point
    // volumeSoFar[bucket] - cumulative volume at that progress point (for sufficiency)
    RollingDist vaWidthTicks[static_cast<int>(ProgressBucket::BUCKET_COUNT)];
    RollingDist vaWidthRatio[static_cast<int>(ProgressBucket::BUCKET_COUNT)];
    RollingDist pocShare[static_cast<int>(ProgressBucket::BUCKET_COUNT)];
    RollingDist volumeSoFar[static_cast<int>(ProgressBucket::BUCKET_COUNT)];

    // --- Tracking ---
    int sessionsAccumulated = 0;  // Number of sessions that have contributed
    bool initialized = false;

    // --- Initialize all distributions ---
    void Reset(int maxSamples = 50) {
        for (int i = 0; i < static_cast<int>(ProgressBucket::BUCKET_COUNT); ++i) {
            vaWidthTicks[i].reset(maxSamples);
            vaWidthRatio[i].reset(maxSamples);
            pocShare[i].reset(maxSamples);
            volumeSoFar[i].reset(maxSamples);
        }
        sessionsAccumulated = 0;
        initialized = true;
    }

    // --- Push a snapshot for a specific bucket ---
    void PushSnapshot(const ProfileFeatureSnapshot& snap) {
        if (!snap.valid) return;
        const int idx = static_cast<int>(snap.bucket);
        if (idx < 0 || idx >= static_cast<int>(ProgressBucket::BUCKET_COUNT)) return;

        vaWidthTicks[idx].push(snap.vaWidthTicks);
        if (snap.vaWidthRatio > 0.0) {
            vaWidthRatio[idx].push(snap.vaWidthRatio);
        }
        // Only push pocShare if it was validly computed (not synthesized)
        // NO-FALLBACK POLICY: pocShareValid gates access to pocShare data
        if (snap.pocShareValid) {
            if (snap.pocShare > 0.0) {
                pocShare[idx].push(snap.pocShare);
            }
        }
        if (snap.cumulativeVolume > 0.0) {
            volumeSoFar[idx].push(snap.cumulativeVolume);
        }
    }

    // --- Check readiness for a specific bucket ---
    bool IsReady(ProgressBucket bucket, size_t minSamples = ProfileBaselineMinSamples::VA_WIDTH) const {
        const int idx = static_cast<int>(bucket);
        return vaWidthTicks[idx].size() >= minSamples;
    }

    // --- Get percentile rank for VA width at a bucket ---
    double GetVAWidthPercentile(ProgressBucket bucket, double currentWidthTicks) const {
        const int idx = static_cast<int>(bucket);
        if (vaWidthTicks[idx].size() < ProfileBaselineMinSamples::VA_WIDTH) return -1.0;
        return vaWidthTicks[idx].percentileRank(currentWidthTicks);
    }

    // --- Get percentile rank for POC share at a bucket ---
    double GetPOCSharePercentile(ProgressBucket bucket, double currentPocShare) const {
        const int idx = static_cast<int>(bucket);
        if (pocShare[idx].size() < ProfileBaselineMinSamples::POC_DOMINANCE) return -1.0;
        return pocShare[idx].percentileRank(currentPocShare);
    }

    // --- Diagnostic: Get sample counts for a bucket ---
    void GetSampleCounts(ProgressBucket bucket, size_t& outVAWidth, size_t& outPOCShare) const {
        const int idx = static_cast<int>(bucket);
        outVAWidth = vaWidthTicks[idx].size();
        outPOCShare = pocShare[idx].size();
    }

    // --- Check if volume sufficiency baseline is ready for a bucket ---
    bool IsVolumeSufficiencyReady(ProgressBucket bucket, size_t minSamples = 5) const {
        const int idx = static_cast<int>(bucket);
        return volumeSoFar[idx].size() >= minSamples;
    }

    // --- Check if POC share baseline is ready for a bucket ---
    bool IsPocShareBaselineReady(ProgressBucket bucket, size_t minSamples = ProfileBaselineMinSamples::POC_DOMINANCE) const {
        const int idx = static_cast<int>(bucket);
        return pocShare[idx].size() >= minSamples;
    }

    // --- Get percentile rank for cumulative volume at a bucket ---
    // Returns the percentile of current volume vs historical volume-so-far at same bucket
    // Used for progress-conditioned volume sufficiency check
    // Returns -1.0 if baseline not ready
    double GetVolumeSufficiencyPercentile(ProgressBucket bucket, double currentVolume) const {
        const int idx = static_cast<int>(bucket);
        if (volumeSoFar[idx].size() < 5) return -1.0;
        return volumeSoFar[idx].percentileRank(currentVolume);
    }
};

// ============================================================================
// BASELINE-AWARE PROFILE MATURITY CHECK
// ============================================================================
// NO-FALLBACK POLICY: Uses progress-conditioned volume sufficiency percentile.
// When baseline not ready: volumeSufficiencyValid = false, volume gate NOT applied.
// We do NOT inject absolute volume thresholds - structural gates are sufficient.

inline ProfileMaturityResult CheckProfileMaturityWithBaseline(
    int priceLevels,
    double totalVolume,
    int sessionBars,
    int sessionMinutes,
    ProgressBucket currentBucket,
    const HistoricalProfileBaseline* baseline)
{
    ProfileMaturityResult result;
    result.priceLevels = priceLevels;
    result.totalVolume = totalVolume;
    result.sessionBars = sessionBars;
    result.sessionMinutes = sessionMinutes;

    // Structural gates (always applied)
    result.hasMinLevels = (priceLevels >= ProfileMaturity::MIN_PRICE_LEVELS);
    result.hasMinBars = (sessionBars >= ProfileMaturity::MIN_BARS);
    result.hasMinMinutes = (sessionMinutes >= ProfileMaturity::MIN_MINUTES);

    // VOLUME SUFFICIENCY: Only applied when baseline available
    if (baseline != nullptr && baseline->IsVolumeSufficiencyReady(currentBucket)) {
        // Baseline ready: apply progress-conditioned percentile check
        result.volumeSufficiencyValid = true;
        result.volumePercentile = baseline->GetVolumeSufficiencyPercentile(currentBucket, totalVolume);
        result.hasMinVolume = (result.volumePercentile >= ProfileMaturity::VOLUME_SUFFICIENCY_PERCENTILE);
    } else {
        // Baseline not ready: volume gate NOT applied (no fallback)
        result.volumeSufficiencyValid = false;
        result.hasMinVolume = false;  // Meaningless without baseline
        result.volumePercentile = -1.0;
    }

    // Maturity decision:
    // - Structural gates always required
    // - Volume gate only required when volumeSufficiencyValid
    if (result.volumeSufficiencyValid) {
        result.isMature = result.hasMinLevels && result.hasMinBars &&
                          result.hasMinMinutes && result.hasMinVolume;
    } else {
        // NO-FALLBACK: Use only structural gates when volume baseline unavailable
        result.isMature = result.hasMinLevels && result.hasMinBars && result.hasMinMinutes;
    }

    // Set reason for first failed gate (for logging)
    if (!result.hasMinLevels) {
        result.gateFailedReason = "insufficient price levels";
    } else if (!result.hasMinBars) {
        result.gateFailedReason = "insufficient bars";
    } else if (!result.hasMinMinutes) {
        result.gateFailedReason = "insufficient minutes";
    } else if (result.volumeSufficiencyValid && !result.hasMinVolume) {
        result.gateFailedReason = "volume below historical percentile";
    }
    // Note: if volumeSufficiencyValid=false, volume is not a gate failure reason

    return result;
}

// ============================================================================
// VBP LEVEL CONTEXT (For MiniVP Integration)
// ============================================================================

struct VbPLevelContext
{
    bool valid = false;

    // Location relative to Value Area
    bool insideValueArea = false;
    bool atPOC = false;
    bool aboveVAH = false;
    bool belowVAL = false;

    // SSOT classification (orthogonal outputs)
    VolumeNodeClassification classification;

    // Legacy accessors (delegate to classification for backward compatibility)
    bool isHVN = false;           // High Volume Node (set from classification.IsHVN())
    bool isLVN = false;           // Low Volume Node (set from classification.IsLVN())
    double volumeAtPrice = 0.0;   // Raw volume at this level
    double volumePercentile = 0.0; // 0.0-1.0, where 1.0 = POC level

    // Nearby structure
    double nearestHVN = 0.0;      // Closest HVN price
    double nearestLVN = 0.0;      // Closest LVN price
    double distToHVNTicks = 0.0;  // Distance to nearest HVN
    double distToLVNTicks = 0.0;  // Distance to nearest LVN

    // Sync legacy fields from classification (call after setting classification)
    void SyncFromClassification() {
        isHVN = classification.IsHVN();
        isLVN = classification.IsLVN();
    }
};

// ============================================================================
// SSOT: VALUE AREA EXPANSION (DRY helper)
// Single source of truth for 70% value area calculation.
// Called by both MicroAuction and SessionVolumeProfile.
// ============================================================================

/**
 * Compute Value Area from sorted volume vector using 70% expansion from POC.
 *
 * @param sorted_vols  Vector of (tick, volume) pairs sorted by tick (ascending)
 * @param pocIdx       Index of POC in the sorted vector
 * @param tickSize     Tick size for price conversion
 * @param targetRatio  Target volume ratio (default 0.70 for 70% VA)
 * @param outVAL       [out] Computed Value Area Low
 * @param outVAH       [out] Computed Value Area High
 * @return true if computation succeeded and invariants hold
 *
 * INVARIANT (debug): outVAL <= POC price <= outVAH
 * INVARIANT (debug): Captured volume ratio >= targetRatio (within tolerance)
 */
inline bool ComputeValueAreaFromSortedVector(
    const std::vector<std::pair<int, double>>& sorted_vols,
    int pocIdx,
    double tickSize,
    double targetRatio,
    double& outVAL,
    double& outVAH)
{
    if (sorted_vols.empty() || pocIdx < 0 ||
        pocIdx >= static_cast<int>(sorted_vols.size()) || tickSize <= 0.0)
    {
        return false;
    }

    // Calculate total volume
    double totalVol = 0.0;
    for (const auto& kv : sorted_vols) {
        totalVol += kv.second;
    }

    if (totalVol <= 0.0) {
        return false;
    }

    // 70% Value Area expansion from POC
    const double targetVol = totalVol * targetRatio;
    double vaVol = sorted_vols[pocIdx].second;
    int vaLowIdx = pocIdx;
    int vaHighIdx = pocIdx;
    int lowPtr = pocIdx - 1;
    int highPtr = pocIdx + 1;

    while (vaVol < targetVol && (lowPtr >= 0 || highPtr < static_cast<int>(sorted_vols.size())))
    {
        const double lowVol = (lowPtr >= 0) ? sorted_vols[lowPtr].second : 0.0;
        const double highVol = (highPtr < static_cast<int>(sorted_vols.size())) ? sorted_vols[highPtr].second : 0.0;

        if (lowVol >= highVol && lowPtr >= 0)
        {
            vaVol += lowVol;
            vaLowIdx = lowPtr;
            lowPtr--;
        }
        else if (highPtr < static_cast<int>(sorted_vols.size()))
        {
            vaVol += highVol;
            vaHighIdx = highPtr;
            highPtr++;
        }
        else
        {
            break;
        }
    }

    outVAL = sorted_vols[vaLowIdx].first * tickSize;
    outVAH = sorted_vols[vaHighIdx].first * tickSize;

#ifdef _DEBUG
    // ========================================================================
    // INVARIANT CHECKS WITH FINGERPRINT
    // Fingerprint allows tracing divergent inputs at different call sites
    // ========================================================================

    // Compute input fingerprint: count + sumVol + sumPriceVol
    double fingerprint_sumVol = 0.0;
    double fingerprint_sumPriceVol = 0.0;
    for (const auto& kv : sorted_vols) {
        fingerprint_sumVol += kv.second;
        fingerprint_sumPriceVol += static_cast<double>(kv.first) * kv.second;
    }
    const size_t fingerprint_count = sorted_vols.size();

    // Store fingerprint in static for debugger inspection on assert
    // (volatile to prevent optimization)
    static volatile size_t dbg_fp_count = 0;
    static volatile double dbg_fp_sumVol = 0.0;
    static volatile double dbg_fp_sumPriceVol = 0.0;
    static volatile double dbg_fp_val = 0.0;
    static volatile double dbg_fp_vah = 0.0;
    static volatile double dbg_fp_poc = 0.0;

    dbg_fp_count = fingerprint_count;
    dbg_fp_sumVol = fingerprint_sumVol;
    dbg_fp_sumPriceVol = fingerprint_sumPriceVol;
    dbg_fp_val = outVAL;
    dbg_fp_vah = outVAH;

    // INVARIANT: VAL <= POC <= VAH
    const double pocPrice = sorted_vols[pocIdx].first * tickSize;
    dbg_fp_poc = pocPrice;

    if (outVAL > pocPrice || outVAH < pocPrice) {
        // Fingerprint available in dbg_fp_* variables for debugger inspection
        assert(false && "Value Area invariant violated: VAL <= POC <= VAH (check dbg_fp_* vars)");
    }

    // INVARIANT: Captured volume should be close to target (within 5% tolerance for edge cases)
    const double capturedRatio = vaVol / totalVol;
    const double tolerance = 0.05;  // Allow 5% tolerance for small profiles
    if (capturedRatio < (targetRatio - tolerance) && sorted_vols.size() > 3) {
        // Fingerprint available in dbg_fp_* variables for debugger inspection
        assert(false && "Value Area invariant violated: insufficient volume captured (check dbg_fp_* vars)");
    }
#endif

    return true;
}

} // namespace AMT

// ============================================================================
// DUAL-SESSION PEAKS/VALLEYS (RTH + GLOBEX)
// ============================================================================

// Profile classification based on fixed time windows
enum class ProfileSessionType
{
    UNKNOWN = 0,
    RTH,      // 09:30:00 - 16:14:59
    GLOBEX    // 16:15:00 - 09:29:59 (spans midnight)
};

inline const char* to_string(ProfileSessionType t)
{
    switch (t) {
        case ProfileSessionType::RTH:     return "RTH";
        case ProfileSessionType::GLOBEX:  return "GLOBEX";
        default:                          return "UNKNOWN";
    }
}

// Peaks/Valleys for a single profile
struct ProfilePeaksValleys
{
    bool valid = false;
    int profileIndex = -999;           // The profile index used (-1, -2, etc.)
    ProfileSessionType sessionType = ProfileSessionType::UNKNOWN;
    SCDateTime startTime;
    SCDateTime endTime;
    std::vector<double> hvn;           // High Volume Node prices
    std::vector<double> lvn;           // Low Volume Node prices

    void Clear()
    {
        valid = false;
        profileIndex = -999;
        sessionType = ProfileSessionType::UNKNOWN;
        startTime = SCDateTime(0);
        endTime = SCDateTime(0);
        hvn.clear();
        lvn.clear();
    }
};

// Dual-session storage: both RTH and GLOBEX peaks/valleys
struct DualSessionPeaksValleys
{
    ProfilePeaksValleys rth;
    ProfilePeaksValleys globex;

    // Tracking for change detection (avoid log spam)
    int lastLoggedRthIndex = -999;
    int lastLoggedGlobexIndex = -999;
    int lastLoggedRthHvnCount = -1;
    int lastLoggedGlobexHvnCount = -1;

    void Clear()
    {
        rth.Clear();
        globex.Clear();
    }

    bool HasChanged() const
    {
        return rth.profileIndex != lastLoggedRthIndex ||
               globex.profileIndex != lastLoggedGlobexIndex ||
               static_cast<int>(rth.hvn.size()) != lastLoggedRthHvnCount ||
               static_cast<int>(globex.hvn.size()) != lastLoggedGlobexHvnCount;
    }

    void MarkLogged()
    {
        lastLoggedRthIndex = rth.profileIndex;
        lastLoggedGlobexIndex = globex.profileIndex;
        lastLoggedRthHvnCount = static_cast<int>(rth.hvn.size());
        lastLoggedGlobexHvnCount = static_cast<int>(globex.hvn.size());
    }
};

// ============================================================================
// OPENING RANGE TRACKER (IB for RTH, Session Open Range for Globex)
// ============================================================================
// Tracks the opening range for shape confirmation gates.
// - RTH: Initial Balance (first 60 minutes, 9:30-10:30)
// - Globex: Opening Range (first 90 minutes - needs more time due to lower volume)
// ============================================================================

struct OpeningRangeTracker {
    // === SESSION CONFIGURATION ===
    bool isRTH = true;                    // RTH uses IB (60 min), Globex uses 90 min
    int freezeAfterMinutes = 60;          // 60 for RTH IB, 90 for Globex
    int failedAuctionWindow = 30;         // 30 min for RTH, 60 min for Globex

    // === OPENING RANGE STATE ===
    double rangeHigh = 0.0;               // High during opening range period
    double rangeLow = 0.0;                // Low during opening range period
    bool rangeFrozen = false;             // True once opening range period complete
    int frozenBar = -1;                   // Bar at which range was frozen
    int frozenMinutes = 0;                // Minutes when frozen

    // === RANGE EXTENSION TRACKING ===
    double extensionHigh = 0.0;           // Highest price above opening range
    double extensionLow = 0.0;            // Lowest price below opening range
    bool hasExtendedAbove = false;        // Price exceeded rangeHigh
    bool hasExtendedBelow = false;        // Price went below rangeLow
    int lastBreachAboveBar = -1;          // Bar of last breach above
    int lastBreachBelowBar = -1;          // Bar of last breach below
    int lastBreachAboveMinutes = -1;      // Minutes of last breach above
    int lastBreachBelowMinutes = -1;      // Minutes of last breach below

    // === FAILED AUCTION DETECTION ===
    bool failedAuctionUp = false;         // Breached above then returned within window
    bool failedAuctionDown = false;       // Breached below then returned within window

    // Reset for new session with session-specific parameters
    void Reset(bool isRthSession) {
        isRTH = isRthSession;
        freezeAfterMinutes = isRthSession ? 60 : 90;    // RTH=60 (IB), GBX=90 (needs more time due to lower volume)
        failedAuctionWindow = isRthSession ? 30 : 60;   // RTH=30, GBX=60 (slower market)

        rangeHigh = 0.0;
        rangeLow = 0.0;
        rangeFrozen = false;
        frozenBar = -1;
        frozenMinutes = 0;

        extensionHigh = 0.0;
        extensionLow = 0.0;
        hasExtendedAbove = false;
        hasExtendedBelow = false;
        lastBreachAboveBar = -1;
        lastBreachBelowBar = -1;
        lastBreachAboveMinutes = -1;
        lastBreachBelowMinutes = -1;

        failedAuctionUp = false;
        failedAuctionDown = false;
    }

    // Update opening range with each bar's high/low
    void Update(double barHigh, double barLow, double barClose, int sessionMinutes, int bar) {
        // Phase 1: Building opening range (before freeze)
        if (!rangeFrozen) {
            if (rangeHigh == 0.0 || barHigh > rangeHigh) rangeHigh = barHigh;
            if (rangeLow == 0.0 || barLow < rangeLow) rangeLow = barLow;

            // Check if time to freeze
            if (sessionMinutes >= freezeAfterMinutes) {
                rangeFrozen = true;
                frozenBar = bar;
                frozenMinutes = sessionMinutes;
            }
        }
        // Phase 2: Track range extension after freeze
        else {
            // Track new highs above opening range
            if (barHigh > rangeHigh) {
                if (!hasExtendedAbove) {
                    hasExtendedAbove = true;
                    lastBreachAboveBar = bar;
                    lastBreachAboveMinutes = sessionMinutes;
                }
                if (barHigh > extensionHigh) {
                    extensionHigh = barHigh;
                }
            }

            // Track new lows below opening range
            if (barLow < rangeLow) {
                if (!hasExtendedBelow) {
                    hasExtendedBelow = true;
                    lastBreachBelowBar = bar;
                    lastBreachBelowMinutes = sessionMinutes;
                }
                if (barLow < extensionLow || extensionLow == 0.0) {
                    extensionLow = barLow;
                }
            }
        }
    }

    // Check for failed auction (price returned to opening range after breach)
    void CheckFailedAuction(double barClose, int sessionMinutes, int bar) {
        if (!rangeFrozen) return;

        // Check failed auction UP: breached above, now back inside range
        if (hasExtendedAbove && !failedAuctionUp && barClose <= rangeHigh) {
            const int minutesSinceBreach = sessionMinutes - lastBreachAboveMinutes;
            if (minutesSinceBreach > 0 && minutesSinceBreach <= failedAuctionWindow) {
                failedAuctionUp = true;
            }
        }

        // Check failed auction DOWN: breached below, now back inside range
        if (hasExtendedBelow && !failedAuctionDown && barClose >= rangeLow) {
            const int minutesSinceBreach = sessionMinutes - lastBreachBelowMinutes;
            if (minutesSinceBreach > 0 && minutesSinceBreach <= failedAuctionWindow) {
                failedAuctionDown = true;
            }
        }
    }

    // Range extension magnitude in ticks
    double GetExtensionAboveTicks(double tickSize) const {
        if (!hasExtendedAbove || tickSize <= 0.0) return 0.0;
        return (extensionHigh - rangeHigh) / tickSize;
    }

    double GetExtensionBelowTicks(double tickSize) const {
        if (!hasExtendedBelow || tickSize <= 0.0) return 0.0;
        return (rangeLow - extensionLow) / tickSize;
    }
};

// ============================================================================
// SHAPE BREAK TYPE (triggers for shape re-evaluation)
// ============================================================================
enum class ShapeBreakType : int {
    NONE = 0,
    POC_DRIFT,           // POC migrated significantly from confirmed position
    VALUE_ACCEPTANCE,    // Sustained acceptance outside prior value area
    DD_FORMATION         // Second HVN cluster formed (double distribution)
};

inline const char* ShapeBreakTypeToString(ShapeBreakType bt) {
    switch (bt) {
        case ShapeBreakType::NONE: return "NONE";
        case ShapeBreakType::POC_DRIFT: return "POC_DRIFT";
        case ShapeBreakType::VALUE_ACCEPTANCE: return "VALUE_ACCEPT";
        case ShapeBreakType::DD_FORMATION: return "DD_FORM";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// CONFIRMED SHAPE STATE (tracks shape after 6-gate confirmation)
// ============================================================================
// Once a shape passes all 6 gates, it becomes "confirmed" rather than "frozen".
// Confirmed shapes have:
// - Confidence decay as structure diverges
// - Break detection for re-evaluation triggers
// - Transition cooldown to prevent flip-flopping
// ============================================================================

struct ConfirmedShapeState {
    // === CONFIRMATION STATE ===
    bool isConfirmed = false;
    AMT::ProfileShape confirmedShape = AMT::ProfileShape::UNDEFINED;
    int confirmedBar = -1;
    int confirmedSessionMinutes = 0;
    float confirmedConfidence = 0.0f;

    // === SNAPSHOT AT CONFIRMATION (for drift detection) ===
    int confirmedPOCTicks = 0;
    int confirmedVAHTicks = 0;
    int confirmedVALTicks = 0;
    int confirmedHVNCount = 1;
    double confirmedUpperThirdRatio = 1.0;

    // === CONFIDENCE DECAY ===
    float structuralMatchScore = 1.0f;   // 1.0 = perfect match, decays toward 0
    float effectiveConfidence = 0.0f;    // confirmedConfidence * structuralMatchScore

    // === TRANSITION TRACKING ===
    int lastTransitionBar = -1;
    int transitionCount = 0;
    static constexpr int TRANSITION_COOLDOWN_BARS = 30;  // ~30 min on 1-min bars

    // === MATCH SCORE COMPONENTS (for enhanced logging) ===
    // Captured during UpdateStructuralMatch for component-level visibility
    struct MatchScoreComponents {
        int pocDriftTicks = 0;
        float pocPenalty = 0.0f;
        float vaWidthChangePercent = 0.0f;
        float vaPenalty = 0.0f;
        int hvnCountChange = 0;
        float hvnPenalty = 0.0f;
        double ratioChange = 0.0;
        float ratioPenalty = 0.0f;
        float totalScore = 1.0f;

        // Format as log string: "POC=+6t(-0.18) VA_W=+12%(-0.02) HVN=1→2(-0.30) RATIO=0.15(-0.02)"
        std::string FormatLogString() const {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "POC=%+dt(-%.2f) VA_W=%+.0f%%(-%.2f) HVN=%+d(-%.2f) RATIO=%.2f(-%.2f) | SCORE=%.2f",
                pocDriftTicks, pocPenalty,
                vaWidthChangePercent * 100.0f, vaPenalty,
                hvnCountChange, hvnPenalty,
                ratioChange, ratioPenalty,
                totalScore);
            return std::string(buf);
        }
    };
    MatchScoreComponents lastMatchComponents_;

    bool IsConfirmed() const { return isConfirmed && confirmedShape != AMT::ProfileShape::UNDEFINED; }

    bool IsInCooldown(int currentBar) const {
        if (lastTransitionBar < 0) return false;
        return (currentBar - lastTransitionBar) < TRANSITION_COOLDOWN_BARS;
    }

    // Confirm a shape (called when all 6 gates pass)
    void Confirm(AMT::ProfileShape shape, int bar, int sessionMinutes, float confidence,
                 int pocTicks, int vahTicks, int valTicks, int hvnCount, double upperThirdRatio) {
        // If this is a transition (was already confirmed), track it
        if (isConfirmed && confirmedShape != AMT::ProfileShape::UNDEFINED) {
            lastTransitionBar = bar;
            transitionCount++;
        }

        isConfirmed = true;
        confirmedShape = shape;
        confirmedBar = bar;
        confirmedSessionMinutes = sessionMinutes;
        confirmedConfidence = confidence;

        // Snapshot current structure
        confirmedPOCTicks = pocTicks;
        confirmedVAHTicks = vahTicks;
        confirmedVALTicks = valTicks;
        confirmedHVNCount = hvnCount;
        confirmedUpperThirdRatio = upperThirdRatio;

        // Reset decay
        structuralMatchScore = 1.0f;
        effectiveConfidence = confidence;
    }

    // Unconfirm (called when structural break detected)
    void Unconfirm(int bar) {
        isConfirmed = false;
        // Keep confirmed* fields for logging transition
        // Reset decay tracking
        structuralMatchScore = 1.0f;
        effectiveConfidence = 0.0f;
    }

    // Update structural match score (call each bar while confirmed)
    // Also captures component values in lastMatchComponents_ for logging
    void UpdateStructuralMatch(int currentPOCTicks, int currentVAHTicks, int currentVALTicks,
                               int currentHVNCount, double currentUpperThirdRatio) {
        if (!isConfirmed) {
            structuralMatchScore = 1.0f;
            effectiveConfidence = 0.0f;
            lastMatchComponents_ = MatchScoreComponents();  // Reset components
            return;
        }

        float score = 1.0f;
        MatchScoreComponents& mc = lastMatchComponents_;

        // POC drift penalty (0-0.30): 10 ticks drift = max penalty
        mc.pocDriftTicks = currentPOCTicks - confirmedPOCTicks;  // Signed for direction
        const int pocDriftAbs = std::abs(mc.pocDriftTicks);
        mc.pocPenalty = (std::min)(0.30f, pocDriftAbs * 0.03f);
        score -= mc.pocPenalty;

        // VA width change penalty (0-0.20): 50% width change = max penalty
        const int confirmedVAWidth = confirmedVAHTicks - confirmedVALTicks;
        const int currentVAWidth = currentVAHTicks - currentVALTicks;
        mc.vaWidthChangePercent = 0.0f;
        mc.vaPenalty = 0.0f;
        if (confirmedVAWidth > 0) {
            mc.vaWidthChangePercent = static_cast<float>(currentVAWidth - confirmedVAWidth) /
                                      static_cast<float>(confirmedVAWidth);
            mc.vaPenalty = (std::min)(0.20f, std::abs(mc.vaWidthChangePercent) * 0.20f);
            score -= mc.vaPenalty;
        }

        // HVN count change penalty (0-0.30): any cluster count change = max penalty
        mc.hvnCountChange = currentHVNCount - confirmedHVNCount;
        mc.hvnPenalty = (mc.hvnCountChange != 0) ? 0.30f : 0.0f;
        score -= mc.hvnPenalty;

        // Volume distribution change penalty (0-0.20): ratio shift
        mc.ratioChange = currentUpperThirdRatio - confirmedUpperThirdRatio;
        mc.ratioPenalty = (std::min)(0.20f, static_cast<float>(std::abs(mc.ratioChange) * 0.10));
        score -= mc.ratioPenalty;

        mc.totalScore = (std::max)(0.0f, score);
        structuralMatchScore = mc.totalScore;
        effectiveConfidence = confirmedConfidence * structuralMatchScore;
    }

    // Get log string for match score components (for enhanced logging)
    std::string GetMatchScoreLogString() const {
        return lastMatchComponents_.FormatLogString();
    }

    void Reset() {
        isConfirmed = false;
        confirmedShape = AMT::ProfileShape::UNDEFINED;
        confirmedBar = -1;
        confirmedSessionMinutes = 0;
        confirmedConfidence = 0.0f;
        confirmedPOCTicks = 0;
        confirmedVAHTicks = 0;
        confirmedVALTicks = 0;
        confirmedHVNCount = 1;
        confirmedUpperThirdRatio = 1.0;
        structuralMatchScore = 1.0f;
        effectiveConfidence = 0.0f;
        lastTransitionBar = -1;
        transitionCount = 0;
    }
};

// ============================================================================
// ADAPTIVE BREAK THRESHOLDS (volatility-scaled)
// ============================================================================
// Break detection thresholds that scale with VolatilityRegime.
// Quiet markets: smaller moves are meaningful, need more confirmation.
// Volatile markets: larger moves needed, faster confirmation.
// SSOT: This is the single definition - ShapeBreakDetector uses this.
// ============================================================================

struct AdaptiveBreakThresholds {
    int pocDriftTicks = 8;              // Ticks of POC drift to trigger break
    int pocDriftPersistenceBars = 5;    // Bars POC must hold at new level
    int valueAcceptanceBars = 10;       // Bars of acceptance outside VA
    int ddFormationBars = 5;            // Bars HVN count must hold
    int breakConfirmationBars = 5;      // Bars to confirm any break

    // Factory method from VolatilityRegime
    static AdaptiveBreakThresholds FromVolatilityRegime(AMT::VolatilityRegime regime) {
        AdaptiveBreakThresholds t;

        switch (regime) {
            case AMT::VolatilityRegime::COMPRESSION:
                // Tight ranges - smaller moves meaningful, more confirmation needed
                t.pocDriftTicks = 4;           // 1 point ES
                t.pocDriftPersistenceBars = 7;
                t.valueAcceptanceBars = 15;
                t.ddFormationBars = 7;
                t.breakConfirmationBars = 7;
                break;

            case AMT::VolatilityRegime::NORMAL:
                // Standard thresholds
                t.pocDriftTicks = 8;           // 2 points ES
                t.pocDriftPersistenceBars = 5;
                t.valueAcceptanceBars = 10;
                t.ddFormationBars = 5;
                t.breakConfirmationBars = 5;
                break;

            case AMT::VolatilityRegime::EXPANSION:
                // Wide ranges - bigger moves needed, faster confirmation
                t.pocDriftTicks = 12;          // 3 points ES
                t.pocDriftPersistenceBars = 4;
                t.valueAcceptanceBars = 8;
                t.ddFormationBars = 4;
                t.breakConfirmationBars = 4;
                break;

            case AMT::VolatilityRegime::EVENT:
                // Extreme volatility - very large thresholds, fast confirmation
                t.pocDriftTicks = 20;          // 5 points ES
                t.pocDriftPersistenceBars = 3;
                t.valueAcceptanceBars = 5;
                t.ddFormationBars = 3;
                t.breakConfirmationBars = 3;
                break;

            default:
                // UNKNOWN - use normal (default values)
                break;
        }

        return t;
    }
};

// ============================================================================
// SHAPE BREAK DETECTOR (detects structural breaks that trigger re-evaluation)
// ============================================================================
// Monitors for persistent structural changes that should trigger shape
// re-evaluation. Uses hysteresis to prevent false triggers.
// ============================================================================

struct ShapeBreakDetector {
    // === ADAPTIVE THRESHOLDS (set via SetVolatilityRegime) ===
    int pocDriftThresholdTicks = 8;       // ES: 2 points (NORMAL)
    int pocDriftPersistenceBars = 5;      // Must hold for 5 bars
    int valueAcceptanceBars = 10;         // 10 bars accepted outside VA
    int ddFormationPersistenceBars = 5;   // HVN count must hold 5 bars
    int breakConfirmationRequired = 5;    // Bars to confirm any break

    // Current volatility regime (for logging)
    AMT::VolatilityRegime currentRegime = AMT::VolatilityRegime::NORMAL;

    // === POC DRIFT TRACKING ===
    int pocDriftTicks = 0;           // Current drift from confirmed POC
    int pocDriftBars = 0;            // Consecutive bars at drifted position

    // === VALUE ACCEPTANCE OUTSIDE TRACKING ===
    int barsAcceptedOutsideValue = 0;  // Consecutive bars with acceptance outside VA
    bool priceCurrentlyOutsideVA = false;

    // === DD FORMATION TRACKING ===
    int currentHVNCount = 1;
    int hvnCountChangeBars = 0;      // Bars since HVN count changed

    // === BREAK STATE ===
    ShapeBreakType candidateBreak = ShapeBreakType::NONE;
    int breakCandidateBar = -1;
    int breakConfirmationBars = 0;

    // Set thresholds based on volatility regime (uses AdaptiveBreakThresholds SSOT)
    void SetVolatilityRegime(AMT::VolatilityRegime regime) {
        currentRegime = regime;
        const auto t = AdaptiveBreakThresholds::FromVolatilityRegime(regime);
        pocDriftThresholdTicks = t.pocDriftTicks;
        pocDriftPersistenceBars = t.pocDriftPersistenceBars;
        valueAcceptanceBars = t.valueAcceptanceBars;
        ddFormationPersistenceBars = t.ddFormationBars;
        breakConfirmationRequired = t.breakConfirmationBars;
    }

    // Update break detection (call each bar while shape is confirmed)
    void Update(int currentBar, int currentPOCTicks, int confirmedPOCTicks,
                bool isAccepted, bool priceOutsideVA,
                int hvnClusterCount, int confirmedHVNCount) {

        // === POC DRIFT DETECTION ===
        const int drift = std::abs(currentPOCTicks - confirmedPOCTicks);
        if (drift >= pocDriftThresholdTicks) {
            pocDriftTicks = drift;
            pocDriftBars++;
        } else {
            pocDriftBars = 0;
            pocDriftTicks = 0;
        }

        // === VALUE ACCEPTANCE OUTSIDE ===
        priceCurrentlyOutsideVA = priceOutsideVA;
        if (isAccepted && priceOutsideVA) {
            barsAcceptedOutsideValue++;
        } else {
            barsAcceptedOutsideValue = 0;
        }

        // === DD FORMATION ===
        currentHVNCount = hvnClusterCount;
        if (hvnClusterCount > confirmedHVNCount) {
            hvnCountChangeBars++;
        } else {
            hvnCountChangeBars = 0;
        }

        // === DETERMINE CANDIDATE BREAK ===
        ShapeBreakType newCandidate = ShapeBreakType::NONE;

        // Priority: DD_FORMATION > VALUE_ACCEPTANCE > POC_DRIFT
        if (hvnCountChangeBars >= ddFormationPersistenceBars) {
            newCandidate = ShapeBreakType::DD_FORMATION;
        } else if (barsAcceptedOutsideValue >= valueAcceptanceBars) {
            newCandidate = ShapeBreakType::VALUE_ACCEPTANCE;
        } else if (pocDriftBars >= pocDriftPersistenceBars) {
            newCandidate = ShapeBreakType::POC_DRIFT;
        }

        // === BREAK CONFIRMATION HYSTERESIS ===
        if (newCandidate != ShapeBreakType::NONE) {
            if (newCandidate == candidateBreak) {
                breakConfirmationBars++;
            } else {
                // New break type detected - reset confirmation
                candidateBreak = newCandidate;
                breakCandidateBar = currentBar;
                breakConfirmationBars = 1;
            }
        } else {
            // No break candidate - reset
            candidateBreak = ShapeBreakType::NONE;
            breakCandidateBar = -1;
            breakConfirmationBars = 0;
        }
    }

    bool IsBreakConfirmed() const {
        return candidateBreak != ShapeBreakType::NONE &&
               breakConfirmationBars >= breakConfirmationRequired;
    }

    ShapeBreakType GetBreakType() const {
        return IsBreakConfirmed() ? candidateBreak : ShapeBreakType::NONE;
    }

    // Format break detection state for logging
    // Output: "BREAK: POC_DRIFT | SHAPE → SHAPE | TRANS#N | CONF: 0.72 → 0.65"
    std::string FormatBreakLogString(AMT::ProfileShape confirmedShape, AMT::ProfileShape candidateShape,
                                      int transitionCount, float priorConf, float newConf) const {
        static const char* breakTypeNames[] = { "NONE", "POC_DRIFT", "VALUE_ACCEPTANCE", "DD_FORMATION" };
        const char* breakTypeName = breakTypeNames[static_cast<int>(candidateBreak)];

        char buf[256];
        snprintf(buf, sizeof(buf),
            "BREAK: %s | %s -> %s | TRANS#%d | CONF: %.2f -> %.2f",
            breakTypeName,
            AMT::ProfileShapeToString(confirmedShape),
            AMT::ProfileShapeToString(candidateShape),
            transitionCount,
            priorConf, newConf);
        return std::string(buf);
    }

    // Format break detector state for debugging
    // Output: "BREAK_DET: POC=+8t@5b VAL=OK@12b DD=1→2@3b | CAND=POC_DRIFT CONF=3/5 VOL=NORMAL"
    std::string FormatStateLogString() const {
        static const char* breakTypeNames[] = { "NONE", "POC_DRIFT", "VALUE_ACCEPT", "DD_FORM" };
        static const char* volRegimeNames[] = { "UNKNOWN", "COMPRESS", "NORMAL", "EXPAND", "EVENT" };

        const char* breakTypeName = breakTypeNames[static_cast<int>(candidateBreak)];
        const char* volRegimeName = volRegimeNames[static_cast<int>(currentRegime)];

        char buf[256];
        snprintf(buf, sizeof(buf),
            "BREAK_DET: POC=%+dt@%db VAL=%s@%db DD=%d->%d@%db | CAND=%s CONF=%d/%d VOL=%s",
            pocDriftTicks, pocDriftBars,
            priceCurrentlyOutsideVA ? "OUT" : "IN", barsAcceptedOutsideValue,
            1, currentHVNCount, hvnCountChangeBars,  // 1 = confirmed HVN count (assume single)
            breakTypeName, breakConfirmationBars, breakConfirmationRequired,
            volRegimeName);
        return std::string(buf);
    }

    void Reset() {
        pocDriftTicks = 0;
        pocDriftBars = 0;
        barsAcceptedOutsideValue = 0;
        priceCurrentlyOutsideVA = false;
        currentHVNCount = 1;
        hvnCountChangeBars = 0;
        candidateBreak = ShapeBreakType::NONE;
        breakCandidateBar = -1;
        breakConfirmationBars = 0;
    }
};

// ============================================================================
// SHAPE BEHAVIOR TRACKER (forward validation of shape predictions)
// ============================================================================
// Tracks price behavior AFTER a shape is confirmed to validate whether
// the shape prediction was accurate. Computes validation scores at
// multiple time windows (15/30/60 bars).
// ============================================================================

struct ShapeBehaviorTracker {
    // === VALIDATION WINDOWS ===
    static constexpr int SHORT_WINDOW = 15;   // ~15 min on 1-min bars
    static constexpr int MEDIUM_WINDOW = 30;  // ~30 min
    static constexpr int LONG_WINDOW = 60;    // ~60 min

    // === SNAPSHOT AT CONFIRMATION ===
    AMT::ProfileShape confirmedShape = AMT::ProfileShape::UNDEFINED;
    int confirmedBar = -1;
    int confirmedPOCTicks = 0;
    int confirmedVAHTicks = 0;
    int confirmedVALTicks = 0;
    double confirmedPrice = 0.0;
    double confirmedIBHigh = 0.0;
    double confirmedIBLow = 0.0;

    // === FORWARD TRACKING (updated each bar after confirmation) ===
    int barsTracked = 0;
    int barsAbovePOC = 0;
    int barsBelowPOC = 0;
    int barsInVA = 0;
    int barsAboveVA = 0;
    int barsBelowVA = 0;
    double maxPriceReached = 0.0;
    double minPriceReached = 0.0;
    bool didExtendAboveIB = false;
    bool didExtendBelowIB = false;

    // === VALIDATION SCORES (computed at window boundaries) ===
    float shortWindowScore = -1.0f;   // -1 = not yet computed
    float mediumWindowScore = -1.0f;
    float longWindowScore = -1.0f;

    bool IsActive() const {
        return confirmedShape != AMT::ProfileShape::UNDEFINED && confirmedBar >= 0;
    }

    // Start tracking for a newly confirmed shape
    void StartTracking(AMT::ProfileShape shape, int bar, int pocTicks, int vahTicks, int valTicks,
                       double price, double ibHigh, double ibLow) {
        confirmedShape = shape;
        confirmedBar = bar;
        confirmedPOCTicks = pocTicks;
        confirmedVAHTicks = vahTicks;
        confirmedVALTicks = valTicks;
        confirmedPrice = price;
        confirmedIBHigh = ibHigh;
        confirmedIBLow = ibLow;

        barsTracked = 0;
        barsAbovePOC = 0;
        barsBelowPOC = 0;
        barsInVA = 0;
        barsAboveVA = 0;
        barsBelowVA = 0;
        maxPriceReached = price;
        minPriceReached = price;
        didExtendAboveIB = false;
        didExtendBelowIB = false;

        shortWindowScore = -1.0f;
        mediumWindowScore = -1.0f;
        longWindowScore = -1.0f;
    }

    // Update tracking with current bar data
    void Update(double currentPrice, int currentPOCTicks, int currentVAHTicks, int currentVALTicks,
                double tickSize) {
        if (!IsActive()) return;

        barsTracked++;

        // Price position tracking
        const int priceTicks = static_cast<int>(std::round(currentPrice / tickSize));

        if (priceTicks > currentPOCTicks) barsAbovePOC++;
        else if (priceTicks < currentPOCTicks) barsBelowPOC++;

        if (priceTicks >= currentVALTicks && priceTicks <= currentVAHTicks) {
            barsInVA++;
        } else if (priceTicks > currentVAHTicks) {
            barsAboveVA++;
        } else {
            barsBelowVA++;
        }

        // Extreme tracking
        if (currentPrice > maxPriceReached) maxPriceReached = currentPrice;
        if (currentPrice < minPriceReached) minPriceReached = currentPrice;

        // IB extension tracking
        if (currentPrice > confirmedIBHigh) didExtendAboveIB = true;
        if (currentPrice < confirmedIBLow) didExtendBelowIB = true;

        // Compute validation scores at window boundaries
        if (barsTracked == SHORT_WINDOW && shortWindowScore < 0.0f) {
            shortWindowScore = ComputeValidationScore(SHORT_WINDOW);
        }
        if (barsTracked == MEDIUM_WINDOW && mediumWindowScore < 0.0f) {
            mediumWindowScore = ComputeValidationScore(MEDIUM_WINDOW);
        }
        if (barsTracked == LONG_WINDOW && longWindowScore < 0.0f) {
            longWindowScore = ComputeValidationScore(LONG_WINDOW);
        }
    }

    // Compute validation score for a given window
    float ComputeValidationScore(int windowBars) const {
        if (barsTracked < windowBars) return -1.0f;

        switch (confirmedShape) {
            case AMT::ProfileShape::P_SHAPED: {
                // P-shape correct if price stayed above POC and/or extended up
                float aboveRatio = static_cast<float>(barsAbovePOC) / windowBars;
                float extensionBonus = didExtendAboveIB ? 0.2f : 0.0f;
                return (std::min)(1.0f, aboveRatio + extensionBonus);
            }

            case AMT::ProfileShape::B_SHAPED: {
                // b-shape correct if price stayed below POC and/or extended down
                float belowRatio = static_cast<float>(barsBelowPOC) / windowBars;
                float extensionBonus = didExtendBelowIB ? 0.2f : 0.0f;
                return (std::min)(1.0f, belowRatio + extensionBonus);
            }

            case AMT::ProfileShape::D_SHAPED:
            case AMT::ProfileShape::BALANCED:
            case AMT::ProfileShape::NORMAL_DISTRIBUTION: {
                // Balanced correct if price rotated within VA
                float inVARatio = static_cast<float>(barsInVA) / windowBars;
                return inVARatio;
            }

            case AMT::ProfileShape::DOUBLE_DISTRIBUTION: {
                // DD correct if price spent time in both upper and lower areas
                float upperRatio = static_cast<float>(barsAboveVA) / windowBars;
                float lowerRatio = static_cast<float>(barsBelowVA) / windowBars;
                if (upperRatio >= 0.2f && lowerRatio >= 0.2f) return 0.8f;
                if (upperRatio >= 0.1f && lowerRatio >= 0.1f) return 0.5f;
                return 0.2f;  // One-sided = wrong prediction
            }

            default:
                return 0.0f;
        }
    }

    // Check if a validation window is ready
    bool HasShortValidation() const { return shortWindowScore >= 0.0f; }
    bool HasMediumValidation() const { return mediumWindowScore >= 0.0f; }
    bool HasLongValidation() const { return longWindowScore >= 0.0f; }

    // Format validation log string
    // Output: "VALIDATE: P_SHAPED @30bars SCORE=0.78 | abvPOC=22 inVA=18 extUp=YES"
    std::string FormatValidationLogString(int windowBars, float score) const {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "VALIDATE: %s @%dbars SCORE=%.2f | abvPOC=%d blwPOC=%d inVA=%d extUp=%s extDn=%s",
            AMT::ProfileShapeToString(confirmedShape),
            windowBars, score,
            barsAbovePOC, barsBelowPOC, barsInVA,
            didExtendAboveIB ? "YES" : "NO",
            didExtendBelowIB ? "YES" : "NO");
        return std::string(buf);
    }

    // Format short validation if ready (for log output)
    std::string FormatShortValidation() const {
        if (!HasShortValidation()) return "";
        return FormatValidationLogString(SHORT_WINDOW, shortWindowScore);
    }

    // Format medium validation if ready
    std::string FormatMediumValidation() const {
        if (!HasMediumValidation()) return "";
        return FormatValidationLogString(MEDIUM_WINDOW, mediumWindowScore);
    }

    // Format long validation if ready
    std::string FormatLongValidation() const {
        if (!HasLongValidation()) return "";
        return FormatValidationLogString(LONG_WINDOW, longWindowScore);
    }

    void Reset() {
        confirmedShape = AMT::ProfileShape::UNDEFINED;
        confirmedBar = -1;
        confirmedPOCTicks = 0;
        confirmedVAHTicks = 0;
        confirmedVALTicks = 0;
        confirmedPrice = 0.0;
        confirmedIBHigh = 0.0;
        confirmedIBLow = 0.0;
        barsTracked = 0;
        barsAbovePOC = 0;
        barsBelowPOC = 0;
        barsInVA = 0;
        barsAboveVA = 0;
        barsBelowVA = 0;
        maxPriceReached = 0.0;
        minPriceReached = 0.0;
        didExtendAboveIB = false;
        didExtendBelowIB = false;
        shortWindowScore = -1.0f;
        mediumWindowScore = -1.0f;
        longWindowScore = -1.0f;
    }
};

// ============================================================================
// SHAPE VALIDATION STATS (aggregate accuracy tracking)
// ============================================================================
// Tracks prediction accuracy per shape type across sessions.
// Used for evidence-driven tuning of thresholds and gates.
// ============================================================================

struct ShapeValidationStats {
    struct ShapeAccuracy {
        int predictions = 0;
        float sumShortScores = 0.0f;
        float sumMediumScores = 0.0f;
        float sumLongScores = 0.0f;
        int shortValidations = 0;
        int mediumValidations = 0;
        int longValidations = 0;

        float GetShortAccuracy() const {
            return shortValidations > 0 ? sumShortScores / shortValidations : 0.0f;
        }
        float GetMediumAccuracy() const {
            return mediumValidations > 0 ? sumMediumScores / mediumValidations : 0.0f;
        }
        float GetLongAccuracy() const {
            return longValidations > 0 ? sumLongScores / longValidations : 0.0f;
        }
    };

    // Per-shape accuracy tracking
    ShapeAccuracy pShapeAccuracy;
    ShapeAccuracy bShapeAccuracy;
    ShapeAccuracy dShapeAccuracy;
    ShapeAccuracy ddShapeAccuracy;

    ShapeAccuracy& GetAccuracyForShape(AMT::ProfileShape shape) {
        switch (shape) {
            case AMT::ProfileShape::P_SHAPED: return pShapeAccuracy;
            case AMT::ProfileShape::B_SHAPED: return bShapeAccuracy;
            case AMT::ProfileShape::DOUBLE_DISTRIBUTION: return ddShapeAccuracy;
            default: return dShapeAccuracy;  // D-shaped, balanced, etc.
        }
    }

    const ShapeAccuracy& GetAccuracyForShape(AMT::ProfileShape shape) const {
        switch (shape) {
            case AMT::ProfileShape::P_SHAPED: return pShapeAccuracy;
            case AMT::ProfileShape::B_SHAPED: return bShapeAccuracy;
            case AMT::ProfileShape::DOUBLE_DISTRIBUTION: return ddShapeAccuracy;
            default: return dShapeAccuracy;
        }
    }

    void RecordPrediction(AMT::ProfileShape shape) {
        GetAccuracyForShape(shape).predictions++;
    }

    void RecordShortValidation(AMT::ProfileShape shape, float score) {
        auto& acc = GetAccuracyForShape(shape);
        acc.sumShortScores += score;
        acc.shortValidations++;
    }

    void RecordMediumValidation(AMT::ProfileShape shape, float score) {
        auto& acc = GetAccuracyForShape(shape);
        acc.sumMediumScores += score;
        acc.mediumValidations++;
    }

    void RecordLongValidation(AMT::ProfileShape shape, float score) {
        auto& acc = GetAccuracyForShape(shape);
        acc.sumLongScores += score;
        acc.longValidations++;
    }

    float GetOverallShortAccuracy() const {
        int total = pShapeAccuracy.shortValidations + bShapeAccuracy.shortValidations +
                    dShapeAccuracy.shortValidations + ddShapeAccuracy.shortValidations;
        if (total == 0) return 0.0f;
        float sum = pShapeAccuracy.sumShortScores + bShapeAccuracy.sumShortScores +
                    dShapeAccuracy.sumShortScores + ddShapeAccuracy.sumShortScores;
        return sum / total;
    }

    void Reset() {
        pShapeAccuracy = ShapeAccuracy();
        bShapeAccuracy = ShapeAccuracy();
        dShapeAccuracy = ShapeAccuracy();
        ddShapeAccuracy = ShapeAccuracy();
    }
};

// ============================================================================
// SESSION VOLUME PROFILE
// ============================================================================

struct SessionVolumeProfile
{
    std::map<int, VolumeAtPrice> volume_profile;  // price_tick -> data
    double tick_size = 0.0;

    // Logging support (set by main study after initialization)
    AMT::LogManager* logMgr_ = nullptr;
    void SetLogManager(AMT::LogManager* lm) { logMgr_ = lm; }

    // Helper for conditional logging through LogManager or fallback to direct
    void LogVBP(SCStudyInterfaceRef sc, int bar, const char* msg, bool warn = false) const {
        if (logMgr_) {
            if (warn)
                logMgr_->LogWarn(bar, msg, AMT::LogCategory::VBP);
            else
                logMgr_->LogInfo(bar, msg, AMT::LogCategory::VBP);
        } else {
            sc.AddMessageToLog(msg, warn ? 1 : 0);
        }
    }

    // ========================================================================
    // VERSIONED LEVELS (SSOT for POC/VAH/VAL)
    // Three-state semantics: current, stable, previous
    // ========================================================================
    AMT::VersionedLevels levels;

    // Legacy accessors (delegate to versioned levels for backward compatibility)
    // TODO: Migrate call sites to use levels.current.GetPOC() directly
    double session_poc = 0.0;  // Synced from levels.current
    double session_vah = 0.0;
    double session_val = 0.0;
    std::vector<double> session_hvn;  // High Volume Nodes (prices) - legacy flat list
    std::vector<double> session_lvn;  // Low Volume Nodes (prices) - legacy flat list

    // SSOT: Cached volume thresholds (computed once per refresh)
    AMT::VolumeThresholds cachedThresholds;

    // SSOT: Clustered nodes (replaces flat price lists for new code paths)
    std::vector<AMT::VolumeCluster> hvnClusters;
    std::vector<AMT::VolumeCluster> lvnClusters;

    // Prior session preservation
    std::vector<AMT::PriorSessionNode> priorSessionHVN;
    std::vector<AMT::PriorSessionNode> priorSessionLVN;

    // NOTE: NodeCandidate struct and hvnCandidates/lvnCandidates removed (Dec 2024)
    // They were used for hysteresis-based computed HVN/LVN, now replaced by SC's native peaks/valleys

    // Session tracking
    AMT::SessionPhase session_phase = AMT::SessionPhase::UNKNOWN;
    SCDateTime session_start;
    int bars_since_last_compute = 0;
    bool matchLogged = false; // Prevent log spam

    // Log spam prevention: track last logged profile
    mutable int lastLoggedProfileIdx = -1;
    mutable bool lastLoggedIsEvening = false;
    mutable int lastProfileLoadLogBar = -1;  // Throttle "Profile loaded" message

    // SSOT: Current profile index (set by PopulateFromVbPStudy)
    // Used for RTH peaks/valleys (GLOBEX uses -1 = last profile)
    int currentProfileIndex = -1;

    // SSOT: Dual-session peaks/valleys (both RTH and GLOBEX)
    DualSessionPeaksValleys dualSessionPV;

    // Diagnostic: disagreement counter (legacy ratio vs SSOT sigma)
    int sigmaHvnCount = 0;
    int ratioHvnCount = 0;
    int disagreementCount = 0;

    // VBP stability tracking (for diagnostics)
    double prev_poc = 0.0;
    double prev_vah = 0.0;
    double prev_val = 0.0;
    bool sessionSummaryLogged = false;
    int updateCount = 0;

    // POC stability hysteresis (for recenter decision)
    // Only recenter zones if POC has been stable at new price for N bars
    // (Hysteresis tracking removed - now recenters immediately on 2+ tick drift)

    // ========================================================================
    // PROFILE STRUCTURE (engine-like state for ComputeStructure)
    // ========================================================================
    AMT::ProfileStructureConfig structureConfig;
    AMT::ProfileStructureResult lastStructureResult;

    // Opening range tracker (IB for RTH, SOR for Globex)
    OpeningRangeTracker openingRangeTracker_;

    // Maturity FSM persistent state (survives across bars, reset on session)
    AMT::ProfileMaturityState confirmedMaturityState_ = AMT::ProfileMaturityState::IMMATURE;
    AMT::ProfileMaturityState candidateMaturityState_ = AMT::ProfileMaturityState::IMMATURE;
    int candidateConfirmationBars_ = 0;
    int barsInCurrentMaturityState_ = 0;

    // Log-on-change tracking
    AMT::ProfileMaturityState lastLoggedMaturityState_ = AMT::ProfileMaturityState::IMMATURE;
    int lastStructureLogBar_ = -100;

    // === CONFIRMED SHAPE STATE (replaces simple frozen state) ===
    // Shape confirmation tracks:
    // - Confidence decay as structure diverges from confirmed snapshot
    // - Structural break detection for re-evaluation triggers
    // - Transition cooldown to prevent flip-flopping
    ConfirmedShapeState confirmedShapeState_;
    ShapeBreakDetector shapeBreakDetector_;

    // === BEHAVIORAL VALIDATION (forward tracking of shape predictions) ===
    // Tracks price behavior AFTER shape confirmation to validate accuracy
    ShapeBehaviorTracker behaviorTracker_;
    ShapeValidationStats validationStats_;

    // Legacy accessors for backward compatibility
    bool IsShapeConfirmed() const { return confirmedShapeState_.IsConfirmed(); }
    AMT::ProfileShape GetConfirmedShape() const { return confirmedShapeState_.confirmedShape; }
    float GetEffectiveConfidence() const { return confirmedShapeState_.effectiveConfidence; }

    // Behavioral validation accessors
    const ShapeBehaviorTracker& GetBehaviorTracker() const { return behaviorTracker_; }
    const ShapeValidationStats& GetValidationStats() const { return validationStats_; }
    ShapeValidationStats& GetValidationStatsMutable() { return validationStats_; }

    // Volatility regime passthrough to break detector (for adaptive thresholds)
    void SetBreakDetectorVolatilityRegime(AMT::VolatilityRegime regime) {
        shapeBreakDetector_.SetVolatilityRegime(regime);
    }
    AMT::VolatilityRegime GetBreakDetectorVolatilityRegime() const {
        return shapeBreakDetector_.currentRegime;
    }

    // ========================================================================

    // Check if POC has migrated significantly (returns true if zone update needed)
    // migrationThresholdTicks: how many ticks of drift triggers zone update (default 2)
    bool HasPocMigrated(int migrationThresholdTicks = 2) const
    {
        if (prev_poc <= 0.0 || session_poc <= 0.0 || tick_size <= 0.0)
            return false;
        const double pocDrift = std::abs(session_poc - prev_poc);
        return pocDrift >= (tick_size * migrationThresholdTicks);
    }

    // Check and log VBP stability (call after each update)
    // Returns true if POC has been stable at new price for RECENTER_STABILITY_BARS
    // (zones should be recentered)
    bool CheckStability(SCStudyInterfaceRef sc, SCDateTime bar_time, int diagLevel)
    {
        (void)bar_time;  // Unused
        updateCount++;

        // === VERSIONED LEVELS: Sync current levels ===
        AMT::ProfileLevelsTicks newLevels;
        newLevels.SetFromPrices(session_poc, session_vah, session_val, tick_size);
        levels.UpdateCurrent(newLevels, tick_size);

        // Only check after first update (prev values will be 0 on first run)
        if (updateCount == 1)
        {
            prev_poc = session_poc;
            prev_vah = session_vah;
            prev_val = session_val;
            return false;
        }

        const double pocDrift = std::abs(session_poc - prev_poc);
        const double vahDrift = std::abs(session_vah - prev_vah);
        const double valDrift = std::abs(session_val - prev_val);

        // Warn on significant intra-session drift (>10 ticks)
        const double driftThreshold = tick_size * 10.0;
        const bool significantDrift = (pocDrift > driftThreshold || vahDrift > driftThreshold || valDrift > driftThreshold);

        if (significantDrift && diagLevel >= 2)
        {
            SCString msg;
            msg.Format("POC:%.2f->%.2f (%.0ft) VAH:%.2f->%.2f (%.0ft) VAL:%.2f->%.2f (%.0ft)",
                prev_poc, session_poc, pocDrift / tick_size,
                prev_vah, session_vah, vahDrift / tick_size,
                prev_val, session_val, valDrift / tick_size);
            LogVBP(sc, sc.Index, msg.GetChars(), false);
        }

        // === RECENTER DETECTION: Any level drift triggers recenter ===
        // Minimum drift threshold: 2 ticks for any level to trigger recenter
        const double minRecenterDrift = tick_size * 2.0;
        bool shouldRecenter = (pocDrift >= minRecenterDrift ||
                               vahDrift >= minRecenterDrift ||
                               valDrift >= minRecenterDrift);

        if (shouldRecenter)
        {
            // === VERSIONED LEVELS: Promote to stable ===
            levels.PromoteToStable();

            if (diagLevel >= 2)
            {
                SCString msg;
                msg.Format("Recenter triggered: POC=%.0ft VAH=%.0ft VAL=%.0ft",
                    pocDrift / tick_size, vahDrift / tick_size, valDrift / tick_size);
                LogVBP(sc, sc.Index, msg.GetChars(), false);
            }
        }

        prev_poc = session_poc;
        prev_vah = session_vah;
        prev_val = session_val;

        return shouldRecenter;
    }

    void Reset(double ts)
    {
        volume_profile.clear();
        tick_size = ts;
        session_poc = 0.0;
        session_vah = 0.0;
        session_val = 0.0;
        session_hvn.clear();
        session_lvn.clear();
        cachedThresholds.Reset();
        hvnClusters.clear();
        lvnClusters.clear();
        currentProfileIndex = -1;  // Reset SSOT profile index
        dualSessionPV.Clear();     // Reset dual-session peaks/valleys
        // NOTE: hvnCandidates/lvnCandidates removed - SC native peaks/valleys are SSOT
        session_phase = AMT::SessionPhase::UNKNOWN;
        bars_since_last_compute = 0;
        matchLogged = false;
        // Reset diagnostics
        sigmaHvnCount = 0;
        ratioHvnCount = 0;
        disagreementCount = 0;
        // Reset VBP stability tracking
        prev_poc = 0.0;
        prev_vah = 0.0;
        prev_val = 0.0;
        sessionSummaryLogged = false;
        updateCount = 0;
        // POC hysteresis tracking removed - now recenters immediately on 2+ tick drift
        // Reset versioned levels (full reset)
        levels.Reset();
        // Note: priorSession nodes are NOT cleared - they persist
        // Reset profile structure FSM state
        ResetStructureState();
    }

    // Reset profile structure FSM state (call on session transition)
    // NOTE: Also call ResetForNewSession(isRTH) to configure opening range tracker
    void ResetStructureState() {
        confirmedMaturityState_ = AMT::ProfileMaturityState::IMMATURE;
        candidateMaturityState_ = AMT::ProfileMaturityState::IMMATURE;
        candidateConfirmationBars_ = 0;
        barsInCurrentMaturityState_ = 0;
        lastStructureResult = AMT::ProfileStructureResult();
        lastLoggedMaturityState_ = AMT::ProfileMaturityState::IMMATURE;
        lastStructureLogBar_ = -100;
        // Reset confirmed shape state and break detector
        confirmedShapeState_.Reset();
        shapeBreakDetector_.Reset();
        // Reset behavior tracker (per-shape tracking resets on session)
        behaviorTracker_.Reset();
        // NOTE: validationStats_ intentionally NOT reset - accumulates across sessions
    }

    // Reset for new session with session type (call on session transition)
    // Sets session-specific parameters for opening range tracker (IB vs SOR)
    void ResetForNewSession(bool isRTH) {
        ResetStructureState();
        openingRangeTracker_.Reset(isRTH);
    }

    // ========================================================================
    // COMPUTE STRUCTURE (engine-like API for profile metrics + maturity FSM)
    // ========================================================================
    // Call once per bar AFTER PopulateFromVbPStudy() succeeds.
    // Returns ProfileStructureResult with validity, composite metrics, and maturity state.
    //
    // @param currentBar       Current bar index (for logging and event tracking)
    // @param sessionBars      Bars since session start
    // @param sessionMinutes   Minutes since session start
    // @param sessionRangeTicks Session range in ticks (for VA width ratio)
    // @param baseline         Optional historical baseline for volume sufficiency check
    // @return ProfileStructureResult with SSOT validity, metrics, and maturity
    AMT::ProfileStructureResult ComputeStructure(
        int currentBar,
        int sessionBars,
        int sessionMinutes,
        double sessionRangeTicks,
        const AMT::HistoricalProfileBaseline* baseline = nullptr)
    {
        AMT::ProfileStructureResult result;
        result.errorBar = currentBar;

        // --- Step 1: Validate tick_size ---
        if (tick_size <= 0.0) {
            result.errorReason = AMT::ProfileStructureErrorReason::ERR_TICK_SIZE_INVALID;
            lastStructureResult = result;
            return result;
        }

        // --- Step 2: Check volume profile population ---
        if (volume_profile.empty()) {
            result.errorReason = AMT::ProfileStructureErrorReason::WARMUP_VBP_STUDY;
            lastStructureResult = result;
            return result;
        }
        result.volumeProfilePopulated = true;
        result.priceLevelCount = static_cast<int>(volume_profile.size());

        // --- Step 3: Validate POC/VAH/VAL ---
        result.pocValid = AMT::IsValidPrice(session_poc);
        result.vaValid = AMT::IsValidPrice(session_vah) &&
                         AMT::IsValidPrice(session_val) &&
                         session_vah > session_val;

        if (!result.pocValid) {
            result.errorReason = AMT::ProfileStructureErrorReason::ERR_INVALID_POC;
            lastStructureResult = result;
            return result;
        }
        if (!result.vaValid) {
            result.errorReason = AMT::ProfileStructureErrorReason::ERR_INVALID_VA;
            lastStructureResult = result;
            return result;
        }

        // Store raw values
        result.sessionPOC = session_poc;
        result.sessionVAH = session_vah;
        result.sessionVAL = session_val;

        // --- Step 4: Thresholds & POC verification ---
        result.thresholdsComputed = cachedThresholds.valid;
        result.pocVolumeVerified = cachedThresholds.pocVolumeVerified;

        // --- Step 5: Compute composite metrics ---
        if (result.thresholdsComputed) {
            result.totalVolume = cachedThresholds.totalVolume;
            result.pocVolume = cachedThresholds.volumeAtPOC;

            // POC Dominance
            if (result.pocVolumeVerified && result.totalVolume > 0.0) {
                result.pocDominance = result.pocVolume / result.totalVolume;
                result.pocDominanceValid = true;
            }
        }

        // VA width metrics
        const int vahTicks = static_cast<int>(session_vah / tick_size + 0.5);
        const int valTicks = static_cast<int>(session_val / tick_size + 0.5);
        result.vaWidthTicks = vahTicks - valTicks;
        result.vaWidthValid = (result.vaWidthTicks > 0);

        // VA width ratio and compactness
        if (sessionRangeTicks > 0.0 && result.vaWidthValid) {
            result.vaWidthRatio = static_cast<double>(result.vaWidthTicks) / sessionRangeTicks;
            // Profile compactness: 1.0 when VA = 0% of range, 0.0 when VA >= 70% of range
            const double rawCompactness = 1.0 - (result.vaWidthRatio / 0.70);
            result.profileCompactness = (std::max)(0.0, (std::min)(1.0, rawCompactness));
            result.compactnessValid = true;
        }

        // HVN/LVN counts
        result.hvnCount = static_cast<int>(session_hvn.size());
        result.lvnCount = static_cast<int>(session_lvn.size());
        result.peaksValleysLoaded = (result.hvnCount > 0 || result.lvnCount > 0);

        // --- Step 6: Maturity gates ---
        result.hasMinLevels = (result.priceLevelCount >= AMT::ProfileMaturity::MIN_PRICE_LEVELS);
        result.hasMinBars = (sessionBars >= AMT::ProfileMaturity::MIN_BARS);
        result.hasMinMinutes = (sessionMinutes >= AMT::ProfileMaturity::MIN_MINUTES);

        // Volume sufficiency (NO-FALLBACK: only check if baseline ready)
        if (baseline != nullptr) {
            const AMT::ProgressBucket bucket = AMT::GetProgressBucket(sessionMinutes);
            if (baseline->IsVolumeSufficiencyReady(bucket)) {
                result.volumeSufficiencyValid = true;
                result.volumePercentile = baseline->GetVolumeSufficiencyPercentile(bucket, result.totalVolume);
                result.hasMinVolume = (result.volumePercentile >= AMT::ProfileMaturity::VOLUME_SUFFICIENCY_PERCENTILE);
            }
        }

        // --- Step 7: Determine raw maturity state ---
        if (!result.hasMinLevels || !result.hasMinBars) {
            result.rawMaturityState = AMT::ProfileMaturityState::IMMATURE;
        } else if (!result.hasMinMinutes || (result.volumeSufficiencyValid && !result.hasMinVolume)) {
            result.rawMaturityState = AMT::ProfileMaturityState::DEVELOPING;
        } else {
            result.rawMaturityState = AMT::ProfileMaturityState::MATURE;
        }

        // --- Step 8: Apply hysteresis (confirmation bars) ---
        const AMT::ProfileMaturityState prevConfirmed = confirmedMaturityState_;

        if (result.rawMaturityState == candidateMaturityState_) {
            candidateConfirmationBars_++;
        } else {
            // New candidate state
            candidateMaturityState_ = result.rawMaturityState;
            candidateConfirmationBars_ = 1;
        }

        // Check if candidate has enough confirmation bars
        if (candidateConfirmationBars_ >= structureConfig.maturityConfirmationBars) {
            if (candidateMaturityState_ != confirmedMaturityState_) {
                // State change confirmed
                confirmedMaturityState_ = candidateMaturityState_;
                barsInCurrentMaturityState_ = 1;
                result.maturityChanged = true;
                result.becameMature = (confirmedMaturityState_ == AMT::ProfileMaturityState::MATURE &&
                                       prevConfirmed != AMT::ProfileMaturityState::MATURE);
                result.becameImmature = (confirmedMaturityState_ == AMT::ProfileMaturityState::IMMATURE &&
                                         prevConfirmed != AMT::ProfileMaturityState::IMMATURE);
            } else {
                barsInCurrentMaturityState_++;
            }
        }

        // Populate result with FSM state
        result.maturityState = confirmedMaturityState_;
        result.candidateState = candidateMaturityState_;
        result.candidateConfirmationBars = candidateConfirmationBars_;
        result.barsInMaturityState = barsInCurrentMaturityState_;
        result.isTransitioning = (candidateMaturityState_ != confirmedMaturityState_);

        // --- Step 9: Final validity determination ---
        if (result.maturityState == AMT::ProfileMaturityState::IMMATURE) {
            result.errorReason = AMT::ProfileStructureErrorReason::WARMUP_MATURITY;
        } else {
            result.errorReason = AMT::ProfileStructureErrorReason::NONE;
        }

        lastStructureResult = result;
        return result;
    }

    // ========================================================================
    // COMPUTE SHAPE (populates shape fields in ProfileStructureResult)
    // ========================================================================
    // Call AFTER ComputeStructure when profile is ready (thresholdsComputed=true).
    // Extracts features, classifies shape, optionally resolves with DayStructure.
    //
    // Shape confirmation behavior (replaces simple freeze):
    // - Once shape passes all 6 gates, it becomes "confirmed"
    // - Confirmed shape persists but with confidence decay as structure diverges
    // - Structural breaks (POC drift, value acceptance, DD formation) trigger re-evaluation
    // - Transition cooldown prevents flip-flopping between shapes
    // - Call ResetStructureState() on session transition to reset
    //
    // @param result         ProfileStructureResult to populate (modified in place)
    // @param currentBar     Current bar index (for confirmation tracking)
    // @param sessionMinutes Minutes since session start (for time gates)
    // @param isRTH          true for RTH (uses IB), false for Globex (uses SOR)
    // @param sessionHighTicks Session high in ticks (for volume distribution)
    // @param sessionLowTicks  Session low in ticks (for volume distribution)
    // @param isAccepted     Volume acceptance state (for break detection)
    // @param priceOutsideVA  Price is currently outside value area
    // @param dayStructure   Optional DayStructure constraint (UNDEFINED = no constraint)
    // @param confirmOnResolve If true, confirm shape once resolved (default: true)
    void ComputeShape(
        AMT::ProfileStructureResult& result,
        int currentBar,
        int sessionMinutes,
        bool isRTH,
        int sessionHighTicks,
        int sessionLowTicks,
        bool isAccepted = false,
        bool priceOutsideVA = false,
        AMT::DayStructure dayStructure = AMT::DayStructure::UNDEFINED,
        bool confirmOnResolve = true)
    {
        // Get current profile metrics for break detection
        const int pocTick = (tick_size > 0.0) ?
            static_cast<int>(std::round(session_poc / tick_size)) : 0;
        const int vahTick = (tick_size > 0.0) ?
            static_cast<int>(std::round(session_vah / tick_size)) : 0;
        const int valTick = (tick_size > 0.0) ?
            static_cast<int>(std::round(session_val / tick_size)) : 0;

        // =====================================================================
        // STEP 1: IF SHAPE IS CONFIRMED, CHECK FOR BREAKS AND DECAY
        // =====================================================================
        if (confirmedShapeState_.IsConfirmed()) {
            // Get current HVN count for break detection (computed later, use cached)
            const int currentHVNCount = lastStructureResult.hvnClusterCount;

            // Update structural match (confidence decay)
            confirmedShapeState_.UpdateStructuralMatch(
                pocTick, vahTick, valTick,
                currentHVNCount, lastStructureResult.volumeUpperThirdRatio);

            // Update break detector
            shapeBreakDetector_.Update(
                currentBar, pocTick, confirmedShapeState_.confirmedPOCTicks,
                isAccepted, priceOutsideVA,
                currentHVNCount, confirmedShapeState_.confirmedHVNCount);

            // Check for confirmed structural break
            const bool breakConfirmed = shapeBreakDetector_.IsBreakConfirmed();
            const bool inCooldown = confirmedShapeState_.IsInCooldown(currentBar);

            // Populate result with confirmed state
            result.shapeConfirmed = true;
            result.shapeConfirmedBar = confirmedShapeState_.confirmedBar;
            result.effectiveConfidence = confirmedShapeState_.effectiveConfidence;
            result.structuralMatchScore = confirmedShapeState_.structuralMatchScore;
            result.inTransitionCooldown = inCooldown;
            result.transitionCount = confirmedShapeState_.transitionCount;
            result.lastTransitionBar = confirmedShapeState_.lastTransitionBar;

            // Populate break detection info
            result.breakDetected = (shapeBreakDetector_.candidateBreak != ShapeBreakType::NONE);
            result.breakConfirmed = breakConfirmed;
            result.breakType = static_cast<int>(shapeBreakDetector_.candidateBreak);
            result.breakConfirmationBars = shapeBreakDetector_.breakConfirmationBars;
            result.pocDriftTicks = shapeBreakDetector_.pocDriftTicks;
            result.barsAcceptedOutsideVA = shapeBreakDetector_.barsAcceptedOutsideValue;

            // If break confirmed and not in cooldown, unconfirm for re-evaluation
            if (breakConfirmed && !inCooldown) {
                confirmedShapeState_.Unconfirm(currentBar);
                shapeBreakDetector_.Reset();
                // Fall through to re-evaluate shape
            } else {
                // Return confirmed shape with decayed confidence
                result.rawShape = confirmedShapeState_.confirmedShape;
                result.rawShapeValid = true;
                result.resolvedShape = confirmedShapeState_.confirmedShape;
                result.shapeConflict = false;
                result.shapeResolution = breakConfirmed ? "BREAK_PENDING_COOLDOWN" : "CONFIRMED";
                result.shapeConfidence = confirmedShapeState_.effectiveConfidence;
                result.allGatesPass = true;  // Was true when confirmed
                // Legacy fields for compatibility
                result.shapeFrozen = true;
                result.shapeFrozenBar = confirmedShapeState_.confirmedBar;
                return;
            }
        }

        // =====================================================================
        // STEP 2: COMPUTE GATES AND CLASSIFY SHAPE
        // =====================================================================

        // GATE 1: Opening range completion check
        const int requiredMinutes = isRTH ? 60 : 90;  // RTH=IB(60), GBX=90 (lower volume needs more time)
        result.openingRangeComplete = (sessionMinutes >= requiredMinutes);

        // Copy opening range data to result
        result.openingRangeHigh = openingRangeTracker_.rangeHigh;
        result.openingRangeLow = openingRangeTracker_.rangeLow;
        result.hasRangeExtensionUp = openingRangeTracker_.hasExtendedAbove;
        result.hasRangeExtensionDown = openingRangeTracker_.hasExtendedBelow;

        // GATE 4: Failed auction detection (from tracker)
        result.failedAuctionUp = openingRangeTracker_.failedAuctionUp;
        result.failedAuctionDown = openingRangeTracker_.failedAuctionDown;
        result.noFailedAuction = !result.failedAuctionUp && !result.failedAuctionDown;

        // GATE 6: Time-based confidence multiplier
        result.timeConfidenceMultiplier = AMT::GetTimeConfidenceMultiplier(sessionMinutes, isRTH);

        // Gate: need valid thresholds for feature extraction
        if (!result.thresholdsComputed || !cachedThresholds.valid) {
            result.shapeError = AMT::ShapeError::THRESHOLDS_INVALID;
            result.shapeResolution = "NO_THRESH";
            result.allGatesPass = false;
            return;
        }

        // Gate: need volume profile data
        if (volume_profile.empty()) {
            result.shapeError = AMT::ShapeError::HISTOGRAM_EMPTY;
            result.shapeResolution = "NO_DATA";
            result.allGatesPass = false;
            return;
        }

        // Convert map to contiguous array for feature extraction
        std::vector<VolumeAtPrice> vapArray;
        vapArray.reserve(volume_profile.size());
        for (const auto& kv : volume_profile) {
            vapArray.push_back(kv.second);
        }

        if (vapArray.empty()) {
            result.shapeError = AMT::ShapeError::HISTOGRAM_EMPTY;
            result.shapeResolution = "NO_DATA";
            result.allGatesPass = false;
            return;
        }

        // Extract features
        const AMT::ProfileFeatures features = AMT::ExtractProfileFeatures(
            vapArray.data(),
            static_cast<int>(vapArray.size()),
            pocTick,
            vahTick,
            valTick,
            cachedThresholds);

        if (!features.valid) {
            result.shapeError = features.extractionError;
            result.shapeResolution = "EXTRACT_FAIL";
            result.allGatesPass = false;
            return;
        }

        // Populate feature diagnostics
        result.pocInRange = features.pocInRange;
        result.breadth = features.breadth;
        result.asymmetry = features.asymmetry;
        result.peakiness = features.peakiness;
        result.hvnClusterCount = static_cast<int>(features.hvnClusters.size());

        // Classify shape
        const AMT::ShapeClassificationResult classResult = AMT::ClassifyProfileShape(features);

        result.rawShape = classResult.shape;
        result.rawShapeValid = classResult.ok();
        result.shapeError = classResult.error;
        result.shapeConfidence = classResult.confidence01;

        if (!classResult.ok()) {
            result.shapeResolution = "CLASSIFY_FAIL";
            result.allGatesPass = false;
            return;
        }

        // GATE 2: POC stability for shape
        result.pocStableForShape = result.hasMinMinutes;  // Proxy: if profile mature, POC is stable

        // GATE 3: Auction validation for P/b shapes
        const bool isImbalanceShape = (result.rawShape == AMT::ProfileShape::P_SHAPED ||
                                       result.rawShape == AMT::ProfileShape::B_SHAPED);
        if (isImbalanceShape) {
            const bool isP = (result.rawShape == AMT::ProfileShape::P_SHAPED);

            // Check range extension in the right direction
            const bool hasCorrectExtension = isP ? result.hasRangeExtensionUp :
                                                   result.hasRangeExtensionDown;

            // Check single prints (tail/excess) in the thin part
            const double avgVolPerLevel = cachedThresholds.totalVolume /
                (std::max)(1, static_cast<int>(volume_profile.size()));

            bool hasTail = false;
            if (isP) {
                hasTail = AMT::HasSinglePrints(volume_profile, sessionLowTicks, pocTick,
                                               avgVolPerLevel, 0.30);
                result.hasSinglePrintsBelow = hasTail;
            } else {
                hasTail = AMT::HasSinglePrints(volume_profile, pocTick, sessionHighTicks,
                                               avgVolPerLevel, 0.30);
                result.hasSinglePrintsAbove = hasTail;
            }

            result.auctionValidated = hasCorrectExtension && hasTail;

            if (!result.auctionValidated) {
                result.shapeConfidence *= 0.5f;  // Geometric only, no auction evidence
            }
        } else {
            result.auctionValidated = true;
        }

        // GATE 5: Volume distribution validation
        double upperThirdRatio = 1.0;
        result.volumeConfirmsShape = AMT::ValidateVolumeDistribution(
            volume_profile,
            result.rawShape,
            sessionHighTicks,
            sessionLowTicks,
            upperThirdRatio);
        result.volumeUpperThirdRatio = upperThirdRatio;

        // ALL GATES CHECK
        result.allGatesPass = result.openingRangeComplete &&
                              result.pocStableForShape &&
                              result.auctionValidated &&
                              result.noFailedAuction &&
                              result.volumeConfirmsShape &&
                              result.timeConfidenceMultiplier >= 0.7;

        // Apply time-based confidence multiplier to final confidence
        result.shapeConfidence *= static_cast<float>(result.timeConfidenceMultiplier);

        // If opening range not complete, mark resolution accordingly
        if (!result.openingRangeComplete) {
            result.shapeResolution = "OPENING_RANGE_DEVELOPING";
            return;
        }

        // =====================================================================
        // STEP 3: RESOLVE WITH DAYSTRUCTURE AND CONFIRM IF GATES PASS
        // =====================================================================
        result.dayStructureUsed = dayStructure;

        if (dayStructure == AMT::DayStructure::UNDEFINED) {
            result.resolvedShape = result.rawShape;
            result.shapeConflict = false;
            result.shapeResolution = "RAW_ONLY";
        } else {
            const AMT::ShapeResolutionResult resolved =
                AMT::ResolveShapeWithDayStructure(result.rawShape, dayStructure);

            result.resolvedShape = resolved.finalShape;
            result.shapeConflict = resolved.conflict;
            result.shapeResolution = resolved.resolution;

            // Confirm shape if all gates pass and not in cooldown
            const bool inCooldown = confirmedShapeState_.IsInCooldown(currentBar);
            if (confirmOnResolve && !resolved.conflict &&
                resolved.finalShape != AMT::ProfileShape::UNDEFINED &&
                result.allGatesPass && result.shapeConfidence >= 0.6f && !inCooldown)
            {
                confirmedShapeState_.Confirm(
                    resolved.finalShape, currentBar, sessionMinutes, result.shapeConfidence,
                    pocTick, vahTick, valTick,
                    result.hvnClusterCount, upperThirdRatio);

                result.shapeConfirmed = true;
                result.shapeConfirmedBar = currentBar;
                result.effectiveConfidence = result.shapeConfidence;
                result.structuralMatchScore = 1.0f;

                // Legacy fields for compatibility
                result.shapeFrozen = true;
                result.shapeFrozenBar = currentBar;
            }
        }

        // Populate transition state
        result.inTransitionCooldown = confirmedShapeState_.IsInCooldown(currentBar);
        result.transitionCount = confirmedShapeState_.transitionCount;
        result.lastTransitionBar = confirmedShapeState_.lastTransitionBar;
    }

    // Overload without acceptance state (for backward compatibility)
    void ComputeShape(
        AMT::ProfileStructureResult& result,
        int currentBar,
        int sessionMinutes,
        bool isRTH,
        int sessionHighTicks,
        int sessionLowTicks,
        AMT::DayStructure dayStructure,
        bool confirmOnResolve)
    {
        // Call full version with default acceptance state
        ComputeShape(result, currentBar, sessionMinutes, isRTH,
                     sessionHighTicks, sessionLowTicks,
                     false, false,  // isAccepted, priceOutsideVA
                     dayStructure, confirmOnResolve);
    }

    // Legacy overload for backward compatibility (uses default session params)
    void ComputeShape(
        AMT::ProfileStructureResult& result,
        int currentBar,
        AMT::DayStructure dayStructure = AMT::DayStructure::UNDEFINED,
        bool freezeOnResolve = true)
    {
        // Use defaults: RTH, 120 minutes (mid-session), full session range
        const int sessionHighTicks = volume_profile.empty() ? 0 :
            static_cast<int>(volume_profile.rbegin()->first);
        const int sessionLowTicks = volume_profile.empty() ? 0 :
            static_cast<int>(volume_profile.begin()->first);
        ComputeShape(result, currentBar, 120, true, sessionHighTicks, sessionLowTicks,
                     dayStructure, freezeOnResolve);
    }

    // ========================================================================
    // VOLUME QUERY METHODS (for ExtremeAcceptanceTracker)
    // ========================================================================

    /**
     * Get total session volume from the VbP profile.
     * @return Total volume across all price levels, or 0 if empty.
     */
    double GetTotalVolume() const {
        double total = 0.0;
        for (const auto& kv : volume_profile) {
            total += kv.second.Volume;
        }
        return total;
    }

    /**
     * Get volume at a specific price (returns 0 if not found).
     * @param price Price level to query
     * @return Volume traded at that price, or 0 if not in profile.
     */
    double GetVolumeAtPrice(double price) const {
        if (tick_size <= 0.0) return 0.0;
        const int priceTick = static_cast<int>(std::round(price / tick_size));
        auto it = volume_profile.find(priceTick);
        if (it != volume_profile.end()) {
            return it->second.Volume;
        }
        return 0.0;
    }

    /**
     * Get total volume within a band around a price.
     * @param price Center price
     * @param bandTicks Number of ticks above and below center to include
     * @return Sum of volume within [price - bandTicks*tickSize, price + bandTicks*tickSize]
     */
    double GetVolumeInBand(double price, int bandTicks) const {
        if (tick_size <= 0.0) return 0.0;
        const int centerTick = static_cast<int>(std::round(price / tick_size));
        double bandVolume = 0.0;

        for (int offset = -bandTicks; offset <= bandTicks; ++offset) {
            const int targetTick = centerTick + offset;
            auto it = volume_profile.find(targetTick);
            if (it != volume_profile.end()) {
                bandVolume += it->second.Volume;
            }
        }
        return bandVolume;
    }

    /**
     * Query volume concentration at session extremes for ExtremeAcceptanceTracker.
     * @param highPrice Session high price
     * @param lowPrice Session low price
     * @param bandTicks Ticks around extreme to include in band (default: 2)
     * @param[out] highVolumeInBand Volume in band around session high
     * @param[out] lowVolumeInBand Volume in band around session low
     * @param[out] totalVolume Total session volume
     * @return True if profile has data to query.
     */
    bool GetExtremeVolumeConcentration(
        double highPrice, double lowPrice, int bandTicks,
        double& highVolumeInBand, double& lowVolumeInBand, double& totalVolume) const
    {
        if (volume_profile.empty()) {
            highVolumeInBand = 0.0;
            lowVolumeInBand = 0.0;
            totalVolume = 0.0;
            return false;
        }

        totalVolume = GetTotalVolume();
        highVolumeInBand = GetVolumeInBand(highPrice, bandTicks);
        lowVolumeInBand = GetVolumeInBand(lowPrice, bandTicks);
        return true;
    }

    /**
     * Extract volume data as contiguous array for single print detection.
     * Returns volume at each tick from lowTick to highTick.
     *
     * @param volumeArray   Output array of volume values (will be resized)
     * @param priceStart    Output: price at index 0
     * @param avgVolume     Output: average volume per level
     * @return              Number of levels, or 0 if profile empty
     */
    int ExtractVolumeArray(
        std::vector<double>& volumeArray,
        double& priceStart,
        double& avgVolume) const
    {
        if (volume_profile.empty() || tick_size <= 0.0) {
            volumeArray.clear();
            priceStart = 0.0;
            avgVolume = 0.0;
            return 0;
        }

        // Find min and max price ticks
        int minTick = volume_profile.begin()->first;
        int maxTick = volume_profile.rbegin()->first;

        // Compute range
        const int numLevels = maxTick - minTick + 1;
        if (numLevels <= 0 || numLevels > 10000) {  // Sanity check
            volumeArray.clear();
            priceStart = 0.0;
            avgVolume = 0.0;
            return 0;
        }

        // Allocate and fill array
        volumeArray.resize(numLevels, 0.0);
        priceStart = minTick * tick_size;

        double totalVol = 0.0;
        int populatedLevels = 0;

        for (const auto& kv : volume_profile) {
            const int idx = kv.first - minTick;
            if (idx >= 0 && idx < numLevels) {
                volumeArray[idx] = static_cast<double>(kv.second.Volume);
                totalVol += kv.second.Volume;
                populatedLevels++;
            }
        }

        avgVolume = (populatedLevels > 0) ? (totalVol / populatedLevels) : 0.0;
        return numLevels;
    }

    /**
     * Get tail size at session extreme (single-print tail for excess detection).
     * Scans from extreme toward POC counting contiguous thin-volume levels.
     *
     * @param extremePrice  Session high or low price
     * @param poc           Point of Control price
     * @param tickSize      Tick size
     * @param thinThreshold Volume threshold for "thin" (fraction of avgVolume)
     * @return              Tail size in ticks (0 if no thin tail found)
     */
    double GetTailAtExtreme(double extremePrice, double poc, double thinThreshold = 0.15) const
    {
        if (volume_profile.empty() || tick_size <= 0.0 || poc <= 0.0) {
            return 0.0;
        }

        // Determine direction: scanning from extreme toward POC
        const bool scanDown = (extremePrice > poc);  // High extreme, scan downward
        const int extremeTick = static_cast<int>(extremePrice / tick_size + 0.5);
        const int pocTick = static_cast<int>(poc / tick_size + 0.5);

        // Calculate average volume
        double totalVol = 0.0;
        for (const auto& kv : volume_profile) {
            totalVol += kv.second.Volume;
        }
        const double avgVol = totalVol / volume_profile.size();
        const double threshold = avgVol * thinThreshold;

        // Scan from extreme toward POC
        int tailTicks = 0;
        const int direction = scanDown ? -1 : 1;
        int currentTick = extremeTick;

        while (true) {
            // Check if we've reached or passed POC
            if ((scanDown && currentTick <= pocTick) ||
                (!scanDown && currentTick >= pocTick)) {
                break;
            }

            // Look up volume at this tick
            auto it = volume_profile.find(currentTick);
            double vol = 0.0;
            if (it != volume_profile.end()) {
                vol = static_cast<double>(it->second.Volume);
            }

            // Is this level thin?
            if (vol < threshold) {
                tailTicks++;
            } else {
                // Hit a level with significant volume - tail ends
                break;
            }

            currentTick += direction;

            // Safety: don't scan more than 100 ticks
            if (tailTicks > 100) break;
        }

        return static_cast<double>(tailTicks);
    }

    // NOTE: OnSessionBoundary() was removed as dead code.
    // Prior session levels are captured via ZoneSessionState.CapturePriorSession() instead.
    // VersionedLevels.PromoteToPrevious()/ResetForNewSession() remain available in AMT_Levels.h
    // if needed in the future.

    // Archive current session nodes before reset (call before Reset)
    // SSOT FIX: Age existing nodes FIRST, then add new ones with sessionAge=1
    void ArchivePriorSession(int currentBar, AMT::SessionPhase closingSessionType = AMT::SessionPhase::UNKNOWN)
    {
        // Capture state BEFORE archiving for diagnostic log
        lastArchiveLog.bar = currentBar;
        lastArchiveLog.sessionType = closingSessionType;
        lastArchiveLog.priorHvnCountBefore = static_cast<int>(priorSessionHVN.size());
        lastArchiveLog.priorLvnCountBefore = static_cast<int>(priorSessionLVN.size());
        lastArchiveLog.hvnArchived = static_cast<int>(session_hvn.size());
        lastArchiveLog.lvnArchived = static_cast<int>(session_lvn.size());

        // Capture first 3 HVN prices being archived
        for (int i = 0; i < 3 && i < static_cast<int>(session_hvn.size()); ++i) {
            lastArchiveLog.firstHvnPrices[i] = session_hvn[i];
        }
        // Capture first 3 LVN prices being archived
        for (int i = 0; i < 3 && i < static_cast<int>(session_lvn.size()); ++i) {
            lastArchiveLog.firstLvnPrices[i] = session_lvn[i];
        }

        // 1. Age existing prior session nodes FIRST (before adding new ones)
        for (auto& node : priorSessionHVN) {
            node.sessionAge++;
        }
        for (auto& node : priorSessionLVN) {
            node.sessionAge++;
        }

        // 2. Move current HVN to prior session list (new nodes start at age 1)
        for (const double hvnPrice : session_hvn) {
            AMT::PriorSessionNode node;
            node.price = hvnPrice;
            node.density = AMT::VAPDensityClass::HIGH;
            node.strengthAtClose = 1.0;  // Could be computed from cluster data
            node.touchCount = 0;
            node.sessionAge = 1;  // Age 1 = most recent prior session
            node.sessionType = closingSessionType;  // Track session type (RTH/GLOBEX)
            priorSessionHVN.push_back(node);
        }

        // 3. Move current LVN to prior session list
        for (const double lvnPrice : session_lvn) {
            AMT::PriorSessionNode node;
            node.price = lvnPrice;
            node.density = AMT::VAPDensityClass::LOW;
            node.strengthAtClose = 1.0;
            node.touchCount = 0;
            node.sessionAge = 1;
            node.sessionType = closingSessionType;
            priorSessionLVN.push_back(node);
        }

        // Capture state AFTER archiving
        lastArchiveLog.priorHvnCountAfter = static_cast<int>(priorSessionHVN.size());
        lastArchiveLog.priorLvnCountAfter = static_cast<int>(priorSessionLVN.size());
    }

    // Prune old prior session references
    void PrunePriorReferences(int maxSessionAge = 3)
    {
        priorSessionHVN.erase(
            std::remove_if(priorSessionHVN.begin(), priorSessionHVN.end(),
                [maxSessionAge](const AMT::PriorSessionNode& n) {
                    return n.sessionAge > maxSessionAge;
                }),
            priorSessionHVN.end());
        priorSessionLVN.erase(
            std::remove_if(priorSessionLVN.begin(), priorSessionLVN.end(),
                [maxSessionAge](const AMT::PriorSessionNode& n) {
                    return n.sessionAge > maxSessionAge;
                }),
            priorSessionLVN.end());
    }

    // NOTE: AddBar() and ComputeDerivedLevels() REMOVED - VbP Study is SSOT
    // All profile data comes from PopulateFromVbPStudy()

    // Compute and cache SSOT thresholds from current profile
    void ComputeThresholds(int currentBar, double hvnSigmaCoeff = 1.5, double lvnSigmaCoeff = 0.5)
    {
        cachedThresholds.Reset();

        if (volume_profile.size() < 5) {
            return;
        }

        // Calculate mean, stddev, and maxVol from volume_profile (single pass for total + max)
        double totalVol = 0.0;
        double maxVol = 0.0;
        for (const auto& kv : volume_profile) {
            const double vol = static_cast<double>(kv.second.Volume);
            totalVol += vol;
            if (vol > maxVol) maxVol = vol;
        }

        const size_t numLevels = volume_profile.size();
        const double mean = totalVol / numLevels;

        double variance = 0.0;
        for (const auto& kv : volume_profile) {
            double diff = static_cast<double>(kv.second.Volume) - mean;
            variance += diff * diff;
        }
        const double stddev = std::sqrt(variance / numLevels);

        // Store in cache
        cachedThresholds.mean = mean;
        cachedThresholds.stddev = stddev;
        cachedThresholds.hvnThreshold = mean + hvnSigmaCoeff * stddev;
        cachedThresholds.lvnThreshold = mean - lvnSigmaCoeff * stddev;
        cachedThresholds.sampleSize = static_cast<int>(numLevels);
        cachedThresholds.totalVolume = totalVol;
        cachedThresholds.maxLevelVolume = maxVol;
        cachedThresholds.computedAtBar = currentBar;
        cachedThresholds.valid = true;
    }

    // =========================================================================
    // NOTE: Computed HVN/LVN functions were removed (Dec 2024)
    // SSOT is now Sierra Chart's native peaks/valleys via GetStudyPeakValleyLine()
    // See PopulatePeaksValleysFromVbP() for the current implementation.
    //
    // Removed functions:
    // - FindHvnLvn() - computed HVN/LVN from sigma thresholds
    // - MergeClusters() - merged adjacent volume clusters
    // - GetLVNsInGap(), HasLVNInGap() - gap detection for merge blocking
    // - RefreshWithHysteresis() - intra-session refresh with confirmation bars
    // - ApplyHysteresis() - node candidate confirmation/demotion logic
    // - GetAuditLogString() - audit logging for computed nodes
    //
    // cachedThresholds remains for volume density classification at any price
    // (separate concept from peaks/valleys which are specific price levels)
    // =========================================================================

    // Session archive log
    struct ArchiveLog {
        int bar = 0;
        AMT::SessionPhase sessionType = AMT::SessionPhase::UNKNOWN;
        int hvnArchived = 0;
        int lvnArchived = 0;
        double firstHvnPrices[3] = {0.0, 0.0, 0.0};
        double firstLvnPrices[3] = {0.0, 0.0, 0.0};
        int priorHvnCountBefore = 0;
        int priorHvnCountAfter = 0;
        int priorLvnCountBefore = 0;
        int priorLvnCountAfter = 0;
    };
    ArchiveLog lastArchiveLog;

    // -------------------------------------------------------------------------
    // VbP Session Info: SSOT for session boundaries
    // -------------------------------------------------------------------------
    struct VbPSessionInfo {
        SCDateTime sessionStart = 0;  // Profile's m_StartDateTime
        bool       isEvening = false; // true = Globex, false = RTH
        bool       valid = false;     // true if successfully read from VbP
        int        profileIndex = -1; // The actual profile index found
    };

    // NOTE: Session time determination uses AMT::IsTimeInRTH() from amt_core.h (SSOT)
    // Evening/GLOBEX = !AMT::IsTimeInRTH(timeOfDaySec, rthStartSec, rthEndSec)

    // GetCurrentProfileIndex: VBP profile index 0 is ALWAYS the current session's profile.
    // Per Sierra Chart ACSIL documentation, profile 0 = most recent/active session.
    int GetCurrentProfileIndex(
        SCStudyInterfaceRef /*sc*/,
        int vbpStudyId,
        int /*diagLevel*/) const
    {
        // VBP profile 0 is always the current session - per SC documentation
        if (vbpStudyId <= 0)
            return 0;

        return 0;  // Always use profile 0 - the current session
    }

    // Legacy wrapper for compatibility - now just returns 0
    int FindCurrentSessionProfile(
        SCStudyInterfaceRef sc,
        int vbpStudyId,
        bool /*expectEvening*/,  // Ignored - session type comes from profile, not bar
        int /*rthStartSec*/,
        int /*rthEndSec*/,
        int diagLevel) const
    {
        return GetCurrentProfileIndex(sc, vbpStudyId, diagLevel);
    }

    // GetVbPSessionInfo: Query VbP study for session boundary info (SSOT)
    // IMPORTANT: Session type is derived FROM the profile metadata, not from bar time.
    VbPSessionInfo GetVbPSessionInfo(
        SCStudyInterfaceRef sc,
        int vbpStudyId,
        bool /*expectEvening*/,  // Ignored - derived from profile
        int rthStartSec,
        int rthEndSec,
        int diagLevel) const
    {
        (void)diagLevel;  // Unused
        VbPSessionInfo info;

        if (vbpStudyId <= 0)
            return info;

        // Always use profile 0 (current session)
        const int profileIndex = 0;

        n_ACSIL::s_StudyProfileInformation profileInfo;
        const int result = sc.GetStudyProfileInformation(vbpStudyId, profileIndex, profileInfo);

        if (result != 0)
        {
            info.sessionStart = profileInfo.m_StartDateTime;
            info.profileIndex = profileIndex;
            info.valid = true;

            // Derive isEvening from the profile's actual start time using SSOT function
            int startHour, startMinute, startSecond;
            profileInfo.m_StartDateTime.GetTimeHMS(startHour, startMinute, startSecond);
            const int profileStartTimeSec = startHour * 3600 + startMinute * 60 + startSecond;
            info.isEvening = !AMT::IsTimeInRTH(profileStartTimeSec, rthStartSec, rthEndSec);
        }

        return info;
    }

    // -------------------------------------------------------------------------
    // PopulateFromVbPStudy: Read native Volume by Price histogram from VbP study
    //
    // NOTE: When "Use Separate Profile For Evening Session" is enabled in VbP,
    //       Sierra Chart creates separate Day and Evening profiles. We use the
    //       m_EveningSession flag to identify session type (not start time).
    //       We search through available profiles to find the one matching our
    //       expected session type (RTH or GLOBEX).
    //
    // Returns: true if profile was successfully populated, false otherwise
    //          Returns false if no matching session profile is found
    // -------------------------------------------------------------------------
    bool PopulateFromVbPStudy(
        SCStudyInterfaceRef sc,
        int vbpStudyId,
        bool isRTHExpected,  // From SessionKey - MUST match VbP profile session type
        int rthStartSec,
        int rthEndSec,
        int diagLevel,
        bool isLiveBar = false,
        int barIdx = -1)  // Bar index for AutoLoop=0 compatibility (-1 = use sc.Index)
    {
        (void)rthStartSec;  // No longer used - we use m_EveningSession flag
        (void)rthEndSec;

        // AutoLoop=0 compatibility: use explicit barIdx if provided, else fallback to sc.Index
        const int currentBar = (barIdx >= 0) ? barIdx : sc.Index;

        // Validate inputs
        if (vbpStudyId <= 0 || tick_size <= 0.0)
        {
            SCString msg;
            msg.Format("Invalid inputs: studyId=%d, tick_size=%.6f", vbpStudyId, tick_size);
            LogVBP(sc, currentBar, msg.GetChars(), true);
            return false;
        }

        // --- Step 1: Get number of available profiles ---
        const int numProfiles = sc.GetNumStudyProfiles(vbpStudyId);

        if (numProfiles <= 0)
        {
            if (diagLevel >= 1)
            {
                SCString msg;
                msg.Format("No profiles available for studyId=%d", vbpStudyId);
                LogVBP(sc, currentBar, msg.GetChars(), true);
            }
            return false;
        }

        // --- Step 2: Search for matching session profile ---
        // Iterate through profiles to find one that matches our expected session type
        // Profile 0 is typically most recent, but we search all to be safe
        int matchedProfileIndex = -1;
        n_ACSIL::s_StudyProfileInformation profileInfo;
        bool isContinuousProfile = false;

        for (int profileIdx = 0; profileIdx < numProfiles; ++profileIdx)
        {
            n_ACSIL::s_StudyProfileInformation tempInfo;
            const int infoResult = sc.GetStudyProfileInformation(vbpStudyId, profileIdx, tempInfo);

            if (infoResult == 0)
                continue;  // Failed to get this profile, try next

            // Check if profile is CONTINUOUS (spans multiple sessions)
            // A continuous profile spans > 12 hours, crossing session boundaries
            const double profileDurationDays = tempInfo.m_EndDateTime.GetAsDouble() - tempInfo.m_StartDateTime.GetAsDouble();
            const bool profileIsContinuous = (profileDurationDays > 0.5);  // > 12 hours

            if (profileIsContinuous)
            {
                // Continuous profile contains all session data - use it
                matchedProfileIndex = profileIdx;
                profileInfo = tempInfo;
                isContinuousProfile = true;
                break;
            }

            // Use Sierra Chart's native m_EveningSession flag (SSOT)
            // m_EveningSession = true means GLOBEX/Evening, false means RTH/Day
            const bool profileIsRTH = !tempInfo.m_EveningSession;

            if (profileIsRTH == isRTHExpected)
            {
                // Found matching session profile
                matchedProfileIndex = profileIdx;
                profileInfo = tempInfo;
                break;
            }
        }

        // --- Step 3: Handle no match found ---
        if (matchedProfileIndex < 0)
        {
            if (diagLevel >= 1)
            {
                SCString msg;
                msg.Format("No %s profile found among %d profiles - session data not yet available",
                    isRTHExpected ? "RTH" : "GLOBEX", numProfiles);
                LogVBP(sc, currentBar, msg.GetChars(), true);
            }
            return false;
        }

        // Get profile start time for logging
        int startHour, startMinute, startSecond;
        profileInfo.m_StartDateTime.GetTimeHMS(startHour, startMinute, startSecond);
        const bool profileIsRTH = !profileInfo.m_EveningSession;

        // --- Step 4: Log which profile we're using (live bar only to avoid spam) ---
        // Reset matchLogged if we switched profiles
        if (lastLoggedProfileIdx != matchedProfileIndex || lastLoggedIsEvening != profileInfo.m_EveningSession)
        {
            matchLogged = false;
        }

        if (diagLevel >= 2 && isLiveBar && !matchLogged)
        {
            SCString msg;
            msg.Format("Using %s profile idx=%d (start=%02d:%02d:%02d, m_EveningSession=%s)%s",
                profileIsRTH ? "RTH" : "GLOBEX", matchedProfileIndex,
                startHour, startMinute, startSecond,
                profileInfo.m_EveningSession ? "true" : "false",
                isContinuousProfile ? " [CONTINUOUS]" : "");
            LogVBP(sc, currentBar, msg.GetChars(), false);
            matchLogged = true;
            lastLoggedProfileIdx = matchedProfileIndex;
            lastLoggedIsEvening = profileInfo.m_EveningSession;
        }

        const int profileIndex = matchedProfileIndex;

        // SSOT: Store for use by PopulatePeaksValleysFromVbP
        currentProfileIndex = matchedProfileIndex;

        // --- Step 5: Get number of price levels ---
        const int numLevels = sc.GetNumPriceLevelsForStudyProfile(vbpStudyId, profileIndex);

        if (numLevels <= 0)
        {
            SCString msg;
            msg.Format("No price levels in profile. StudyID=%d, ProfileIdx=%d, numLevels=%d",
                vbpStudyId, profileIndex, numLevels);
            LogVBP(sc, currentBar, msg.GetChars(), true);
            return false;
        }

        // --- Step 6: Clear existing data ---
        volume_profile.clear();
        session_hvn.clear();
        session_lvn.clear();

        double totalVolume = 0.0;
        double maxVolume = 0.0;
        int pocTick = 0;

        // --- Step 7: Iterate through all price levels ---
        for (int priceIdx = 0; priceIdx < numLevels; ++priceIdx)
        {
            VolumeAtPrice vapData;

            const int result = sc.GetVolumeAtPriceDataForStudyProfile(
                vbpStudyId,
                profileIndex,
                priceIdx,
                vapData);

            if (result == 0)
            {
                // Failed to get this level - skip but continue
                continue;
            }

            // vapData.PriceInTicks is already in tick units
            const int priceTick = vapData.PriceInTicks;

            // Sanity check
            if (priceTick <= 0)
                continue;

            // Store the API struct directly
            volume_profile[priceTick] = vapData;

            totalVolume += vapData.Volume;

            // Track POC (highest volume level)
            if (vapData.Volume > maxVolume)
            {
                maxVolume = vapData.Volume;
                pocTick = priceTick;
            }
        }

        // --- Step 8: Validate we got data ---
        if (volume_profile.empty() || totalVolume <= 0.0)
        {
            if (diagLevel >= 2)
            {
                SCString msg;
                msg.Format("Empty profile after reading %d levels. StudyID=%d",
                    numLevels, vbpStudyId);
                LogVBP(sc, currentBar, msg.GetChars(), true);
            }
            return false;
        }

        // --- Step 9: Get POC from VbP study (SSOT - no fallback) ---
        if (!AMT::IsValidPrice(profileInfo.m_VolumePOCPrice))
        {
            if (diagLevel >= 1)
            {
                SCString msg;
                msg.Format("VbP study did not provide valid POC. POC=%.2f",
                    profileInfo.m_VolumePOCPrice);
                LogVBP(sc, currentBar, msg.GetChars(), true);
            }
            return false;
        }
        session_poc = profileInfo.m_VolumePOCPrice;

        // --- Step 10: Get VAH/VAL from VbP study (SSOT - no fallback) ---
        if (!AMT::IsValidPrice(profileInfo.m_VolumeValueAreaHigh) ||
            !AMT::IsValidPrice(profileInfo.m_VolumeValueAreaLow))
        {
            if (diagLevel >= 1)
            {
                SCString msg;
                msg.Format("VbP study did not provide valid VAH/VAL. VAH=%.2f VAL=%.2f",
                    profileInfo.m_VolumeValueAreaHigh, profileInfo.m_VolumeValueAreaLow);
                LogVBP(sc, currentBar, msg.GetChars(), true);
            }
            return false;
        }
        session_vah = profileInfo.m_VolumeValueAreaHigh;
        session_val = profileInfo.m_VolumeValueAreaLow;

        // NOTE: HVN/LVN (Peaks/Valleys) are now loaded via PopulatePeaksValleysFromVbP()
        // after this function returns. SSOT is Sierra Chart's native GetStudyPeakValleyLine API.

        bars_since_last_compute = 0;

        // Compute volume density thresholds for GetVbPContextAtPrice classification
        // This is separate from peaks/valleys - it classifies any price as HIGH/NORMAL/LOW
        ComputeThresholds(currentBar);

        // --- Step 11: POC Volume Verification ---
        // Verify that VbP study's POC corresponds to the max-volume level
        // This guards against VbP using smoothing, ties, or grouping rules that could
        // make "volume at POC" != "maximum volume across levels"
        {
            // Look up volume at VbP study's POC price
            const int pocTick = static_cast<int>(AMT::PriceToTicks(session_poc, tick_size));
            double volumeAtPOCPrice = 0.0;

            auto it = volume_profile.find(pocTick);
            if (it != volume_profile.end())
            {
                volumeAtPOCPrice = static_cast<double>(it->second.Volume);
            }
            else
            {
                // Try adjacent ticks (POC might be slightly off due to rounding)
                for (int offset = -1; offset <= 1; ++offset)
                {
                    auto nearby = volume_profile.find(pocTick + offset);
                    if (nearby != volume_profile.end())
                    {
                        if (static_cast<double>(nearby->second.Volume) > volumeAtPOCPrice)
                            volumeAtPOCPrice = static_cast<double>(nearby->second.Volume);
                    }
                }
            }

            cachedThresholds.volumeAtPOC = volumeAtPOCPrice;

            // Verify: does volumeAtPOC == maxLevelVolume (within 1% tolerance)?
            const double maxLevelVol = cachedThresholds.maxLevelVolume;
            constexpr double POC_VOLUME_TOLERANCE = 0.01;  // 1% tolerance

            if (maxLevelVol > 0.0 && volumeAtPOCPrice > 0.0)
            {
                const double relDiff = std::abs(volumeAtPOCPrice - maxLevelVol) / maxLevelVol;
                cachedThresholds.pocVolumeVerified = (relDiff <= POC_VOLUME_TOLERANCE);

                // Log diagnostic if they differ (rate-limited)
                if (!cachedThresholds.pocVolumeVerified && diagLevel >= 2)
                {
                    static int lastMismatchLogBar = -100;
                    if (currentBar - lastMismatchLogBar >= 50)
                    {
                        lastMismatchLogBar = currentBar;
                        SCString msg;
                        msg.Format("POC volume mismatch: volumeAtPOC=%.0f maxLevelVol=%.0f diff=%.1f%% - VbP may use smoothing/grouping",
                            volumeAtPOCPrice, maxLevelVol, relDiff * 100.0);
                        LogVBP(sc, currentBar, msg.GetChars(), true);
                    }
                }
            }
            else
            {
                cachedThresholds.pocVolumeVerified = false;
            }
        }

        // Log profile loaded with bar-based throttling (once per bar max)
        if (diagLevel >= 3 && currentBar != lastProfileLoadLogBar)
        {
            lastProfileLoadLogBar = currentBar;
            SCString msg;
            msg.Format("Profile loaded: Levels=%d POC=%.2f VAH=%.2f VAL=%.2f",
                static_cast<int>(volume_profile.size()),
                session_poc, session_vah, session_val);
            LogVBP(sc, currentBar, msg.GetChars(), false);
        }

        return true;
    }

    // -------------------------------------------------------------------------
    // PopulatePeaksValleysFromVbP: Read native Peaks/Valleys from VbP study
    //
    // profileIndex: -1 = last profile (GLOBEX), 0+ = specific profile index (RTH)
    // Returns: Number of peaks + valleys found
    // -------------------------------------------------------------------------
    int PopulatePeaksValleysFromVbP(
        SCStudyInterfaceRef sc,
        int vbpStudyId,
        int profileIndex,          // -1 = GLOBEX (last), 0 = RTH (first)
        int diagLevel)
    {
        // Clear existing computed HVN/LVN - we're replacing with native SC data
        session_hvn.clear();
        session_lvn.clear();
        hvnClusters.clear();
        lvnClusters.clear();

        const double tickSize = sc.TickSize;
        int peakCount = 0;
        int valleyCount = 0;

        if (diagLevel >= 1)
        {
            SCString msg;
            msg.Format("Using profileIndex=%d for GetStudyPeakValleyLine",
                profileIndex);
            LogVBP(sc, sc.Index, msg.GetChars(), false);
        }

        for (int pvIndex = 0; pvIndex < 100; ++pvIndex)  // Safety limit
        {
            float pvPrice = 0.0f;
            int pvType = 0;  // 1 = Peak (HVN), 2 = Valley (LVN)
            int startIndex = 0;
            int endIndex = 0;

            const int result = sc.GetStudyPeakValleyLine(
                sc.ChartNumber,
                vbpStudyId,
                pvPrice,
                pvType,
                startIndex,
                endIndex,
                profileIndex,  // -1 = GLOBEX (last), 0 = RTH (first)
                pvIndex);

            if (result == 0 || pvType == 0)
                break;  // No more peaks/valleys

            // Log each peak/valley with bar range
            if (diagLevel >= 1)
            {
                SCString msg;
                msg.Format("  PV[%d]: type=%d price=%.2f bars=%d-%d",
                    pvIndex, pvType, pvPrice, startIndex, endIndex);
                LogVBP(sc, sc.Index, msg.GetChars(), false);
            }

            if (pvType == 1)  // Peak = HVN
            {
                session_hvn.push_back(static_cast<double>(pvPrice));
                peakCount++;

                // Create cluster entry for HVN
                AMT::VolumeCluster cluster;
                cluster.lowPrice = pvPrice;
                cluster.highPrice = pvPrice;
                cluster.peakPrice = pvPrice;
                cluster.widthTicks = 1;
                cluster.density = AMT::VAPDensityClass::HIGH;
                hvnClusters.push_back(cluster);
            }
            else if (pvType == 2)  // Valley = LVN
            {
                session_lvn.push_back(static_cast<double>(pvPrice));
                valleyCount++;

                // Create cluster entry for LVN
                AMT::VolumeCluster cluster;
                cluster.lowPrice = pvPrice;
                cluster.highPrice = pvPrice;
                cluster.peakPrice = pvPrice;
                cluster.widthTicks = 1;
                cluster.density = AMT::VAPDensityClass::LOW;
                lvnClusters.push_back(cluster);
            }
        }

        // Diagnostic logging
        if (diagLevel >= 1)
        {
            SCString msg;
            msg.Format("Peaks: ProfileIdx=%d | HVN=%d LVN=%d",
                profileIndex, peakCount, valleyCount);
            LogVBP(sc, sc.Index, msg.GetChars(), false);

            // Log each HVN with price
            for (size_t i = 0; i < session_hvn.size(); ++i)
            {
                msg.Format("  HVN[%d]: %.2f", static_cast<int>(i), session_hvn[i]);
                LogVBP(sc, sc.Index, msg.GetChars(), false);
            }
            // Log each LVN with price
            for (size_t i = 0; i < session_lvn.size(); ++i)
            {
                msg.Format("  LVN[%d]: %.2f", static_cast<int>(i), session_lvn[i]);
                LogVBP(sc, sc.Index, msg.GetChars(), false);
            }
        }

        return peakCount + valleyCount;
    }

    // =========================================================================
    // PopulateDualSessionPeaksValleys: Load peaks/valleys for BOTH RTH and GLOBEX
    //
    // Scans recent profiles using negative indices (-1, -2, ...) to find:
    //   - Most recent RTH profile
    //   - Most recent GLOBEX profile
    // Then loads peaks/valleys from each.
    //
    // rthStartSec/rthEndSec: Session boundaries from study inputs (seconds from midnight)
    // GLOBEX window: Outside RTH window (spans midnight)
    //
    // Returns: true if at least one profile was found and loaded
    // =========================================================================
    bool PopulateDualSessionPeaksValleys(
        SCStudyInterfaceRef sc,
        int vbpStudyId,
        int rthStartSec,
        int rthEndSec,
        int diagLevel)
    {

        dualSessionPV.Clear();

        if (vbpStudyId <= 0)
            return false;

        // Get total number of profiles (positive indices only work correctly)
        const int numProfiles = sc.GetNumStudyProfiles(vbpStudyId);
        if (numProfiles <= 0)
        {
            if (diagLevel >= 1)
            {
                SCString msg;
                msg.Format("No profiles available for studyId=%d", vbpStudyId);
                LogVBP(sc, sc.Index, msg.GetChars(), false);
            }
            return false;
        }

        // Track which profiles we found (store positive indices)
        int foundRthIdx = -1;
        int foundGlobexIdx = -1;
        SCDateTime foundRthDate;
        SCDateTime foundGlobexDate;

        // Diagnostic: build profile dump table
        struct ProfileDump {
            int idx;
            SCDateTime startDT;
            SCDateTime endDT;
            int startHour, startMin, startSec;
            ProfileSessionType classification;
            bool valid;
        };
        std::vector<ProfileDump> profileDumps;

        // Scan profiles using positive indices, from most recent (numProfiles-1) backwards
        // This ensures we find the MOST RECENT RTH and GLOBEX profiles first
        const int maxToScan = (std::min)(numProfiles, 12);
        for (int i = 0; i < maxToScan; ++i)
        {
            const int profileIdx = numProfiles - 1 - i;  // Start from most recent

            n_ACSIL::s_StudyProfileInformation profileInfo;
            const int result = sc.GetStudyProfileInformation(vbpStudyId, profileIdx, profileInfo);

            if (result == 0)
                continue;  // Skip invalid profiles

            // Extract start time components
            int startHour, startMin, startSecond;
            profileInfo.m_StartDateTime.GetTimeHMS(startHour, startMin, startSecond);
            const int startTimeSec = startHour * 3600 + startMin * 60 + startSecond;

            // Classify using SSOT: AMT::IsTimeInRTH()
            ProfileSessionType classification = ProfileSessionType::UNKNOWN;
            if (AMT::IsTimeInRTH(startTimeSec, rthStartSec, rthEndSec))
            {
                classification = ProfileSessionType::RTH;
                // Keep the most recent RTH (first one we find when iterating backwards)
                if (foundRthIdx < 0 || profileInfo.m_StartDateTime > foundRthDate)
                {
                    foundRthIdx = profileIdx;
                    foundRthDate = profileInfo.m_StartDateTime;
                }
            }
            else
            {
                // GLOBEX: Outside RTH window (spans midnight)
                classification = ProfileSessionType::GLOBEX;
                // Keep the most recent GLOBEX (first one we find when iterating backwards)
                if (foundGlobexIdx < 0 || profileInfo.m_StartDateTime > foundGlobexDate)
                {
                    foundGlobexIdx = profileIdx;
                    foundGlobexDate = profileInfo.m_StartDateTime;
                }
            }

            // Store for diagnostic dump
            ProfileDump dump;
            dump.idx = profileIdx;
            dump.startDT = profileInfo.m_StartDateTime;
            dump.endDT = profileInfo.m_EndDateTime;
            dump.startHour = startHour;
            dump.startMin = startMin;
            dump.startSec = startSecond;
            dump.classification = classification;
            dump.valid = true;
            profileDumps.push_back(dump);
        }

        // =====================================================================
        // API-BASED APPROACH: Use GetStudyPeakValleyLine with found profile indices
        // Note: SG18/SG19 subgraph approach was tested but arrays return size=0
        // =====================================================================

        // Helper to check if a price is already in a vector (avoid duplicates)
        auto containsPrice = [](const std::vector<double>& vec, double price, double tolerance = 0.001) -> bool {
            for (double p : vec)
                if (std::abs(p - price) < tolerance)
                    return true;
            return false;
        };

        // Helper lambda to load peaks/valleys for a specific profile
        // NOTE: SC API has a bug where profileIdx is ignored - it always returns current profile data
        // We use profileIdx=-1 (current profile) as workaround
        auto loadPeaksValleys = [&](int profileIdx, std::vector<double>& hvnOut, std::vector<double>& lvnOut,
                                    const char* sessionName) -> int {
            int peakCount = 0, valleyCount = 0;

            for (int pvIndex = 0; pvIndex < 50; ++pvIndex)  // Safety limit
            {
                float pvPrice = 0.0f;
                int pvType = 0;  // 1 = Peak (HVN), 2 = Valley (LVN)
                int startIndex = 0, endIndex = 0;

                const int result = sc.GetStudyPeakValleyLine(
                    sc.ChartNumber, vbpStudyId, pvPrice, pvType,
                    startIndex, endIndex, profileIdx, pvIndex);

                if (result == 0 || pvType == 0)
                    break;  // No more peaks/valleys

                const double priceD = static_cast<double>(pvPrice);

                if (pvType == 1)  // Peak = HVN
                {
                    if (!containsPrice(hvnOut, priceD))
                    {
                        hvnOut.push_back(priceD);
                        ++peakCount;
                    }
                }
                else if (pvType == 2)  // Valley = LVN
                {
                    if (!containsPrice(lvnOut, priceD))
                    {
                        lvnOut.push_back(priceD);
                        ++valleyCount;
                    }
                }
            }
            return peakCount + valleyCount;
        };

        // SC API BUG WORKAROUND:
        // GetStudyPeakValleyLine ignores profileIdx and always returns current profile's peaks/valleys.
        // So we query once with profileIdx=-1 (current) and store in the appropriate session.

        // Determine current session from first bar time
        int curHour = 0, curMin = 0, curSec = 0;
        if (sc.ArraySize > 0)
            sc.BaseDateTimeIn[sc.ArraySize - 1].GetTimeHMS(curHour, curMin, curSec);
        const int curTimeSec = curHour * 3600 + curMin * 60 + curSec;
        const bool isCurrentlyRTH = AMT::IsTimeInRTH(curTimeSec, rthStartSec, rthEndSec);

        int rthPeaks = 0, rthValleys = 0;
        int globexPeaks = 0, globexValleys = 0;

        // Load peaks/valleys from current profile only (API limitation)
        if (isCurrentlyRTH)
        {
            loadPeaksValleys(-1, dualSessionPV.rth.hvn, dualSessionPV.rth.lvn, "RTH");
            rthPeaks = static_cast<int>(dualSessionPV.rth.hvn.size());
            rthValleys = static_cast<int>(dualSessionPV.rth.lvn.size());
            // Use found RTH profile info if available
            if (foundRthIdx >= 0)
                dualSessionPV.rth.profileIndex = foundRthIdx;
        }
        else
        {
            loadPeaksValleys(-1, dualSessionPV.globex.hvn, dualSessionPV.globex.lvn, "GBX");
            globexPeaks = static_cast<int>(dualSessionPV.globex.hvn.size());
            globexValleys = static_cast<int>(dualSessionPV.globex.lvn.size());
            // Use found GLOBEX profile info if available
            if (foundGlobexIdx >= 0)
                dualSessionPV.globex.profileIndex = foundGlobexIdx;
        }

        // Mark sessions as valid if we found any data
        dualSessionPV.rth.valid = (rthPeaks > 0 || rthValleys > 0);
        dualSessionPV.globex.valid = (globexPeaks > 0 || globexValleys > 0);

        // Set profile info from the found indices (for logging)
        if (foundRthIdx >= 0)
        {
            n_ACSIL::s_StudyProfileInformation rthInfo;
            if (sc.GetStudyProfileInformation(vbpStudyId, foundRthIdx, rthInfo) != 0)
            {
                dualSessionPV.rth.profileIndex = foundRthIdx;
                dualSessionPV.rth.startTime = rthInfo.m_StartDateTime;
                dualSessionPV.rth.endTime = rthInfo.m_EndDateTime;
                dualSessionPV.rth.sessionType = ProfileSessionType::RTH;
            }
        }
        if (foundGlobexIdx >= 0)
        {
            n_ACSIL::s_StudyProfileInformation gbxInfo;
            if (sc.GetStudyProfileInformation(vbpStudyId, foundGlobexIdx, gbxInfo) != 0)
            {
                dualSessionPV.globex.profileIndex = foundGlobexIdx;
                dualSessionPV.globex.startTime = gbxInfo.m_StartDateTime;
                dualSessionPV.globex.endTime = gbxInfo.m_EndDateTime;
                dualSessionPV.globex.sessionType = ProfileSessionType::GLOBEX;
            }
        }

        bool rthLoaded = dualSessionPV.rth.valid;
        bool globexLoaded = dualSessionPV.globex.valid;

        // Log results once on first load (note: API only returns current session due to SC bug)
        if (diagLevel >= 1 && (rthPeaks > 0 || rthValleys > 0 || globexPeaks > 0 || globexValleys > 0))
        {
            static bool loggedOnce = false;
            if (!loggedOnce || dualSessionPV.HasChanged())
            {
                SCString msg;
                msg.Format("Loaded (current session only): %s HVN=%d LVN=%d",
                    isCurrentlyRTH ? "RTH" : "GBX",
                    isCurrentlyRTH ? rthPeaks : globexPeaks,
                    isCurrentlyRTH ? rthValleys : globexValleys);
                LogVBP(sc, sc.Index, msg.GetChars(), false);
                loggedOnce = true;
            }
        }

        // Log if no peaks/valleys found for current session (throttled)
        static bool loggedNoPV = false;
        const bool noPVFound = (rthPeaks == 0 && rthValleys == 0 && globexPeaks == 0 && globexValleys == 0);
        if (noPVFound && diagLevel >= 1 && !loggedNoPV)
        {
            LogVBP(sc, sc.Index, "No peaks/valleys found - check VbP 'Draw Peaks/Valleys' setting", false);
            loggedNoPV = true;
        }

        // Log results (only when changed or diagLevel >= 1)
        if (diagLevel >= 1 && dualSessionPV.HasChanged())
        {
            SCString msg;

            // Profile dump table (only at diagLevel >= 2)
            if (diagLevel >= 2)
            {
                msg.Format("Profile dump: Scanned %d of %d profiles:", static_cast<int>(profileDumps.size()), numProfiles);
                LogVBP(sc, sc.Index, msg.GetChars(), false);

                for (const auto& d : profileDumps)
                {
                    int endHour, endMin, endSec;
                    d.endDT.GetTimeHMS(endHour, endMin, endSec);
                    int startYear, startMonth, startDay;
                    d.startDT.GetDateYMD(startYear, startMonth, startDay);

                    msg.Format("  ProfileIdx=%d | %04d-%02d-%02d %02d:%02d:%02d - %02d:%02d:%02d | %s%s",
                        d.idx, startYear, startMonth, startDay,
                        d.startHour, d.startMin, d.startSec,
                        endHour, endMin, endSec,
                        to_string(d.classification),
                        (d.idx == foundRthIdx ? " <- RTH" :
                         d.idx == foundGlobexIdx ? " <- GLOBEX" : ""));
                    LogVBP(sc, sc.Index, msg.GetChars(), false);
                }
            }

            // RTH summary
            if (dualSessionPV.rth.valid)
            {
                int sh, sm, ss, eh, em, es;
                dualSessionPV.rth.startTime.GetTimeHMS(sh, sm, ss);
                dualSessionPV.rth.endTime.GetTimeHMS(eh, em, es);
                msg.Format("RTH ProfileIndex=%d Start=%02d:%02d:%02d End=%02d:%02d:%02d HVN=%d LVN=%d",
                    dualSessionPV.rth.profileIndex, sh, sm, ss, eh, em, es,
                    static_cast<int>(dualSessionPV.rth.hvn.size()),
                    static_cast<int>(dualSessionPV.rth.lvn.size()));
                LogVBP(sc, sc.Index, msg.GetChars(), false);

                for (size_t i = 0; i < dualSessionPV.rth.hvn.size(); ++i)
                {
                    msg.Format("  RTH HVN[%d]: %.2f", static_cast<int>(i), dualSessionPV.rth.hvn[i]);
                    LogVBP(sc, sc.Index, msg.GetChars(), false);
                }
                for (size_t i = 0; i < dualSessionPV.rth.lvn.size(); ++i)
                {
                    msg.Format("  RTH LVN[%d]: %.2f", static_cast<int>(i), dualSessionPV.rth.lvn[i]);
                    LogVBP(sc, sc.Index, msg.GetChars(), false);
                }
            }

            // GLOBEX summary
            if (dualSessionPV.globex.valid)
            {
                int sh, sm, ss, eh, em, es;
                dualSessionPV.globex.startTime.GetTimeHMS(sh, sm, ss);
                dualSessionPV.globex.endTime.GetTimeHMS(eh, em, es);
                msg.Format("GLOBEX ProfileIndex=%d Start=%02d:%02d:%02d End=%02d:%02d:%02d HVN=%d LVN=%d",
                    dualSessionPV.globex.profileIndex, sh, sm, ss, eh, em, es,
                    static_cast<int>(dualSessionPV.globex.hvn.size()),
                    static_cast<int>(dualSessionPV.globex.lvn.size()));
                LogVBP(sc, sc.Index, msg.GetChars(), false);

                for (size_t i = 0; i < dualSessionPV.globex.hvn.size(); ++i)
                {
                    msg.Format("  GLOBEX HVN[%d]: %.2f", static_cast<int>(i), dualSessionPV.globex.hvn[i]);
                    LogVBP(sc, sc.Index, msg.GetChars(), false);
                }
                for (size_t i = 0; i < dualSessionPV.globex.lvn.size(); ++i)
                {
                    msg.Format("  GLOBEX LVN[%d]: %.2f", static_cast<int>(i), dualSessionPV.globex.lvn[i]);
                    LogVBP(sc, sc.Index, msg.GetChars(), false);
                }
            }

            dualSessionPV.MarkLogged();
        }

        return rthLoaded || globexLoaded;
    }

private:
    // NOTE: ComputeValueAreaFromMap and ComputeHvnLvnFromMap removed (Dec 2024)
    // VbP study is SSOT for VAH/VAL, SC's GetStudyPeakValleyLine() is SSOT for peaks/valleys

public:
};

// ============================================================================
// VBP CONTEXT HELPER (For MiniVP Integration)
// ============================================================================

// Get VbP context at a specific price level
// Uses the SessionVolumeProfile (which contains VbP data when native mode enabled)
// SSOT: Uses cached thresholds instead of recomputing per-call
inline AMT::VbPLevelContext GetVbPContextAtPrice(
    const SessionVolumeProfile& profile,
    double queryPrice,
    double tickSize,
    double hvnSigmaCoeff = 1.5,
    double lvnSigmaCoeff = 0.5)
{
    AMT::VbPLevelContext ctx;

    if (profile.volume_profile.empty() || tickSize <= 0.0 || !AMT::IsValidPrice(queryPrice))
        return ctx;

    ctx.valid = true;
    const int queryTick = static_cast<int>(AMT::PriceToTicks(queryPrice, tickSize));

    // --- Value Area position ---
    ctx.atPOC = std::abs(queryPrice - profile.session_poc) < tickSize * 0.5;
    ctx.insideValueArea = (queryPrice >= profile.session_val && queryPrice <= profile.session_vah);
    ctx.aboveVAH = (queryPrice > profile.session_vah);
    ctx.belowVAL = (queryPrice < profile.session_val);

    // --- Volume at this price ---
    auto it = profile.volume_profile.find(queryTick);
    if (it != profile.volume_profile.end())
    {
        ctx.volumeAtPrice = static_cast<double>(it->second.Volume);
    }

    // --- Calculate volume percentile (use cached maxVol if available) ---
    double maxVol = profile.cachedThresholds.valid ? profile.cachedThresholds.maxLevelVolume : 0.0;
    if (maxVol <= 0.0)
    {
        // Fallback: compute maxVol if cache not valid (should be rare)
        for (const auto& kv : profile.volume_profile)
        {
            maxVol = (std::max)(maxVol, static_cast<double>(kv.second.Volume));
        }
    }

    if (maxVol > 0.0)
    {
        ctx.volumePercentile = ctx.volumeAtPrice / maxVol;
    }

    // --- HVN/LVN classification using SSOT cached thresholds ---
    if (profile.cachedThresholds.valid)
    {
        // SSOT classification from cache
        ctx.classification.density = profile.cachedThresholds.ClassifyVolume(ctx.volumeAtPrice);

        // Set flags for special cases
        if (ctx.classification.density == AMT::VAPDensityClass::LOW &&
            ctx.volumeAtPrice > 0 && ctx.volumeAtPrice <= profile.cachedThresholds.mean * 0.3) {
            ctx.classification.flags = ctx.classification.flags | AMT::NodeFlags::SINGLE_PRINT;
        }

        // Sync legacy fields from SSOT classification
        ctx.SyncFromClassification();
    }
    else
    {
        // Fallback: compute inline if cache not valid (should be rare)
        const size_t numLevels = profile.volume_profile.size();
        if (numLevels > 0)
        {
            double totalVol = 0.0;
            for (const auto& kv : profile.volume_profile)
            {
                totalVol += kv.second.Volume;
            }
            const double mean = totalVol / numLevels;
            double variance = 0.0;
            for (const auto& kv : profile.volume_profile)
            {
                double diff = static_cast<double>(kv.second.Volume) - mean;
                variance += diff * diff;
            }
            const double stddev = std::sqrt(variance / numLevels);

            // SSOT: Use config coefficients instead of hardcoded 1.5/0.5
            const double hvnThreshold = mean + hvnSigmaCoeff * stddev;
            const double lvnThreshold = mean - lvnSigmaCoeff * stddev;

            // Set both SSOT classification and legacy fields
            if (ctx.volumeAtPrice > hvnThreshold) {
                ctx.classification.density = AMT::VAPDensityClass::HIGH;
            } else if (ctx.volumeAtPrice < lvnThreshold && ctx.volumeAtPrice > 0) {
                ctx.classification.density = AMT::VAPDensityClass::LOW;
            }
            ctx.SyncFromClassification();
        }
    }

    // --- Find nearest HVN ---
    double minHVNDist = 1e9;
    for (const double hvn : profile.session_hvn)
    {
        double dist = std::abs(queryPrice - hvn);
        if (dist < minHVNDist)
        {
            minHVNDist = dist;
            ctx.nearestHVN = hvn;
        }
    }
    ctx.distToHVNTicks = (minHVNDist < 1e8) ? (minHVNDist / tickSize) : 1e9;

    // --- Find nearest LVN ---
    double minLVNDist = 1e9;
    for (const double lvn : profile.session_lvn)
    {
        double dist = std::abs(queryPrice - lvn);
        if (dist < minLVNDist)
        {
            minLVNDist = dist;
            ctx.nearestLVN = lvn;
        }
    }
    ctx.distToLVNTicks = (minLVNDist < 1e8) ? (minLVNDist / tickSize) : 1e9;

    return ctx;
}

// ============================================================================
// PROFILE CLARITY COMPUTATION
// Stage 3: volumeProfileClarity with validity tracking
// Components: POC dominance (z-score), VA compactness, unimodality (HVN count)
// ============================================================================

// Context for profile clarity computation (maturity + baseline)
struct ProfileClarityContext {
    // --- Session Progress ---
    int sessionBars = 0;            // Bars into current session
    int sessionMinutes = 0;         // Minutes into current session

    // --- Total Volume (for maturity check) ---
    double sessionTotalVolume = 0.0;

    // --- Baseline Reference (optional) ---
    const AMT::HistoricalProfileBaseline* baseline = nullptr;  // nullptr = no baseline comparison
    bool isRTH = true;              // Determines which bucket timings to use

    // --- Progress Bucket (computed from sessionMinutes) ---
    AMT::ProgressBucket GetCurrentBucket() const {
        return AMT::GetProgressBucket(sessionMinutes);
    }
};

struct ProfileClarityResult
{
    float clarity = 0.0f;       // Final composite score [0, 1]
    bool valid = false;         // True if computation succeeded

    // Component scores - USE ACCESSORS FOR READS (direct access banned except assignment)
    float pocDominance_ = 0.0f;  // PRIVATE: use GetPocDominance()
    float vaCompactness = 0.0f;  // [0, 1] VA width vs profile range
    float unimodality = 0.0f;    // [0, 1] penalty for multiple peaks

    // Component validity flags (NO-FALLBACK POLICY)
    bool pocDominanceValid = false;  // z-score requires sufficient sample size

    // Raw inputs for diagnostics
    double pocVolume = 0.0;
    double meanVolume = 0.0;
    double stddevVolume = 0.0;
    int vaWidthTicks = 0;
    int profileRangeTicks = 0;
    int hvnCount = 0;
    int sampleSize = 0;  // Price levels with volume data

    // --- Maturity Gate Results ---
    AMT::ProfileMaturityResult maturity;          // Maturity check details
    bool profileMature = false;              // True if profile passed maturity gate

    // --- Baseline Context Results ---
    AMT::ProgressBucket currentBucket = AMT::ProgressBucket::BUCKET_15M;  // Current progress bucket

    // VA Width baseline
    double vaWidthPercentile = -1.0;         // Percentile vs historical baseline (-1 = unavailable)
    bool vaWidthPercentileValid = false;     // True if baseline comparison was computed (NO-FALLBACK POLICY)
    bool baselineReady = false;              // True if VA width baseline has enough samples
    size_t baselineSamples = 0;              // How many prior sessions in VA width baseline

    // POC Share (dominance) baseline
    double pocSharePercentile = -1.0;        // Percentile vs historical baseline (-1 = unavailable)
    bool pocSharePercentileValid = false;    // True if baseline comparison was computed (NO-FALLBACK POLICY)
    bool pocShareBaselineReady = false;      // True if POC share baseline has enough samples
    size_t pocShareBaselineSamples = 0;      // How many prior sessions in POC share baseline

    // Current POC share value (for baseline comparison)
    double currentPocShare = 0.0;            // volume_at_POC / total_volume [0, 1]
    bool currentPocShareValid = false;       // True if POC volume data was available

    // GUARDED ACCESSOR: asserts validity before returning dead-value field
    float GetPocDominance() const
    {
        assert(pocDominanceValid && "BUG: reading pocDominance without validity check");
        return pocDominance_;
    }
};

inline ProfileClarityResult ComputeVolumeProfileClarity(
    const SessionVolumeProfile& profile,
    double tickSize)
{
    ProfileClarityResult result;

    // Validity checks
    if (tickSize <= 0.0)
        return result;

    if (profile.volume_profile.size() < 5)
        return result;

    if (!profile.cachedThresholds.valid)
        return result;

    if (!AMT::IsValidPrice(profile.session_poc))
        return result;

    if (!AMT::IsValidPrice(profile.session_vah) || !AMT::IsValidPrice(profile.session_val))
        return result;

    if (profile.session_vah < profile.session_val)
        return result;

    const double mean = profile.cachedThresholds.mean;
    const double stddev = profile.cachedThresholds.stddev;
    const double maxVol = profile.cachedThresholds.maxLevelVolume;

    if (mean <= 0.0 || stddev <= 0.0 || maxVol <= 0.0)
        return result;

    // Find POC volume
    const int pocTick = static_cast<int>(AMT::PriceToTicks(profile.session_poc, tickSize));
    double pocVol = 0.0;

    auto it = profile.volume_profile.find(pocTick);
    if (it != profile.volume_profile.end())
    {
        pocVol = static_cast<double>(it->second.Volume);
    }
    else
    {
        // Try adjacent ticks (POC might be slightly off due to rounding)
        for (int offset = -1; offset <= 1; ++offset)
        {
            auto nearby = profile.volume_profile.find(pocTick + offset);
            if (nearby != profile.volume_profile.end())
            {
                if (static_cast<double>(nearby->second.Volume) > pocVol)
                    pocVol = static_cast<double>(nearby->second.Volume);
            }
        }
    }

    if (pocVol <= 0.0)
        return result;

    // Calculate profile range
    int minTick = INT_MAX, maxTick = INT_MIN;
    for (const auto& kv : profile.volume_profile)
    {
        if (kv.first < minTick) minTick = kv.first;
        if (kv.first > maxTick) maxTick = kv.first;
    }

    const int profileRangeTicks = maxTick - minTick + 1;
    if (profileRangeTicks < 3)
        return result;

    const int vahTick = static_cast<int>(AMT::PriceToTicks(profile.session_vah, tickSize));
    const int valTick = static_cast<int>(AMT::PriceToTicks(profile.session_val, tickSize));
    const int vaWidthTicks = vahTick - valTick + 1;

    if (vaWidthTicks < 1)
        return result;

    // Store diagnostics
    result.pocVolume = pocVol;
    result.meanVolume = mean;
    result.stddevVolume = stddev;
    result.vaWidthTicks = vaWidthTicks;
    result.profileRangeTicks = profileRangeTicks;
    result.hvnCount = static_cast<int>(profile.session_hvn.size());
    result.sampleSize = profile.cachedThresholds.sampleSize;

    // Compute POC share (dominance ratio) from VbP study data
    // POC share = volume_at_POC / total_profile_volume
    // Uses volumeAtPOC (the actual volume at VbP study's POC price)
    // Only valid if pocVolumeVerified confirms volumeAtPOC == maxLevelVolume
    {
        const double totalVol = profile.cachedThresholds.totalVolume;
        const double pocVol = profile.cachedThresholds.volumeAtPOC;

        // NO-FALLBACK POLICY: Only use POC share if we verified the volume source
        // If maxLevelVolume != volumeAtPOC, VbP may use smoothing/grouping rules
        // and our "POC share" assumption would be incorrect
        if (profile.cachedThresholds.valid &&
            profile.cachedThresholds.pocVolumeVerified &&
            totalVol > 0.0 && pocVol > 0.0)
        {
            result.currentPocShare = pocVol / totalVol;
            result.currentPocShareValid = true;
        }
        else
        {
            result.currentPocShare = 0.0;
            result.currentPocShareValid = false;
        }
    }

    // Component 1: POC Dominance (40% weight when valid)
    // Z-score baseline: mean/stddev from profile's own cachedThresholds
    // (self-referential: measures how POC stands out from same profile)
    // NO-FALLBACK POLICY: requires minimum sample size for statistical validity
    {
        constexpr int Z_SCORE_MIN_SAMPLES = 10;  // Minimum price levels for stable z-score
        constexpr double DOMINANCE_SIGMA_SCALE = 3.0;

        if (result.sampleSize >= Z_SCORE_MIN_SAMPLES)
        {
            const double zScore = (pocVol - mean) / stddev;
            const double rawDominance = zScore / DOMINANCE_SIGMA_SCALE;
            result.pocDominance_ = static_cast<float>(
                (std::max)(0.0, (std::min)(1.0, rawDominance)));
            result.pocDominanceValid = true;
        }
        else
        {
            // Insufficient sample - component EXCLUDED from blend
            // pocDominanceValid=false gates GetPocDominance() accessor
            result.pocDominance_ = 0.0f;  // Dead value - accessor asserts validity
            result.pocDominanceValid = false;
        }
    }

    // Component 2: VA Compactness (35% weight)
    {
        constexpr double COMPACTNESS_TARGET_RATIO = 0.70;
        const double vaRatio = static_cast<double>(vaWidthTicks) / profileRangeTicks;
        const double rawCompactness = 1.0 - (vaRatio / COMPACTNESS_TARGET_RATIO);
        result.vaCompactness = static_cast<float>(
            (std::max)(0.0, (std::min)(1.0, rawCompactness)));
    }

    // Component 3: Unimodality (25% weight)
    {
        constexpr int MAX_PENALTY_PEAKS = 3;
        const int hvnCount = static_cast<int>(profile.session_hvn.size());
        const int excessPeaks = (std::max)(0, hvnCount - 1);
        const double penaltyRatio = static_cast<double>(excessPeaks) / MAX_PENALTY_PEAKS;
        const double rawUnimodality = 1.0 - (std::min)(1.0, penaltyRatio);
        result.unimodality = static_cast<float>(rawUnimodality);
    }

    // Composite score (with renormalization for missing components)
    {
        constexpr float W_DOMINANCE = 0.40f;
        constexpr float W_COMPACTNESS = 0.35f;
        constexpr float W_UNIMODALITY = 0.25f;

        float score = 0.0f;
        float totalWeight = 0.0f;

        // POC Dominance: only included if sample size sufficient
        if (result.pocDominanceValid)
        {
            score += W_DOMINANCE * result.GetPocDominance();  // Accessor asserts validity
            totalWeight += W_DOMINANCE;
        }

        // VA Compactness: always included (profile-derived, no baseline)
        score += W_COMPACTNESS * result.vaCompactness;
        totalWeight += W_COMPACTNESS;

        // Unimodality: always included (HVN count, no baseline)
        score += W_UNIMODALITY * result.unimodality;
        totalWeight += W_UNIMODALITY;

        // Renormalize
        if (totalWeight > 0.0f)
        {
            result.clarity = (std::max)(0.0f, (std::min)(1.0f, score / totalWeight));
        }
        else
        {
            result.clarity = 0.0f;
        }
        result.valid = true;
    }

    return result;
}

// Overload with maturity + baseline context
// Adds maturity gating and progress-conditioned baseline comparison
inline ProfileClarityResult ComputeVolumeProfileClarity(
    const SessionVolumeProfile& profile,
    double tickSize,
    const ProfileClarityContext& ctx)
{
    // First compute base clarity using the original function
    ProfileClarityResult result = ComputeVolumeProfileClarity(profile, tickSize);

    // --- Step 0: Determine Progress Bucket early (needed for maturity check) ---
    result.currentBucket = ctx.GetCurrentBucket();

    // --- Step 1: Profile Maturity Gate ---
    // Use baseline-aware version for progress-conditioned volume sufficiency
    const int priceLevels = static_cast<int>(profile.volume_profile.size());
    result.maturity = AMT::CheckProfileMaturityWithBaseline(
        priceLevels,
        ctx.sessionTotalVolume,
        ctx.sessionBars,
        ctx.sessionMinutes,
        result.currentBucket,
        ctx.baseline);
    result.profileMature = result.maturity.isMature;

    // If profile is not mature, mark result as invalid
    // (even if base computation succeeded, maturity gate must pass)
    if (!result.profileMature && result.valid)
    {
        result.valid = false;
        // Keep the computed values for diagnostics, but valid=false gates usage
    }

    // --- Step 2: VA Width Baseline Comparison (if baseline available) ---
    if (ctx.baseline != nullptr && result.vaWidthTicks > 0)
    {
        const AMT::ProgressBucket bucket = result.currentBucket;

        // Check if baseline has enough samples for this bucket
        result.baselineReady = ctx.baseline->IsReady(bucket);
        result.baselineSamples = ctx.baseline->vaWidthTicks[static_cast<int>(bucket)].size();

        if (result.baselineReady)
        {
            // Get percentile rank of current VA width vs historical baseline at same bucket
            result.vaWidthPercentile = ctx.baseline->GetVAWidthPercentile(
                bucket, static_cast<double>(result.vaWidthTicks));
            result.vaWidthPercentileValid = (result.vaWidthPercentile >= 0.0);
        }
        else
        {
            result.vaWidthPercentile = -1.0;  // Unavailable
            result.vaWidthPercentileValid = false;
        }
    }

    // --- Step 3: POC Share (Dominance) Baseline Comparison ---
    // NO-FALLBACK POLICY: Only compare if both current POC share and baseline are available
    if (ctx.baseline != nullptr && result.currentPocShareValid)
    {
        const AMT::ProgressBucket bucket = result.currentBucket;

        // Check if POC share baseline has enough samples for this bucket
        result.pocShareBaselineReady = ctx.baseline->IsPocShareBaselineReady(bucket);
        result.pocShareBaselineSamples = ctx.baseline->pocShare[static_cast<int>(bucket)].size();

        if (result.pocShareBaselineReady)
        {
            // Get percentile rank of current POC share vs historical baseline at same bucket
            result.pocSharePercentile = ctx.baseline->GetPOCSharePercentile(
                bucket, result.currentPocShare);
            result.pocSharePercentileValid = (result.pocSharePercentile >= 0.0);
        }
        else
        {
            result.pocSharePercentile = -1.0;  // Unavailable
            result.pocSharePercentileValid = false;
        }
    }
    else
    {
        // No baseline or no current POC share data
        result.pocShareBaselineReady = false;
        result.pocSharePercentile = -1.0;
        result.pocSharePercentileValid = false;
    }

    return result;
}

// ============================================================================
// TPO ACCEPTANCE COMPUTATION
// Stage 3: tpoAcceptance with validity tracking
// Components: VA balance (POC position), TPO-VBP alignment, VA compactness
// ============================================================================

// Config constants for TPO acceptance thresholds
constexpr int TPO_ALIGNMENT_MAX_DIVERGENCE_TICKS = 12;  // 3 ES points
constexpr int TPO_COMPACTNESS_MAX_WIDTH_TICKS = 100;    // 25 ES points

struct TPOAcceptanceResult
{
    float acceptance = 0.0f;    // Final composite score [0, 1]
    bool valid = false;         // True if computation succeeded

    // Component scores - USE ACCESSORS FOR READS (direct access banned except assignment)
    float vaBalance = 0.0f;         // [0, 1] POC position symmetry within VA
    float tpoVbpAlignment_ = 0.0f;  // PRIVATE: use GetTpoVbpAlignment()
    float vaCompactness = 0.0f;     // [0, 1] how narrow VA is

    // Component validity flags (no-fallback policy)
    bool alignmentValid = false;    // True if VBP POC was available for alignment calc

    // Raw inputs for diagnostics
    double tpoPOC = 0.0;
    double tpoVAH = 0.0;
    double tpoVAL = 0.0;
    double vbpPOC = 0.0;            // May be 0 if VBP unavailable
    int vaWidthTicks = 0;
    int pocDivergenceTicks = 0;

    // GUARDED ACCESSOR: asserts validity before returning dead-value field
    // This is the ONLY allowed read path for tpoVbpAlignment
    float GetTpoVbpAlignment() const
    {
        assert(alignmentValid && "BUG: reading tpoVbpAlignment without validity check");
        return tpoVbpAlignment_;
    }
};

inline TPOAcceptanceResult ComputeTPOAcceptance(
    double tpoPOC,
    double tpoVAH,
    double tpoVAL,
    double vbpPOC,
    double tickSize,
    int alignmentMaxDivergenceTicks = TPO_ALIGNMENT_MAX_DIVERGENCE_TICKS,
    int compactnessMaxWidthTicks = TPO_COMPACTNESS_MAX_WIDTH_TICKS)
{
    TPOAcceptanceResult result;

    // Validity checks
    if (tickSize <= 0.0)
        return result;

    if (!AMT::IsValidPrice(tpoPOC) || tpoPOC <= 0.0)
        return result;

    if (!AMT::IsValidPrice(tpoVAH) || tpoVAH <= 0.0)
        return result;

    if (!AMT::IsValidPrice(tpoVAL) || tpoVAL <= 0.0)
        return result;

    if (tpoVAH <= tpoVAL)
        return result;

    // Store raw inputs for diagnostics
    result.tpoPOC = tpoPOC;
    result.tpoVAH = tpoVAH;
    result.tpoVAL = tpoVAL;
    result.vbpPOC = vbpPOC;

    const double vaWidth = tpoVAH - tpoVAL;
    result.vaWidthTicks = static_cast<int>(vaWidth / tickSize);

    // Component 1: VA Balance (40% weight)
    {
        const double pocRelPos = (tpoPOC - tpoVAL) / vaWidth;
        const double clampedPos = (std::max)(0.0, (std::min)(1.0, pocRelPos));
        const double distFromCenter = std::abs(clampedPos - 0.5) * 2.0;
        result.vaBalance = static_cast<float>(1.0 - distFromCenter);
    }

    // Component 2: TPO-VBP Alignment (35% weight when valid)
    // NO-FALLBACK POLICY: If VBP unavailable, alignment EXCLUDED from blend
    {
        const double thresholdTicks = static_cast<double>(alignmentMaxDivergenceTicks);

        if (AMT::IsValidPrice(vbpPOC) && vbpPOC > 0.0)
        {
            const double divergence = std::abs(tpoPOC - vbpPOC);
            const double divergenceTicks = divergence / tickSize;
            result.pocDivergenceTicks = static_cast<int>(divergenceTicks);

            const double rawAlignment = 1.0 - (divergenceTicks / thresholdTicks);
            result.tpoVbpAlignment_ = static_cast<float>(
                (std::max)(0.0, (std::min)(1.0, rawAlignment)));
            result.alignmentValid = true;
        }
        else
        {
            // VBP unavailable - alignment EXCLUDED (no fallback)
            // alignmentValid=false gates GetTpoVbpAlignment() accessor
            result.tpoVbpAlignment_ = 0.0f;  // Dead value - accessor asserts validity
            result.alignmentValid = false;
            result.pocDivergenceTicks = -1;
        }
    }

    // Component 3: VA Compactness (25% weight)
    {
        const double maxWidthTicks = static_cast<double>(compactnessMaxWidthTicks);
        const double vaWidthTicks = static_cast<double>(result.vaWidthTicks);
        const double rawCompactness = 1.0 - (vaWidthTicks / maxWidthTicks);

        result.vaCompactness = static_cast<float>(
            (std::max)(0.0, (std::min)(1.0, rawCompactness)));
    }

    // Composite acceptance score (with renormalization for missing components)
    {
        constexpr float W_BALANCE = 0.40f;
        constexpr float W_ALIGNMENT = 0.35f;
        constexpr float W_COMPACTNESS = 0.25f;

        float score = 0.0f;
        float totalWeight = 0.0f;

        // Balance: always included
        score += W_BALANCE * result.vaBalance;
        totalWeight += W_BALANCE;

        // Alignment: only included if VBP POC was available
        if (result.alignmentValid)
        {
            score += W_ALIGNMENT * result.GetTpoVbpAlignment();  // Accessor asserts validity
            totalWeight += W_ALIGNMENT;
        }

        // Compactness: always included
        score += W_COMPACTNESS * result.vaCompactness;
        totalWeight += W_COMPACTNESS;

        // Renormalize
        if (totalWeight > 0.0f)
        {
            result.acceptance = (std::max)(0.0f, (std::min)(1.0f, score / totalWeight));
        }
        else
        {
            result.acceptance = 0.0f;
        }

        result.valid = true;
    }

    return result;
}

#endif // AMT_VOLUMEPROFILE_H
