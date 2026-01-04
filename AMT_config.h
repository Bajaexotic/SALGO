// ============================================================================
// AMT_Config.h
// Configuration structures and instrument profiles
// ============================================================================

#ifndef AMT_CONFIG_H
#define AMT_CONFIG_H

#include "amt_core.h"
#include <string>
#include <algorithm>
#include <cmath>
#include <cassert>

namespace AMT {

// ============================================================================
// CANONICAL TICK MATH (SSOT for price <-> tick conversion)
// All comparison/threshold logic MUST use these functions.
// Display/logging may read cached anchorPrice but decisions use ticks.
// ============================================================================

/**
 * Policy: ROUND_NEAREST
 * We use llround (round half away from zero) for price-to-tick conversion.
 * This is appropriate for tick-aligned prices from Sierra Chart.
 */

/**
 * Convert price to ticks (authoritative conversion).
 * @param price The price to convert
 * @param tickSize The tick size (must be > 0)
 * @return Tick count as long long
 */
inline long long PriceToTicks(double price, double tickSize) {
    assert(tickSize > 0.0 && "PriceToTicks: tickSize must be positive");
    return static_cast<long long>(std::llround(price / tickSize));
}

/**
 * Convert ticks to price (derived value).
 * @param ticks The tick count
 * @param tickSize The tick size (must be > 0)
 * @return Price as double
 */
inline double TicksToPrice(long long ticks, double tickSize) {
    assert(tickSize > 0.0 && "TicksToPrice: tickSize must be positive");
    return static_cast<double>(ticks) * tickSize;
}

/**
 * Check if a price is tick-aligned (debug utility).
 * Returns true if price is within epsilon of a tick boundary.
 * Use this to validate inputs that SHOULD be tick-aligned.
 */
inline bool IsTickAligned(double price, double tickSize, double epsilon = 1e-9) {
    if (tickSize <= 0.0) return false;
    double ticks = price / tickSize;
    double rounded = std::round(ticks);
    return std::fabs(ticks - rounded) < epsilon;
}

/**
 * Assert tick alignment (debug builds only).
 * Call this before PriceToTicks() when input MUST be tick-aligned.
 */
inline void AssertTickAligned([[maybe_unused]] double price,
                               [[maybe_unused]] double tickSize,
                               [[maybe_unused]] const char* context = nullptr) {
#ifdef _DEBUG
    if (!IsTickAligned(price, tickSize)) {
        // In debug: log warning but don't crash (price may be derived value)
        // If you want hard failure, uncomment the assert below
        // assert(false && "Price not tick-aligned");
    }
#endif
}

// ============================================================================
// SYMBOL FINGERPRINT (Invalidation Guard)
// Used to detect when cached tick-based values must be invalidated.
// This is NOT the tickSize SSOT - it's a guard for detecting changes.
// ============================================================================

/**
 * SymbolFingerprint: Captures symbol identity for cache invalidation.
 *
 * Usage:
 *   1. Store a fingerprint alongside tick-based caches (zones, levels)
 *   2. On each update, compare current fingerprint to stored
 *   3. If mismatch: invalidate caches and reset
 *
 * This prevents stale tickSize from corrupting tick arithmetic after
 * symbol change, contract roll, or chart reconfiguration.
 */
struct SymbolFingerprint {
    std::string symbol;
    double tickSize = 0.0;

    /**
     * Check if fingerprint matches another.
     * Returns true if same symbol AND same tickSize.
     */
    bool Matches(const SymbolFingerprint& other) const {
        if (symbol != other.symbol) return false;
        // Use small epsilon for tickSize comparison (floating point)
        return std::fabs(tickSize - other.tickSize) < 1e-10;
    }

    /**
     * Check if fingerprint is valid (non-empty symbol, positive tickSize).
     */
    bool IsValid() const {
        return !symbol.empty() && tickSize > 0.0;
    }

    /**
     * Update fingerprint from current symbol context.
     * Returns true if fingerprint changed (cache invalidation needed).
     */
    bool UpdateFrom(const std::string& newSymbol, double newTickSize) {
        SymbolFingerprint newFp{newSymbol, newTickSize};
        if (Matches(newFp)) {
            return false;  // No change
        }
        symbol = newSymbol;
        tickSize = newTickSize;
        return true;  // Changed - caller should invalidate caches
    }

    void Reset() {
        symbol.clear();
        tickSize = 0.0;
    }
};

// ============================================================================
// BASELINE MINIMUM SAMPLES (No-Fallback Contract)
// ============================================================================
// Defines minimum sample counts for each baseline model type before outputs
// are considered READY. Below these thresholds, consumers MUST treat outputs
// as INVALID and set *Valid=false - no fallback values like 50.0 or 1.0.
//
// Model types:
//   ROBUST_CONTINUOUS: Heavy-tailed rates (vol/sec, bar_range, etc.)
//   BOUNDED_RATIO: Metrics in [0,1] or [-1,1] (delta_pct, session_delta_pct)
//   POSITIVE_SKEW: Non-negative magnitudes (depth_mass_core, stack_rate)
//   COUNT_MODEL: Discrete counts (trades_sec, time_in_zone)
// ============================================================================

namespace BaselineMinSamples {
    // Model-type thresholds
    constexpr size_t ROBUST_CONTINUOUS = 20;   // vol_sec, total_vol, max_delta, bar_range
    constexpr size_t BOUNDED_RATIO = 10;       // delta_pct, session_delta_pct
    constexpr size_t POSITIVE_SKEW = 10;       // depth_mass_core, depth_mass_halo, stack/pull_rate
    constexpr size_t COUNT_MODEL = 10;         // trades_sec, time_in_zone

    // Metric-specific overrides (if different from model default)
    constexpr size_t VOL_SEC = ROBUST_CONTINUOUS;
    constexpr size_t TOTAL_VOL = ROBUST_CONTINUOUS;
    constexpr size_t DELTA_PCT = BOUNDED_RATIO;
    constexpr size_t SESSION_DELTA_PCT = BOUNDED_RATIO;
    constexpr size_t MAX_DELTA = ROBUST_CONTINUOUS;
    constexpr size_t TRADES_SEC = COUNT_MODEL;
    constexpr size_t DEPTH_MASS_CORE = POSITIVE_SKEW;
    constexpr size_t DEPTH_MASS_HALO = POSITIVE_SKEW;
    constexpr size_t STACK_RATE = POSITIVE_SKEW;
    constexpr size_t PULL_RATE = POSITIVE_SKEW;
    constexpr size_t BAR_RANGE = ROBUST_CONTINUOUS;
    constexpr size_t ESCAPE_VELOCITY = ROBUST_CONTINUOUS;
    constexpr size_t TIME_IN_ZONE = COUNT_MODEL;

    // Facilitation requires both volume and range baselines
    constexpr size_t FACILITATION = ROBUST_CONTINUOUS;

    // Liquidity (DOM-derived) has lower threshold due to limited live data
    constexpr size_t LIQUIDITY = 10;
}

// ============================================================================
// ZONE POSTURE FLAGS
// Controls which zone families are active. TPO disabled by design.
// ============================================================================

/**
 * ZonePosture: Defines which zone families are instantiated.
 *
 * Current posture: VBP + PRIOR + STRUCTURE (no TPO)
 * - VBP: Current session profile zones (POC/VAH/VAL)
 * - PRIOR: Prior session reference zones (PRIOR_POC/VAH/VAL)
 * - STRUCTURE: Dynamic extrema (SESSION_HIGH/LOW, IB_HIGH/LOW)
 * - TPO: Disabled (TPO_* zones are not created)
 */
struct ZonePosture {
    bool enableVBP = true;        // VPB_POC, VPB_VAH, VPB_VAL
    bool enablePrior = true;      // PRIOR_POC, PRIOR_VAH, PRIOR_VAL
    bool enableTPO = false;       // TPO_* zones (DISABLED by design)
    bool enableStructure = true;  // SESSION_HIGH/LOW, IB_HIGH/LOW

    // Structure tracking mode:
    // If false: structure levels are tracked/logged but NOT created as zones
    // If true: structure levels ARE created as zones (enables engagement tracking)
    bool createStructureZones = false;  // Default: track only, don't create zones

    /**
     * Check if a zone type is allowed by current posture.
     */
    bool IsZoneTypeAllowed(ZoneType type) const {
        switch (type) {
            case ZoneType::VPB_POC:
            case ZoneType::VPB_VAH:
            case ZoneType::VPB_VAL:
                return enableVBP;

            case ZoneType::PRIOR_POC:
            case ZoneType::PRIOR_VAH:
            case ZoneType::PRIOR_VAL:
                return enablePrior;

            case ZoneType::TPO_POC:
            case ZoneType::TPO_VAH:
            case ZoneType::TPO_VAL:
                return enableTPO;  // Always false in current posture

            case ZoneType::IB_HIGH:
            case ZoneType::IB_LOW:
            case ZoneType::SESSION_HIGH:
            case ZoneType::SESSION_LOW:
                return enableStructure && createStructureZones;

            case ZoneType::VWAP:
                return true;  // Always allowed

            default:
                return false;
        }
    }

    /**
     * Log posture on init (one-time diagnostic).
     */
    std::string ToString() const {
        std::string s = "Posture: VBP=";
        s += enableVBP ? "ON" : "OFF";
        s += " PRIOR=";
        s += enablePrior ? "ON" : "OFF";
        s += " TPO=";
        s += enableTPO ? "ON" : "OFF";
        s += " STRUCT=";
        s += enableStructure ? "ON" : "OFF";
        if (enableStructure) {
            s += createStructureZones ? "(zones)" : "(track-only)";
        }
        return s;
    }
};

// Global posture instance (compile-time default)
inline ZonePosture g_zonePosture;

// ============================================================================
// ZONE CONFIGURATION
// ============================================================================

/**
 * ZoneConfig: Per-session configuration for zone behavior
 * Can be adjusted dynamically based on volatility, session, etc.
 */
struct ZoneConfig {
    // ========================================================================
    // Instrument Properties (SSOT - set at initialization)
    // ========================================================================
    double tickSize = 0.25;       // Tick size for anchor math (ES default)

    // ========================================================================
    // Distance Thresholds (base values, adjusted by volatility scalar)
    // ========================================================================
    int baseCoreTicks = 3;        // ±3 ticks for core (ES default)
    int baseHaloTicks = 8;        // ±8 ticks for halo (ES default)
    int inactiveThresholdBars = 50; // Bars away before INACTIVE

    // Volatility adjustment (updated dynamically from ATR)
    double volatilityScalar = 1.0;
    
    // Dynamic widths (base × volatility scalar)
    // Note: Use (std::max) to avoid Windows max macro conflict
    int GetCoreWidth() const {
        int width = static_cast<int>(baseCoreTicks * volatilityScalar);
        return (std::max)(width, 2);  // Minimum 2 ticks
    }

    int GetHaloWidth() const {
        int width = static_cast<int>(baseHaloTicks * volatilityScalar);
        return (std::max)(width, 5);  // Minimum 5 ticks
    }
    
    // ========================================================================
    // Engagement Criteria
    // ========================================================================
    int acceptanceMinBars = 3;          // Min bars for acceptance
    double acceptanceVolRatio = 1.3;    // Min volume ratio for acceptance
    int failedAuctionMaxBars = 12;      // Max bars outside for failed auction (~30 min)
    int failedAuctionMaxSeconds = 1800; // Max seconds (30 minutes)
    
    // ========================================================================
    // Volume Thresholds (SSOT - sigma-based classification)
    // ========================================================================
    // Primary SSOT method: sigma-based with configurable coefficients
    double hvnSigmaCoeff = 1.5;         // HVN = mean + 1.5σ
    double lvnSigmaCoeff = 0.5;         // LVN = mean - 0.5σ
    int minProfileLevels = 10;          // Minimum sample size for valid classification

    // Legacy ratio thresholds (for diagnostic comparison only, not used for decisions)
    double hvnThreshold = 1.5;          // Volume ratio for HVN (deprecated - use sigma)
    double lvnThreshold = 0.5;          // Volume ratio for LVN (deprecated - use sigma)
    double singlePrintThreshold = 0.3;  // Volume ratio for single print

    // Intra-session refresh control
    int hvnLvnRefreshIntervalBars = 25; // Bars between HVN/LVN recomputation
    int hvnConfirmationBars = 3;        // Bars to confirm new HVN candidate
    int hvnDemotionBars = 5;            // Bars to demote existing HVN

    // Cluster segmentation
    int maxClusterGapTicks = 2;         // Adjacent HVN within 2 ticks merge into cluster

    // ========================================================================
    // Micro-Window HVN/LVN (MiniVP/MicroAuction derived features)
    // ========================================================================
    // These are for the probe micro-window, NOT session-level peaks/valleys
    // Session HVN/LVN SSOT remains Sierra Chart's GetStudyPeakValleyLine()
    int microNodeTolTicks = 3;          // Tolerance for "near micro HVN/LVN" classification
                                         // Conservative default: 3 ticks (same as baseCoreTicks)
                                         // Used only as tie-breaker, not primary decision

    // ========================================================================
    // Delta Thresholds
    // ========================================================================
    double buyingNodeThreshold = 0.3;   // Delta ratio > 0.3 for buying node
    double sellingNodeThreshold = -0.3; // Delta ratio < -0.3 for selling node
    
    // ========================================================================
    // Defense/Aggression Thresholds
    // ========================================================================
    double defenseVolRatio = 1.5;       // Volume ratio for responsive defense
    double lowVolRejectRatio = 0.7;     // Volume ratio for low-vol reject
    double aggressionHighThreshold = 0.7; // Aggression ratio for initiative
    double aggressionLowThreshold = 0.3;  // Aggression ratio for responsive
    
    // ========================================================================
    // Touch Tracking
    // ========================================================================
    int minBarsBetweenTouches = 3;      // Hysteresis for touch counting
    
    // ========================================================================
    // Strength Decay
    // ========================================================================
    double touchDecayFactor = 0.2;      // Decay per touch: 1/(1 + 0.2*touches)
    double ageDecayBars = 300.0;        // Age decay: e^(-bars/300)
    
    // ========================================================================
    // Strength Tier Thresholds
    // ========================================================================
    double strongThreshold = 1.2;       // Score > 1.2 = STRONG
    double moderateThreshold = 0.8;     // Score 0.8-1.2 = MODERATE
    double weakThreshold = 0.5;         // Score 0.5-0.8 = WEAK
    
    // ========================================================================
    // Cluster Detection
    // ========================================================================
    int clusterMinWidth = 3;            // Min ticks for HVN cluster
    
    // ========================================================================
    // VA Region Boundaries
    // ========================================================================
    double upperVAThreshold = 0.70;     // Top 30% of VA
    double lowerVAThreshold = 0.30;     // Bottom 30% of VA

    // ========================================================================
    // Phase Detection Thresholds
    // ========================================================================
    int nearExtremeTicks = 3;           // Ticks from session extreme to be "near"
    int extremeUpdateWindowBars = 5;    // Bars window for "new extreme recently"
    double trendingDistanceRatio = 0.8; // Distance from POC as ratio of VA range

    // Phase System v2 parameters
    int boundaryToleranceTicks = 1;     // Ticks from VAH/VAL to be "at boundary"
    int failedAuctionRecencyBars = 10;  // Bars window for FAILED_AUCTION regime
    int directionalAfterglowBars = 30;  // Bars window for wasDirectionalRecently (PULLBACK gate)
    int approachingPOCLookback = 2;     // Consecutive bars of contracting dPOC for approachingPOC

    // AMT Acceptance/Regime parameters
    int acceptanceClosesRequired = 3;   // Consecutive closes outside VA (beyond tolerance) to confirm IMBALANCE

    // ========================================================================
    // Market State Estimator Tunables (Session-Gated Prior Influence)
    // ========================================================================
    // Session-only sufficiency: minimum bars before confirmedState can be non-UNDEFINED
    int marketStateMinSessionBars = 20;  // Session must have this many bars for valid state

    // Hysteresis confirmation: bars of consistent rawState to change confirmedState
    int marketStateConfirmationBars = 5;

    // Confirmation margin: extra evidence beyond 50% for state change
    // e.g., 0.1 means need 60% (50% + 10%) of session evidence favoring new state
    double marketStateConfirmationMargin = 0.1;

    // Prior influence: pseudo-count weight for historical prior
    // Decay: priorInfluence = priorMass / (sessionBars + priorMass)
    double marketStatePriorMass = 30.0;

    // Prior update EWMA factor: how much new session outcome influences prior
    // newPrior = (1 - inertia) * sessionOutcome + inertia * oldPrior
    // Higher inertia = slower prior adaptation
    double marketStatePriorInertia = 0.8;

    // Minimum session quality for prior update
    // Session must have at least this many bars to update the prior
    int marketStatePriorUpdateMinBars = 100;

    // ========================================================================
    // Facilitation Classification Thresholds (Percentile-Based)
    // ========================================================================
    // Uses quartile boundaries (25/75) consistent with RollingDist.isExtreme()
    // which uses 2.5 MAD ≈ ~1% tails. Quartiles are less extreme, appropriate
    // for facilitation (not anomaly detection).
    //
    // Semantic justification:
    //   - 75th percentile = upper quartile (high relative to session)
    //   - 25th percentile = lower quartile (low relative to session)
    //   - 10th percentile = extreme tail (consistent with extreme event detection)
    double facilHighPctl = 75.0;    // Upper quartile for high volume/range
    double facilLowPctl = 25.0;     // Lower quartile for low volume/range
    double facilExtremePctl = 10.0; // Extreme tail for failed auction
    int facilMinSamples = 20;       // Minimum baseline samples for valid classification

    // ========================================================================
    // DELTA SEMANTIC CONTRACT (Dec 2024 fix)
    // ========================================================================
    // TWO SEPARATE METRICS for different purposes:
    //
    // 1. deltaConsistency: AGGRESSOR FRACTION [0,1] where 0.5=neutral
    //    Formula: 0.5 + 0.5 * deltaPct  (equivalent to AskVol / TotalVol)
    //    Thresholds: >0.7 = extreme buying (70%+ at ask)
    //                <0.3 = extreme selling (70%+ at bid)
    //    Used for: isExtremeDeltaBar, barDeltaPositive, side classification
    //
    // 2. deltaStrength: MAGNITUDE [0,1] where 0=neutral, 1=max one-sided
    //    Formula: |deltaPct|
    //    Used for: confidence scoring (direction-agnostic signal strength)
    //
    // THIN-BAR HANDLING:
    //    Bars with volume < deltaMinVolAbs get:
    //    - deltaConsistency = 0.5 (neutral fraction)
    //    - deltaStrength = 0.0 (no signal)
    //    - Both marked invalid (prevents false extreme flags)
    // ========================================================================
    double deltaMinVolAbs = 20.0;     // Absolute floor: bars < 20 contracts are thin
    double deltaMinVolFrac = 0.25;    // Reserved for future adaptive floor
};

// ============================================================================
// INSTRUMENT PROFILES
// ============================================================================

/**
 * InstrumentProfile: Instrument-specific configuration
 * Pre-configured settings for common futures contracts
 */
struct InstrumentProfile {
    std::string symbol;
    double tickSize;
    double tickValue;
    
    // Base zone widths (before volatility adjustment)
    int baseCoreTicks;
    int baseHaloTicks;
    
    // Volume thresholds
    double hvnThreshold;
    double lvnThreshold;
    
    // Engagement criteria
    int acceptanceMinBars;
    double acceptanceVolRatio;
    
    // Session parameters
    int ibLengthMinutes;  // Initial Balance period (typically 60)
};

/**
 * ES (E-mini S&P 500) profile
 */
inline InstrumentProfile GetProfileES() {
    InstrumentProfile p;
    p.symbol = "ES";
    p.tickSize = 0.25;
    p.tickValue = 12.50;
    p.baseCoreTicks = 3;      // 0.75 points
    p.baseHaloTicks = 8;      // 2.0 points
    p.hvnThreshold = 1.5;
    p.lvnThreshold = 0.5;
    p.acceptanceMinBars = 3;
    p.acceptanceVolRatio = 1.3;
    p.ibLengthMinutes = 60;
    return p;
}

/**
 * MES (Micro E-mini S&P 500) profile
 * Same price levels as ES, smaller contract
 */
inline InstrumentProfile GetProfileMES() {
    InstrumentProfile p;
    p.symbol = "MES";
    p.tickSize = 0.25;
    p.tickValue = 1.25;
    p.baseCoreTicks = 3;      // Same as ES
    p.baseHaloTicks = 8;
    p.hvnThreshold = 1.5;
    p.lvnThreshold = 0.5;
    p.acceptanceMinBars = 3;
    p.acceptanceVolRatio = 1.3;
    p.ibLengthMinutes = 60;
    return p;
}

/**
 * NQ (E-mini NASDAQ 100) profile
 * More volatile, wider zones
 */
inline InstrumentProfile GetProfileNQ() {
    InstrumentProfile p;
    p.symbol = "NQ";
    p.tickSize = 0.25;
    p.tickValue = 5.00;
    p.baseCoreTicks = 5;      // 1.25 points (more volatile)
    p.baseHaloTicks = 12;     // 3.0 points
    p.hvnThreshold = 1.5;
    p.lvnThreshold = 0.5;
    p.acceptanceMinBars = 3;
    p.acceptanceVolRatio = 1.3;
    p.ibLengthMinutes = 60;
    return p;
}

/**
 * MNQ (Micro E-mini NASDAQ 100) profile
 */
inline InstrumentProfile GetProfileMNQ() {
    InstrumentProfile p;
    p.symbol = "MNQ";
    p.tickSize = 0.25;
    p.tickValue = 0.50;
    p.baseCoreTicks = 5;      // Same as NQ
    p.baseHaloTicks = 12;
    p.hvnThreshold = 1.5;
    p.lvnThreshold = 0.5;
    p.acceptanceMinBars = 3;
    p.acceptanceVolRatio = 1.3;
    p.ibLengthMinutes = 60;
    return p;
}

/**
 * YM (E-mini Dow) profile
 * Much larger tick value, wider zones
 */
inline InstrumentProfile GetProfileYM() {
    InstrumentProfile p;
    p.symbol = "YM";
    p.tickSize = 1.0;
    p.tickValue = 5.00;
    p.baseCoreTicks = 3;      // 3 points
    p.baseHaloTicks = 8;      // 8 points
    p.hvnThreshold = 1.5;
    p.lvnThreshold = 0.5;
    p.acceptanceMinBars = 3;
    p.acceptanceVolRatio = 1.3;
    p.ibLengthMinutes = 60;
    return p;
}

/**
 * RTY (E-mini Russell 2000) profile
 */
inline InstrumentProfile GetProfileRTY() {
    InstrumentProfile p;
    p.symbol = "RTY";
    p.tickSize = 0.10;
    p.tickValue = 5.00;
    p.baseCoreTicks = 8;      // 0.8 points
    p.baseHaloTicks = 20;     // 2.0 points
    p.hvnThreshold = 1.5;
    p.lvnThreshold = 0.5;
    p.acceptanceMinBars = 3;
    p.acceptanceVolRatio = 1.3;
    p.ibLengthMinutes = 60;
    return p;
}

/**
 * Get profile by symbol (auto-detect)
 */
inline InstrumentProfile GetProfile(const std::string& symbol) {
    if (symbol.find("ES") != std::string::npos && symbol.find("MES") == std::string::npos) {
        return GetProfileES();
    } else if (symbol.find("MES") != std::string::npos) {
        return GetProfileMES();
    } else if (symbol.find("NQ") != std::string::npos && symbol.find("MNQ") == std::string::npos) {
        return GetProfileNQ();
    } else if (symbol.find("MNQ") != std::string::npos) {
        return GetProfileMNQ();
    } else if (symbol.find("YM") != std::string::npos) {
        return GetProfileYM();
    } else if (symbol.find("RTY") != std::string::npos) {
        return GetProfileRTY();
    } else {
        // Default to ES
        return GetProfileES();
    }
}

/**
 * Apply instrument profile to zone config
 */
inline void ApplyProfileToConfig(ZoneConfig& config, const InstrumentProfile& profile) {
    config.baseCoreTicks = profile.baseCoreTicks;
    config.baseHaloTicks = profile.baseHaloTicks;
    config.hvnThreshold = profile.hvnThreshold;
    config.lvnThreshold = profile.lvnThreshold;
    config.acceptanceMinBars = profile.acceptanceMinBars;
    config.acceptanceVolRatio = profile.acceptanceVolRatio;
}

} // namespace AMT

#endif // AMT_CONFIG_H