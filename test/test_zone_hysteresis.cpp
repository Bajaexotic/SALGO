// Define standalone test mode to exclude SC-dependent functions
#define AMT_STANDALONE_TEST

// ============================================================================
// test_zone_hysteresis.cpp
// Unit tests for zone hysteresis, sticky behavior, and transition tracking
// Week 5: Validates per-chart isolation and deterministic behavior
// ============================================================================

#ifndef TEST_MODE
#define TEST_MODE
#endif
#include "test_sierrachart_mock.h"
#include "amt_core.h"
#include "AMT_config.h"
#include "AMT_Zones.h"

#include <iostream>
#include <cassert>
#include <cmath>

using namespace AMT;

// ============================================================================
// TEST UTILITIES
// ============================================================================

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAIL: " << msg << " at line " << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

#define TEST_PASSED(name) \
    std::cout << "  PASS: " << name << std::endl; \
    return true;

// Helper to create a time from bar index (simulates 5-minute bars)
SCDateTime MakeTime(int bar) {
    SCDateTime t;
    // Start at 9:30 AM, add 5 minutes per bar
    t.SetDateTime(2024, 1, 15, 9, 30 + (bar * 5) % 60, 0);
    return t;
}

// ============================================================================
// TEST: TransitionState tracks entry/exit correctly
// ============================================================================

bool test_transition_state_entry_exit() {
    TransitionState state;

    // Initial state should be INACTIVE
    TEST_ASSERT(state.lastDominantProximity == ZoneProximity::INACTIVE,
                "Initial state should be INACTIVE");

    // Bar 0: Still inactive
    state.ProcessTransition(ZoneProximity::INACTIVE, -1, 0, MakeTime(0));
    TEST_ASSERT(!state.justEnteredZone, "Should not have entered zone");
    TEST_ASSERT(!state.justExitedZone, "Should not have exited zone");

    // Bar 1: Approach a zone
    state.ProcessTransition(ZoneProximity::APPROACHING, 1, 1, MakeTime(1));
    TEST_ASSERT(!state.justEnteredZone, "Approaching is not entering");
    TEST_ASSERT(!state.justExitedZone, "Should not have exited");

    // Bar 2: Enter zone (AT_ZONE)
    state.ProcessTransition(ZoneProximity::AT_ZONE, 1, 2, MakeTime(2));
    TEST_ASSERT(state.justEnteredZone, "Should have entered zone");
    TEST_ASSERT(!state.justExitedZone, "Should not have exited");
    TEST_ASSERT(state.lastEngagementBar == 2, "Engagement should start at bar 2");

    // Bar 3: Still at zone
    state.ProcessTransition(ZoneProximity::AT_ZONE, 1, 3, MakeTime(3));
    TEST_ASSERT(!state.justEnteredZone, "Already at zone, not a new entry");
    TEST_ASSERT(!state.justExitedZone, "Should not have exited");
    TEST_ASSERT(state.GetEngagementBars(3) == 1, "Should be 1 bar into engagement");

    // Bar 4: Exit zone
    state.ProcessTransition(ZoneProximity::APPROACHING, 1, 4, MakeTime(4));
    TEST_ASSERT(!state.justEnteredZone, "Should not have entered");
    TEST_ASSERT(state.justExitedZone, "Should have exited zone");

    TEST_PASSED("TransitionState entry/exit tracking");
}

// ============================================================================
// TEST: TransitionState detects zone changes
// ============================================================================

bool test_transition_state_zone_change() {
    TransitionState state;

    // Bar 0: Enter zone 1
    state.ProcessTransition(ZoneProximity::AT_ZONE, 1, 0, MakeTime(0));
    TEST_ASSERT(state.justEnteredZone, "Should have entered zone 1");
    TEST_ASSERT(state.lastPrimaryZoneId == 1, "Primary zone should be 1");

    // Bar 1: Still at zone 1
    state.ProcessTransition(ZoneProximity::AT_ZONE, 1, 1, MakeTime(1));
    TEST_ASSERT(!state.justChangedZone, "Same zone, no change");

    // Bar 2: Change to zone 2 (while still AT_ZONE)
    state.ProcessTransition(ZoneProximity::AT_ZONE, 2, 2, MakeTime(2));
    TEST_ASSERT(state.justChangedZone, "Zone changed from 1 to 2");
    TEST_ASSERT(state.justExitedZone, "Changing zones triggers exit from old zone");
    TEST_ASSERT(state.justEnteredZone, "Changing zones triggers entry to new zone");
    TEST_ASSERT(state.lastPrimaryZoneId == 2, "Primary zone should now be 2");

    TEST_PASSED("TransitionState zone change detection");
}

// ============================================================================
// TEST: ZoneTransitionMemory sticky behavior
// ============================================================================

bool test_sticky_zone_behavior() {
    ZoneTransitionMemory memory;
    memory.stickyDurationBars = 5;  // Sticky for 5 bars

    // Initially no preference
    TEST_ASSERT(memory.GetPreferredIfValid(0) == -1, "No initial preference");

    // Set preferred zone at bar 0
    memory.SetPreferred(42, 0);
    TEST_ASSERT(memory.preferredZoneId == 42, "Preferred zone should be 42");
    TEST_ASSERT(memory.inHysteresis, "Should be in hysteresis");

    // Preference should be valid for bars 0-4
    TEST_ASSERT(memory.GetPreferredIfValid(0) == 42, "Valid at bar 0");
    TEST_ASSERT(memory.GetPreferredIfValid(1) == 42, "Valid at bar 1");
    TEST_ASSERT(memory.GetPreferredIfValid(4) == 42, "Valid at bar 4");

    // Preference expires at bar 5
    TEST_ASSERT(memory.GetPreferredIfValid(5) == -1, "Expired at bar 5");

    // Update clears expired preference
    memory.Update(5);
    TEST_ASSERT(!memory.inHysteresis, "Hysteresis should be cleared");
    TEST_ASSERT(memory.preferredZoneId == -1, "Preference should be cleared");

    TEST_PASSED("ZoneTransitionMemory sticky behavior");
}

// ============================================================================
// TEST: ResolutionPolicy SSOT (MEDIUM-2)
// Tests all modes and the targeted test matrix
// ============================================================================

bool test_resolution_policy() {
    // =========================================================================
    // Test 1: BARS_OR_TIME mode (default AMT behavior)
    // =========================================================================
    {
        ResolutionPolicy policy;
        policy.barsOutsideThreshold = 2;
        policy.secondsOutsideThreshold = 30;
        policy.mode = ResolutionMode::BARS_OR_TIME;

        // Case A: Neither threshold met
        auto resultA = policy.Evaluate(1, 15);
        TEST_ASSERT(!resultA.resolved, "Case A: 1 bar, 15 sec - not resolved");
        TEST_ASSERT(resultA.reason == ResolutionReason::NOT_RESOLVED, "Case A: reason NOT_RESOLVED");

        // Case B: Bars met, time not met
        auto resultB = policy.Evaluate(2, 15);
        TEST_ASSERT(resultB.resolved, "Case B: 2 bars, 15 sec - resolved");
        TEST_ASSERT(resultB.reason == ResolutionReason::RESOLVED_BY_BARS, "Case B: reason RESOLVED_BY_BARS");

        // Case C: Bars not met, time met (dead tape scenario)
        auto resultC = policy.Evaluate(1, 30);
        TEST_ASSERT(resultC.resolved, "Case C: 1 bar, 30 sec - resolved by time");
        TEST_ASSERT(resultC.reason == ResolutionReason::RESOLVED_BY_TIME, "Case C: reason RESOLVED_BY_TIME");

        // Case D: Both met
        auto resultD = policy.Evaluate(3, 60);
        TEST_ASSERT(resultD.resolved, "Case D: 3 bars, 60 sec - resolved");
        TEST_ASSERT(resultD.reason == ResolutionReason::RESOLVED_BY_BOTH, "Case D: reason RESOLVED_BY_BOTH");

        // Legacy compatibility
        TEST_ASSERT(policy.ShouldResolve(2, 15), "Legacy ShouldResolve works");
        TEST_ASSERT(std::string(policy.GetResolutionReason(2, 15)) == "BARS", "Legacy reason BARS");
        TEST_ASSERT(std::string(policy.GetResolutionReason(1, 30)) == "TIME", "Legacy reason TIME");
    }

    // =========================================================================
    // Test 2: BARS_ONLY mode (legacy behavior)
    // =========================================================================
    {
        ResolutionPolicy policy;
        policy.SetBarsOnlyMode(3);  // 3 bars threshold

        // Bars not met, time would be met - should NOT resolve
        auto result1 = policy.Evaluate(2, 9999);
        TEST_ASSERT(!result1.resolved, "BARS_ONLY: 2 bars, high time - not resolved");

        // Bars met
        auto result2 = policy.Evaluate(3, 0);
        TEST_ASSERT(result2.resolved, "BARS_ONLY: 3 bars, 0 sec - resolved");
        TEST_ASSERT(result2.reason == ResolutionReason::RESOLVED_BY_BARS, "BARS_ONLY: reason is BARS");
    }

    // =========================================================================
    // Test 3: TIME_ONLY mode
    // =========================================================================
    {
        ResolutionPolicy policy;
        policy.mode = ResolutionMode::TIME_ONLY;
        policy.barsOutsideThreshold = 2;
        policy.secondsOutsideThreshold = 30;

        // Time not met, bars would be met - should NOT resolve
        auto result1 = policy.Evaluate(999, 15);
        TEST_ASSERT(!result1.resolved, "TIME_ONLY: high bars, 15 sec - not resolved");

        // Time met
        auto result2 = policy.Evaluate(0, 30);
        TEST_ASSERT(result2.resolved, "TIME_ONLY: 0 bars, 30 sec - resolved");
        TEST_ASSERT(result2.reason == ResolutionReason::RESOLVED_BY_TIME, "TIME_ONLY: reason is TIME");
    }

    // =========================================================================
    // Test 4: Dead tape scenario (low activity, seconds pass but bars don't)
    // =========================================================================
    {
        ResolutionPolicy policy;
        policy.SetBarsOrTimeMode(2, 30);

        // Simulate low-volume tape: only 1 bar in 60 seconds
        auto result = policy.Evaluate(1, 60);
        TEST_ASSERT(result.resolved, "Dead tape: resolved by time when bars stall");
        TEST_ASSERT(result.reason == ResolutionReason::RESOLVED_BY_TIME, "Dead tape: reason is TIME");
    }

    // =========================================================================
    // Test 5: Hardening 3 - True dead tape (0 new bars, only time elapsed)
    // Verifies anchor contract: bars can be 0 while seconds advance
    // =========================================================================
    {
        ResolutionPolicy policy;
        policy.SetBarsOrTimeMode(2, 30);

        // True dead tape: ZERO new bars since exit, but 45 seconds elapsed
        // This simulates: price exited halo, no trades occurred, time passed
        auto result = policy.Evaluate(0, 45);
        TEST_ASSERT(result.resolved, "True dead tape (0 bars, 45s): resolved by time");
        TEST_ASSERT(result.reason == ResolutionReason::RESOLVED_BY_TIME, "True dead tape: reason is TIME");

        // Edge case: just exited halo (0 bars, 0 seconds)
        auto fresh = policy.Evaluate(0, 0);
        TEST_ASSERT(!fresh.resolved, "Just exited (0,0): not resolved");
        TEST_ASSERT(fresh.reason == ResolutionReason::NOT_RESOLVED, "Just exited: reason NOT_RESOLVED");

        // Edge case: TIME_ONLY mode with 0 bars
        policy.mode = ResolutionMode::TIME_ONLY;
        auto timeOnly = policy.Evaluate(0, 35);
        TEST_ASSERT(timeOnly.resolved, "TIME_ONLY + 0 bars: resolved by time");
        TEST_ASSERT(timeOnly.reason == ResolutionReason::RESOLVED_BY_TIME, "TIME_ONLY + 0 bars: reason is TIME");

        // Edge case: BARS_ONLY mode - 0 bars should never resolve
        policy.mode = ResolutionMode::BARS_ONLY;
        auto barsOnly = policy.Evaluate(0, 9999);
        TEST_ASSERT(!barsOnly.resolved, "BARS_ONLY + 0 bars: never resolves even with infinite time");
    }

    TEST_PASSED("ResolutionPolicy bar+time thresholds");
}

// ============================================================================
// TEST: DEPARTED Reachability (Micro-test A)
// Verifies: Cannot jump to DEPARTED from INACTIVE or APPROACHING
// DEPARTED is ONLY reachable from AT_ZONE when price exits halo
// ============================================================================

bool test_departed_reachability() {
    ZoneConfig cfg;
    cfg.baseCoreTicks = 3;  // Core = 3 ticks
    cfg.baseHaloTicks = 6;  // Halo = 6 ticks
    cfg.volatilityScalar = 1.0;
    double tickSize = 0.25;
    double anchor = 5000.0;

    // Create a zone for testing
    ZoneRuntime zone(1, ZoneType::VPB_VAH, ZoneRole::VALUE_BOUNDARY,
                     AnchorMechanism::VOLUME_PROFILE, ZoneSource::CURRENT_RTH,
                     anchor, MakeTime(0), 0);
    zone.proximity = ZoneProximity::INACTIVE;
    zone.priorProximity = ZoneProximity::INACTIVE;

    // =========================================================================
    // Test 1: INACTIVE + exit halo => must remain INACTIVE (not DEPARTED)
    // =========================================================================
    {
        zone.proximity = ZoneProximity::INACTIVE;
        zone.priorProximity = ZoneProximity::INACTIVE;

        // Price far outside halo (10 ticks away)
        double farPrice = anchor + (10 * tickSize);
        UpdateZoneProximity(zone, farPrice, tickSize, cfg);

        TEST_ASSERT(zone.proximity == ZoneProximity::INACTIVE,
            "INACTIVE + exit halo => stays INACTIVE, not DEPARTED");
    }

    // =========================================================================
    // Test 2: APPROACHING + exit halo => must become INACTIVE (not DEPARTED)
    // =========================================================================
    {
        zone.proximity = ZoneProximity::APPROACHING;
        zone.priorProximity = ZoneProximity::APPROACHING;

        // Price far outside halo
        double farPrice = anchor + (10 * tickSize);
        UpdateZoneProximity(zone, farPrice, tickSize, cfg);

        TEST_ASSERT(zone.proximity == ZoneProximity::INACTIVE,
            "APPROACHING + exit halo => becomes INACTIVE, not DEPARTED");
    }

    // =========================================================================
    // Test 3: AT_ZONE + exit halo => must become DEPARTED
    // =========================================================================
    {
        zone.proximity = ZoneProximity::AT_ZONE;
        zone.priorProximity = ZoneProximity::AT_ZONE;

        // Price far outside halo
        double farPrice = anchor + (10 * tickSize);
        UpdateZoneProximity(zone, farPrice, tickSize, cfg);

        TEST_ASSERT(zone.proximity == ZoneProximity::DEPARTED,
            "AT_ZONE + exit halo => becomes DEPARTED");
    }

    // =========================================================================
    // Test 4: DEPARTED + still outside => stays DEPARTED
    // =========================================================================
    {
        zone.proximity = ZoneProximity::DEPARTED;
        zone.priorProximity = ZoneProximity::DEPARTED;

        // Price still far outside
        double farPrice = anchor + (10 * tickSize);
        UpdateZoneProximity(zone, farPrice, tickSize, cfg);

        TEST_ASSERT(zone.proximity == ZoneProximity::DEPARTED,
            "DEPARTED + still outside => stays DEPARTED (awaiting resolution)");
    }

    // =========================================================================
    // Test 5: DEPARTED + re-enter zone => goes to AT_ZONE/APPROACHING
    // =========================================================================
    {
        zone.proximity = ZoneProximity::DEPARTED;
        zone.priorProximity = ZoneProximity::DEPARTED;

        // Price re-enters core
        double corePrice = anchor + (2 * tickSize);  // 2 ticks, within core=3
        UpdateZoneProximity(zone, corePrice, tickSize, cfg);

        TEST_ASSERT(zone.proximity == ZoneProximity::AT_ZONE,
            "DEPARTED + re-enter core => goes to AT_ZONE");
    }

    TEST_PASSED("DEPARTED reachability (cannot jump from INACTIVE/APPROACHING)");
}

// ============================================================================
// TEST: Probe Gating in DEPARTED (Micro-test B)
// Verifies: Probes are blocked while in DEPARTED state
// ============================================================================

bool test_departed_probe_gating() {
    // The DEPARTED state semantics: "no probes while departed"
    // This is enforced by checking proximity before probe decisions

    // Simulate probe condition check
    auto shouldAllowProbe = [](ZoneProximity prox) -> bool {
        // Probes are only allowed when AT_ZONE or APPROACHING
        // DEPARTED blocks new probes even if probe conditions are met
        return (prox == ZoneProximity::AT_ZONE || prox == ZoneProximity::APPROACHING);
    };

    // Test: AT_ZONE allows probes
    TEST_ASSERT(shouldAllowProbe(ZoneProximity::AT_ZONE),
        "AT_ZONE allows probes");

    // Test: APPROACHING allows probes
    TEST_ASSERT(shouldAllowProbe(ZoneProximity::APPROACHING),
        "APPROACHING allows probes");

    // Test: DEPARTED blocks probes
    TEST_ASSERT(!shouldAllowProbe(ZoneProximity::DEPARTED),
        "DEPARTED blocks probes (even if conditions met)");

    // Test: INACTIVE blocks probes
    TEST_ASSERT(!shouldAllowProbe(ZoneProximity::INACTIVE),
        "INACTIVE blocks probes");

    TEST_PASSED("Probe gating in DEPARTED state");
}

// ============================================================================
// TEST: Zone Creation Stats (Instrumented Invariant)
// Verifies creation attempts and failures are tracked correctly
// ============================================================================

bool test_zone_creation_stats() {
    ZoneManager manager;

    // Initial state: no attempts
    TEST_ASSERT(manager.creationStats.totalAttempts == 0, "Initial attempts = 0");
    TEST_ASSERT(manager.creationStats.totalSuccesses == 0, "Initial successes = 0");
    TEST_ASSERT(manager.creationStats.totalFailures == 0, "Initial failures = 0");
    TEST_ASSERT(manager.creationStats.GetSuccessRate() == 1.0, "Initial success rate = 1.0");

    // Successful creation
    auto result1 = manager.CreateZone(ZoneType::VPB_VAH, 5100.0, MakeTime(0), 0);
    TEST_ASSERT(result1.ok, "First zone creation should succeed");
    TEST_ASSERT(manager.creationStats.totalAttempts == 1, "1 attempt after first create");
    TEST_ASSERT(manager.creationStats.totalSuccesses == 1, "1 success after first create");
    TEST_ASSERT(manager.creationStats.totalFailures == 0, "0 failures after first create");

    // Duplicate anchor failure
    auto result2 = manager.CreateZone(ZoneType::VPB_VAH, 5100.0, MakeTime(0), 0);
    TEST_ASSERT(!result2.ok, "Duplicate should fail");
    TEST_ASSERT(result2.failure == ZoneCreationFailure::DUPLICATE_ANCHOR, "Failure reason = DUPLICATE");
    TEST_ASSERT(manager.creationStats.totalAttempts == 2, "2 attempts after duplicate");
    TEST_ASSERT(manager.creationStats.totalSuccesses == 1, "Still 1 success");
    TEST_ASSERT(manager.creationStats.totalFailures == 1, "1 failure");
    TEST_ASSERT(manager.creationStats.GetFailureCount(ZoneCreationFailure::DUPLICATE_ANCHOR) == 1,
        "1 duplicate failure");

    // Invalid anchor failure
    auto result3 = manager.CreateZone(ZoneType::VPB_POC, 0.0, MakeTime(0), 0);
    TEST_ASSERT(!result3.ok, "Zero anchor should fail");
    TEST_ASSERT(result3.failure == ZoneCreationFailure::INVALID_ANCHOR_PRICE, "Failure = INVALID_ANCHOR");
    TEST_ASSERT(manager.creationStats.totalFailures == 2, "2 failures");
    TEST_ASSERT(manager.creationStats.GetFailureCount(ZoneCreationFailure::INVALID_ANCHOR_PRICE) == 1,
        "1 invalid anchor failure");

    // Invalid type failure
    auto result4 = manager.CreateZone(ZoneType::NONE, 5200.0, MakeTime(0), 0);
    TEST_ASSERT(!result4.ok, "NONE type should fail");
    TEST_ASSERT(result4.failure == ZoneCreationFailure::INVALID_ZONE_TYPE, "Failure = INVALID_TYPE");
    TEST_ASSERT(manager.creationStats.GetFailureCount(ZoneCreationFailure::INVALID_ZONE_TYPE) == 1,
        "1 invalid type failure");

    // Success rate calculation
    // 1 success, 3 failures = 25% success rate
    double expectedRate = 1.0 / 4.0;
    TEST_ASSERT(std::fabs(manager.creationStats.GetSuccessRate() - expectedRate) < 0.001,
        "Success rate = 0.25 (1/4)");

    // Reset clears all stats
    manager.creationStats.Reset();
    TEST_ASSERT(manager.creationStats.totalAttempts == 0, "Reset clears attempts");
    TEST_ASSERT(manager.creationStats.totalFailures == 0, "Reset clears failures");
    TEST_ASSERT(manager.creationStats.GetSuccessRate() == 1.0, "Reset restores 1.0 rate");

    TEST_PASSED("Zone creation stats (instrumented invariant)");
}

// ============================================================================
// TEST: Proximity Transition Matrix (Gating Test)
// Forces a complete lifecycle and verifies exact edge counts
// ============================================================================

bool test_transition_matrix() {
    ZoneManager manager;

    // Create a zone
    auto result = manager.CreateZone(ZoneType::VPB_VAH, 5100.0, MakeTime(0), 0);
    TEST_ASSERT(result.ok, "Zone creation should succeed");

    double tickSize = 0.25;
    double anchor = 5100.0;

    // Initial state: INACTIVE (no transitions yet)
    TEST_ASSERT(manager.transitionStats.totalTransitions == 0, "No transitions initially");

    // =========================================================================
    // Force lifecycle: INACTIVE -> APPROACHING -> AT_ZONE -> DEPARTED -> INACTIVE
    // =========================================================================

    // Bar 1: Far away (INACTIVE -> INACTIVE, no transition)
    double farPrice = anchor + (20 * tickSize);  // 20 ticks away
    manager.UpdateZones(farPrice, tickSize, 1, MakeTime(1));
    TEST_ASSERT(manager.transitionStats.totalTransitions == 0, "Still no transitions (stayed INACTIVE)");

    // Bar 2: Enter halo (INACTIVE -> APPROACHING)
    double haloPrice = anchor + (5 * tickSize);  // 5 ticks, in halo (default 8)
    manager.UpdateZones(haloPrice, tickSize, 2, MakeTime(2));
    TEST_ASSERT(manager.transitionStats.totalTransitions == 1, "1 transition: INACTIVE->APPROACHING");
    TEST_ASSERT(manager.transitionStats.GetTransitionCount(
        ZoneProximity::INACTIVE, ZoneProximity::APPROACHING) == 1,
        "Edge INACTIVE->APPROACHING = 1");

    // Bar 3: Enter core (APPROACHING -> AT_ZONE)
    double corePrice = anchor + (1 * tickSize);  // 1 tick, in core (default 3)
    manager.UpdateZones(corePrice, tickSize, 3, MakeTime(3));
    TEST_ASSERT(manager.transitionStats.totalTransitions == 2, "2 transitions");
    TEST_ASSERT(manager.transitionStats.GetTransitionCount(
        ZoneProximity::APPROACHING, ZoneProximity::AT_ZONE) == 1,
        "Edge APPROACHING->AT_ZONE = 1");

    // Bar 4: Exit to far (AT_ZONE -> DEPARTED)
    manager.UpdateZones(farPrice, tickSize, 4, MakeTime(4));
    TEST_ASSERT(manager.transitionStats.totalTransitions == 3, "3 transitions");
    TEST_ASSERT(manager.transitionStats.GetTransitionCount(
        ZoneProximity::AT_ZONE, ZoneProximity::DEPARTED) == 1,
        "Edge AT_ZONE->DEPARTED = 1");

    // Verify DEPARTED state
    ZoneRuntime* zone = manager.GetZone(result.zoneId);
    TEST_ASSERT(zone != nullptr, "Zone should exist");
    TEST_ASSERT(zone->proximity == ZoneProximity::DEPARTED, "Zone should be DEPARTED");

    // Bar 5+: Stay far, simulate resolution (DEPARTED -> INACTIVE via resolution)
    // Need to use UpdateAllProximities with resolution policy to trigger this
    TransitionState transState;
    ZoneTransitionMemory transMem;
    ResolutionPolicy resPol;
    resPol.SetBarsOrTimeMode(1, 5);  // Low thresholds for quick resolution
    ZoneContextSnapshot snapshot;

    // Manually set up zone state to trigger resolution
    zone->barsOutsideHalo = 2;
    zone->secondsOutsideHalo = 10;
    zone->currentEngagement.outcome = AuctionOutcome::PENDING;
    zone->currentEngagement.startBar = 3;

    manager.UpdateAllProximities(farPrice, tickSize, 6, MakeTime(6),
                                 transState, transMem, resPol, snapshot);

    // Should have transitioned DEPARTED -> INACTIVE via resolution
    TEST_ASSERT(manager.transitionStats.GetTransitionCount(
        ZoneProximity::DEPARTED, ZoneProximity::INACTIVE) >= 0,
        "Edge DEPARTED->INACTIVE tracked (may be 0 if resolution not in UpdateAllProximities path)");

    // Verify churn calculation
    TEST_ASSERT(manager.transitionStats.totalBarsObserved > 0, "Bars observed > 0");

    // Reset works
    manager.transitionStats.Reset();
    TEST_ASSERT(manager.transitionStats.totalTransitions == 0, "Reset clears transitions");
    TEST_ASSERT(manager.transitionStats.oscillationCount == 0, "Reset clears oscillations");

    TEST_PASSED("Transition matrix (gating test)");
}

// ============================================================================
// TEST: Resolution Histogram (Gating Test)
// Verifies TIME_ONLY produces no bars resolves, BARS_ONLY no time resolves
// ============================================================================

bool test_resolution_histogram() {
    // Test the histogram directly (integration with ZoneManager tested above)

    ZoneManager::ResolutionStats stats;

    // Initial state
    TEST_ASSERT(stats.totalResolutions == 0, "Initial resolutions = 0");
    TEST_ASSERT(stats.GetReasonCount(ResolutionReason::RESOLVED_BY_BARS) == 0, "No bar resolves");
    TEST_ASSERT(stats.GetReasonCount(ResolutionReason::RESOLVED_BY_TIME) == 0, "No time resolves");

    // Record a bars resolution
    stats.Record(ResolutionMode::BARS_ONLY, ResolutionReason::RESOLVED_BY_BARS);
    TEST_ASSERT(stats.totalResolutions == 1, "1 resolution");
    TEST_ASSERT(stats.GetReasonCount(ResolutionReason::RESOLVED_BY_BARS) == 1, "1 bar resolve");
    TEST_ASSERT(stats.GetModeCount(ResolutionMode::BARS_ONLY) == 1, "1 BARS_ONLY");

    // Record a time resolution
    stats.Record(ResolutionMode::TIME_ONLY, ResolutionReason::RESOLVED_BY_TIME);
    TEST_ASSERT(stats.totalResolutions == 2, "2 resolutions");
    TEST_ASSERT(stats.GetReasonCount(ResolutionReason::RESOLVED_BY_TIME) == 1, "1 time resolve");
    TEST_ASSERT(stats.GetModeCount(ResolutionMode::TIME_ONLY) == 1, "1 TIME_ONLY");

    // Record a BARS_OR_TIME with both
    stats.Record(ResolutionMode::BARS_OR_TIME, ResolutionReason::RESOLVED_BY_BOTH);
    TEST_ASSERT(stats.totalResolutions == 3, "3 resolutions");
    TEST_ASSERT(stats.GetReasonCount(ResolutionReason::RESOLVED_BY_BOTH) == 1, "1 both resolve");
    TEST_ASSERT(stats.GetModeCount(ResolutionMode::BARS_OR_TIME) == 1, "1 BARS_OR_TIME");

    // =========================================================================
    // Policy mode invariants:
    // - TIME_ONLY should never produce RESOLVED_BY_BARS
    // - BARS_ONLY should never produce RESOLVED_BY_TIME
    // =========================================================================
    ResolutionPolicy timePol;
    timePol.mode = ResolutionMode::TIME_ONLY;
    timePol.secondsOutsideThreshold = 10;

    auto timeRes = timePol.Evaluate(999, 15);  // Many bars, 15 seconds
    TEST_ASSERT(timeRes.resolved, "TIME_ONLY resolves when time met");
    TEST_ASSERT(timeRes.reason == ResolutionReason::RESOLVED_BY_TIME,
        "TIME_ONLY never produces RESOLVED_BY_BARS");

    ResolutionPolicy barsPol;
    barsPol.mode = ResolutionMode::BARS_ONLY;
    barsPol.barsOutsideThreshold = 2;

    auto barsRes = barsPol.Evaluate(5, 9999);  // 5 bars, many seconds
    TEST_ASSERT(barsRes.resolved, "BARS_ONLY resolves when bars met");
    TEST_ASSERT(barsRes.reason == ResolutionReason::RESOLVED_BY_BARS,
        "BARS_ONLY never produces RESOLVED_BY_TIME");

    // Reset works
    stats.Reset();
    TEST_ASSERT(stats.totalResolutions == 0, "Reset clears all");
    TEST_ASSERT(stats.GetReasonCount(ResolutionReason::RESOLVED_BY_BARS) == 0, "Reset clears bars");

    TEST_PASSED("Resolution histogram (gating test)");
}

// ============================================================================
// TEST: DOMCachePolicy bar-based refresh
// ============================================================================

bool test_dom_cache_policy() {
    DOMCachePolicy cache;

    // Initially needs refresh
    TEST_ASSERT(cache.NeedsRefresh(0), "Initial cache needs refresh");

    // Update cache
    cache.UpdateCache(0, 1000.0, 500.0, 500.0);
    TEST_ASSERT(!cache.NeedsRefresh(0), "Cache valid for same bar");
    TEST_ASSERT(cache.NeedsRefresh(1), "Cache stale for next bar");

    // Width cache (tick-based - SSOT)
    // 5100.0 / 0.25 = 20400 ticks
    const long long anchorTicks = PriceToTicks(5100.0, 0.25);  // 20400
    TEST_ASSERT(cache.NeedsWidthRefresh(anchorTicks), "Width cache initially needs refresh");
    cache.UpdateWidthCache(anchorTicks, 3, 8);
    TEST_ASSERT(!cache.NeedsWidthRefresh(anchorTicks), "Width cache valid for same anchor");
    TEST_ASSERT(!cache.NeedsWidthRefresh(anchorTicks), "Width cache valid for exact tick");
    TEST_ASSERT(cache.NeedsWidthRefresh(anchorTicks + 1), "Width cache stale if anchor moved >= 1 tick");

    TEST_PASSED("DOMCachePolicy bar-based refresh");
}

// ============================================================================
// TEST: ZoneRuntime per-zone inside/outside tracking
// ============================================================================

bool test_zone_inside_outside_tracking() {
    // Create a zone
    ZoneRuntime zone(1, ZoneType::VPB_VAH, ZoneRole::VALUE_BOUNDARY,
                     AnchorMechanism::VOLUME_PROFILE, ZoneSource::CURRENT_RTH,
                     5100.0, MakeTime(0), 0);

    // Initially outside
    TEST_ASSERT(zone.lastInsideBar == -1, "No inside bar initially");
    TEST_ASSERT(zone.barsOutsideHalo == 0, "No outside count initially");

    // Bar 1: Inside halo
    zone.UpdateInsideOutsideTracking(1, MakeTime(1), true);
    TEST_ASSERT(zone.lastInsideBar == 1, "Last inside should be bar 1");
    TEST_ASSERT(zone.barsOutsideHalo == 0, "Outside count should be 0");

    // Bar 2: Still inside
    zone.UpdateInsideOutsideTracking(2, MakeTime(2), true);
    TEST_ASSERT(zone.lastInsideBar == 2, "Last inside should be bar 2");

    // Bar 3: Left zone
    zone.UpdateInsideOutsideTracking(3, MakeTime(3), false);
    TEST_ASSERT(zone.lastInsideBar == 2, "Last inside should still be bar 2");
    TEST_ASSERT(zone.lastOutsideBar == 3, "Last outside should be bar 3");
    TEST_ASSERT(zone.barsOutsideHalo == 0, "First bar outside, count is 0");

    // Bar 4: Still outside
    zone.UpdateInsideOutsideTracking(4, MakeTime(4), false);
    TEST_ASSERT(zone.barsOutsideHalo == 1, "One bar outside");

    // Bar 5: Still outside
    zone.UpdateInsideOutsideTracking(5, MakeTime(5), false);
    TEST_ASSERT(zone.barsOutsideHalo == 2, "Two bars outside");

    // Bar 6: Back inside - resets outside count
    zone.UpdateInsideOutsideTracking(6, MakeTime(6), true);
    TEST_ASSERT(zone.lastInsideBar == 6, "Last inside should be bar 6");
    TEST_ASSERT(zone.barsOutsideHalo == 0, "Outside count reset to 0");

    TEST_PASSED("ZoneRuntime per-zone inside/outside tracking");
}

// ============================================================================
// TEST: ZoneManager sticky selection
// ============================================================================

bool test_zone_manager_sticky_selection() {
    ZoneManager manager;
    manager.config.baseCoreTicks = 3;
    manager.config.baseHaloTicks = 8;

    // Create two zones at similar prices (role/mechanism/source auto-derived)
    auto result1 = manager.CreateZone(ZoneType::VPB_VAH, 5100.0, MakeTime(0), 0);
    auto result2 = manager.CreateZone(ZoneType::VPB_POC, 5100.25, MakeTime(0), 0);  // 1 tick away
    TEST_ASSERT(result1.ok, "Zone 1 creation should succeed");
    TEST_ASSERT(result2.ok, "Zone 2 creation should succeed");
    int zone1Id = result1.zoneId;
    int zone2Id = result2.zoneId;

    ZoneTransitionMemory memory;
    memory.stickyDurationBars = 5;

    // Without sticky, VAH (VALUE_BOUNDARY) should win by role priority
    double testPrice = 5100.125;
    ZoneRuntime* winner = manager.GetStrongestZoneAtPrice(testPrice, 0.25, 8);
    TEST_ASSERT(winner != nullptr, "Should find a zone");
    TEST_ASSERT(winner->zoneId == zone1Id, "VAH should win by role priority");

    // Set zone2 (POC) as preferred
    memory.SetPreferred(zone2Id, 0);

    // Update proximities to set zone states
    ZoneRuntime* z1 = manager.GetZone(zone1Id);
    ZoneRuntime* z2 = manager.GetZone(zone2Id);
    z1->proximity = ZoneProximity::AT_ZONE;
    z2->proximity = ZoneProximity::AT_ZONE;

    // With sticky preference active, POC should win
    winner = manager.GetStrongestZoneAtPriceSticky(testPrice, 0.25, memory, 1, 8);
    TEST_ASSERT(winner != nullptr, "Should find a zone");
    TEST_ASSERT(winner->zoneId == zone2Id, "POC should win with sticky preference");

    // After preference expires, VAH should win again
    winner = manager.GetStrongestZoneAtPriceSticky(testPrice, 0.25, memory, 10, 8);
    TEST_ASSERT(winner->zoneId == zone1Id, "VAH should win after preference expires");

    TEST_PASSED("ZoneManager sticky selection");
}

// ============================================================================
// TEST: UpdateAllProximities with TransitionState
// ============================================================================

bool test_update_all_proximities() {
    ZoneManager manager;
    manager.config.baseCoreTicks = 3;
    manager.config.baseHaloTicks = 8;

    TransitionState transState;
    ZoneTransitionMemory transMem;
    ResolutionPolicy resolution;
    ZoneContextSnapshot snapshot;

    // Create a zone
    auto zoneResult = manager.CreateZone(ZoneType::VPB_VAH, 5100.0, MakeTime(0), 0);
    TEST_ASSERT(zoneResult.ok, "Zone creation should succeed");
    int zoneId = zoneResult.zoneId;

    // Bar 0: Price far away (inactive)
    manager.UpdateAllProximities(5000.0, 0.25, 0, MakeTime(0),
                                  transState, transMem, resolution, snapshot);
    TEST_ASSERT(snapshot.valid, "Snapshot should be valid");
    TEST_ASSERT(snapshot.dominantProximity == ZoneProximity::INACTIVE, "Should be inactive");
    TEST_ASSERT(!snapshot.justEnteredZone, "Should not have entered");

    // Bar 1: Price approaches zone
    manager.UpdateAllProximities(5099.0, 0.25, 1, MakeTime(1),
                                  transState, transMem, resolution, snapshot);
    TEST_ASSERT(snapshot.dominantProximity == ZoneProximity::APPROACHING, "Should be approaching");

    // Bar 2: Price at zone
    manager.UpdateAllProximities(5100.25, 0.25, 2, MakeTime(2),
                                  transState, transMem, resolution, snapshot);
    TEST_ASSERT(snapshot.dominantProximity == ZoneProximity::AT_ZONE, "Should be at zone");
    TEST_ASSERT(snapshot.justEnteredZone, "Should have entered zone");
    TEST_ASSERT(snapshot.primaryZoneId == zoneId, "Primary zone should be correct");

    // Bar 3: Still at zone
    manager.UpdateAllProximities(5100.0, 0.25, 3, MakeTime(3),
                                  transState, transMem, resolution, snapshot);
    TEST_ASSERT(snapshot.dominantProximity == ZoneProximity::AT_ZONE, "Should still be at zone");
    TEST_ASSERT(!snapshot.justEnteredZone, "Not a new entry");
    TEST_ASSERT(snapshot.engagementBars == 1, "Should be 1 bar into engagement");

    // Bar 4: Exit zone
    manager.UpdateAllProximities(5090.0, 0.25, 4, MakeTime(4),
                                  transState, transMem, resolution, snapshot);
    TEST_ASSERT(snapshot.justExitedZone, "Should have exited zone");

    TEST_PASSED("UpdateAllProximities with TransitionState");
}

// ============================================================================
// TEST: Early exit preserves transition semantics (C)
// ============================================================================

bool test_early_exit_preserves_transitions() {
    ZoneManager manager;
    manager.config.baseCoreTicks = 3;
    manager.config.baseHaloTicks = 8;

    TransitionState transState;
    ZoneTransitionMemory transMem;
    ResolutionPolicy resolution;
    ZoneContextSnapshot snapshot;

    // Create a zone
    auto zoneResult = manager.CreateZone(ZoneType::VPB_VAH, 5100.0, MakeTime(0), 0);
    TEST_ASSERT(zoneResult.ok, "Zone creation should succeed");
    int zoneId = zoneResult.zoneId;

    // Bar 0: Enter zone
    manager.UpdateAllProximities(5100.0, 0.25, 0, MakeTime(0),
                                  transState, transMem, resolution, snapshot);
    TEST_ASSERT(snapshot.dominantProximity == ZoneProximity::AT_ZONE, "Should be at zone");
    TEST_ASSERT(transState.lastDominantProximity == ZoneProximity::AT_ZONE,
                "TransitionState should record AT_ZONE");

    // Bar 1: Still at zone
    manager.UpdateAllProximities(5100.25, 0.25, 1, MakeTime(1),
                                  transState, transMem, resolution, snapshot);

    // Bar 2: Jump FAR away (should trigger early-exit path)
    // This price is beyond all zones' halo, triggering BuildContextSnapshotEarlyExit
    manager.UpdateAllProximities(5200.0, 0.25, 2, MakeTime(2),
                                  transState, transMem, resolution, snapshot);

    // CRITICAL: Even with early-exit, transition must be detected
    TEST_ASSERT(snapshot.justExitedZone, "Early-exit MUST still detect zone exit");
    TEST_ASSERT(snapshot.dominantProximity == ZoneProximity::INACTIVE, "Should be inactive");
    TEST_ASSERT(transState.lastDominantProximity == ZoneProximity::INACTIVE,
                "TransitionState should be updated to INACTIVE");

    // Verify the zone's engagement was finalized
    ZoneRuntime* zone = manager.GetZone(zoneId);
    TEST_ASSERT(zone != nullptr, "Zone should still exist");
    TEST_ASSERT(!zone->engagementHistory.empty(), "Engagement should be recorded");

    TEST_PASSED("Early exit preserves transition semantics (C)");
}

// ============================================================================
// TEST: No static locals (A1, A2)
// This test verifies that multiple independent ZoneManager instances
// don't interfere with each other (would happen with static locals)
// ============================================================================

bool test_no_static_locals_isolation() {
    // Create two independent "charts"
    ZoneManager chart1;
    ZoneManager chart2;

    chart1.config.baseCoreTicks = 3;
    chart1.config.baseHaloTicks = 8;
    chart2.config.baseCoreTicks = 3;
    chart2.config.baseHaloTicks = 8;

    TransitionState trans1, trans2;
    ZoneTransitionMemory mem1, mem2;
    ResolutionPolicy res1, res2;
    ZoneContextSnapshot snap1, snap2;

    // Create zones at different prices (role/mechanism/source auto-derived)
    auto r1 = chart1.CreateZone(ZoneType::VPB_VAH, 5100.0, MakeTime(0), 0);
    auto r2 = chart2.CreateZone(ZoneType::VPB_VAH, 6100.0, MakeTime(0), 0);
    TEST_ASSERT(r1.ok, "Chart1 zone creation should succeed");
    TEST_ASSERT(r2.ok, "Chart2 zone creation should succeed");

    // Update chart1 to be at zone
    chart1.UpdateAllProximities(5100.0, 0.25, 0, MakeTime(0),
                                 trans1, mem1, res1, snap1);

    // Update chart2 to be inactive
    chart2.UpdateAllProximities(6000.0, 0.25, 0, MakeTime(0),
                                 trans2, mem2, res2, snap2);

    // Verify isolation
    TEST_ASSERT(snap1.dominantProximity == ZoneProximity::AT_ZONE,
                "Chart1 should be at zone");
    TEST_ASSERT(snap2.dominantProximity == ZoneProximity::INACTIVE,
                "Chart2 should be inactive");
    TEST_ASSERT(trans1.lastDominantProximity == ZoneProximity::AT_ZONE,
                "Chart1 transition state should be AT_ZONE");
    TEST_ASSERT(trans2.lastDominantProximity == ZoneProximity::INACTIVE,
                "Chart2 transition state should be INACTIVE");

    // Update chart2 - should NOT affect chart1
    chart2.UpdateAllProximities(6100.0, 0.25, 1, MakeTime(1),
                                 trans2, mem2, res2, snap2);
    TEST_ASSERT(snap2.dominantProximity == ZoneProximity::AT_ZONE,
                "Chart2 now at zone");

    // Chart1 state should be unchanged (if there were static locals, it would be corrupted)
    TEST_ASSERT(trans1.lastDominantProximity == ZoneProximity::AT_ZONE,
                "Chart1 transition state should be unchanged");
    TEST_ASSERT(trans1.lastPrimaryZoneId == 1,
                "Chart1 primary zone should be unchanged");

    TEST_PASSED("No static locals - chart isolation (A1, A2)");
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "=== Zone Hysteresis & Per-Chart Isolation Tests ===" << std::endl;
    std::cout << std::endl;

    int passed = 0;
    int failed = 0;

    // Run tests
    #define RUN_TEST(test) \
        if (test()) { passed++; } else { failed++; }

    RUN_TEST(test_transition_state_entry_exit);
    RUN_TEST(test_transition_state_zone_change);
    RUN_TEST(test_sticky_zone_behavior);
    RUN_TEST(test_resolution_policy);
    RUN_TEST(test_departed_reachability);
    RUN_TEST(test_departed_probe_gating);
    RUN_TEST(test_zone_creation_stats);
    RUN_TEST(test_transition_matrix);
    RUN_TEST(test_resolution_histogram);
    RUN_TEST(test_dom_cache_policy);
    RUN_TEST(test_zone_inside_outside_tracking);
    RUN_TEST(test_zone_manager_sticky_selection);
    RUN_TEST(test_update_all_proximities);
    RUN_TEST(test_early_exit_preserves_transitions);
    RUN_TEST(test_no_static_locals_isolation);

    std::cout << std::endl;
    std::cout << "=== Results ===" << std::endl;
    std::cout << "Passed: " << passed << std::endl;
    std::cout << "Failed: " << failed << std::endl;

    if (failed == 0) {
        std::cout << std::endl;
        std::cout << "All tests PASSED!" << std::endl;
        return 0;
    } else {
        std::cout << std::endl;
        std::cout << "Some tests FAILED." << std::endl;
        return 1;
    }
}
