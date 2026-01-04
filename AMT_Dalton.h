// ============================================================================
// AMT_Dalton.h
// Dalton's Auction Market Theory Framework Implementation
// ============================================================================
//
// Based on Jim Dalton's Market Profile framework from "Mind Over Markets"
// and "Markets in Profile", implementing proper categorical separation:
//
// 1. MARKET PHASES: Balance vs Imbalance (Trending)
// 2. TIMEFRAME PATTERNS: One-Time Framing (1TF) vs Two-Time Framing (2TF)
// 3. ACTIVITY TYPES: Initiative vs Responsive
// 4. STRUCTURAL FEATURES: Initial Balance, Range Extension, Excess
// 5. MARKET EVENTS: Failed Auction
//
// Key insight: 1TF/2TF is the DETECTION MECHANISM for Balance/Imbalance.
// - 1TF (consecutive HH or LL) indicates IMBALANCE (one side in control)
// - 2TF (overlapping periods) indicates BALANCE (both sides active)
//
// Sources:
// - https://www.shadowtrader.net/glossary/one-time-framing/
// - https://www.tradingview.com/script/Xor6V4C2-Rotation-Factor-for-TPO-and-OHLC-Plot/
// - https://www.sierrachart.com/index.php?page=doc/StudiesReference.php&ID=445
// - https://tradingriot.com/market-profile/
// ============================================================================

#ifndef AMT_DALTON_H
#define AMT_DALTON_H

#include "amt_core.h"
#include "AMT_Signals.h"   // For ActivityClassifier
#include "AMT_DayType.h"   // For DaltonDayType, DayTypeClassifier (merged)
#include <vector>
#include <algorithm>
#include <cmath>

namespace AMT {

// ============================================================================
// TIMEFRAME PATTERN (Detection Mechanism for Balance/Imbalance)
// ============================================================================
// One-Time Framing: Each low > prev low (1TF Up) OR each high < prev high (1TF Down)
// Two-Time Framing: Overlapping periods (neither pure 1TF up nor 1TF down)
// ============================================================================

enum class TimeframePattern : int {
    UNKNOWN = 0,
    ONE_TIME_FRAMING_UP = 1,    // Each low > prev low (buyers in control)
    ONE_TIME_FRAMING_DOWN = 2,  // Each high < prev high (sellers in control)
    TWO_TIME_FRAMING = 3        // Overlapping (both sides active)
};

inline const char* TimeframePatternToString(TimeframePattern pattern) {
    switch (pattern) {
        case TimeframePattern::UNKNOWN:               return "UNKNOWN";
        case TimeframePattern::ONE_TIME_FRAMING_UP:   return "1TF_UP";
        case TimeframePattern::ONE_TIME_FRAMING_DOWN: return "1TF_DOWN";
        case TimeframePattern::TWO_TIME_FRAMING:      return "2TF";
        default:                                      return "INVALID";
    }
}

// ============================================================================
// DAY TYPE - MOVED TO AMT_DayType.h
// ============================================================================
// DaltonDayType enum and DaltonDayTypeToString() are now in AMT_DayType.h
// to consolidate with the existing DayTypeClassifier.
// ============================================================================

// ============================================================================
// RANGE EXTENSION TYPE
// ============================================================================
// Which side broke Initial Balance
// ============================================================================

enum class RangeExtensionType : int {
    NONE = 0,
    BUYING = 1,      // Extended above IB (buyers in control)
    SELLING = 2,     // Extended below IB (sellers in control)
    BOTH = 3         // Extended both sides (neutral day pattern)
};

inline const char* RangeExtensionTypeToString(RangeExtensionType type) {
    switch (type) {
        case RangeExtensionType::NONE:    return "NONE";
        case RangeExtensionType::BUYING:  return "BUY_EXT";
        case RangeExtensionType::SELLING: return "SELL_EXT";
        case RangeExtensionType::BOTH:    return "BOTH_EXT";
        default:                          return "INVALID";
    }
}

// ============================================================================
// ROTATION TRACKER
// ============================================================================
// Tracks rotation factor and detects One-Time Framing vs Two-Time Framing.
//
// Rotation Factor per bar (Sierra Chart formula):
//   +1 if High > prev High
//   +1 if Low > prev Low
//   -1 if High < prev High
//   -1 if Low < prev Low
//   Range: -2 to +2 per bar
//
// One-Time Framing Detection:
//   1TF Up: Consecutive bars where low > prev low
//   1TF Down: Consecutive bars where high < prev high
//   2TF: Bars overlap (neither pure 1TF)
// ============================================================================

class RotationTracker {
public:
    struct Config {
        int minConsecutiveBars = 2;     // Minimum consecutive bars to confirm 1TF
        int lookbackBars = 6;           // Bars to analyze for pattern
        int periodMinutes = 30;         // TPO period duration (standard: 30 min)

        Config() = default;
    };

    struct PeriodData {
        double high = 0.0;
        double low = 0.0;
        int barIndex = 0;
        bool valid = false;
    };

    struct RotationResult {
        TimeframePattern pattern = TimeframePattern::UNKNOWN;
        int rotationFactor = 0;           // Cumulative rotation for session
        int consecutiveUp = 0;            // Consecutive 1TF up bars
        int consecutiveDown = 0;          // Consecutive 1TF down bars
        int lastBarRotation = 0;          // Rotation of most recent bar (-2 to +2)
        bool is1TF = false;               // True if currently one-time framing
        bool is2TF = false;               // True if currently two-time framing
    };

    RotationTracker() : config_() {}
    explicit RotationTracker(const Config& cfg) : config_(cfg) {}

    /**
     * Update rotation tracking with new bar data.
     * Call this with each 30-minute bar (or configured period).
     *
     * @param high      Current bar high
     * @param low       Current bar low
     * @param barIndex  Current bar index
     * @return          RotationResult with current pattern and metrics
     */
    RotationResult Update(double high, double low, int barIndex) {
        RotationResult result;

        // Add current period
        PeriodData current;
        current.high = high;
        current.low = low;
        current.barIndex = barIndex;
        current.valid = true;

        periods_.push_back(current);

        // Keep only lookback window
        while (periods_.size() > static_cast<size_t>(config_.lookbackBars + 1)) {
            periods_.erase(periods_.begin());
        }

        // Need at least 2 periods for comparison
        if (periods_.size() < 2) {
            result.pattern = TimeframePattern::UNKNOWN;
            return result;
        }

        // Calculate last bar rotation
        const PeriodData& prev = periods_[periods_.size() - 2];
        const PeriodData& curr = periods_[periods_.size() - 1];

        int barRotation = 0;
        if (curr.high > prev.high) barRotation += 1;
        if (curr.high < prev.high) barRotation -= 1;
        if (curr.low > prev.low) barRotation += 1;
        if (curr.low < prev.low) barRotation -= 1;

        result.lastBarRotation = barRotation;
        sessionRotation_ += barRotation;
        result.rotationFactor = sessionRotation_;

        // Detect timeframe pattern
        DetectTimeframePattern(result);

        return result;
    }

    /**
     * Reset for new session.
     */
    void Reset() {
        periods_.clear();
        sessionRotation_ = 0;
    }

    // Getters
    int GetSessionRotation() const { return sessionRotation_; }
    const std::vector<PeriodData>& GetPeriods() const { return periods_; }

private:
    Config config_;
    std::vector<PeriodData> periods_;
    int sessionRotation_ = 0;

    void DetectTimeframePattern(RotationResult& result) {
        if (periods_.size() < 2) {
            result.pattern = TimeframePattern::UNKNOWN;
            return;
        }

        // Count consecutive 1TF up (each low > prev low)
        int consUp = 0;
        for (size_t i = periods_.size() - 1; i > 0; --i) {
            if (periods_[i].low > periods_[i-1].low) {
                consUp++;
            } else {
                break;
            }
        }

        // Count consecutive 1TF down (each high < prev high)
        int consDown = 0;
        for (size_t i = periods_.size() - 1; i > 0; --i) {
            if (periods_[i].high < periods_[i-1].high) {
                consDown++;
            } else {
                break;
            }
        }

        result.consecutiveUp = consUp;
        result.consecutiveDown = consDown;

        // Determine pattern
        if (consUp >= config_.minConsecutiveBars) {
            result.pattern = TimeframePattern::ONE_TIME_FRAMING_UP;
            result.is1TF = true;
            result.is2TF = false;
        }
        else if (consDown >= config_.minConsecutiveBars) {
            result.pattern = TimeframePattern::ONE_TIME_FRAMING_DOWN;
            result.is1TF = true;
            result.is2TF = false;
        }
        else {
            result.pattern = TimeframePattern::TWO_TIME_FRAMING;
            result.is1TF = false;
            result.is2TF = true;
        }
    }
};

// ============================================================================
// INITIAL BALANCE TRACKER
// ============================================================================
// Tracks the first 60 minutes (A+B periods) of RTH trading.
// Detects range extensions beyond IB.
// ============================================================================

class InitialBalanceTracker {
public:
    struct Config {
        int ibDurationMinutes = 60;     // Standard: 60 minutes (2 x 30-min periods)
        double extensionMultiple = 2.0; // Range extension threshold for day type

        Config() = default;
    };

    struct IBState {
        double ibHigh = 0.0;
        double ibLow = 0.0;
        double ibRange = 0.0;           // ibHigh - ibLow
        bool ibComplete = false;         // True after IB period ends

        // Extension tracking
        double sessionHigh = 0.0;
        double sessionLow = 0.0;
        double sessionRange = 0.0;

        RangeExtensionType extension = RangeExtensionType::NONE;
        double extensionAboveIB = 0.0;   // How far above IB high
        double extensionBelowIB = 0.0;   // How far below IB low
        double extensionRatio = 0.0;     // sessionRange / ibRange

        bool extendedAbove = false;
        bool extendedBelow = false;

        // Failed auction tracking
        bool failedAuctionAbove = false; // Broke above IB, failed within 30 min
        bool failedAuctionBelow = false; // Broke below IB, failed within 30 min
        int barsAboveIB = 0;             // Bars spent above IB high
        int barsBelowIB = 0;             // Bars spent below IB low
    };

    InitialBalanceTracker() : config_() {}
    explicit InitialBalanceTracker(const Config& cfg) : config_(cfg) {}

    /**
     * Update IB tracking.
     *
     * @param high           Current bar high
     * @param low            Current bar low
     * @param close          Current bar close
     * @param minutesFromOpen Minutes elapsed since session open
     * @param barIndex       Current bar index
     * @return               Current IBState
     */
    IBState Update(double high, double low, double close, int minutesFromOpen, int barIndex) {
        // During IB period, update IB range
        if (minutesFromOpen <= config_.ibDurationMinutes) {
            if (!state_.ibComplete) {
                if (state_.ibHigh == 0.0 || high > state_.ibHigh) {
                    state_.ibHigh = high;
                }
                if (state_.ibLow == 0.0 || low < state_.ibLow) {
                    state_.ibLow = low;
                }
                state_.ibRange = state_.ibHigh - state_.ibLow;
            }
        }
        else if (!state_.ibComplete) {
            // IB period just ended
            state_.ibComplete = true;
        }

        // Always update session extremes
        if (state_.sessionHigh == 0.0 || high > state_.sessionHigh) {
            state_.sessionHigh = high;
        }
        if (state_.sessionLow == 0.0 || low < state_.sessionLow) {
            state_.sessionLow = low;
        }
        state_.sessionRange = state_.sessionHigh - state_.sessionLow;

        // Track range extension (only after IB complete)
        if (state_.ibComplete && state_.ibRange > 0.0) {
            // Extension above IB
            if (high > state_.ibHigh) {
                state_.extendedAbove = true;
                state_.extensionAboveIB = high - state_.ibHigh;
                state_.barsAboveIB++;

                // Check for failed auction (returned inside IB within 1 bar)
                if (close < state_.ibHigh && state_.barsAboveIB == 1) {
                    state_.failedAuctionAbove = true;
                }
            }

            // Extension below IB
            if (low < state_.ibLow) {
                state_.extendedBelow = true;
                state_.extensionBelowIB = state_.ibLow - low;
                state_.barsBelowIB++;

                // Check for failed auction (returned inside IB within 1 bar)
                if (close > state_.ibLow && state_.barsBelowIB == 1) {
                    state_.failedAuctionBelow = true;
                }
            }

            // Determine extension type
            if (state_.extendedAbove && state_.extendedBelow) {
                state_.extension = RangeExtensionType::BOTH;
            }
            else if (state_.extendedAbove) {
                state_.extension = RangeExtensionType::BUYING;
            }
            else if (state_.extendedBelow) {
                state_.extension = RangeExtensionType::SELLING;
            }
            else {
                state_.extension = RangeExtensionType::NONE;
            }

            // Extension ratio
            state_.extensionRatio = state_.sessionRange / state_.ibRange;
        }

        return state_;
    }

    /**
     * Reset for new session.
     */
    void Reset() {
        state_ = IBState();
    }

    // Getters
    const IBState& GetState() const { return state_; }
    bool IsIBComplete() const { return state_.ibComplete; }
    double GetIBRange() const { return state_.ibRange; }

private:
    Config config_;
    IBState state_;
};

// ============================================================================
// DAY TYPE CLASSIFIER - MOVED TO AMT_DayType.h
// ============================================================================
// The DayTypeClassifier has been enhanced with ClassifyDaltonDayType() method.
// Use DayTypeClassifier from AMT_DayType.h for both RE-based classification
// (DayStructure) and pattern-based classification (DaltonDayType).
// ============================================================================

// ============================================================================
// SPIKE CONTEXT (Late-Day Imbalance Tracking)
// ============================================================================
// A spike is a breakout in final ~30 minutes that hasn't been validated by time.
// Next-day opening relative to spike determines if move was real or trap.
// ============================================================================

struct SpikeContext {
    bool hasSpike = false;
    double spikeHigh = 0.0;
    double spikeLow = 0.0;
    double spikeOrigin = 0.0;     // Price before spike (target on rejection)
    int spikeStartBar = 0;
    bool isUpSpike = false;       // Direction of spike (true=up, false=down)
    SpikeOpenRelation todayOpen = SpikeOpenRelation::NONE;

    void Reset() {
        hasSpike = false;
        spikeHigh = 0.0;
        spikeLow = 0.0;
        spikeOrigin = 0.0;
        spikeStartBar = 0;
        isUpSpike = false;
        todayOpen = SpikeOpenRelation::NONE;
    }

    /**
     * Detect spike in final 30 minutes of session.
     * Call this when price makes new session extreme late in session.
     */
    void DetectSpike(double high, double low, double priceBeforeMove,
                     double sessionHigh, double sessionLow,
                     int barIndex, bool isNewHigh, bool isNewLow) {
        if (isNewHigh && high >= sessionHigh) {
            hasSpike = true;
            spikeHigh = high;
            spikeLow = low;
            spikeOrigin = priceBeforeMove;
            spikeStartBar = barIndex;
            isUpSpike = true;
        } else if (isNewLow && low <= sessionLow) {
            hasSpike = true;
            spikeHigh = high;
            spikeLow = low;
            spikeOrigin = priceBeforeMove;
            spikeStartBar = barIndex;
            isUpSpike = false;
        }
    }

    /**
     * Evaluate next session's opening relative to spike.
     * Call this at start of new session if prior session had spike.
     */
    void EvaluateOpening(double openPrice) {
        if (!hasSpike) {
            todayOpen = SpikeOpenRelation::NONE;
            return;
        }

        if (isUpSpike) {
            // Up spike: above=accept, within=partial, below=reject
            if (openPrice > spikeHigh) {
                todayOpen = SpikeOpenRelation::ABOVE_SPIKE;
            } else if (openPrice >= spikeLow) {
                todayOpen = SpikeOpenRelation::WITHIN_SPIKE;
            } else {
                todayOpen = SpikeOpenRelation::BELOW_SPIKE;
            }
        } else {
            // Down spike: below=accept, within=partial, above=reject
            if (openPrice < spikeLow) {
                todayOpen = SpikeOpenRelation::BELOW_SPIKE;  // Acceptance of lower prices
            } else if (openPrice <= spikeHigh) {
                todayOpen = SpikeOpenRelation::WITHIN_SPIKE;
            } else {
                todayOpen = SpikeOpenRelation::ABOVE_SPIKE;  // Rejection of lower prices
            }
        }
    }

    /**
     * Get spike target price (for trading back to origin on rejection).
     */
    double GetSpikeTarget() const {
        return spikeOrigin;
    }
};

// ============================================================================
// DALTON STATE
// ============================================================================
// Complete Dalton framework state combining all components.
// ============================================================================

struct DaltonState {
    // ========================================================================
    // PRIMARY MARKET PHASE (derived from timeframe pattern)
    // ========================================================================
    AMTMarketState phase = AMTMarketState::UNKNOWN;  // BALANCE or IMBALANCE

    // ========================================================================
    // TIMEFRAME PATTERN (detection mechanism)
    // ========================================================================
    TimeframePattern timeframe = TimeframePattern::UNKNOWN;
    int rotationFactor = 0;
    int consecutiveUp = 0;
    int consecutiveDown = 0;

    // ========================================================================
    // ACTIVITY TYPE
    // ========================================================================
    AMTActivityType activity = AMTActivityType::NEUTRAL;

    // ========================================================================
    // STRUCTURAL FEATURES
    // ========================================================================
    // Initial Balance
    double ibHigh = 0.0;
    double ibLow = 0.0;
    double ibRange = 0.0;
    bool ibComplete = false;

    // Range Extension
    RangeExtensionType extension = RangeExtensionType::NONE;
    double extensionRatio = 0.0;

    // Day Type (from AMT_DayType.h)
    DaltonDayType dayType = DaltonDayType::UNKNOWN;

    // ========================================================================
    // MARKET EVENTS
    // ========================================================================
    bool failedAuctionAbove = false;
    bool failedAuctionBelow = false;
    ExcessType excess = ExcessType::NONE;

    // ========================================================================
    // VALUE CONTEXT
    // ========================================================================
    ValueLocation location = ValueLocation::INSIDE_VALUE;
    double distFromPOCTicks = 0.0;

    // ========================================================================
    // VOLUME NODE PROXIMITY
    // ========================================================================
    bool atHVN = false;  // At High Volume Node
    bool atLVN = false;  // At Low Volume Node

    // ========================================================================
    // VALIDITY
    // ========================================================================
    bool valid = false;

    // ========================================================================
    // DALTON DECISION SUPPORT (Phase 2-5)
    // ========================================================================
    VolumeConfirmation volumeConf = VolumeConfirmation::UNKNOWN;
    int boundaryTestCount = 0;          // How many times this level tested
    bool isAcceptingNewValue = false;   // Building acceptance outside prior VA
    int barsInNewValue = 0;             // Time spent in new value
    TradingBias bias = TradingBias::WAIT;  // THE ACTIONABLE OUTPUT

    // ========================================================================
    // ACCEPTANCE & VALUE MIGRATION (Advanced Dalton Concepts)
    // ========================================================================
    DaltonAcceptance acceptance = DaltonAcceptance::PROBING;
    ValueMigration valueMigration = ValueMigration::UNKNOWN;
    SpikeContext spikeContext;  // Prior session spike tracking

    // Acceptance tracking
    int barsAtCurrentLevel = 0;     // Time at current price level
    int tpoCountAtLevel = 0;        // TPO count (profile widening)
    double levelAnchorPrice = 0.0;  // Reference price for acceptance tracking

    // ========================================================================
    // LEVEL ACCEPTANCE SIGNALS (from LevelAcceptanceEngine)
    // ========================================================================
    // These are populated by the LevelAcceptanceEngine and used in DeriveTradingBias()
    bool hasLVNAcceptance = false;      // LVN accepted = STRONGEST trend signal
    bool hasHVNRejection = false;       // HVN rejected = momentum signal
    bool hasIBBreak = false;            // IB broken = range extension day
    bool ibBreakIsUp = false;           // Direction of IB break
    int levelDirectionSignal = 0;       // Net signal from level acceptance (-1, 0, +1)
    LevelTestOutcome vahOutcome = LevelTestOutcome::UNTESTED;  // VAH test result
    LevelTestOutcome valOutcome = LevelTestOutcome::UNTESTED;  // VAL test result

    /**
     * Compute acceptance state based on time at level.
     * "One hour of trading at a new level constitutes initial acceptance"
     */
    static DaltonAcceptance ComputeAcceptance(int barsAtLevel, int barIntervalSec, int tpoCount) {
        const int secondsAtLevel = barsAtLevel * barIntervalSec;
        const int ONE_HOUR = 3600;
        const int HALF_HOUR = 1800;

        if (secondsAtLevel < HALF_HOUR) {
            return DaltonAcceptance::PROBING;
        }

        if (secondsAtLevel >= ONE_HOUR && tpoCount >= 3) {
            // Strong TPO stacking = confirmed
            if (tpoCount >= 5) {
                return DaltonAcceptance::CONFIRMED_ACCEPTANCE;
            }
            return DaltonAcceptance::INITIAL_ACCEPTANCE;
        }

        return DaltonAcceptance::PROBING;
    }

    /**
     * Derive AMTMarketState from timeframe pattern.
     * Per Dalton: 1TF = IMBALANCE (trending), 2TF = BALANCE
     */
    void DerivePhaseFromTimeframe() {
        switch (timeframe) {
            case TimeframePattern::ONE_TIME_FRAMING_UP:
            case TimeframePattern::ONE_TIME_FRAMING_DOWN:
                phase = AMTMarketState::IMBALANCE;
                break;
            case TimeframePattern::TWO_TIME_FRAMING:
                phase = AMTMarketState::BALANCE;
                break;
            default:
                phase = AMTMarketState::UNKNOWN;
                break;
        }
    }

    /**
     * Derive CurrentPhase from Dalton state.
     *
     * AMT-COMPLIANT PRIORITY ORDER:
     * 1. Failed Auction (explicit flags) - absolute priority
     * 2. Excess (single-print rejection at extreme) - equals failed auction
     * 3. BALANCE states:
     *    - At boundary (VAH/VAL) = TESTING_BOUNDARY (probing edge)
     *    - Inside value = ROTATION (two-sided trade)
     * 4. IMBALANCE states (1TF directional):
     *    - At boundary + responsive = FAILED_AUCTION (rejection at breakout)
     *    - Range extension + initiative = RANGE_EXTENSION (OTF breakout)
     *    - Responsive = PULLBACK (counter-move within trend)
     *    - Default = DRIVING_UP/DRIVING_DOWN (1TF directional)
     *
     * Key insight: Boundary check moved INSIDE state logic because being at
     * VAH/VAL has DIFFERENT meanings depending on market state:
     * - In BALANCE: Probing the edge (normal rotation behavior)
     * - In IMBALANCE + responsive: Rejection/failed breakout attempt
     */
    CurrentPhase DeriveCurrentPhase() const {
        // =====================================================================
        // PRIORITY 1: Failed Auction (explicit flags) - absolute priority
        // =====================================================================
        if (failedAuctionAbove || failedAuctionBelow) {
            return CurrentPhase::FAILED_AUCTION;
        }

        // =====================================================================
        // PRIORITY 2: Excess = auction rejection at extreme (single-print tail)
        // =====================================================================
        if (excess != ExcessType::NONE) {
            return CurrentPhase::FAILED_AUCTION;
        }

        // =====================================================================
        // PRIORITY 3: BALANCE states (2TF - both sides active)
        // =====================================================================
        if (phase == AMTMarketState::BALANCE) {
            // At boundary = probing the edge (testing for breakout/rejection)
            if (location == ValueLocation::AT_VAH || location == ValueLocation::AT_VAL) {
                return CurrentPhase::TESTING_BOUNDARY;
            }
            // Inside value = rotation (two-sided trade, mean reversion)
            return CurrentPhase::ROTATION;
        }

        // =====================================================================
        // PRIORITY 4: IMBALANCE states (1TF - one side in control)
        // =====================================================================
        if (phase == AMTMarketState::IMBALANCE) {
            // At boundary with responsive activity = rejection (failed breakout)
            // Per Dalton: Price at boundary during imbalance showing responsive
            // activity indicates the breakout attempt is being rejected
            if ((location == ValueLocation::AT_VAH || location == ValueLocation::AT_VAL) &&
                activity == AMTActivityType::RESPONSIVE) {
                return CurrentPhase::FAILED_AUCTION;
            }

            // Range extension with initiative = successful OTF breakout
            // IB has been broken AND there's conviction (initiative activity)
            if (extension != RangeExtensionType::NONE &&
                activity == AMTActivityType::INITIATIVE) {
                return CurrentPhase::RANGE_EXTENSION;
            }

            // Responsive activity within imbalance = pullback (counter-move)
            // Price retracing within the dominant trend
            if (activity == AMTActivityType::RESPONSIVE) {
                return CurrentPhase::PULLBACK;
            }

            // Default imbalance = directional based on 1TF pattern
            // DRIVING_UP/DOWN tells you which side is in control
            if (timeframe == TimeframePattern::ONE_TIME_FRAMING_UP) {
                return CurrentPhase::DRIVING_UP;
            }
            if (timeframe == TimeframePattern::ONE_TIME_FRAMING_DOWN) {
                return CurrentPhase::DRIVING_DOWN;
            }
            // Fallback (shouldn't happen if state is IMBALANCE)
            return CurrentPhase::DRIVING_UP;
        }

        return CurrentPhase::UNKNOWN;
    }

    /**
     * Derive TradingBias from current state.
     *
     * PRIORITY ORDER (Dalton-compliant):
     * 1. SPIKE RULES - Unvalidated overnight moves (most urgent)
     * 2. VALUE MIGRATION - Multi-day context
     * 3. ACCEPTANCE STATE - Time-validated moves
     * 4. INTRADAY STATE - Balance/Imbalance with volume confirmation
     *
     * Key insight: "Fade the extremes, go with breakouts" BUT only after
     * time validates the move. Spikes and unaccepted moves are traps.
     */
    TradingBias DeriveTradingBias() const {
        // =====================================================================
        // PRIORITY 1: SPIKE RULES (unvalidated overnight moves)
        // =====================================================================
        // A spike is a late-day move that hasn't been validated by time.
        // Next-day opening relative to spike determines if real or trap.
        if (spikeContext.hasSpike && spikeContext.todayOpen != SpikeOpenRelation::NONE) {
            if (spikeContext.isUpSpike) {
                // Up spike: above=accept, within=consolidate, below=reject
                switch (spikeContext.todayOpen) {
                    case SpikeOpenRelation::ABOVE_SPIKE:
                        return TradingBias::FOLLOW;  // Gap & Go - bullish acceptance
                    case SpikeOpenRelation::BELOW_SPIKE:
                        return TradingBias::FADE;    // Trap - trade back to origin
                    case SpikeOpenRelation::WITHIN_SPIKE:
                        return TradingBias::WAIT;    // Consolidation expected
                    default: break;
                }
            } else {
                // Down spike: below=accept, within=consolidate, above=reject
                switch (spikeContext.todayOpen) {
                    case SpikeOpenRelation::BELOW_SPIKE:
                        return TradingBias::FOLLOW;  // Gap down acceptance
                    case SpikeOpenRelation::ABOVE_SPIKE:
                        return TradingBias::FADE;    // Trap - trade back to origin
                    case SpikeOpenRelation::WITHIN_SPIKE:
                        return TradingBias::WAIT;    // Consolidation expected
                    default: break;
                }
            }
        }

        // =====================================================================
        // PRIORITY 2: VALUE MIGRATION (multi-day context)
        // =====================================================================
        // Value migration tells us the daily context before intraday signals.
        switch (valueMigration) {
            case ValueMigration::INSIDE:
                // Contraction day - await breakout, don't trade until it happens
                return TradingBias::WAIT;

            case ValueMigration::HIGHER:
            case ValueMigration::LOWER:
                // Trend day - but ONLY follow if move is accepted (not just a probe)
                if (acceptance >= DaltonAcceptance::INITIAL_ACCEPTANCE) {
                    return TradingBias::FOLLOW;  // Time-validated trend
                }
                // Trend developing but not yet accepted - wait for validation
                return TradingBias::WAIT;

            case ValueMigration::OVERLAPPING:
                // Balance day - reversion strategies (fall through to level acceptance)
                break;

            default:
                break;
        }

        // =====================================================================
        // PRIORITY 3: LEVEL ACCEPTANCE (from LevelAcceptanceEngine)
        // =====================================================================
        // Level acceptance provides the most direct, actionable signals.
        // Key insight: Unexpected outcomes at key levels are the signals!

        // 3a. LVN Acceptance = STRONGEST trend signal
        // Price is building value where it "shouldn't be" - major conviction
        if (hasLVNAcceptance) {
            return TradingBias::FOLLOW;  // Strong trend confirmed
        }

        // 3b. IB Break with acceptance = Range Extension Day
        if (hasIBBreak) {
            // IB break is a trend signal (range extension day)
            return TradingBias::FOLLOW;
        }

        // 3c. VAH/VAL resolution determines direction
        if (vahOutcome == LevelTestOutcome::ACCEPTED ||
            vahOutcome == LevelTestOutcome::BROKEN_THROUGH) {
            // VAH accepted = bullish, follow the break
            return TradingBias::FOLLOW;
        }
        if (valOutcome == LevelTestOutcome::ACCEPTED ||
            valOutcome == LevelTestOutcome::BROKEN_THROUGH) {
            // VAL accepted = bearish, follow the break
            return TradingBias::FOLLOW;
        }
        if (vahOutcome == LevelTestOutcome::REJECTED) {
            // VAH rejected = fade, sell the high
            return TradingBias::FADE;
        }
        if (valOutcome == LevelTestOutcome::REJECTED) {
            // VAL rejected = fade, buy the low
            return TradingBias::FADE;
        }

        // 3d. HVN Rejection = unusual momentum (less common)
        if (hasHVNRejection) {
            // Momentum through HVN - follow direction
            return TradingBias::FOLLOW;
        }

        // =====================================================================
        // PRIORITY 4: INTRADAY STATE (Balance/Imbalance + Volume)
        // =====================================================================
        // BALANCE = fade extremes (reversion)
        if (phase == AMTMarketState::BALANCE) {
            if (location == ValueLocation::AT_VAH ||
                location == ValueLocation::AT_VAL) {
                // At boundary - fade if volume weak, wait if strong
                if (volumeConf == VolumeConfirmation::WEAK) {
                    return TradingBias::FADE;
                }
                if (volumeConf == VolumeConfirmation::STRONG) {
                    // Strong volume at boundary could be breakout
                    // But need acceptance to confirm
                    if (acceptance >= DaltonAcceptance::INITIAL_ACCEPTANCE) {
                        return TradingBias::FOLLOW;
                    }
                    return TradingBias::WAIT;  // Wait for time validation
                }
                return TradingBias::FADE;  // Default: fade extremes in balance
            }
            return TradingBias::WAIT;  // Inside VA, wait for extremes
        }

        // IMBALANCE = follow if volume AND acceptance confirm
        if (phase == AMTMarketState::IMBALANCE) {
            if (volumeConf == VolumeConfirmation::STRONG) {
                // Strong volume - but is the move accepted?
                if (acceptance >= DaltonAcceptance::INITIAL_ACCEPTANCE) {
                    return TradingBias::FOLLOW;  // Validated trend
                }
                // Strong volume but still probing - could be liquidation break
                return TradingBias::WAIT;
            }
            if (volumeConf == VolumeConfirmation::WEAK) {
                return TradingBias::FADE;  // Weak breakout = likely rejection
            }
            return TradingBias::WAIT;  // Neutral volume, wait for confirmation
        }

        return TradingBias::WAIT;
    }

    /**
     * Derive VolumeConfirmation from volume percentile.
     * Called externally with baseline percentile data.
     *
     * @param volumePercentile Volume percentile [0-100] from baseline
     * @return VolumeConfirmation category
     */
    static VolumeConfirmation DeriveVolumeConfirmation(double volumePercentile) {
        if (volumePercentile < 0.0) return VolumeConfirmation::UNKNOWN;
        if (volumePercentile < 25.0) return VolumeConfirmation::WEAK;
        if (volumePercentile < 75.0) return VolumeConfirmation::NEUTRAL;
        return VolumeConfirmation::STRONG;
    }

    /**
     * Derive PhaseReason from current state.
     *
     * Returns the most specific AMT concept explaining the current situation.
     * Priority order: most actionable/specific first.
     */
    PhaseReason DerivePhaseReason() const {
        // Priority 1: Excess at extremes (most actionable)
        if (excess == ExcessType::EXCESS_HIGH) return PhaseReason::EXCESS_HIGH;
        if (excess == ExcessType::EXCESS_LOW) return PhaseReason::EXCESS_LOW;
        if (excess == ExcessType::POOR_HIGH) return PhaseReason::POOR_HIGH;
        if (excess == ExcessType::POOR_LOW) return PhaseReason::POOR_LOW;

        // Priority 2: IB breaks
        if (extension == RangeExtensionType::BUYING) return PhaseReason::IB_BREAK_UP;
        if (extension == RangeExtensionType::SELLING) return PhaseReason::IB_BREAK_DOWN;

        // Priority 3: Volume nodes (LVN more actionable - price tends to move through)
        if (atLVN) return PhaseReason::AT_LVN;
        if (atHVN) return PhaseReason::AT_HVN;

        // Priority 4: Value area location
        if (location == ValueLocation::AT_POC) return PhaseReason::AT_POC;
        if (location == ValueLocation::AT_VAH) return PhaseReason::AT_VAH;
        if (location == ValueLocation::AT_VAL) return PhaseReason::AT_VAL;

        // Priority 5: Activity type
        if (activity == AMTActivityType::RESPONSIVE) return PhaseReason::RESPONSIVE;
        if (activity == AMTActivityType::INITIATIVE) return PhaseReason::INITIATIVE;

        // Priority 6: Timeframe pattern (explains state)
        if (timeframe == TimeframePattern::ONE_TIME_FRAMING_UP) return PhaseReason::ONE_TF_UP;
        if (timeframe == TimeframePattern::ONE_TIME_FRAMING_DOWN) return PhaseReason::ONE_TF_DOWN;
        if (timeframe == TimeframePattern::TWO_TIME_FRAMING) return PhaseReason::TWO_TF;

        // Priority 7: Inside/outside value
        if (location == ValueLocation::INSIDE_VALUE) return PhaseReason::INSIDE_VALUE;
        if (location == ValueLocation::ABOVE_VALUE ||
            location == ValueLocation::BELOW_VALUE) return PhaseReason::OUTSIDE_VALUE;

        return PhaseReason::NONE;
    }
};

// ============================================================================
// DALTON ENGINE
// ============================================================================
// Main engine that coordinates all Dalton framework components.
// ============================================================================

class DaltonEngine {
public:
    struct Config {
        RotationTracker::Config rotation;
        InitialBalanceTracker::Config ib;
        ActivityClassifier::Config activity;

        Config() = default;
    };

    DaltonEngine()
        : rotationTracker_()
        , ibTracker_()
        , activityClassifier_()
    {}

    explicit DaltonEngine(const Config& cfg)
        : rotationTracker_(cfg.rotation)
        , ibTracker_(cfg.ib)
        , activityClassifier_(cfg.activity)
    {}

    /**
     * Process a bar and update all Dalton framework components.
     *
     * @param high            Bar high
     * @param low             Bar low
     * @param close           Bar close
     * @param prevClose       Previous bar close
     * @param poc             Point of Control
     * @param vah             Value Area High
     * @param val             Value Area Low
     * @param deltaPct        Bar delta as fraction of volume
     * @param tickSize        Tick size
     * @param minutesFromOpen Minutes since session open
     * @param barIndex        Current bar index
     * @return                Complete DaltonState
     */
    DaltonState ProcessBar(
        double high,
        double low,
        double close,
        double prevClose,
        double poc,
        double vah,
        double val,
        double deltaPct,
        double tickSize,
        int minutesFromOpen,
        int barIndex
    ) {
        DaltonState state;
        state.valid = true;

        // 1. Update rotation tracking (1TF/2TF detection)
        auto rotation = rotationTracker_.Update(high, low, barIndex);
        state.timeframe = rotation.pattern;
        state.rotationFactor = rotation.rotationFactor;
        state.consecutiveUp = rotation.consecutiveUp;
        state.consecutiveDown = rotation.consecutiveDown;

        // 2. Derive phase from timeframe pattern
        state.DerivePhaseFromTimeframe();

        // 3. Update Initial Balance tracking
        auto ib = ibTracker_.Update(high, low, close, minutesFromOpen, barIndex);
        state.ibHigh = ib.ibHigh;
        state.ibLow = ib.ibLow;
        state.ibRange = ib.ibRange;
        state.ibComplete = ib.ibComplete;
        state.extension = ib.extension;
        state.extensionRatio = ib.extensionRatio;
        state.failedAuctionAbove = ib.failedAuctionAbove;
        state.failedAuctionBelow = ib.failedAuctionBelow;

        // 4. Classify activity
        auto activity = activityClassifier_.Classify(
            close, prevClose, poc, vah, val, deltaPct, tickSize);
        state.activity = activity.activityType;
        state.location = activity.location;
        state.distFromPOCTicks = activity.priceVsPOC;

        // 5. Classify Dalton day type (inline - no separate classifier needed)
        state.dayType = ClassifyDaltonDayType(ib, rotation, close);

        return state;
    }

private:
    /**
     * Classify Dalton day type from IB state and rotation data.
     * Uses thresholds from DaltonThresholds namespace in AMT_DayType.h
     */
    static DaltonDayType ClassifyDaltonDayType(
        const InitialBalanceTracker::IBState& ibState,
        const RotationTracker::RotationResult& rotation,
        double close
    ) {
        // Need IB complete to classify
        if (!ibState.ibComplete || ibState.ibRange <= 0.0 || ibState.sessionRange <= 0.0) {
            return DaltonDayType::UNKNOWN;
        }

        const double extensionRatio = ibState.extensionRatio;
        const double ibRatio = ibState.ibRange / ibState.sessionRange;

        // Check close position relative to range
        const double rangePos = (close - ibState.sessionLow) / ibState.sessionRange;
        const bool atHighExtreme = rangePos >= (1.0 - DaltonThresholds::CLOSE_AT_EXTREME_RATIO);
        const bool atLowExtreme = rangePos <= DaltonThresholds::CLOSE_AT_EXTREME_RATIO;
        const bool atExtreme = atHighExtreme || atLowExtreme;

        // Close in value area (middle of range)
        const bool closeInValue = (close >= ibState.ibLow && close <= ibState.ibHigh);

        // Extension both sides
        const bool extendedBoth = (ibState.extension == RangeExtensionType::BOTH);

        // =====================================================================
        // CLASSIFICATION LOGIC (Dalton framework)
        // =====================================================================

        // TREND DAY: Narrow IB, 1TF, large extension, closes at extreme
        if (ibRatio < DaltonThresholds::TREND_DAY_IB_RATIO &&
            rotation.is1TF &&
            extensionRatio >= DaltonThresholds::TREND_DAY_EXTENSION &&
            atExtreme) {
            return DaltonDayType::TREND_DAY;
        }

        // NEUTRAL DAY: Extension both sides, closes in value
        if (extendedBoth && closeInValue) {
            return DaltonDayType::NEUTRAL_DAY;
        }

        // NON-TREND DAY: Very narrow range, no conviction
        if (extensionRatio <= 1.1 && ibRatio > 0.8) {
            return DaltonDayType::NON_TREND_DAY;
        }

        // NORMAL VARIATION: Extension < 2x IB
        if (extensionRatio < DaltonThresholds::NORMAL_VAR_EXTENSION) {
            return DaltonDayType::NORMAL_VARIATION;
        }

        // NORMAL DAY: Wide IB, stays within (no significant extension)
        if (ibState.extension == RangeExtensionType::NONE) {
            return DaltonDayType::NORMAL_DAY;
        }

        // Default to Normal Variation for remaining cases
        return DaltonDayType::NORMAL_VARIATION;
    }

public:

    /**
     * Reset for new session.
     */
    void ResetSession() {
        rotationTracker_.Reset();
        ibTracker_.Reset();
    }

    // Component access
    const RotationTracker& GetRotationTracker() const { return rotationTracker_; }
    const InitialBalanceTracker& GetIBTracker() const { return ibTracker_; }

    /**
     * Check price proximity to HVN/LVN and update state flags.
     *
     * @param state         DaltonState to update
     * @param price         Current price
     * @param tickSize      Tick size
     * @param toleranceTicks Proximity tolerance in ticks
     * @param hvnPrices     Vector of High Volume Node prices
     * @param lvnPrices     Vector of Low Volume Node prices
     */
    static void CheckVolumeNodeProximity(
        DaltonState& state,
        double price,
        double tickSize,
        int toleranceTicks,
        const std::vector<double>& hvnPrices,
        const std::vector<double>& lvnPrices
    ) {
        state.atHVN = false;
        state.atLVN = false;
        const double tolerance = toleranceTicks * tickSize;

        for (double hvn : hvnPrices) {
            if (std::abs(price - hvn) <= tolerance) {
                state.atHVN = true;
                break;
            }
        }
        for (double lvn : lvnPrices) {
            if (std::abs(price - lvn) <= tolerance) {
                state.atLVN = true;
                break;
            }
        }
    }

private:
    RotationTracker rotationTracker_;
    InitialBalanceTracker ibTracker_;
    ActivityClassifier activityClassifier_;
};

} // namespace AMT

#endif // AMT_DALTON_H
