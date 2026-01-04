#pragma once
// ============================================================================
// AMT_BaselineGate.h - Centralized Baseline Decision Gate
// ============================================================================
//
// PURPOSE: Single access point for all baseline-derived decision inputs.
// Provides validity-gated queries that enforce the NO-FALLBACK contract.
//
// DESIGN PRINCIPLES:
//   1. All decision consumers query through this gate, not directly
//   2. Every output includes explicit validity (no silent defaults)
//   3. Missing baselines produce "not ready" state, not fake values
//   4. Centralizes the phase→bucket routing complexity
//
// DECISION CONSUMERS:
//   1. ExtremeDelta       - Is this bar/session delta extreme?
//   2. MarketComposition  - Is volume/trade activity elevated?
//   3. RangeClassification - Is bar range expanded/compressed?
//   4. DirectionalTravel  - Is price movement significant?
//   5. LiquidityState     - Is liquidity available/stressed?
//   6. DepthPercentile    - Where is current depth vs baseline?
//
// USAGE:
//   BaselineDecisionGate gate(effortStore, sessionDeltaBaseline,
//                             liquidityEngine, domWarmup);
//
//   auto deltaInput = gate.QueryExtremeDelta(phase, barDeltaPct, sessionDeltaPct);
//   if (deltaInput.IsReady()) {
//       if (deltaInput.barPctile.value >= 85.0 && deltaInput.sessionPctile.value >= 85.0) {
//           // Extreme delta confirmed
//       }
//   }
//
// ============================================================================

#include "amt_core.h"
#include "AMT_Snapshots.h"
#include "AMT_Liquidity.h"

namespace AMT {

// ============================================================================
// DECISION INPUT STRUCTS
// ============================================================================
// Each struct represents the gate's output for a specific decision domain.
// All include validity flags - consumers MUST check before using values.
// ============================================================================

// ----------------------------------------------------------------------------
// Extreme Delta Decision Input
// ----------------------------------------------------------------------------
// Used to determine if current bar/session delta is extreme relative to baseline.
// Both conditions must be valid for a complete extreme delta assessment.
//
// Threshold: barPctile >= 85 AND sessionPctile >= 85 → extreme
//
struct ExtremeDeltaInput {
    PercentileResult barPctile;      // Bar delta_pct percentile (EffortBaselineStore)
    PercentileResult sessionPctile;  // Session delta_ratio percentile (SessionDeltaBaseline)

    // Both components must be valid for a complete decision
    bool IsReady() const {
        return barPctile.valid && sessionPctile.valid;
    }

    // Check if delta is extreme (both bar and session exceed threshold)
    bool IsExtreme(double threshold = 85.0) const {
        if (!IsReady()) return false;
        return barPctile.value >= threshold && sessionPctile.value >= threshold;
    }

    // Diagnostic: which component is missing
    bool HasBarBaseline() const { return barPctile.valid; }
    bool HasSessionBaseline() const { return sessionPctile.valid; }
};

// ----------------------------------------------------------------------------
// Market Composition Decision Input
// ----------------------------------------------------------------------------
// Used to assess overall market activity level (volume + trades intensity).
// Elevated composition suggests responsive/initiative activity.
//
struct MarketCompositionInput {
    PercentileResult volSecPctile;    // Volume per second percentile
    PercentileResult tradesSecPctile; // Trades per second percentile
    PercentileResult avgTradeSizePctile; // Average trade size percentile

    // Need at least volume or trades to assess composition
    bool IsReady() const {
        return volSecPctile.valid || tradesSecPctile.valid;
    }

    // Fully ready with all components
    bool IsFullyReady() const {
        return volSecPctile.valid && tradesSecPctile.valid && avgTradeSizePctile.valid;
    }

    // Composite activity level (average of available components)
    double GetActivityLevel() const {
        if (!IsReady()) return 0.0;

        double sum = 0.0;
        int count = 0;

        if (volSecPctile.valid) { sum += volSecPctile.value; ++count; }
        if (tradesSecPctile.valid) { sum += tradesSecPctile.value; ++count; }

        return (count > 0) ? sum / count : 0.0;
    }

    // Is activity elevated (above 75th percentile average)
    bool IsElevated(double threshold = 75.0) const {
        return GetActivityLevel() >= threshold;
    }
};

// ----------------------------------------------------------------------------
// Range Classification Decision Input
// ----------------------------------------------------------------------------
// Used to classify bar range as compressed, normal, or expanded.
// Drives volatility regime detection and zone width adaptation.
//
enum class RangeRegime : int {
    UNKNOWN = 0,       // Baseline not ready
    COMPRESSED,        // Below 25th percentile
    NORMAL,            // 25th to 75th percentile
    EXPANDED           // Above 75th percentile
};

struct RangeClassificationInput {
    PercentileResult rangePctile;  // Bar range percentile

    bool IsReady() const { return rangePctile.valid; }

    RangeRegime GetRegime() const {
        if (!IsReady()) return RangeRegime::UNKNOWN;

        if (rangePctile.value < 25.0) return RangeRegime::COMPRESSED;
        if (rangePctile.value > 75.0) return RangeRegime::EXPANDED;
        return RangeRegime::NORMAL;
    }

    // Is range expanded (above threshold)
    bool IsExpanded(double threshold = 75.0) const {
        return IsReady() && rangePctile.value >= threshold;
    }

    // Is range compressed (below threshold)
    bool IsCompressed(double threshold = 25.0) const {
        return IsReady() && rangePctile.value <= threshold;
    }
};

inline const char* RangeRegimeToString(RangeRegime r) {
    switch (r) {
        case RangeRegime::UNKNOWN: return "UNKNOWN";
        case RangeRegime::COMPRESSED: return "COMPRESSED";
        case RangeRegime::NORMAL: return "NORMAL";
        case RangeRegime::EXPANDED: return "EXPANDED";
    }
    return "UNK";
}

// ----------------------------------------------------------------------------
// Directional Travel Decision Input
// ----------------------------------------------------------------------------
// Used to assess significance of price movement.
// Elevated travel suggests directional conviction (trend continuation or reversal).
//
struct DirectionalTravelInput {
    PercentileResult absChangePctile;  // |close - prevClose| percentile

    bool IsReady() const { return absChangePctile.valid; }

    // Is travel significant (above threshold)
    bool IsSignificant(double threshold = 70.0) const {
        return IsReady() && absChangePctile.value >= threshold;
    }

    // Is travel minimal (below threshold)
    bool IsMinimal(double threshold = 30.0) const {
        return IsReady() && absChangePctile.value <= threshold;
    }
};

// ----------------------------------------------------------------------------
// Liquidity State Decision Input
// ----------------------------------------------------------------------------
// Used to assess current liquidity availability vs stress.
// Wraps Liq3Result with additional convenience methods.
//
struct LiquidityStateInput {
    Liq3Result liq3;  // Full 3-component liquidity result

    bool IsReady() const { return liq3.liqValid; }

    // GetState() is validity-safe: returns LIQ_NOT_READY when invalid
    LiquidityState GetState() const { return liq3.liqState; }

    // Component availability
    bool HasDepth() const { return liq3.depthBaselineReady; }
    bool HasStress() const { return liq3.stressBaselineReady; }
    bool HasResilience() const { return liq3.resilienceBaselineReady; }

    // ========================================================================
    // VALIDITY-SAFE STATE CHECKS (No coincidental safety)
    // ========================================================================
    // Each helper explicitly checks IsReady() first.
    // Returns false when invalid (not just because state != target).
    // ========================================================================

    bool IsVoid() const {
        return IsReady() && liq3.liqState == LiquidityState::LIQ_VOID;
    }

    bool IsThin() const {
        return IsReady() && liq3.liqState == LiquidityState::LIQ_THIN;
    }

    bool IsNormal() const {
        return IsReady() && liq3.liqState == LiquidityState::LIQ_NORMAL;
    }

    bool IsThick() const {
        return IsReady() && liq3.liqState == LiquidityState::LIQ_THICK;
    }

    // Is liquidity available for trading (not void or thin)
    bool IsAvailable() const {
        return IsReady() && (liq3.liqState == LiquidityState::LIQ_NORMAL ||
                             liq3.liqState == LiquidityState::LIQ_THICK);
    }

    // Is liquidity stressed (void or thin)
    bool IsStressed() const {
        return IsReady() && (liq3.liqState == LiquidityState::LIQ_VOID ||
                             liq3.liqState == LiquidityState::LIQ_THIN);
    }

    // ========================================================================
    // ERROR ACCESS (for logging and counters)
    // ========================================================================

    LiquidityErrorReason GetErrorReason() const { return liq3.errorReason; }
    bool IsWarmup() const { return liq3.IsWarmup(); }
    bool IsHardError() const { return liq3.IsHardError(); }
};

// ----------------------------------------------------------------------------
// Depth Percentile Decision Input
// ----------------------------------------------------------------------------
// Used for DOM-based depth assessment relative to historical baseline.
// Complements LiquidityStateInput with phase-aware DOM metrics.
//
struct DepthPercentileInput {
    PercentileResult depthPctile;       // Core depth mass percentile
    PercentileResult haloPctile;        // Halo depth percentile (optional)
    PercentileResult imbalancePctile;   // Bid/Ask imbalance percentile (optional)
    PercentileResult spreadPctile;      // Spread percentile (optional)

    bool IsReady() const { return depthPctile.valid; }
    bool IsHaloReady() const { return haloPctile.valid; }
    bool IsSpreadReady() const { return spreadPctile.valid; }

    // Is depth depleted (below threshold)
    bool IsDepleted(double threshold = 25.0) const {
        return IsReady() && depthPctile.value <= threshold;
    }

    // Is depth elevated (above threshold)
    bool IsElevated(double threshold = 75.0) const {
        return IsReady() && depthPctile.value >= threshold;
    }
};

// ============================================================================
// BASELINE DECISION GATE
// ============================================================================
// Central access point for all baseline-derived decision inputs.
// Encapsulates phase routing and validity checking.
// ============================================================================

class BaselineDecisionGate {
public:
    // References to underlying baseline systems (not owned)
    const EffortBaselineStore* effortStore = nullptr;
    const SessionDeltaBaseline* sessionDeltaBaseline = nullptr;
    const LiquidityEngine* liquidityEngine = nullptr;
    const DOMWarmup* domWarmup = nullptr;

    // Configuration
    SessionPhase currentPhase = SessionPhase::GLOBEX;  // Updated by caller each bar

    // ========================================================================
    // INITIALIZATION
    // ========================================================================

    BaselineDecisionGate() = default;

    BaselineDecisionGate(
        const EffortBaselineStore* effort,
        const SessionDeltaBaseline* sessionDelta,
        const LiquidityEngine* liq,
        const DOMWarmup* dom)
        : effortStore(effort)
        , sessionDeltaBaseline(sessionDelta)
        , liquidityEngine(liq)
        , domWarmup(dom)
    {}

    void SetPhase(SessionPhase phase) {
        currentPhase = phase;
    }

    // ========================================================================
    // READINESS CHECK
    // ========================================================================
    // Returns true if gate has all required baseline references

    bool IsConfigured() const {
        return effortStore != nullptr;  // Effort is minimum requirement
    }

    bool HasEffortBaseline() const { return effortStore != nullptr; }
    bool HasSessionDeltaBaseline() const { return sessionDeltaBaseline != nullptr; }
    bool HasLiquidityEngine() const { return liquidityEngine != nullptr; }
    bool HasDOMWarmup() const { return domWarmup != nullptr; }

    // ========================================================================
    // DECISION QUERIES
    // ========================================================================

    // ------------------------------------------------------------------------
    // 1. Extreme Delta
    // ------------------------------------------------------------------------
    // Queries both bar-level and session-level delta baselines.
    //
    // barDeltaPct: Current bar's delta/volume ratio (-1 to +1)
    // sessionDeltaPct: Current session's cumulative delta ratio
    //
    ExtremeDeltaInput QueryExtremeDelta(double barDeltaPct, double sessionDeltaPct) const {
        ExtremeDeltaInput result;

        // Bar-level delta percentile from EffortBaselineStore
        if (effortStore != nullptr) {
            const auto& bucket = effortStore->Get(currentPhase);
            result.barPctile = bucket.delta_pct.TryPercentile(std::abs(barDeltaPct));
        }

        // Session-level delta percentile from SessionDeltaBaseline
        if (sessionDeltaBaseline != nullptr) {
            result.sessionPctile = sessionDeltaBaseline->TryGetPercentile(
                currentPhase, sessionDeltaPct);
        }

        return result;
    }

    // Overload using specific phase
    ExtremeDeltaInput QueryExtremeDelta(
        SessionPhase phase, double barDeltaPct, double sessionDeltaPct) const {

        ExtremeDeltaInput result;

        if (effortStore != nullptr) {
            const auto& bucket = effortStore->Get(phase);
            result.barPctile = bucket.delta_pct.TryPercentile(std::abs(barDeltaPct));
        }

        if (sessionDeltaBaseline != nullptr) {
            result.sessionPctile = sessionDeltaBaseline->TryGetPercentile(
                phase, sessionDeltaPct);
        }

        return result;
    }

    // ------------------------------------------------------------------------
    // 2. Market Composition
    // ------------------------------------------------------------------------
    // Queries volume and trade activity baselines.
    //
    // volSec: Current bar's volume per second
    // tradesSec: Current bar's trades per second
    // avgTradeSize: Current bar's average trade size (volume / numTrades)
    //
    MarketCompositionInput QueryMarketComposition(
        double volSec, double tradesSec, double avgTradeSize = 0.0) const {

        MarketCompositionInput result;

        if (effortStore != nullptr) {
            const auto& bucket = effortStore->Get(currentPhase);
            result.volSecPctile = bucket.vol_sec.TryPercentile(volSec);
            result.tradesSecPctile = bucket.trades_sec.TryPercentile(tradesSec);
            if (avgTradeSize > 0.0) {
                result.avgTradeSizePctile = bucket.avg_trade_size.TryPercentile(avgTradeSize);
            }
        }

        return result;
    }

    // Overload using specific phase
    MarketCompositionInput QueryMarketComposition(
        SessionPhase phase, double volSec, double tradesSec, double avgTradeSize = 0.0) const {

        MarketCompositionInput result;

        if (effortStore != nullptr) {
            const auto& bucket = effortStore->Get(phase);
            result.volSecPctile = bucket.vol_sec.TryPercentile(volSec);
            result.tradesSecPctile = bucket.trades_sec.TryPercentile(tradesSec);
            if (avgTradeSize > 0.0) {
                result.avgTradeSizePctile = bucket.avg_trade_size.TryPercentile(avgTradeSize);
            }
        }

        return result;
    }

    // ------------------------------------------------------------------------
    // 3. Range Classification
    // ------------------------------------------------------------------------
    // Queries bar range baseline.
    //
    // barRangeTicks: Current bar's range (high - low) in ticks
    //
    RangeClassificationInput QueryRangeClassification(double barRangeTicks) const {
        RangeClassificationInput result;

        if (effortStore != nullptr) {
            const auto& bucket = effortStore->Get(currentPhase);
            result.rangePctile = bucket.bar_range.TryPercentile(barRangeTicks);
        }

        return result;
    }

    // Overload using specific phase
    RangeClassificationInput QueryRangeClassification(
        SessionPhase phase, double barRangeTicks) const {

        RangeClassificationInput result;

        if (effortStore != nullptr) {
            const auto& bucket = effortStore->Get(phase);
            result.rangePctile = bucket.bar_range.TryPercentile(barRangeTicks);
        }

        return result;
    }

    // ------------------------------------------------------------------------
    // 4. Directional Travel
    // ------------------------------------------------------------------------
    // Queries absolute close change baseline.
    //
    // absCloseChangeTicks: |close - prevClose| in ticks
    //
    DirectionalTravelInput QueryDirectionalTravel(double absCloseChangeTicks) const {
        DirectionalTravelInput result;

        if (effortStore != nullptr) {
            const auto& bucket = effortStore->Get(currentPhase);
            result.absChangePctile = bucket.abs_close_change.TryPercentile(absCloseChangeTicks);
        }

        return result;
    }

    // Overload using specific phase
    DirectionalTravelInput QueryDirectionalTravel(
        SessionPhase phase, double absCloseChangeTicks) const {

        DirectionalTravelInput result;

        if (effortStore != nullptr) {
            const auto& bucket = effortStore->Get(phase);
            result.absChangePctile = bucket.abs_close_change.TryPercentile(absCloseChangeTicks);
        }

        return result;
    }

    // ------------------------------------------------------------------------
    // 5. Liquidity State
    // ------------------------------------------------------------------------
    // Returns the most recent liquidity computation result.
    // NOTE: LiquidityEngine.Compute() must be called by the main loop first.
    // This just wraps the last result for decision convenience.
    //
    LiquidityStateInput WrapLiquidityResult(const Liq3Result& liq3) const {
        LiquidityStateInput result;
        result.liq3 = liq3;
        return result;
    }

    // ------------------------------------------------------------------------
    // 6. Depth Percentile
    // ------------------------------------------------------------------------
    // Queries DOM warmup baselines for depth metrics.
    //
    // depthMassCore: Current bar's core depth mass
    // depthMassHalo: Current bar's halo depth mass (optional)
    // imbalance: Current bar's bid/ask imbalance (optional)
    // spreadTicks: Current bar's spread in ticks (optional)
    //
    DepthPercentileInput QueryDepthPercentile(
        double depthMassCore,
        double depthMassHalo = 0.0,
        double imbalance = 0.0,
        double spreadTicks = -1.0) const {

        DepthPercentileInput result;

        if (domWarmup != nullptr) {
            result.depthPctile = domWarmup->TryDepthPercentile(currentPhase, depthMassCore);

            if (depthMassHalo > 0.0) {
                result.haloPctile = domWarmup->TryHaloPercentile(currentPhase, depthMassHalo);
                result.imbalancePctile = domWarmup->TryImbalancePercentile(currentPhase, imbalance);
            }

            if (spreadTicks >= 0.0) {
                result.spreadPctile = domWarmup->TrySpreadPercentile(currentPhase, spreadTicks);
            }
        }

        return result;
    }

    // Overload using specific phase
    DepthPercentileInput QueryDepthPercentile(
        SessionPhase phase,
        double depthMassCore,
        double depthMassHalo = 0.0,
        double imbalance = 0.0,
        double spreadTicks = -1.0) const {

        DepthPercentileInput result;

        if (domWarmup != nullptr) {
            result.depthPctile = domWarmup->TryDepthPercentile(phase, depthMassCore);

            if (depthMassHalo > 0.0) {
                result.haloPctile = domWarmup->TryHaloPercentile(phase, depthMassHalo);
                result.imbalancePctile = domWarmup->TryImbalancePercentile(phase, imbalance);
            }

            if (spreadTicks >= 0.0) {
                result.spreadPctile = domWarmup->TrySpreadPercentile(phase, spreadTicks);
            }
        }

        return result;
    }

    // ========================================================================
    // DIAGNOSTIC: Get overall readiness summary
    // ========================================================================

    struct ReadinessSummary {
        bool effortReady = false;
        bool sessionDeltaReady = false;
        bool liquidityReady = false;
        bool domReady = false;

        int readyCount() const {
            return (effortReady ? 1 : 0) +
                   (sessionDeltaReady ? 1 : 0) +
                   (liquidityReady ? 1 : 0) +
                   (domReady ? 1 : 0);
        }

        bool isFullyReady() const { return readyCount() == 4; }
    };

    ReadinessSummary GetReadinessSummary() const {
        ReadinessSummary summary;

        if (effortStore != nullptr) {
            summary.effortReady = effortStore->Get(currentPhase).IsReady();
        }

        if (sessionDeltaBaseline != nullptr) {
            summary.sessionDeltaReady = sessionDeltaBaseline->IsPhaseReady(currentPhase);
        }

        if (liquidityEngine != nullptr) {
            summary.liquidityReady = liquidityEngine->depthBaseline.IsReady(
                liquidityEngine->config.baselineMinSamples);
        }

        if (domWarmup != nullptr) {
            summary.domReady = domWarmup->IsReady(currentPhase);
        }

        return summary;
    }
};

} // namespace AMT
