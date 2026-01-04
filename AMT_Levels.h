// ============================================================================
// AMT_Levels.h
// Versioned profile levels (SSOT for POC/VAH/VAL)
// Implements three-state semantics: current, stable, previous
// ============================================================================

#ifndef AMT_LEVELS_H
#define AMT_LEVELS_H

#include "AMT_config.h"
#include <cmath>

namespace AMT {

// ============================================================================
// PROFILE LEVELS (Tick-based SSOT)
// All level storage uses ticks as the authoritative representation.
// Prices are derived at the edge when needed for display/logging.
// ============================================================================

/**
 * ProfileLevelsTicks: POC/VAH/VAL in tick format (SSOT)
 *
 * Invariant: VAL <= POC <= VAH (when valid)
 */
struct ProfileLevelsTicks {
    long long pocTicks = 0;
    long long vahTicks = 0;
    long long valTicks = 0;

    bool IsValid() const {
        return pocTicks > 0 && vahTicks > 0 && valTicks > 0;
    }

    bool IsEmpty() const {
        return pocTicks == 0 && vahTicks == 0 && valTicks == 0;
    }

    /**
     * Check invariant: VAL <= POC <= VAH
     * Returns true if invariant holds (or if empty/invalid)
     */
    bool CheckInvariant() const {
        if (IsEmpty()) return true;
        return valTicks <= pocTicks && pocTicks <= vahTicks;
    }

    /**
     * Get prices (derived from ticks)
     */
    double GetPOC(double tickSize) const { return TicksToPrice(pocTicks, tickSize); }
    double GetVAH(double tickSize) const { return TicksToPrice(vahTicks, tickSize); }
    double GetVAL(double tickSize) const { return TicksToPrice(valTicks, tickSize); }

    /**
     * Set from prices (converts to ticks using canonical function)
     */
    void SetFromPrices(double poc, double vah, double val, double tickSize) {
        pocTicks = PriceToTicks(poc, tickSize);
        vahTicks = PriceToTicks(vah, tickSize);
        valTicks = PriceToTicks(val, tickSize);
    }

    /**
     * Check if levels differ by at least minDriftTicks
     */
    bool HasDrifted(const ProfileLevelsTicks& other, int minDriftTicks = 1) const {
        return std::abs(pocTicks - other.pocTicks) >= minDriftTicks ||
               std::abs(vahTicks - other.vahTicks) >= minDriftTicks ||
               std::abs(valTicks - other.valTicks) >= minDriftTicks;
    }

    /**
     * Get VA range in ticks
     */
    long long GetVARangeTicks() const {
        if (!IsValid()) return 0;
        return vahTicks - valTicks;
    }

    void Reset() {
        pocTicks = 0;
        vahTicks = 0;
        valTicks = 0;
    }

    bool operator==(const ProfileLevelsTicks& other) const {
        return pocTicks == other.pocTicks &&
               vahTicks == other.vahTicks &&
               valTicks == other.valTicks;
    }

    bool operator!=(const ProfileLevelsTicks& other) const {
        return !(*this == other);
    }
};

// ============================================================================
// VERSIONED LEVELS (Three-state semantics)
// ============================================================================

/**
 * VersionedLevels: Three-state level management
 *
 * States:
 *   - current:  Latest computed levels (can drift intraday)
 *   - stable:   Last "accepted stable" levels (post-stability confirmation)
 *   - previous: Last stable levels from prior session (or prior promotion epoch)
 *
 * Promotion Triggers:
 *   - PromoteToStable(): Called when CheckStability() succeeds (intra-session debounce)
 *   - PromoteToPrevious(): Called on session boundary (RTH->Globex or Globex->RTH)
 *
 * Key Invariant:
 *   Within a session, 'previous' does NOT change. Only 'stable' changes on stability.
 *   On session boundary, 'stable' becomes 'previous', then 'stable' is reset.
 */
struct VersionedLevels {
    ProfileLevelsTicks current;
    ProfileLevelsTicks stable;
    ProfileLevelsTicks previous;

    // Stability tracking (for intra-session debounce)
    int barsAtCurrentPoc = 0;
    int stabilityThresholdBars = 3;  // Bars POC must be stable before promotion

    /**
     * Update current levels from VBP computation
     * Call this every bar after VBP values are computed
     * @param newLevels New levels from VBP
     * @param tickSize Tick size for drift comparison
     * @return true if current changed significantly (>= 1 tick)
     */
    bool UpdateCurrent(const ProfileLevelsTicks& newLevels, double tickSize) {
        (void)tickSize;  // Used for future logging if needed

        bool changed = (current != newLevels);

        // Track POC stability
        if (newLevels.pocTicks == current.pocTicks) {
            barsAtCurrentPoc++;
        } else {
            barsAtCurrentPoc = 1;  // Reset on any POC change
        }

        current = newLevels;
        return changed;
    }

    /**
     * Check if current levels are stable enough to promote
     * @return true if POC has been stable for stabilityThresholdBars
     */
    bool IsStable() const {
        return barsAtCurrentPoc >= stabilityThresholdBars && current.IsValid();
    }

    /**
     * Promote current -> stable (intra-session stability confirmation)
     * Call this when CheckStability() succeeds
     * @return true if stable was actually updated (differs from previous stable)
     */
    bool PromoteToStable() {
        if (!current.IsValid()) return false;

        bool changed = (stable != current);
        stable = current;
        return changed;
    }

    /**
     * Promote stable -> previous (session boundary)
     * Call this on session transition (RTH->Globex or Globex->RTH)
     */
    void PromoteToPrevious() {
        if (stable.IsValid()) {
            previous = stable;
        }
        // Note: We don't reset stable here - let next session populate it
        barsAtCurrentPoc = 0;
    }

    /**
     * Full reset (chart reset, symbol change, etc.)
     */
    void Reset() {
        current.Reset();
        stable.Reset();
        previous.Reset();
        barsAtCurrentPoc = 0;
    }

    /**
     * Session reset (preserves previous, clears current/stable)
     * Call on session start after PromoteToPrevious()
     */
    void ResetForNewSession() {
        current.Reset();
        stable.Reset();
        barsAtCurrentPoc = 0;
        // previous is preserved
    }

    /**
     * Get the best available levels for decision-making
     * Priority: stable > current > previous
     */
    const ProfileLevelsTicks& GetBestLevels() const {
        if (stable.IsValid()) return stable;
        if (current.IsValid()) return current;
        return previous;  // May be empty
    }

    /**
     * Check if stable has drifted from previous (for diagnostics)
     */
    bool HasStableDriftedFromPrevious(int minDriftTicks = 2) const {
        if (!stable.IsValid() || !previous.IsValid()) return false;
        return stable.HasDrifted(previous, minDriftTicks);
    }
};

} // namespace AMT

#endif // AMT_LEVELS_H
