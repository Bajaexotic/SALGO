// ============================================================================
// AMT_LoggingContext.h
// Logging lifecycle contract implementation
// Enforces SSOT compliance for all logged values
// ============================================================================
//
// CONTRACT: This module implements the AMT Logging Lifecycle Contract.
// All logged values MUST be sourced from this context, never from cached
// amtContext.* fields directly.
//
// LIFECYCLE RULES:
//   1.1: Logging at bar close only (GetBarHasClosedStatus == CLOSED)
//   1.2: No logging during historical replay (except via SampleHistoricalContext)
//   1.5: Defer logging on session boundary bars
//   2.1: Use authoritative SSOT sources only
//   2.2: Recompute derived values at log time
//   2.3: Never read from forbidden cached sources
//
// ============================================================================

#ifndef AMT_LOGGING_CONTEXT_H
#define AMT_LOGGING_CONTEXT_H

#include "amt_core.h"
#include "AMT_Helpers.h"
#include "AMT_Snapshots.h"
#include "AMT_config.h"
#include "AMT_Arbitration_Seam.h"
#include "AMT_Logger.h"  // For SessionEvent used in PopulateEventFromContext
#include <cmath>
#include <algorithm>

namespace AMT {

// ============================================================================
// LOGGING CONTEXT STRUCT
// ============================================================================
// Contains all values needed for logging with explicit validity flags.
// Values are either VALID, SUPPRESSED (unavailable), or the entire record
// should be DEFERRED.
// ============================================================================

struct LoggingContext {
    // ========================================================================
    // LIFECYCLE STATUS
    // ========================================================================
    bool shouldDefer = false;       // If true, do not log this bar at all
    bool isValid = false;           // If false, entire context is invalid

    // ========================================================================
    // BAR IDENTIFICATION
    // ========================================================================
    int barIndex = 0;
    SCDateTime barTime;
    bool isHistorical = false;      // True if sampled from subgraphs

    // ========================================================================
    // DELTA CONSISTENCY (Rule 2.2: Must be recomputed)
    // ========================================================================
    float deltaConf = 0.0f;
    bool deltaConfValid = false;    // False if baseline insufficient

    // ========================================================================
    // FACILITATION (Rule 2.2: Must be recomputed)
    // ========================================================================
    AuctionFacilitation facilitation = AuctionFacilitation::EFFICIENT;
    bool facilitationValid = false; // False if baseline insufficient

    // ========================================================================
    // AGGRESSION (Rule 2.2: Must be recomputed from extreme delta chain)
    // ========================================================================
    AggressionType aggression = AggressionType::RESPONSIVE;
    bool aggressionValid = false;   // False if dependencies invalid

    // ========================================================================
    // LIQUIDITY AVAILABILITY (Rule 2.2: Recompute from DOM + baseline)
    // ========================================================================
    float liquidityAvailability = 0.0f;
    bool liquidityValid = false;    // False on historical or DOM invalid

    // ========================================================================
    // MARKET STATE (SSOT: DaltonEngine via 1TF/2TF detection)
    // ========================================================================
    AMTMarketState marketState = AMTMarketState::UNKNOWN;
    bool marketStateValid = false;

    // ========================================================================
    // PHASE (SSOT: phaseCoordinator.GetPhase() or Subgraph[3] for historical)
    // ========================================================================
    SessionPhase phase = SessionPhase::UNKNOWN;
    bool phaseValid = false;

    // ========================================================================
    // SESSION DELTA METRICS
    // ========================================================================
    double sessDeltaPct = 0.0;      // sessionCumDelta / sessionTotalVolume
    bool sessDeltaPctValid = false; // False if sessionTotalVolume == 0

    int sessDeltaPctl = -1;         // Percentile rank [0-100], -1 if invalid
    bool sessDeltaPctlValid = false;// False if baseline size < 10

    bool coherent = false;          // Session delta direction matches bar delta
    bool coherentValid = false;     // False if sessDeltaPct invalid

    // ========================================================================
    // EXTREME DELTA COMPONENTS (for diagnostic logging)
    // ========================================================================
    bool isExtremeDeltaBar = false;     // Per-bar: deltaConf > 0.7
    bool isExtremeDeltaSession = false; // Session: percentile >= 85
    bool isExtremeDelta = false;        // Combined: bar && session

    // ========================================================================
    // VBP FIELDS (SSOT: sessionMgr.GetPOC/VAH/VAL())
    // ========================================================================
    double poc = 0.0;
    double vah = 0.0;
    double val = 0.0;
    bool vbpValid = false;          // False if any <= 0 or VAH <= VAL

    // ========================================================================
    // BAR DATA (SSOT: Native SC arrays - always valid for closed bars)
    // ========================================================================
    double volume = 0.0;
    double rangeTicks = 0.0;
    double delta = 0.0;             // Bar delta (Ask - Bid volume)

    // ========================================================================
    // HELPER: Check if record should be suppressed entirely
    // ========================================================================
    bool ShouldSuppress() const {
        // Rule 3.1: Suppress if phase UNDEFINED or VBP invalid
        if (phase == SessionPhase::UNKNOWN) return true;
        if (!vbpValid) return true;
        return false;
    }

    // ========================================================================
    // HELPER: Get field display string with N/A fallback
    // ========================================================================
    const char* GetDeltaConfStr() const {
        static char buf[16];
        if (!deltaConfValid) return "N/A";
        snprintf(buf, sizeof(buf), "%.2f", deltaConf);
        return buf;
    }

    const char* GetFacilitationStr() const {
        if (!facilitationValid) return "UNKNOWN";
        return to_string(facilitation);
    }

    const char* GetAggressionStr() const {
        if (!aggressionValid) return "UNKNOWN";
        return to_string(aggression);
    }

    const char* GetLiquidityStr() const {
        static char buf[16];
        if (!liquidityValid) return "N/A";
        snprintf(buf, sizeof(buf), "%.2f", liquidityAvailability);
        return buf;
    }

    const char* GetMarketStateStr() const {
        if (!marketStateValid) return "UNDEFINED";
        return to_string(marketState);
    }

    int GetSessDeltaPctlDisplay() const {
        return sessDeltaPctlValid ? sessDeltaPctl : -1;
    }
};

// ============================================================================
// LOGGING CONTEXT THRESHOLDS
// ============================================================================

namespace LogContextThresholds {
    // Minimum baseline samples for valid percentile computation
    static constexpr int MIN_BASELINE_SAMPLES = 10;
    static constexpr int MIN_FACIL_SAMPLES = 20;
    static constexpr int MIN_LIQUIDITY_SAMPLES = 10;
    static constexpr int MIN_SESSION_DELTA_SAMPLES = 10;

    // Delta consistency normalization (BOTH directions)
    static constexpr double DELTA_EXTREME_HIGH_THRESHOLD = AMT_Arb::EXTREME_DELTA_HIGH_THRESHOLD;  // 0.7
    static constexpr double DELTA_EXTREME_LOW_THRESHOLD = AMT_Arb::EXTREME_DELTA_LOW_THRESHOLD;    // 0.3
    static constexpr double SESSION_EXTREME_PCTILE = AMT_Arb::SESSION_EXTREME_PCTILE_THRESHOLD;    // 85.0

    // Legacy alias
    static constexpr double DELTA_EXTREME_THRESHOLD = DELTA_EXTREME_HIGH_THRESHOLD;
}

// ============================================================================
// COLLECT LOGGING CONTEXT - REMOVED (Dec 2024)
// ============================================================================
// This function has been removed because:
// 1. It was never called (dead code)
// 2. It depended on legacy BaselineEngine which has been removed
//
// If logging context collection is needed in the future, it should be
// reimplemented using the new baseline system:
//   - EffortBaselineStore (bucket-based effort baselines)
//   - SessionDeltaBaseline (session-aggregate delta)
//   - DOMWarmup (live 15-min DOM warmup)
// ============================================================================

// ============================================================================
// SAMPLE HISTORICAL CONTEXT (Historical Bar - Read from Subgraphs)
// ============================================================================
// Samples values from stored subgraphs for historical bars.
// Used by LogAmtBar and engagement finalization logging.
//
// SUBGRAPH MAPPING:
//   Subgraph[3]  - Phase (CurrentPhase enum as int)
//   Subgraph[6]  - POC price
//   Subgraph[7]  - VAH price
//   Subgraph[8]  - VAL price
//   Subgraph[9]  - POC proximity
//   Subgraph[10] - VAH proximity
//   Subgraph[11] - VAL proximity
//   Subgraph[12] - Facilitation (AuctionFacilitation enum as int)
//   Subgraph[13] - MarketState (AMTMarketState enum as int)
//   Subgraph[14] - DeltaConsistency (float)
//
// PARAMETERS:
//   sc      - Sierra Chart interface
//   barIdx  - Historical bar index to sample
//   tickSize - Instrument tick size
//
// RETURNS:
//   Populated LoggingContext with sampled values
// ============================================================================

inline LoggingContext SampleHistoricalContext(
    s_sc& sc,
    int barIdx,
    double tickSize)
{
    LoggingContext ctx;

    // ========================================================================
    // BAR IDENTIFICATION
    // ========================================================================
    ctx.barIndex = barIdx;
    ctx.barTime = sc.BaseDateTimeIn[barIdx];
    ctx.isHistorical = true;
    ctx.shouldDefer = false;

    // ========================================================================
    // BAR DATA (SSOT: Native SC arrays - always valid)
    // ========================================================================
    ctx.volume = sc.Volume[barIdx];
    ctx.rangeTicks = (sc.High[barIdx] - sc.Low[barIdx]) / tickSize;
    ctx.delta = sc.AskVolume[barIdx] - sc.BidVolume[barIdx];

    // ========================================================================
    // PHASE (Sample from Subgraph[3])
    // ========================================================================
    const int storedPhaseInt = static_cast<int>(sc.Subgraph[3][barIdx]);
    // Convert CurrentPhase to SessionPhase for logging compatibility
    // Note: Subgraph stores CurrentPhase, but we need SessionPhase for context
    // For historical bars, we use the stored value directly as validation
    ctx.phaseValid = (storedPhaseInt > 0);  // 0 = uninitialized

    // Map CurrentPhase to a reasonable SessionPhase for logging
    // This is approximate - historical bars don't have full phase context
    if (ctx.phaseValid) {
        // Use MID_SESSION as default for RTH bars (most common case)
        ctx.phase = SessionPhase::MID_SESSION;
    } else {
        ctx.phase = SessionPhase::UNKNOWN;
    }

    // ========================================================================
    // VBP FIELDS (Sample from Subgraph[6-8])
    // ========================================================================
    ctx.poc = sc.Subgraph[6][barIdx];
    ctx.vah = sc.Subgraph[7][barIdx];
    ctx.val = sc.Subgraph[8][barIdx];
    ctx.vbpValid = (ctx.poc > 0.0 && ctx.vah > 0.0 && ctx.val > 0.0 && ctx.vah > ctx.val);

    // ========================================================================
    // FACILITATION (Sample from Subgraph[12])
    // ========================================================================
    const int storedFacilInt = static_cast<int>(sc.Subgraph[12][barIdx]);
    if (storedFacilInt >= 1 && storedFacilInt <= 4) {
        ctx.facilitation = static_cast<AuctionFacilitation>(storedFacilInt);
        ctx.facilitationValid = true;
    } else {
        ctx.facilitation = AuctionFacilitation::EFFICIENT;
        ctx.facilitationValid = false;
    }

    // ========================================================================
    // MARKET STATE (Sample from Subgraph[13])
    // ========================================================================
    const int storedStateInt = static_cast<int>(sc.Subgraph[13][barIdx]);
    if (storedStateInt >= 0 && storedStateInt <= 2) {
        ctx.marketState = static_cast<AMTMarketState>(storedStateInt);
        ctx.marketStateValid = (ctx.marketState != AMTMarketState::UNKNOWN);
    } else {
        ctx.marketState = AMTMarketState::UNKNOWN;
        ctx.marketStateValid = false;
    }

    // ========================================================================
    // DELTA CONSISTENCY (Sample from Subgraph[14])
    // ========================================================================
    ctx.deltaConf = sc.Subgraph[14][barIdx];
    ctx.deltaConfValid = (ctx.deltaConf > 0.0f || sc.Subgraph[14][barIdx] != 0.0f);

    // ========================================================================
    // HISTORICAL BARS: These fields cannot be computed
    // ========================================================================
    // Liquidity: DOM not available on historical
    ctx.liquidityAvailability = 0.0f;
    ctx.liquidityValid = false;

    // Session delta: Would require accumulation from session start
    ctx.sessDeltaPct = 0.0;
    ctx.sessDeltaPctValid = false;
    ctx.sessDeltaPctl = -1;
    ctx.sessDeltaPctlValid = false;
    ctx.coherent = false;
    ctx.coherentValid = false;

    // Aggression: Depends on session delta, so cannot compute
    ctx.aggression = AggressionType::RESPONSIVE;
    ctx.aggressionValid = false;

    // Extreme delta: Partially available from stored deltaConf
    // BUG FIX: Check BOTH buying (>0.7) AND selling (<0.3) extremes
    ctx.isExtremeDeltaBar = ctx.deltaConfValid &&
        (ctx.deltaConf > LogContextThresholds::DELTA_EXTREME_HIGH_THRESHOLD ||
         ctx.deltaConf < LogContextThresholds::DELTA_EXTREME_LOW_THRESHOLD);
    ctx.isExtremeDeltaSession = false;  // Cannot compute
    ctx.isExtremeDelta = false;         // Cannot compute

    // ========================================================================
    // FINAL VALIDITY
    // ========================================================================
    ctx.isValid = ctx.phaseValid || ctx.vbpValid;  // At least some data available

    return ctx;
}

// ============================================================================
// POPULATE SESSION EVENT FROM LOGGING CONTEXT
// ============================================================================
// Helper to populate a SessionEvent struct from a LoggingContext.
// Uses N/A fallbacks for invalid fields per contract.
// ============================================================================

inline void PopulateEventFromContext(
    SessionEvent& evt,
    const LoggingContext& ctx)
{
    // Delta consistency
    evt.deltaConf = ctx.deltaConfValid ? ctx.deltaConf : 0.0;

    // Session delta
    evt.sessDeltaPct = ctx.sessDeltaPctValid ? ctx.sessDeltaPct : 0.0;
    evt.sessDeltaPctl = ctx.sessDeltaPctlValid ? ctx.sessDeltaPctl : -1;
    evt.coherent = ctx.coherentValid ? (ctx.coherent ? 1 : 0) : 0;

    // Aggression (string field)
    if (ctx.aggressionValid) {
        evt.aggression = to_string(ctx.aggression);
    } else {
        evt.aggression = "UNKNOWN";
    }

    // Facilitation (string field)
    if (ctx.facilitationValid) {
        evt.facilitation = to_string(ctx.facilitation);
    } else {
        evt.facilitation = "UNKNOWN";
    }

    // Market state (string field)
    if (ctx.marketStateValid) {
        evt.marketState = to_string(ctx.marketState);
    } else {
        evt.marketState = "UNDEFINED";
    }

    // VBP fields
    evt.poc = ctx.poc;
    evt.vah = ctx.vah;
    evt.val = ctx.val;

    // Volume/range
    evt.volume = ctx.volume;
    evt.range = ctx.rangeTicks;
}

// ============================================================================
// BAR CLOSE GUARD HELPER
// ============================================================================
// Returns true if the bar is closed and eligible for logging.
// Encapsulates the Rule 1.1 check.
// ============================================================================

inline bool IsBarClosedForLogging(s_sc& sc, int barIdx) {
    return (sc.GetBarHasClosedStatus(barIdx) == BHCS_BAR_HAS_CLOSED);
}

// ============================================================================
// HISTORICAL REPLAY GUARD HELPER
// ============================================================================
// Returns true if we're in historical replay mode.
// Encapsulates the Rule 1.2 check.
// ============================================================================

inline bool IsHistoricalReplay(s_sc& sc) {
    return sc.IsFullRecalculation;
}

} // namespace AMT

#endif // AMT_LOGGING_CONTEXT_H
