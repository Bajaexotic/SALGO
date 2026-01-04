// ============================================================================
// AMT_BehaviorMapping.h
// Profile Shape → Behavioral Hypothesis Mapping (v1.2 Specification)
//
// SSOT: docs/profile_shape_behavior_mapping.md
//
// This module implements:
//   1. Outcome labels (O1-O5) with formal detection logic
//   2. Shape → Hypothesis mapping
//   3. Frozen reference management
//   4. Edge case handling per specification
//
// NO FALLBACKS: If outcome cannot be determined, returns UNRESOLVED.
// NO LOOKAHEAD: All detection uses bars strictly after t_freeze.
// ============================================================================

#ifndef AMT_BEHAVIORMAPPING_H
#define AMT_BEHAVIORMAPPING_H

#include "amt_core.h"
#include "AMT_ProfileShape.h"
#include <cmath>
#include <algorithm>

namespace AMT {

// ============================================================================
// OUTCOME LABELS (O1-O5 + UNRESOLVED)
// Per specification §2.1
// ============================================================================

enum class BehaviorOutcome : int {
    PENDING = 0,           // Observation in progress, no outcome yet
    O1_CONTINUATION_UP,    // Sustained breakout above VAH
    O2_CONTINUATION_DN,    // Sustained breakout below VAL
    O3_MEAN_REVERT_HIGH,   // Touched VAH, returned to VA_mid
    O4_MEAN_REVERT_LOW,    // Touched VAL, returned to VA_mid
    O5_RANGE_BOUND,        // No sustained breakout, no completed reversion
    UNRESOLVED             // Session ended before outcome determined
};

inline const char* BehaviorOutcomeToString(BehaviorOutcome o) {
    switch (o) {
        case BehaviorOutcome::PENDING: return "PENDING";
        case BehaviorOutcome::O1_CONTINUATION_UP: return "O1_CONT_UP";
        case BehaviorOutcome::O2_CONTINUATION_DN: return "O2_CONT_DN";
        case BehaviorOutcome::O3_MEAN_REVERT_HIGH: return "O3_MR_HIGH";
        case BehaviorOutcome::O4_MEAN_REVERT_LOW: return "O4_MR_LOW";
        case BehaviorOutcome::O5_RANGE_BOUND: return "O5_RANGE";
        case BehaviorOutcome::UNRESOLVED: return "UNRESOLVED";
    }
    return "UNKNOWN";
}

// ============================================================================
// HYPOTHESIS TYPE
// Per specification §3.1
// ============================================================================

enum class HypothesisType : int {
    NONE = 0,              // No hypothesis (UNDEFINED shape or gated dependency)
    CONTINUATION_UP,       // Expect O1
    CONTINUATION_DN,       // Expect O2
    MEAN_REVERSION,        // Expect O3 or O4 (direction determined by price location)
    MEAN_REVERSION_HIGH,   // Expect O3 specifically (D_SHAPED a > 0)
    MEAN_REVERSION_LOW,    // Expect O4 specifically (D_SHAPED a < 0)
    RANGE_BOUND            // Expect O5
};

inline const char* HypothesisTypeToString(HypothesisType h) {
    switch (h) {
        case HypothesisType::NONE: return "NONE";
        case HypothesisType::CONTINUATION_UP: return "CONT_UP";
        case HypothesisType::CONTINUATION_DN: return "CONT_DN";
        case HypothesisType::MEAN_REVERSION: return "MEAN_REV";
        case HypothesisType::MEAN_REVERSION_HIGH: return "MR_HIGH";
        case HypothesisType::MEAN_REVERSION_LOW: return "MR_LOW";
        case HypothesisType::RANGE_BOUND: return "RANGE_BOUND";
    }
    return "UNKNOWN";
}

// ============================================================================
// FROZEN REFERENCES (per specification §1.2)
// Captured at t_freeze, never updated
// ============================================================================

struct FrozenReferences {
    // Core frozen levels
    float POC_0 = 0.0f;
    float VAH_0 = 0.0f;
    float VAL_0 = 0.0f;
    float VA_mid_0 = 0.0f;
    float W_va = 0.0f;
    float R_0 = 0.0f;

    // Freeze timing
    int t_freeze = -1;      // Bar index at freeze
    int t_end = -1;         // Session end bar (set when known)

    // Classification at freeze
    ProfileShape shape = ProfileShape::UNDEFINED;
    float asymmetry = 0.0f; // For D_SHAPED direction

    // Validation
    bool valid = false;

    // Compute derived values
    void ComputeDerived() {
        VA_mid_0 = (VAH_0 + VAL_0) / 2.0f;
        W_va = VAH_0 - VAL_0;
    }

    // Check if levels are valid
    bool IsValid() const {
        return valid && W_va > 0.0f && t_freeze >= 0;
    }
};

// ============================================================================
// BREAKOUT ATTEMPT TRACKER
// Tracks O1/O2 trigger and hold state per specification §2.2
// ============================================================================

struct BreakoutAttempt {
    int t_brk = -1;              // Trigger bar (-1 = no active attempt)
    int holdBarsRemaining = 0;   // Bars remaining in hold window
    bool isUpBreakout = true;    // true = O1 attempt, false = O2 attempt

    void Reset() {
        t_brk = -1;
        holdBarsRemaining = 0;
    }

    bool IsActive() const { return t_brk >= 0; }
};

// ============================================================================
// BEHAVIOR OBSERVATION STATE
// Tracks all outcome detection state during observation window
// ============================================================================

struct BehaviorObservation {
    // Frozen references (set once at t_freeze)
    FrozenReferences frozen;

    // Observation parameters (per specification §2.4)
    int N = 3;                           // Hold bars for breakout confirmation
    float toleranceRatio = 0.25f;        // tolerance = 0.25 * W_va

    // Derived tolerance (computed from W_va)
    float tolerance = 0.0f;

    // Current state
    BehaviorOutcome outcome = BehaviorOutcome::PENDING;
    int completionBar = -1;              // Bar where outcome was determined

    // Breakout tracking (O1/O2)
    BreakoutAttempt upBreakout;
    BreakoutAttempt dnBreakout;

    // Mean-reversion tracking (O3/O4)
    bool touchedVAH = false;             // Ever touched VAH_0
    bool touchedVAL = false;             // Ever touched VAL_0
    int firstTouchBar = -1;              // Bar of first boundary touch
    bool firstTouchWasHigh = false;      // Which boundary was touched first

    // Session tracking
    bool sessionEnded = false;

    // Initialize from frozen references
    void Initialize(const FrozenReferences& refs, int holdBars = 3, float tolRatio = 0.25f) {
        frozen = refs;
        N = holdBars;
        toleranceRatio = tolRatio;
        tolerance = tolRatio * frozen.W_va;

        outcome = BehaviorOutcome::PENDING;
        completionBar = -1;

        upBreakout.Reset();
        dnBreakout.Reset();

        touchedVAH = false;
        touchedVAL = false;
        firstTouchBar = -1;
        firstTouchWasHigh = false;

        sessionEnded = false;
    }

    bool IsComplete() const {
        return outcome != BehaviorOutcome::PENDING;
    }
};

// ============================================================================
// OUTCOME DETECTION ENGINE
// Implements formal outcome definitions from specification §2.2
// ============================================================================

class OutcomeDetector {
public:
    // -------------------------------------------------------------------------
    // Process a single bar (call once per bar after t_freeze)
    // Returns true if outcome was just determined
    // -------------------------------------------------------------------------
    static bool ProcessBar(
        BehaviorObservation& obs,
        int barIndex,
        float P_hi,
        float P_lo,
        float P_t)  // P_t = close price
    {
        // Already complete - no further processing
        if (obs.IsComplete()) {
            return false;
        }

        // Validate frozen references
        if (!obs.frozen.IsValid()) {
            return false;
        }

        // Must be after t_freeze
        if (barIndex <= obs.frozen.t_freeze) {
            return false;
        }

        const float VAH = obs.frozen.VAH_0;
        const float VAL = obs.frozen.VAL_0;
        const float VA_mid = obs.frozen.VA_mid_0;
        const int N = obs.N;

        // ---------------------------------------------------------------------
        // Step 1: Check for same-bar collision (per §2.3)
        // If P_hi >= VAH AND P_lo <= VAL, this is a spike
        // Neither O1 nor O2 can complete from this bar
        // ---------------------------------------------------------------------
        const bool sameBarCollision = (P_hi >= VAH && P_lo <= VAL);

        // ---------------------------------------------------------------------
        // Step 2: Track boundary touches (for O3/O4)
        // ---------------------------------------------------------------------
        const bool touchesVAH = (P_hi >= VAH);
        const bool touchesVAL = (P_lo <= VAL);

        if (touchesVAH && !obs.touchedVAH) {
            obs.touchedVAH = true;
            if (obs.firstTouchBar < 0) {
                obs.firstTouchBar = barIndex;
                obs.firstTouchWasHigh = true;
            }
        }
        if (touchesVAL && !obs.touchedVAL) {
            obs.touchedVAL = true;
            if (obs.firstTouchBar < 0) {
                obs.firstTouchBar = barIndex;
                obs.firstTouchWasHigh = false;
            }
        }

        // ---------------------------------------------------------------------
        // Step 3: Process UP breakout attempt (O1)
        // Trigger: P_hi >= VAH_0
        // Hold: P_lo >= VAH_0 for N consecutive bars after trigger
        // ---------------------------------------------------------------------
        if (!sameBarCollision) {
            if (!obs.upBreakout.IsActive()) {
                // Check for new trigger
                if (P_hi >= VAH) {
                    obs.upBreakout.t_brk = barIndex;
                    obs.upBreakout.holdBarsRemaining = N;
                    obs.upBreakout.isUpBreakout = true;
                }
            } else {
                // Active attempt - check hold condition
                // Hold bar: P_lo must be >= VAH
                if (P_lo >= VAH) {
                    obs.upBreakout.holdBarsRemaining--;
                    if (obs.upBreakout.holdBarsRemaining <= 0) {
                        // O1 COMPLETED
                        obs.outcome = BehaviorOutcome::O1_CONTINUATION_UP;
                        obs.completionBar = barIndex;
                        return true;
                    }
                } else {
                    // Hold failed - reset attempt
                    obs.upBreakout.Reset();

                    // Check if this bar is a new trigger
                    if (P_hi >= VAH) {
                        obs.upBreakout.t_brk = barIndex;
                        obs.upBreakout.holdBarsRemaining = N;
                        obs.upBreakout.isUpBreakout = true;
                    }
                }
            }
        } else {
            // Same-bar collision - cannot be a valid breakout trigger
            // Reset any active attempt
            obs.upBreakout.Reset();
        }

        // ---------------------------------------------------------------------
        // Step 4: Process DN breakout attempt (O2)
        // Trigger: P_lo <= VAL_0
        // Hold: P_hi <= VAL_0 for N consecutive bars after trigger
        // ---------------------------------------------------------------------
        if (!sameBarCollision) {
            if (!obs.dnBreakout.IsActive()) {
                // Check for new trigger
                if (P_lo <= VAL) {
                    obs.dnBreakout.t_brk = barIndex;
                    obs.dnBreakout.holdBarsRemaining = N;
                    obs.dnBreakout.isUpBreakout = false;
                }
            } else {
                // Active attempt - check hold condition
                // Hold bar: P_hi must be <= VAL
                if (P_hi <= VAL) {
                    obs.dnBreakout.holdBarsRemaining--;
                    if (obs.dnBreakout.holdBarsRemaining <= 0) {
                        // O2 COMPLETED
                        obs.outcome = BehaviorOutcome::O2_CONTINUATION_DN;
                        obs.completionBar = barIndex;
                        return true;
                    }
                } else {
                    // Hold failed - reset attempt
                    obs.dnBreakout.Reset();

                    // Check if this bar is a new trigger
                    if (P_lo <= VAL) {
                        obs.dnBreakout.t_brk = barIndex;
                        obs.dnBreakout.holdBarsRemaining = N;
                        obs.dnBreakout.isUpBreakout = false;
                    }
                }
            }
        } else {
            // Same-bar collision - cannot be a valid breakout trigger
            obs.dnBreakout.Reset();
        }

        // ---------------------------------------------------------------------
        // Step 5: Check for mean-reversion completion (O3/O4)
        // Requires prior touch + return to VA_mid ± tolerance
        // ---------------------------------------------------------------------
        const bool atVAMid = (std::abs(P_t - VA_mid) <= obs.tolerance);

        if (atVAMid) {
            // Check O3: touched VAH, returned to center
            if (obs.touchedVAH && !obs.touchedVAL) {
                obs.outcome = BehaviorOutcome::O3_MEAN_REVERT_HIGH;
                obs.completionBar = barIndex;
                return true;
            }
            // Check O4: touched VAL, returned to center
            if (obs.touchedVAL && !obs.touchedVAH) {
                obs.outcome = BehaviorOutcome::O4_MEAN_REVERT_LOW;
                obs.completionBar = barIndex;
                return true;
            }
            // Both touched - use first touch to determine (per §2.3 tolerance collision)
            if (obs.touchedVAH && obs.touchedVAL) {
                if (obs.firstTouchWasHigh) {
                    obs.outcome = BehaviorOutcome::O3_MEAN_REVERT_HIGH;
                } else {
                    obs.outcome = BehaviorOutcome::O4_MEAN_REVERT_LOW;
                }
                obs.completionBar = barIndex;
                return true;
            }
        }

        return false;
    }

    // -------------------------------------------------------------------------
    // Finalize observation at session end
    // Per specification §2.3: session ends unresolved rules
    // -------------------------------------------------------------------------
    static void FinalizeSession(BehaviorObservation& obs, int sessionEndBar) {
        if (obs.IsComplete()) {
            return;
        }

        obs.frozen.t_end = sessionEndBar;
        obs.sessionEnded = true;

        // Check for incomplete breakout attempts (§2.3: session ends before hold)
        const bool incompleteUpBreakout = obs.upBreakout.IsActive();
        const bool incompleteDnBreakout = obs.dnBreakout.IsActive();

        // Check for touch without return (§2.3: session ends after touch but before return)
        const bool touchedButNoReturn = (obs.touchedVAH || obs.touchedVAL);

        if (incompleteUpBreakout || incompleteDnBreakout) {
            // Breakout in progress but hold not complete → UNRESOLVED
            obs.outcome = BehaviorOutcome::UNRESOLVED;
        } else if (touchedButNoReturn) {
            // Touched boundary but never returned → UNRESOLVED
            obs.outcome = BehaviorOutcome::UNRESOLVED;
        } else {
            // No touches, no breakouts → O5 (trivially range-bound)
            obs.outcome = BehaviorOutcome::O5_RANGE_BOUND;
        }

        obs.completionBar = sessionEndBar;
    }
};

// ============================================================================
// HYPOTHESIS MAPPER
// Maps ProfileShape → HypothesisType per specification §3.1
// ============================================================================

struct HypothesisMapping {
    HypothesisType hypothesis = HypothesisType::NONE;
    float targetPrice = 0.0f;           // Frozen target reference
    float invalidationPrice = 0.0f;     // Frozen invalidation reference
    bool requiresTrendDirection = false; // For THIN_VERTICAL
    const char* reason = "";
};

class HypothesisMapper {
public:
    // -------------------------------------------------------------------------
    // Map shape to hypothesis
    // trendDirection: 1 = UP, -1 = DOWN, 0 = not supplied (for THIN_VERTICAL)
    // -------------------------------------------------------------------------
    static HypothesisMapping MapShapeToHypothesis(
        ProfileShape shape,
        float asymmetry,
        const FrozenReferences& frozen,
        int trendDirection = 0)
    {
        HypothesisMapping m;

        if (!frozen.IsValid()) {
            m.reason = "Invalid frozen references";
            return m;
        }

        const float VAH = frozen.VAH_0;
        const float VAL = frozen.VAL_0;
        const float VA_mid = frozen.VA_mid_0;
        const float POC = frozen.POC_0;
        const float W_va = frozen.W_va;

        switch (shape) {
            case ProfileShape::NORMAL_DISTRIBUTION:
                m.hypothesis = HypothesisType::MEAN_REVERSION;
                m.targetPrice = VA_mid;
                m.invalidationPrice = VAH + 0.5f * W_va; // or VAL - 0.5*W_va (both)
                m.reason = "NORMAL: Mean-reversion to VA_mid";
                break;

            case ProfileShape::D_SHAPED:
                if (asymmetry > 0) {
                    m.hypothesis = HypothesisType::MEAN_REVERSION_HIGH;
                    m.targetPrice = VA_mid;
                    m.invalidationPrice = VAH + 0.5f * W_va;
                    m.reason = "D_SHAPED(a>0): MR from high";
                } else {
                    m.hypothesis = HypothesisType::MEAN_REVERSION_LOW;
                    m.targetPrice = VA_mid;
                    m.invalidationPrice = VAL - 0.5f * W_va;
                    m.reason = "D_SHAPED(a<0): MR from low";
                }
                break;

            case ProfileShape::BALANCED:
                m.hypothesis = HypothesisType::RANGE_BOUND;
                m.targetPrice = VA_mid;
                m.invalidationPrice = VAH; // or VAL - check both
                m.reason = "BALANCED: Range-bound in VA";
                break;

            case ProfileShape::P_SHAPED:
                m.hypothesis = HypothesisType::CONTINUATION_UP;
                m.targetPrice = VAH;
                m.invalidationPrice = POC;
                m.reason = "P_SHAPED: Continuation up";
                break;

            case ProfileShape::B_SHAPED:
                m.hypothesis = HypothesisType::CONTINUATION_DN;
                m.targetPrice = VAL;
                m.invalidationPrice = POC;
                m.reason = "B_SHAPED: Continuation down";
                break;

            case ProfileShape::THIN_VERTICAL:
                m.requiresTrendDirection = true;
                if (trendDirection > 0) {
                    m.hypothesis = HypothesisType::CONTINUATION_UP;
                    m.targetPrice = VAH;
                    m.invalidationPrice = POC;
                    m.reason = "THIN_VERTICAL(UP): Continuation up";
                } else if (trendDirection < 0) {
                    m.hypothesis = HypothesisType::CONTINUATION_DN;
                    m.targetPrice = VAL;
                    m.invalidationPrice = POC;
                    m.reason = "THIN_VERTICAL(DN): Continuation down";
                } else {
                    m.hypothesis = HypothesisType::NONE;
                    m.reason = "THIN_VERTICAL: No trend direction supplied";
                }
                break;

            case ProfileShape::DOUBLE_DISTRIBUTION:
                // Requires cluster analysis - simplified to NONE for now
                // Full implementation would use hvnClusters from ProfileFeatures
                m.hypothesis = HypothesisType::NONE;
                m.reason = "DOUBLE_DIST: Requires cluster analysis";
                break;

            case ProfileShape::UNDEFINED:
            default:
                m.hypothesis = HypothesisType::NONE;
                m.reason = "UNDEFINED: Classifier abstained";
                break;
        }

        return m;
    }

    // -------------------------------------------------------------------------
    // Check if outcome matches hypothesis
    // -------------------------------------------------------------------------
    static bool OutcomeMatchesHypothesis(BehaviorOutcome outcome, HypothesisType hypothesis) {
        switch (hypothesis) {
            case HypothesisType::CONTINUATION_UP:
                return outcome == BehaviorOutcome::O1_CONTINUATION_UP;

            case HypothesisType::CONTINUATION_DN:
                return outcome == BehaviorOutcome::O2_CONTINUATION_DN;

            case HypothesisType::MEAN_REVERSION:
                return outcome == BehaviorOutcome::O3_MEAN_REVERT_HIGH ||
                       outcome == BehaviorOutcome::O4_MEAN_REVERT_LOW;

            case HypothesisType::MEAN_REVERSION_HIGH:
                return outcome == BehaviorOutcome::O3_MEAN_REVERT_HIGH;

            case HypothesisType::MEAN_REVERSION_LOW:
                return outcome == BehaviorOutcome::O4_MEAN_REVERT_LOW;

            case HypothesisType::RANGE_BOUND:
                return outcome == BehaviorOutcome::O5_RANGE_BOUND;

            case HypothesisType::NONE:
            default:
                return false; // No hypothesis to match
        }
    }
};

// ============================================================================
// BEHAVIOR SESSION MANAGER
// Orchestrates freeze → observation → outcome flow
// ============================================================================

class BehaviorSessionManager {
public:
    // State
    BehaviorObservation observation;
    HypothesisMapping hypothesis;
    bool frozen = false;

    // -------------------------------------------------------------------------
    // Freeze references at classification time
    // -------------------------------------------------------------------------
    void Freeze(
        int barIndex,
        float POC, float VAH, float VAL,
        float profileHigh, float profileLow,
        ProfileShape shape,
        float asymmetry,
        int holdBars = 3,
        float toleranceRatio = 0.25f)
    {
        FrozenReferences refs;
        refs.POC_0 = POC;
        refs.VAH_0 = VAH;
        refs.VAL_0 = VAL;
        refs.R_0 = profileHigh - profileLow;
        refs.t_freeze = barIndex;
        refs.shape = shape;
        refs.asymmetry = asymmetry;
        refs.ComputeDerived();
        refs.valid = (refs.W_va > 0.0f);

        observation.Initialize(refs, holdBars, toleranceRatio);
        hypothesis = HypothesisMapper::MapShapeToHypothesis(shape, asymmetry, refs);
        frozen = true;
    }

    // -------------------------------------------------------------------------
    // Process bar during observation window
    // -------------------------------------------------------------------------
    bool ProcessBar(int barIndex, float P_hi, float P_lo, float P_t) {
        if (!frozen) return false;
        return OutcomeDetector::ProcessBar(observation, barIndex, P_hi, P_lo, P_t);
    }

    // -------------------------------------------------------------------------
    // Finalize at session end
    // -------------------------------------------------------------------------
    void FinalizeSession(int sessionEndBar) {
        if (!frozen) return;
        OutcomeDetector::FinalizeSession(observation, sessionEndBar);
    }

    // -------------------------------------------------------------------------
    // Check if hypothesis was correct
    // -------------------------------------------------------------------------
    bool WasHypothesisCorrect() const {
        if (!frozen || !observation.IsComplete()) return false;
        return HypothesisMapper::OutcomeMatchesHypothesis(observation.outcome, hypothesis.hypothesis);
    }

    // -------------------------------------------------------------------------
    // Reset for new session
    // -------------------------------------------------------------------------
    void Reset() {
        observation = BehaviorObservation();
        hypothesis = HypothesisMapping();
        frozen = false;
    }
};

// ============================================================================
// BEHAVIOR HISTORY TRACKER
// Accumulates per-shape hit rates across sessions for confidence multiplier
// ============================================================================

struct ShapeHistoryEntry {
    int attempts = 0;      // Sessions where this shape was frozen
    int matches = 0;       // Sessions where outcome matched hypothesis

    float GetHitRate() const {
        return (attempts > 0) ? static_cast<float>(matches) / attempts : 0.0f;
    }
};

class BehaviorHistoryTracker {
public:
    static constexpr int MIN_SAMPLES = 10;           // Minimum sessions before applying multiplier
    static constexpr float BASE_MULTIPLIER = 1.0f;   // Default when insufficient data
    static constexpr float MIN_MULTIPLIER = 0.8f;    // Floor (0% hit rate)
    static constexpr float MAX_MULTIPLIER = 1.2f;    // Ceiling (100% hit rate)

    // Per-shape history (indexed by ProfileShape enum)
    ShapeHistoryEntry history[static_cast<int>(ProfileShape::COUNT)];

    // -------------------------------------------------------------------------
    // Record session result
    // -------------------------------------------------------------------------
    void RecordSession(ProfileShape shape, bool hypothesisMatched) {
        const int idx = static_cast<int>(shape);
        if (idx < 0 || idx >= static_cast<int>(ProfileShape::COUNT)) return;

        history[idx].attempts++;
        if (hypothesisMatched) {
            history[idx].matches++;
        }
    }

    // -------------------------------------------------------------------------
    // Get confidence multiplier for a shape
    // Returns BASE_MULTIPLIER if insufficient samples
    // -------------------------------------------------------------------------
    float GetConfidenceMultiplier(ProfileShape shape) const {
        const int idx = static_cast<int>(shape);
        if (idx < 0 || idx >= static_cast<int>(ProfileShape::COUNT)) {
            return BASE_MULTIPLIER;
        }

        const ShapeHistoryEntry& entry = history[idx];
        if (entry.attempts < MIN_SAMPLES) {
            return BASE_MULTIPLIER;  // Not enough data yet
        }

        // Linear interpolation: hitRate 0% → 0.8, hitRate 100% → 1.2
        const float hitRate = entry.GetHitRate();
        return MIN_MULTIPLIER + hitRate * (MAX_MULTIPLIER - MIN_MULTIPLIER);
    }

    // -------------------------------------------------------------------------
    // Get statistics for logging
    // -------------------------------------------------------------------------
    void GetStats(ProfileShape shape, int& outAttempts, int& outMatches, float& outHitRate) const {
        const int idx = static_cast<int>(shape);
        if (idx < 0 || idx >= static_cast<int>(ProfileShape::COUNT)) {
            outAttempts = outMatches = 0;
            outHitRate = 0.0f;
            return;
        }

        outAttempts = history[idx].attempts;
        outMatches = history[idx].matches;
        outHitRate = history[idx].GetHitRate();
    }

    // -------------------------------------------------------------------------
    // Reset all history (chart full recalc)
    // -------------------------------------------------------------------------
    void Reset() {
        for (int i = 0; i < static_cast<int>(ProfileShape::COUNT); ++i) {
            history[i] = ShapeHistoryEntry();
        }
    }
};

} // namespace AMT

#endif // AMT_BEHAVIORMAPPING_H
