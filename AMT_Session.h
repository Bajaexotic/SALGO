// ============================================================================
// AMT_Session.h
// Session management structures and SessionPhaseCoordinator (SSOT)
// Extracted from AuctionSensor_v1.cpp for modularization
// ============================================================================

#ifndef AMT_SESSION_H
#define AMT_SESSION_H

#include "amt_core.h"
#include "AMT_Snapshots.h"  // For RollingDist, EffortBaselineStore, SessionDeltaBaseline
#include <cassert>
#include <vector>
#include <set>

namespace AMT {

// ============================================================================
// SESSION PHASE COORDINATOR (SSOT for Session Phase)
// ============================================================================
//
// Problem: SessionPhase was stored in 4 locations:
//   1. sessionMgr.activePhase
//   2. sessionVolumeProfile.session_phase
//   3. st->prevPhase
//   4. SessionContext.sessionPhase (in ZoneRuntime)
//
// Solution: Single source of truth for session phase with unified API.
// All consumers must use this coordinator instead of direct storage.
//
// Usage:
//   coordinator.UpdatePhase(newPhase, currentBar, diagLevel, sc);
//   SessionPhase phase = coordinator.GetPhase();
//   bool changed = coordinator.DidSessionChange();
//   bool isRTH = coordinator.IsRTH();
// ============================================================================

class SessionPhaseCoordinator
{
public:
    // --- Read-only accessors (consumers use these) ---

    SessionPhase GetPhase() const { return current_; }
    SessionPhase GetPrevPhase() const { return previous_; }
    bool IsRTH() const { return IsRTHSession(current_); }
    bool IsGlobex() const { return IsGlobexSession(current_); }

    // Returns true if session changed during last UpdatePhase() call
    bool DidSessionChange() const { return sessionChanged_; }

    // Returns true if phase changed within same session type (e.g., IB -> MID_SESSION)
    bool DidPhaseChange() const { return phaseChanged_; }

    // Returns true if we transitioned between RTH and Globex
    // DEPRECATED: Use SessionManager.DidSessionChange() instead
    // DEPRECATED - use SessionManager.DidSessionChange()
    bool DidSessionTypeChange() const { return sessionTypeChanged_; } // DEPRECATED

    // --- Write interface (single point of mutation) ---

    // Update the current session phase (call once per bar)
    // Returns true if phase changed
    bool UpdatePhase(SessionPhase newPhase)
    {
        // Reset change flags
        sessionChanged_ = false;
        phaseChanged_ = false;
        sessionTypeChanged_ = false;

        if (newPhase == current_)
            return false;

        // Detect session type change (RTH <-> Globex)
        const bool wasRTH = IsRTHSession(current_);
        const bool isRTH = IsRTHSession(newPhase);
        sessionTypeChanged_ = (wasRTH != isRTH) && (current_ != SessionPhase::UNKNOWN);

        // Any phase change is a session change
        sessionChanged_ = true;
        phaseChanged_ = true;

        // Store transition
        previous_ = current_;
        current_ = newPhase;
        transitionCount_++;

        return true;
    }

    // Get transition count (for diagnostics)
    int GetTransitionCount() const { return transitionCount_; }

    // Reset coordinator (call on chart reset/study restart)
    void Reset()
    {
        current_ = SessionPhase::UNKNOWN;
        previous_ = SessionPhase::UNKNOWN;
        sessionChanged_ = false;
        phaseChanged_ = false;
        sessionTypeChanged_ = false;
        transitionCount_ = 0;
    }

private:
    SessionPhase current_ = SessionPhase::UNKNOWN;
    SessionPhase previous_ = SessionPhase::UNKNOWN;
    bool sessionChanged_ = false;
    bool phaseChanged_ = false;
    bool sessionTypeChanged_ = false;
    int transitionCount_ = 0;
};

} // namespace AMT

// ============================================================================
// PHASE CONTEXT (Auction Phase State - Extracted from SessionContext)
// ============================================================================
// Tracks auction phase transitions and excursion metrics within a session.
// This is separated from level tracking for clearer SSOT responsibilities:
//   - PhaseContext: Auction phase state machine
//   - VersionedLevels (in AMT_VolumeProfile.h): POC/VAH/VAL SSOT
//   - SessionContext: Session H/L extremes + legacy level cache
// ============================================================================

struct PhaseContext
{
    // Current auction phase state
    AMT::CurrentPhase priorPhase = AMT::CurrentPhase::ROTATION;

    // Excursion tracking (outside value area)
    bool inOutsideExcursion = false;
    int barsOutsideValue = 0;
    double volOutsideValue = 0.0;
    double maxExcursionDist = 0.0;

    // Last excursion metrics (captured on re-entry to VA)
    int lastBarsOutside = 0;
    double lastVolOutside = 0.0;
    double lastMaxExcursionDist = 0.0;

    // Extension state (directional move)
    bool extensionActive = false;
    int extensionDirection = 0;  // 1 = Up, -1 = Down

    void Reset()
    {
        priorPhase = AMT::CurrentPhase::ROTATION;
        inOutsideExcursion = false;
        barsOutsideValue = 0;
        volOutsideValue = 0.0;
        maxExcursionDist = 0.0;
        lastBarsOutside = 0;
        lastVolOutside = 0.0;
        lastMaxExcursionDist = 0.0;
        extensionActive = false;
        extensionDirection = 0;
    }

    /**
     * Record excursion completion (call on re-entry to value area)
     */
    void CaptureExcursionMetrics()
    {
        lastBarsOutside = barsOutsideValue;
        lastVolOutside = volOutsideValue;
        lastMaxExcursionDist = maxExcursionDist;

        // Reset current excursion
        inOutsideExcursion = false;
        barsOutsideValue = 0;
        volOutsideValue = 0.0;
        maxExcursionDist = 0.0;
    }
};

// ============================================================================
// SESSION CONTEXT (Session-scoped tracking)
// ============================================================================
// Responsibilities:
//   - PhaseContext (extracted auction phase state)
//
// NOTE: Many fields have been migrated elsewhere:
//   - Session identity → SessionManager.IsRTH()
//   - Session H/L extremes → StructureTracker (in ZoneManager.structure)
//   - BaselineEngine → EffortBaselineStore, SessionDeltaBaseline, DOMWarmup (in StudyState)
//   - POC/VAH/VAL → SessionManager.GetPOC/VAH/VAL()
//
// SSOT NOTE: POC/VAH/VAL are cached here for convenience but the SSOT is:
//   SessionVolumeProfile.levels (VersionedLevels with current/stable/previous)
// ============================================================================

struct SessionContext
{
    // NOTE: isRTHContext removed - was never read (dead code)
    // Use SessionManager.IsRTH() to determine session type

    // NOTE: Legacy BaselineEngine removed (Dec 2024)
    // New architecture uses:
    //   - EffortBaselineStore (bucket-based, prior RTH sessions)
    //   - SessionDeltaBaseline (session-aggregate delta)
    //   - DOMWarmup (live 15-min warmup)
    // These are stored in StudyState, not SessionContext.

    // NOTE: Session extremes (sessionHigh/Low) removed - SSOT is now
    // StructureTracker (in ZoneManager.structure), updated with bar High/Low data.

    // NOTE: POC/VAH/VAL have been moved to SessionManager as the SSOT.
    // The old sessionVPOC/VAH/VAL fields were dead code (written but never read).
    // Use SessionManager.GetPOC()/GetVAH()/GetVAL() instead.

    // Phase state (extracted to PhaseContext for clarity)
    PhaseContext phase;

    void reset()
    {
        phase.Reset();
    }

    // --- Legacy accessors (delegate to PhaseContext) ---
    // These maintain backward compatibility during migration
    AMT::CurrentPhase& priorPhase() { return phase.priorPhase; }
    bool& inOutsideExcursion() { return phase.inOutsideExcursion; }
    int& barsOutsideValue() { return phase.barsOutsideValue; }
    double& volOutsideValue() { return phase.volOutsideValue; }
    double& maxExcursionDist() { return phase.maxExcursionDist; }
    int& lastBarsOutside() { return phase.lastBarsOutside; }
    double& lastVolOutside() { return phase.lastVolOutside; }
    double& lastMaxExcursionDist() { return phase.lastMaxExcursionDist; }
    bool& extensionActive() { return phase.extensionActive; }
    int& extensionDirection() { return phase.extensionDirection; }
};

// ============================================================================
// SESSION MANAGER (SSOT for Session Identity and Core Levels)
// ============================================================================
// SessionManager is the single source of truth for:
//   1. Session Identity (SessionKey) - which session we're in
//   2. Core Levels (POC/VAH/VAL) - populated from VbP study
//   3. Session Transition Detection - when to reset zones
//
// Usage:
//   bool changed = sessionMgr.UpdateSession(newKey, diagLevel, sc);
//   if (changed) { /* handle session transition */ }
//   double poc = sessionMgr.GetPOC();
// ============================================================================

class SessionManager
{
public:
    // --- Session Identity (SSOT) ---
    AMT::SessionKey currentSession;
    AMT::SessionKey previousSession;
    bool sessionChanged = false;

    // --- Session Contexts ---
    SessionContext ctx_rth;
    SessionContext ctx_globex;
    AMT::SessionPhase activePhase = AMT::SessionPhase::UNKNOWN;

private:
    // --- Core Levels (SSOT for POC/VAH/VAL) - PRIVATE, use accessors ---
    // These are populated from VbP study and are the authoritative source.
    // All consumers should read via GetPOC()/GetVAH()/GetVAL().
    // Only UpdateLevels() may write these values.
    double sessionPOC_ = 0.0;
    double sessionVAH_ = 0.0;
    double sessionVAL_ = 0.0;
    int sessionVARangeTicks_ = 0;

    // NOTE: Session extremes (sessionHigh/Low) removed - SSOT is now
    // StructureTracker (in ZoneManager.structure), updated via
    // structure.UpdateExtremes(sc.High, sc.Low) with bar High/Low data.

    // --- Session Timing (SSOT) - PRIVATE, use accessors ---
    int sessionStartBar_ = -1;
    double tickSizeCache_ = 0.0;

public:

    // Update session identity (call every bar)
    // Returns true if session changed (requires zone reset)
    bool UpdateSession(const AMT::SessionKey& newKey)
    {
        // G2 Guardrail: Validate SessionKey has semantic integrity
        // See docs/ssot_session_contract.md for SSOT contract
#ifdef _DEBUG
        // SessionKey must have valid YMD (20200101-20991231) and explicit session type
        assert(newKey.tradingDay >= 20200101 && newKey.tradingDay <= 20991231 &&
               "SessionKey tradingDay out of valid range");
        assert((newKey.IsRTH() || newKey.IsGlobex()) &&
               "SessionKey must have explicit RTH or GLOBEX type");
#endif

        // Trigger session change on:
        // 1. First valid key (initialization) - currentSession invalid, newKey valid
        // 2. Session boundary crossing - currentSession valid, newKey different
        sessionChanged = (!currentSession.IsValid() && newKey.IsValid()) ||
                         (currentSession.IsValid() && newKey != currentSession);

        if (sessionChanged) {
            previousSession = currentSession;
            currentSession = newKey;
        }

        // NOTE: activePhase is managed EXCLUSIVELY by SyncSessionPhase() (SSOT).
        // The old auto-adjust code was removed because:
        // 1. SyncSessionPhase runs BEFORE UpdateSession in the main loop
        // 2. SyncSessionPhase always sets a phase matching the session type
        // 3. Having two writers creates fragile implicit coupling
        // If activePhase needs adjustment, call SyncSessionPhase() explicitly.

        return sessionChanged;
    }

    // Update core levels from VbP study (SINGLE WRITER for POC/VAH/VAL)
    void UpdateLevels(double poc, double vah, double val, double tickSize)
    {
        sessionPOC_ = poc;
        sessionVAH_ = vah;
        sessionVAL_ = val;
        tickSizeCache_ = tickSize;
        if (tickSize > 0.0 && vah > val) {
            sessionVARangeTicks_ = static_cast<int>((vah - val) / tickSize);
        }
    }

    // NOTE: UpdateExtremes() removed - session extremes are now managed by
    // StructureTracker (ZoneManager.structure.UpdateExtremes)

    // --- Read-only accessors for SSOT fields ---
    double GetPOC() const { return sessionPOC_; }
    double GetVAH() const { return sessionVAH_; }
    double GetVAL() const { return sessionVAL_; }
    int GetVARangeTicks() const { return sessionVARangeTicks_; }
    int GetSessionStartBar() const { return sessionStartBar_; }
    double GetTickSizeCache() const { return tickSizeCache_; }

    // --- Single-writer for session timing ---
    void SetSessionStartBar(int bar) { sessionStartBar_ = bar; }
    // Consume-on-read: Returns true ONCE per session change, then auto-clears.
    // This is the SSOT for session transition detection - no manual clear needed.
    bool ConsumeSessionChange() {
        const bool changed = sessionChanged;
        sessionChanged = false;
        return changed;
    }

    // Read-only peek for diagnostics (does NOT clear the flag)
    bool PeekSessionChanged() const { return sessionChanged; }

    bool IsRTH() const { return currentSession.IsRTH(); }
    bool IsGlobex() const { return currentSession.IsGlobex(); }

    void reset()
    {
        ctx_rth.reset();
        ctx_globex.reset();

        activePhase = AMT::SessionPhase::UNKNOWN;
        currentSession = AMT::SessionKey{};
        previousSession = AMT::SessionKey{};
        sessionChanged = false;

        sessionPOC_ = sessionVAH_ = sessionVAL_ = 0.0;
        sessionVARangeTicks_ = 0;
        tickSizeCache_ = 0.0;
        // NOTE: sessionHigh/sessionLow removed - now in StructureTracker
        sessionStartBar_ = -1;  // SSOT: Will be set at session transition
    }

    // Reset levels only (for session transition without full reset)
    void resetLevels()
    {
        sessionPOC_ = sessionVAH_ = sessionVAL_ = 0.0;
        sessionVARangeTicks_ = 0;
        // NOTE: sessionHigh/sessionLow removed - now in StructureTracker
    }

    SessionContext& getActiveContext()
    {
        return AMT::IsRTHSession(activePhase) ? ctx_rth : ctx_globex;
    }

    const SessionContext& getActiveContext() const
    {
        return AMT::IsRTHSession(activePhase) ? ctx_rth : ctx_globex;
    }

    // --- Get context by explicit session type (for baseline accumulation routing) ---
    SessionContext& getContextByType(AMT::SessionType type)
    {
        return (type == AMT::SessionType::RTH) ? ctx_rth : ctx_globex;
    }

    const SessionContext& getContextByType(AMT::SessionType type) const
    {
        return (type == AMT::SessionType::RTH) ? ctx_rth : ctx_globex;
    }

    // NOTE: Legacy getRTHBaselines()/getGBXBaselines() removed (Dec 2024)
    // Use StudyState.effortBaselines, .sessionDeltaBaseline, .domWarmup instead.
};

// ============================================================================
// SESSION ACCUMULATORS - Tracks counts that aggregate during the session
// These are copied into SessionStatistics when stats are calculated
// ============================================================================

struct SessionAccumulators {
    // HVN/LVN changes
    int hvnAdded = 0;
    int hvnRemoved = 0;
    int lvnAdded = 0;
    int lvnRemoved = 0;

    // Zone engagements
    int engagementCount = 0;
    int escapeCount = 0;
    int totalEngagementBars = 0;      // For averaging
    double totalEscapeVelocity = 0.0; // For averaging

    // Extreme conditions
    int extremeVolumeCount = 0;
    int extremeDeltaCount = 0;
    int extremeTradesCount = 0;
    int extremeStackCount = 0;
    int extremePullCount = 0;
    int extremeDepthCount = 0;

    // Data quality tracking (debug only, not in session stats)
    int domStaleCount = 0;
    int pocDriftCount = 0;
    int profileRefreshCount = 0;

    // Probes
    int probesFired = 0;
    int probesResolved = 0;
    int probesHit = 0;
    int probesMissed = 0;
    int probesExpired = 0;
    double totalProbeScore = 0.0;  // For averaging

    // Session/state transitions
    int sessionChangeCount = 0;
    int phaseTransitionCount = 0;
    int intentChangeCount = 0;
    int marketStateChangeCount = 0;

    // Warnings/errors
    int zoneWidthMismatchCount = 0;
    int validationDivergenceCount = 0;
    int configErrorCount = 0;
    int vbpWarningCount = 0;

    // ========================================================================
    // VOLUME AND DELTA ACCUMULATORS (per-bar accumulation for session stats)
    // ========================================================================
    // SSOT: Computed from native sc.AskVolume/sc.BidVolume, not from Numbers Bars
    //
    // IDEMPOTENCY INVARIANTS:
    //   1. lastAccumulatedBarIndex tracks the last bar index that was accumulated
    //   2. sessionStartBarIndex tracks where the current session started (for rebuild)
    //   3. Each bar is counted exactly once, at its FINAL (closed) values
    //   4. On recalculation rewind, we detect and rebuild from sessionStartBarIndex
    //
    // SEMANTIC CONTRACT (CLOSED-BAR ONLY):
    //   - sessionCumDelta includes ONLY closed/finalized bars
    //   - The current forming bar is NOT included (it has partial values)
    //   - This means sessionCumDelta LAGS by the current bar's delta intrabar
    //   - For live parity with NB: sessionCumDeltaLive = sessionCumDelta + currentBarDelta
    //
    // REWIND DETECTION:
    //   - If sc.Index <= lastAccumulatedBarIndex, a recalculation rewind occurred
    //   - Response: Rebuild from sessionStartBarIndex (or from sc.Index if >= sessionStart)
    // ========================================================================
    double sessionTotalVolume = 0.0;   // Sum of all CLOSED bar volumes (from sc.Volume)
    double sessionCumDelta = 0.0;      // Sum of all CLOSED bar deltas (AskVol - BidVol)
    double firstBarVolume = 0.0;       // Volume of first bar in session (for diagnostics)
    double firstBarDelta = 0.0;        // Delta of first bar in session (for diagnostics)
    int lastAccumulatedBarIndex = -1;  // Last bar index accumulated (prevents double-counting)
    int sessionStartBarIndex = -1;     // First bar index of current session (for rebuild)
    int lastResetSessionId = 0;        // Hash of SessionKey that triggered last reset (exactly-once guard)

    // First-bar state flags (for re-application after session reset)
    // These capture state from UpdateSessionBaselines() for the first bar,
    // which runs BEFORE session detection. After Reset(), we re-apply these.
    bool firstBarDomStale = false;

    // ========================================================================
    // SESSION-SCOPED DELTA CONTRACT (SSOT - First-Class Decision Input)
    // ========================================================================
    //
    // DEFINITIONS (ROBUST POLICY - native SC arrays, no cross-study dependencies):
    //   barDelta := sc.AskVolume[idx] - sc.BidVolume[idx]  (SSOT)
    //   sessionCumDelta := accumulated sum of barDelta (reset at session boundary)
    //   sessionDeltaRatio := sessionCumDelta / max(sessionTotalVolume, 1.0)
    //
    // CONTRACT INVARIANTS:
    //   1. At session boundary, sessionCumDelta resets to 0
    //   2. Each bar adds barDelta to sessionCumDelta
    //   3. Denominator is ALWAYS sessionAccum.sessionTotalVolume (SSOT for session volume)
    //   4. No dependency on Numbers Bars study - fully self-contained
    //
    // USAGE:
    //   - sessionDeltaRatio measures net directional conviction for the ENTIRE session
    //   - Combined with per-bar deltaConsistency for persistence-validated extreme detection
    //   - Sign indicates net buyer (+) vs seller (-) pressure across session
    //
    // CONSUMERS:
    //   - SessionDeltaBaseline: Rolling distribution for percentile ranking (in StudyState)
    //   - IsExtremeDeltaSession(): Percentile-based persistence check
    //   - Directional coherence check for aggression classification
    //
    // DEBUG CROSS-CHECK (optional):
    //   - nbCumDelta can be read from Numbers Bars SG10 for validation
    //   - Expected: sessionCumDelta ≈ nbCumDelta (within rounding)
    //
    // cumDeltaAtSessionStart and lastSeenCumDelta are DEPRECATED (kept for migration)
    // ========================================================================
    double cumDeltaAtSessionStart = 0.0;  // DEPRECATED: kept for migration compatibility
    double lastSeenCumDelta = 0.0;        // DEPRECATED: kept for migration compatibility
    bool cumDeltaAtSessionStartValid = false;  // DEPRECATED: kept for migration compatibility

    void Reset() {
        // NOTE: cumDeltaAtSessionStart, lastSeenCumDelta, and cumDeltaAtSessionStartValid
        // are NOT reset here. They are set explicitly at session transitions and first bar.
        hvnAdded = hvnRemoved = lvnAdded = lvnRemoved = 0;
        engagementCount = escapeCount = totalEngagementBars = 0;
        totalEscapeVelocity = 0.0;
        extremeVolumeCount = extremeDeltaCount = extremeTradesCount = 0;
        extremeStackCount = extremePullCount = extremeDepthCount = 0;
        domStaleCount = pocDriftCount = profileRefreshCount = 0;
        probesFired = probesResolved = probesHit = probesMissed = probesExpired = 0;
        totalProbeScore = 0.0;
        sessionChangeCount = phaseTransitionCount = intentChangeCount = marketStateChangeCount = 0;
        zoneWidthMismatchCount = validationDivergenceCount = configErrorCount = vbpWarningCount = 0;
        sessionTotalVolume = 0.0;
        sessionCumDelta = 0.0;
        firstBarVolume = 0.0;
        firstBarDelta = 0.0;
        lastAccumulatedBarIndex = -1;  // Reset to allow re-accumulation for new session
        sessionStartBarIndex = -1;     // Will be set to sc.Index at session boundary
        // NOTE: lastResetSessionId is NOT reset here - it's set at session boundary
        firstBarDomStale = false;
    }

    // Accumulate from another instance (for session rollup)
    void Accumulate(const SessionAccumulators& other) {
        hvnAdded += other.hvnAdded;
        hvnRemoved += other.hvnRemoved;
        lvnAdded += other.lvnAdded;
        lvnRemoved += other.lvnRemoved;
        engagementCount += other.engagementCount;
        escapeCount += other.escapeCount;
        totalEngagementBars += other.totalEngagementBars;
        totalEscapeVelocity += other.totalEscapeVelocity;
        extremeVolumeCount += other.extremeVolumeCount;
        extremeDeltaCount += other.extremeDeltaCount;
        extremeTradesCount += other.extremeTradesCount;
        extremeStackCount += other.extremeStackCount;
        extremePullCount += other.extremePullCount;
        extremeDepthCount += other.extremeDepthCount;
        domStaleCount += other.domStaleCount;
        pocDriftCount += other.pocDriftCount;
        profileRefreshCount += other.profileRefreshCount;
        probesFired += other.probesFired;
        probesResolved += other.probesResolved;
        probesHit += other.probesHit;
        probesMissed += other.probesMissed;
        probesExpired += other.probesExpired;
        totalProbeScore += other.totalProbeScore;
        sessionChangeCount += other.sessionChangeCount;
        phaseTransitionCount += other.phaseTransitionCount;
        intentChangeCount += other.intentChangeCount;
        marketStateChangeCount += other.marketStateChangeCount;
        zoneWidthMismatchCount += other.zoneWidthMismatchCount;
        validationDivergenceCount += other.validationDivergenceCount;
        configErrorCount += other.configErrorCount;
        vbpWarningCount += other.vbpWarningCount;
        sessionTotalVolume += other.sessionTotalVolume;
    }

    // Get average engagement bars (returns 0 if no engagements)
    double GetAvgEngagementBars() const {
        return (engagementCount > 0) ? static_cast<double>(totalEngagementBars) / engagementCount : 0.0;
    }

    // Get average escape velocity (returns 0 if no escapes)
    double GetAvgEscapeVelocity() const {
        return (escapeCount > 0) ? totalEscapeVelocity / escapeCount : 0.0;
    }

    // Get average probe score (returns 0 if no probes resolved)
    double GetAvgProbeScore() const {
        return (probesResolved > 0) ? totalProbeScore / probesResolved : 0.0;
    }

    // Get probe hit rate as percentage (returns 0 if no probes resolved)
    double GetProbeHitRate() const {
        return (probesResolved > 0) ? (static_cast<double>(probesHit) / probesResolved) * 100.0 : 0.0;
    }
};

// ============================================================================
// SESSION ENGAGEMENT ACCUMULATOR (Per-Anchor Engagement SSOT)
// ============================================================================
//
// BACKFILL STABILITY INVARIANT:
// =============================
// This accumulator is the SSOT for session engagement statistics by anchor type.
// - CalculateSessionStats() MUST read from this, NOT from zone.lifetime* fields
// - When zones are cleared/recreated (backfill, recalc), stats persist here
// - Only Reset() on explicit session roll
//
// OWNERSHIP:
// ==========
// Instance lives in StudyState (AuctionSensor_v1.cpp), NOT in ZoneManager.
// This ensures stats survive zone destruction.
//
// WIRING PATTERN:
// ===============
// 1. Zone finalizes engagement → returns FinalizationResult
// 2. Study receives result via callback or direct return
// 3. Study calls: engagementAccum.RecordEngagement(zone.type, result.touchRecord)
// 4. CalculateSessionStats reads from engagementAccum
//
// ============================================================================

/**
 * Engagement stats for a single anchor type (POC, VAH, VAL, etc.)
 * Updated via FinalizationResult, NOT by reading zone objects.
 */
struct AnchorEngagementStats {
    int touchCount = 0;
    int acceptances = 0;
    int rejections = 0;
    int tags = 0;
    int unresolved = 0;
    int probes = 0;      // Subset of rejections
    int tests = 0;       // Subset of rejections

    void RecordEngagement(AMT::TouchType type) {
        touchCount++;
        switch (type) {
            case AMT::TouchType::TAG:
                tags++;
                break;
            case AMT::TouchType::PROBE:
                rejections++;
                probes++;
                break;
            case AMT::TouchType::TEST:
                rejections++;
                tests++;
                break;
            case AMT::TouchType::ACCEPTANCE:
                acceptances++;
                break;
            case AMT::TouchType::UNRESOLVED:
                unresolved++;
                break;
        }
    }

    double GetAcceptanceRateOfAttempts() const {
        return (touchCount > 0) ? static_cast<double>(acceptances) / touchCount : 0.0;
    }

    double GetAcceptanceRateOfDecisions() const {
        int decisions = acceptances + rejections;
        return (decisions > 0) ? static_cast<double>(acceptances) / decisions : 0.0;
    }

    void Reset() {
        touchCount = 0;
        acceptances = 0;
        rejections = 0;
        tags = 0;
        unresolved = 0;
        probes = 0;
        tests = 0;
    }
};

/**
 * Session-level engagement accumulator by anchor type.
 * SSOT for engagement statistics - survives zone destruction.
 */
struct SessionEngagementAccumulator {
    AnchorEngagementStats poc;
    AnchorEngagementStats vah;
    AnchorEngagementStats val;
    AnchorEngagementStats vwap;
    AnchorEngagementStats ibHigh;
    AnchorEngagementStats ibLow;

    int totalEngagements = 0;

    /**
     * Record an engagement from a finalized zone.
     * @param type The zone type (determines which bucket)
     * @param touchType The touch classification from FinalizationResult
     */
    void RecordEngagement(AMT::ZoneType type, AMT::TouchType touchType) {
        totalEngagements++;

        switch (type) {
            case AMT::ZoneType::VPB_POC:
                poc.RecordEngagement(touchType);
                break;
            case AMT::ZoneType::VPB_VAH:
                vah.RecordEngagement(touchType);
                break;
            case AMT::ZoneType::VPB_VAL:
                val.RecordEngagement(touchType);
                break;
            case AMT::ZoneType::VWAP:
                vwap.RecordEngagement(touchType);
                break;
            case AMT::ZoneType::IB_HIGH:
                ibHigh.RecordEngagement(touchType);
                break;
            case AMT::ZoneType::IB_LOW:
                ibLow.RecordEngagement(touchType);
                break;
            default:
                // Other zone types not tracked at session level
                break;
        }
    }

    void Reset() {
        poc.Reset();
        vah.Reset();
        val.Reset();
        vwap.Reset();
        ibHigh.Reset();
        ibLow.Reset();
        totalEngagements = 0;
    }

    int GetTotalTouches() const {
        return poc.touchCount + vah.touchCount + val.touchCount +
               vwap.touchCount + ibHigh.touchCount + ibLow.touchCount;
    }
};

// ============================================================================
// SESSION SUMMARY SNAPSHOT (For high-level reporting)
// NOTE: This is DIFFERENT from AMT::SessionStatistics in AMT_Analytics.h
//       which contains detailed zone-based statistics.
// ============================================================================

struct SessionSummarySnapshot
{
    // Session identity
    AMT::SessionPhase sessionType = AMT::SessionPhase::UNKNOWN;
    int sessionBarCount = 0;

    // Copied from accumulators
    SessionAccumulators accum;

    // Derived metrics (computed at snapshot time)
    double avgEngagementBars = 0.0;
    double avgEscapeVelocity = 0.0;
    double avgProbeScore = 0.0;
    double probeHitRatePct = 0.0;

    // Value area metrics (copied from session context)
    double vaRangeTicks = 0.0;
    double pocPrice = 0.0;
    double vahPrice = 0.0;
    double valPrice = 0.0;

    void ComputeDerived() {
        avgEngagementBars = accum.GetAvgEngagementBars();
        avgEscapeVelocity = accum.GetAvgEscapeVelocity();
        avgProbeScore = accum.GetAvgProbeScore();
        probeHitRatePct = accum.GetProbeHitRate();
    }

    void Reset() {
        sessionType = AMT::SessionPhase::UNKNOWN;
        sessionBarCount = 0;
        accum.Reset();
        avgEngagementBars = 0.0;
        avgEscapeVelocity = 0.0;
        avgProbeScore = 0.0;
        probeHitRatePct = 0.0;
        vaRangeTicks = 0.0;
        pocPrice = 0.0;
        vahPrice = 0.0;
        valPrice = 0.0;
    }
};

// ============================================================================
// SESSION HISTORY ENTRY (Discovered Session Metadata)
// ============================================================================
// Lightweight record of a discovered session for baseline eligibility.
// Created during O(sessions) Phase 0 bootstrap scan.
// ============================================================================

struct SessionHistoryEntry {
    AMT::SessionKey key;            // Session identity (YYYYMMDD + RTH/GLOBEX)
    int firstBarIndex = -1;         // First bar in this session
    int lastBarIndex = -1;          // Last bar in this session
    int barCount = 0;               // Number of bars (lastBarIndex - firstBarIndex + 1)

    // --- Historical VBP Levels (populated after discovery) ---
    double poc = 0.0;               // Point of Control
    double vah = 0.0;               // Value Area High
    double val = 0.0;               // Value Area Low
    double vwap = 0.0;              // Session VWAP
    bool levelsPopulated = false;   // True after VBP/VWAP fetch succeeds

    bool IsValid() const {
        return key.IsValid() && firstBarIndex >= 0 && barCount > 0;
    }

    bool HasLevels() const {
        return levelsPopulated && poc > 0.0;
    }
};

// ============================================================================
// BASELINE SESSION MANAGER (SSOT for Three-Phase Execution)
// ============================================================================
// Controls the three-phase execution model:
//   Phase 0 (BOOTSTRAP): One-time O(sessions) discovery at recalc start
//   Phase 1 (BASELINE_ACCUMULATION): Bars in eligible sessions -> baseline only
//   Phase 2 (ACTIVE_SESSION): Current session -> full strategy logic
//
// Key invariant: Current session NEVER contributes to baselines.
// Primary gate: Session membership (not bar index).
// ============================================================================

struct BaselineSessionManager {
    // =========================================================================
    // SESSION-TYPE BASELINES: RTH and GBX are independent baseline domains.
    // Each domain uses only completed sessions of the matching type.
    // Current session NEVER contributes to its own baseline.
    // =========================================================================

    // --- Session Discovery Results (populated in Phase 0) ---
    AMT::SessionKey currentChartSessionKey;                 // Session of last bar on chart
    std::vector<SessionHistoryEntry> completedSessions;     // All completed sessions found

    // --- DUAL BASELINE DOMAINS (RTH and GBX are independent) ---
    std::set<AMT::SessionKey> eligibleRTHSessionKeys;       // Eligible RTH sessions for RTH baseline
    std::set<AMT::SessionKey> eligibleGBXSessionKeys;       // Eligible GBX sessions for GBX baseline

    // --- Per-Domain Tracking ---
    int rthBaselineBarCount = 0;        // Bars pushed to RTH baseline
    int gbxBaselineBarCount = 0;        // Bars pushed to GBX baseline
    int rthBaselineSessionCount = 0;    // Complete RTH sessions in baseline
    int gbxBaselineSessionCount = 0;    // Complete GBX sessions in baseline
    bool rthBaselineReady = false;      // True if >= 1 complete RTH session
    bool gbxBaselineReady = false;      // True if >= 1 complete GBX session

    // --- Phase Tracking ---
    AMT::BaselinePhase currentPhase = AMT::BaselinePhase::BOOTSTRAP;
    int activeSessionFirstBar = -1;     // First bar of current/active session
    bool sessionDiscoveryComplete = false;  // True after DiscoverSessions() runs

    // --- Active Session Baseline Lock ---
    // Once active session starts, baseline is locked (immutable for session duration)
    bool baselineLockedForSession = false;
    AMT::SessionType activeBaselineType = AMT::SessionType::GLOBEX;  // Which baseline domain is active

    // --- Configuration ---
    int maxBaselineSessions = 10;       // Max sessions per domain (default 10)

    // --- Profile Baselines (progress-conditioned) ---
    // Separate baselines for RTH and GBX domains
    // Forward declaration - HistoricalProfileBaseline is in AMT_VolumeProfile.h
    // We'll use a pointer pattern to avoid circular include
    bool profileBaselinesPopulated = false;

    // --- Reset all state (called on full recalc) ---
    void Reset() {
        currentChartSessionKey = AMT::SessionKey{};
        completedSessions.clear();
        eligibleRTHSessionKeys.clear();
        eligibleGBXSessionKeys.clear();
        rthBaselineBarCount = 0;
        gbxBaselineBarCount = 0;
        rthBaselineSessionCount = 0;
        gbxBaselineSessionCount = 0;
        rthBaselineReady = false;
        gbxBaselineReady = false;
        currentPhase = AMT::BaselinePhase::BOOTSTRAP;
        activeSessionFirstBar = -1;
        sessionDiscoveryComplete = false;
        baselineLockedForSession = false;
        activeBaselineType = AMT::SessionType::GLOBEX;
        profileBaselinesPopulated = false;
    }

    // --- O(sessions) Discovery Algorithm ---
    // Walks chart ONCE detecting session boundaries, NOT iterating every bar.
    // Stops early once current session is found.
    // Populates: currentChartSessionKey, completedSessions, eligibleRTH/GBXSessionKeys
    template<typename SCRef>
    void DiscoverSessions(SCRef& sc, int rthStartSec, int rthEndSec,
                          int /*maxBaselineBars*/ = 300, int maxSessions = 10)
    {
        Reset();
        maxBaselineSessions = maxSessions;

        const int lastBar = sc.ArraySize - 1;
        if (lastBar < 0) {
            sessionDiscoveryComplete = true;
            return;  // Empty chart
        }

        // 1. Compute currentChartSessionKey from LAST bar
        const SCDateTime lastBarTime = sc.BaseDateTimeIn[lastBar];
        const int lastDate = lastBarTime.GetYear() * 10000 + lastBarTime.GetMonth() * 100 + lastBarTime.GetDay();
        const int lastTime = lastBarTime.GetTimeInSeconds();
        currentChartSessionKey = AMT::ComputeSessionKey(lastDate, lastTime, rthStartSec, rthEndSec);

        // 2. Walk chart detecting session boundaries
        AMT::SessionKey prevKey{};
        int firstBarOfSession = 0;

        for (int i = 0; i < sc.ArraySize; ++i) {
            const SCDateTime barDateTime = sc.BaseDateTimeIn[i];
            const int barDate = barDateTime.GetYear() * 10000 + barDateTime.GetMonth() * 100 + barDateTime.GetDay();
            const int barTime = barDateTime.GetTimeInSeconds();
            const AMT::SessionKey barKey = AMT::ComputeSessionKey(barDate, barTime, rthStartSec, rthEndSec);

            // Detect session boundary
            if (barKey != prevKey && prevKey.IsValid()) {
                // Previous session just ended - record it if NOT the current session
                if (prevKey != currentChartSessionKey) {
                    SessionHistoryEntry entry;
                    entry.key = prevKey;
                    entry.firstBarIndex = firstBarOfSession;
                    entry.lastBarIndex = i - 1;
                    entry.barCount = i - firstBarOfSession;
                    completedSessions.push_back(entry);
                }
                firstBarOfSession = i;
            }

            // EARLY TERMINATION: Stop once we hit the current session
            if (barKey == currentChartSessionKey) {
                activeSessionFirstBar = i;
                break;
            }

            prevKey = barKey;
        }

        // 3. Build SEPARATE eligibility sets for RTH and GBX (newest first)
        // No bar caps - entire completed sessions only
        int rthCount = 0, gbxCount = 0;
        for (int i = static_cast<int>(completedSessions.size()) - 1; i >= 0; --i) {
            const auto& entry = completedSessions[i];
            if (entry.key.sessionType == AMT::SessionType::RTH && rthCount < maxBaselineSessions) {
                eligibleRTHSessionKeys.insert(entry.key);
                rthCount++;
            }
            else if (entry.key.sessionType == AMT::SessionType::GLOBEX && gbxCount < maxBaselineSessions) {
                eligibleGBXSessionKeys.insert(entry.key);
                gbxCount++;
            }
        }

        // Set baseline readiness based on discovered eligible sessions
        rthBaselineSessionCount = static_cast<int>(eligibleRTHSessionKeys.size());
        gbxBaselineSessionCount = static_cast<int>(eligibleGBXSessionKeys.size());
        rthBaselineReady = (rthBaselineSessionCount >= 1);
        gbxBaselineReady = (gbxBaselineSessionCount >= 1);

        sessionDiscoveryComplete = true;
    }

    // --- SESSION-INDEXED Eligibility by Type ---
    bool IsRTHEligible(const AMT::SessionKey& sessionKey) const {
        return sessionKey.sessionType == AMT::SessionType::RTH &&
               eligibleRTHSessionKeys.count(sessionKey) > 0;
    }

    bool IsGBXEligible(const AMT::SessionKey& sessionKey) const {
        return sessionKey.sessionType == AMT::SessionType::GLOBEX &&
               eligibleGBXSessionKeys.count(sessionKey) > 0;
    }

    // Combined eligibility check (type-matched)
    bool IsBaselineEligibleSession(const AMT::SessionKey& sessionKey) const {
        if (sessionKey.sessionType == AMT::SessionType::RTH) {
            return eligibleRTHSessionKeys.count(sessionKey) > 0;
        } else {
            return eligibleGBXSessionKeys.count(sessionKey) > 0;
        }
    }

    // --- Check if bar belongs to current/active session ---
    bool IsActiveSessionBar(const AMT::SessionKey& barSessionKey) const {
        return barSessionKey == currentChartSessionKey;
    }

    // --- Get baseline type for current session ---
    AMT::SessionType GetActiveBaselineType() const {
        return currentChartSessionKey.sessionType;
    }

    // --- Check baseline readiness by type ---
    bool IsBaselineReadyForType(AMT::SessionType type) const {
        return (type == AMT::SessionType::RTH) ? rthBaselineReady : gbxBaselineReady;
    }

    // --- Update phase based on bar's session membership ---
    AMT::BaselinePhase UpdatePhase(const AMT::SessionKey& barSessionKey) {
        if (!sessionDiscoveryComplete) {
            currentPhase = AMT::BaselinePhase::BOOTSTRAP;
            return currentPhase;
        }

        if (IsActiveSessionBar(barSessionKey)) {
            // Transition to active session
            if (currentPhase != AMT::BaselinePhase::ACTIVE_SESSION) {
                // Lock the baseline for this session type
                activeBaselineType = barSessionKey.sessionType;
                baselineLockedForSession = true;
            }
            currentPhase = AMT::BaselinePhase::ACTIVE_SESSION;
        }
        else if (IsBaselineEligibleSession(barSessionKey)) {
            // Eligible prior session - accumulate to matching domain
            currentPhase = AMT::BaselinePhase::BASELINE_ACCUMULATION;
        }
        else {
            // Non-eligible historical bar (too old or wrong type)
            if (currentPhase != AMT::BaselinePhase::ACTIVE_SESSION) {
                currentPhase = AMT::BaselinePhase::BASELINE_ACCUMULATION;
            }
        }

        return currentPhase;
    }

    // --- Increment baseline count for session type ---
    void IncrementBaselineCount(AMT::SessionType type) {
        if (type == AMT::SessionType::RTH) {
            rthBaselineBarCount++;
        } else {
            gbxBaselineBarCount++;
        }
    }

    // --- Mark session complete in baseline (call at session boundary) ---
    void MarkSessionComplete(AMT::SessionType type) {
        if (type == AMT::SessionType::RTH) {
            rthBaselineSessionCount++;
            if (rthBaselineSessionCount >= 1) {
                rthBaselineReady = true;
            }
        } else {
            gbxBaselineSessionCount++;
            if (gbxBaselineSessionCount >= 1) {
                gbxBaselineReady = true;
            }
        }
    }

    // --- Diagnostic logging of discovery results ---
    template<typename SCRef>
    void LogDiscoveryResults(SCRef& sc, int diagLevel) const {
        if (diagLevel < 1) return;

        char buf[512];
        snprintf(buf, sizeof(buf),
            "[PHASE-0] Bootstrap | RTH=%d sessions | GBX=%d sessions | "
            "ActiveSession=%s | ActiveStart=%d",
            static_cast<int>(eligibleRTHSessionKeys.size()),
            static_cast<int>(eligibleGBXSessionKeys.size()),
            currentChartSessionKey.ToString().c_str(),
            activeSessionFirstBar);
        sc.AddMessageToLog(buf, 0);

        // Log eligible sessions by type if high diag level
        if (diagLevel >= 2) {
            for (const auto& entry : completedSessions) {
                const bool isRTHElig = eligibleRTHSessionKeys.count(entry.key) > 0;
                const bool isGBXElig = eligibleGBXSessionKeys.count(entry.key) > 0;
                if (isRTHElig || isGBXElig) {
                    snprintf(buf, sizeof(buf),
                        "[BASELINE-%s] %s [%d-%d] (%d bars)",
                        isRTHElig ? "RTH" : "GBX",
                        entry.key.ToString().c_str(),
                        entry.firstBarIndex,
                        entry.lastBarIndex,
                        entry.barCount);
                    sc.AddMessageToLog(buf, 0);
                }
            }
        }
    }

    // --- Get baseline bar count by type ---
    int GetBaselineBarCount(AMT::SessionType type) const {
        return (type == AMT::SessionType::RTH) ? rthBaselineBarCount : gbxBaselineBarCount;
    }

    // --- Get baseline session count by type ---
    int GetBaselineSessionCount(AMT::SessionType type) const {
        return (type == AMT::SessionType::RTH) ? rthBaselineSessionCount : gbxBaselineSessionCount;
    }

    // --- Check if baseline is ready for current session type ---
    bool IsBaselineReady() const {
        return IsBaselineReadyForType(currentChartSessionKey.sessionType);
    }

    // --- Populate historical VBP/VWAP levels for all completed sessions ---
    // Call this AFTER DiscoverSessions() completes.
    // Reads from VBP study (Inputs 22-24) and VWAP study (Input 50).
    template<typename SCRef>
    void PopulateHistoricalLevels(SCRef& sc, int diagLevel) {
        if (completedSessions.empty()) return;

        // Get study arrays for POC/VAH/VAL (VBP study, Inputs 22-24)
        SCFloatArray pocArray, vahArray, valArray, vwapArray;

        // VBP subgraphs
        const int vbpPocStudyId = sc.Input[22].GetStudyID();
        const int vbpPocSG = sc.Input[22].GetSubgraphIndex();
        const int vbpVahStudyId = sc.Input[23].GetStudyID();
        const int vbpVahSG = sc.Input[23].GetSubgraphIndex();
        const int vbpValStudyId = sc.Input[24].GetStudyID();
        const int vbpValSG = sc.Input[24].GetSubgraphIndex();

        // VWAP (Input 50)
        const int vwapStudyId = sc.Input[50].GetStudyID();
        const int vwapSG = sc.Input[50].GetSubgraphIndex();

        // Fetch arrays
        if (vbpPocStudyId > 0) sc.GetStudyArrayUsingID(vbpPocStudyId, vbpPocSG, pocArray);
        if (vbpVahStudyId > 0) sc.GetStudyArrayUsingID(vbpVahStudyId, vbpVahSG, vahArray);
        if (vbpValStudyId > 0) sc.GetStudyArrayUsingID(vbpValStudyId, vbpValSG, valArray);
        if (vwapStudyId > 0)   sc.GetStudyArrayUsingID(vwapStudyId, vwapSG, vwapArray);

        const int pocSize = pocArray.GetArraySize();
        const int vahSize = vahArray.GetArraySize();
        const int valSize = valArray.GetArraySize();
        const int vwapSize = vwapArray.GetArraySize();

        int populated = 0;
        for (auto& entry : completedSessions) {
            const int barIdx = entry.lastBarIndex;
            if (barIdx < 0) continue;

            // Read POC/VAH/VAL at session's last bar
            if (barIdx < pocSize) entry.poc = pocArray[barIdx];
            if (barIdx < vahSize) entry.vah = vahArray[barIdx];
            if (barIdx < valSize) entry.val = valArray[barIdx];
            if (barIdx < vwapSize) entry.vwap = vwapArray[barIdx];

            // Mark as populated if we got valid POC
            entry.levelsPopulated = (entry.poc > 0.0);
            if (entry.levelsPopulated) populated++;
        }

        if (diagLevel >= 1) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "[HIST-LEVELS] Populated %d/%d sessions with VBP/VWAP levels",
                populated, static_cast<int>(completedSessions.size()));
            sc.AddMessageToLog(buf, 0);
        }

        // Log individual session levels at high diag level
        if (diagLevel >= 2) {
            for (const auto& entry : completedSessions) {
                if (entry.levelsPopulated) {
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                        "[HIST-LEVELS] %s | POC=%.2f VAH=%.2f VAL=%.2f VWAP=%.2f",
                        entry.key.ToString().c_str(),
                        entry.poc, entry.vah, entry.val, entry.vwap);
                    sc.AddMessageToLog(buf, 0);
                }
            }
        }
    }

    // --- Get session entry by index (for external access) ---
    const SessionHistoryEntry* GetSession(int index) const {
        if (index < 0 || index >= static_cast<int>(completedSessions.size())) {
            return nullptr;
        }
        return &completedSessions[index];
    }

    // --- Get session count ---
    int GetSessionCount() const {
        return static_cast<int>(completedSessions.size());
    }

    // --- Find session by key ---
    const SessionHistoryEntry* FindSession(const AMT::SessionKey& key) const {
        for (const auto& entry : completedSessions) {
            if (entry.key == key) return &entry;
        }
        return nullptr;
    }

    // --- Get prior session for a given type ---
    const SessionHistoryEntry* GetPriorSession(AMT::SessionType type) const {
        // Walk backwards to find the most recent session of the given type
        for (int i = static_cast<int>(completedSessions.size()) - 1; i >= 0; --i) {
            if (completedSessions[i].key.sessionType == type) {
                return &completedSessions[i];
            }
        }
        return nullptr;
    }
};

#endif // AMT_SESSION_H
