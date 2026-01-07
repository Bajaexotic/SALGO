// ============================================================================
// AMT_Phase.h
// AMT-Aligned Phase System: MARKET STATE (macro) + PHASE (micro)
// ============================================================================
//
// MARKET STATE (Macro - from Dalton SSOT):
//   AMTMarketState::BALANCE   - Inside value, two-sided trade, horizontal development
//   AMTMarketState::IMBALANCE - Outside value, one-sided conviction, vertical price discovery
//
//   Market state is determined by Dalton's 1TF/2TF time-framing analysis.
//   This is the SSOT for macro market classification.
//
// CURRENT PHASE (Micro - location/activity based):
//   ROTATION          - Inside VA, balanced two-sided trade
//   TESTING_BOUNDARY  - At VA edge (VAL/VAH), probing
//   RANGE_EXTENSION   - Outside VA, making new extreme, accepted
//   DRIVING_UP        - Outside VA above, 1TF bullish, buyers in control
//   DRIVING_DOWN      - Outside VA below, 1TF bearish, sellers in control
//   PULLBACK          - Outside VA, approaching POC, returning toward value
//   FAILED_AUCTION    - Probed outside VA, rejected, returning to value
//
// KEY AMT INVARIANTS (non-negotiable):
//   A. ROTATION => insideVA && !atVAL && !atVAH
//   B. outsideVA => phase != ROTATION (even after hysteresis)
//   C. Hysteresis cannot output AMT-impossible labels
//
// ACCEPTANCE DEFINITION:
//   acceptanceOutsideVA = outsideCloseStreak >= acceptanceClosesRequired
//
// ============================================================================

#ifndef AMT_PHASE_H
#define AMT_PHASE_H

#include "amt_core.h"
#include "AMT_Zones.h"
#include "AMT_Helpers.h"
#include <array>
#include <algorithm>  // For std::clamp
#include <cmath>      // For std::abs

namespace AMT {

// ============================================================================
// PHASE PRIMITIVES (SSOT - computed once per bar)
// ============================================================================

struct PhasePrimitives {
    // ========================================================================
    // Value References (from VbP study via SessionManager)
    // ========================================================================
    double poc = 0.0;
    double vah = 0.0;
    double val = 0.0;
    double vaRangeTicks = 0.0;

    // ========================================================================
    // Price Basis (same as used for zone logic decisions)
    // ========================================================================
    double price = 0.0;
    double closePrice = 0.0;  // Bar close for acceptance detection
    double tickSize = 0.25;

    // ========================================================================
    // Location Flags (computed from price vs VA)
    // ========================================================================
    bool insideVA = false;       // VAL <= price <= VAH
    bool outsideLow = false;     // price < VAL
    bool outsideHigh = false;    // price > VAH
    bool atVAL = false;          // |price - VAL| <= boundaryToleranceTicks
    bool atVAH = false;          // |price - VAH| <= boundaryToleranceTicks

    // ========================================================================
    // Distance Metrics (in ticks)
    // ========================================================================
    double dPOC_ticks = 0.0;
    double dVAL_ticks = 0.0;
    double dVAH_ticks = 0.0;

    // ========================================================================
    // Session Extreme State (from StructureTracker)
    // ========================================================================
    double sessHi = 0.0;
    double sessLo = 0.0;
    double dSessHi_ticks = 0.0;
    double dSessLo_ticks = 0.0;
    bool madeNewHighRecently = false;
    bool madeNewLowRecently = false;
    bool nearSessionExtreme = false;

    // ========================================================================
    // ACCEPTANCE SIGNALS (AMT key concept)
    // ========================================================================
    int outsideCloseStreak = 0;      // Consecutive closes outside VA
    bool acceptanceOutsideVA = false; // Sustained trade outside VA confirmed

    // ========================================================================
    // Approach/Reversion Signal
    // ========================================================================
    bool approachingPOC = false;

    // ========================================================================
    // Directional Memory (from PhaseHistory)
    // ========================================================================
    bool wasDirectionalRecently = false;

    // ========================================================================
    // Failure Recency (from zone state)
    // ========================================================================
    int barsSinceFailure = -1;
    bool failureRecent = false;

    // ========================================================================
    // Return-to-Value State (for FAILED_AUCTION admissibility)
    // ========================================================================
    bool justReturnedFromOutside = false;  // True if recently returned from outside VA

    // ========================================================================
    // EXTREME ACCEPTANCE STATE (from ExtremeAcceptanceTracker)
    // AMT-aligned acceptance/rejection using tail, delta, time, retest signals
    // ========================================================================
    bool highProbeAccepted = false;    // Session high is accepted (RANGE_EXTENSION eligible)
    bool lowProbeAccepted = false;     // Session low is accepted (RANGE_EXTENSION eligible)
    bool highProbeRejected = false;    // Session high is rejected (triggers FAILED_AUCTION)
    bool lowProbeRejected = false;     // Session low is rejected (triggers FAILED_AUCTION)

    double highAcceptanceScore = 0.0;  // Composite score for high (-1 to +1)
    double lowAcceptanceScore = 0.0;   // Composite score for low (-1 to +1)

    // For diagnostics: which extreme is relevant for current price location
    bool usingHighExtreme = false;     // True if outside high, false if outside low

    // Convenience accessors for current location
    bool currentExtremeAccepted() const {
        return usingHighExtreme ? highProbeAccepted : lowProbeAccepted;
    }
    bool currentExtremeRejected() const {
        return usingHighExtreme ? highProbeRejected : lowProbeRejected;
    }
    double currentAcceptanceScore() const {
        return usingHighExtreme ? highAcceptanceScore : lowAcceptanceScore;
    }

    // ========================================================================
    // Validity
    // ========================================================================
    bool valid = false;
    int bar = -1;
};

// ============================================================================
// PHASE HISTORY (Ring buffer for directional afterglow)
// ============================================================================

struct PhaseHistory {
    static constexpr int MAX_HISTORY = 64;

    std::array<CurrentPhase, MAX_HISTORY> history;
    int head = 0;
    int count = 0;

    PhaseHistory() {
        history.fill(CurrentPhase::ROTATION);
    }

    void Push(CurrentPhase phase) {
        history[head] = phase;
        head = (head + 1) & (MAX_HISTORY - 1);
        if (count < MAX_HISTORY) count++;
    }

    bool WasDirectionalWithin(int lookbackBars) const {
        int checkCount = (std::min)(lookbackBars, count);
        for (int i = 0; i < checkCount; ++i) {
            int idx = (head - 1 - i + MAX_HISTORY) & (MAX_HISTORY - 1);
            CurrentPhase p = history[idx];
            if (p == CurrentPhase::DRIVING_UP || p == CurrentPhase::DRIVING_DOWN ||
                p == CurrentPhase::RANGE_EXTENSION) {
                return true;
            }
        }
        return false;
    }

    void Reset() {
        history.fill(CurrentPhase::ROTATION);
        head = 0;
        count = 0;
    }
};

// ============================================================================
// POC DISTANCE HISTORY (for approachingPOC detection)
// ============================================================================

struct POCDistanceHistory {
    static constexpr int MAX_HISTORY = 8;

    std::array<double, MAX_HISTORY> distances;
    int head = 0;
    int count = 0;

    POCDistanceHistory() {
        distances.fill(0.0);
    }

    void Push(double dPOC) {
        distances[head] = dPOC;
        head = (head + 1) % MAX_HISTORY;
        if (count < MAX_HISTORY) count++;
    }

    bool IsContractingFor(int nBars) const {
        if (count < nBars + 1) return false;

        for (int i = 0; i < nBars; ++i) {
            int curIdx = (head - 1 - i + MAX_HISTORY) % MAX_HISTORY;
            int prevIdx = (head - 2 - i + MAX_HISTORY) % MAX_HISTORY;
            if (distances[curIdx] >= distances[prevIdx]) {
                return false;
            }
        }
        return true;
    }

    void Reset() {
        distances.fill(0.0);
        head = 0;
        count = 0;
    }
};

// ============================================================================
// OUTSIDE CLOSE TRACKER (for acceptance detection)
// ============================================================================

struct OutsideCloseTracker {
    int consecutiveClosesOutsideVA = 0;
    bool lastCloseWasOutside = false;
    int barsSinceReturnedToVA = -1;  // Tracks how long since we returned from outside

    void Update(bool closeOutsideVA) {
        if (closeOutsideVA) {
            consecutiveClosesOutsideVA++;
            barsSinceReturnedToVA = -1;  // Still outside, reset return counter
        } else {
            // Just returned to inside VA
            if (lastCloseWasOutside) {
                barsSinceReturnedToVA = 0;  // Just returned this bar
            } else if (barsSinceReturnedToVA >= 0) {
                barsSinceReturnedToVA++;  // Increment time since return
            }
            consecutiveClosesOutsideVA = 0;
        }
        lastCloseWasOutside = closeOutsideVA;
    }

    int GetStreak() const {
        return consecutiveClosesOutsideVA;
    }

    /**
     * Returns true if price just returned from outside VA within threshold bars.
     * AMT SEMANTIC: Used to gate FAILED_AUCTION phase inside VA.
     */
    bool JustReturnedFromOutside(int thresholdBars = 3) const {
        return (barsSinceReturnedToVA >= 0 && barsSinceReturnedToVA <= thresholdBars);
    }

    void Reset() {
        consecutiveClosesOutsideVA = 0;
        lastCloseWasOutside = false;
        barsSinceReturnedToVA = -1;
    }
};

// ============================================================================
// EXTREME ACCEPTANCE TRACKER (AMT-aligned acceptance/rejection detection)
// ============================================================================
//
// AMT PRINCIPLE: Acceptance at session extremes is determined by:
//   1. TAIL RATIO - Bar structure showing rejection (excess) or acceptance
//   2. DELTA DIRECTION - Volume pushing toward or away from extreme
//   3. TIME AT PRICE - Duration spent near the extreme (TPO-like)
//   4. RETEST OUTCOMES - Returns to the level after departure
//
// A bar that makes a new extreme is just DISCOVERY. Acceptance/rejection is
// determined by SUBSEQUENT behavior, especially RETESTS (returns after leaving).
//
// ============================================================================

/**
 * Tracks the state of a single session extreme (high or low).
 * Updated every bar to accumulate acceptance/rejection signals.
 */
struct ExtremeLevel {
    // Level identity
    double price = 0.0;              // The extreme price
    int establishedBar = -1;         // Bar when extreme was first made
    int lastExtendedBar = -1;        // Bar when extreme was last extended

    // Proximity tracking
    int barsNearExtreme = 0;         // TPO-like: count of bars near this level
    int barsAway = 0;                // Consecutive bars NOT near (for retest detection)
    int lastBarNear = -1;            // Most recent bar that was near

    // Per-bar signal accumulators
    int rejectionTailCount = 0;      // Bars showing rejection tail at this level
    int confirmingDeltaCount = 0;    // Bars with delta toward the extreme
    int totalBarsEvaluated = 0;      // Total bars since establishment

    // Retest tracking (price left and returned)
    int retestCount = 0;             // Total retest events
    int retestHeldCount = 0;         // Retests that showed acceptance
    int retestRejectedCount = 0;     // Retests that showed rejection

    // Volume concentration tracking (from VbP profile)
    // Uses VolumeThresholds sigma-based classification for AMT-aligned signals
    double volumeAtExtreme = 0.0;    // Volume traded at/near extreme price
    double totalVolumeNearExtreme = 0.0;  // Total session volume (for ratio calculation)
    int barsWithVolumeData = 0;      // Count of bars with valid volume data
    VAPDensityClass volumeDensityClass = VAPDensityClass::NORMAL;  // HVN/LVN/NORMAL at extreme

    // Most recent bar's signals (for immediate decision)
    double lastTailRatio = 0.0;
    bool lastDeltaConfirmed = false;
    bool lastClosedNear = false;
    bool lastWasRetest = false;
    double lastVolumeConcentration = 0.0;  // Volume concentration at extreme for last bar
    bool lastVolumeDataValid = false;      // True if volume distribution data was available

    bool IsValid() const { return price > 0.0 && establishedBar >= 0; }

    double GetVolumeConcentration() const {
        if (totalVolumeNearExtreme <= 0.0) return 0.0;
        return volumeAtExtreme / totalVolumeNearExtreme;
    }

    bool HasVolumeData() const { return barsWithVolumeData > 0; }

    void Reset() {
        price = 0.0;
        establishedBar = -1;
        lastExtendedBar = -1;
        barsNearExtreme = 0;
        barsAway = 0;
        lastBarNear = -1;
        rejectionTailCount = 0;
        confirmingDeltaCount = 0;
        totalBarsEvaluated = 0;
        retestCount = 0;
        retestHeldCount = 0;
        retestRejectedCount = 0;
        volumeAtExtreme = 0.0;
        totalVolumeNearExtreme = 0.0;
        barsWithVolumeData = 0;
        volumeDensityClass = VAPDensityClass::NORMAL;
        lastTailRatio = 0.0;
        lastDeltaConfirmed = false;
        lastClosedNear = false;
        lastWasRetest = false;
        lastVolumeConcentration = 0.0;
        lastVolumeDataValid = false;
    }
};

/**
 * Extreme behavior state computed from ExtremeLevel signals.
 * Tracks acceptance/rejection at session extremes (high/low).
 * NOTE: Renamed from AcceptanceState to avoid confusion with LevelAcceptance framework.
 */
struct ExtremeBehaviorState {
    bool accepted = false;           // Composite: extreme is accepted
    bool rejected = false;           // Composite: extreme is rejected (triggers FAILED_AUCTION)
    double score = 0.0;              // Acceptance score (-1.0 to +1.0)

    // Component scores for diagnostics
    double tailScore = 0.0;          // Tail component
    double deltaScore = 0.0;         // Delta component
    double timeScore = 0.0;          // Time-at-price component
    double retestScore = 0.0;        // Retest outcome component
    double volumeScore = 0.0;        // Volume concentration component

    // Metadata
    bool hasVolumeData = false;      // True if volume distribution was available
    bool meetsTPOMinimum = false;    // True if barsNearExtreme >= minTPOsForAcceptance
};

/**
 * Configuration for acceptance detection thresholds.
 */
struct AcceptanceConfig {
    // Tail detection
    double rejectionTailRatio = 0.33;      // Tail/range >= this = rejection signal

    // Adaptive "near extreme" threshold
    int minNearExtremeTicks = 2;           // Floor for adaptive threshold
    int maxNearExtremeTicks = 8;           // Ceiling for adaptive threshold
    double nearExtremeRangePct = 0.05;     // 5% of session range

    // Delta confirmation
    double deltaConfirmThreshold = 0.50;   // deltaConsistency threshold

    // Retest detection
    int retestDepartureBars = 2;           // Bars away before return counts as retest

    // TPO (Time-Price Opportunity) threshold
    // AMT: Single print (1 TPO) is tentative, not acceptance
    int minTPOsForAcceptance = 2;          // Minimum bars near extreme to consider accepted

    // Volume concentration (from Numbers Bars)
    double volumeConcentrationThreshold = 0.60;  // 60%+ volume at extreme = confirmation

    // Acceptance decision thresholds
    double acceptanceThreshold = 0.35;     // Score >= this = accepted
    double rejectionThreshold = -0.25;     // Score <= this = rejected

    // Component weights (should sum to 1.0)
    // NOTE: When volume signal is available, weights are renormalized
    double tailWeight = 0.25;
    double deltaWeight = 0.15;
    double timeWeight = 0.20;
    double retestWeight = 0.20;
    double volumeWeight = 0.20;            // Volume concentration at extreme
};

/**
 * Tracks acceptance/rejection at session extremes using AMT-aligned signals.
 *
 * USAGE:
 *   1. Call OnNewSessionHigh/Low when extremes are extended
 *   2. Call UpdateBar EVERY bar to accumulate signals
 *   3. Call ComputeAcceptance to get current acceptance state
 *   4. Call OnSessionReset at session boundaries
 */
class ExtremeAcceptanceTracker {
public:
    ExtremeLevel sessionHigh;
    ExtremeLevel sessionLow;
    AcceptanceConfig config;

    // Cached acceptance state (updated by ComputeAcceptance)
    ExtremeBehaviorState highAcceptance;
    ExtremeBehaviorState lowAcceptance;

    /**
     * Called when a new session high is established or extended.
     */
    void OnNewSessionHigh(int bar, double price) {
        if (!sessionHigh.IsValid() || price > sessionHigh.price) {
            if (!sessionHigh.IsValid()) {
                sessionHigh.establishedBar = bar;
            }
            sessionHigh.price = price;
            sessionHigh.lastExtendedBar = bar;
        }
    }

    /**
     * Called when a new session low is established or extended.
     */
    void OnNewSessionLow(int bar, double price) {
        if (!sessionLow.IsValid() || price < sessionLow.price) {
            if (!sessionLow.IsValid()) {
                sessionLow.establishedBar = bar;
            }
            sessionLow.price = price;
            sessionLow.lastExtendedBar = bar;
        }
    }

    /**
     * Compute adaptive "near extreme" threshold based on session range.
     */
    int ComputeAdaptiveThreshold(int sessionRangeTicks) const {
        int rangeBased = static_cast<int>(sessionRangeTicks * config.nearExtremeRangePct);
        return std::clamp(rangeBased, config.minNearExtremeTicks, config.maxNearExtremeTicks);
    }

    /**
     * Called EVERY bar to update tracking. This is the core accumulation logic.
     *
     * @param bar Current bar index
     * @param high Bar high
     * @param low Bar low
     * @param open Bar open
     * @param close Bar close
     * @param delta Bar delta (from Numbers Bars or calculated)
     * @param deltaConsistency How one-sided the bar was (0-1)
     * @param sessionRangeTicks Current session range in ticks
     * @param tickSize Tick size for price comparisons
     */
    void UpdateBar(int bar, double high, double low, double open, double close,
                   double delta, double deltaConsistency,
                   int sessionRangeTicks, double tickSize) {
        const int adaptiveThreshold = ComputeAdaptiveThreshold(sessionRangeTicks);
        const double thresholdPrice = adaptiveThreshold * tickSize;
        const double barRange = high - low;

        // Update session low tracking
        if (sessionLow.IsValid()) {
            sessionLow.totalBarsEvaluated++;

            // Check if this bar is near the session low
            const bool nearLow = (low <= sessionLow.price + thresholdPrice);

            if (nearLow) {
                // Detect retest (returning after being away)
                const bool isRetest = (sessionLow.barsAway >= config.retestDepartureBars);
                if (isRetest) {
                    sessionLow.retestCount++;
                    sessionLow.lastWasRetest = true;
                } else {
                    sessionLow.lastWasRetest = false;
                }

                // Compute tail ratio (lower tail for session low)
                double lowerTail = (std::min)(open, close) - low;
                double tailRatio = (barRange > 0) ? lowerTail / barRange : 0.0;
                sessionLow.lastTailRatio = tailRatio;

                // Check for rejection tail
                const bool rejectionTail = (tailRatio >= config.rejectionTailRatio);
                if (rejectionTail) {
                    sessionLow.rejectionTailCount++;
                }

                // Check delta confirmation (negative delta confirms low acceptance)
                const bool deltaConfirms = (delta < 0) &&
                    (std::abs(deltaConsistency) >= config.deltaConfirmThreshold);
                sessionLow.lastDeltaConfirmed = deltaConfirms;
                if (deltaConfirms) {
                    sessionLow.confirmingDeltaCount++;
                }

                // Check close proximity
                const bool closedNear = ((close - sessionLow.price) <= thresholdPrice);
                sessionLow.lastClosedNear = closedNear;

                // Update retest outcomes
                if (isRetest) {
                    if (closedNear && !rejectionTail) {
                        sessionLow.retestHeldCount++;
                    } else if (rejectionTail) {
                        sessionLow.retestRejectedCount++;
                    }
                }

                sessionLow.barsNearExtreme++;
                sessionLow.lastBarNear = bar;
                sessionLow.barsAway = 0;
            } else {
                sessionLow.barsAway++;
                sessionLow.lastWasRetest = false;
            }
        }

        // Update session high tracking
        if (sessionHigh.IsValid()) {
            sessionHigh.totalBarsEvaluated++;

            // Check if this bar is near the session high
            const bool nearHigh = (high >= sessionHigh.price - thresholdPrice);

            if (nearHigh) {
                // Detect retest
                const bool isRetest = (sessionHigh.barsAway >= config.retestDepartureBars);
                if (isRetest) {
                    sessionHigh.retestCount++;
                    sessionHigh.lastWasRetest = true;
                } else {
                    sessionHigh.lastWasRetest = false;
                }

                // Compute tail ratio (upper tail for session high)
                double upperTail = high - (std::max)(open, close);
                double tailRatio = (barRange > 0) ? upperTail / barRange : 0.0;
                sessionHigh.lastTailRatio = tailRatio;

                // Check for rejection tail
                const bool rejectionTail = (tailRatio >= config.rejectionTailRatio);
                if (rejectionTail) {
                    sessionHigh.rejectionTailCount++;
                }

                // Check delta confirmation (positive delta confirms high acceptance)
                const bool deltaConfirms = (delta > 0) &&
                    (std::abs(deltaConsistency) >= config.deltaConfirmThreshold);
                sessionHigh.lastDeltaConfirmed = deltaConfirms;
                if (deltaConfirms) {
                    sessionHigh.confirmingDeltaCount++;
                }

                // Check close proximity
                const bool closedNear = ((sessionHigh.price - close) <= thresholdPrice);
                sessionHigh.lastClosedNear = closedNear;

                // Update retest outcomes
                if (isRetest) {
                    if (closedNear && !rejectionTail) {
                        sessionHigh.retestHeldCount++;
                    } else if (rejectionTail) {
                        sessionHigh.retestRejectedCount++;
                    }
                }

                sessionHigh.barsNearExtreme++;
                sessionHigh.lastBarNear = bar;
                sessionHigh.barsAway = 0;
            } else {
                sessionHigh.barsAway++;
                sessionHigh.lastWasRetest = false;
            }
        }
    }

    /**
     * Compute acceptance state from accumulated signals.
     * Call after UpdateBar to get current acceptance/rejection state.
     */
    void ComputeAcceptance() {
        highAcceptance = ComputeAcceptanceForLevel(sessionHigh, true);
        lowAcceptance = ComputeAcceptanceForLevel(sessionLow, false);
    }

    /**
     * Update volume concentration from VbP profile data.
     * Called after VbP profile is populated to add volume signal.
     *
     * @param highVolumeAtExtreme Volume traded at session high price (from VbP)
     * @param highTotalVolume Total session volume for comparison
     * @param lowVolumeAtExtreme Volume traded at session low price (from VbP)
     * @param lowTotalVolume Total session volume for comparison
     * @param bandVolume Optional: sum of volume within N ticks of extreme
     */
    void UpdateVolumeConcentration(
        double highVolumeAtExtreme, double highTotalVolume,
        double lowVolumeAtExtreme, double lowTotalVolume)
    {
        // Update session high volume concentration
        if (sessionHigh.IsValid() && highTotalVolume > 0.0) {
            double concentration = highVolumeAtExtreme / highTotalVolume;
            sessionHigh.volumeAtExtreme = highVolumeAtExtreme;
            sessionHigh.totalVolumeNearExtreme = highTotalVolume;
            sessionHigh.lastVolumeConcentration = concentration;
            sessionHigh.lastVolumeDataValid = true;
            sessionHigh.barsWithVolumeData++;
        }

        // Update session low volume concentration
        if (sessionLow.IsValid() && lowTotalVolume > 0.0) {
            double concentration = lowVolumeAtExtreme / lowTotalVolume;
            sessionLow.volumeAtExtreme = lowVolumeAtExtreme;
            sessionLow.totalVolumeNearExtreme = lowTotalVolume;
            sessionLow.lastVolumeConcentration = concentration;
            sessionLow.lastVolumeDataValid = true;
            sessionLow.barsWithVolumeData++;
        }
    }

    /**
     * Update volume concentration with band (multiple price levels around extreme).
     * More robust than single-price query.
     *
     * @param isHigh True for session high, false for session low
     * @param volumeInBand Sum of volume within bandTicks of extreme price
     * @param totalVolume Total session volume
     * @param bandTicks Number of ticks included in the band
     */
    void UpdateVolumeBand(bool isHigh, double volumeInBand, double totalVolume, int bandTicks) {
        (void)bandTicks;  // Unused for now
        ExtremeLevel& level = isHigh ? sessionHigh : sessionLow;

        if (level.IsValid() && totalVolume > 0.0) {
            level.volumeAtExtreme = volumeInBand;
            level.totalVolumeNearExtreme = totalVolume;
            level.lastVolumeConcentration = volumeInBand / totalVolume;
            level.lastVolumeDataValid = true;
            level.barsWithVolumeData++;
        }
    }

    /**
     * Update volume concentration with VolumeThresholds for AMT-aligned classification.
     * Uses sigma-based HVN/LVN thresholds to classify volume at extremes.
     *
     * @param highVolumeInBand Volume in band around session high
     * @param lowVolumeInBand Volume in band around session low
     * @param totalVolume Total session volume
     * @param thresholds VolumeThresholds with computed HVN/LVN thresholds
     */
    void UpdateVolumeWithThresholds(
        double highVolumeInBand, double lowVolumeInBand,
        double totalVolume, const VolumeThresholds& thresholds)
    {
        // Update session high with classification
        if (sessionHigh.IsValid() && totalVolume > 0.0) {
            sessionHigh.volumeAtExtreme = highVolumeInBand;
            sessionHigh.totalVolumeNearExtreme = totalVolume;
            sessionHigh.lastVolumeConcentration = highVolumeInBand / totalVolume;
            sessionHigh.lastVolumeDataValid = true;
            sessionHigh.barsWithVolumeData++;

            // Classify using sigma-based thresholds
            if (thresholds.valid) {
                sessionHigh.volumeDensityClass = thresholds.ClassifyVolume(highVolumeInBand);
            }
        }

        // Update session low with classification
        if (sessionLow.IsValid() && totalVolume > 0.0) {
            sessionLow.volumeAtExtreme = lowVolumeInBand;
            sessionLow.totalVolumeNearExtreme = totalVolume;
            sessionLow.lastVolumeConcentration = lowVolumeInBand / totalVolume;
            sessionLow.lastVolumeDataValid = true;
            sessionLow.barsWithVolumeData++;

            // Classify using sigma-based thresholds
            if (thresholds.valid) {
                sessionLow.volumeDensityClass = thresholds.ClassifyVolume(lowVolumeInBand);
            }
        }
    }

    /**
     * Reset tracking for new session.
     */
    void OnSessionReset() {
        sessionHigh.Reset();
        sessionLow.Reset();
        highAcceptance = ExtremeBehaviorState{};
        lowAcceptance = ExtremeBehaviorState{};
    }

private:
    ExtremeBehaviorState ComputeAcceptanceForLevel(const ExtremeLevel& level, bool isHigh) const {
        ExtremeBehaviorState state;

        if (!level.IsValid() || level.totalBarsEvaluated == 0) {
            return state;  // No data yet
        }

        // ====================================================================
        // TPO MINIMUM CHECK (AMT: single print is tentative, not acceptance)
        // ====================================================================
        state.meetsTPOMinimum = (level.barsNearExtreme >= config.minTPOsForAcceptance);

        // ====================================================================
        // 1. TAIL SIGNAL: Rejection rate at this level
        //    High rejection tail count = rejection signal
        // ====================================================================
        double rejectionRate = 0.0;
        if (level.barsNearExtreme > 0) {
            rejectionRate = static_cast<double>(level.rejectionTailCount) / level.barsNearExtreme;
        }
        // 0% rejection = +1.0, 50% = 0.0, 100% = -1.0
        state.tailScore = 1.0 - (rejectionRate * 2.0);

        // ====================================================================
        // 2. DELTA SIGNAL: Confirmation rate
        //    High delta confirmation = acceptance signal
        // ====================================================================
        double confirmRate = 0.0;
        if (level.barsNearExtreme > 0) {
            confirmRate = static_cast<double>(level.confirmingDeltaCount) / level.barsNearExtreme;
        }
        // 0% = -1.0, 50% = 0.0, 100% = +1.0
        state.deltaScore = (confirmRate * 2.0) - 1.0;

        // ====================================================================
        // 3. TIME SIGNAL: Proportion of time spent at level
        //    More bars near = more acceptance
        // ====================================================================
        double timeRatio = static_cast<double>(level.barsNearExtreme) / level.totalBarsEvaluated;
        // 50%+ of time at level = max score
        state.timeScore = (std::min)(1.0, timeRatio * 2.0);

        // ====================================================================
        // 4. RETEST SIGNAL: How did retests resolve?
        //    Retests that held = strong acceptance
        // ====================================================================
        state.retestScore = 0.0;
        if (level.retestCount > 0) {
            double heldRate = static_cast<double>(level.retestHeldCount) / level.retestCount;
            double rejectedRate = static_cast<double>(level.retestRejectedCount) / level.retestCount;
            state.retestScore = heldRate - rejectedRate;  // -1 to +1
        }

        // ====================================================================
        // 5. VOLUME SIGNAL: Volume density classification at extreme (from VbP)
        //    Uses VolumeThresholds sigma-based HVN/LVN classification
        //    HVN (high volume) = acceptance (value was found at this level)
        //    LVN (low volume) = rejection (price auctioned through quickly)
        //    NORMAL = neutral (typical volume, no strong signal)
        // ====================================================================
        state.hasVolumeData = level.HasVolumeData();
        if (state.hasVolumeData) {
            // Use density classification from VolumeThresholds
            switch (level.volumeDensityClass) {
                case VAPDensityClass::HIGH:
                    state.volumeScore = 1.0;   // HVN = strong acceptance
                    break;
                case VAPDensityClass::LOW:
                    state.volumeScore = -1.0;  // LVN = strong rejection
                    break;
                case VAPDensityClass::NORMAL:
                default:
                    state.volumeScore = 0.0;   // Neutral
                    break;
            }
        }

        // ====================================================================
        // WEIGHTED COMBINATION (with weight renormalization)
        // ====================================================================
        double totalWeight = config.tailWeight + config.deltaWeight +
                             config.timeWeight + config.retestWeight;

        // Include volume weight only if data is available
        if (state.hasVolumeData) {
            totalWeight += config.volumeWeight;
        }

        // Compute weighted score with renormalization
        if (totalWeight > 0.0) {
            double weightedSum = (state.tailScore * config.tailWeight) +
                                 (state.deltaScore * config.deltaWeight) +
                                 (state.timeScore * config.timeWeight) +
                                 (state.retestScore * config.retestWeight);

            if (state.hasVolumeData) {
                weightedSum += (state.volumeScore * config.volumeWeight);
            }

            state.score = weightedSum / totalWeight;  // Renormalize to handle missing volume
        }

        // Clamp to valid range
        state.score = std::clamp(state.score, -1.0, 1.0);

        // ====================================================================
        // ACCEPTANCE/REJECTION DECISION
        // AMT: Single print (< minTPOsForAcceptance) cannot be considered accepted
        // ====================================================================
        state.accepted = state.meetsTPOMinimum && (state.score >= config.acceptanceThreshold);
        state.rejected = (state.score <= config.rejectionThreshold);

        return state;
    }
};

// ============================================================================
// PHASE TRACKER (Hysteresis + History + Acceptance)
// ============================================================================

struct PhaseTracker {
    // Phase hysteresis
    CurrentPhase confirmedPhase = CurrentPhase::ROTATION;
    CurrentPhase candidatePhase = CurrentPhase::ROTATION;
    int candidateBars = 0;
    int minConfirmationBars = 3;       // Default for most phases
    int pullbackConfirmationBars = 2;  // PULLBACK is transient by AMT nature

    // NOTE: Market state (BALANCE/IMBALANCE) comes from Dalton SSOT
    // PhaseTracker only handles micro-phase hysteresis, not macro state

    // ========================================================================
    // INVARIANT OBSERVABILITY (diagnostic tracking, no behavioral impact)
    // Captures state changes for runtime invariant validation
    // ========================================================================
    struct UpdateDiagnostics {
        // Phase tracking
        CurrentPhase priorConfPhase = CurrentPhase::ROTATION;
        int phaseStreakBeforeUpdate = 0;
        int phaseStreakAfterUpdate = 0;
        int phaseThresholdUsed = 0;
        bool phaseConfirmedThisUpdate = false;
        bool phaseClampApplied = false;
        CurrentPhase phaseBeforeClamp = CurrentPhase::ROTATION;

        // Location at update time
        bool outsideVA = false;
        bool atBoundary = false;

        void Reset() {
            phaseConfirmedThisUpdate = false;
            phaseClampApplied = false;
        }
    };
    UpdateDiagnostics lastUpdateDiag;

    // Phase history for afterglow detection
    PhaseHistory history;

    // POC distance history for approaching detection
    POCDistanceHistory pocDistHistory;

    // Outside close tracker for acceptance
    OutsideCloseTracker outsideTracker;

    /**
     * Get required confirmation bars for a specific phase.
     * AMT SEMANTIC: PULLBACK is transient (1-3 bars typical on 1-min charts).
     * Using a lower threshold prevents systematic suppression.
     */
    int GetConfirmationBarsFor(CurrentPhase phase) const {
        if (phase == CurrentPhase::PULLBACK) {
            return pullbackConfirmationBars;  // Default: 2 bars
        }
        return minConfirmationBars;  // Default: 3 bars
    }

    /**
     * Update phase with hysteresis + AMT admissibility enforcement.
     * CRITICAL: Even after hysteresis, ROTATION is only valid inside VA.
     * Uses per-phase confirmation thresholds (PULLBACK = 2, others = 3).
     */
    CurrentPhase Update(CurrentPhase rawPhase, const PhasePrimitives& p) {
        // DIAGNOSTIC: Capture prior state
        lastUpdateDiag.priorConfPhase = confirmedPhase;
        lastUpdateDiag.phaseStreakBeforeUpdate = candidateBars;
        lastUpdateDiag.phaseConfirmedThisUpdate = false;
        lastUpdateDiag.phaseClampApplied = false;

        // Per-phase confirmation threshold
        int requiredBars = GetConfirmationBarsFor(rawPhase);
        lastUpdateDiag.phaseThresholdUsed = requiredBars;

        // Standard hysteresis with per-phase thresholds
        if (rawPhase == confirmedPhase) {
            candidatePhase = confirmedPhase;
            candidateBars = 0;
        }
        else if (rawPhase == candidatePhase) {
            candidateBars++;
            if (candidateBars >= requiredBars) {
                lastUpdateDiag.phaseConfirmedThisUpdate = true;
                confirmedPhase = candidatePhase;
                candidateBars = 0;
            }
        }
        else {
            candidatePhase = rawPhase;
            candidateBars = 1;
        }

        // DIAGNOSTIC: Capture streak after hysteresis (before clamp)
        lastUpdateDiag.phaseStreakAfterUpdate = candidateBars;
        lastUpdateDiag.phaseBeforeClamp = confirmedPhase;

        // AMT ADMISSIBILITY CLAMP (non-negotiable)
        bool outsideVA = (p.outsideLow || p.outsideHigh);
        bool atBoundary = (p.atVAL || p.atVAH);
        lastUpdateDiag.outsideVA = outsideVA;
        lastUpdateDiag.atBoundary = atBoundary;

        // If outside VA OR at boundary, confirmedPhase CANNOT be ROTATION
        // (ROTATION = balanced trade inside value, NOT at boundary)
        if ((outsideVA || atBoundary) && confirmedPhase == CurrentPhase::ROTATION) {
            lastUpdateDiag.phaseClampApplied = true;
            confirmedPhase = rawPhase;  // Use raw phase since it's AMT-valid
        }

        // If inside VA (not at boundary), confirmedPhase CANNOT be outside-only phases
        // Outside-only phases: DRIVING_UP/DOWN, RANGE_EXTENSION, PULLBACK, FAILED_AUCTION
        bool insideVA = !outsideVA && !atBoundary;
        if (insideVA) {
            if (confirmedPhase == CurrentPhase::DRIVING_UP ||
                confirmedPhase == CurrentPhase::DRIVING_DOWN ||
                confirmedPhase == CurrentPhase::RANGE_EXTENSION ||
                confirmedPhase == CurrentPhase::PULLBACK ||
                confirmedPhase == CurrentPhase::FAILED_AUCTION) {
                lastUpdateDiag.phaseClampApplied = true;
                confirmedPhase = rawPhase;  // Clamp to current bar's valid phase
            }
        }

        // Record to history
        history.Push(confirmedPhase);

        return confirmedPhase;
    }

    void UpdatePOCDistance(double dPOC_ticks) {
        pocDistHistory.Push(dPOC_ticks);
    }

    void UpdateOutsideClose(bool closeOutsideVA) {
        outsideTracker.Update(closeOutsideVA);
    }

    bool IsApproachingPOC(int lookbackBars) const {
        return pocDistHistory.IsContractingFor(lookbackBars);
    }

    bool WasDirectionalRecently(int windowBars) const {
        return history.WasDirectionalWithin(windowBars);
    }

    int GetOutsideCloseStreak() const {
        return outsideTracker.GetStreak();
    }

    bool JustReturnedFromOutside(int thresholdBars = 3) const {
        return outsideTracker.JustReturnedFromOutside(thresholdBars);
    }

    void ForcePhase(CurrentPhase phase) {
        confirmedPhase = phase;
        candidatePhase = phase;
        candidateBars = 0;
        history.Push(phase);
    }

    double GetConfirmationProgress() const {
        if (candidateBars == 0) return 0.0;
        return static_cast<double>(candidateBars) / minConfirmationBars;
    }

    int GetCandidateStreak() const {
        return candidateBars;
    }

    void Reset() {
        confirmedPhase = CurrentPhase::ROTATION;
        candidatePhase = CurrentPhase::ROTATION;
        candidateBars = 0;
        // NOTE: Market state (BALANCE/IMBALANCE) comes from Dalton SSOT
        // No regime state to reset here
        history.Reset();
        pocDistHistory.Reset();
        outsideTracker.Reset();
    }
};

// ============================================================================
// PHASE SNAPSHOT (Authoritative output)
// ============================================================================

struct PhaseSnapshot {
    // MARKET STATE (macro - from Dalton SSOT)
    // AMTMarketState is the unified state enum (BALANCE/IMBALANCE)
    // Derived from Dalton's 1TF/2TF time-framing analysis
    AMTMarketState marketState = AMTMarketState::BALANCE;

    // PHASE (micro - location/activity based)
    CurrentPhase phase = CurrentPhase::ROTATION;
    CurrentPhase rawPhase = CurrentPhase::ROTATION;

    // Hysteresis state
    int phaseStreak = 0;

    // Primitives reference
    PhasePrimitives primitives;

    // Context tags
    bool isOutsideVA = false;
    bool hasAcceptanceAfterglow = false;
    int barsSinceAcceptance = -1;
    int barsSinceFailure = -1;

    // Distance metrics
    double distFromPOCTicks = 0.0;
    double distFromBoundaryTicks = 0.0;
    double vaRangeTicks = 0.0;

    // Expansion evidence
    bool isAtSessionExtreme = false;
    bool isNearSessionExtreme = false;
    bool newExtremeRecently = false;
    bool isActivelyExpanding = false;

    // Decision tracing (AMT reason enum)
    PhaseReason phaseReason = PhaseReason::NONE;

    // Dalton decision support
    TradingBias bias = TradingBias::WAIT;
    VolumeConfirmation volumeConf = VolumeConfirmation::UNKNOWN;

    bool IsDirectional() const {
        // DRIVING_UP/DOWN and special events are directional
        return phase == CurrentPhase::DRIVING_UP ||
               phase == CurrentPhase::DRIVING_DOWN ||
               phase == CurrentPhase::RANGE_EXTENSION ||
               phase == CurrentPhase::FAILED_AUCTION;
    }

    bool IsAtBoundary() const {
        return phase == CurrentPhase::TESTING_BOUNDARY;
    }

    bool ShouldFollow() const {
        return bias == TradingBias::FOLLOW;
    }

    bool ShouldFade() const {
        return bias == TradingBias::FADE;
    }
};

// ============================================================================
// COMPUTE PHASE PRIMITIVES
// ============================================================================

inline PhasePrimitives ComputePhasePrimitives(
    const ZoneManager& zm,
    double currentPrice,
    double closePrice,
    double tickSize,
    int currentBar,
    const PhaseTracker& tracker,
    const ZoneConfig& config)
{
    PhasePrimitives p;
    p.bar = currentBar;
    p.price = currentPrice;
    p.closePrice = closePrice;
    p.tickSize = tickSize;

    const ZoneRuntime* vahZone = zm.GetZone(zm.vahId);
    const ZoneRuntime* valZone = zm.GetZone(zm.valId);
    const ZoneRuntime* pocZone = zm.GetZone(zm.pocId);

    if (!vahZone || !valZone || !pocZone || tickSize <= 0.0) {
        p.valid = false;
        return p;
    }

    // Value References
    p.vah = vahZone->GetAnchorPrice();
    p.val = valZone->GetAnchorPrice();
    p.poc = pocZone->GetAnchorPrice();

    if (p.vah <= 0.0 || p.val <= 0.0 || p.poc <= 0.0 || p.vah < p.val) {
        p.valid = false;
        return p;
    }

    p.vaRangeTicks = (p.vah - p.val) / tickSize;

    // Distance Metrics
    p.dPOC_ticks = GetExactTickDistance(currentPrice, p.poc, tickSize);
    p.dVAL_ticks = GetExactTickDistance(currentPrice, p.val, tickSize);
    p.dVAH_ticks = GetExactTickDistance(currentPrice, p.vah, tickSize);

    // Location Flags
    const double boundaryTol = static_cast<double>(config.boundaryToleranceTicks);

    p.outsideLow = (currentPrice < p.val);
    p.outsideHigh = (currentPrice > p.vah);
    p.insideVA = (!p.outsideLow && !p.outsideHigh);
    p.atVAL = (p.dVAL_ticks <= boundaryTol);
    p.atVAH = (p.dVAH_ticks <= boundaryTol);

    // Session Extreme State
    p.sessHi = zm.GetSessionHigh();
    p.sessLo = zm.GetSessionLow();

    if (p.sessHi > 0.0) {
        p.dSessHi_ticks = GetExactTickDistance(currentPrice, p.sessHi, tickSize);
    } else {
        p.dSessHi_ticks = 9999.0;
    }

    if (p.sessLo > 0.0) {
        p.dSessLo_ticks = GetExactTickDistance(currentPrice, p.sessLo, tickSize);
    } else {
        p.dSessLo_ticks = 9999.0;
    }

    const double nearThresh = static_cast<double>(config.nearExtremeTicks);
    p.nearSessionExtreme = (p.dSessHi_ticks <= nearThresh) || (p.dSessLo_ticks <= nearThresh);

    const int extremeWindow = config.extremeUpdateWindowBars;
    p.madeNewHighRecently = zm.IsHighUpdatedRecently(currentBar, extremeWindow);
    p.madeNewLowRecently = zm.IsLowUpdatedRecently(currentBar, extremeWindow);

    // Acceptance Signals (from tracker)
    p.outsideCloseStreak = tracker.GetOutsideCloseStreak();
    p.acceptanceOutsideVA = (p.outsideCloseStreak >= config.acceptanceClosesRequired);

    // Approach/Reversion
    p.approachingPOC = tracker.IsApproachingPOC(config.approachingPOCLookback);

    // Directional Memory
    p.wasDirectionalRecently = tracker.WasDirectionalRecently(config.directionalAfterglowBars);

    // Failure Recency
    p.barsSinceFailure = -1;
    if (vahZone->lastFailureBar >= 0) {
        int bars = currentBar - vahZone->lastFailureBar;
        if (p.barsSinceFailure < 0 || bars < p.barsSinceFailure) {
            p.barsSinceFailure = bars;
        }
    }
    if (valZone->lastFailureBar >= 0) {
        int bars = currentBar - valZone->lastFailureBar;
        if (p.barsSinceFailure < 0 || bars < p.barsSinceFailure) {
            p.barsSinceFailure = bars;
        }
    }
    p.failureRecent = (p.barsSinceFailure >= 0 && p.barsSinceFailure < config.failedAuctionRecencyBars);

    // Return-to-Value State (for FAILED_AUCTION admissibility)
    // Use a 3-bar window for "just returned" - this is the transition period
    // where FAILED_AUCTION phase is semantically valid inside VA
    p.justReturnedFromOutside = tracker.JustReturnedFromOutside(3);

    p.valid = true;
    return p;
}

// ============================================================================
// COMPUTE RAW PHASE (Micro - location/activity based)
// ============================================================================
//
// NOTE (Dec 2024 Migration):
// The legacy AuctionRegime four-phase cycle (BALANCE→IMBALANCE→EXCESS→REBALANCE)
// has been removed. Market state (BALANCE/IMBALANCE) now comes from Dalton's
// 1TF/2TF time-framing analysis as the SSOT (AMTMarketState enum).
//
// The mapping was:
//   - EXCESS → CurrentPhase::FAILED_AUCTION
//   - REBALANCE → CurrentPhase::PULLBACK within IMBALANCE state
//   - BALANCE/IMBALANCE → AMTMarketState from Dalton
// ============================================================================

/**
 * Compute raw phase using priority-based rules.
 *
 * PRIORITY ORDER (AMT-aligned):
 *   1. FAILED_AUCTION   - failureRecent OR (outsideVA && extremeRejected)
 *   2. TESTING_BOUNDARY - atVAL || atVAH
 *   3. RANGE_EXTENSION  - outsideVA && madeNewExtremeRecently && extremeAccepted && !approachingPOC
 *   4. PULLBACK         - outsideVA && approachingPOC && wasDirectionalRecently
 *   5. DRIVING_UP/DOWN  - outsideVA (default for outside VA - directional conviction)
 *   6. ROTATION         - insideVA && !atVAL && !atVAH
 *
 * AMT INVARIANT: ROTATION is ONLY returned if insideVA && !atVAL && !atVAH
 *
 * EXTREME ACCEPTANCE (AMT-aligned, from ExtremeAcceptanceTracker):
 *   Acceptance/rejection is determined by five independent signals:
 *     1. TAIL RATIO      - Bar structure showing excess (rejection) or control (acceptance)
 *     2. DELTA DIRECTION - Volume pushing toward (acceptance) or away (rejection) from extreme
 *     3. TIME AT PRICE   - Duration spent near extreme (TPO-like, cumulative)
 *     4. RETEST OUTCOMES - Returns after departure that held (acceptance) or rejected
 *     5. VOLUME DENSITY  - HVN at extreme = acceptance, LVN = rejection
 *
 *   The initial bar that makes a new extreme is just DISCOVERY.
 *   Acceptance/rejection is determined by SUBSEQUENT behavior, especially RETESTS.
 *
 *   - extremeAccepted: Composite score >= 0.35 → RANGE_EXTENSION eligible
 *   - extremeRejected: Composite score <= -0.25 → FAILED_AUCTION
 *   - In between: DRIVING_UP/DRIVING_DOWN (default outside VA behavior)
 *
 * @deprecated SSOT VIOLATION (Dec 2024): Use DaltonState.DeriveCurrentPhase() instead.
 * This function computes phase independently from Dalton, violating SSOT.
 * It is retained ONLY for test compatibility during transition.
 * Production code MUST use daltonPhase from DaltonState.DeriveCurrentPhase().
 */
[[deprecated("Use DaltonState.DeriveCurrentPhase() as SSOT for CurrentPhase")]]
inline CurrentPhase ComputeRawPhase(const PhasePrimitives& p,
                                    const ZoneConfig& config,
                                    const char*& outReason)
{
    if (!p.valid) {
        outReason = "VA_INPUTS_INVALID";
        return CurrentPhase::UNKNOWN;
    }

    bool outsideVA = (p.outsideLow || p.outsideHigh);
    bool atBoundary = (p.atVAL || p.atVAH);
    bool insideVAMidRange = !outsideVA && !atBoundary;

    // PRIORITY 1: FAILED_AUCTION
    // AMT SEMANTIC: FAILED_AUCTION represents auction failure - responsive activity
    // rejected the probe. Detected by:
    //   a) Recent failure event (failureRecent from zone state), OR
    //   b) Outside VA with rejected extreme (from ExtremeAcceptanceTracker)
    //
    // Admissibility constraint: Not valid deep inside VA after time has passed.
    const bool extremeRejected = p.currentExtremeRejected();
    if (p.failureRecent || (outsideVA && extremeRejected)) {
        bool failedAuctionAdmissible = atBoundary || outsideVA || p.justReturnedFromOutside;
        if (failedAuctionAdmissible) {
            outReason = extremeRejected ? "EXTREME_REJECTED" : "FAILED_AUCTION_RECENT";
            return CurrentPhase::FAILED_AUCTION;
        }
        // failureRecent is true but not admissible as phase - fall through to normal logic
    }

    // PRIORITY 2: TESTING_BOUNDARY
    if (atBoundary) {
        outReason = p.atVAH ? "AT_VAH" : "AT_VAL";
        return CurrentPhase::TESTING_BOUNDARY;
    }

    bool madeNewExtremeRecently = (p.madeNewHighRecently || p.madeNewLowRecently);

    // PRIORITY 3: RANGE_EXTENSION
    // AMT SEMANTIC: Extension requires ACCEPTED expansion at the range frontier.
    // Acceptance is determined by ExtremeAcceptanceTracker using:
    //   - Tail ratio (no rejection tail)
    //   - Delta direction (volume toward extreme)
    //   - Time at price (TPO-like accumulation)
    //   - Retest outcomes (returns that held)
    //
    // If approachingPOC is true, price is retracing toward value = NOT extending.
    if (outsideVA && madeNewExtremeRecently && !p.approachingPOC) {
        const bool extremeAccepted = p.currentExtremeAccepted();
        if (extremeAccepted) {
            outReason = p.outsideHigh ? "RANGE_EXT_HIGH_ACCEPTED" : "RANGE_EXT_LOW_ACCEPTED";
            return CurrentPhase::RANGE_EXTENSION;
        }
    }

    // PRIORITY 4: PULLBACK
    // AMT SEMANTIC: Retracement toward value after directional move.
    // Checked before DRIVING because approachingPOC is a specific condition.
    if (outsideVA && p.approachingPOC && p.wasDirectionalRecently) {
        outReason = "PULLBACK_TO_VALUE";
        return CurrentPhase::PULLBACK;
    }

    // PRIORITY 5: DRIVING (default for outside VA)
    // AMT SEMANTIC: DRIVING represents sustained directional conviction outside value.
    // This is the default phase for any price outside VA that isn't:
    //   - At a rejected extreme (→ FAILED_AUCTION)
    //   - Making a new accepted extreme (→ RANGE_EXTENSION)
    //   - Pulling back toward POC (→ PULLBACK)
    //
    // Note: Distance from POC is not required - being outside VA IS the signal.
    // The market has already accepted price outside value; that's conviction.
    if (outsideVA) {
        if (p.outsideHigh) {
            outReason = "DRIVING_ABOVE_VA";
            return CurrentPhase::DRIVING_UP;
        } else {
            outReason = "DRIVING_BELOW_VA";
            return CurrentPhase::DRIVING_DOWN;
        }
    }

    // PRIORITY 6: ROTATION (inside VA, not at boundary)
    outReason = "INSIDE_VALUE_DEFAULT";
    return CurrentPhase::ROTATION;
}

// ============================================================================
// BUILD PHASE SNAPSHOT (Authoritative Decision Locus)
// ============================================================================
// MIGRATION NOTE (Dec 2024):
// daltonState is the SSOT for market state (BALANCE/IMBALANCE) from Dalton's
// 1TF/2TF time-framing analysis. The legacy AuctionRegime four-phase cycle
// (BALANCE→IMBALANCE→EXCESS→REBALANCE) has been removed:
//   - EXCESS is now CurrentPhase::FAILED_AUCTION
//   - REBALANCE is now CurrentPhase::PULLBACK within IMBALANCE state
//   - BALANCE/IMBALANCE use AMTMarketState from Dalton
//
// SSOT UNIFICATION (Dec 2024):
// daltonPhase is NOW the SSOT for CurrentPhase. Previously ComputeRawPhase()
// computed phase independently - this created conflicting phase values in logs.
// Now DaltonState.DeriveCurrentPhase() is the single authoritative source.
// PhaseTracker applies hysteresis only, not independent phase computation.
// ============================================================================

inline PhaseSnapshot BuildPhaseSnapshot(
    const ZoneManager& zm,
    double currentPrice,
    double closePrice,
    double tickSize,
    int currentBar,
    PhaseTracker& tracker,
    AMTMarketState daltonState,  // SSOT from Dalton 1TF/2TF
    CurrentPhase daltonPhase,    // SSOT from DaltonState.DeriveCurrentPhase()
    PhaseReason daltonReason,    // SSOT from DaltonState.DerivePhaseReason()
    TradingBias daltonBias = TradingBias::WAIT,           // SSOT from DaltonState.DeriveTradingBias()
    VolumeConfirmation daltonVolConf = VolumeConfirmation::UNKNOWN)  // SSOT from volume baseline
{
    PhaseSnapshot snap;

    // Set market state from Dalton (SSOT)
    snap.marketState = daltonState;
    snap.bias = daltonBias;
    snap.volumeConf = daltonVolConf;

    // Compute primitives
    snap.primitives = ComputePhasePrimitives(zm, currentPrice, closePrice, tickSize,
                                              currentBar, tracker, zm.config);

    if (!snap.primitives.valid) {
        // AMT: Invalid VA inputs → UNKNOWN (no fallback, no CORE_VA assumption)
        snap.phase = CurrentPhase::UNKNOWN;
        snap.rawPhase = CurrentPhase::UNKNOWN;
        snap.marketState = AMTMarketState::UNKNOWN;
        snap.phaseReason = PhaseReason::NONE;  // Invalid inputs
        return snap;
    }

    PhasePrimitives& p = snap.primitives;

    // Update trackers with TOLERANCE-AWARE acceptance logic
    // AMT: "Outside" for acceptance streak must be beyond boundary tolerance
    // to avoid counting 1-2 tick oscillation as acceptance
    double boundaryTol = static_cast<double>(zm.config.boundaryToleranceTicks) * tickSize;
    bool closeOutsideBeyondTolerance = (closePrice > p.vah + boundaryTol) ||
                                        (closePrice < p.val - boundaryTol);
    tracker.UpdateOutsideClose(closeOutsideBeyondTolerance);
    tracker.UpdatePOCDistance(p.dPOC_ticks);

    // Recompute with updated tracker state
    p.outsideCloseStreak = tracker.GetOutsideCloseStreak();
    p.acceptanceOutsideVA = (p.outsideCloseStreak >= zm.config.acceptanceClosesRequired);
    p.approachingPOC = tracker.IsApproachingPOC(zm.config.approachingPOCLookback);

    // Market state is already set from daltonState (SSOT)

    // ========================================================================
    // PHASE DETERMINATION (SSOT: Dalton's DeriveCurrentPhase)
    // ========================================================================
    // daltonPhase MUST be provided - no fallback to ComputeRawPhase.
    // PhaseTracker applies hysteresis only, not independent phase computation.
    // ========================================================================
    // Accept UNKNOWN phase when in IMBALANCE (use DRIVING_UP/DOWN)
    // But require actual phase input when in BALANCE
    if (daltonPhase == CurrentPhase::UNKNOWN && snap.marketState == AMTMarketState::BALANCE) {
        // Phase input required for BALANCE
        snap.phase = CurrentPhase::UNKNOWN;
        snap.rawPhase = CurrentPhase::UNKNOWN;
        snap.phaseReason = PhaseReason::NONE;
        return snap;  // Return early with error state
    }
    snap.rawPhase = daltonPhase;
    snap.phaseReason = daltonReason;  // AMT reason from Dalton
    snap.phase = tracker.Update(snap.rawPhase, p);  // Apply hysteresis
    snap.phaseStreak = tracker.GetCandidateStreak();

    // ========================================================================
    // AMT INVARIANT CLAMP (mandatory enforcement)
    // Hysteresis can temporarily output ROTATION from warmup, but we must
    // NEVER output ROTATION when market state is IMBALANCE (AMT violation).
    // Clamp to rawPhase if hysteresis violates AMT invariants.
    // ========================================================================
    if (snap.marketState == AMTMarketState::IMBALANCE &&
        snap.phase == CurrentPhase::ROTATION) {
        // Hysteresis trying to output ROTATION in IMBALANCE - clamp to raw phase
        snap.phase = snap.rawPhase;
        // Note: phaseReason already set from daltonReason, keep it
    }

    // ========================================================================
    // AMT CONSISTENCY CONSTRAINT (debug assertion)
    // BALANCE state → phase ∈ {ROTATION, TESTING_BOUNDARY, FAILED_AUCTION}
    // IMBALANCE state → phase != ROTATION (outside VA phases)
    //
    // Note: Only assert after hysteresis warmup (candidateBars > 0).
    // PhaseTracker defaults to ROTATION, so first few bars may output
    // ROTATION from hysteresis even when raw phase differs.
    // ========================================================================
#ifndef NDEBUG
    if (tracker.candidateBars > 0) {  // Only after warmup
        if (snap.marketState == AMTMarketState::BALANCE) {
            assert((snap.phase == CurrentPhase::ROTATION ||
                    snap.phase == CurrentPhase::TESTING_BOUNDARY ||
                    snap.phase == CurrentPhase::FAILED_AUCTION) &&
                   "AMT CONSISTENCY: BALANCE state but phase not in {ROTATION, TESTING_BOUNDARY, FAILED_AUCTION}");
        }
        if (snap.marketState == AMTMarketState::IMBALANCE) {
            assert(snap.phase != CurrentPhase::ROTATION &&
                   "AMT CONSISTENCY: IMBALANCE state but phase is ROTATION");
        }
    }
#endif

    // Populate derived fields
    snap.isOutsideVA = (p.outsideLow || p.outsideHigh);
    snap.distFromPOCTicks = p.dPOC_ticks;
    snap.vaRangeTicks = p.vaRangeTicks;
    snap.barsSinceFailure = p.barsSinceFailure;

    if (p.outsideHigh) {
        snap.distFromBoundaryTicks = p.dVAH_ticks;
    } else if (p.outsideLow) {
        snap.distFromBoundaryTicks = p.dVAL_ticks;
    }

    snap.isAtSessionExtreme = (p.dSessHi_ticks <= 0.5) || (p.dSessLo_ticks <= 0.5);
    snap.isNearSessionExtreme = p.nearSessionExtreme;
    snap.newExtremeRecently = (p.madeNewHighRecently || p.madeNewLowRecently);
    snap.isActivelyExpanding = snap.isOutsideVA && snap.isNearSessionExtreme && snap.newExtremeRecently;

    // Acceptance afterglow
    const ZoneRuntime* vahZone = zm.GetZone(zm.vahId);
    const ZoneRuntime* valZone = zm.GetZone(zm.valId);

    if (p.outsideHigh && vahZone && vahZone->lastAcceptanceBar >= 0) {
        snap.barsSinceAcceptance = currentBar - vahZone->lastAcceptanceBar;
        snap.hasAcceptanceAfterglow = (snap.barsSinceAcceptance < zm.config.directionalAfterglowBars);
    }
    else if (p.outsideLow && valZone && valZone->lastAcceptanceBar >= 0) {
        snap.barsSinceAcceptance = currentBar - valZone->lastAcceptanceBar;
        snap.hasAcceptanceAfterglow = (snap.barsSinceAcceptance < zm.config.directionalAfterglowBars);
    }

    return snap;
}

// ============================================================================
// LEGACY API WRAPPERS - REMOVED (Dec 2024 SSOT Unification)
// ============================================================================
// All legacy BuildPhaseSnapshot overloads have been REMOVED.
// daltonPhase is now REQUIRED - no fallback to ComputeRawPhase.
//
// If you see a compile error here, update your call site to:
//   BuildPhaseSnapshot(zm, currentPrice, closePrice, tickSize, currentBar,
//                      tracker, daltonState, daltonPhase)
//
// Where:
//   daltonState = st->lastDaltonState.marketState
//   daltonPhase = st->lastDaltonState.DeriveCurrentPhase()
// ============================================================================

inline std::string GetPhaseDescription(CurrentPhase phase,
                                       const PhaseTracker& tracker)
{
    std::string name = CurrentPhaseToString(phase);

    if (tracker.candidatePhase != tracker.confirmedPhase) {
        double progress = tracker.GetConfirmationProgress() * 100.0;
        name += std::string(" (transitioning to ") +
                CurrentPhaseToString(tracker.candidatePhase) +
                " " + std::to_string(static_cast<int>(progress)) + "%)";
    }

    return name;
}

// ============================================================================
// TELEMETRY FORMATTING
// ============================================================================

inline std::string FormatStatePhaseTelemetry(const PhaseSnapshot& snap,
                                              const PhaseTracker& tracker)
{
    char buf[384];
    snprintf(buf, sizeof(buf),
        "STATE: %s | PHASE: %s | bias=%s | vol=%s | reason=%s",
        AMTMarketStateToString(snap.marketState),
        CurrentPhaseToString(snap.phase),
        TradingBiasToString(snap.bias),
        VolumeConfirmationToString(snap.volumeConf),
        PhaseReasonToString(snap.phaseReason));
    return std::string(buf);
}

// Legacy alias for backward compatibility during migration
inline std::string FormatRegimePhaseTelemetry(const PhaseSnapshot& snap,
                                               const PhaseTracker& tracker)
{
    return FormatStatePhaseTelemetry(snap, tracker);
}

inline std::string FormatPrimitivesCompact(const PhasePrimitives& p)
{
    char buf[384];
    snprintf(buf, sizeof(buf),
        "P=%.2f POC=%.2f VAH=%.2f VAL=%.2f | inVA=%d atVAL=%d atVAH=%d | "
        "dPOC=%.1f vaRange=%.1f | outsideStreak=%d accepted=%d",
        p.price, p.poc, p.vah, p.val,
        p.insideVA ? 1 : 0, p.atVAL ? 1 : 0, p.atVAH ? 1 : 0,
        p.dPOC_ticks, p.vaRangeTicks,
        p.outsideCloseStreak, p.acceptanceOutsideVA ? 1 : 0);
    return std::string(buf);
}

// ============================================================================
// PHASE INVARIANT VALIDATION (diagLevel >= 3)
// Runtime log-only checks for state machine invariants.
// No behavioral impact - observability only.
// ============================================================================
//
// NOTE (Dec 2024 Migration):
// Market state (BALANCE/IMBALANCE) now comes from Dalton SSOT, not from
// hysteresis tracking. Only phase invariants are validated here.
// ============================================================================

/**
 * Validate phase invariants and log any violations.
 *
 * Invariants checked:
 *   [PHASE-INVAR] P01: CONF_AT_STREAK - phase confirmation only at streak >= threshold
 *   [PHASE-INVAR] P02: LOC_ADMIT_PHASE - phase admissible for current VA location
 *   [PHASE-INVAR] X01: BALANCE_PHASES - BALANCE state → phase ∈ {ROTATION, TESTING_BOUNDARY}
 *   [PHASE-INVAR] X02: IMBALANCE_NO_ROTATION - IMBALANCE state → phase ≠ ROTATION
 *
 * @param snap Current PhaseSnapshot (after update)
 * @param tracker PhaseTracker with diagnostic state
 * @param bar Current bar index
 * @param sc Sierra Chart interface for logging
 * @param diagLevel Diagnostic level (checks only run at >= 3)
 */
inline void ValidatePhaseInvariants(
    const PhaseSnapshot& snap,
    const PhaseTracker& tracker,
    int bar,
    SCStudyInterfaceRef sc,
    int diagLevel)
{
    if (diagLevel < 3) return;

    const auto& diag = tracker.lastUpdateDiag;

    // ========================================================================
    // P01: PHASE CONF_AT_STREAK
    // If phase was confirmed this update, streak should have been >= threshold
    // ========================================================================
    if (diag.phaseConfirmedThisUpdate) {
        // When confirmed, the streak that triggered it should be >= threshold
        // Note: streakBeforeUpdate is the streak BEFORE increment on this bar
        // Confirmation happens when streakBeforeUpdate+1 >= threshold
        int effectiveStreak = diag.phaseStreakBeforeUpdate + 1;
        if (effectiveStreak < diag.phaseThresholdUsed) {
            SCString msg;
            msg.Format("[PHASE-INVAR] bar=%d P01:CONF_AT_STREAK | "
                       "priorConf=%s newConf=%s raw=%s | streak=%d+1=%d < thr=%d",
                bar,
                CurrentPhaseToString(diag.priorConfPhase),
                CurrentPhaseToString(snap.phase),
                CurrentPhaseToString(snap.rawPhase),
                diag.phaseStreakBeforeUpdate, effectiveStreak,
                diag.phaseThresholdUsed);
            sc.AddMessageToLog(msg, 1);
        }
    }

    // ========================================================================
    // P02: LOC_ADMIT_PHASE
    // After clamp, phase should be admissible for current VA location
    // ========================================================================
    bool outsideVA = diag.outsideVA;
    bool atBoundary = diag.atBoundary;
    bool insideVA = !outsideVA && !atBoundary;

    // Outside or at boundary: ROTATION not allowed
    if ((outsideVA || atBoundary) && snap.phase == CurrentPhase::ROTATION) {
        SCString msg;
        msg.Format("[PHASE-INVAR] bar=%d P02:LOC_ADMIT_PHASE | "
                   "phase=ROTATION but outsideVA=%d atBound=%d | "
                   "raw=%s clamp=%d",
            bar, outsideVA ? 1 : 0, atBoundary ? 1 : 0,
            CurrentPhaseToString(snap.rawPhase),
            diag.phaseClampApplied ? 1 : 0);
        sc.AddMessageToLog(msg, 1);
    }

    // Inside VA: outside-only phases not allowed
    if (insideVA) {
        if (snap.phase == CurrentPhase::RANGE_EXTENSION ||
            snap.phase == CurrentPhase::DRIVING_UP ||
            snap.phase == CurrentPhase::DRIVING_DOWN ||
            snap.phase == CurrentPhase::PULLBACK ||
            snap.phase == CurrentPhase::FAILED_AUCTION) {
            SCString msg;
            msg.Format("[PHASE-INVAR] bar=%d P02:LOC_ADMIT_PHASE | "
                       "phase=%s but insideVA=1 | raw=%s clamp=%d",
                bar, CurrentPhaseToString(snap.phase),
                CurrentPhaseToString(snap.rawPhase),
                diag.phaseClampApplied ? 1 : 0);
            sc.AddMessageToLog(msg, 1);
        }
    }

    // ========================================================================
    // X01: BALANCE_PHASES
    // BALANCE state → phase ∈ {ROTATION, TESTING_BOUNDARY}
    // Note: Market state comes from Dalton SSOT
    // ========================================================================
    if (snap.marketState == AMTMarketState::BALANCE) {
        if (snap.phase != CurrentPhase::ROTATION &&
            snap.phase != CurrentPhase::TESTING_BOUNDARY) {
            SCString msg;
            msg.Format("[PHASE-INVAR] bar=%d X01:BALANCE_PHASES | "
                       "state=BALANCE but phase=%s (expected ROTATION/TESTING_BOUNDARY) | "
                       "rawPh=%s",
                bar, CurrentPhaseToString(snap.phase),
                CurrentPhaseToString(snap.rawPhase));
            sc.AddMessageToLog(msg, 1);
        }
    }

    // ========================================================================
    // X02: IMBALANCE_NO_ROTATION
    // IMBALANCE state → phase ≠ ROTATION
    // Note: Market state comes from Dalton SSOT
    // ========================================================================
    if (snap.marketState == AMTMarketState::IMBALANCE) {
        if (snap.phase == CurrentPhase::ROTATION) {
            SCString msg;
            msg.Format("[PHASE-INVAR] bar=%d X02:IMBALANCE_NO_ROTATION | "
                       "state=IMBALANCE but phase=ROTATION | rawPh=%s",
                bar, CurrentPhaseToString(snap.rawPhase));
            sc.AddMessageToLog(msg, 1);
        }
    }
}

// Legacy alias for backward compatibility during migration
inline void ValidatePhaseRegimeInvariants(
    const PhaseSnapshot& snap,
    const PhaseTracker& tracker,
    int bar,
    SCStudyInterfaceRef sc,
    int diagLevel)
{
    ValidatePhaseInvariants(snap, tracker, bar, sc, diagLevel);
}

} // namespace AMT

#endif // AMT_PHASE_H
