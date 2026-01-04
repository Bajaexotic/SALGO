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
     * Classify activity for a bar given price context and delta.
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

        // 5. Determine Location
        result.location = DetermineLocation(price, poc, vah, val, tickSize);

        // 6. Determine Intent (value-relative direction) - internal
        result.intent_ = DetermineIntent(price, prevPrice, poc, tickSize);

        // 7. Determine Participation (delta vs price alignment) - internal
        result.participation_ = DetermineParticipation(result.priceChange, deltaPct);

        // 8. Derive activity type from Intent × Participation
        result.DeriveActivityType();

        return result;
    }

private:
    Config config_;

    ValueLocation DetermineLocation(
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
            return ValueLocation::AT_POC;
        }

        // Check boundaries
        if (std::abs(distFromVAH) <= config_.vaBoundaryTicks) {
            return ValueLocation::AT_VAH;
        }
        if (std::abs(distFromVAL) <= config_.vaBoundaryTicks) {
            return ValueLocation::AT_VAL;
        }

        // Check outside value
        if (price > vah) {
            return ValueLocation::ABOVE_VALUE;
        }
        if (price < val) {
            return ValueLocation::BELOW_VALUE;
        }

        // Inside value area
        return ValueLocation::INSIDE_VALUE;
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
// Maintains BALANCE/IMBALANCE state using a leaky accumulator.
//
// The accumulator tracks "imbalance strength":
//   - INITIATIVE activity adds to strength (moving away from value)
//   - RESPONSIVE activity subtracts from strength (returning to value)
//   - Each bar applies decay (information ages out)
//
// State flips are strength-modulated:
//   - Higher strength required to flip from BALANCE to IMBALANCE
//   - Lower strength threshold to flip back to BALANCE (hysteresis)
// ============================================================================

class AMTStateTracker {
public:
    struct Config {
        double decayRate = 0.95;              // Per-bar decay multiplier
        double gainInitiative = 0.15;         // Gain per initiative bar
        double gainResponsive = 0.10;         // Gain per responsive bar
        double balanceToImbalanceThreshold = 0.60;  // Base threshold to flip to IMBALANCE
        double imbalanceToBalanceThreshold = 0.40;  // Base threshold to flip to BALANCE

        // Location modifiers: outside value amplifies initiative signal
        double outsideValueMultiplier = 1.5;
        double atBoundaryMultiplier = 1.2;

        // Location bias: being outside value is itself evidence of imbalance
        // This small positive bias prevents decay to 0 during consolidation outside VA
        double outsideValueBias = 0.05;

        Config() = default;
    };

    AMTStateTracker() : config_(), currentState_(AMTMarketState::UNKNOWN),
        imbalanceStrength_(0.5), barsInState_(0), previousState_(AMTMarketState::UNKNOWN) {}
    explicit AMTStateTracker(const Config& cfg)
        : config_(cfg)
        , currentState_(AMTMarketState::UNKNOWN)
        , imbalanceStrength_(0.5)  // Start neutral
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
     * The strength accumulator now tracks "confirmation" rather than determining state.
     *
     * @param activity    This bar's activity classification (determines WHO)
     * @param currentBar  Current bar index (for transition logging)
     * @param daltonState The authoritative state from DaltonEngine (1TF/2TF derived)
     *                    If UNKNOWN, falls back to accumulator-based state (legacy mode)
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
            evidence.stateStrength = imbalanceStrength_;
            return evidence;
        }

        // Store previous state for transition detection
        previousState_ = currentState_;

        // 1. Apply decay to existing strength
        imbalanceStrength_ *= config_.decayRate;

        // 2. Compute contribution based on activity type, location, and volume conviction
        // Per Dalton: Volume confirms conviction
        // - VOLUME_VACUUM (low percentile) → volumeConviction near 0 → contribution minimized
        // - High volume (high percentile) → volumeConviction > 1 → contribution amplified
        double contribution = 0.0;
        const double locationMultiplier = GetLocationMultiplier(activity.location);
        const double volumeWeight = activity.volumeConviction;  // 0.0-2.0, 1.0 = normal

        if (activity.activityType == AMTActivityType::INITIATIVE) {
            contribution = config_.gainInitiative * locationMultiplier * volumeWeight;
        }
        else if (activity.activityType == AMTActivityType::RESPONSIVE) {
            contribution = -config_.gainResponsive * locationMultiplier * volumeWeight;
        }
        // NEUTRAL contributes 0

        // 3. Location bias: Being OUTSIDE value is itself evidence of imbalance
        // Add a small positive bias when price is outside the value area
        // This prevents strength from decaying to 0 just because we're consolidating
        // Note: Location bias is NOT volume-weighted (structural, not activity-based)
        if (activity.location == ValueLocation::ABOVE_VALUE ||
            activity.location == ValueLocation::BELOW_VALUE) {
            contribution += config_.outsideValueBias;
        }

        // 4. Update strength (clamp to [0, 1])
        // NOTE: Strength is now a "confirmation metric", not the state determinant
        imbalanceStrength_ += contribution;
        imbalanceStrength_ = (std::max)(0.0, (std::min)(1.0, imbalanceStrength_));

        // 5. Determine state: Dalton (1TF/2TF) is SSOT, accumulator is fallback
        AMTMarketState newState;
        if (daltonState != AMTMarketState::UNKNOWN) {
            // SSOT: Use Dalton's 1TF/2TF derived state
            newState = daltonState;
        } else {
            // Fallback: Use accumulator-based state (legacy mode or warmup)
            newState = DetermineState();
        }

        // 6. Update state tracking
        if (newState != currentState_) {
            evidence.previousState = currentState_;
            evidence.strengthAtTransition = imbalanceStrength_;
            evidence.barAtTransition = currentBar;
            currentState_ = newState;
            barsInState_ = 1;
        }
        else {
            evidence.previousState = previousState_;
            barsInState_++;
        }

        // 7. Populate evidence
        evidence.currentState = currentState_;
        evidence.stateStrength = imbalanceStrength_;
        evidence.barsInState = barsInState_;
        evidence.activity = activity;
        evidence.location = activity.location;

        // 8. Store derived phase (SSOT: Dalton, fallback: local derivation)
        evidence.derivedPhase = daltonPhase;

        return evidence;
    }

    // Getters
    AMTMarketState GetCurrentState() const { return currentState_; }
    double GetStrength() const { return imbalanceStrength_; }
    int GetBarsInState() const { return barsInState_; }

    // Reset for new session
    void Reset() {
        currentState_ = AMTMarketState::UNKNOWN;
        imbalanceStrength_ = 0.5;
        barsInState_ = 0;
        previousState_ = AMTMarketState::UNKNOWN;
    }

private:
    Config config_;
    AMTMarketState currentState_;
    double imbalanceStrength_;
    int barsInState_;
    AMTMarketState previousState_;

    double GetLocationMultiplier(ValueLocation location) const {
        switch (location) {
            case ValueLocation::ABOVE_VALUE:
            case ValueLocation::BELOW_VALUE:
                return config_.outsideValueMultiplier;

            case ValueLocation::AT_VAH:
            case ValueLocation::AT_VAL:
                return config_.atBoundaryMultiplier;

            default:
                return 1.0;
        }
    }

    AMTMarketState DetermineState() const {
        // Hysteresis: different thresholds for each direction
        if (currentState_ == AMTMarketState::BALANCE ||
            currentState_ == AMTMarketState::UNKNOWN) {
            // Currently in BALANCE - need higher strength to flip to IMBALANCE
            if (imbalanceStrength_ >= config_.balanceToImbalanceThreshold) {
                return AMTMarketState::IMBALANCE;
            }
            return AMTMarketState::BALANCE;
        }
        else {
            // Currently in IMBALANCE - need lower strength to flip back to BALANCE
            if (imbalanceStrength_ <= config_.imbalanceToBalanceThreshold) {
                return AMTMarketState::BALANCE;
            }
            return AMTMarketState::IMBALANCE;
        }
    }
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
        AMTStateTracker::Config stateConfig;
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
        , stateTracker_(cfg.stateConfig)
        , singlePrintDetector_(cfg.singlePrintConfig)
        , excessDetector_(cfg.excessConfig)
    {}

    /**
     * Process a bar and update all AMT signals.
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
        ActivityClassification activity = activityClassifier_.Classify(
            price, prevPrice, poc, vah, val, deltaPct, tickSize, volumeConviction);

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
