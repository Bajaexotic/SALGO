// ============================================================================
// AMT_Signals.h
// Auction Market Theory Signal Processing
// ============================================================================
// Implements:
//   - ActivityClassifier: Value-relative Intent × Participation classification
//   - AMTStateTracker: Leaky accumulator for BALANCE/IMBALANCE state
//   - SinglePrintDetector: Profile-structural single print detection
//   - ExcessDetector: Excess/poor high-low confirmation logic
// ============================================================================

#ifndef AMT_SIGNALS_H
#define AMT_SIGNALS_H

#include "amt_core.h"
#include "AMT_ValueLocation.h"  // For ValueLocationResult (SSOT)
#include <vector>
#include <algorithm>
#include <cmath>

namespace AMT {

// ============================================================================
// ACTIVITY CLASSIFIER
// ============================================================================
// Computes value-relative activity classification from bar data.
// This is the core AMT classification: Intent × Participation → ActivityType
//
// Intent: TOWARD_VALUE, AWAY_FROM_VALUE, AT_VALUE (relative to POC)
// Participation: AGGRESSIVE, ABSORPTIVE, BALANCED (from delta vs price)
// ActivityType: INITIATIVE (away + aggressive), RESPONSIVE (toward or absorptive)
// ============================================================================

class ActivityClassifier {
public:
    // Configuration
    struct Config {
        int pocToleranceTicks = 2;      // Within 2 ticks = AT_POC
        int vaBoundaryTicks = 2;        // Within 2 ticks of VAH/VAL = AT boundary
        double neutralDeltaThreshold = 0.10;  // |delta%| below this = BALANCED
        double neutralPriceThreshold = 0.5;   // |priceChange| in ticks below this = neutral

        Config() = default;
    };

    ActivityClassifier() : config_() {}
    explicit ActivityClassifier(const Config& cfg) : config_(cfg) {}

    /**
     * DEPRECATED: Classify activity for a bar given price context and delta.
     * Use ClassifyFromValueLocation() instead which consumes ValueLocationResult (SSOT).
     *
     * @param price            Current price (typically close)
     * @param prevPrice        Previous price (for direction)
     * @param poc              Point of Control price (value center)
     * @param vah              Value Area High
     * @param val              Value Area Low
     * @param deltaPct         Bar delta as fraction of volume (-1 to +1)
     * @param tickSize         Tick size for conversions
     * @param volumeConviction Volume conviction (0-2, 1.0 = normal, from percentile/50)
     * @return                 Complete ActivityClassification
     */
    [[deprecated("Use ClassifyFromValueLocation() with ValueLocationResult from ValueLocationEngine (SSOT)")]]
    ActivityClassification Classify(
        double price,
        double prevPrice,
        double poc,
        double vah,
        double val,
        double deltaPct,
        double tickSize,
        double volumeConviction = 1.0  // Default to normal conviction
    ) const {
        ActivityClassification result;

        // Validate inputs
        if (tickSize <= 0.0 || poc <= 0.0 || vah <= val) {
            result.valid = false;
            return result;
        }

        result.valid = true;

        // 1. Compute price distance from POC (in ticks, signed)
        result.priceVsPOC = (price - poc) / tickSize;

        // 2. Compute price change (direction and magnitude)
        result.priceChange = (price - prevPrice) / tickSize;

        // 3. Store delta
        result.deltaPct = deltaPct;

        // 4. Store volume conviction (clamped to [0, 2])
        result.volumeConviction = (std::max)(0.0, (std::min)(2.0, volumeConviction));

        // 5. Determine Zone (9-state ValueZone)
        result.zone = DetermineZone(price, poc, vah, val, tickSize);

        // 6. Determine Intent (value-relative direction) - internal
        result.intent_ = DetermineIntent(price, prevPrice, poc, tickSize);

        // 7. Determine Participation (delta vs price alignment) - internal
        result.participation_ = DetermineParticipation(result.priceChange, deltaPct);

        // 8. Derive activity type from Intent × Participation
        result.DeriveActivityType();

        return result;
    }

    /**
     * PREFERRED: Classify activity using ValueLocationResult (SSOT-compliant)
     *
     * ValueLocationEngine is the SSOT for value location. This overload consumes
     * its output rather than duplicating the location classification logic.
     *
     * @param valLocResult   ValueLocationResult from ValueLocationEngine (SSOT)
     * @param price          Current price (typically close)
     * @param prevPrice      Previous price (for direction)
     * @param deltaPct       Bar delta as fraction of volume (-1 to +1)
     * @param volumeConviction Volume conviction (0-2, 1.0 = normal)
     * @return               Complete ActivityClassification
     */
    ActivityClassification ClassifyFromValueLocation(
        const ValueLocationResult& valLocResult,
        double price,
        double prevPrice,
        double deltaPct,
        double volumeConviction = 1.0
    ) const {
        ActivityClassification result;

        // Validate SSOT input
        if (!valLocResult.IsReady()) {
            result.valid = false;
            return result;
        }

        result.valid = true;

        // 1. Use SSOT distances directly
        result.priceVsPOC = valLocResult.distFromPOCTicks;

        // 2. Compute price change (from raw inputs, not in SSOT)
        const double tickSize = valLocResult.vaWidthTicks > 0
            ? (valLocResult.distFromVAHTicks - valLocResult.distFromVALTicks) / valLocResult.vaWidthTicks
            : 0.25;  // Fallback to ES tick size
        result.priceChange = (price - prevPrice) / tickSize;

        // 3. Store delta
        result.deltaPct = deltaPct;

        // 4. Store volume conviction (clamped to [0, 2])
        result.volumeConviction = (std::max)(0.0, (std::min)(2.0, volumeConviction));

        // 5. Store zone from SSOT (ValueLocationEngine)
        result.zone = valLocResult.zone;

        // 6. Determine Intent using SSOT POC distance
        const double poc = price - (valLocResult.distFromPOCTicks * tickSize);
        result.intent_ = DetermineIntent(price, prevPrice, poc, tickSize);

        // 7. Determine Participation
        result.participation_ = DetermineParticipation(result.priceChange, deltaPct);

        // 8. Derive activity type
        result.DeriveActivityType();

        return result;
    }

private:
    Config config_;

    ValueZone DetermineZone(
        double price,
        double poc,
        double vah,
        double val,
        double tickSize
    ) const {
        const double distFromPOC = std::abs(price - poc) / tickSize;
        const double distFromVAH = (price - vah) / tickSize;
        const double distFromVAL = (price - val) / tickSize;

        // Check POC first (highest priority for AT_POC)
        if (distFromPOC <= config_.pocToleranceTicks) {
            return ValueZone::AT_POC;
        }

        // Check boundaries
        if (std::abs(distFromVAH) <= config_.vaBoundaryTicks) {
            return ValueZone::AT_VAH;
        }
        if (std::abs(distFromVAL) <= config_.vaBoundaryTicks) {
            return ValueZone::AT_VAL;
        }

        // Check outside value (use NEAR_ variants for deprecated method)
        if (price > vah) {
            return ValueZone::NEAR_ABOVE_VALUE;
        }
        if (price < val) {
            return ValueZone::NEAR_BELOW_VALUE;
        }

        // Inside value area (use upper half by default for deprecated method)
        if (price >= poc) {
            return ValueZone::UPPER_VALUE;
        }
        return ValueZone::LOWER_VALUE;
    }

    ValueIntent DetermineIntent(
        double price,
        double prevPrice,
        double poc,
        double tickSize
    ) const {
        const double currentDistFromPOC = std::abs(price - poc);
        const double prevDistFromPOC = std::abs(prevPrice - poc);

        // At POC (within tolerance)?
        if (currentDistFromPOC / tickSize <= config_.pocToleranceTicks) {
            return ValueIntent::AT_VALUE;
        }

        // No significant price change?
        const double priceChange = std::abs(price - prevPrice) / tickSize;
        if (priceChange < config_.neutralPriceThreshold) {
            // Stationary - but WHERE are we stationary?
            // If consolidating far from POC, we're still AWAY from value
            // Only return AT_VALUE if actually near POC
            const double distFromPOCTicks = currentDistFromPOC / tickSize;
            if (distFromPOCTicks > config_.pocToleranceTicks * 2) {
                // Consolidating outside value = still away from value
                return ValueIntent::AWAY_FROM_VALUE;
            }
            return ValueIntent::AT_VALUE;
        }

        // Determine if moving toward or away from POC
        if (currentDistFromPOC < prevDistFromPOC) {
            return ValueIntent::TOWARD_VALUE;
        }
        else if (currentDistFromPOC > prevDistFromPOC) {
            return ValueIntent::AWAY_FROM_VALUE;
        }

        return ValueIntent::AT_VALUE;
    }

    ParticipationMode DetermineParticipation(
        double priceChangeTicks,
        double deltaPct
    ) const {
        // Neutral delta?
        if (std::abs(deltaPct) < config_.neutralDeltaThreshold) {
            return ParticipationMode::BALANCED;
        }

        // Neutral price?
        if (std::abs(priceChangeTicks) < config_.neutralPriceThreshold) {
            // No significant price move - participation unclear
            return ParticipationMode::BALANCED;
        }

        // Check alignment: delta sign matches price direction?
        const bool priceUp = priceChangeTicks > 0;
        const bool deltaPositive = deltaPct > 0;

        if ((priceUp && deltaPositive) || (!priceUp && !deltaPositive)) {
            // Delta aligned with price direction = AGGRESSIVE (initiators)
            return ParticipationMode::AGGRESSIVE;
        }
        else {
            // Delta opposite to price direction = ABSORPTIVE (responsive)
            return ParticipationMode::ABSORPTIVE;
        }
    }
};

// ============================================================================
// AMT STATE TRACKER
// ============================================================================
// Tracks BALANCE/IMBALANCE state from DaltonEngine (SSOT).
//
// DaltonEngine determines state via 1TF/2TF pattern detection:
//   - 1TF (One-Time Framing) = IMBALANCE (one side in control)
//   - 2TF (Two-Time Framing) = BALANCE (both sides active)
//
// This tracker:
//   - Receives state from DaltonEngine (does not compute it)
//   - Tracks consecutive bars in state for transition detection
//   - Populates StateEvidence for downstream consumers
// ============================================================================

class AMTStateTracker {
public:
    AMTStateTracker() : currentState_(AMTMarketState::UNKNOWN),
        barsInState_(0), previousState_(AMTMarketState::UNKNOWN) {}
    explicit AMTStateTracker(int /*unused*/)
        : currentState_(AMTMarketState::UNKNOWN)
        , barsInState_(0)
        , previousState_(AMTMarketState::UNKNOWN)
    {}

    /**
     * Update state based on Dalton's 1TF/2TF pattern (SSOT) and activity classification.
     *
     * Per Dalton: 1TF/2TF is the DETECTION MECHANISM for Balance/Imbalance.
     * Activity classification determines WHO is in control (INITIATIVE/RESPONSIVE),
     * not WHAT the state is.
     *
     * @param activity    This bar's activity classification (determines WHO)
     * @param currentBar  Current bar index (for transition logging)
     * @param daltonState The authoritative state from DaltonEngine (1TF/2TF derived)
     * @param daltonPhase The authoritative phase from DaltonState.DeriveCurrentPhase()
     *                    If UNKNOWN, StateEvidence.DerivePhase() will compute locally
     * @return            Updated StateEvidence with current state and phase
     */
    StateEvidence Update(const ActivityClassification& activity, int currentBar,
                         AMTMarketState daltonState = AMTMarketState::UNKNOWN,
                         CurrentPhase daltonPhase = CurrentPhase::UNKNOWN) {
        StateEvidence evidence;

        if (!activity.valid) {
            evidence.currentState = AMTMarketState::UNKNOWN;
            return evidence;
        }

        // Store previous state for transition detection
        previousState_ = currentState_;

        // Use Dalton's 1TF/2TF state (SSOT)
        const AMTMarketState newState = daltonState;

        // Update state tracking
        if (newState != currentState_) {
            evidence.previousState = currentState_;
            evidence.barAtTransition = currentBar;
            currentState_ = newState;
            barsInState_ = 1;
        }
        else {
            evidence.previousState = previousState_;
            barsInState_++;
        }

        // Populate evidence
        evidence.currentState = currentState_;
        evidence.barsInState = barsInState_;
        evidence.activity = activity;
        evidence.location = activity.zone;

        // Store derived phase (SSOT: Dalton, fallback: local derivation)
        evidence.derivedPhase = daltonPhase;

        return evidence;
    }

    // Getters
    AMTMarketState GetCurrentState() const { return currentState_; }
    int GetBarsInState() const { return barsInState_; }

    // Reset for new session
    void Reset() {
        currentState_ = AMTMarketState::UNKNOWN;
        barsInState_ = 0;
        previousState_ = AMTMarketState::UNKNOWN;
    }

private:
    AMTMarketState currentState_;
    int barsInState_;
    AMTMarketState previousState_;
};

// ============================================================================
// SINGLE PRINT DETECTOR
// ============================================================================
// Detects single print zones from volume profile structure.
// Single prints are contiguous areas of thin volume (LVN) that indicate
// one-sided aggressive activity with no two-sided trade.
//
// Detection is profile-structural (not per-bar):
//   - Scan profile for contiguous LVN areas
//   - Filter by minimum width (MIN_SINGLE_PRINT_TICKS)
//   - Track fill-in progress across the session
// ============================================================================

class SinglePrintDetector {
public:
    struct Config {
        int minWidthTicks = 3;          // Minimum contiguous ticks for single print
        double volumeThreshold = 0.15;  // % of session avg for "thin"
        double fillCompletePct = 0.80;  // 80% filled = zone invalid

        Config() = default;
    };

    SinglePrintDetector() : config_() {}
    explicit SinglePrintDetector(const Config& cfg) : config_(cfg) {}

    /**
     * Detect single print zones from volume profile data.
     *
     * @param volumeData    Array of volume at each price level
     * @param priceStart    Price at index 0
     * @param tickSize      Tick size
     * @param numLevels     Number of price levels
     * @param avgVolume     Average volume per level (for threshold)
     * @param currentBar    Current bar index
     * @return              Vector of detected SinglePrintZone
     */
    std::vector<SinglePrintZone> DetectFromProfile(
        const double* volumeData,
        double priceStart,
        double tickSize,
        int numLevels,
        double avgVolume,
        int currentBar
    ) {
        std::vector<SinglePrintZone> zones;

        if (numLevels < config_.minWidthTicks || avgVolume <= 0.0 || tickSize <= 0.0) {
            return zones;
        }

        const double threshold = avgVolume * config_.volumeThreshold;

        // Scan for contiguous thin-volume regions
        int runStart = -1;
        int runLength = 0;

        for (int i = 0; i < numLevels; ++i) {
            const bool isThin = (volumeData[i] < threshold);

            if (isThin) {
                if (runStart < 0) {
                    runStart = i;
                    runLength = 1;
                }
                else {
                    runLength++;
                }
            }
            else {
                // End of thin region - check if long enough
                if (runStart >= 0 && runLength >= config_.minWidthTicks) {
                    SinglePrintZone zone;
                    zone.lowPrice = priceStart + runStart * tickSize;
                    zone.highPrice = priceStart + (runStart + runLength - 1) * tickSize;
                    zone.widthTicks = runLength;
                    zone.creationBar = currentBar;
                    zone.valid = true;
                    zones.push_back(zone);
                }
                runStart = -1;
                runLength = 0;
            }
        }

        // Check final run
        if (runStart >= 0 && runLength >= config_.minWidthTicks) {
            SinglePrintZone zone;
            zone.lowPrice = priceStart + runStart * tickSize;
            zone.highPrice = priceStart + (runStart + runLength - 1) * tickSize;
            zone.widthTicks = runLength;
            zone.creationBar = currentBar;
            zone.valid = true;
            zones.push_back(zone);
        }

        return zones;
    }

    /**
     * Update fill progress for existing single print zones.
     *
     * @param zones         Vector of existing zones to update
     * @param volumeData    Current volume at each price level
     * @param priceStart    Price at index 0
     * @param tickSize      Tick size
     * @param numLevels     Number of price levels
     * @param avgVolume     Average volume per level
     */
    void UpdateFillProgress(
        std::vector<SinglePrintZone>& zones,
        const double* volumeData,
        double priceStart,
        double tickSize,
        int numLevels,
        double avgVolume
    ) const {
        const double threshold = avgVolume * config_.volumeThreshold;

        for (auto& zone : zones) {
            if (!zone.valid) continue;

            // Count how many ticks in the zone now have significant volume
            int filledTicks = 0;
            const int startIdx = static_cast<int>((zone.lowPrice - priceStart) / tickSize + 0.5);
            const int endIdx = static_cast<int>((zone.highPrice - priceStart) / tickSize + 0.5);

            for (int i = startIdx; i <= endIdx && i < numLevels && i >= 0; ++i) {
                if (volumeData[i] >= threshold) {
                    filledTicks++;
                }
            }

            zone.fillProgress = static_cast<double>(filledTicks) / zone.widthTicks;

            if (!zone.fillStarted && zone.fillProgress > 0.0) {
                zone.fillStarted = true;
            }

            if (zone.fillProgress >= config_.fillCompletePct) {
                zone.valid = false;  // Zone fully filled, no longer significant
            }
        }
    }

    /**
     * Get active (valid) single print zones.
     */
    static std::vector<SinglePrintZone> GetActiveZones(
        const std::vector<SinglePrintZone>& zones
    ) {
        std::vector<SinglePrintZone> active;
        for (const auto& zone : zones) {
            if (zone.valid) {
                active.push_back(zone);
            }
        }
        return active;
    }

private:
    Config config_;
};

// ============================================================================
// EXCESS DETECTOR
// ============================================================================
// Detects excess (auction failure) at session extremes.
//
// Excess requires confirmation:
//   - Tail evidence: Single-print tail at extreme (auction probed and rejected)
//   - Failure evidence: Multi-bar failure to accept the extreme level
//
// Poor high/low: Incomplete auction (no tail, abrupt rejection)
// True excess: Tail + sustained rejection
// ============================================================================

class ExcessDetector {
public:
    struct Config {
        double minTailTicks = 2.0;     // Minimum tail size for excess signal
        int confirmationBars = 3;      // Bars to confirm excess (multi-bar failure)
        int toleranceTicks = 2;        // Tolerance for "at extreme"

        Config() = default;
    };

    struct ExtremeState {
        double price = 0.0;
        int touchBar = 0;
        double tailTicks = 0.0;
        int barsAway = 0;           // Bars spent away from extreme
        double maxDistanceAway = 0.0;  // Max distance traveled away
        bool tailDetected = false;
        bool rejected = false;
        bool confirmedExcess = false;
        AMTActivityType activityAtExtreme = AMTActivityType::NEUTRAL;
    };

    ExcessDetector() : config_() {}
    explicit ExcessDetector(const Config& cfg) : config_(cfg) {}

    /**
     * Update excess detection for session high.
     *
     * @param sessionHigh   Current session high price
     * @param currentPrice  Current price
     * @param tickSize      Tick size
     * @param currentBar    Current bar index
     * @param activity      This bar's activity classification
     * @param tailAtHigh    Tail size at high (from profile, if available)
     * @return              ExcessType detected (or NONE)
     */
    ExcessType UpdateHigh(
        double sessionHigh,
        double currentPrice,
        double tickSize,
        int currentBar,
        const ActivityClassification& activity,
        double tailAtHigh = 0.0
    ) {
        const double distFromHigh = (sessionHigh - currentPrice) / tickSize;
        const bool atHigh = distFromHigh <= config_.toleranceTicks;

        // New high touched?
        if (atHigh) {
            highState_.price = sessionHigh;
            highState_.touchBar = currentBar;
            highState_.tailTicks = tailAtHigh;
            highState_.tailDetected = tailAtHigh >= config_.minTailTicks;
            highState_.activityAtExtreme = activity.activityType;
            highState_.barsAway = 0;
            highState_.maxDistanceAway = 0.0;
            highState_.rejected = false;
            highState_.confirmedExcess = false;
        }
        else if (highState_.price > 0.0) {
            // Moving away from high
            highState_.barsAway++;
            highState_.maxDistanceAway = (std::max)(highState_.maxDistanceAway, distFromHigh);

            // Check for rejection/confirmation
            if (highState_.barsAway >= config_.confirmationBars) {
                highState_.rejected = true;

                if (highState_.tailDetected &&
                    highState_.activityAtExtreme == AMTActivityType::RESPONSIVE) {
                    highState_.confirmedExcess = true;
                }
            }
        }

        return ClassifyHigh();
    }

    /**
     * Update excess detection for session low.
     */
    ExcessType UpdateLow(
        double sessionLow,
        double currentPrice,
        double tickSize,
        int currentBar,
        const ActivityClassification& activity,
        double tailAtLow = 0.0
    ) {
        const double distFromLow = (currentPrice - sessionLow) / tickSize;
        const bool atLow = distFromLow <= config_.toleranceTicks;

        if (atLow) {
            lowState_.price = sessionLow;
            lowState_.touchBar = currentBar;
            lowState_.tailTicks = tailAtLow;
            lowState_.tailDetected = tailAtLow >= config_.minTailTicks;
            lowState_.activityAtExtreme = activity.activityType;
            lowState_.barsAway = 0;
            lowState_.maxDistanceAway = 0.0;
            lowState_.rejected = false;
            lowState_.confirmedExcess = false;
        }
        else if (lowState_.price > 0.0) {
            lowState_.barsAway++;
            lowState_.maxDistanceAway = (std::max)(lowState_.maxDistanceAway, distFromLow);

            if (lowState_.barsAway >= config_.confirmationBars) {
                lowState_.rejected = true;

                if (lowState_.tailDetected &&
                    lowState_.activityAtExtreme == AMTActivityType::RESPONSIVE) {
                    lowState_.confirmedExcess = true;
                }
            }
        }

        return ClassifyLow();
    }

    /**
     * Get combined excess type (prioritizes confirmed excess over poor).
     */
    ExcessType GetCombinedExcess() const {
        ExcessType highType = ClassifyHigh();
        ExcessType lowType = ClassifyLow();

        // Prioritize confirmed excess
        if (highType == ExcessType::EXCESS_HIGH) return highType;
        if (lowType == ExcessType::EXCESS_LOW) return lowType;

        // Then poor high/low
        if (highType == ExcessType::POOR_HIGH) return highType;
        if (lowType == ExcessType::POOR_LOW) return lowType;

        return ExcessType::NONE;
    }

    // Getters
    const ExtremeState& GetHighState() const { return highState_; }
    const ExtremeState& GetLowState() const { return lowState_; }

    // Reset for new session
    void Reset() {
        highState_ = ExtremeState();
        lowState_ = ExtremeState();
    }

private:
    Config config_;
    ExtremeState highState_;
    ExtremeState lowState_;

    ExcessType ClassifyHigh() const {
        if (!highState_.rejected) {
            return ExcessType::NONE;
        }

        if (highState_.confirmedExcess) {
            return ExcessType::EXCESS_HIGH;
        }

        // Rejected but no tail/responsive = poor high
        return ExcessType::POOR_HIGH;
    }

    ExcessType ClassifyLow() const {
        if (!lowState_.rejected) {
            return ExcessType::NONE;
        }

        if (lowState_.confirmedExcess) {
            return ExcessType::EXCESS_LOW;
        }

        return ExcessType::POOR_LOW;
    }
};

// ============================================================================
// AMT SIGNAL ENGINE
// ============================================================================
// Coordinates all signal components into a unified interface.
// This is the main entry point for AMT signal processing.
// ============================================================================

class AMTSignalEngine {
public:
    struct Config {
        ActivityClassifier::Config activityConfig;
        SinglePrintDetector::Config singlePrintConfig;
        ExcessDetector::Config excessConfig;

        Config() = default;
    };

    AMTSignalEngine()
        : activityClassifier_()
        , stateTracker_()
        , singlePrintDetector_()
        , excessDetector_()
    {}

    explicit AMTSignalEngine(const Config& cfg)
        : activityClassifier_(cfg.activityConfig)
        , stateTracker_()
        , singlePrintDetector_(cfg.singlePrintConfig)
        , excessDetector_(cfg.excessConfig)
    {}

    /**
     * Process a bar and update all AMT signals.
     *
     * DEPRECATED: Use ProcessBarFromValueLocation() which consumes ValueLocationResult
     * from ValueLocationEngine (SSOT) instead of computing location internally.
     *
     * SSOT: The daltonState parameter (derived from 1TF/2TF) is the authoritative
     * source for Balance/Imbalance. Activity classification determines WHO is in
     * control (INITIATIVE/RESPONSIVE), not WHAT the state is.
     *
     * @param price            Current price
     * @param prevPrice        Previous bar's price
     * @param poc              Point of Control
     * @param vah              Value Area High
     * @param val              Value Area Low
     * @param deltaPct         Bar delta as fraction of volume
     * @param tickSize         Tick size
     * @param sessionHigh      Current session high
     * @param sessionLow       Current session low
     * @param currentBar       Current bar index
     * @param tailAtHigh       Single print tail at high (if available)
     * @param tailAtLow        Single print tail at low (if available)
     * @param volumeConviction Volume conviction (0-2, 1.0 = normal), weights strength
     * @param daltonState      SSOT: Balance/Imbalance state from DaltonEngine (1TF/2TF)
     *                         If UNKNOWN, falls back to accumulator (legacy/warmup)
     * @param daltonPhase      SSOT: CurrentPhase from DaltonState.DeriveCurrentPhase()
     *                         If UNKNOWN, StateEvidence.DerivePhase() computes locally
     * @return                 Complete StateEvidence
     */
    [[deprecated("Use ProcessBarFromValueLocation() with ValueLocationResult from ValueLocationEngine (SSOT)")]]
    StateEvidence ProcessBar(
        double price,
        double prevPrice,
        double poc,
        double vah,
        double val,
        double deltaPct,
        double tickSize,
        double sessionHigh,
        double sessionLow,
        int currentBar,
        double tailAtHigh = 0.0,
        double tailAtLow = 0.0,
        double volumeConviction = 1.0,
        AMTMarketState daltonState = AMTMarketState::UNKNOWN,
        CurrentPhase daltonPhase = CurrentPhase::UNKNOWN
    ) {
        // 1. Classify activity (with volume conviction for strength weighting)
        // This determines WHO is in control, not WHAT the state is
        // Suppress deprecation warning: deprecated method calling deprecated method is expected
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
        ActivityClassification activity = activityClassifier_.Classify(
            price, prevPrice, poc, vah, val, deltaPct, tickSize, volumeConviction);
#ifdef _MSC_VER
#pragma warning(pop)
#endif

        // 2. Update state tracker (daltonState/daltonPhase are SSOT)
        StateEvidence evidence = stateTracker_.Update(activity, currentBar, daltonState, daltonPhase);

        // 3. Update excess detection
        ExcessType highExcess = excessDetector_.UpdateHigh(
            sessionHigh, price, tickSize, currentBar, activity, tailAtHigh);
        ExcessType lowExcess = excessDetector_.UpdateLow(
            sessionLow, price, tickSize, currentBar, activity, tailAtLow);

        evidence.excessDetected = excessDetector_.GetCombinedExcess();

        // 4. Fill in value context
        evidence.pocPrice = poc;
        evidence.vahPrice = vah;
        evidence.valPrice = val;
        evidence.distFromPOCTicks = (price - poc) / tickSize;
        evidence.distFromVAHTicks = (price - vah) / tickSize;
        evidence.distFromVALTicks = (price - val) / tickSize;

        // 5. Set range extension flag
        const double prevSessionHigh = sessionHigh;  // Would need tracking
        const double prevSessionLow = sessionLow;
        evidence.rangeExtended = (price >= sessionHigh || price <= sessionLow);

        return evidence;
    }

    /**
     * Process bar with SSOT value location from ValueLocationEngine.
     *
     * @param valLocResult    SSOT: ValueLocationResult from ValueLocationEngine.Compute()
     * @param price           Current price
     * @param prevPrice       Previous bar close
     * @param deltaPct        Delta percentage for this bar
     * @param tickSize        Tick size for conversions
     * @param sessionHigh     Current session high
     * @param sessionLow      Current session low
     * @param currentBar      Current bar index
     * @param tailAtHigh      Tail size at high (ticks)
     * @param tailAtLow       Tail size at low (ticks)
     * @param volumeConviction Volume conviction score [0,1]
     * @param daltonState     SSOT: AMTMarketState from DaltonEngine
     * @param daltonPhase     SSOT: CurrentPhase from DaltonState.DeriveCurrentPhase()
     * @return                Complete StateEvidence
     */
    StateEvidence ProcessBarFromValueLocation(
        const ValueLocationResult& valLocResult,
        double price,
        double prevPrice,
        double deltaPct,
        double tickSize,
        double sessionHigh,
        double sessionLow,
        int currentBar,
        double tailAtHigh = 0.0,
        double tailAtLow = 0.0,
        double volumeConviction = 1.0,
        AMTMarketState daltonState = AMTMarketState::UNKNOWN,
        CurrentPhase daltonPhase = CurrentPhase::UNKNOWN
    ) {
        // 1. Classify activity using SSOT (value location from ValueLocationEngine)
        ActivityClassification activity = activityClassifier_.ClassifyFromValueLocation(
            valLocResult, price, prevPrice, deltaPct, volumeConviction);

        // 2. Update state tracker (daltonState/daltonPhase are SSOT)
        StateEvidence evidence = stateTracker_.Update(activity, currentBar, daltonState, daltonPhase);

        // 3. Update excess detection
        ExcessType highExcess = excessDetector_.UpdateHigh(
            sessionHigh, price, tickSize, currentBar, activity, tailAtHigh);
        ExcessType lowExcess = excessDetector_.UpdateLow(
            sessionLow, price, tickSize, currentBar, activity, tailAtLow);

        evidence.excessDetected = excessDetector_.GetCombinedExcess();

        // 4. Fill in value context - derive prices from SSOT distances
        //    POC = price - (distFromPOCTicks * tickSize)
        //    VAH = price - (distFromVAHTicks * tickSize)
        //    VAL = price - (distFromVALTicks * tickSize)
        evidence.pocPrice = price - (valLocResult.distFromPOCTicks * tickSize);
        evidence.vahPrice = price - (valLocResult.distFromVAHTicks * tickSize);
        evidence.valPrice = price - (valLocResult.distFromVALTicks * tickSize);
        evidence.distFromPOCTicks = valLocResult.distFromPOCTicks;
        evidence.distFromVAHTicks = valLocResult.distFromVAHTicks;
        evidence.distFromVALTicks = valLocResult.distFromVALTicks;

        // 5. Set range extension flag
        evidence.rangeExtended = (price >= sessionHigh || price <= sessionLow);

        return evidence;
    }

    /**
     * Update single print zones from volume profile.
     */
    std::vector<SinglePrintZone> DetectSinglePrints(
        const double* volumeData,
        double priceStart,
        double tickSize,
        int numLevels,
        double avgVolume,
        int currentBar
    ) {
        return singlePrintDetector_.DetectFromProfile(
            volumeData, priceStart, tickSize, numLevels, avgVolume, currentBar);
    }

    /**
     * Update fill progress for existing single print zones.
     */
    void UpdateSinglePrintFill(
        std::vector<SinglePrintZone>& zones,
        const double* volumeData,
        double priceStart,
        double tickSize,
        int numLevels,
        double avgVolume
    ) {
        singlePrintDetector_.UpdateFillProgress(
            zones, volumeData, priceStart, tickSize, numLevels, avgVolume);
    }

    // Component access
    const AMTStateTracker& GetStateTracker() const { return stateTracker_; }
    const ExcessDetector& GetExcessDetector() const { return excessDetector_; }

    // Reset for new session
    void ResetSession() {
        stateTracker_.Reset();
        excessDetector_.Reset();
    }

private:
    ActivityClassifier activityClassifier_;
    AMTStateTracker stateTracker_;
    SinglePrintDetector singlePrintDetector_;
    ExcessDetector excessDetector_;
};

} // namespace AMT

#endif // AMT_SIGNALS_H
