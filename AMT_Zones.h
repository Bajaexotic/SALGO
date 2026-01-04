// ============================================================================
// AMT_Zones.h
// Zone structures, manager, and runtime state
// ============================================================================

#ifndef AMT_ZONES_H
#define AMT_ZONES_H

#include "amt_core.h"
#include "AMT_config.h"
#include "AMT_Helpers.h"
#include "AMT_Bridge.h"
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

namespace AMT {

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

struct ZoneRuntime;
struct ZoneConfig;
struct SessionContext;
struct TransitionState;
struct ZoneTransitionMemory;
struct DOMCachePolicy;
struct ResolutionPolicy;
struct ZoneContextSnapshot;

// ============================================================================
// ZONE CREATION RESULT (MEDIUM-0)
// Explicit result type for zone creation - prevents silent corruption
// ============================================================================

/**
 * Reason codes for zone creation failures.
 * Used for structured logging and diagnostics.
 */
enum class ZoneCreationFailure : int {
    NONE = 0,                    // Success
    INVALID_ANCHOR_PRICE,        // Anchor price is zero, negative, or NaN
    INVALID_TICK_SIZE,           // Tick size in config is invalid
    INVALID_ZONE_TYPE,           // Zone type is UNKNOWN or invalid
    DUPLICATE_ANCHOR,            // Zone already exists at this anchor price
    MAX_ZONES_EXCEEDED,          // Would exceed maximum zone count
    INVALID_TIME,                // Creation time is invalid
    POSTURE_DISALLOWED,          // Zone type disallowed by current posture (defense-in-depth)
    INTERNAL_ERROR,              // Unexpected error (should not happen)

    // SENTINEL: Must be last - used to size arrays
    _COUNT
};

// Compile-time constant for array sizing (avoids magic numbers)
constexpr int kZoneCreationFailureCount = static_cast<int>(ZoneCreationFailure::_COUNT);

/**
 * Result of zone creation attempt.
 * Call sites MUST check ok before using zoneId.
 */
struct ZoneCreationResult {
    bool ok = false;                               // True if zone was created
    int zoneId = -1;                               // Zone ID if ok, -1 otherwise
    ZoneCreationFailure failure = ZoneCreationFailure::NONE;

    // Convenience accessors
    explicit operator bool() const { return ok; }

    static ZoneCreationResult Success(int id) {
        return {true, id, ZoneCreationFailure::NONE};
    }

    static ZoneCreationResult Failure(ZoneCreationFailure reason) {
        return {false, -1, reason};
    }
};

/**
 * Get string representation of failure reason (for logging).
 */
inline const char* GetZoneCreationFailureString(ZoneCreationFailure failure) {
    switch (failure) {
        case ZoneCreationFailure::NONE: return "NONE";
        case ZoneCreationFailure::INVALID_ANCHOR_PRICE: return "INVALID_ANCHOR_PRICE";
        case ZoneCreationFailure::INVALID_TICK_SIZE: return "INVALID_TICK_SIZE";
        case ZoneCreationFailure::INVALID_ZONE_TYPE: return "INVALID_ZONE_TYPE";
        case ZoneCreationFailure::DUPLICATE_ANCHOR: return "DUPLICATE_ANCHOR";
        case ZoneCreationFailure::MAX_ZONES_EXCEEDED: return "MAX_ZONES_EXCEEDED";
        case ZoneCreationFailure::INVALID_TIME: return "INVALID_TIME";
        case ZoneCreationFailure::INTERNAL_ERROR: return "INTERNAL_ERROR";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// HISTORY BUFFER LIMITS
// Prevents unbounded memory growth in long sessions
// ============================================================================
constexpr int MAX_TOUCH_HISTORY = 50;       // Max touches per zone
constexpr int MAX_ENGAGEMENT_HISTORY = 50;  // Max engagements per zone

// ============================================================================
// DOM CACHE POLICY (D)
// Bar-based cache for DOM liquidity data
// ============================================================================

/**
 * DOM Cache Policy - Bar-based DOM liquidity caching.
 *
 * PURPOSE: Cache DOM (depth-of-market) data once per bar to avoid
 * redundant platform calls during the update cycle.
 *
 * CALLER CONTRACT (mandatory for non-dead code):
 * ===============================================
 * The study (caller) MUST implement this integration pattern:
 *
 *   1. On each bar, check: domCachePolicy.NeedsRefresh(currentBar)
 *   2. If true, fetch DOM data from platform (e.g., sc.GetBidAskMarketDepthData)
 *   3. Call: domCachePolicy.UpdateCache(bar, totalLiq, bidLiq, askLiq)
 *   4. Use cached liquidity for dynamic zone width calculation
 *
 * If this contract is NOT implemented, DOMCachePolicy is dead code
 * and should be deleted. There is no "not yet wired" state allowed.
 *
 * CACHE SEMANTICS:
 * - cachedAtBar: Bar index when cache was last refreshed
 * - NeedsRefresh(bar): Returns true if bar differs from cachedAtBar
 * - NeedsWidthRefresh(anchorTicks): Returns true if anchor moved >= 1 tick
 *
 * LIFETIME: Per-session. Call Reset() on session roll.
 */
struct DOMCachePolicy {
    // Cache validity
    int cachedAtBar = -1;
    double cachedTotalLiquidity = 0.0;
    double cachedBidLiquidity = 0.0;
    double cachedAskLiquidity = 0.0;

    // For intrabar mode (Mode 2) - optional future use
    SCDateTime cachedAtTime;
    int refreshIntervalSeconds = 5;  // Only used in Mode 2

    // Zone-specific cached widths (SSOT: anchorTicks is authoritative)
    long long cachedAnchorTicks = 0;
    int cachedCoreTicks = 0;
    int cachedHaloTicks = 0;

    /**
     * Check if cache needs refresh (BAR-BASED MODE)
     * Returns true if current bar differs from cached bar
     */
    bool NeedsRefresh(int currentBar) const {
        return cachedAtBar != currentBar;
    }

    /**
     * Check if width cache needs refresh (tick-based - SSOT)
     * Invalidated when anchor changes by >= 1 tick
     * @param anchorTicks Current anchor in ticks
     */
    bool NeedsWidthRefresh(long long anchorTicks) const {
        if (cachedAnchorTicks == 0) return true;
        return std::abs(anchorTicks - cachedAnchorTicks) >= 1;
    }

    /**
     * Update cache with new values
     */
    void UpdateCache(int bar, double totalLiq, double bidLiq, double askLiq) {
        cachedAtBar = bar;
        cachedTotalLiquidity = totalLiq;
        cachedBidLiquidity = bidLiq;
        cachedAskLiquidity = askLiq;
    }

    /**
     * Update width cache (tick-based - SSOT)
     * @param anchorTicks Anchor in ticks (authoritative)
     */
    void UpdateWidthCache(long long anchorTicks, int coreTicks, int haloTicks) {
        cachedAnchorTicks = anchorTicks;
        cachedCoreTicks = coreTicks;
        cachedHaloTicks = haloTicks;
    }

    void Reset() {
        cachedAtBar = -1;
        cachedTotalLiquidity = 0.0;
        cachedBidLiquidity = 0.0;
        cachedAskLiquidity = 0.0;
        cachedAtTime = SCDateTime();
        cachedAnchorTicks = 0;
        cachedCoreTicks = 0;
        cachedHaloTicks = 0;
    }
};

// ============================================================================
// RESOLUTION POLICY
// Combines bar count AND time for zone resolution (Issue 5 fix)
// ============================================================================

// ============================================================================
// RESOLUTION POLICY (MEDIUM-2)
// SSOT for zone resolution decisions - unified bars+time evaluation
// ============================================================================

/**
 * Resolution policy mode - determines which thresholds to evaluate.
 */
enum class ResolutionMode : int {
    BARS_ONLY = 0,      // Legacy mode: only bar count matters
    TIME_ONLY = 1,      // Only elapsed time matters
    BARS_OR_TIME = 2    // Default: resolve if EITHER threshold met (AMT behavior)
};

/**
 * Resolution reason codes - deterministic for debugging and tests.
 */
enum class ResolutionReason : int {
    NOT_RESOLVED = 0,       // Neither threshold met
    RESOLVED_BY_BARS,       // Bar threshold triggered
    RESOLVED_BY_TIME,       // Time threshold triggered
    RESOLVED_BY_BOTH        // Both thresholds met (for diagnostics)
};

/**
 * Get string representation of resolution reason.
 */
inline const char* GetResolutionReasonString(ResolutionReason reason) {
    switch (reason) {
        case ResolutionReason::NOT_RESOLVED: return "NOT_RESOLVED";
        case ResolutionReason::RESOLVED_BY_BARS: return "RESOLVED_BY_BARS";
        case ResolutionReason::RESOLVED_BY_TIME: return "RESOLVED_BY_TIME";
        case ResolutionReason::RESOLVED_BY_BOTH: return "RESOLVED_BY_BOTH";
        default: return "UNKNOWN";
    }
}

/**
 * Resolution result with reason code.
 */
struct ResolutionResult {
    bool resolved = false;
    ResolutionReason reason = ResolutionReason::NOT_RESOLVED;

    explicit operator bool() const { return resolved; }
};

/**
 * SSOT Resolution Policy (MEDIUM-2)
 *
 * Single evaluator for all resolution decisions.
 * Supports legacy bars-only mode and AMT bars+time mode through PolicyMode.
 *
 * Resolution uses EITHER bar count OR time threshold, whichever triggers first.
 * This prevents the issue where 2 bars could mean seconds or hours depending on TF.
 *
 * ANCHOR CONTRACT (Hardening 3):
 * ==============================
 * - `barsOutside`: Number of bars since price last touched zone halo.
 *   Measured from the FIRST bar that exited halo, not from zone creation.
 *   Caller must pass a monotonically increasing counter that resets when
 *   price re-enters halo.
 *
 * - `secondsOutside`: Seconds since price last touched zone halo.
 *   Measured from the TIMESTAMP of the first bar that exited halo.
 *   This allows dead-tape detection: bars may not advance but time does.
 *   Caller must compute: currentTime - lastHaloTouchTime.
 *
 * INVARIANTS:
 * - barsOutside >= 0 (cannot be negative)
 * - secondsOutside >= 0 (cannot be negative)
 * - If barsOutside == 0 and secondsOutside > 0: "dead tape" scenario
 *   (no new bars but time has elapsed)
 */
struct ResolutionPolicy {
    // Thresholds (configurable)
    int barsOutsideThreshold = 2;       // Default: 2 bars outside halo
    int secondsOutsideThreshold = 30;   // Default: 30 seconds outside halo

    // Mode (configurable)
    ResolutionMode mode = ResolutionMode::BARS_OR_TIME;

    /**
     * SSOT: Evaluate resolution with full result (MEDIUM-2)
     * This is the ONLY place resolution decisions should be computed.
     *
     * @param barsOutside Bars since last halo touch (must be >= 0)
     * @param secondsOutside Seconds since last halo touch (must be >= 0)
     * @return ResolutionResult with resolved flag and reason code
     */
    ResolutionResult Evaluate(int barsOutside, int secondsOutside) const {
        // Anchor contract enforcement (debug builds only)
        assert(barsOutside >= 0 && "barsOutside must be >= 0 (measured from first exit bar)");
        assert(secondsOutside >= 0 && "secondsOutside must be >= 0 (measured from halo exit time)");

        const bool barsMet = (barsOutside >= barsOutsideThreshold);
        const bool timeMet = (secondsOutside >= secondsOutsideThreshold);

        ResolutionResult result;

        switch (mode) {
            case ResolutionMode::BARS_ONLY:
                if (barsMet) {
                    result.resolved = true;
                    result.reason = ResolutionReason::RESOLVED_BY_BARS;
                }
                break;

            case ResolutionMode::TIME_ONLY:
                if (timeMet) {
                    result.resolved = true;
                    result.reason = ResolutionReason::RESOLVED_BY_TIME;
                }
                break;

            case ResolutionMode::BARS_OR_TIME:
            default:
                if (barsMet && timeMet) {
                    result.resolved = true;
                    result.reason = ResolutionReason::RESOLVED_BY_BOTH;
                } else if (barsMet) {
                    result.resolved = true;
                    result.reason = ResolutionReason::RESOLVED_BY_BARS;
                } else if (timeMet) {
                    result.resolved = true;
                    result.reason = ResolutionReason::RESOLVED_BY_TIME;
                }
                break;
        }

        return result;
    }

    /**
     * Legacy-compatible: Check if zone engagement should be resolved.
     * Delegates to Evaluate() for SSOT.
     */
    bool ShouldResolve(int barsOutside, int secondsOutside) const {
        return Evaluate(barsOutside, secondsOutside).resolved;
    }

    /**
     * Legacy-compatible: Get resolution reason string for logging.
     * Delegates to Evaluate() for SSOT.
     */
    const char* GetResolutionReason(int barsOutside, int secondsOutside) const {
        ResolutionReason reason = Evaluate(barsOutside, secondsOutside).reason;
        switch (reason) {
            case ResolutionReason::RESOLVED_BY_BARS: return "BARS";
            case ResolutionReason::RESOLVED_BY_TIME: return "TIME";
            case ResolutionReason::RESOLVED_BY_BOTH: return "BOTH";
            default: return "NONE";
        }
    }

    /**
     * Configure for legacy bars-only behavior.
     */
    void SetBarsOnlyMode(int barsThreshold) {
        mode = ResolutionMode::BARS_ONLY;
        barsOutsideThreshold = barsThreshold;
        secondsOutsideThreshold = INT_MAX;  // Effectively disabled
    }

    /**
     * Configure for AMT bars+time behavior.
     */
    void SetBarsOrTimeMode(int barsThreshold, int secondsThreshold) {
        mode = ResolutionMode::BARS_OR_TIME;
        barsOutsideThreshold = barsThreshold;
        secondsOutsideThreshold = secondsThreshold;
    }
};

// ============================================================================
// TRANSITION STATE (A1)
// Per-chart persistent state for zone transition tracking
// NO STATIC LOCALS - passed by reference to update functions
// ============================================================================

struct TransitionState {
    // Last dominant zone state
    ZoneProximity lastDominantProximity = ZoneProximity::INACTIVE;
    int lastPrimaryZoneId = -1;

    // Engagement timing
    SCDateTime lastEngagementStart;
    int lastEngagementBar = -1;

    // Transition flags (set on each update, consumed by caller)
    bool justEnteredZone = false;
    bool justExitedZone = false;
    bool justChangedZone = false;  // Changed from one zone to another

    // Last update tracking
    int lastUpdateBar = -1;

    /**
     * Reset transition flags (call at start of each update)
     */
    void ResetTransitionFlags() {
        justEnteredZone = false;
        justExitedZone = false;
        justChangedZone = false;
    }

    /**
     * Process a new dominant proximity state
     * Updates transition flags based on state change
     */
    void ProcessTransition(ZoneProximity newProximity, int newZoneId,
                          int currentBar, SCDateTime currentTime) {
        ResetTransitionFlags();

        // Detect entry: was not at zone, now at zone
        if (lastDominantProximity != ZoneProximity::AT_ZONE &&
            newProximity == ZoneProximity::AT_ZONE) {
            justEnteredZone = true;
            lastEngagementStart = currentTime;
            lastEngagementBar = currentBar;
        }

        // Detect exit: was at zone, now not at zone
        if (lastDominantProximity == ZoneProximity::AT_ZONE &&
            newProximity != ZoneProximity::AT_ZONE) {
            justExitedZone = true;
        }

        // Detect zone change: same proximity but different zone
        if (newZoneId != lastPrimaryZoneId && lastPrimaryZoneId != -1) {
            justChangedZone = true;

            // If changing while at zone, treat as exit + entry
            if (lastDominantProximity == ZoneProximity::AT_ZONE &&
                newProximity == ZoneProximity::AT_ZONE) {
                justExitedZone = true;
                justEnteredZone = true;
                lastEngagementStart = currentTime;
                lastEngagementBar = currentBar;
            }
        }

        // Update state
        lastDominantProximity = newProximity;
        lastPrimaryZoneId = newZoneId;
        lastUpdateBar = currentBar;
    }

    /**
     * Get engagement duration in seconds (since last entry)
     */
    int GetEngagementSeconds(SCDateTime currentTime) const {
        if (lastDominantProximity != ZoneProximity::AT_ZONE) return 0;
        if (lastEngagementStart.GetAsDouble() <= 0.0) return 0;
        return GetElapsedSeconds(lastEngagementStart, currentTime);
    }

    /**
     * Get engagement duration in bars
     */
    int GetEngagementBars(int currentBar) const {
        if (lastDominantProximity != ZoneProximity::AT_ZONE) return 0;
        if (lastEngagementBar < 0) return 0;
        return currentBar - lastEngagementBar;
    }

    void Reset() {
        lastDominantProximity = ZoneProximity::INACTIVE;
        lastPrimaryZoneId = -1;
        lastEngagementStart = SCDateTime();
        lastEngagementBar = -1;
        justEnteredZone = false;
        justExitedZone = false;
        justChangedZone = false;
        lastUpdateBar = -1;
    }
};

// ============================================================================
// ZONE TRANSITION MEMORY (E)
// Sticky zone behavior - preferred zone wins for N bars after selection
// ============================================================================

struct ZoneTransitionMemory {
    // Preferred zone (sticky)
    int preferredZoneId = -1;
    int preferredSetAtBar = -1;
    int stickyDurationBars = 5;  // How long preference lasts

    // Hysteresis state
    bool inHysteresis = false;

    /**
     * Set preferred zone (starts sticky period)
     */
    void SetPreferred(int zoneId, int currentBar) {
        preferredZoneId = zoneId;
        preferredSetAtBar = currentBar;
        inHysteresis = true;
    }

    /**
     * Check if preference is still active
     */
    bool IsPreferenceActive(int currentBar) const {
        if (!inHysteresis || preferredZoneId < 0) return false;
        return (currentBar - preferredSetAtBar) < stickyDurationBars;
    }

    /**
     * Get preferred zone if still valid
     * Returns -1 if no valid preference
     */
    int GetPreferredIfValid(int currentBar) const {
        if (!IsPreferenceActive(currentBar)) return -1;
        return preferredZoneId;
    }

    /**
     * Clear preference (expired or zone no longer valid)
     */
    void ClearPreference() {
        preferredZoneId = -1;
        preferredSetAtBar = -1;
        inHysteresis = false;
    }

    /**
     * Update preference state (call each bar)
     * Clears if expired
     */
    void Update(int currentBar) {
        if (inHysteresis && !IsPreferenceActive(currentBar)) {
            ClearPreference();
        }
    }

    void Reset() {
        preferredZoneId = -1;
        preferredSetAtBar = -1;
        inHysteresis = false;
    }
};

// ============================================================================
// ROTATION METRICS
// Tracks higher highs, lower lows for absorption/exhaustion detection
// ============================================================================

struct RotationMetrics {
    int consecutiveHigherHighs = 0;
    int consecutiveHigherLows = 0;
    int consecutiveLowerHighs = 0;
    int consecutiveLowerLows = 0;
    double priceRangeTicks = 0.0;

    // Derived flags (match AuctionIntent enum)
    bool isAbsorption = false;  // Selling into rising price (bullish)
    bool isExhaustion = false;  // Buying into falling price (bearish)

    void Reset() {
        consecutiveHigherHighs = 0;
        consecutiveHigherLows = 0;
        consecutiveLowerHighs = 0;
        consecutiveLowerLows = 0;
        priceRangeTicks = 0.0;
        isAbsorption = false;
        isExhaustion = false;
    }
};

// ============================================================================
// VOLUME CHARACTERISTICS
// Raw volume metrics + computed accessors (SSOT enforcement)
// ============================================================================

struct VolumeCharacteristics {
    // ========================================================================
    // RAW FACTS (authoritative - set by UpdateZoneVolume)
    // ========================================================================
    double volumeRatio = 0.0;        // volume at level / session avg per tick (for logs/diagnostics only)
    double absoluteVolume = 0.0;     // Actual contracts traded
    double cumulativeDelta = 0.0;    // Net buy - sell pressure
    double deltaRatio = 0.0;         // delta / volume (-1.0 to +1.0)
    double bidVolume = 0.0;          // Volume on bid
    double askVolume = 0.0;          // Volume on ask
    int barsAtLevel = 0;             // Bars that traded at this price
    int rankByVolume = 0;            // 1 = POC, 2 = 2nd highest, etc.
    int clusterWidthTicks = 1;       // Width of HVN cluster

    // SSOT classification (orthogonal - set by SSOT classifier)
    VolumeNodeClassification classification;

    // ========================================================================
    // SSOT ACCESSORS (use cached thresholds from VolumeThresholds)
    // ========================================================================

    // Classify using SSOT cached thresholds
    void ClassifyFromThresholds(const VolumeThresholds& thresholds) {
        classification.density = thresholds.ClassifyVolume(absoluteVolume);
        // Single print flag
        if (classification.density == VAPDensityClass::LOW &&
            absoluteVolume > 0 && absoluteVolume <= thresholds.mean * 0.3) {
            classification.flags = classification.flags | NodeFlags::SINGLE_PRINT;
        }
    }

    // SSOT HVN/LVN accessors (delegate to classification)
    // These are the ONLY way to check HVN/LVN status - legacy ratio-based removed
    bool IsHVN_SSOT() const { return classification.IsHVN(); }
    bool IsLVN_SSOT() const { return classification.IsLVN(); }

    // ========================================================================
    // DELTA-BASED ACCESSORS
    // SSOT: Delta checks are now inlined in ClassifyIntent to prevent
    // dual classification paths. Legacy IsSinglePrint/IsBuyingNode/IsSellingNode
    // removed - see Change Set B.
    // ========================================================================

    double GetAggressionRatio() const {
        double total = bidVolume + askVolume;
        return (total > 0.0) ? (askVolume / total) : 0.5;
    }

    bool IsClusteredNode(const ZoneConfig& cfg) const {
        return clusterWidthTicks >= cfg.clusterMinWidth;
    }

    double GetVolumeConviction() const {
        // Composite score: volume × time × delta alignment
        // Note: Use (std::min) to avoid Windows min macro conflict
        double volScore = (std::min)(volumeRatio / 1.5, 2.0);  // Cap at 2.0
        double timeScore = (std::min)(static_cast<double>(barsAtLevel) / 5.0, 2.0);
        double deltaAlign = std::fabs(deltaRatio);
        return volScore * timeScore * (0.5 + 0.5 * deltaAlign);
    }

    // ========================================================================
    // FLOW INTENT CLASSIFICATION (orthogonal to density)
    // SSOT: Delta threshold checks inlined here - no separate accessor methods
    // ========================================================================

    FlowIntent ClassifyIntent(const ZoneConfig& cfg, bool isUpperBoundary, bool isLowerBoundary) const {
        double aggression = GetAggressionRatio();

        // SSOT: Inline delta threshold checks (previously IsBuyingNode/IsSellingNode)
        const bool isBuyingDelta = (deltaRatio >= cfg.buyingNodeThreshold);
        const bool isSellingDelta = (deltaRatio <= cfg.sellingNodeThreshold);

        if (isUpperBoundary) {
            // At upper boundary (VAH/session high)
            if (isSellingDelta && aggression < cfg.aggressionLowThreshold) {
                return FlowIntent::RESPONSIVE;  // Sellers defending
            } else if (isBuyingDelta && aggression > cfg.aggressionHighThreshold) {
                return FlowIntent::INITIATIVE;  // Buyers attacking
            }
        } else if (isLowerBoundary) {
            // At lower boundary (VAL/session low)
            if (isBuyingDelta && aggression > cfg.aggressionHighThreshold) {
                return FlowIntent::RESPONSIVE;  // Buyers defending
            } else if (isSellingDelta && aggression < cfg.aggressionLowThreshold) {
                return FlowIntent::INITIATIVE;  // Sellers attacking
            }
        }
        return FlowIntent::NEUTRAL;
    }

    // Get full orthogonal classification
    VolumeNodeClassification GetOrthogonalClassification(
        const ZoneConfig& cfg,
        bool isUpperBoundary,
        bool isLowerBoundary) const
    {
        VolumeNodeClassification result = classification;
        result.intent = ClassifyIntent(cfg, isUpperBoundary, isLowerBoundary);
        return result;
    }

    // Classify volume node type (uses computed accessors)
    // DEPRECATED: Use GetOrthogonalClassification().ToLegacyType() for new code
    VolumeNodeType GetNodeType(const ZoneConfig& cfg, const ZoneRuntime& zone) const;

    // Reset to defaults
    void Reset() {
        volumeRatio = 0.0;
        absoluteVolume = 0.0;
        cumulativeDelta = 0.0;
        deltaRatio = 0.0;
        bidVolume = 0.0;
        askVolume = 0.0;
        barsAtLevel = 0;
        rankByVolume = 0;
        clusterWidthTicks = 1;
        classification = VolumeNodeClassification();
    }
};

// ============================================================================
// ENGAGEMENT METRICS
// Tracks what happened during a zone engagement
// ============================================================================

struct EngagementMetrics {
    // Time bounds (immutable after finalization)
    SCDateTime startTime;
    SCDateTime endTime;
    int startBar = -1;
    int endBar = -1;

    // Duration
    int barsEngaged = 0;
    int secondsEngaged = 0;

    // Volume/Delta
    double cumulativeVolume = 0.0;
    double cumulativeDelta = 0.0;
    double volumeRatio = 0.0;     // Avg volume vs session average

    // Price action
    int peakPenetrationTicks = 0; // Max distance beyond anchor
    double avgClosePrice = 0.0;

    // Escape velocity (Phase 1A)
    double entryPrice = 0.0;      // Close price when engagement started
    double exitPrice = 0.0;       // Close price when engagement finalized
    double escapeVelocity = 0.0;  // abs(exit-entry) / tickSize / barsEngaged (ticks/bar)

    // Rotation tracking
    RotationMetrics rotation;

    // Outcome (set when engagement ends)
    AuctionOutcome outcome = AuctionOutcome::PENDING;

    // Outcome classification flags
    bool wasHighVolume = false;
    bool wasLowVolume = false;
    bool wasDeltaAligned = false;
    bool wasFailedAuction = false;
    bool wasResponsiveDefense = false;

    // Exactly-once finalization guard (Phase 2 robustness)
    // Set true by Finalize(), cleared by Start()
    // Prevents double-push to baselines if state machine has unexpected re-entry
    bool finalizedThisEngagement = false;

    // Lifecycle methods
    void Start(int bar, SCDateTime time, double currentPrice) {
        startBar = bar;
        startTime = time;
        entryPrice = currentPrice;  // Phase 1A: Record entry price
        barsEngaged = 0;
        secondsEngaged = 0;
        cumulativeVolume = 0.0;
        cumulativeDelta = 0.0;
        peakPenetrationTicks = 0;
        exitPrice = 0.0;
        escapeVelocity = 0.0;
        rotation.Reset();
        outcome = AuctionOutcome::PENDING;
        finalizedThisEngagement = false;  // Clear guard for new engagement
    }

    /**
     * Finalize engagement and compute escape velocity.
     * Sets finalizedThisEngagement flag to prevent double-finalization.
     * @param bar Current bar index
     * @param time Current bar time
     * @param currentPrice Close price at finalization (exitPrice)
     * @param tickSize Tick size for escape velocity calculation
     * @return true if finalization occurred, false if already finalized (guard)
     */
    bool Finalize(int bar, SCDateTime time, double currentPrice, double tickSize) {
        // Exactly-once guard: prevent double-finalization
        if (finalizedThisEngagement) {
            return false;  // Already finalized this engagement
        }

        endBar = bar;
        endTime = time;
        exitPrice = currentPrice;  // Phase 1A: Record exit price
        barsEngaged = endBar - startBar + 1;  // Inclusive (minimum 1)
        secondsEngaged = GetElapsedSeconds(startTime, endTime);

        // Phase 1A: Compute escape velocity
        // Formula: |exitPrice - entryPrice| / tickSize / barsEngaged
        // Unit: ticks/bar
        if (barsEngaged > 0 && tickSize > 0.0) {
            escapeVelocity = std::fabs(exitPrice - entryPrice) / tickSize / static_cast<double>(barsEngaged);
        } else {
            escapeVelocity = 0.0;
        }

        finalizedThisEngagement = true;  // Mark as finalized

        return true;  // Finalization occurred
    }

    void Reset() {
        startBar = -1;
        endBar = -1;
        barsEngaged = 0;
        secondsEngaged = 0;
        cumulativeVolume = 0.0;
        cumulativeDelta = 0.0;
        volumeRatio = 0.0;
        peakPenetrationTicks = 0;
        avgClosePrice = 0.0;
        entryPrice = 0.0;
        exitPrice = 0.0;
        escapeVelocity = 0.0;
        rotation.Reset();
        outcome = AuctionOutcome::PENDING;
        wasHighVolume = false;
        wasLowVolume = false;
        wasDeltaAligned = false;
        wasFailedAuction = false;
        wasResponsiveDefense = false;
        finalizedThisEngagement = false;
    }
};

// ============================================================================
// TOUCH RECORD
// Frozen record of a completed engagement
// ============================================================================

struct TouchRecord {
    int touchNumber = 0;
    TouchType type = TouchType::TAG;
    int barsEngaged = 0;
    int penetrationTicks = 0;
    AuctionOutcome outcome = AuctionOutcome::PENDING;
    SCDateTime timestamp;
    UnresolvedReason unresolvedReason = UnresolvedReason::NONE;  // Reason if type == UNRESOLVED
};

// ============================================================================
// FINALIZATION RESULT
// Immutable event record returned by FinalizeEngagement
// INVARIANT: Callback receives this snapshot, NEVER the mutable buffer
// ============================================================================

/**
 * Result of engagement finalization.
 * Contains immutable snapshot of finalized metrics - NEVER references mutable buffer.
 *
 * INVARIANT: If finalized==true, metrics contains valid finalized data.
 *            If finalized==false, metrics is default-constructed (caller must not use).
 */
struct FinalizationResult {
    bool finalized = false;           // True if finalization occurred
    EngagementMetrics metrics;        // Immutable copy of finalized metrics (valid only if finalized)
    TouchRecord touchRecord;          // Frozen touch record (valid only if finalized)

    // Convenience accessors
    explicit operator bool() const { return finalized; }

    static FinalizationResult None() {
        return FinalizationResult{false, {}, {}};
    }

    static FinalizationResult Success(const EngagementMetrics& m, const TouchRecord& t) {
        return FinalizationResult{true, m, t};
    }
};

// ============================================================================
// ZONE RUNTIME
// Complete zone object with immutable identity
// A2: Per-zone inside/outside tracking (allocation-free hot path)
// ============================================================================

struct ZoneRuntime {
    // ========================================================================
    // IMMUTABLE IDENTITY (const - never changes after construction)
    // ========================================================================
    const int zoneId;
    const ZoneType type;
    const ZoneRole role;
    const AnchorMechanism mechanism;
    const ZoneSource source;
    double originalAnchorPrice;  // Original anchor at creation (for history) - NEVER MUTATE
    const SCDateTime creationTime;
    const int creationBar;

    // ========================================================================
    // MUTABLE ANCHOR (can be recentered on POC migration)
    // SSOT: anchorTicks is authoritative; anchorPrice is DERIVED (ticks * tickSize)
    // This eliminates "round the rounded thing" float drift
    //
    // ENCAPSULATION: These fields are PRIVATE. Use accessors:
    //   - GetAnchorTicks(): Read authoritative tick value
    //   - GetAnchorPrice(): Read derived price (for display/logging only)
    //   - GetTickSize(): Read cached tick size
    //   - RecenterEx(): Controlled modification of anchor
    // ========================================================================
private:
    long long anchorTicks_ = 0;        // Authoritative anchor in ticks (wide type for overflow safety)
    double anchorPrice_ = 0.0;         // DERIVED: anchorTicks * tickSize (recomputed after any change)
    double tickSizeCache_ = 0.0;       // Cached tick size for price derivation
public:
    int recenterCount = 0;             // How many times this zone has been recentered

    // Pending action (latched when blocked by engagement, applied after finalize)
    enum class PendingAction { NONE, RECENTER, REPLACE };
    PendingAction pendingAction = PendingAction::NONE;
    long long pendingTicks = 0;        // Target ticks for pending action

    // Set after FinalizeEngagement if a REPLACE was pending - caller should retire this zone
    bool pendingReplaceNeeded = false;

    // ========================================================================
    // STRUCTURAL CONTEXT (slow-changing, updated on session/profile change)
    // ========================================================================
    ValueAreaRegion vaRegion = ValueAreaRegion::CORE_VA;
    int distanceFromPOCTicks = 0;        // Signed: + above, - below
    VolumeCharacteristics levelProfile;  // Volume attributes of the level

    // ========================================================================
    // PROXIMITY STATE (fast-changing, updated every bar)
    // ========================================================================
    ZoneProximity proximity = ZoneProximity::INACTIVE;
    ZoneProximity priorProximity = ZoneProximity::INACTIVE;

    // Diagnostic: Count times price was exactly at core boundary (for chatter detection)
    int proximityBoundaryHits = 0;

    // ========================================================================
    // A2: PER-ZONE INSIDE/OUTSIDE TRACKING (replaces static map)
    // These fields eliminate the need for static unordered_map<int, SCDateTime>
    // ========================================================================
    SCDateTime lastInsideTime;           // Last time price was inside this zone
    int lastInsideBar = -1;              // Last bar price was inside this zone
    SCDateTime lastOutsideTime;          // When price left this zone
    int lastOutsideBar = -1;             // When price left this zone
    double secondsOutsideHalo = 0.0;     // Time spent outside halo since leaving
    int barsOutsideHalo = 0;             // Bars spent outside halo since leaving

    // ========================================================================
    // BOUNDARY-SPECIFIC TRACKING (for failed auction detection)
    // Tracks when price was outside the VALUE AREA boundary (not just zone halo)
    // Only used for VALUE_BOUNDARY zones (VAH, VAL)
    // ========================================================================
    int lastOutsideBoundaryBar = -1;     // Last bar price was outside VA boundary
    SCDateTime lastOutsideBoundaryTime;  // When price was last outside boundary
    int barsSinceReturnedFromOutside = 0;// Bars since returning from outside boundary
    bool wasOutsideBoundary = false;     // Was price beyond this boundary level?

    // ========================================================================
    // OUTCOME (set once per engagement, frozen after)
    // ========================================================================
    AuctionOutcome outcome = AuctionOutcome::PENDING;

    // ========================================================================
    // TOUCH TRACKING
    // ========================================================================
    int touchCount = 0;
    int lastTouchBar = -1;        // Last time ANY engagement started
    int barsSinceTouch = 0;       // Current bar - lastTouchBar

    // Explicit event tracking (prevents aliasing)
    int lastFailureBar = -1;      // Last failed auction event
    int lastAcceptanceBar = -1;   // Last acceptance event
    int lastRejectionBar = -1;    // Last rejection event

    // ========================================================================
    // LIFETIME OUTCOME COUNTERS (SSOT - survive truncation)
    // Invariant: touchCount == lifetimeAcceptances + lifetimeRejections
    //            + lifetimeTags + lifetimeUnresolved + pending
    // where pending = 1 if HasPendingEngagement(), else 0
    // ========================================================================
    int lifetimeAcceptances = 0;      // Outcomes classified ACCEPTANCE
    int lifetimeRejections = 0;       // Outcomes classified PROBE/TEST (meaningful rejections)
    int lifetimeTags = 0;             // Outcomes classified TAG (noise)
    int lifetimeUnresolved = 0;       // Engagements force-finalized without resolution

    // Rejection subtypes (must sum to lifetimeRejections)
    // Invariant: lifetimeProbes + lifetimeTests + lifetimeRejectionsOther == lifetimeRejections
    int lifetimeProbes = 0;           // Quick rejection (PROBE)
    int lifetimeTests = 0;            // Sustained rejection (TEST)
    int lifetimeRejectionsOther = 0;  // Future rejection subtypes (currently 0)

    // Halo width at zone creation (for schema comparability)
    int creationHaloWidthTicks = 0;   // Set at construction, not default

    // ========================================================================
    // ENGAGEMENT
    // ========================================================================
    EngagementMetrics currentEngagement;
    std::vector<TouchRecord> touchHistory;
    std::vector<EngagementMetrics> engagementHistory;

    // ========================================================================
    // STRENGTH
    // ========================================================================
    ZoneStrength strengthTier = ZoneStrength::VIRGIN;
    double strengthScore = 1.0;

    // ========================================================================
    // CONFIGURATION (can be updated, but rarely)
    // ========================================================================
    int coreWidthTicks = 3;
    int haloWidthTicks = 8;

    // ========================================================================
    // CONSTRUCTOR (enforces immutability)
    // ========================================================================
    ZoneRuntime(int id, ZoneType t, ZoneRole r, AnchorMechanism m,
                ZoneSource s, double anchor, SCDateTime created, int bar,
                int haloWidth = 8, double tickSize = 0.25)  // haloWidth for SSOT schema tracking
        : zoneId(id), type(t), role(r), mechanism(m), source(s)
        , originalAnchorPrice(anchor), creationTime(created), creationBar(bar)
    {
        // Identity fields are now const and can never be changed
        creationHaloWidthTicks = haloWidth;  // Capture halo width at creation

        // SSOT: Initialize anchor from ticks (authoritative)
        // Use canonical PriceToTicks for consistent rounding policy
        tickSizeCache_ = (tickSize > 0.0) ? tickSize : 0.25;
        anchorTicks_ = PriceToTicks(anchor, tickSizeCache_);
        anchorPrice_ = TicksToPrice(anchorTicks_, tickSizeCache_);  // Derived from ticks
    }

    // Delete copy constructor/assignment (zones are unique)
    ZoneRuntime(const ZoneRuntime&) = delete;
    ZoneRuntime& operator=(const ZoneRuntime&) = delete;

    // Allow move constructor/assignment
    ZoneRuntime(ZoneRuntime&&) = default;
    ZoneRuntime& operator=(ZoneRuntime&&) = default;

    // ========================================================================
    // HELPER METHODS
    // ========================================================================

    /**
     * Check if there is a pending (active) engagement
     * @return true if engagement is in progress (startBar >= 0)
     */
    bool HasPendingEngagement() const {
        return currentEngagement.startBar >= 0;
    }

    /**
     * Get anchor in ticks (SSOT accessor).
     * Use this for all comparisons and threshold logic.
     * @return Anchor price as integer ticks
     */
    long long GetAnchorTicks() const {
        return anchorTicks_;
    }

    /**
     * Get anchor price (DERIVED from anchorTicks * tickSize).
     * Prefer GetAnchorTicks() for comparisons to avoid float issues.
     * @return Anchor price in price units
     */
    double GetAnchorPrice() const {
        return anchorPrice_;
    }

    /**
     * Get cached tick size used for this zone's anchor.
     * @return Tick size used for anchor derivation
     */
    double GetTickSize() const {
        return tickSizeCache_;
    }

    // Thresholds for recenter vs retire decisions (in ticks)
    // NOTE: These are ES-specific (8 ticks = 2.00 points). For other instruments,
    // consider expressing as fraction of VA width or ATR.
    static constexpr int RECENTER_MIN_TICKS = 1;    // Minimum change to trigger recenter
    static constexpr int LARGE_JUMP_TICKS = 8;      // Jump >= this means structural change (retire zone)

    /**
     * Result of a recenter attempt
     */
    enum class RecenterResult {
        NO_CHANGE,          // Change too small (< RECENTER_MIN_TICKS)
        APPLIED,            // Recenter applied successfully
        LATCHED_RECENTER,   // Blocked by engagement, recenter latched for later
        LATCHED_REPLACE,    // Blocked by engagement, replace latched for later (large jump)
        LARGE_JUMP          // Jump too large AND no engagement - caller should retire+create
    };

    /**
     * Recenter the zone to a new anchor price.
     * IMPORTANT: This preserves all stats (touch counts, engagement history, etc.)
     *
     * @param newPrice The new anchor price
     * @param tickSize Tick size for validation
     * @return RecenterResult indicating what happened
     *
     * Guardrails:
     * - If change >= LARGE_JUMP_TICKS AND engaged: LATCH as REPLACE (apply after finalize)
     * - If change >= LARGE_JUMP_TICKS AND not engaged: LARGE_JUMP (caller retires+creates)
     * - If engaged: LATCH as RECENTER (apply after finalize)
     * - Only applies if change >= 1 whole tick
     */
    RecenterResult RecenterEx(double newPrice, double tickSize) {
        if (tickSize <= 0.0) return RecenterResult::NO_CHANGE;

        // SSOT: Work in integer ticks (authoritative)
        // Use canonical PriceToTicks for consistent rounding policy
        const long long newTicks = PriceToTicks(newPrice, tickSize);
        const long long deltaTicks = std::abs(newTicks - anchorTicks_);

        // Guard: Change too small
        if (deltaTicks < RECENTER_MIN_TICKS) {
            return RecenterResult::NO_CHANGE;
        }

        const bool isLargeJump = (deltaTicks >= LARGE_JUMP_TICKS);

        // If engagement active: LATCH (don't discard, apply after finalize)
        // REPLACE takes priority over RECENTER if both queued
        if (HasPendingEngagement()) {
            if (isLargeJump) {
                // Large jump during engagement - latch as REPLACE
                pendingAction = PendingAction::REPLACE;
                pendingTicks = newTicks;
                return RecenterResult::LATCHED_REPLACE;
            } else {
                // Normal drift during engagement - latch as RECENTER (unless REPLACE already pending)
                if (pendingAction != PendingAction::REPLACE) {
                    pendingAction = PendingAction::RECENTER;
                    pendingTicks = newTicks;
                }
                return RecenterResult::LATCHED_RECENTER;
            }
        }

        // Not engaged - can apply immediately
        if (isLargeJump) {
            // Signal caller to retire+create instead
            return RecenterResult::LARGE_JUMP;
        }

        // Apply recenter - preserve all stats
        anchorTicks_ = newTicks;
        tickSizeCache_ = tickSize;
        anchorPrice_ = TicksToPrice(anchorTicks_, tickSizeCache_);  // Derive price from ticks
        recenterCount++;
        pendingAction = PendingAction::NONE;
        pendingTicks = 0;
        return RecenterResult::APPLIED;
    }

    /**
     * Legacy wrapper - returns true only if recenter was applied
     */
    bool Recenter(double newPrice, double tickSize) {
        RecenterResult result = RecenterEx(newPrice, tickSize);
        return result == RecenterResult::APPLIED;
    }

    /**
     * Result of applying pending action
     */
    enum class PendingApplyResult {
        NONE,               // No pending action
        STILL_ENGAGED,      // Still in engagement (safety check failed)
        RECENTER_APPLIED,   // Pending recenter was applied
        REPLACE_NEEDED      // Pending replace - caller must retire+create this zone
    };

    /**
     * Apply any pending action that was latched during engagement.
     * Call this after engagement finalizes.
     * @param tickSize Tick size for validation
     * @return PendingApplyResult indicating what happened
     *
     * IMPORTANT: If result is REPLACE_NEEDED, caller must:
     * 1. Retire this zone (remove from activeZones)
     * 2. Create a new zone at the pending price
     */
    PendingApplyResult ApplyPendingAction(double tickSize) {
        if (pendingAction == PendingAction::NONE || pendingTicks <= 0) {
            return PendingApplyResult::NONE;
        }

        // Don't apply if still in engagement (safety check)
        if (HasPendingEngagement()) {
            return PendingApplyResult::STILL_ENGAGED;
        }

        const long long deltaTicks = std::abs(pendingTicks - anchorTicks_);
        PendingApplyResult result = PendingApplyResult::NONE;

        if (pendingAction == PendingAction::REPLACE) {
            // Large jump was latched - signal caller to retire+create
            result = PendingApplyResult::REPLACE_NEEDED;
            // Don't apply - let caller handle retirement
        }
        else if (pendingAction == PendingAction::RECENTER && deltaTicks >= 1) {
            // Apply recenter
            anchorTicks_ = pendingTicks;
            tickSizeCache_ = tickSize;
            anchorPrice_ = TicksToPrice(anchorTicks_, tickSizeCache_);  // Derive price from ticks
            recenterCount++;
            result = PendingApplyResult::RECENTER_APPLIED;
        }

        // Clear pending state
        pendingAction = PendingAction::NONE;
        pendingTicks = 0;
        return result;
    }

    // Legacy wrapper for backward compatibility
    bool ApplyPendingRecenter(double tickSize) {
        PendingApplyResult result = ApplyPendingAction(tickSize);
        return result == PendingApplyResult::RECENTER_APPLIED;
    }

    /**
     * Get the pending target price (for REPLACE handling)
     */
    double GetPendingPrice(double tickSize) const {
        return pendingTicks * tickSize;
    }

    /**
     * Check if there's a pending REPLACE action
     */
    bool HasPendingReplace() const {
        return pendingAction == PendingAction::REPLACE && pendingTicks > 0;
    }

    // ========================================================================
    // LIFECYCLE METHODS
    // ========================================================================

    /**
     * Start a new zone engagement.
     * @param bar Current bar index
     * @param time Current bar time
     * @param currentPrice Close price at entry (entryPrice for escape velocity)
     */
    void StartEngagement(int bar, SCDateTime time, double currentPrice) {
        currentEngagement.Start(bar, time, currentPrice);
        lastTouchBar = bar;
        touchCount++;
        lastInsideBar = bar;
        lastInsideTime = time;
    }

    /**
     * Finalize the current engagement.
     * @param bar Current bar index
     * @param time Current bar time
     * @param exitPrice Close price at exit (for escape velocity)
     * @param tickSize Tick size for escape velocity calculation
     * @param cfg Zone configuration for touch classification
     * @return FinalizationResult containing immutable snapshot of metrics (or None() if no finalization)
     *
     * INVARIANT: Caller must use result.metrics for callbacks, NEVER zone.currentEngagement.
     *            After this call returns, currentEngagement is reset.
     */
    FinalizationResult FinalizeEngagement(int bar, SCDateTime time, double exitPrice,
                                          double tickSize, const ZoneConfig& cfg);

    /**
     * Force-finalize an engagement that cannot complete normally.
     * Used for session roll, zone expiry, chart reset, or timeout.
     * @param bar Current bar index (must be valid)
     * @param time Current bar time (must be valid)
     * @param reason Why engagement is being force-finalized
     * @return true if force-finalization occurred, false if no pending engagement
     */
    FinalizationResult ForceFinalize(int bar, SCDateTime time, UnresolvedReason reason);

    void RecordOutcome(AuctionOutcome newOutcome, int currentBar) {
        outcome = newOutcome;

        if (newOutcome == AuctionOutcome::ACCEPTED) {
            lastAcceptanceBar = currentBar;
        } else if (newOutcome == AuctionOutcome::REJECTED) {
            lastRejectionBar = currentBar;

            // Check if it was a failed auction
            if (!engagementHistory.empty() &&
                engagementHistory.back().wasFailedAuction) {
                lastFailureBar = currentBar;
            }
        }
    }

    /**
     * Update inside/outside tracking (A2)
     * Called each bar to maintain allocation-free hot-path tracking
     */
    void UpdateInsideOutsideTracking(int currentBar, SCDateTime currentTime,
                                     bool isInsideHalo) {
        if (isInsideHalo) {
            // Inside zone - update last inside
            lastInsideBar = currentBar;
            lastInsideTime = currentTime;
            barsOutsideHalo = 0;
            secondsOutsideHalo = 0.0;
        } else {
            // Outside zone - track time/bars outside
            if (lastInsideBar >= 0 && lastOutsideBar != currentBar) {
                // First bar outside or new bar
                if (lastOutsideBar < 0) {
                    lastOutsideBar = currentBar;
                    lastOutsideTime = currentTime;
                }
                barsOutsideHalo = currentBar - lastOutsideBar;
                secondsOutsideHalo = GetElapsedSeconds(lastOutsideTime, currentTime);
            }
        }
    }

    /**
     * Update boundary tracking (for failed auction detection)
     * Only meaningful for VALUE_BOUNDARY zones (VAH, VAL)
     *
     * @param currentBar Current bar index
     * @param currentTime Current bar time
     * @param isOutsideBoundary True if price is beyond this boundary (above VAH or below VAL)
     * @param isInsideVA True if price is inside the value area
     */
    void UpdateBoundaryTracking(int currentBar, SCDateTime currentTime,
                                bool isOutsideBoundary, bool isInsideVA) {
        if (role != ZoneRole::VALUE_BOUNDARY) return;

        if (isOutsideBoundary) {
            // Price is outside this boundary
            lastOutsideBoundaryBar = currentBar;
            lastOutsideBoundaryTime = currentTime;
            wasOutsideBoundary = true;
            barsSinceReturnedFromOutside = 0;
        } else if (wasOutsideBoundary && isInsideVA) {
            // Price has returned from outside - track bars since return
            barsSinceReturnedFromOutside = currentBar - lastOutsideBoundaryBar;
        }
        // If price is at boundary (not outside, not inside VA), don't change tracking
    }

    /**
     * Check if this is a failed auction (price broke out then returned quickly)
     */
    bool IsFailedAuction(int maxBarsForFailedAuction) const {
        if (role != ZoneRole::VALUE_BOUNDARY) return false;
        if (!wasOutsideBoundary) return false;
        return barsSinceReturnedFromOutside > 0 &&
               barsSinceReturnedFromOutside <= maxBarsForFailedAuction;
    }

    /**
     * Reset boundary tracking (call on new session)
     */
    void ResetBoundaryTracking() {
        lastOutsideBoundaryBar = -1;
        lastOutsideBoundaryTime = SCDateTime();
        barsSinceReturnedFromOutside = 0;
        wasOutsideBoundary = false;
    }

    // Age check
    bool IsExpired(SCDateTime now, int maxAgeSeconds) const {
        return IsOlderThan(creationTime, now, maxAgeSeconds);
    }

    int GetAgeSeconds(SCDateTime now) const {
        return GetElapsedSeconds(creationTime, now);
    }
};

// ============================================================================
// ZONE PRIORITY (for deterministic tie-breaking)
// ============================================================================

struct ZonePriorityExtended {
    // Primary (lexicographic)
    int role = 0;        // Higher is better
    int source = 0;      // Higher is better
    int strength = 0;    // Higher is better

    // Tie-breakers
    double distanceTicks = 9999.0;  // SMALLER is better (closer wins)
    int lastTouchBar = -1;          // HIGHER is better (more recent wins)
    int zoneId = 999999;            // LOWER wins (arbitrary but deterministic)

    /**
     * Full comparison with correct direction for tie-breakers
     * Used with max_element to find highest priority zone
     *
     * Return true if *this is LESS THAN other (other wins)
     */
    bool operator<(const ZonePriorityExtended& other) const {
        // Primary: lexicographic (higher wins)
        if (role != other.role) return role < other.role;
        if (source != other.source) return source < other.source;
        if (strength != other.strength) return strength < other.strength;

        // Secondary: distance (SMALLER wins, so LARGER is "less than")
        if (distanceTicks != other.distanceTicks) {
            return distanceTicks > other.distanceTicks;  // INVERTED
        }

        // Tertiary: recency (HIGHER bar wins, so SMALLER bar is "less than")
        if (lastTouchBar != other.lastTouchBar) {
            return lastTouchBar < other.lastTouchBar;
        }

        // Quaternary: stable ID (LOWER wins, so HIGHER is "less than")
        return zoneId > other.zoneId;  // INVERTED
    }

    bool operator>(const ZonePriorityExtended& other) const {
        return other < *this;
    }

    std::string ToString() const {
        char buf[256];
        snprintf(buf, sizeof(buf),
                "Priority(role=%d, source=%d, strength=%d, dist=%.1f, touch=%d, id=%d)",
                role, source, strength, distanceTicks, lastTouchBar, zoneId);
        return std::string(buf);
    }
};

// ============================================================================
// ZONE SESSION STATE (formerly SessionContext - renamed to avoid collision)
// ============================================================================
// This struct contains session-related state used by ZoneManager.
// NOTE: This is a CONSUMER of session state, not the SSOT.
// SSOT for session identity: SessionManager.currentSession (SessionKey)
// SSOT for POC/VAH/VAL: SessionManager.sessionPOC/VAH/VAL
// This struct caches values for zone operations but should be synced from SSOT.
//
// TODO: Migrate consumers to read from SessionManager directly and eliminate
// this cache, or make it clearly a read-only sync copy.
// ============================================================================

struct ZoneSessionState {
    // NOTE: POC/VAH/VAL have been REMOVED from this struct.
    // SSOT for levels is now SessionManager.sessionPOC/VAH/VAL.
    // All code must use SessionManager.GetPOC()/GetVAH()/GetVAL() instead.
    // The old rth_poc/rth_vah/rth_val/rth_vaRangeTicks fields are DELETED.

    // NOTE: Session extremes (rth_high/low, rth_high_bar, etc.) have been REMOVED.
    // SSOT for session extremes is now StructureTracker (ZoneManager.structure),
    // which uses bar High/Low data (not Close). Use ZoneManager accessors:
    //   zm.GetSessionHigh(), zm.GetSessionLow()
    //   zm.IsHighUpdatedRecently(), zm.IsLowUpdatedRecently()

    // NOTE: rth_ib_high/low/vwap removed - SSOT is StructureTracker.ibHigh/ibLow
    // NOTE: rth_vwap was never used (dead code)

    // ========================================================================
    // PRIOR SESSION (Tri-State Contract for Prior VBP Availability)
    // ========================================================================
    // SSOT: Updated exactly once per session roll, must remain constant intra-session
    // NOTE: 0.0 is NOT a valid "unknown" marker - use priorVBPState field
    //
    // Tri-State Contract:
    //   - PRIOR_VALID: prior_* values are usable, zones should be created
    //   - PRIOR_MISSING: Insufficient history (degraded mode, no zones)
    //   - PRIOR_DUPLICATES_CURRENT: Logic defect, log BUG with diagnostic context
    // ========================================================================
    double prior_poc = 0.0;
    double prior_vah = 0.0;
    double prior_val = 0.0;
    long long prior_poc_ticks = 0;  // Tick-based for comparisons
    long long prior_vah_ticks = 0;
    long long prior_val_ticks = 0;
    bool hasPriorProfile = false;   // True only after first session completes
    PriorVBPState priorVBPState = PriorVBPState::PRIOR_MISSING;  // Tri-state status

    // Session metadata
    ProfileShape profileShape = ProfileShape::UNDEFINED;
    // NOTE: sessionPhase removed - SSOT is phaseCoordinator.GetPhase()

    // Volume baselines
    double sessionTotalVolume = 0.0;  // NOTE: Stores SESSION TOTAL, not per-bar average
    double avgVolumePerTick = 0.0;
    // NOTE: sessionStartBar/sessionStartTime removed - SSOT is SessionManager.sessionStartBar

    // ========================================================================
    // SSOT INVARIANT: Single-writer enforcement (Follow-through 1)
    // Context should be written by exactly ONE code path per update cycle.
    // ========================================================================
    int initializationBar = -1;      // Bar when context was initialized
    int currentCycleBar = -1;        // Current cycle for write counting
    int writeCountThisCycle = 0;     // Number of writes in current cycle

    /**
     * Begin a new update cycle. Resets write counter.
     * Call this at the START of each bar's processing.
     */
    void BeginCycle(int bar) {
#ifdef _DEBUG
        // If we're starting a new cycle and had writes in previous cycle,
        // verify exactly one write occurred (unless first run)
        if (currentCycleBar >= 0 && currentCycleBar != bar && writeCountThisCycle != 1) {
            // writeCountThisCycle == 0 is ok for bars with no profile update
            // writeCountThisCycle > 1 means multiple writers - SSOT breach
            if (writeCountThisCycle > 1) {
                assert(false && "SessionContext had multiple writers in cycle - SSOT breach");
            }
        }
#endif
        if (currentCycleBar != bar) {
            currentCycleBar = bar;
            writeCountThisCycle = 0;
        }
    }

    /**
     * Record a write to session context. Called by the single authorized writer.
     * In debug builds, asserts if this is a second write in the same cycle.
     */
    void RecordWrite(int bar) {
        // Ensure cycle is started
        if (currentCycleBar != bar) {
            BeginCycle(bar);
        }
        writeCountThisCycle++;

#ifdef _DEBUG
        // INVARIANT: Only one write per cycle
        if (writeCountThisCycle > 1) {
            assert(false && "SessionContext written multiple times in cycle - SSOT breach");
        }
#endif
        initializationBar = bar;
    }

    /**
     * End-of-cycle validation. Call at END of bar processing.
     * Verifies single-writer invariant was maintained.
     */
    void EndCycle([[maybe_unused]] int bar) {
#ifdef _DEBUG
        if (currentCycleBar == bar && writeCountThisCycle > 1) {
            assert(false && "SessionContext cycle ended with multiple writers - SSOT breach");
        }
#endif
    }

    /**
     * Check if context was initialized this bar (for debug diagnostics).
     */
    bool WasInitializedThisBar(int currentBar) const {
        return initializationBar == currentBar;
    }

    /**
     * Get write count for current cycle (debug diagnostics).
     */
    int GetWriteCountThisCycle() const { return writeCountThisCycle; }

    // NOTE: UpdateExtremes() and IsHighUpdatedRecently/IsLowUpdatedRecently/IsExtremeUpdatedRecently
    // have been REMOVED. Session extremes are now managed by StructureTracker.
    // Use ZoneManager accessors: zm.GetSessionHigh(), zm.IsHighUpdatedRecently(), etc.

    /**
     * Reset all session context for new session.
     * Caller MUST invoke this on session roll.
     */
    void Reset() {
        // NOTE: POC/VAH/VAL are now in SessionManager - reset those via sessionMgr.resetLevels()
        // NOTE: Session extremes are now in StructureTracker - reset via structure.Reset()
        // NOTE: IB levels are now in StructureTracker - reset via structure.Reset()
        // NOTE: sessionStartBar is now in SessionManager - set at session transition

        // NOTE: prior_* fields are NOT reset here - they persist across sessions
        // They are updated ONLY by CapturePriorSession() at session roll

        // Metadata
        profileShape = ProfileShape::UNDEFINED;

        // Volume baselines
        sessionTotalVolume = 0.0;
        avgVolumePerTick = 0.0;

        // SSOT tracking
        initializationBar = -1;
        currentCycleBar = -1;
        writeCountThisCycle = 0;
    }

    /**
     * Capture current session levels as prior session levels.
     * SSOT: Call this ONCE at session roll, BEFORE Reset().
     * @param poc Current session POC (from SessionManager)
     * @param vah Current session VAH (from SessionManager)
     * @param val Current session VAL (from SessionManager)
     * @param tickSize Tick size for computing tick-based values
     */
    void CapturePriorSession(double poc, double vah, double val, double tickSize) {
        // Only capture if current session has valid levels
        if (poc > 0.0 && vah > val && vah > 0.0 && val > 0.0) {
            prior_poc = poc;
            prior_vah = vah;
            prior_val = val;

            // Compute tick-based versions for comparisons (SSOT: PriceToTicks)
            if (tickSize > 0.0) {
                prior_poc_ticks = PriceToTicks(poc, tickSize);
                prior_vah_ticks = PriceToTicks(vah, tickSize);
                prior_val_ticks = PriceToTicks(val, tickSize);
            }

            hasPriorProfile = true;
        }
        // If current session invalid, prior_* retains previous values (or stays invalid)
    }
};

// ============================================================================
// ZONE CONTEXT SNAPSHOT (C)
// Result of a zone update cycle - includes transition info
// Used for early-exit optimization that preserves semantics
// ============================================================================

struct ZoneContextSnapshot {
    // Primary zone info
    int primaryZoneId = -1;
    ZoneProximity dominantProximity = ZoneProximity::INACTIVE;

    // Confluence
    int zonesAtPrice = 0;
    int zonesApproaching = 0;

    // Transition flags (copied from TransitionState after processing)
    bool justEnteredZone = false;
    bool justExitedZone = false;
    bool justChangedZone = false;

    // Engagement info (if at zone)
    int engagementBars = 0;
    int engagementSeconds = 0;

    // Validity
    bool valid = false;
    int computedAtBar = -1;

    void Reset() {
        primaryZoneId = -1;
        dominantProximity = ZoneProximity::INACTIVE;
        zonesAtPrice = 0;
        zonesApproaching = 0;
        justEnteredZone = false;
        justExitedZone = false;
        justChangedZone = false;
        engagementBars = 0;
        engagementSeconds = 0;
        valid = false;
        computedAtBar = -1;
    }
};

// ============================================================================
// ENGAGEMENT FINALIZATION CALLBACK
// Receives immutable data from FinalizationResult (defined above TouchRecord)
// ============================================================================

/**
 * Callback type for engagement finalization.
 * Called when a zone engagement is finalized (zone exited or expired).
 * Used to push metrics to session baselines and update engagement accumulators.
 *
 * IMPORTANT: This callback receives IMMUTABLE data from FinalizationResult.
 * - metrics: Snapshot of engagement metrics at finalization time
 * - result: Full finalization result including TouchRecord for classification
 *
 * Logging should happen at the call site where sc is in scope.
 */
using EngagementFinalizedCallback = std::function<void(const ZoneRuntime&, const FinalizationResult&)>;

// ============================================================================
// SESSION ANCHORS (MEDIUM-1)
// SSOT for structural zone IDs - prevents fragmented storage
// All anchor IDs MUST be resolvable in activeZones or be -1
// ============================================================================

struct SessionAnchors {
    // Current session VBP zones
    int pocId = -1;
    int vahId = -1;
    int valId = -1;
    int vwapId = -1;

    // Prior session zones (SSOT for PRIOR_* zone IDs)
    int priorPocId = -1;
    int priorVahId = -1;
    int priorValId = -1;

    // Structure zones (only used if createStructureZones = true)
    int ibHighId = -1;
    int ibLowId = -1;
    int sessionHighId = -1;
    int sessionLowId = -1;

    /**
     * Clear an anchor if it matches the given zone ID.
     * Called atomically when a zone is removed.
     */
    void ClearIfMatches(int zoneId) {
        if (pocId == zoneId) pocId = -1;
        if (vahId == zoneId) vahId = -1;
        if (valId == zoneId) valId = -1;
        if (vwapId == zoneId) vwapId = -1;
        if (priorPocId == zoneId) priorPocId = -1;
        if (priorVahId == zoneId) priorVahId = -1;
        if (priorValId == zoneId) priorValId = -1;
        if (ibHighId == zoneId) ibHighId = -1;
        if (ibLowId == zoneId) ibLowId = -1;
        if (sessionHighId == zoneId) sessionHighId = -1;
        if (sessionLowId == zoneId) sessionLowId = -1;
    }

    /**
     * Check if any anchor references the given zone ID.
     */
    bool ReferencesZone(int zoneId) const {
        return pocId == zoneId || vahId == zoneId || valId == zoneId ||
               vwapId == zoneId || ibHighId == zoneId || ibLowId == zoneId ||
               priorPocId == zoneId || priorVahId == zoneId || priorValId == zoneId ||
               sessionHighId == zoneId || sessionLowId == zoneId;
    }

    /**
     * Reset all anchors to invalid.
     * NOTE: Prior anchors are also reset (they'll be recreated from sessionCtx.prior_* values)
     */
    void Reset() {
        pocId = vahId = valId = vwapId = -1;
        priorPocId = priorVahId = priorValId = -1;
        ibHighId = ibLowId = sessionHighId = sessionLowId = -1;
    }

    /**
     * Validate that all non-negative anchors exist in the given zone map.
     * Returns false if any anchor is stale (points to non-existent zone).
     */
    bool ValidateAgainstZones(const std::unordered_map<int, ZoneRuntime>& zones) const {
        auto valid = [&zones](int id) {
            return id < 0 || zones.count(id) > 0;
        };
        return valid(pocId) && valid(vahId) && valid(valId) &&
               valid(vwapId) && valid(ibHighId) && valid(ibLowId) &&
               valid(priorPocId) && valid(priorVahId) && valid(priorValId) &&
               valid(sessionHighId) && valid(sessionLowId);
    }
};

// ============================================================================
// STRUCTURE TRACKER
// Tracks session extremes and IB levels for logging (not as zones by default)
// SSOT for structure values used in log output
// ============================================================================

class StructureTracker {
private:
    // --- Initial Balance (frozen after IB window) - PRIVATE, use accessors ---
    double ibHigh_ = 0.0;
    double ibLow_ = 0.0;
    bool ibFrozen_ = false;          // True after IB window ends
    int ibWindowMinutes_ = 60;       // Standard: first 60 minutes of RTH
    int ibStartBar_ = -1;            // Bar when IB window started
    SCDateTime ibStartTime_;         // Time when IB window started

    // --- Session Extremes (dynamic) - PRIVATE, use accessors ---
    double sessionHigh_ = 0.0;
    double sessionLow_ = 0.0;
    int sessionHighBar_ = -1;        // Bar when session high was last updated
    int sessionLowBar_ = -1;         // Bar when session low was last updated

    // --- Range-Adaptive Thresholds - PRIVATE, use accessors ---
    int sessionRangeTicks_ = 0;      // Current session range in ticks
    int adaptiveCoreTicks_ = 3;      // Computed from range
    int adaptiveHaloTicks_ = 8;      // Computed from range
    int lastRangeUpdateBar_ = -1;    // For log-on-change

public:
    // --- Read-only accessors ---
    double GetSessionHigh() const { return sessionHigh_; }
    double GetSessionLow() const { return sessionLow_; }
    int GetSessionHighBar() const { return sessionHighBar_; }
    int GetSessionLowBar() const { return sessionLowBar_; }
    double GetIBHigh() const { return ibHigh_; }
    double GetIBLow() const { return ibLow_; }
    bool IsIBFrozen() const { return ibFrozen_; }
    int GetSessionRangeTicks() const { return sessionRangeTicks_; }
    int GetAdaptiveCoreTicks() const { return adaptiveCoreTicks_; }
    int GetAdaptiveHaloTicks() const { return adaptiveHaloTicks_; }

    /**
     * Update session extremes. Called every bar. (SINGLE WRITER for session extremes)
     * @param high Current bar high
     * @param low Current bar low
     * @param bar Current bar index
     */
    void UpdateExtremes(double high, double low, int bar) {
        if (high > sessionHigh_ || sessionHigh_ == 0.0) {
            sessionHigh_ = high;
            sessionHighBar_ = bar;
        }
        if (low < sessionLow_ || sessionLow_ == 0.0) {
            sessionLow_ = low;
            sessionLowBar_ = bar;
        }
    }

    /**
     * Update IB levels during IB window. Freeze when window ends. (SINGLE WRITER for IB levels)
     * @param high Current bar high
     * @param low Current bar low
     * @param time Current bar time
     * @param bar Current bar index
     * @param isRTH True if in RTH session
     */
    void UpdateIB(double high, double low, SCDateTime time, int bar, bool isRTH) {
        if (ibFrozen_) return;  // IB window closed

        // IB only tracks during RTH
        if (!isRTH) return;

        // Initialize IB start if this is first RTH bar
        if (ibStartBar_ < 0) {
            ibStartBar_ = bar;
            ibStartTime_ = time;
            ibHigh_ = high;
            ibLow_ = low;
            return;
        }

        // Update IB extremes while window is open
        if (high > ibHigh_) ibHigh_ = high;
        if (low < ibLow_ || ibLow_ == 0.0) ibLow_ = low;
    }

    /**
     * Check if IB window should be frozen.
     * @param time Current bar time
     * @param bar Current bar index (for tracking)
     */
    void CheckIBFreeze(SCDateTime time, int bar) {
        if (ibFrozen_) return;
        if (ibStartTime_.IsUnset()) return;

        // Calculate elapsed minutes since IB start
        // SCDateTime::GetAsDouble() returns days since epoch, multiply by 86400 for seconds
        double elapsedSeconds = (time.GetAsDouble() - ibStartTime_.GetAsDouble()) * 86400.0;
        int elapsedMinutes = static_cast<int>(elapsedSeconds / 60.0);

        if (elapsedMinutes >= ibWindowMinutes_) {
            ibFrozen_ = true;
            // IB is now frozen - levels will not change for rest of session
        }
    }

    /**
     * Compute range-adaptive thresholds.
     * Core/halo scale with session range, with floors and clamps.
     * @param tickSize Tick size for conversion
     * @param bar Current bar for log-on-change tracking
     * @return True if thresholds changed (for logging)
     */
    bool UpdateAdaptiveThresholds(double tickSize, int bar) {
        if (tickSize <= 0.0) return false;
        if (sessionHigh_ <= 0.0 || sessionLow_ <= 0.0) return false;

        int oldCore = adaptiveCoreTicks_;
        int oldHalo = adaptiveHaloTicks_;
        int oldRange = sessionRangeTicks_;

        // Compute range in ticks
        sessionRangeTicks_ = static_cast<int>((sessionHigh_ - sessionLow_) / tickSize);

        // Range-adaptive scaling:
        // - Quiet session (range < 40 ticks): core=3, halo=8 (floors)
        // - Normal session (40-80 ticks): scale proportionally
        // - Active session (range > 80 ticks): core=6, halo=16 (clamps)
        //
        // Formula: core = range / 12, clamped to [3, 6]
        //          halo = core * 2.5, clamped to [8, 16]

        int rawCore = sessionRangeTicks_ / 12;
        adaptiveCoreTicks_ = (std::max)(3, (std::min)(rawCore, 6));
        adaptiveHaloTicks_ = (std::max)(8, (std::min)(static_cast<int>(adaptiveCoreTicks_ * 2.5), 16));

        // Return true if thresholds changed (for log-on-change)
        bool changed = (oldCore != adaptiveCoreTicks_ || oldHalo != adaptiveHaloTicks_ ||
                        oldRange != sessionRangeTicks_);
        if (changed) {
            lastRangeUpdateBar_ = bar;
        }
        return changed;
    }

    /**
     * Get distance from current price to session high (in ticks).
     */
    int GetDistToSessionHighTicks(double price, double tickSize) const {
        if (sessionHigh_ <= 0.0 || tickSize <= 0.0) return -1;
        return static_cast<int>(std::round((sessionHigh_ - price) / tickSize));
    }

    /**
     * Get distance from current price to session low (in ticks).
     */
    int GetDistToSessionLowTicks(double price, double tickSize) const {
        if (sessionLow_ <= 0.0 || tickSize <= 0.0) return -1;
        return static_cast<int>(std::round((price - sessionLow_) / tickSize));
    }

    /**
     * Get distance from current price to IB high (in ticks).
     */
    int GetDistToIBHighTicks(double price, double tickSize) const {
        if (ibHigh_ <= 0.0 || tickSize <= 0.0) return -1;
        return static_cast<int>(std::round((ibHigh_ - price) / tickSize));
    }

    /**
     * Get distance from current price to IB low (in ticks).
     */
    int GetDistToIBLowTicks(double price, double tickSize) const {
        if (ibLow_ <= 0.0 || tickSize <= 0.0) return -1;
        return static_cast<int>(std::round((price - ibLow_) / tickSize));
    }

    /**
     * Reset for new session.
     */
    void Reset() {
        ibHigh_ = ibLow_ = 0.0;
        ibFrozen_ = false;
        ibStartBar_ = -1;
        ibStartTime_ = SCDateTime();

        sessionHigh_ = sessionLow_ = 0.0;
        sessionHighBar_ = sessionLowBar_ = -1;

        sessionRangeTicks_ = 0;
        adaptiveCoreTicks_ = 3;
        adaptiveHaloTicks_ = 8;
        lastRangeUpdateBar_ = -1;
    }

    /**
     * Format structure values for logging.
     */
    std::string FormatForLog(double price, double tickSize) const {
        char buf[256];
        int distSessHi = GetDistToSessionHighTicks(price, tickSize);
        int distSessLo = GetDistToSessionLowTicks(price, tickSize);
        int distIBHi = GetDistToIBHighTicks(price, tickSize);
        int distIBLo = GetDistToIBLowTicks(price, tickSize);

        snprintf(buf, sizeof(buf),
            "SESS_HI=%.2f SESS_LO=%.2f DIST_HI_T=%d DIST_LO_T=%d | "
            "IB_HI=%.2f IB_LO=%.2f DIST_IB_HI_T=%d DIST_IB_LO_T=%d IB_FROZEN=%s | "
            "RANGE_T=%d",
            sessionHigh_, sessionLow_, distSessHi, distSessLo,
            ibHigh_, ibLow_, distIBHi, distIBLo, ibFrozen_ ? "Y" : "N",
            sessionRangeTicks_);
        return std::string(buf);
    }
};

// ============================================================================
// ZONE MANAGER
// Central manager with stable ID-based storage
// ============================================================================

struct ZoneManager {
    // ========================================================================
    // ZONE LIFECYCLE INVARIANTS (enforced by design)
    // ========================================================================
    //
    // CREATION:
    //   Zones are created via CreateZone() or CreateZoneExplicit().
    //   Each zone gets a unique ID from nextZoneId (monotonically increasing).
    //   Zone identity (type, role, mechanism, source) is IMMUTABLE after creation.
    //
    // SESSION BOUNDARIES:
    //   Caller MUST invoke ResetForSession() on session roll.
    //   This clears: activeZones, anchors, sessionCtx, all stats.
    //   Zones do NOT survive session rolls by design.
    //   If zones could persist across sessions, classification caches would
    //   need versioning - but they don't because we reset.
    //
    // THRESHOLD CHANGES:
    //   ZoneConfig thresholds (core/halo width, acceptance criteria) can only
    //   change on session reset (via ResetForSession) or explicit reconfiguration.
    //   VolumeCharacteristics.classification is valid for zone lifetime because:
    //     - Thresholds don't change mid-session
    //     - Zones don't survive session rolls
    //   If this invariant is violated, classification becomes a stale cache.
    //
    // ID STABILITY:
    //   nextZoneId is NOT reset on session roll (monotonically increasing).
    //   This ensures zone IDs are globally unique within a process lifetime.
    //   Anchors (pocId, vahId, etc.) are reset to -1 on session roll.
    //
    // ========================================================================

    // Stable ID-based storage (no pointer invalidation)
    std::unordered_map<int, ZoneRuntime> activeZones;

    // Context (cache of session state - synced from SessionManager SSOT)
    ZoneSessionState sessionCtx;
    ZoneConfig config;
    int currentBar = 0;

    // ID generation (NOT reset on session roll - ensures unique IDs)
    int nextZoneId = 1;

    // SSOT anchor storage (MEDIUM-1)
    // All anchor access goes through this struct
    SessionAnchors anchors;

    // Legacy accessors for backward compatibility (delegate to anchors)
    // TODO: Eventually migrate call sites to use anchors directly
    int& vahId = anchors.vahId;
    int& valId = anchors.valId;
    int& pocId = anchors.pocId;
    int& vwapId = anchors.vwapId;
    int& ibHighId = anchors.ibHighId;
    int& ibLowId = anchors.ibLowId;

    // Prior session zone accessors
    int& priorPocId = anchors.priorPocId;
    int& priorVahId = anchors.priorVahId;
    int& priorValId = anchors.priorValId;

    // Structure zone accessors
    int& sessionHighId = anchors.sessionHighId;
    int& sessionLowId = anchors.sessionLowId;

    // ========================================================================
    // STRUCTURE TRACKER (SSOT for session extremes and IB levels)
    // Used for logging; NOT for zone selection (unless createStructureZones=true)
    // ========================================================================
    StructureTracker structure;

    // Cleanup tracking
    int barsSinceLastCleanup = 0;
    int cleanupIntervalBars = 100;

    // PERFORMANCE: Cached statistics (updated in UpdateZones, avoids per-bar loops)
    int cachedTotalTouches = 0;      // Sum of all zone touchCounts
    int cachedActiveZoneCount = 0;   // activeZones.size() cached

    // DEFENSE-IN-DEPTH: Posture rejection counter
    // Incremented when CreateZoneExplicit() rejects a zone type disallowed by posture
    // Non-zero indicates a call site bypassed the primary posture gate
    // RESET SEMANTICS: Per-session (reset in ResetForSession)
    int postureRejections = 0;

    // ZOMBIE DETECTION: Warn-once tracking for DEPARTED zones exceeding resolution thresholds
    // Zone IDs that have already triggered a zombie warning (cleared on session reset)
    // DIAGNOSTIC ONLY: No behavioral impact
    std::set<int> zombieWarnedIds;

    // ========================================================================
    // ZONE CREATION STATISTICS (Instrumented Invariant)
    // Tracks zone creation attempts and failures for health monitoring
    // ========================================================================
    struct CreationStats {
        int totalAttempts = 0;
        int totalSuccesses = 0;
        int totalFailures = 0;

        // Failure counts by reason (indexed by enum)
        // Uses kZoneCreationFailureCount to auto-size; static_assert prevents drift
        int failuresByReason[kZoneCreationFailureCount] = {0};
        static_assert(kZoneCreationFailureCount == 9,
            "ZoneCreationFailure enum changed - update failuresByReason handling if needed");

        void RecordAttempt(const ZoneCreationResult& result) {
            totalAttempts++;
            if (result.ok) {
                totalSuccesses++;
            } else {
                totalFailures++;
                int idx = static_cast<int>(result.failure);
                if (idx >= 0 && idx < 8) {
                    failuresByReason[idx]++;
                }
            }
        }

        int GetFailureCount(ZoneCreationFailure reason) const {
            int idx = static_cast<int>(reason);
            if (idx >= 0 && idx < 8) {
                return failuresByReason[idx];
            }
            return 0;
        }

        double GetSuccessRate() const {
            if (totalAttempts == 0) return 1.0;
            return static_cast<double>(totalSuccesses) / totalAttempts;
        }

        void Reset() {
            totalAttempts = 0;
            totalSuccesses = 0;
            totalFailures = 0;
            for (int i = 0; i < 8; i++) failuresByReason[i] = 0;
        }
    };
    CreationStats creationStats;

    // ========================================================================
    // PROXIMITY TRANSITION STATS (Instrumented Invariant 1)
    // Tracks state changes in the 4-state FSM for churn detection
    // ========================================================================
    struct TransitionStats {
        // Transition matrix: transitions[from][to] indexed by ZoneProximity enum
        // INACTIVE=0, APPROACHING=1, AT_ZONE=2, DEPARTED=3
        int transitions[4][4] = {{0}};
        int totalTransitions = 0;
        int totalBarsObserved = 0;

        // Oscillation tracking (rapid back-and-forth)
        int oscillationCount = 0;  // APPROACHING<->AT_ZONE cycles
        ZoneProximity lastFrom = ZoneProximity::INACTIVE;
        ZoneProximity lastTo = ZoneProximity::INACTIVE;

        /**
         * Record a state transition (only call when oldProx != newProx)
         * Zero overhead when no change - caller gates this.
         */
        void Record(ZoneProximity from, ZoneProximity to) {
            int f = static_cast<int>(from);
            int t = static_cast<int>(to);
            if (f >= 0 && f < 4 && t >= 0 && t < 4) {
                transitions[f][t]++;
                totalTransitions++;

                // Detect oscillation: A->B then B->A
                if (from == lastTo && to == lastFrom) {
                    oscillationCount++;
                }
                lastFrom = from;
                lastTo = to;
            }
        }

        void IncrementBars() { totalBarsObserved++; }

        int GetTransitionCount(ZoneProximity from, ZoneProximity to) const {
            int f = static_cast<int>(from);
            int t = static_cast<int>(to);
            if (f >= 0 && f < 4 && t >= 0 && t < 4) {
                return transitions[f][t];
            }
            return 0;
        }

        // Churn indicator: transitions per 100 bars
        double GetTransitionsPer100Bars() const {
            if (totalBarsObserved == 0) return 0.0;
            return (static_cast<double>(totalTransitions) / totalBarsObserved) * 100.0;
        }

        void Reset() {
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 4; j++) {
                    transitions[i][j] = 0;
                }
            }
            totalTransitions = 0;
            totalBarsObserved = 0;
            oscillationCount = 0;
            lastFrom = ZoneProximity::INACTIVE;
            lastTo = ZoneProximity::INACTIVE;
        }
    };
    TransitionStats transitionStats;

    // ========================================================================
    // RESOLUTION REASON HISTOGRAM (Instrumented Invariant 2)
    // Tracks resolution events by reason and policy mode
    // Only incremented on actual resolution (DEPARTED->INACTIVE)
    // ========================================================================
    struct ResolutionStats {
        // By reason: indexed by ResolutionReason enum (0-3)
        int byReason[4] = {0};

        // By policy mode: indexed by ResolutionMode enum (0-2)
        int byMode[3] = {0};

        // Cross-tabulation: [mode][reason]
        int byModeAndReason[3][4] = {{0}};

        int totalResolutions = 0;

        /**
         * Record a resolution event (only call when actually resolving)
         */
        void Record(ResolutionMode mode, ResolutionReason reason) {
            int m = static_cast<int>(mode);
            int r = static_cast<int>(reason);

            if (r >= 0 && r < 4) byReason[r]++;
            if (m >= 0 && m < 3) byMode[m]++;
            if (m >= 0 && m < 3 && r >= 0 && r < 4) {
                byModeAndReason[m][r]++;
            }
            totalResolutions++;
        }

        int GetReasonCount(ResolutionReason reason) const {
            int r = static_cast<int>(reason);
            return (r >= 0 && r < 4) ? byReason[r] : 0;
        }

        int GetModeCount(ResolutionMode mode) const {
            int m = static_cast<int>(mode);
            return (m >= 0 && m < 3) ? byMode[m] : 0;
        }

        void Reset() {
            for (int i = 0; i < 4; i++) byReason[i] = 0;
            for (int i = 0; i < 3; i++) byMode[i] = 0;
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 4; j++) {
                    byModeAndReason[i][j] = 0;
                }
            }
            totalResolutions = 0;
        }
    };
    ResolutionStats resolutionStats;

    // Resolution policy for DEPARTED->INACTIVE transitions (uses defaults)
    ResolutionPolicy resolution;

    // ========================================================================
    // PHASE 2: ENGAGEMENT CALLBACK
    // ========================================================================

    /**
     * Callback invoked when a zone engagement is finalized.
     * Set by caller (study) to push metrics to session baselines.
     * Data-only: logging happens at call site where sc is in scope.
     */
    EngagementFinalizedCallback onEngagementFinalized;

    /**
     * Zone IDs finalized this bar.
     * Used for logging at call site after update loop completes.
     * Cleared at start of each UpdateZones() call.
     */
    std::vector<int> finalizedThisBar;

    /**
     * Zone IDs that started engagement this bar (AT_ZONE transition).
     * Used for telemetry emission at call site after update loop completes.
     * Cleared at start of each UpdateZones() call.
     * TELEMETRY ONLY: Does not affect any behavioral logic.
     */
    std::vector<int> engagedThisBar;

    // ========================================================================
    // SESSION LIFECYCLE
    // ========================================================================

    /**
     * Force-finalize all pending engagements before zone destruction.
     *
     * INVARIANT: This MUST be called before any operation that destroys zones
     * (ResetForSession, activeZones.clear(), per-zone erase). Otherwise,
     * in-flight engagements are silently lost and not recorded to accumulators.
     *
     * @param bar     Current bar index (for finalization record)
     * @param time    Current bar time (for finalization record)
     * @param reason  Why engagements are being force-finalized
     * @return        Number of engagements finalized
     */
    int ForceFinalizePendingEngagements(int bar, SCDateTime time, UnresolvedReason reason) {
        int finalized = 0;
        for (auto& [id, zone] : activeZones) {
            if (zone.HasPendingEngagement()) {
                auto result = zone.ForceFinalize(bar, time, reason);
                if (result && onEngagementFinalized) {
                    onEngagementFinalized(zone, result);
                }
                finalized++;
            }
        }
        return finalized;
    }

    /**
     * Force-finalize a single zone's pending engagement before destruction.
     *
     * Use this when erasing individual zones (e.g., large POC jump retire).
     *
     * @param zoneId  ID of zone to finalize
     * @param bar     Current bar index
     * @param time    Current bar time
     * @param reason  Why engagement is being force-finalized
     * @return        true if an engagement was finalized
     */
    bool ForceFinalizeSingleZone(int zoneId, int bar, SCDateTime time, UnresolvedReason reason) {
        auto it = activeZones.find(zoneId);
        if (it == activeZones.end()) return false;

        ZoneRuntime& zone = it->second;
        if (!zone.HasPendingEngagement()) return false;

        auto result = zone.ForceFinalize(bar, time, reason);
        if (result && onEngagementFinalized) {
            onEngagementFinalized(zone, result);
        }
        return true;
    }

    // ========================================================================
    // DRY HELPER: ClearZonesOnly (Atomic Zone Clearing)
    // ========================================================================
    // Use this when you need to clear zones without full session reset.
    // Examples: Profile refresh, chart recalc, backfill scenarios.
    //
    // INVARIANT: Always finalizes pending engagements BEFORE clearing zones.
    // INVARIANT: Always resets anchors atomically with zone clearing.
    //
    // This is the ONLY correct way to clear zones (DRY: one implementation).
    // ========================================================================

    /**
     * Clear all zones atomically (finalize + clear + reset anchors).
     *
     * This is the DRY helper for zone clearing. Use this instead of manually:
     *   ForceFinalizePendingEngagements(...);
     *   activeZones.clear();
     *   anchors.Reset();
     *
     * @param bar     Current bar index (for finalization record)
     * @param time    Current bar time (for finalization record)
     * @param reason  Why zones are being cleared (for finalization logging)
     */
    void ClearZonesOnly(int bar, SCDateTime time, UnresolvedReason reason) {
        // CRITICAL: Force-finalize pending engagements BEFORE destroying zones
        // This ensures all engagement data is recorded to accumulators
        ForceFinalizePendingEngagements(bar, time, reason);

        // Clear all zones atomically with anchor reset
        activeZones.clear();
        anchors.Reset();
    }

    /**
     * Reset all state for a new session.
     * Caller MUST invoke this on session roll (RTH <-> Globex boundary).
     *
     * CRITICAL: This method force-finalizes all pending engagements BEFORE
     * clearing zones, ensuring no engagement data is lost.
     *
     * Resets:
     * - activeZones (cleared, after force-finalize)
     * - anchors (all set to -1)
     * - sessionCtx (all values reset)
     * - creationStats, transitionStats, resolutionStats (counters zeroed)
     * - currentBar, barsSinceLastCleanup, finalizedThisBar
     *
     * Does NOT reset:
     * - nextZoneId (ensures unique IDs across sessions)
     * - config (caller manages configuration separately)
     * - onEngagementFinalized (callback persists across sessions)
     *
     * @param bar   Current bar index (for finalization)
     * @param time  Current bar time (for finalization)
     */
    void ResetForSession(int bar, SCDateTime time) {
        // Use DRY helper for zone clearing
        ClearZonesOnly(bar, time, UnresolvedReason::SESSION_ROLL);

        // Reset session context
        sessionCtx.Reset();

        // Reset structure tracker (IB and session extremes)
        structure.Reset();

        // Reset statistics
        creationStats.Reset();
        transitionStats.Reset();
        resolutionStats.Reset();

        // Reset per-bar tracking
        currentBar = 0;
        barsSinceLastCleanup = 0;
        finalizedThisBar.clear();
        engagedThisBar.clear();  // TELEMETRY

        // Reset diagnostic counters (per-session)
        postureRejections = 0;
        zombieWarnedIds.clear();

        // NOTE: nextZoneId is NOT reset - ensures globally unique IDs
        // NOTE: config is NOT reset - caller manages configuration
        // NOTE: onEngagementFinalized is NOT reset - callback persists
    }

    // ========================================================================
    // CORE METHODS
    // ========================================================================

    /**
     * Get zone by ID (safe accessor)
     */
    ZoneRuntime* GetZone(int id) {
        auto it = activeZones.find(id);
        return (it != activeZones.end()) ? &it->second : nullptr;
    }

    const ZoneRuntime* GetZone(int id) const {
        auto it = activeZones.find(id);
        return (it != activeZones.end()) ? &it->second : nullptr;
    }

    /**
     * Quick accessors for major zones
     */
    ZoneRuntime* GetVAH() { return GetZone(vahId); }
    ZoneRuntime* GetVAL() { return GetZone(valId); }
    ZoneRuntime* GetPOC() { return GetZone(pocId); }

    // PERFORMANCE: Cached statistics (O(1) access, updated in UpdateZones)
    int GetTotalTouches() const { return cachedTotalTouches; }
    int GetActiveZoneCount() const { return cachedActiveZoneCount; }
    ZoneRuntime* GetVWAP() { return GetZone(vwapId); }
    ZoneRuntime* GetIBHigh() { return GetZone(ibHighId); }
    ZoneRuntime* GetIBLow() { return GetZone(ibLowId); }

    // Prior session zone accessors
    ZoneRuntime* GetPriorPOC() { return GetZone(priorPocId); }
    ZoneRuntime* GetPriorVAH() { return GetZone(priorVahId); }
    ZoneRuntime* GetPriorVAL() { return GetZone(priorValId); }

    // Structure zone accessors
    ZoneRuntime* GetSessionHighZone() { return GetZone(sessionHighId); }
    ZoneRuntime* GetSessionLowZone() { return GetZone(sessionLowId); }

    // ========================================================================
    // SESSION EXTREMES ACCESSORS (SSOT: StructureTracker)
    // These delegate to StructureTracker accessors for true bar-based extremes.
    // Phase logic and stats should use these, NOT sessionCtx.rth_high/low.
    // ========================================================================
    double GetSessionHigh() const { return structure.GetSessionHigh(); }
    double GetSessionLow() const { return structure.GetSessionLow(); }
    int GetSessionHighBar() const { return structure.GetSessionHighBar(); }
    int GetSessionLowBar() const { return structure.GetSessionLowBar(); }

    bool IsHighUpdatedRecently(int currentBar, int windowBars = 5) const {
        const int highBar = structure.GetSessionHighBar();
        return highBar >= 0 && (currentBar - highBar) < windowBars;
    }
    bool IsLowUpdatedRecently(int currentBar, int windowBars = 5) const {
        const int lowBar = structure.GetSessionLowBar();
        return lowBar >= 0 && (currentBar - lowBar) < windowBars;
    }
    bool IsExtremeUpdatedRecently(int currentBar, int windowBars = 5) const {
        return IsHighUpdatedRecently(currentBar, windowBars) ||
               IsLowUpdatedRecently(currentBar, windowBars);
    }

    // ========================================================================
    // ZONE RECENTERING (POC migration without clearing stats)
    // ========================================================================

    /**
     * Result of recenter operation for a zone type
     */
    struct RecenterOutcome {
        bool applied = false;        // Was recenter applied?
        bool latched = false;        // Was recenter latched for later?
        bool largeJump = false;      // Did jump exceed threshold (needs retire+create)?
        bool noZone = false;         // Was there no zone to recenter?
    };

    /**
     * Recenter the POC zone to a new price.
     * Preserves all stats (touches, engagements, etc.)
     * @param newPrice New POC price from VbP
     * @param tickSize Tick size for validation
     * @return RecenterOutcome indicating what happened
     */
    RecenterOutcome RecenterPOCEx(double newPrice, double tickSize) {
        RecenterOutcome outcome;
        ZoneRuntime* poc = GetPOC();
        if (!poc) {
            outcome.noZone = true;
            return outcome;
        }
        auto result = poc->RecenterEx(newPrice, tickSize);
        outcome.applied = (result == ZoneRuntime::RecenterResult::APPLIED);
        outcome.latched = (result == ZoneRuntime::RecenterResult::LATCHED_RECENTER ||
                          result == ZoneRuntime::RecenterResult::LATCHED_REPLACE);
        outcome.largeJump = (result == ZoneRuntime::RecenterResult::LARGE_JUMP ||
                            result == ZoneRuntime::RecenterResult::LATCHED_REPLACE);
        return outcome;
    }

    /**
     * Recenter the VAH zone to a new price.
     */
    RecenterOutcome RecenterVAHEx(double newPrice, double tickSize) {
        RecenterOutcome outcome;
        ZoneRuntime* vah = GetVAH();
        if (!vah) {
            outcome.noZone = true;
            return outcome;
        }
        auto result = vah->RecenterEx(newPrice, tickSize);
        outcome.applied = (result == ZoneRuntime::RecenterResult::APPLIED);
        outcome.latched = (result == ZoneRuntime::RecenterResult::LATCHED_RECENTER ||
                          result == ZoneRuntime::RecenterResult::LATCHED_REPLACE);
        outcome.largeJump = (result == ZoneRuntime::RecenterResult::LARGE_JUMP ||
                            result == ZoneRuntime::RecenterResult::LATCHED_REPLACE);
        return outcome;
    }

    /**
     * Recenter the VAL zone to a new price.
     */
    RecenterOutcome RecenterVALEx(double newPrice, double tickSize) {
        RecenterOutcome outcome;
        ZoneRuntime* val = GetVAL();
        if (!val) {
            outcome.noZone = true;
            return outcome;
        }
        auto result = val->RecenterEx(newPrice, tickSize);
        outcome.applied = (result == ZoneRuntime::RecenterResult::APPLIED);
        outcome.latched = (result == ZoneRuntime::RecenterResult::LATCHED_RECENTER ||
                          result == ZoneRuntime::RecenterResult::LATCHED_REPLACE);
        outcome.largeJump = (result == ZoneRuntime::RecenterResult::LARGE_JUMP ||
                            result == ZoneRuntime::RecenterResult::LATCHED_REPLACE);
        return outcome;
    }

    // Legacy wrappers (return true only if applied)
    bool RecenterPOC(double newPrice, double tickSize) { return RecenterPOCEx(newPrice, tickSize).applied; }
    bool RecenterVAH(double newPrice, double tickSize) { return RecenterVAHEx(newPrice, tickSize).applied; }
    bool RecenterVAL(double newPrice, double tickSize) { return RecenterVALEx(newPrice, tickSize).applied; }

    /**
     * Result of recenter operation for all anchors
     */
    struct RecenterAnchorsResult {
        int applied = 0;             // Number of zones recentered
        int latched = 0;             // Number of zones with latched recenter
        bool pocLargeJump = false;   // POC had large jump (needs retire+create)
        bool vahLargeJump = false;   // VAH had large jump
        bool valLargeJump = false;   // VAL had large jump

        bool anyLargeJump() const { return pocLargeJump || vahLargeJump || valLargeJump; }
    };

    /**
     * Recenter all anchor zones (POC/VAH/VAL) to new prices.
     * @return RecenterAnchorsResult with full status
     */
    RecenterAnchorsResult RecenterAnchorsEx(double newPoc, double newVah, double newVal, double tickSize) {
        RecenterAnchorsResult result;

        auto pocOut = RecenterPOCEx(newPoc, tickSize);
        if (pocOut.applied) result.applied++;
        if (pocOut.latched) result.latched++;
        result.pocLargeJump = pocOut.largeJump;

        auto vahOut = RecenterVAHEx(newVah, tickSize);
        if (vahOut.applied) result.applied++;
        if (vahOut.latched) result.latched++;
        result.vahLargeJump = vahOut.largeJump;

        auto valOut = RecenterVALEx(newVal, tickSize);
        if (valOut.applied) result.applied++;
        if (valOut.latched) result.latched++;
        result.valLargeJump = valOut.largeJump;

        return result;
    }

    // Legacy wrapper
    int RecenterAnchors(double newPoc, double newVah, double newVal, double tickSize) {
        return RecenterAnchorsEx(newPoc, newVah, newVal, tickSize).applied;
    }

    // ========================================================================
    // ZONE CREATION WITH VALIDATION (MEDIUM-0)
    // Returns explicit result - call sites MUST check before using zone ID
    // ========================================================================

    /**
     * Create new zone with auto-derived role, mechanism, and source (PREFERRED)
     * Returns ZoneCreationResult with validation - check .ok before using .zoneId
     *
     * Uses DeriveRoleFromType, DeriveMechanismFromType, DeriveSourceFromType
     * from AMT_Bridge.h to ensure consistency.
     */
    ZoneCreationResult CreateZone(ZoneType type, double anchor, SCDateTime time, int bar,
                                   bool isRTH = true) {
        ZoneRole role = DeriveRoleFromType(type);
        AnchorMechanism mechanism = DeriveMechanismFromType(type);
        ZoneSource source = DeriveSourceFromType(type, isRTH);

        return CreateZoneExplicit(type, role, mechanism, source, anchor, time, bar);
    }

    /**
     * Create new zone with explicit parameters (for legacy compatibility)
     * Returns ZoneCreationResult with validation - check .ok before using .zoneId
     *
     * WARNING: Prefer CreateZone(type, anchor, time, bar) for automatic derivation
     *
     * NOTE: All creation attempts are recorded in creationStats for health monitoring.
     */
    ZoneCreationResult CreateZoneExplicit(ZoneType type, ZoneRole role, AnchorMechanism mechanism,
                                           ZoneSource source, double anchor, SCDateTime time, int bar) {
        // Helper to record stats before returning
        auto recordAndReturn = [this](ZoneCreationResult result) -> ZoneCreationResult {
            creationStats.RecordAttempt(result);
            return result;
        };

        // =====================================================================
        // VALIDATION (MEDIUM-0): Explicit failure modes instead of silent corruption
        //
        // INVARIANT: All validation checks are NON-MUTATING except:
        //   - creationStats.RecordAttempt() (diagnostic only)
        //   - postureRejections++ (diagnostic only)
        // No ID allocation, anchor updates, or map inserts occur before validation passes.
        // =====================================================================

        // Check for invalid anchor price (zero, negative, NaN, Inf)
        if (anchor <= 0.0 || std::isnan(anchor) || std::isinf(anchor)) {
            return recordAndReturn(ZoneCreationResult::Failure(ZoneCreationFailure::INVALID_ANCHOR_PRICE));
        }

        // Check for invalid zone type
        if (type == ZoneType::NONE) {
            return recordAndReturn(ZoneCreationResult::Failure(ZoneCreationFailure::INVALID_ZONE_TYPE));
        }

        // DEFENSE-IN-DEPTH: Check posture allows this zone type
        // Primary gating is at CreateZonesFromProfile(); this is a safety net
        // Must be early in validation to reject disallowed types before any work
        if (!g_zonePosture.IsZoneTypeAllowed(type)) {
            postureRejections++;  // Diagnostic counter (only mutation on rejection path)
            return recordAndReturn(ZoneCreationResult::Failure(ZoneCreationFailure::POSTURE_DISALLOWED));
        }

        // Check for reasonable zone limit (prevent unbounded growth)
        constexpr size_t MAX_ACTIVE_ZONES = 100;
        if (activeZones.size() >= MAX_ACTIVE_ZONES) {
            return recordAndReturn(ZoneCreationResult::Failure(ZoneCreationFailure::MAX_ZONES_EXCEEDED));
        }

        // Check for duplicate anchor (tick-based comparison - SSOT)
        // Use canonical PriceToTicks for consistent comparison
        const long long newAnchorTicks = PriceToTicks(anchor, config.tickSize);
        for (const auto& [existingId, existingZone] : activeZones) {
            if (existingZone.GetAnchorTicks() == newAnchorTicks &&
                existingZone.type == type) {
                return recordAndReturn(ZoneCreationResult::Failure(ZoneCreationFailure::DUPLICATE_ANCHOR));
            }
        }

        // =====================================================================
        // CREATION (all validations passed)
        // =====================================================================
        int id = nextZoneId++;

        // Pass tickSize from config so ZoneRuntime can store anchorTicks (SSOT)
        const double tickSizeForZone = (config.tickSize > 0.0) ? config.tickSize : 0.25;
        ZoneRuntime zone(id, type, role, mechanism, source, anchor, time, bar,
                         config.GetHaloWidth(), tickSizeForZone);
        zone.coreWidthTicks = config.GetCoreWidth();
        zone.haloWidthTicks = config.GetHaloWidth();

        activeZones.emplace(id, std::move(zone));

        return recordAndReturn(ZoneCreationResult::Success(id));
    }

    /**
     * Remove zone (MEDIUM-1: atomically clears matching anchors)
     *
     * IMPORTANT: This method does NOT force-finalize pending engagements.
     * Callers MUST call ForceFinalizeSingleZone() before RemoveZone() if
     * engagement preservation is required. For bulk removals, use
     * ResetForSession() which handles force-finalization internally.
     *
     * @param id Zone ID to remove
     */
    void RemoveZone(int id) {
        // SSOT: Clear any anchors pointing to this zone before erasing
        anchors.ClearIfMatches(id);
        activeZones.erase(id);
    }

    /**
     * Validate anchor integrity (debug assertion)
     * All non-negative anchor IDs must exist in activeZones.
     */
    bool ValidateAnchors() const {
        return anchors.ValidateAgainstZones(activeZones);
    }

    // ========================================================================
    // ZONE SELECTION THRESHOLD (SSOT: aligned with halo for consistency)
    // ========================================================================
    //
    // CONTRACT A: Zone selection uses the same halo threshold as proximity FSM.
    // This ensures ZONE=NONE means "no profile anchor in halo," consistent with
    // the FSM meaning of INACTIVE (price > haloWidthTicks from anchor).
    //
    // Rationale: If proximity says APPROACHING (within halo), zone selection
    // should return that zone. Using a smaller fixed tolerance would cause
    // confusing states where proximity=APPROACHING but ZONE=NONE.
    //
    // ========================================================================

    /**
     * Get selection tolerance (SSOT: equals halo width)
     * All zone selection functions should use this for consistency.
     */
    int GetSelectionTolerance() const {
        return config.GetHaloWidth();
    }

    /**
     * Get strongest zone at price (deterministic tie-breaking)
     * Uses halo-based tolerance by default for consistency with proximity FSM.
     */
    ZoneRuntime* GetStrongestZoneAtPrice(double price, double tickSize,
                                         int toleranceTicks = -1);  // -1 = use halo

    /**
     * Get strongest zone at price with sticky preference (E)
     * If preferred zone is valid and within tolerance, it wins
     * Uses halo-based tolerance by default for consistency with proximity FSM.
     */
    ZoneRuntime* GetStrongestZoneAtPriceSticky(double price, double tickSize,
                                               const ZoneTransitionMemory& memory,
                                               int currentBar,
                                               int toleranceTicks = -1);  // -1 = use halo

    /**
     * Get description string for nearest zone at price
     * Returns "TYPE(PROXIMITY)" or "NONE" if no zone nearby
     * Uses halo-based tolerance by default (Contract A: aligned with FSM).
     */
    std::string GetNearestZoneDescription(double price, double tickSize, int toleranceTicks = -1) {
        const int effectiveTol = (toleranceTicks < 0) ? GetSelectionTolerance() : toleranceTicks;
        ZoneRuntime* nearest = GetStrongestZoneAtPrice(price, tickSize, effectiveTol);
        if (!nearest) return "NONE";
        return std::string(ZoneTypeToString(nearest->type)) + "(" + ZoneProximityToString(nearest->proximity) + ")";
    }

    /**
     * Count zones at price (for confluence detection)
     * Uses halo-based tolerance by default for consistency.
     */
    int CountZonesAtPrice(double price, double tickSize, int toleranceTicks = -1) const;

    /**
     * Count zones approaching (in halo but not at core)
     */
    int CountZonesApproaching(double price, double tickSize) const;

    /**
     * Cleanup expired zones
     * @param bar Current bar index (for ForceFinalize)
     * @param time Current bar time (for ForceFinalize)
     */
    void CleanupExpiredZones(int bar, SCDateTime time);

    /**
     * Update all zones (called every bar)
     * NO STATIC LOCALS - all state via parameters
     * @param sc Sierra Chart interface for logging (diagLevel >= 2)
     * @param diagLevel Diagnostic level: 2=ENGAGE/EXIT edges, 3=all PROX transitions
     */
    void UpdateZones(double currentPrice, double tickSize, int bar, SCDateTime time,
                     SCStudyInterfaceRef sc, int diagLevel);

    /**
     * Update all proximities and build context snapshot (A1, C)
     * Main entry point that replaces any static-local patterns
     *
     * @param currentPrice Current price to check proximity
     * @param tickSize Tick size for distance calculations
     * @param bar Current bar index
     * @param time Current bar time
     * @param transitionState Per-chart transition tracking (passed by reference)
     * @param transitionMemory Per-chart sticky zone memory (passed by reference)
     * @param resolution Resolution policy for bar+time based resolution
     * @param snapshot Output: filled with current context
     * @param sc Sierra Chart interface for logging
     * @param diagLevel Diagnostic level for logging
     */
    void UpdateAllProximities(double currentPrice, double tickSize, int bar,
                             SCDateTime time, TransitionState& transitionState,
                             ZoneTransitionMemory& transitionMemory,
                             const ResolutionPolicy& resolution,
                             ZoneContextSnapshot& snapshot,
                             SCStudyInterfaceRef sc, int diagLevel);

    /**
     * Build context snapshot for early-exit optimization (C)
     * Even when price is far from all zones, this ensures:
     * - Transition detection runs against prior state
     * - Exit transitions trigger engagement finalization
     * - TransitionState is updated consistently
     */
    void BuildContextSnapshotEarlyExit(double currentPrice, double tickSize, int bar,
                                       SCDateTime time, TransitionState& transitionState,
                                       ZoneContextSnapshot& snapshot);
};

// ============================================================================
// HELPER FUNCTIONS (Implementation below)
// ============================================================================

// Zone priority computation
ZonePriorityExtended GetZonePriorityExtended(const ZoneRuntime& zone,
                                              double currentPrice,
                                              double tickSize);

// Proximity update
void UpdateZoneProximity(ZoneRuntime& zone, double currentPrice,
                        double tickSize, const ZoneConfig& cfg);

// Touch classification
TouchType ClassifyTouch(const EngagementMetrics& engagement,
                       const ZoneRuntime& zone,
                       const ZoneConfig& cfg);

// Strength calculation
double CalculateStrengthScore(const ZoneRuntime& zone, int currentBar);
ZoneStrength ClassifyStrength(double score, int touchCount);

// Value area region calculation (takes vah/val from SessionManager, not from ZoneSessionState)
ValueAreaRegion CalculateVARegion(double price, double vah, double val);

// ============================================================================
// IMPLEMENTATION
// ============================================================================

/**
 * Compute extended priority (includes tie-breakers)
 */
inline ZonePriorityExtended GetZonePriorityExtended(
    const ZoneRuntime& zone,
    double currentPrice,
    double tickSize)
{
    ZonePriorityExtended p;

    // Primary (lexicographic) - matches role enum values
    p.role = static_cast<int>(zone.role);

    // Source
    p.source = static_cast<int>(zone.source);

    // Strength
    p.strength = static_cast<int>(zone.strengthTier);

    // Tie-breakers (use EXACT distance for precision)
    p.distanceTicks = GetExactTickDistance(currentPrice, zone.GetAnchorPrice(), tickSize);
    p.lastTouchBar = zone.lastTouchBar;
    p.zoneId = zone.zoneId;

    return p;
}

/**
 * Get strongest zone at price (DETERMINISTIC)
 * CONTRACT A: Default tolerance = haloWidthTicks (aligned with proximity FSM)
 */
inline ZoneRuntime* ZoneManager::GetStrongestZoneAtPrice(
    double price,
    double tickSize,
    int toleranceTicks)
{
    // SSOT: Use halo width if no explicit tolerance provided
    const int effectiveTol = (toleranceTicks < 0) ? config.GetHaloWidth() : toleranceTicks;

    std::vector<ZoneRuntime*> overlapping;

    // Find all zones within tolerance (uses CEIL)
    for (auto& [id, zone] : activeZones) {
        int distCeil = GetTickDistanceForOverlap(price, zone.GetAnchorPrice(), tickSize);
        if (distCeil <= effectiveTol) {
            overlapping.push_back(&zone);
        }
    }

    if (overlapping.empty()) return nullptr;
    if (overlapping.size() == 1) return overlapping[0];

    // Find max with extended priority (deterministic)
    return *std::max_element(overlapping.begin(), overlapping.end(),
        [price, tickSize](ZoneRuntime* a, ZoneRuntime* b) {
            auto prioA = GetZonePriorityExtended(*a, price, tickSize);
            auto prioB = GetZonePriorityExtended(*b, price, tickSize);
            return prioA < prioB;  // max_element selects where this is FALSE
        });
}

/**
 * Get strongest zone at price with sticky preference (E)
 * CONTRACT A: Default tolerance = haloWidthTicks (aligned with proximity FSM)
 */
inline ZoneRuntime* ZoneManager::GetStrongestZoneAtPriceSticky(
    double price, double tickSize,
    const ZoneTransitionMemory& memory,
    int currentBar,
    int toleranceTicks)
{
    // SSOT: Use halo width if no explicit tolerance provided
    const int effectiveTol = (toleranceTicks < 0) ? config.GetHaloWidth() : toleranceTicks;

    // Check if we have an active preference
    int preferredId = memory.GetPreferredIfValid(currentBar);

    if (preferredId >= 0) {
        // Check if preferred zone is still valid and within tolerance
        ZoneRuntime* preferred = GetZone(preferredId);
        if (preferred) {
            int distCeil = GetTickDistanceForOverlap(price, preferred->GetAnchorPrice(), tickSize);
            // Preferred zone wins if it's AT_ZONE or APPROACHING
            if (preferred->proximity == ZoneProximity::AT_ZONE ||
                preferred->proximity == ZoneProximity::APPROACHING) {
                if (distCeil <= effectiveTol) {
                    return preferred;
                }
            }
        }
    }

    // Fall back to normal priority selection
    return GetStrongestZoneAtPrice(price, tickSize, effectiveTol);
}

/**
 * Count zones at price
 * CONTRACT A: Default tolerance = haloWidthTicks (aligned with proximity FSM)
 */
inline int ZoneManager::CountZonesAtPrice(double price, double tickSize,
                                         int toleranceTicks) const
{
    // SSOT: Use halo width if no explicit tolerance provided
    const int effectiveTol = (toleranceTicks < 0) ? config.GetHaloWidth() : toleranceTicks;

    int count = 0;
    for (const auto& [id, zone] : activeZones) {
        int distCeil = GetTickDistanceForOverlap(price, zone.GetAnchorPrice(), tickSize);
        if (distCeil <= effectiveTol) {
            count++;
        }
    }
    return count;
}

/**
 * Count zones approaching (in halo but not at core)
 */
inline int ZoneManager::CountZonesApproaching(double price, double tickSize) const
{
    int count = 0;
    for (const auto& [id, zone] : activeZones) {
        if (zone.proximity == ZoneProximity::APPROACHING) {
            count++;
        }
    }
    return count;
}

/**
 * Update zone proximity using INTEGER TICK comparisons.
 *
 * Implements the 4-state FSM:
 *   INACTIVE <-> APPROACHING <-> AT_ZONE -> DEPARTED -> INACTIVE
 *
 * DEPARTED is reached when:
 *   - Prior state was AT_ZONE
 *   - Price has now exited the halo
 *   - Acts as transient "cooling off" state before full INACTIVE
 *
 * Note: DEPARTED -> INACTIVE transition is handled by resolution timer,
 * not by this function. This function only handles distance-based transitions.
 *
 * BOUNDARY FLICKER PREVENTION:
 * - All distance comparisons use INTEGER ticks (not floating-point)
 * - Prices are rounded to ticks using canonical PriceToTicks (std::llround)
 * - Thresholds are integer tick counts
 * - This eliminates epsilon-induced oscillation at exact boundaries
 */
inline void UpdateZoneProximity(ZoneRuntime& zone, double currentPrice,
                                double tickSize, const ZoneConfig& cfg)
{
    // =========================================================================
    // INTEGER TICK COMPARISON (eliminates floating-point boundary flicker)
    // Both prices converted to integer ticks using canonical rounding policy
    // =========================================================================
    const long long priceTicks = PriceToTicks(currentPrice, tickSize);
    const long long anchorTicks = zone.GetAnchorTicks();  // Already in ticks (SSOT)
    const long long distTicks = std::abs(priceTicks - anchorTicks);

    // Integer thresholds (no casting, no epsilon issues)
    const int coreWidthTicks = cfg.GetCoreWidth();
    const int haloWidthTicks = cfg.GetHaloWidth();

    // HYSTERESIS: 1-tick buffer between enter and exit thresholds
    // Enter AT_ZONE at coreWidthTicks, exit AT_ZONE at coreWidthTicks + 1
    // This prevents oscillation when price hovers at exact boundary
    // INVARIANT: coreExitTicks must never exceed haloWidthTicks to preserve
    // DEPARTED/INACTIVE reachability when price exits the halo
    const int coreExitTicks = (std::min)(coreWidthTicks + 1, haloWidthTicks);

    zone.priorProximity = zone.proximity;

    // Track boundary hits for diagnostics (price exactly at core threshold)
    if (distTicks == coreWidthTicks || distTicks == coreExitTicks) {
        zone.proximityBoundaryHits++;
    }

    // Compute raw proximity based on INTEGER distance with HYSTERESIS
    ZoneProximity rawProximity;
    if (zone.proximity == ZoneProximity::AT_ZONE) {
        // Currently AT_ZONE: use EXIT threshold (core + 1) to stay in zone longer
        if (distTicks <= coreExitTicks) {
            rawProximity = ZoneProximity::AT_ZONE;
        } else if (distTicks <= haloWidthTicks) {
            rawProximity = ZoneProximity::APPROACHING;
        } else {
            rawProximity = ZoneProximity::INACTIVE;
        }
    } else {
        // Not AT_ZONE: use ENTER threshold (core) for initial entry
        if (distTicks <= coreWidthTicks) {
            rawProximity = ZoneProximity::AT_ZONE;
        } else if (distTicks <= haloWidthTicks) {
            rawProximity = ZoneProximity::APPROACHING;
        } else {
            rawProximity = ZoneProximity::INACTIVE;
        }
    }

    // Apply 4-state FSM rules:
    // 1. If re-entering zone from any state, go to computed state
    if (rawProximity == ZoneProximity::AT_ZONE || rawProximity == ZoneProximity::APPROACHING) {
        zone.proximity = rawProximity;
    }
    // 2. If was AT_ZONE and now exiting halo -> DEPARTED (not INACTIVE)
    else if (zone.priorProximity == ZoneProximity::AT_ZONE && rawProximity == ZoneProximity::INACTIVE) {
        zone.proximity = ZoneProximity::DEPARTED;
    }
    // 3. If already DEPARTED and still outside -> stay DEPARTED
    //    (resolution timer will move to INACTIVE)
    else if (zone.priorProximity == ZoneProximity::DEPARTED && rawProximity == ZoneProximity::INACTIVE) {
        zone.proximity = ZoneProximity::DEPARTED;
    }
    // 4. All other cases: use raw proximity (INACTIVE -> INACTIVE, etc.)
    else {
        zone.proximity = rawProximity;
    }
}

/**
 * Classify touch type (deterministic rules)
 */
inline TouchType ClassifyTouch(const EngagementMetrics& engagement,
                               const ZoneRuntime& zone,
                               const ZoneConfig& cfg)
{
    int bars = engagement.barsEngaged;
    int penetration = engagement.peakPenetrationTicks;
    AuctionOutcome outcome = engagement.outcome;

    // TAG: Brief contact, no penetration
    if (bars <= 2 && penetration <= cfg.GetCoreWidth()) {
        return TouchType::TAG;
    }

    // PROBE: Penetrated beyond core, quick rejection
    if (penetration > cfg.GetCoreWidth() &&
        bars <= 5 &&
        outcome == AuctionOutcome::REJECTED) {
        return TouchType::PROBE;
    }

    // TEST: Handled by default fallthrough below
    // (bars > 2, not PROBE, not ACCEPTANCE → TEST)
    // Note: Explicit TEST rule removed (was unreachable: bars > 5 AND bars < 6)

    // ACCEPTANCE: Met acceptance criteria and held
    if (bars >= cfg.acceptanceMinBars &&
        outcome == AuctionOutcome::ACCEPTED) {
        return TouchType::ACCEPTANCE;
    }

    // Default
    return TouchType::TEST;
}

/**
 * Calculate strength score
 */
inline double CalculateStrengthScore(const ZoneRuntime& zone, int currentBar)
{
    // Base structural weight
    double baseWeight = 1.0;
    switch (zone.role) {
        case ZoneRole::VALUE_BOUNDARY: baseWeight = 1.6; break;
        case ZoneRole::VALUE_CORE:     baseWeight = 1.4; break;
        case ZoneRole::RANGE_BOUNDARY: baseWeight = 1.2; break;
        case ZoneRole::MEAN_REFERENCE: baseWeight = 1.0; break;
    }

    // Volume boost (if available)
    double volumeBoost = 1.0;
    if (zone.levelProfile.volumeRatio > 0.0) {
        volumeBoost = 0.5 + 0.5 * zone.levelProfile.volumeRatio;
    }

    // Touch decay (based on touch history)
    double touchDecay = 1.0;
    for (const auto& touch : zone.touchHistory) {
        switch (touch.type) {
            case TouchType::TAG:        touchDecay *= 0.95; break;
            case TouchType::PROBE:      touchDecay *= 0.90; break;
            case TouchType::TEST:       touchDecay *= 0.80; break;
            case TouchType::ACCEPTANCE: touchDecay *= 0.60; break;
            case TouchType::UNRESOLVED: touchDecay *= 0.98; break;  // Minimal decay - engagement never completed
        }
    }

    // Age decay
    int age = (zone.lastTouchBar >= 0) ?
              (currentBar - zone.lastTouchBar) :
              (currentBar - zone.creationBar);
    double ageDecay = std::exp(-age / 300.0);

    return baseWeight * volumeBoost * touchDecay * ageDecay;
}

/**
 * Classify strength tier
 */
inline ZoneStrength ClassifyStrength(double score, int touchCount)
{
    if (touchCount == 0) return ZoneStrength::VIRGIN;
    if (score > 1.2)     return ZoneStrength::STRONG;
    if (score >= 0.8)    return ZoneStrength::MODERATE;
    if (score >= 0.5)    return ZoneStrength::WEAK;
    return ZoneStrength::EXPIRED;
}

/**
 * Calculate value area region
 * @param price Current price
 * @param vah Value Area High (from SessionManager)
 * @param val Value Area Low (from SessionManager)
 */
inline ValueAreaRegion CalculateVARegion(double price, double vah, double val)
{
    if (vah == 0.0 || val == 0.0 || vah <= val) {
        return ValueAreaRegion::CORE_VA;  // Default if VA not set or invalid
    }

    if (price > vah) {
        return ValueAreaRegion::OUTSIDE_ABOVE;
    } else if (price < val) {
        return ValueAreaRegion::OUTSIDE_BELOW;
    }

    // Inside value area - vaRange guaranteed > 0 by check above
    double vaRange = vah - val;
    double positionInVA = (price - val) / vaRange;

    if (positionInVA > 0.70) {
        return ValueAreaRegion::UPPER_VA;
    } else if (positionInVA < 0.30) {
        return ValueAreaRegion::LOWER_VA;
    } else {
        return ValueAreaRegion::CORE_VA;
    }
}

/**
 * Finalize engagement (when leaving zone)
 * Returns immutable FinalizationResult - caller uses result.metrics for callbacks.
 *
 * INVARIANT: After this returns, currentEngagement is reset.
 *            Caller must NEVER use zone.currentEngagement after calling this.
 *            Use result.metrics instead.
 */
inline FinalizationResult ZoneRuntime::FinalizeEngagement(int bar, SCDateTime time,
                                                          double exitPrice, double tickSize,
                                                          const ZoneConfig& cfg)
{
    // Guard: no pending engagement - nothing to finalize
    if (!HasPendingEngagement()) {
        return FinalizationResult::None();
    }

    // Set end time and compute escape velocity
    // Returns false if already finalized (exactly-once guard)
    if (!currentEngagement.Finalize(bar, time, exitPrice, tickSize)) {
        return FinalizationResult::None();  // Already finalized
    }

    // --- FORCE TERMINAL STATE (but don't update recency trackers yet) ---
    // If outcome is still PENDING at finalize, it means acceptance criteria
    // were not met. This is semantically a rejection (soft or hard).
    if (currentEngagement.outcome == AuctionOutcome::PENDING) {
        currentEngagement.outcome = AuctionOutcome::REJECTED;
        // NOTE: Do NOT update lastRejectionBar here - defer until after classification
    }

    // Classify touch type (outcome now guaranteed terminal)
    TouchType touchType = ClassifyTouch(currentEngagement, *this, cfg);

    // --- UPDATE RECENCY TRACKERS (meaningful outcomes only) ---
    switch (touchType) {
        case TouchType::PROBE:
        case TouchType::TEST:
            lastRejectionBar = bar;
            break;
        case TouchType::ACCEPTANCE:
            lastAcceptanceBar = bar;
            break;
        case TouchType::TAG:
        case TouchType::UNRESOLVED:
            // Noise and unresolved do NOT update recency trackers
            break;
        default:
            // Future rejection subtypes should update lastRejectionBar
            if (currentEngagement.outcome == AuctionOutcome::REJECTED) {
                lastRejectionBar = bar;
            }
            break;
    }

    // --- SSOT COUNTER INCREMENT ---
    switch (touchType) {
        case TouchType::TAG:
            lifetimeTags++;
            break;
        case TouchType::PROBE:
            lifetimeRejections++;
            lifetimeProbes++;
            break;
        case TouchType::TEST:
            lifetimeRejections++;
            lifetimeTests++;
            break;
        case TouchType::ACCEPTANCE:
            lifetimeAcceptances++;
            break;
        case TouchType::UNRESOLVED:
            // Should not happen via normal finalize
            lifetimeUnresolved++;
            break;
        default:
            // Future rejection subtypes
            if (currentEngagement.outcome == AuctionOutcome::REJECTED) {
                lifetimeRejections++;
                lifetimeRejectionsOther++;
            }
            break;
    }

    // Create frozen TouchRecord (explicit field assignment, no reliance on defaults)
    TouchRecord record;
    record.touchNumber = touchCount;
    record.type = touchType;
    record.barsEngaged = currentEngagement.barsEngaged;
    record.penetrationTicks = currentEngagement.peakPenetrationTicks;
    record.outcome = currentEngagement.outcome;
    record.timestamp = currentEngagement.endTime;
    record.unresolvedReason = UnresolvedReason::NONE;  // Normal finalization

    // --- COHERENCE CHECK (debug) ---
#ifdef _DEBUG
    bool coherent = false;
    switch (record.type) {
        case TouchType::ACCEPTANCE:
            coherent = (record.outcome == AuctionOutcome::ACCEPTED);
            break;
        case TouchType::TAG:
        case TouchType::PROBE:
        case TouchType::TEST:
            coherent = (record.outcome == AuctionOutcome::REJECTED);
            break;
        case TouchType::UNRESOLVED:
            coherent = (record.outcome == AuctionOutcome::PENDING);
            break;
        default:
            coherent = false;
            break;
    }
    if (!coherent) {
        assert(false && "TouchRecord outcome/type coherence violated");
    }
#endif

    // Store records with bounded history (ring buffer behavior)
    if (touchHistory.size() >= MAX_TOUCH_HISTORY) {
        touchHistory.erase(touchHistory.begin());
    }
    touchHistory.push_back(record);

    if (engagementHistory.size() >= MAX_ENGAGEMENT_HISTORY) {
        engagementHistory.erase(engagementHistory.begin());
    }
    engagementHistory.push_back(currentEngagement);

    // =========================================================================
    // CAPTURE IMMUTABLE SNAPSHOT BEFORE RESET
    // This is the ONLY point where we capture metrics for the return value.
    // After this, currentEngagement will be reset and must NOT be used.
    // =========================================================================
    EngagementMetrics finalizedMetrics = currentEngagement;  // Copy BEFORE reset

    // Update strength
    strengthScore = CalculateStrengthScore(*this, bar);
    strengthTier = ClassifyStrength(strengthScore, touchCount);

    // Mark when we left the zone
    lastOutsideBar = bar;
    lastOutsideTime = time;

    // Reset current engagement so HasPendingEngagement() returns false
    currentEngagement.Reset();

    // Apply any pending action that was latched during engagement
    // (engagement is now finalized, safe to move anchor or signal retire)
    // NOTE: If result is REPLACE_NEEDED, the zone should be retired by caller
    // The pendingReplaceNeeded flag will be set for caller to check
    if (pendingAction != PendingAction::NONE) {
        PendingApplyResult applyResult = ApplyPendingAction(tickSize);
        pendingReplaceNeeded = (applyResult == PendingApplyResult::REPLACE_NEEDED);
    }

    // Return immutable snapshot - caller uses this for callbacks
    return FinalizationResult::Success(finalizedMetrics, record);
}

/**
 * Cleanup expired zones
 * @param bar Current bar index (for ForceFinalize)
 * @param time Current bar time (for ForceFinalize)
 */
inline void ZoneManager::CleanupExpiredZones(int bar, SCDateTime time)
{
    auto it = activeZones.begin();
    while (it != activeZones.end()) {
        ZoneRuntime& zone = it->second;  // Non-const to allow ForceFinalize

        // Removal eligibility: lifecycle state only (EXPIRED && INACTIVE)
        // Do NOT gate on outcome != PENDING (would strand pending zones)
        bool shouldRemove = (zone.strengthTier == ZoneStrength::EXPIRED &&
                            zone.proximity == ZoneProximity::INACTIVE);

        // CRITICAL: Never remove anchor zones (POC/VAH/VAL/VWAP/IB)
        // Anchor zones represent session-level market structure and must persist
        if (shouldRemove && anchors.ReferencesZone(zone.zoneId)) {
            shouldRemove = false;  // Protect anchor zones from cleanup
        }

        if (shouldRemove) {
            // INVARIANT: Force-finalize pending engagement and invoke callback
            // to record to accumulators BEFORE erasing zone
            if (zone.HasPendingEngagement()) {
                auto result = zone.ForceFinalize(bar, time, UnresolvedReason::ZONE_EXPIRY);
                if (result && onEngagementFinalized) {
                    onEngagementFinalized(zone, result);
                }
            }
            it = activeZones.erase(it);
        } else {
            ++it;
        }
    }
}

/**
 * Force-finalize engagement (unresolved outcome)
 * Creates TouchRecord with UNRESOLVED type and reason code
 */
inline FinalizationResult ZoneRuntime::ForceFinalize(int bar, SCDateTime time, UnresolvedReason reason)
{
    // No pending engagement - nothing to force-finalize
    if (!HasPendingEngagement()) {
        return FinalizationResult::None();
    }

    // Capture engagement data before reset
    EngagementMetrics snapshot = currentEngagement;
    snapshot.endBar = bar;
    snapshot.endTime = time;

    // Increment SSOT counter
    lifetimeUnresolved++;

    // Create frozen record with UNRESOLVED type (explicit field assignment)
    TouchRecord record;
    record.touchNumber = touchCount;
    record.type = TouchType::UNRESOLVED;
    record.barsEngaged = (bar >= snapshot.startBar) ? (bar - snapshot.startBar + 1) : 0;
    record.penetrationTicks = snapshot.peakPenetrationTicks;
    record.outcome = AuctionOutcome::PENDING;  // Never resolved
    record.timestamp = time;
    record.unresolvedReason = reason;

    // --- COHERENCE CHECK (debug) ---
#ifdef _DEBUG
    // UNRESOLVED must have PENDING outcome
    if (record.type != TouchType::UNRESOLVED || record.outcome != AuctionOutcome::PENDING) {
        assert(false && "ForceFinalize coherence violated");
    }
#endif

    // Store in history (ring buffer)
    if (touchHistory.size() >= MAX_TOUCH_HISTORY) {
        touchHistory.erase(touchHistory.begin());
    }
    touchHistory.push_back(record);

    // Store engagement metrics (ring buffer)
    if (engagementHistory.size() >= MAX_ENGAGEMENT_HISTORY) {
        engagementHistory.erase(engagementHistory.begin());
    }
    engagementHistory.push_back(snapshot);

    // Reset current engagement
    currentEngagement.Reset();

    return FinalizationResult::Success(snapshot, record);
}

/**
 * Update all zones (called every bar)
 * Logs [ZONE-PROX] at diagLevel >= 2 for engagement edges, >= 3 for all transitions
 */
inline void ZoneManager::UpdateZones(double currentPrice, double tickSize,
                                    int bar, SCDateTime time,
                                    SCStudyInterfaceRef sc, int diagLevel)
{
    currentBar = bar;

    // Phase 2: Clear finalized/engaged lists at start of each update cycle
    finalizedThisBar.clear();
    engagedThisBar.clear();  // TELEMETRY: Track new engagements this bar

    // Increment bar counter for churn metrics
    transitionStats.IncrementBars();

    // PERFORMANCE: Accumulate stats during single loop pass
    int touchAccum = 0;

    // Update proximity for all zones
    for (auto& [id, zone] : activeZones) {
        touchAccum += zone.touchCount;  // Accumulate touches (O(1) per zone)
        ZoneProximity priorProx = zone.proximity;  // Capture before update
        UpdateZoneProximity(zone, currentPrice, tickSize, config);

        // Record transition only if state changed (zero overhead otherwise)
        if (zone.proximity != priorProx) {
            transitionStats.Record(priorProx, zone.proximity);

            // [ZONE-PROX] PROX transition log (diagLevel >= 3)
            if (diagLevel >= 3) {
                const long long priceTicks = PriceToTicks(currentPrice, tickSize);
                const long long anchorTicks = zone.GetAnchorTicks();
                const int distTicks = static_cast<int>(std::abs(priceTicks - anchorTicks));
                const int coreW = config.GetCoreWidth();
                const int haloW = config.GetHaloWidth();
                const int coreExit = (std::min)(coreW + 1, haloW);
                SCString msg;
                msg.Format("[ZONE-PROX] TRANSITION bar=%d id=%d %s dist=%dt core=%d exit=%d halo=%d %s->%s hits=%d",
                    bar, zone.zoneId, ZoneTypeToString(zone.type), distTicks,
                    coreW, coreExit, haloW,
                    ZoneProximityToString(priorProx), ZoneProximityToString(zone.proximity),
                    zone.proximityBoundaryHits);
                sc.AddMessageToLog(msg, 0);
            }
        }

        // Update bars since touch
        if (zone.lastTouchBar >= 0) {
            zone.barsSinceTouch = bar - zone.lastTouchBar;
        }

        // A2: Update per-zone inside/outside tracking
        bool isInsideHalo = (zone.proximity == ZoneProximity::AT_ZONE ||
                            zone.proximity == ZoneProximity::APPROACHING);
        zone.UpdateInsideOutsideTracking(bar, time, isInsideHalo);

        // Detect engagement transitions
        bool wasAtZone = (zone.priorProximity == ZoneProximity::AT_ZONE);
        bool nowAtZone = (zone.proximity == ZoneProximity::AT_ZONE);

        if (nowAtZone && !wasAtZone) {
            // Entering zone - pass currentPrice for entryPrice
            zone.StartEngagement(bar, time, currentPrice);

            // TELEMETRY: Record zone ID for telemetry emission at call site
            engagedThisBar.push_back(zone.zoneId);

            // [ZONE-PROX] ENGAGE edge log (diagLevel >= 2)
            if (diagLevel >= 2) {
                const long long priceTicks = PriceToTicks(currentPrice, tickSize);
                const long long anchorTicks = zone.GetAnchorTicks();
                const int distTicks = static_cast<int>(std::abs(priceTicks - anchorTicks));
                const int coreW = config.GetCoreWidth();
                const int haloW = config.GetHaloWidth();
                const int coreExit = (std::min)(coreW + 1, haloW);
                SCString msg;
                msg.Format("[ZONE-PROX] ENGAGE bar=%d id=%d %s dist=%dt core=%d exit=%d halo=%d %s->%s hits=%d",
                    bar, zone.zoneId, ZoneTypeToString(zone.type), distTicks,
                    coreW, coreExit, haloW,
                    ZoneProximityToString(zone.priorProximity), ZoneProximityToString(zone.proximity),
                    zone.proximityBoundaryHits);
                sc.AddMessageToLog(msg, 0);
            }
        } else if (wasAtZone && !nowAtZone) {
            // [ZONE-PROX] EXIT edge log (diagLevel >= 2) - log BEFORE finalization
            if (diagLevel >= 2) {
                const long long priceTicks = PriceToTicks(currentPrice, tickSize);
                const long long anchorTicks = zone.GetAnchorTicks();
                const int distTicks = static_cast<int>(std::abs(priceTicks - anchorTicks));
                const int coreW = config.GetCoreWidth();
                const int haloW = config.GetHaloWidth();
                const int coreExit = (std::min)(coreW + 1, haloW);
                SCString msg;
                msg.Format("[ZONE-PROX] EXIT bar=%d id=%d %s dist=%dt core=%d exit=%d halo=%d %s->%s hits=%d",
                    bar, zone.zoneId, ZoneTypeToString(zone.type), distTicks,
                    coreW, coreExit, haloW,
                    ZoneProximityToString(zone.priorProximity), ZoneProximityToString(zone.proximity),
                    zone.proximityBoundaryHits);
                sc.AddMessageToLog(msg, 0);
            }

            // Leaving zone - finalize and get immutable result
            FinalizationResult result = zone.FinalizeEngagement(bar, time, currentPrice, tickSize, config);

            if (result) {
                // Invoke callback with IMMUTABLE FinalizationResult (metrics + touchRecord)
                if (onEngagementFinalized) {
                    onEngagementFinalized(zone, result);
                }
                finalizedThisBar.push_back(zone.zoneId);
            }
        }
    }

    // =========================================================================
    // DEPARTED->INACTIVE RESOLUTION (ported from UpdateAllProximities)
    // Runs every bar to resolve zones that have been outside halo long enough
    // NOTE: Resolution must occur regardless of engagement outcome. Engagement
    // is finalized when exiting AT_ZONE (line ~3527), but proximity FSM
    // transition DEPARTED->INACTIVE is a separate concern driven by timeout.
    // =========================================================================
    for (auto& [id, zone] : activeZones) {
        // Check all DEPARTED zones (outcome-independent)
        if (zone.proximity == ZoneProximity::DEPARTED &&
            zone.barsOutsideHalo > 0) {

            ResolutionResult resResult = resolution.Evaluate(
                zone.barsOutsideHalo,
                static_cast<int>(zone.secondsOutsideHalo));

            if (resResult.resolved) {
                // Only set outcome if still pending (engagement not yet finalized)
                if (zone.currentEngagement.outcome == AuctionOutcome::PENDING) {
                    zone.currentEngagement.outcome = AuctionOutcome::REJECTED;
                    zone.RecordOutcome(AuctionOutcome::REJECTED, bar);
                }

                // 4-state FSM: DEPARTED -> INACTIVE (always, regardless of outcome)
                zone.proximity = ZoneProximity::INACTIVE;
                resolutionStats.Record(resolution.mode, resResult.reason);

                // [ZONE-PROX] RESOLVED log (diagLevel >= 3)
                if (diagLevel >= 3) {
                    const char* reasonStr = "UNKNOWN";
                    switch (resResult.reason) {
                        case ResolutionReason::RESOLVED_BY_BARS: reasonStr = "BARS"; break;
                        case ResolutionReason::RESOLVED_BY_TIME: reasonStr = "TIME"; break;
                        case ResolutionReason::RESOLVED_BY_BOTH: reasonStr = "BOTH"; break;
                        default: break;
                    }
                    SCString msg;
                    msg.Format("[ZONE-PROX] RESOLVED bar=%d id=%d %s DEPARTED->INACTIVE reason=%s barsOut=%d secsOut=%d",
                        bar, zone.zoneId, ZoneTypeToString(zone.type),
                        reasonStr, zone.barsOutsideHalo, static_cast<int>(zone.secondsOutsideHalo));
                    sc.AddMessageToLog(msg, 0);
                }
            }
        }
    }

    // =========================================================================
    // ZOMBIE DETECTION (diagLevel >= 3)
    // Warn-once per zone if DEPARTED exceeds 5x resolution thresholds
    // DIAGNOSTIC ONLY: Does not affect any behavioral logic
    // =========================================================================
    if (diagLevel >= 3) {
        // Zombie thresholds: 5x the configured resolution thresholds
        const int zombieBarsMargin = 5 * resolution.barsOutsideThreshold;
        const int zombieSecsMargin = 5 * resolution.secondsOutsideThreshold;

        for (const auto& [id, zone] : activeZones) {
            // Only check DEPARTED zones not yet warned
            if (zone.proximity == ZoneProximity::DEPARTED &&
                zombieWarnedIds.find(zone.zoneId) == zombieWarnedIds.end()) {

                // Check if zone exceeds zombie thresholds
                const bool barsExceeded = (zone.barsOutsideHalo > zombieBarsMargin);
                const bool secsExceeded = (static_cast<int>(zone.secondsOutsideHalo) > zombieSecsMargin);

                if (barsExceeded || secsExceeded) {
                    // Mark as warned (prevents repeat warnings for this zone)
                    zombieWarnedIds.insert(zone.zoneId);

                    // Get mode string for logging
                    const char* modeStr = "UNKNOWN";
                    switch (resolution.mode) {
                        case ResolutionMode::BARS_ONLY: modeStr = "BARS_ONLY"; break;
                        case ResolutionMode::TIME_ONLY: modeStr = "TIME_ONLY"; break;
                        case ResolutionMode::BARS_OR_TIME: modeStr = "BARS_OR_TIME"; break;
                    }

                    // Get outcome string for logging
                    const char* outcomeStr = "PENDING";
                    switch (zone.currentEngagement.outcome) {
                        case AuctionOutcome::PENDING: outcomeStr = "PENDING"; break;
                        case AuctionOutcome::ACCEPTED: outcomeStr = "ACCEPTED"; break;
                        case AuctionOutcome::REJECTED: outcomeStr = "REJECTED"; break;
                    }

                    SCString msg;
                    msg.Format("[ZONE-ZOMBIE] bar=%d id=%d %s prox=DEPARTED outcome=%s "
                               "barsOut=%d secsOut=%d lastOutBar=%d | "
                               "policy: barsThr=%d secsThr=%d mode=%s",
                        bar, zone.zoneId, ZoneTypeToString(zone.type), outcomeStr,
                        zone.barsOutsideHalo, static_cast<int>(zone.secondsOutsideHalo),
                        zone.lastOutsideBar,
                        resolution.barsOutsideThreshold, resolution.secondsOutsideThreshold,
                        modeStr);
                    sc.AddMessageToLog(msg, 1);  // Level 1 = warning
                }
            }
        }
    }

    // Handle zones that need replacement (latched large-jump during engagement)
    // After finalization, zones with pendingReplaceNeeded should be retired
    int pendingRemoveCount = 0;
    for (auto it = activeZones.begin(); it != activeZones.end(); ) {
        ZoneRuntime& zone = it->second;
        if (zone.pendingReplaceNeeded) {
            pendingRemoveCount++;
            // Clear anchor reference before removing
            anchors.ClearIfMatches(zone.zoneId);
            it = activeZones.erase(it);
        } else {
            ++it;
        }
    }
    (void)pendingRemoveCount;  // Used for debugging

    // Periodic cleanup
    barsSinceLastCleanup++;
    if (barsSinceLastCleanup >= cleanupIntervalBars) {
        CleanupExpiredZones(bar, time);
        barsSinceLastCleanup = 0;
    }

    // PERFORMANCE: Cache accumulated stats (avoids per-bar loops in consumers)
    cachedTotalTouches = touchAccum;
    cachedActiveZoneCount = static_cast<int>(activeZones.size());
}

/**
 * Update all proximities and build context snapshot (A1, C, E)
 * Main entry point - NO STATIC LOCALS
 */
inline void ZoneManager::UpdateAllProximities(
    double currentPrice, double tickSize, int bar, SCDateTime time,
    TransitionState& transitionState,
    ZoneTransitionMemory& transitionMemory,
    const ResolutionPolicy& resolution,
    ZoneContextSnapshot& snapshot,
    SCStudyInterfaceRef sc, int diagLevel)
{
    snapshot.Reset();
    snapshot.computedAtBar = bar;

    // First, update all zone proximities
    UpdateZones(currentPrice, tickSize, bar, time, sc, diagLevel);

    // Update sticky zone memory
    transitionMemory.Update(bar);

    // Early exit check: if no zones are nearby at all
    bool anyZoneNearby = false;
    for (const auto& [id, zone] : activeZones) {
        if (zone.proximity != ZoneProximity::INACTIVE) {
            anyZoneNearby = true;
            break;
        }
    }

    if (!anyZoneNearby) {
        // C: Early exit MUST still process transitions
        BuildContextSnapshotEarlyExit(currentPrice, tickSize, bar, time,
                                      transitionState, snapshot);
        return;
    }

    // Find the primary (dominant) zone using sticky selection
    ZoneRuntime* primary = GetStrongestZoneAtPriceSticky(
        currentPrice, tickSize, transitionMemory, bar, config.GetHaloWidth());

    if (primary) {
        snapshot.primaryZoneId = primary->zoneId;
        snapshot.dominantProximity = primary->proximity;

        // Count confluence
        snapshot.zonesAtPrice = CountZonesAtPrice(currentPrice, tickSize,
                                                   config.GetCoreWidth());
        snapshot.zonesApproaching = CountZonesApproaching(currentPrice, tickSize);

        // E: Update sticky preference if we're at a new zone
        if (primary->proximity == ZoneProximity::AT_ZONE) {
            if (transitionMemory.preferredZoneId != primary->zoneId) {
                transitionMemory.SetPreferred(primary->zoneId, bar);
            }
        }

        // Process transition state (A1)
        transitionState.ProcessTransition(primary->proximity, primary->zoneId,
                                          bar, time);

        // Copy transition flags to snapshot
        snapshot.justEnteredZone = transitionState.justEnteredZone;
        snapshot.justExitedZone = transitionState.justExitedZone;
        snapshot.justChangedZone = transitionState.justChangedZone;

        // Engagement info
        if (primary->proximity == ZoneProximity::AT_ZONE) {
            snapshot.engagementBars = transitionState.GetEngagementBars(bar);
            snapshot.engagementSeconds = transitionState.GetEngagementSeconds(time);
        }
    } else {
        // No primary zone found - treat as inactive
        transitionState.ProcessTransition(ZoneProximity::INACTIVE, -1, bar, time);
        snapshot.dominantProximity = ZoneProximity::INACTIVE;
        snapshot.justExitedZone = transitionState.justExitedZone;
    }

    // =========================================================================
    // CONTINUOUS RESOLUTION CHECK - runs EVERY bar, not just on exit
    // HIGH PRIORITY FIX: Outcome finalization must happen continuously
    // Also handles DEPARTED -> INACTIVE transition (4-state FSM)
    // Instrumented: Records resolution reason histogram
    // =========================================================================
    for (auto& [id, zone] : activeZones) {
        // Only check zones that are outside the halo and have pending outcomes
        if (zone.proximity != ZoneProximity::AT_ZONE &&
            zone.barsOutsideHalo > 0 &&
            zone.currentEngagement.outcome == AuctionOutcome::PENDING) {

            // Use Evaluate() to get full result with reason for instrumentation
            ResolutionResult resResult = resolution.Evaluate(
                zone.barsOutsideHalo,
                static_cast<int>(zone.secondsOutsideHalo));

            if (resResult.resolved) {
                zone.currentEngagement.outcome = AuctionOutcome::REJECTED;
                zone.RecordOutcome(AuctionOutcome::REJECTED, bar);

                // 4-state FSM: DEPARTED -> INACTIVE on resolution
                if (zone.proximity == ZoneProximity::DEPARTED) {
                    zone.proximity = ZoneProximity::INACTIVE;

                    // Record resolution stats (only on actual DEPARTED->INACTIVE)
                    resolutionStats.Record(resolution.mode, resResult.reason);
                }
            }
        }
    }

    snapshot.valid = true;
}

/**
 * Build context snapshot for early-exit (C)
 * Ensures transitions are processed even when price is far from all zones
 */
inline void ZoneManager::BuildContextSnapshotEarlyExit(
    double currentPrice, double tickSize, int bar, SCDateTime time,
    TransitionState& transitionState,
    ZoneContextSnapshot& snapshot)
{
    // C: CRITICAL - Must still process transition to detect exits
    // Even though no zones are nearby, the PRIOR state may have been AT_ZONE

    ZoneProximity priorDominant = transitionState.lastDominantProximity;

    // Process transition: new state is INACTIVE
    transitionState.ProcessTransition(ZoneProximity::INACTIVE, -1, bar, time);

    // If we were at a zone and now we're inactive, this is an exit
    if (transitionState.justExitedZone) {
        // Find the zone we exited and finalize its engagement
        int exitedZoneId = transitionState.lastPrimaryZoneId;
        ZoneRuntime* exitedZone = GetZone(exitedZoneId);

        if (exitedZone && exitedZone->HasPendingEngagement()) {
            // Finalize and get immutable result
            FinalizationResult result = exitedZone->FinalizeEngagement(bar, time, currentPrice, tickSize, config);

            if (result) {
                // Invoke callback with IMMUTABLE FinalizationResult (metrics + touchRecord)
                if (onEngagementFinalized) {
                    onEngagementFinalized(*exitedZone, result);
                }
                finalizedThisBar.push_back(exitedZone->zoneId);
            }
        }
    }

    // Fill snapshot
    snapshot.primaryZoneId = -1;
    snapshot.dominantProximity = ZoneProximity::INACTIVE;
    snapshot.zonesAtPrice = 0;
    snapshot.zonesApproaching = 0;
    snapshot.justEnteredZone = transitionState.justEnteredZone;
    snapshot.justExitedZone = transitionState.justExitedZone;
    snapshot.justChangedZone = transitionState.justChangedZone;
    snapshot.engagementBars = 0;
    snapshot.engagementSeconds = 0;
    snapshot.valid = true;
    snapshot.computedAtBar = bar;
}

/**
 * Get volume node type (implementation of VolumeCharacteristics method)
 * SSOT: Uses cached classification.density instead of ratio-based checks
 * SSOT: Delta threshold checks inlined (Change Set B)
 */
inline VolumeNodeType VolumeCharacteristics::GetNodeType(
    const ZoneConfig& cfg,
    const ZoneRuntime& zone) const
{
    // HIGH VOLUME NODES - SSOT: Use IsHVN_SSOT() instead of IsHVN(cfg)
    if (IsHVN_SSOT()) {
        bool isUpperBoundary = (zone.role == ZoneRole::VALUE_BOUNDARY &&
                               zone.type == ZoneType::VPB_VAH);
        bool isLowerBoundary = (zone.role == ZoneRole::VALUE_BOUNDARY &&
                               zone.type == ZoneType::VPB_VAL);

        double aggression = GetAggressionRatio();

        // SSOT: Inline delta threshold checks (previously IsBuyingNode/IsSellingNode)
        const bool isBuyingDelta = (deltaRatio >= cfg.buyingNodeThreshold);
        const bool isSellingDelta = (deltaRatio <= cfg.sellingNodeThreshold);

        if (isUpperBoundary) {
            // At VAH
            if (isSellingDelta && aggression < cfg.aggressionLowThreshold) {
                return VolumeNodeType::HVN_RESPONSIVE;  // Sellers defending
            } else if (isBuyingDelta && aggression > cfg.aggressionHighThreshold) {
                return VolumeNodeType::HVN_INITIATIVE;  // Buyers attacking
            }
        } else if (isLowerBoundary) {
            // At VAL
            if (isBuyingDelta && aggression > cfg.aggressionHighThreshold) {
                return VolumeNodeType::HVN_RESPONSIVE;  // Buyers defending
            } else if (isSellingDelta && aggression < cfg.aggressionLowThreshold) {
                return VolumeNodeType::HVN_INITIATIVE;  // Sellers attacking
            }
        }

        // Not at boundary or mixed
        return VolumeNodeType::HVN_BALANCED;
    }

    // LOW VOLUME NODES - SSOT: Use IsLVN_SSOT() instead of IsLVN(cfg)
    if (IsLVN_SSOT()) {
        if (classification.IsSinglePrint()) {
            return VolumeNodeType::LVN_SINGLE_PRINT;
        }
        return VolumeNodeType::LVN_GAP;
    }

    // Normal
    return VolumeNodeType::NORMAL;
}

// ============================================================================
// PHASE 3: VALIDATION INFRASTRUCTURE
// Engagement episode comparison for legacy/AMT parity checking
// All logic gated by VALIDATE_ZONE_MIGRATION compile flag
// ============================================================================

/**
 * Reason codes for validation mismatches.
 * Used for structured logging and categorization.
 */
enum class ValidationMismatchReason {
    NONE = 0,
    ENTRY_BAR_DIFF,
    EXIT_BAR_DIFF,
    BARS_ENGAGED_DIFF,
    ENTRY_PRICE_DIFF,
    EXIT_PRICE_DIFF,
    ESC_VEL_DIFF,
    WIDTH_CORE_DIFF,
    WIDTH_HALO_DIFF,
    MISSING_LEGACY_EPISODE,
    MISSING_AMT_EPISODE,
    WIDTH_UNEXPECTED_CHANGE  // AMT width changed without legacy liqTicks change
};

/**
 * Get string representation of mismatch reason.
 */
inline const char* GetMismatchReasonString(ValidationMismatchReason reason) {
    switch (reason) {
        case ValidationMismatchReason::NONE: return "NONE";
        case ValidationMismatchReason::ENTRY_BAR_DIFF: return "ENTRY_BAR_DIFF";
        case ValidationMismatchReason::EXIT_BAR_DIFF: return "EXIT_BAR_DIFF";
        case ValidationMismatchReason::BARS_ENGAGED_DIFF: return "BARS_ENGAGED_DIFF";
        case ValidationMismatchReason::ENTRY_PRICE_DIFF: return "ENTRY_PRICE_DIFF";
        case ValidationMismatchReason::EXIT_PRICE_DIFF: return "EXIT_PRICE_DIFF";
        case ValidationMismatchReason::ESC_VEL_DIFF: return "ESC_VEL_DIFF";
        case ValidationMismatchReason::WIDTH_CORE_DIFF: return "WIDTH_CORE_DIFF";
        case ValidationMismatchReason::WIDTH_HALO_DIFF: return "WIDTH_HALO_DIFF";
        case ValidationMismatchReason::MISSING_LEGACY_EPISODE: return "MISSING_LEGACY_EPISODE";
        case ValidationMismatchReason::MISSING_AMT_EPISODE: return "MISSING_AMT_EPISODE";
        case ValidationMismatchReason::WIDTH_UNEXPECTED_CHANGE: return "WIDTH_UNEXPECTED_CHANGE";
        default: return "UNKNOWN";
    }
}

/**
 * Captured engagement episode for validation comparison.
 * Immutable after capture - represents a single finalized engagement.
 */
struct ValidationEpisode {
    // Identity
    int zoneId = -1;
    ZoneType zoneType = ZoneType::VPB_POC;
    double anchorPrice = 0.0;

    // Engagement boundaries
    int entryBar = -1;
    int exitBar = -1;
    int barsEngaged = 0;

    // Price metrics
    double entryPrice = 0.0;
    double exitPrice = 0.0;
    double escapeVelocity = 0.0;

    // Width at engagement (for width parity)
    int coreWidthTicks = 0;
    int haloWidthTicks = 0;

    // Source identification
    bool isLegacy = false;  // true = legacy, false = AMT
    bool matched = false;   // Has this episode been matched?

    /**
     * Round anchor to tick for matching (uses canonical converter).
     * Both systems should agree on tick-level anchor.
     */
    int GetAnchorInTicks(double tickSize) const {
        if (tickSize <= 0.0) return 0;
        // Use canonical PriceToTicks for consistent rounding policy
        return static_cast<int>(PriceToTicks(anchorPrice, tickSize));
    }

    /**
     * Check if two episodes could be the same engagement.
     * Primary: same anchor tick and zone type
     * Secondary: entry bar within tolerance
     */
    bool CouldMatch(const ValidationEpisode& other, double tickSize, int barTolerance = 2) const {
        // Must be different sources
        if (isLegacy == other.isLegacy) return false;

        // Primary: anchor and type must match
        if (GetAnchorInTicks(tickSize) != other.GetAnchorInTicks(tickSize)) return false;
        if (zoneType != other.zoneType) return false;

        // Secondary: entry bar within tolerance
        int entryDiff = std::abs(entryBar - other.entryBar);
        if (entryDiff > barTolerance) return false;

        // Check for interval overlap
        int overlapStart = (std::max)(entryBar, other.entryBar);
        int overlapEnd = (std::min)(exitBar, other.exitBar);
        if (overlapEnd < overlapStart) return false;  // No overlap

        return true;
    }
};

/**
 * Validation tolerances - defined up front per spec.
 */
struct ValidationTolerances {
    int barTolerance = 1;           // ±1 bar for entry/exit matching
    double escVelEpsilon = 1e-6;    // Floating point tolerance for escape velocity
    // Prices: exact tick match required (no tolerance)
    // Widths: exact match required
};

/**
 * Validation counters for summary reporting.
 */
struct ValidationCounters {
    int amtFinalizedCount = 0;
    int legacyFinalizedCount = 0;
    int matchedCount = 0;
    int mismatchCount = 0;
    int missingLegacyCount = 0;
    int missingAmtCount = 0;
    int widthMismatchCount = 0;

    // Per-reason breakdown
    int entryBarDiffCount = 0;
    int exitBarDiffCount = 0;
    int barsEngagedDiffCount = 0;
    int escVelDiffCount = 0;
    int widthCoreDiffCount = 0;
    int widthHaloDiffCount = 0;

    void Reset() {
        amtFinalizedCount = 0;
        legacyFinalizedCount = 0;
        matchedCount = 0;
        mismatchCount = 0;
        missingLegacyCount = 0;
        missingAmtCount = 0;
        widthMismatchCount = 0;
        entryBarDiffCount = 0;
        exitBarDiffCount = 0;
        barsEngagedDiffCount = 0;
        escVelDiffCount = 0;
        widthCoreDiffCount = 0;
        widthHaloDiffCount = 0;
    }

    void IncrementForReason(ValidationMismatchReason reason) {
        switch (reason) {
            case ValidationMismatchReason::ENTRY_BAR_DIFF: entryBarDiffCount++; break;
            case ValidationMismatchReason::EXIT_BAR_DIFF: exitBarDiffCount++; break;
            case ValidationMismatchReason::BARS_ENGAGED_DIFF: barsEngagedDiffCount++; break;
            case ValidationMismatchReason::ESC_VEL_DIFF: escVelDiffCount++; break;
            case ValidationMismatchReason::WIDTH_CORE_DIFF: widthCoreDiffCount++; break;
            case ValidationMismatchReason::WIDTH_HALO_DIFF: widthHaloDiffCount++; break;
            case ValidationMismatchReason::MISSING_LEGACY_EPISODE: missingLegacyCount++; break;
            case ValidationMismatchReason::MISSING_AMT_EPISODE: missingAmtCount++; break;
            default: break;
        }
    }
};

/**
 * Width parity state for tracking unexpected changes.
 */
struct WidthParityState {
    int lastLegacyLiqTicks = -1;
    int lastAmtCoreTicks = -1;
    int lastAmtHaloTicks = -1;
    int lastUpdateBar = -1;

    /**
     * Record a legacy width update.
     */
    void RecordLegacyUpdate(int liqTicks, int bar) {
        lastLegacyLiqTicks = liqTicks;
        lastUpdateBar = bar;
    }

    /**
     * Record an AMT width update.
     */
    void RecordAmtUpdate(int coreTicks, int haloTicks, int bar) {
        lastAmtCoreTicks = coreTicks;
        lastAmtHaloTicks = haloTicks;
        lastUpdateBar = bar;
    }

    void Reset() {
        lastLegacyLiqTicks = -1;
        lastAmtCoreTicks = -1;
        lastAmtHaloTicks = -1;
        lastUpdateBar = -1;
    }
};

/**
 * Complete validation state for a session.
 * Holds episode buffers, counters, and matching state.
 */
struct ValidationState {
    // Episode buffers (ring buffer - keep last N for matching)
    static constexpr int MAX_EPISODES = 100;
    std::vector<ValidationEpisode> legacyEpisodes;
    std::vector<ValidationEpisode> amtEpisodes;

    // Counters
    ValidationCounters counters;

    // Tolerances
    ValidationTolerances tolerances;

    // Width parity
    WidthParityState widthState;

    // Session tracking
    bool sessionActive = false;
    int sessionStartBar = -1;

    void StartSession(int bar) {
        sessionActive = true;
        sessionStartBar = bar;
        legacyEpisodes.clear();
        amtEpisodes.clear();
        counters.Reset();
        widthState.Reset();
    }

    void EndSession() {
        sessionActive = false;
    }

    /**
     * Add a legacy episode and attempt to match with pending AMT episodes.
     */
    void AddLegacyEpisode(const ValidationEpisode& episode, double tickSize) {
        ValidationEpisode ep = episode;
        ep.isLegacy = true;
        counters.legacyFinalizedCount++;

        // Try to match with unmatched AMT episodes
        for (auto& amtEp : amtEpisodes) {
            if (!amtEp.matched && ep.CouldMatch(amtEp, tickSize, tolerances.barTolerance)) {
                ep.matched = true;
                amtEp.matched = true;
                counters.matchedCount++;
                break;
            }
        }

        // Add to buffer (with ring buffer behavior)
        if (legacyEpisodes.size() >= MAX_EPISODES) {
            legacyEpisodes.erase(legacyEpisodes.begin());
        }
        legacyEpisodes.push_back(ep);
    }

    /**
     * Add an AMT episode and attempt to match with pending legacy episodes.
     */
    void AddAmtEpisode(const ValidationEpisode& episode, double tickSize) {
        ValidationEpisode ep = episode;
        ep.isLegacy = false;
        counters.amtFinalizedCount++;

        // Try to match with unmatched legacy episodes
        for (auto& legEp : legacyEpisodes) {
            if (!legEp.matched && ep.CouldMatch(legEp, tickSize, tolerances.barTolerance)) {
                ep.matched = true;
                legEp.matched = true;
                counters.matchedCount++;
                break;
            }
        }

        // Add to buffer
        if (amtEpisodes.size() >= MAX_EPISODES) {
            amtEpisodes.erase(amtEpisodes.begin());
        }
        amtEpisodes.push_back(ep);
    }

    /**
     * Compare two matched episodes and return first mismatch reason.
     * Returns NONE if episodes match within tolerances.
     */
    ValidationMismatchReason CompareEpisodes(
        const ValidationEpisode& legacy,
        const ValidationEpisode& amt) const
    {
        // Entry bar (with tolerance)
        if (std::abs(legacy.entryBar - amt.entryBar) > tolerances.barTolerance) {
            return ValidationMismatchReason::ENTRY_BAR_DIFF;
        }

        // Exit bar (with tolerance)
        if (std::abs(legacy.exitBar - amt.exitBar) > tolerances.barTolerance) {
            return ValidationMismatchReason::EXIT_BAR_DIFF;
        }

        // Bars engaged (exact, since derived from entry/exit)
        if (legacy.barsEngaged != amt.barsEngaged) {
            return ValidationMismatchReason::BARS_ENGAGED_DIFF;
        }

        // Escape velocity (with epsilon)
        if (std::fabs(legacy.escapeVelocity - amt.escapeVelocity) > tolerances.escVelEpsilon) {
            return ValidationMismatchReason::ESC_VEL_DIFF;
        }

        // Width parity (exact)
        if (legacy.coreWidthTicks != amt.coreWidthTicks) {
            return ValidationMismatchReason::WIDTH_CORE_DIFF;
        }
        if (legacy.haloWidthTicks != amt.haloWidthTicks) {
            return ValidationMismatchReason::WIDTH_HALO_DIFF;
        }

        return ValidationMismatchReason::NONE;
    }

    /**
     * Find matching legacy episode for an AMT episode.
     * Returns nullptr if no match found.
     */
    const ValidationEpisode* FindMatchingLegacy(
        const ValidationEpisode& amtEpisode,
        double tickSize) const
    {
        for (const auto& legEp : legacyEpisodes) {
            if (amtEpisode.CouldMatch(legEp, tickSize, tolerances.barTolerance)) {
                return &legEp;
            }
        }
        return nullptr;
    }

    /**
     * Count unmatched episodes (for end-of-session summary).
     */
    void CountUnmatched() {
        for (const auto& ep : legacyEpisodes) {
            if (!ep.matched) {
                counters.missingAmtCount++;
            }
        }
        for (const auto& ep : amtEpisodes) {
            if (!ep.matched) {
                counters.missingLegacyCount++;
            }
        }
    }
};

} // namespace AMT

#endif // AMT_ZONES_H
