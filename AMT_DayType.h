// ============================================================================
// AMT_DayType.h
// Day Structure Classification (Phase 2)
//
// PURPOSE: Once-per-session structural classification that determines
// BALANCED vs IMBALANCED based on acceptance (sustained trade + volume),
// not mere price movement.
//
// CONTRACT:
//   - Classification is session-scoped SSOT
//   - Immutable once set (no reclassification)
//   - Evidence-based only (no forced time gates)
//   - Delta tracked for diagnostics only, never used in classification
//   - VA migration is confirmatory, not mandatory
//
// PHASE 2 SCOPE:
//   - Binary classification: BALANCED / IMBALANCED / UNDEFINED
//   - Sub-types (NORMAL_DAY, TREND_DAY, etc.) deferred to Phase 3
//   - Does NOT populate balanceType/imbalanceType (Phase 3 responsibility)
// ============================================================================

#ifndef AMT_DAYTYPE_H
#define AMT_DAYTYPE_H

#include "sierrachart.h"
#include "AMT_Patterns.h"  // For DayStructure enum
#include <algorithm>

namespace AMT {

// DayStructure enum is defined in AMT_Patterns.h

// ============================================================================
// DALTON DAY TYPE (Profile Structure Classification)
// ============================================================================
// Based on Jim Dalton's Market Profile framework.
// Classifies day type from IB ratio, extension, rotation pattern (1TF/2TF),
// and close position relative to range.
//
// This is a flat enum combining balanced and imbalanced sub-types for
// convenience in the Dalton framework.
// ============================================================================

enum class DaltonDayType : int {
    UNKNOWN = 0,
    TREND_DAY = 1,              // Narrow IB (<25%), 1TF, extension >3x IB, closes at extreme
    DOUBLE_DISTRIBUTION = 2,    // Two distributions separated by single prints
    NORMAL_DAY = 3,             // Wide IB, stays mostly within IB (50-60% of days)
    NORMAL_VARIATION = 4,       // Extension <2x IB
    NEUTRAL_DAY = 5,            // Extension both sides, closes in value
    NON_TREND_DAY = 6           // Very narrow range, no conviction (holiday/news wait)
};

inline const char* DaltonDayTypeToString(DaltonDayType type) {
    switch (type) {
        case DaltonDayType::UNKNOWN:             return "UNKNOWN";
        case DaltonDayType::TREND_DAY:           return "TREND";
        case DaltonDayType::DOUBLE_DISTRIBUTION: return "DOUBLE_DIST";
        case DaltonDayType::NORMAL_DAY:          return "NORMAL";
        case DaltonDayType::NORMAL_VARIATION:    return "NORMAL_VAR";
        case DaltonDayType::NEUTRAL_DAY:         return "NEUTRAL";
        case DaltonDayType::NON_TREND_DAY:       return "NON_TREND";
        default:                                 return "INVALID";
    }
}

// ============================================================================
// DALTON DAY TYPE CLASSIFICATION THRESHOLDS
// ============================================================================

namespace DaltonThresholds {
    constexpr double TREND_DAY_IB_RATIO = 0.25;      // IB < 25% of range = trend day candidate
    constexpr double NORMAL_VAR_EXTENSION = 2.0;     // Extension < 2x IB = normal variation
    constexpr double TREND_DAY_EXTENSION = 3.0;      // Extension > 3x IB = trend day
    constexpr double CLOSE_AT_EXTREME_RATIO = 0.25;  // Close within 25% of range = at extreme
}

// ============================================================================
// RANGE EXTENSION STATE
// Tracks the lifecycle of an individual RE attempt
// ============================================================================

enum class RangeExtensionState : int {
    NONE = 0,       // No RE attempt active
    ATTEMPTING = 1, // Price outside IB, acceptance window open
    ACCEPTED = 2,   // RE achieved acceptance (sustained trade + volume)
    REJECTED = 3    // RE attempt failed (price returned before acceptance)
};

inline const char* to_string(RangeExtensionState res) {
    switch (res) {
        case RangeExtensionState::NONE:       return "NONE";
        case RangeExtensionState::ATTEMPTING: return "ATTEMPTING";
        case RangeExtensionState::ACCEPTED:   return "ACCEPTED";
        case RangeExtensionState::REJECTED:   return "REJECTED";
    }
    return "UNK";
}

// ============================================================================
// RANGE EXTENSION DIRECTION
// ============================================================================

enum class REDirection : int {
    NONE = 0,
    ABOVE_IB = 1,   // RE attempt above IB high
    BELOW_IB = 2    // RE attempt below IB low
};

inline const char* to_string(REDirection dir) {
    switch (dir) {
        case REDirection::NONE:     return "NONE";
        case REDirection::ABOVE_IB: return "ABOVE";
        case REDirection::BELOW_IB: return "BELOW";
    }
    return "UNK";
}

// ============================================================================
// ACCEPTANCE THRESHOLDS
// ============================================================================

namespace AcceptanceThresholds {
    // Minimum bars outside IB for acceptance (~30 min at 5-min bars)
    constexpr int MIN_BARS = 6;

    // Minimum session volume percentage accumulated outside IB
    constexpr double MIN_VOLUME_PCT = 0.10;  // 10%
}

// ============================================================================
// RE ATTEMPT TRACKER
// Tracks an individual Range Extension attempt with acceptance measurement
// ============================================================================

struct REAttempt {
    REDirection direction = REDirection::NONE;
    int startBar = -1;                    // Bar when price first left IB
    SCDateTime startTime;                 // Time when attempt started
    double furthestExtension = 0.0;       // Max price reached outside IB
    int barsOutsideIB = 0;                // Bars spent outside IB (acceptance time)
    double volumeOutsideIB = 0.0;         // Volume accumulated outside IB
    double deltaOutsideIB = 0.0;          // Net delta accumulated (diagnostics only)
    RangeExtensionState state = RangeExtensionState::NONE;

    void Reset() {
        direction = REDirection::NONE;
        startBar = -1;
        startTime = SCDateTime();
        furthestExtension = 0.0;
        barsOutsideIB = 0;
        volumeOutsideIB = 0.0;
        deltaOutsideIB = 0.0;
        state = RangeExtensionState::NONE;
    }

    bool IsActive() const {
        return state == RangeExtensionState::ATTEMPTING;
    }

    bool IsAccepted() const {
        return state == RangeExtensionState::ACCEPTED;
    }

    bool IsRejected() const {
        return state == RangeExtensionState::REJECTED;
    }
};

// ============================================================================
// DAY TYPE CLASSIFIER
// SSOT for structural classification (once per session)
// ============================================================================

struct DayTypeClassifier {
private:
    // Classification result (immutable once set)
    DayStructure classification_ = DayStructure::UNDEFINED;
    bool classificationLocked_ = false;   // Once true, cannot change
    int classificationBar_ = -1;          // Bar when classification was made
    SCDateTime classificationTime_;       // Time when classification was made

    // VA migration tracking (confirmatory, not mandatory)
    bool vaMigratedAbove_ = false;
    bool vaMigratedBelow_ = false;

    // RE tracking
    REAttempt currentAttempt_;            // Current RE attempt (if any)
    int reAttemptsAbove_ = 0;             // Count of RE attempts above IB
    int reAttemptsBelow_ = 0;             // Count of RE attempts below IB
    int reAcceptedAbove_ = 0;             // Count of accepted REs above
    int reAcceptedBelow_ = 0;             // Count of accepted REs below
    int reRejectedAbove_ = 0;             // Count of rejected REs above
    int reRejectedBelow_ = 0;             // Count of rejected REs below

    // Gates
    bool ibComplete_ = false;             // IB window has closed
    bool profileMature_ = false;          // Sufficient profile data
    int sessionStartBar_ = -1;            // For session reset detection

    // Dalton framework classification (Phase 3)
    DaltonDayType daltonDayType_ = DaltonDayType::UNKNOWN;

public:
    // =========================================================================
    // READ-ONLY ACCESSORS
    // =========================================================================

    DayStructure GetClassification() const { return classification_; }
    bool IsClassified() const { return classificationLocked_; }
    int GetClassificationBar() const { return classificationBar_; }
    bool IsIBComplete() const { return ibComplete_; }
    bool IsProfileMature() const { return profileMature_; }

    // Dalton framework accessor
    DaltonDayType GetDaltonDayType() const { return daltonDayType_; }

    const REAttempt& GetCurrentAttempt() const { return currentAttempt_; }
    int GetREAttemptsAbove() const { return reAttemptsAbove_; }
    int GetREAttemptsBelow() const { return reAttemptsBelow_; }
    int GetREAcceptedAbove() const { return reAcceptedAbove_; }
    int GetREAcceptedBelow() const { return reAcceptedBelow_; }
    int GetRERejectedAbove() const { return reRejectedAbove_; }
    int GetRERejectedBelow() const { return reRejectedBelow_; }

    bool HasVAMigratedAbove() const { return vaMigratedAbove_; }
    bool HasVAMigratedBelow() const { return vaMigratedBelow_; }
    bool HasVAMigrated() const { return vaMigratedAbove_ || vaMigratedBelow_; }

    int GetTotalREAttempts() const { return reAttemptsAbove_ + reAttemptsBelow_; }
    int GetTotalREAccepted() const { return reAcceptedAbove_ + reAcceptedBelow_; }
    int GetTotalRERejected() const { return reRejectedAbove_ + reRejectedBelow_; }

    // =========================================================================
    // NOTIFICATION METHODS
    // =========================================================================

    void NotifyIBComplete(int bar, SCDateTime time) {
        if (ibComplete_) return;  // Already complete
        ibComplete_ = true;
    }

    void NotifyProfileMature(bool mature) {
        profileMature_ = mature;
    }

    // =========================================================================
    // RE TRACKING UPDATE (called each bar after IB complete)
    // =========================================================================

    RangeExtensionState UpdateRETracking(
        double barHigh,            // Bar high price (for detecting RE above IB)
        double barLow,             // Bar low price (for detecting RE below IB)
        double barClose,           // Bar close price (for rejection detection)
        double ibHigh,
        double ibLow,
        double barVolume,
        double barDelta,           // Diagnostics only, not used in logic
        double sessionTotalVolume,
        int currentBar,
        SCDateTime currentTime,
        double tickSize)
    {
        if (!ibComplete_) return RangeExtensionState::NONE;
        if (classificationLocked_) return currentAttempt_.state;

        // Determine if bar extended outside IB (use High/Low, not Close)
        // This catches RE even when bar closes back inside IB
        const bool outsideIBAbove = (barHigh > ibHigh);
        const bool outsideIBBelow = (barLow < ibLow);
        const bool outsideIB = outsideIBAbove || outsideIBBelow;

        // For rejection detection: is close back inside IB?
        const bool closeInsideIB = (barClose <= ibHigh && barClose >= ibLow);

        // No active attempt - check for new RE
        if (!currentAttempt_.IsActive()) {
            if (outsideIB) {
                // Start new RE attempt
                currentAttempt_.direction = outsideIBAbove ?
                    REDirection::ABOVE_IB : REDirection::BELOW_IB;
                currentAttempt_.startBar = currentBar;
                currentAttempt_.startTime = currentTime;
                currentAttempt_.state = RangeExtensionState::ATTEMPTING;
                // Use actual extension price (high for above, low for below)
                currentAttempt_.furthestExtension = outsideIBAbove ? barHigh : barLow;
                currentAttempt_.barsOutsideIB = 1;
                currentAttempt_.volumeOutsideIB = barVolume;
                currentAttempt_.deltaOutsideIB = barDelta;

                // Increment attempt count
                if (outsideIBAbove) reAttemptsAbove_++;
                else reAttemptsBelow_++;

                return RangeExtensionState::ATTEMPTING;
            }
            return RangeExtensionState::NONE;
        }

        // Active attempt - check if bar CLOSE is outside IB in the attempt direction
        // BUG FIX: Previous logic only counted bars where HIGH/LOW extended further.
        // This caused consolidation bars outside IB to not count toward the 6-bar acceptance.
        // Market Profile acceptance = TIME outside IB, not just extension bars.
        const bool closeOutsideAbove = (barClose > ibHigh);
        const bool closeOutsideBelow = (barClose < ibLow);
        const bool closeContinuesAbove = (currentAttempt_.direction == REDirection::ABOVE_IB && closeOutsideAbove);
        const bool closeContinuesBelow = (currentAttempt_.direction == REDirection::BELOW_IB && closeOutsideBelow);
        const bool closeContinuesRE = closeContinuesAbove || closeContinuesBelow;

        // Also track if bar HIGH/LOW actually extended (for furthest extension tracking)
        const bool extendsAbove = (currentAttempt_.direction == REDirection::ABOVE_IB && outsideIBAbove);
        const bool extendsBelow = (currentAttempt_.direction == REDirection::BELOW_IB && outsideIBBelow);
        const bool extendsRE = extendsAbove || extendsBelow;

        if (closeContinuesRE) {
            // Bar CLOSES outside IB in the RE direction - accumulate toward acceptance
            currentAttempt_.barsOutsideIB++;
            currentAttempt_.volumeOutsideIB += barVolume;
            currentAttempt_.deltaOutsideIB += barDelta;

            // Update furthest extension only when bar actually extends (HIGH/LOW)
            if (extendsRE) {
                if (currentAttempt_.direction == REDirection::ABOVE_IB) {
                    currentAttempt_.furthestExtension =
                        (std::max)(currentAttempt_.furthestExtension, barHigh);
                } else {
                    currentAttempt_.furthestExtension =
                        (std::min)(currentAttempt_.furthestExtension, barLow);
                }
            }

            // Check for acceptance (bars + volume criteria)
            const bool timeAccepted =
                (currentAttempt_.barsOutsideIB >= AcceptanceThresholds::MIN_BARS);
            const double volumePct = sessionTotalVolume > 0 ?
                currentAttempt_.volumeOutsideIB / sessionTotalVolume : 0.0;
            const bool volumeAccepted =
                (volumePct >= AcceptanceThresholds::MIN_VOLUME_PCT);

            if (timeAccepted && volumeAccepted) {
                // RE accepted!
                currentAttempt_.state = RangeExtensionState::ACCEPTED;
                if (currentAttempt_.direction == REDirection::ABOVE_IB) {
                    reAcceptedAbove_++;
                } else {
                    reAcceptedBelow_++;
                }
                return RangeExtensionState::ACCEPTED;
            }

            return RangeExtensionState::ATTEMPTING;
        }

        // Bar CLOSE no longer continues RE direction - handle rejection or reversal
        if (currentAttempt_.state == RangeExtensionState::ATTEMPTING) {
            // Case 1: Close returned inside IB -> reject
            // Case 2: Close went to opposite side of IB (e.g., tracking ABOVE but closed BELOW) -> also reject
            // Only continue if close is still outside IB in the same direction (but not accumulating)

            const bool closeOnWrongSide =
                (currentAttempt_.direction == REDirection::ABOVE_IB && barClose < ibLow) ||
                (currentAttempt_.direction == REDirection::BELOW_IB && barClose > ibHigh);

            if (closeInsideIB || closeOnWrongSide) {
                // Reject the attempt - price returned inside IB or reversed through
                currentAttempt_.state = RangeExtensionState::REJECTED;
                if (currentAttempt_.direction == REDirection::ABOVE_IB) {
                    reRejectedAbove_++;
                } else {
                    reRejectedBelow_++;
                }

                // Clear for next potential attempt
                currentAttempt_.Reset();
                return RangeExtensionState::REJECTED;
            }

            // Close still outside IB in correct direction but didn't meet accumulation criteria
            // This shouldn't happen with the new CLOSE-based logic, but handle gracefully
            return RangeExtensionState::ATTEMPTING;
        }

        // Edge case: clear stale state
        currentAttempt_.Reset();
        return RangeExtensionState::NONE;
    }

    // =========================================================================
    // VA MIGRATION UPDATE (called each bar, confirmatory only)
    // =========================================================================

    void UpdateVAMigration(double vah, double val, double ibHigh, double ibLow, double tickSize) {
        if (!ibComplete_) return;

        // Check if VA has migrated outside IB (confirmatory evidence)
        if (vah > ibHigh + tickSize) {
            vaMigratedAbove_ = true;
        }
        if (val < ibLow - tickSize) {
            vaMigratedBelow_ = true;
        }
    }

    // =========================================================================
    // DALTON DAY TYPE CLASSIFICATION (Profile-based, uses IB + rotation)
    // =========================================================================
    // This is the Dalton Market Profile approach:
    // - Classifies from IB ratio, extension ratio, 1TF/2TF pattern, close position
    // - Complementary to RE-based DayStructure classification
    //
    // @param ibHigh         Initial Balance high
    // @param ibLow          Initial Balance low
    // @param sessionHigh    Current session high
    // @param sessionLow     Current session low
    // @param close          Current close price
    // @param is1TF          True if One-Time Framing detected
    // @param extensionAbove True if extended above IB
    // @param extensionBelow True if extended below IB
    // =========================================================================

    DaltonDayType ClassifyDaltonDayType(
        double ibHigh,
        double ibLow,
        double sessionHigh,
        double sessionLow,
        double close,
        bool is1TF,
        bool extensionAbove,
        bool extensionBelow
    ) {
        // Need IB complete to classify
        if (!ibComplete_ || ibHigh <= ibLow) {
            daltonDayType_ = DaltonDayType::UNKNOWN;
            return daltonDayType_;
        }

        const double ibRange = ibHigh - ibLow;
        const double sessionRange = sessionHigh - sessionLow;

        // Guard against division by zero
        if (sessionRange <= 0.0 || ibRange <= 0.0) {
            daltonDayType_ = DaltonDayType::UNKNOWN;
            return daltonDayType_;
        }

        const double ibRatio = ibRange / sessionRange;          // IB as fraction of total range
        const double extensionRatio = sessionRange / ibRange;   // How much we extended vs IB

        // Check close position relative to range
        const double rangePos = (close - sessionLow) / sessionRange;
        const bool atHighExtreme = rangePos >= (1.0 - DaltonThresholds::CLOSE_AT_EXTREME_RATIO);
        const bool atLowExtreme = rangePos <= DaltonThresholds::CLOSE_AT_EXTREME_RATIO;
        const bool atExtreme = atHighExtreme || atLowExtreme;

        // Close in value area (middle of range)
        const bool closeInValue = (close >= ibLow && close <= ibHigh);

        // Determine extension type
        const bool extendedBoth = extensionAbove && extensionBelow;

        // =====================================================================
        // CLASSIFICATION LOGIC (Dalton framework)
        // =====================================================================

        // TREND DAY: Narrow IB, 1TF, large extension, closes at extreme
        // The most directional day type - strong one-sided conviction
        if (ibRatio < DaltonThresholds::TREND_DAY_IB_RATIO &&
            is1TF &&
            extensionRatio >= DaltonThresholds::TREND_DAY_EXTENSION &&
            atExtreme) {
            daltonDayType_ = DaltonDayType::TREND_DAY;
            return daltonDayType_;
        }

        // NEUTRAL DAY: Extension both sides, closes in value
        // Two-sided auction, neither side won
        if (extendedBoth && closeInValue) {
            daltonDayType_ = DaltonDayType::NEUTRAL_DAY;
            return daltonDayType_;
        }

        // NON-TREND DAY: Very narrow range, no conviction
        // Often seen on holidays or before major news
        if (extensionRatio <= 1.1 && ibRatio > 0.8) {
            daltonDayType_ = DaltonDayType::NON_TREND_DAY;
            return daltonDayType_;
        }

        // NORMAL VARIATION: Extension < 2x IB
        // Mild directional bias but not trend-like
        if (extensionRatio < DaltonThresholds::NORMAL_VAR_EXTENSION) {
            daltonDayType_ = DaltonDayType::NORMAL_VARIATION;
            return daltonDayType_;
        }

        // NORMAL DAY: Wide IB, stays within (no significant extension)
        // Most common day type (50-60% of days)
        if (!extensionAbove && !extensionBelow) {
            daltonDayType_ = DaltonDayType::NORMAL_DAY;
            return daltonDayType_;
        }

        // Default to Normal Variation for remaining cases
        daltonDayType_ = DaltonDayType::NORMAL_VARIATION;
        return daltonDayType_;
    }

    // =========================================================================
    // CLASSIFICATION ATTEMPT (returns true if classification changed)
    // =========================================================================

    bool TryClassify(int currentBar, SCDateTime currentTime) {
        // Gate checks
        if (!ibComplete_) return false;
        if (!profileMature_) return false;
        if (classificationLocked_) return false;

        // Evidence-based classification (no forced time gates)

        // IMBALANCED: Any RE accepted
        if (reAcceptedAbove_ > 0 || reAcceptedBelow_ > 0) {
            classification_ = DayStructure::IMBALANCED;
            classificationLocked_ = true;
            classificationBar_ = currentBar;
            classificationTime_ = currentTime;
            return true;
        }

        // BALANCED: Do NOT lock mid-session! Price may extend outside IB later.
        // BUG FIX: Previously locked BALANCED immediately if no active RE, which prevented
        // IMBALANCED classification even when RE was later accepted.
        //
        // New behavior:
        // - Set tentative BALANCED (not locked) so STRUCT= shows something useful
        // - Only lock at session end via TryClassifyAtSessionEnd()
        // - Allow IMBALANCED to override at any time if RE is accepted
        const bool hasActiveAttempt = (currentAttempt_.state == RangeExtensionState::ATTEMPTING);

        if (!hasActiveAttempt) {
            // No active attempt - set tentative BALANCED but DON'T lock
            // This allows later RE attempts to still trigger IMBALANCED
            classification_ = DayStructure::BALANCED;
            // classificationLocked_ = false;  // Explicitly NOT locking
            classificationBar_ = currentBar;
            classificationTime_ = currentTime;
            // Return false - classification is tentative, not final
            return false;
        }

        // Active RE attempt in progress - wait for resolution
        return false;
    }

    // =========================================================================
    // SESSION END CLASSIFICATION (called at session boundary)
    // =========================================================================

    bool TryClassifyAtSessionEnd(int currentBar, SCDateTime currentTime) {
        if (classificationLocked_) return false;
        if (!ibComplete_) {
            // IB never completed - leave UNDEFINED
            return false;
        }

        // IMBALANCED: Any RE accepted (check again in case missed)
        if (reAcceptedAbove_ > 0 || reAcceptedBelow_ > 0) {
            classification_ = DayStructure::IMBALANCED;
            classificationLocked_ = true;
            classificationBar_ = currentBar;
            classificationTime_ = currentTime;
            return true;
        }

        // BALANCED: All RE attempts rejected OR no RE attempts
        const int totalAttempts = reAttemptsAbove_ + reAttemptsBelow_;
        const int totalAccepted = reAcceptedAbove_ + reAcceptedBelow_;

        if (totalAttempts == 0) {
            // No RE attempts - pure rotation within IB
            classification_ = DayStructure::BALANCED;
            classificationLocked_ = true;
            classificationBar_ = currentBar;
            classificationTime_ = currentTime;
            return true;
        }

        if (totalAccepted == 0) {
            // All attempts rejected
            classification_ = DayStructure::BALANCED;
            classificationLocked_ = true;
            classificationBar_ = currentBar;
            classificationTime_ = currentTime;
            return true;
        }

        // Still have active attempt that wasn't resolved - stay UNDEFINED
        return false;
    }

    // =========================================================================
    // RESET FOR NEW SESSION
    // =========================================================================

    void Reset(int sessionStartBar) {
        classification_ = DayStructure::UNDEFINED;
        classificationLocked_ = false;
        classificationBar_ = -1;
        classificationTime_ = SCDateTime();

        vaMigratedAbove_ = false;
        vaMigratedBelow_ = false;

        currentAttempt_.Reset();
        reAttemptsAbove_ = 0;
        reAttemptsBelow_ = 0;
        reAcceptedAbove_ = 0;
        reAcceptedBelow_ = 0;
        reRejectedAbove_ = 0;
        reRejectedBelow_ = 0;

        ibComplete_ = false;
        profileMature_ = false;
        sessionStartBar_ = sessionStartBar;

        // Reset Dalton day type
        daltonDayType_ = DaltonDayType::UNKNOWN;
    }

    // =========================================================================
    // LOGGING HELPERS
    // =========================================================================

    // Format RE summary: "RE_UP=1/2 RE_DN=0/1"
    // accepted/attempts for each direction
    void FormatRESummary(char* buffer, size_t bufferSize) const {
        snprintf(buffer, bufferSize, "RE_UP=%d/%d RE_DN=%d/%d",
            reAcceptedAbove_, reAttemptsAbove_,
            reAcceptedBelow_, reAttemptsBelow_);
    }

    // Format VA migration status
    const char* FormatVAMigration() const {
        if (vaMigratedAbove_ && vaMigratedBelow_) return "BOTH";
        if (vaMigratedAbove_) return "ABOVE";
        if (vaMigratedBelow_) return "BELOW";
        return "NONE";
    }

    // Get primary RE direction (for Phase 3 semantic mapping)
    REDirection GetPrimaryREDirection() const {
        // If accepted in both directions, prefer the one with more volume
        if (reAcceptedAbove_ > 0 && reAcceptedBelow_ > 0) {
            // Both accepted - this is rare, return the first one
            return REDirection::ABOVE_IB;
        }
        if (reAcceptedAbove_ > 0) return REDirection::ABOVE_IB;
        if (reAcceptedBelow_ > 0) return REDirection::BELOW_IB;
        return REDirection::NONE;
    }
};

// ============================================================================
// PHASE 3: SEMANTIC DAY TYPE MAPPING
// Pure function: (structure, shape, RE metadata) -> semantic subtype
// ============================================================================

struct SemanticMappingResult {
    BalanceStructure balanceType = BalanceStructure::NONE;
    ImbalanceStructure imbalanceType = ImbalanceStructure::NONE;
    const char* evidence = "";  // Brief reason for logging

    bool HasSubtype() const {
        return balanceType != BalanceStructure::NONE ||
               imbalanceType != ImbalanceStructure::NONE;
    }
};

// ============================================================================
// MapStructureToSemantics
// Pure function implementing Phase 3 semantic mapping
//
// CONTRACT:
//   - dayStructure is SSOT from Phase 2 (never modified)
//   - If shape is UNDEFINED, subtype remains NONE
//   - Uses explicit fallbacks (*_OTHER) rather than guessing
//   - No delta, no predictions, just taxonomy
// ============================================================================

inline SemanticMappingResult MapStructureToSemantics(
    DayStructure dayStructure,
    BalanceProfileShape balanceShape,
    ImbalanceProfileShape imbalanceShape,
    REDirection primaryREDirection)
{
    SemanticMappingResult result;

    // UNDEFINED structure -> no semantic mapping possible (NONE is correct here)
    if (dayStructure == DayStructure::UNDEFINED) {
        result.evidence = "structure undefined";
        return result;
    }

    // =========================================================================
    // BALANCED DAY MAPPING
    // =========================================================================
    if (dayStructure == DayStructure::BALANCED) {
        // Check balance shape first
        switch (balanceShape) {
            case BalanceProfileShape::NORMAL_DISTRIBUTION:
                result.balanceType = BalanceStructure::NORMAL_DAY;
                result.evidence = "BALANCED + NORMAL_DIST";
                return result;

            case BalanceProfileShape::D_SHAPED:
                result.balanceType = BalanceStructure::DOUBLE_DISTRIBUTION_DAY;
                result.evidence = "BALANCED + D_SHAPED";
                return result;

            case BalanceProfileShape::BALANCED:
                // Generic balanced shape - use NORMAL_DAY as closest match
                result.balanceType = BalanceStructure::NORMAL_DAY;
                result.evidence = "BALANCED + BALANCED_SHAPE";
                return result;

            case BalanceProfileShape::UNDEFINED:
                // Check imbalance shape as secondary (rare for balanced day)
                if (imbalanceShape == ImbalanceProfileShape::THIN_VERTICAL) {
                    result.balanceType = BalanceStructure::NEUTRAL_DAY_CENTER;
                    result.evidence = "BALANCED + THIN_VERT";
                    return result;
                }
                // Shape unmapped but structure is BALANCED -> explicit fallback
                result.balanceType = BalanceStructure::BALANCED_OTHER;
                result.evidence = "BALANCED, shape unmapped";
                return result;
        }
    }

    // =========================================================================
    // IMBALANCED DAY MAPPING
    // =========================================================================
    if (dayStructure == DayStructure::IMBALANCED) {
        // Check imbalance shape first
        switch (imbalanceShape) {
            case ImbalanceProfileShape::P_SHAPED:
                // P-shape requires RE_ABOVE for directional consistency
                if (primaryREDirection == REDirection::ABOVE_IB) {
                    result.imbalanceType = ImbalanceStructure::TREND_DAY;
                    result.evidence = "IMBALANCED + P_SHAPE + RE_ABOVE";
                } else {
                    // P-shape but RE not above is directionally inconsistent
                    result.imbalanceType = ImbalanceStructure::IMBALANCED_OTHER;
                    result.evidence = "IMBALANCED + P_SHAPE, RE mismatch";
                }
                return result;

            case ImbalanceProfileShape::B_SHAPED_LOWER:
            case ImbalanceProfileShape::B_SHAPED_BIMODAL:
                // B-shape requires RE_BELOW for directional consistency
                if (primaryREDirection == REDirection::BELOW_IB) {
                    result.imbalanceType = ImbalanceStructure::TREND_DAY;
                    result.evidence = "IMBALANCED + B_SHAPE + RE_BELOW";
                } else {
                    // B-shape but RE not below is directionally inconsistent
                    result.imbalanceType = ImbalanceStructure::IMBALANCED_OTHER;
                    result.evidence = "IMBALANCED + B_SHAPE, RE mismatch";
                }
                return result;

            case ImbalanceProfileShape::THIN_VERTICAL:
                // Thin vertical = directional but structurally thin
                result.imbalanceType = ImbalanceStructure::EXPANSION_DAY;
                result.evidence = "IMBALANCED + THIN_VERT";
                return result;

            case ImbalanceProfileShape::UNDEFINED:
                // Check balance shape as secondary
                if (balanceShape == BalanceProfileShape::D_SHAPED) {
                    // D-shape on imbalanced day is rare but valid
                    result.imbalanceType = ImbalanceStructure::IMBALANCED_OTHER;
                    result.evidence = "IMBALANCED + D_SHAPED (rare)";
                    return result;
                }
                // Shape unmapped but structure is IMBALANCED -> explicit fallback
                result.imbalanceType = ImbalanceStructure::IMBALANCED_OTHER;
                result.evidence = "IMBALANCED, shape unmapped";
                return result;
        }
    }

    return result;
}

} // namespace AMT

#endif // AMT_DAYTYPE_H
