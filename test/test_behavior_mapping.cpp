// ============================================================================
// test_behavior_mapping.cpp
// Unit tests for AMT_BehaviorMapping.h
// Tests outcome detection per specification v1.2
// ============================================================================

#include "../AMT_BehaviorMapping.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>

using namespace AMT;

// Test counters
static int g_testsRun = 0;
static int g_testsPassed = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_testsRun++; \
    if (!(cond)) { \
        std::cerr << "[FAIL] " << msg << " (line " << __LINE__ << ")\n"; \
    } else { \
        g_testsPassed++; \
        std::cout << "[PASS] " << msg << "\n"; \
    } \
} while(0)

// ============================================================================
// Helper: Create frozen references for testing
// ============================================================================

FrozenReferences createFrozenRefs(
    float POC, float VAH, float VAL,
    int t_freeze = 30,
    ProfileShape shape = ProfileShape::NORMAL_DISTRIBUTION,
    float asymmetry = 0.0f)
{
    FrozenReferences refs;
    refs.POC_0 = POC;
    refs.VAH_0 = VAH;
    refs.VAL_0 = VAL;
    refs.R_0 = 100.0f; // Arbitrary range
    refs.t_freeze = t_freeze;
    refs.shape = shape;
    refs.asymmetry = asymmetry;
    refs.ComputeDerived();
    refs.valid = true;
    return refs;
}

// ============================================================================
// TEST: O1 Continuation Up - Basic
// ============================================================================

void test_O1_basic_breakout() {
    std::cout << "\n=== Test: O1 Basic Breakout ===\n";

    // Setup: VAH=100, VAL=90, POC=95
    FrozenReferences refs = createFrozenRefs(95.0f, 100.0f, 90.0f, 30);

    BehaviorObservation obs;
    obs.Initialize(refs, 3, 0.25f);  // N=3 hold bars

    // Bar 31: Trigger bar - P_hi >= VAH (100)
    bool completed = OutcomeDetector::ProcessBar(obs, 31, 101.0f, 99.5f, 100.5f);
    TEST_ASSERT(!completed, "O1: Bar 31 should not complete (trigger only)");
    TEST_ASSERT(obs.upBreakout.IsActive(), "O1: Up breakout should be active");
    TEST_ASSERT(obs.upBreakout.holdBarsRemaining == 3, "O1: Should have 3 hold bars remaining");

    // Bar 32: Hold bar 1 - P_lo >= VAH (staying above)
    completed = OutcomeDetector::ProcessBar(obs, 32, 102.0f, 100.5f, 101.0f);
    TEST_ASSERT(!completed, "O1: Bar 32 should not complete (hold 1/3)");
    TEST_ASSERT(obs.upBreakout.holdBarsRemaining == 2, "O1: Should have 2 hold bars remaining");

    // Bar 33: Hold bar 2
    completed = OutcomeDetector::ProcessBar(obs, 33, 103.0f, 101.0f, 102.0f);
    TEST_ASSERT(!completed, "O1: Bar 33 should not complete (hold 2/3)");
    TEST_ASSERT(obs.upBreakout.holdBarsRemaining == 1, "O1: Should have 1 hold bar remaining");

    // Bar 34: Hold bar 3 - completes O1
    completed = OutcomeDetector::ProcessBar(obs, 34, 104.0f, 101.5f, 103.0f);
    TEST_ASSERT(completed, "O1: Bar 34 should complete O1");
    TEST_ASSERT(obs.outcome == BehaviorOutcome::O1_CONTINUATION_UP, "O1: Outcome should be O1");
    TEST_ASSERT(obs.completionBar == 34, "O1: Completion bar should be 34");
}

// ============================================================================
// TEST: O1 Failed Hold - Resets and Retries
// ============================================================================

void test_O1_failed_hold() {
    std::cout << "\n=== Test: O1 Failed Hold ===\n";

    FrozenReferences refs = createFrozenRefs(95.0f, 100.0f, 90.0f, 30);

    BehaviorObservation obs;
    obs.Initialize(refs, 3, 0.25f);

    // Bar 31: Trigger
    OutcomeDetector::ProcessBar(obs, 31, 101.0f, 99.5f, 100.5f);
    TEST_ASSERT(obs.upBreakout.IsActive(), "O1 Failed: Initial trigger active");

    // Bar 32: Hold bar 1
    OutcomeDetector::ProcessBar(obs, 32, 102.0f, 100.5f, 101.0f);

    // Bar 33: FAIL - P_lo drops below VAH, but P_hi doesn't trigger new breakout
    OutcomeDetector::ProcessBar(obs, 33, 99.0f, 98.0f, 98.5f);
    TEST_ASSERT(!obs.upBreakout.IsActive(), "O1 Failed: Breakout should reset on hold failure");
    TEST_ASSERT(obs.outcome == BehaviorOutcome::PENDING, "O1 Failed: Should still be pending");

    // Bar 34: New trigger (P_hi >= VAH again)
    OutcomeDetector::ProcessBar(obs, 34, 102.0f, 100.0f, 101.0f);
    TEST_ASSERT(obs.upBreakout.IsActive(), "O1 Failed: New trigger should start");
}

// ============================================================================
// TEST: O2 Continuation Down - Basic
// ============================================================================

void test_O2_basic_breakout() {
    std::cout << "\n=== Test: O2 Basic Breakout ===\n";

    FrozenReferences refs = createFrozenRefs(95.0f, 100.0f, 90.0f, 30);

    BehaviorObservation obs;
    obs.Initialize(refs, 3, 0.25f);

    // Bar 31: Trigger - P_lo <= VAL (90)
    bool completed = OutcomeDetector::ProcessBar(obs, 31, 91.0f, 89.0f, 89.5f);
    TEST_ASSERT(!completed, "O2: Bar 31 should not complete (trigger only)");
    TEST_ASSERT(obs.dnBreakout.IsActive(), "O2: Dn breakout should be active");

    // Bar 32-34: Hold bars - P_hi <= VAL
    OutcomeDetector::ProcessBar(obs, 32, 89.0f, 88.0f, 88.5f);
    OutcomeDetector::ProcessBar(obs, 33, 88.5f, 87.0f, 87.5f);
    completed = OutcomeDetector::ProcessBar(obs, 34, 88.0f, 86.0f, 86.5f);

    TEST_ASSERT(completed, "O2: Bar 34 should complete O2");
    TEST_ASSERT(obs.outcome == BehaviorOutcome::O2_CONTINUATION_DN, "O2: Outcome should be O2");
}

// ============================================================================
// TEST: O3 Mean-Revert from High
// ============================================================================

void test_O3_mean_revert_high() {
    std::cout << "\n=== Test: O3 Mean-Revert from High ===\n";

    // VAH=100, VAL=90, VA_mid=95, tolerance=2.5 (0.25 * 10)
    // Return condition: |P_t - 95| <= 2.5, so range is [92.5, 97.5]
    FrozenReferences refs = createFrozenRefs(95.0f, 100.0f, 90.0f, 30);

    BehaviorObservation obs;
    obs.Initialize(refs, 3, 0.25f);

    // Bar 31: Touch VAH (P_hi >= 100), close stays high (outside tolerance)
    OutcomeDetector::ProcessBar(obs, 31, 101.0f, 99.0f, 99.5f);
    TEST_ASSERT(obs.touchedVAH, "O3: Should have touched VAH");
    TEST_ASSERT(!obs.touchedVAL, "O3: Should NOT have touched VAL");
    TEST_ASSERT(obs.outcome == BehaviorOutcome::PENDING, "O3: Should be pending (close at 99.5 > 97.5)");

    // Bar 32: Still high, close outside tolerance
    OutcomeDetector::ProcessBar(obs, 32, 100.0f, 98.0f, 98.5f);
    TEST_ASSERT(obs.outcome == BehaviorOutcome::PENDING, "O3: Still pending (close at 98.5 > 97.5)");

    // Bar 33: Return to VA_mid (close at 95 is within tolerance [92.5, 97.5])
    bool completed = OutcomeDetector::ProcessBar(obs, 33, 96.0f, 94.0f, 95.0f);
    TEST_ASSERT(completed, "O3: Should complete on return to VA_mid");
    TEST_ASSERT(obs.outcome == BehaviorOutcome::O3_MEAN_REVERT_HIGH, "O3: Outcome should be O3");
}

// ============================================================================
// TEST: O4 Mean-Revert from Low
// ============================================================================

void test_O4_mean_revert_low() {
    std::cout << "\n=== Test: O4 Mean-Revert from Low ===\n";

    FrozenReferences refs = createFrozenRefs(95.0f, 100.0f, 90.0f, 30);

    BehaviorObservation obs;
    obs.Initialize(refs, 3, 0.25f);

    // Bar 31: Touch VAL (P_lo <= 90)
    OutcomeDetector::ProcessBar(obs, 31, 91.0f, 89.0f, 90.0f);
    TEST_ASSERT(obs.touchedVAL, "O4: Should have touched VAL");

    // Bar 32: Return to VA_mid
    bool completed = OutcomeDetector::ProcessBar(obs, 32, 96.0f, 94.0f, 95.0f);
    TEST_ASSERT(completed, "O4: Should complete on return");
    TEST_ASSERT(obs.outcome == BehaviorOutcome::O4_MEAN_REVERT_LOW, "O4: Outcome should be O4");
}

// ============================================================================
// TEST: O5 Range-Bound (no events)
// ============================================================================

void test_O5_no_events() {
    std::cout << "\n=== Test: O5 Range-Bound (No Events) ===\n";

    FrozenReferences refs = createFrozenRefs(95.0f, 100.0f, 90.0f, 30);

    BehaviorObservation obs;
    obs.Initialize(refs, 3, 0.25f);

    // Bars 31-40: Stay within VA, never touch boundaries
    for (int i = 31; i <= 40; i++) {
        OutcomeDetector::ProcessBar(obs, i, 98.0f, 92.0f, 95.0f);
    }

    TEST_ASSERT(!obs.touchedVAH, "O5: Should not have touched VAH");
    TEST_ASSERT(!obs.touchedVAL, "O5: Should not have touched VAL");
    TEST_ASSERT(obs.outcome == BehaviorOutcome::PENDING, "O5: Still pending before session end");

    // Finalize session
    OutcomeDetector::FinalizeSession(obs, 40);
    TEST_ASSERT(obs.outcome == BehaviorOutcome::O5_RANGE_BOUND, "O5: Outcome should be O5");
}

// ============================================================================
// TEST: Same-Bar Collision → O5
// ============================================================================

void test_same_bar_collision() {
    std::cout << "\n=== Test: Same-Bar Collision ===\n";

    FrozenReferences refs = createFrozenRefs(95.0f, 100.0f, 90.0f, 30);

    BehaviorObservation obs;
    obs.Initialize(refs, 3, 0.25f);

    // Bar 31: Same-bar collision - P_hi >= VAH AND P_lo <= VAL
    // Close at 99.0 is OUTSIDE tolerance [92.5, 97.5]
    OutcomeDetector::ProcessBar(obs, 31, 101.0f, 89.0f, 99.0f);

    // Both should be touched
    TEST_ASSERT(obs.touchedVAH, "Collision: Should have touched VAH");
    TEST_ASSERT(obs.touchedVAL, "Collision: Should have touched VAL");

    // But neither breakout should be active (collision resets both)
    TEST_ASSERT(!obs.upBreakout.IsActive(), "Collision: Up breakout should NOT be active");
    TEST_ASSERT(!obs.dnBreakout.IsActive(), "Collision: Dn breakout should NOT be active");

    // No outcome yet (close not at VA_mid)
    TEST_ASSERT(obs.outcome == BehaviorOutcome::PENDING, "Collision: Should still be pending");

    // Finalize - should be UNRESOLVED (touched but never returned to VA_mid)
    OutcomeDetector::FinalizeSession(obs, 31);
    TEST_ASSERT(obs.outcome == BehaviorOutcome::UNRESOLVED, "Collision: Should be UNRESOLVED");
}

// ============================================================================
// TEST: Session Ends Before Hold Completes → UNRESOLVED
// ============================================================================

void test_session_ends_before_hold() {
    std::cout << "\n=== Test: Session Ends Before Hold ===\n";

    FrozenReferences refs = createFrozenRefs(95.0f, 100.0f, 90.0f, 30);

    BehaviorObservation obs;
    obs.Initialize(refs, 3, 0.25f);

    // Bar 31: Trigger
    OutcomeDetector::ProcessBar(obs, 31, 101.0f, 100.0f, 100.5f);

    // Bar 32: Hold 1
    OutcomeDetector::ProcessBar(obs, 32, 102.0f, 100.5f, 101.0f);

    // Session ends at bar 32 - hold incomplete
    OutcomeDetector::FinalizeSession(obs, 32);
    TEST_ASSERT(obs.outcome == BehaviorOutcome::UNRESOLVED, "Hold incomplete: Should be UNRESOLVED");
}

// ============================================================================
// TEST: Session Ends After Touch Before Return → UNRESOLVED
// ============================================================================

void test_session_ends_touch_no_return() {
    std::cout << "\n=== Test: Touch Without Return ===\n";

    FrozenReferences refs = createFrozenRefs(95.0f, 100.0f, 90.0f, 30);

    BehaviorObservation obs;
    obs.Initialize(refs, 3, 0.25f);

    // Bar 31: Touch VAH
    OutcomeDetector::ProcessBar(obs, 31, 101.0f, 98.0f, 99.0f);
    TEST_ASSERT(obs.touchedVAH, "Touch no return: Should have touched VAH");

    // Bar 32: Stay high, never return to VA_mid
    OutcomeDetector::ProcessBar(obs, 32, 102.0f, 99.0f, 100.0f);

    // Session ends
    OutcomeDetector::FinalizeSession(obs, 32);
    TEST_ASSERT(obs.outcome == BehaviorOutcome::UNRESOLVED, "Touch no return: Should be UNRESOLVED");
}

// ============================================================================
// TEST: Hypothesis Mapping - All Shapes
// ============================================================================

void test_hypothesis_mapping() {
    std::cout << "\n=== Test: Hypothesis Mapping ===\n";

    FrozenReferences refs = createFrozenRefs(95.0f, 100.0f, 90.0f, 30);

    // NORMAL_DISTRIBUTION → MEAN_REVERSION
    {
        auto m = HypothesisMapper::MapShapeToHypothesis(ProfileShape::NORMAL_DISTRIBUTION, 0.0f, refs);
        TEST_ASSERT(m.hypothesis == HypothesisType::MEAN_REVERSION, "NORMAL: Should map to MEAN_REVERSION");
    }

    // D_SHAPED (a > 0) → MEAN_REVERSION_HIGH
    {
        auto m = HypothesisMapper::MapShapeToHypothesis(ProfileShape::D_SHAPED, 0.2f, refs);
        TEST_ASSERT(m.hypothesis == HypothesisType::MEAN_REVERSION_HIGH, "D_SHAPED(+): Should map to MR_HIGH");
    }

    // D_SHAPED (a < 0) → MEAN_REVERSION_LOW
    {
        auto m = HypothesisMapper::MapShapeToHypothesis(ProfileShape::D_SHAPED, -0.2f, refs);
        TEST_ASSERT(m.hypothesis == HypothesisType::MEAN_REVERSION_LOW, "D_SHAPED(-): Should map to MR_LOW");
    }

    // BALANCED → RANGE_BOUND
    {
        auto m = HypothesisMapper::MapShapeToHypothesis(ProfileShape::BALANCED, 0.0f, refs);
        TEST_ASSERT(m.hypothesis == HypothesisType::RANGE_BOUND, "BALANCED: Should map to RANGE_BOUND");
    }

    // P_SHAPED → CONTINUATION_UP
    {
        auto m = HypothesisMapper::MapShapeToHypothesis(ProfileShape::P_SHAPED, 0.0f, refs);
        TEST_ASSERT(m.hypothesis == HypothesisType::CONTINUATION_UP, "P_SHAPED: Should map to CONT_UP");
    }

    // B_SHAPED → CONTINUATION_DN
    {
        auto m = HypothesisMapper::MapShapeToHypothesis(ProfileShape::B_SHAPED, 0.0f, refs);
        TEST_ASSERT(m.hypothesis == HypothesisType::CONTINUATION_DN, "B_SHAPED: Should map to CONT_DN");
    }

    // THIN_VERTICAL without trend → NONE
    {
        auto m = HypothesisMapper::MapShapeToHypothesis(ProfileShape::THIN_VERTICAL, 0.0f, refs, 0);
        TEST_ASSERT(m.hypothesis == HypothesisType::NONE, "THIN_VERTICAL(no trend): Should be NONE");
        TEST_ASSERT(m.requiresTrendDirection, "THIN_VERTICAL: Should require trend direction");
    }

    // THIN_VERTICAL with UP trend → CONTINUATION_UP
    {
        auto m = HypothesisMapper::MapShapeToHypothesis(ProfileShape::THIN_VERTICAL, 0.0f, refs, 1);
        TEST_ASSERT(m.hypothesis == HypothesisType::CONTINUATION_UP, "THIN_VERTICAL(UP): Should map to CONT_UP");
    }

    // UNDEFINED → NONE
    {
        auto m = HypothesisMapper::MapShapeToHypothesis(ProfileShape::UNDEFINED, 0.0f, refs);
        TEST_ASSERT(m.hypothesis == HypothesisType::NONE, "UNDEFINED: Should map to NONE");
    }
}

// ============================================================================
// TEST: Outcome Matches Hypothesis
// ============================================================================

void test_outcome_hypothesis_match() {
    std::cout << "\n=== Test: Outcome-Hypothesis Matching ===\n";

    TEST_ASSERT(
        HypothesisMapper::OutcomeMatchesHypothesis(BehaviorOutcome::O1_CONTINUATION_UP, HypothesisType::CONTINUATION_UP),
        "O1 should match CONTINUATION_UP");

    TEST_ASSERT(
        HypothesisMapper::OutcomeMatchesHypothesis(BehaviorOutcome::O2_CONTINUATION_DN, HypothesisType::CONTINUATION_DN),
        "O2 should match CONTINUATION_DN");

    TEST_ASSERT(
        HypothesisMapper::OutcomeMatchesHypothesis(BehaviorOutcome::O3_MEAN_REVERT_HIGH, HypothesisType::MEAN_REVERSION),
        "O3 should match MEAN_REVERSION");

    TEST_ASSERT(
        HypothesisMapper::OutcomeMatchesHypothesis(BehaviorOutcome::O4_MEAN_REVERT_LOW, HypothesisType::MEAN_REVERSION),
        "O4 should match MEAN_REVERSION");

    TEST_ASSERT(
        HypothesisMapper::OutcomeMatchesHypothesis(BehaviorOutcome::O5_RANGE_BOUND, HypothesisType::RANGE_BOUND),
        "O5 should match RANGE_BOUND");

    TEST_ASSERT(
        !HypothesisMapper::OutcomeMatchesHypothesis(BehaviorOutcome::O1_CONTINUATION_UP, HypothesisType::CONTINUATION_DN),
        "O1 should NOT match CONTINUATION_DN");
}

// ============================================================================
// TEST: BehaviorSessionManager Integration
// ============================================================================

void test_session_manager_integration() {
    std::cout << "\n=== Test: Session Manager Integration ===\n";

    BehaviorSessionManager mgr;

    // Freeze at bar 30 with P_SHAPED
    mgr.Freeze(30, 95.0f, 100.0f, 90.0f, 110.0f, 85.0f,
               ProfileShape::P_SHAPED, 0.0f, 3, 0.25f);

    TEST_ASSERT(mgr.frozen, "Manager: Should be frozen");
    TEST_ASSERT(mgr.hypothesis.hypothesis == HypothesisType::CONTINUATION_UP,
                "Manager: P_SHAPED should predict CONT_UP");

    // Process bars that complete O1
    mgr.ProcessBar(31, 101.0f, 100.0f, 100.5f);  // Trigger
    mgr.ProcessBar(32, 102.0f, 100.5f, 101.0f);  // Hold 1
    mgr.ProcessBar(33, 103.0f, 101.0f, 102.0f);  // Hold 2
    mgr.ProcessBar(34, 104.0f, 101.5f, 103.0f);  // Hold 3 - complete

    TEST_ASSERT(mgr.observation.outcome == BehaviorOutcome::O1_CONTINUATION_UP,
                "Manager: Should have O1 outcome");
    TEST_ASSERT(mgr.WasHypothesisCorrect(), "Manager: Hypothesis should be correct");

    // Reset for new session
    mgr.Reset();
    TEST_ASSERT(!mgr.frozen, "Manager: Should not be frozen after reset");
}

// ============================================================================
// ADVERSARIAL TESTS (spec-conformance edge cases)
// ============================================================================

void test_hold_violation_immediate_reset() {
    std::cout << "\n=== Test: Hold Violation Immediate Reset ===\n";

    FrozenReferences refs = createFrozenRefs(95.0f, 100.0f, 90.0f, 30);
    BehaviorObservation obs;
    obs.Initialize(refs, 3, 0.25f);

    // Bar 31: Trigger
    OutcomeDetector::ProcessBar(obs, 31, 101.0f, 100.0f, 100.5f);
    TEST_ASSERT(obs.upBreakout.IsActive(), "Immediate reset: Trigger active");
    TEST_ASSERT(obs.upBreakout.holdBarsRemaining == 3, "Immediate reset: 3 hold bars");

    // Bar 32: IMMEDIATE violation (very next bar P_lo < VAH)
    // P_hi must also be < VAH to avoid immediate re-trigger
    OutcomeDetector::ProcessBar(obs, 32, 99.5f, 99.0f, 99.0f);
    TEST_ASSERT(!obs.upBreakout.IsActive(), "Immediate reset: Should reset on immediate violation");
}

void test_repeated_triggers_no_overlap() {
    std::cout << "\n=== Test: Repeated Triggers No Overlap ===\n";

    FrozenReferences refs = createFrozenRefs(95.0f, 100.0f, 90.0f, 30);
    BehaviorObservation obs;
    obs.Initialize(refs, 3, 0.25f);

    // Bar 31: Trigger
    OutcomeDetector::ProcessBar(obs, 31, 101.0f, 100.0f, 100.5f);
    int originalT_brk = obs.upBreakout.t_brk;
    TEST_ASSERT(originalT_brk == 31, "No overlap: First trigger at bar 31");

    // Bar 32: Another P_hi >= VAH while hold active - should NOT create new attempt
    OutcomeDetector::ProcessBar(obs, 32, 105.0f, 100.5f, 104.0f);
    TEST_ASSERT(obs.upBreakout.t_brk == 31, "No overlap: t_brk should still be 31 (no overlap)");
    TEST_ASSERT(obs.upBreakout.holdBarsRemaining == 2, "No overlap: Hold continues, now 2 bars");
}

void test_hold_completes_on_final_bar() {
    std::cout << "\n=== Test: Hold Completes On Final Bar ===\n";

    FrozenReferences refs = createFrozenRefs(95.0f, 100.0f, 90.0f, 30);
    BehaviorObservation obs;
    obs.Initialize(refs, 3, 0.25f);

    // Bar 31: Trigger
    OutcomeDetector::ProcessBar(obs, 31, 101.0f, 100.0f, 100.5f);
    // Bar 32: Hold 1
    OutcomeDetector::ProcessBar(obs, 32, 102.0f, 100.5f, 101.0f);
    // Bar 33: Hold 2
    OutcomeDetector::ProcessBar(obs, 33, 103.0f, 101.0f, 102.0f);
    // Bar 34: Hold 3 - session also ends here
    bool completed = OutcomeDetector::ProcessBar(obs, 34, 104.0f, 101.5f, 103.0f);

    TEST_ASSERT(completed, "Final bar: Should complete O1 on final bar");
    TEST_ASSERT(obs.outcome == BehaviorOutcome::O1_CONTINUATION_UP, "Final bar: Outcome is O1 (not UNRESOLVED)");

    // Calling finalize should NOT change outcome
    OutcomeDetector::FinalizeSession(obs, 34);
    TEST_ASSERT(obs.outcome == BehaviorOutcome::O1_CONTINUATION_UP, "Final bar: Still O1 after finalize");
}

void test_tolerance_edge_inclusive() {
    std::cout << "\n=== Test: Tolerance Edge Inclusive ===\n";

    // VAH=100, VAL=90, VA_mid=95, tolerance=2.5 (0.25 * 10)
    // Return condition: |P_t - 95| <= 2.5 → [92.5, 97.5] INCLUSIVE
    FrozenReferences refs = createFrozenRefs(95.0f, 100.0f, 90.0f, 30);
    BehaviorObservation obs;
    obs.Initialize(refs, 3, 0.25f);

    // Touch VAH
    OutcomeDetector::ProcessBar(obs, 31, 101.0f, 99.0f, 99.5f);
    TEST_ASSERT(obs.touchedVAH, "Tolerance edge: Touched VAH");

    // Return EXACTLY at tolerance boundary: 95 + 2.5 = 97.5
    bool completed = OutcomeDetector::ProcessBar(obs, 32, 98.0f, 97.0f, 97.5f);
    TEST_ASSERT(completed, "Tolerance edge: 97.5 should complete (boundary is inclusive)");
    TEST_ASSERT(obs.outcome == BehaviorOutcome::O3_MEAN_REVERT_HIGH, "Tolerance edge: Outcome is O3");
}

void test_tolerance_edge_exclusive_fail() {
    std::cout << "\n=== Test: Tolerance Edge Just Outside ===\n";

    FrozenReferences refs = createFrozenRefs(95.0f, 100.0f, 90.0f, 30);
    BehaviorObservation obs;
    obs.Initialize(refs, 3, 0.25f);

    // Touch VAH
    OutcomeDetector::ProcessBar(obs, 31, 101.0f, 99.0f, 99.5f);

    // Return JUST outside tolerance: 97.51 > 97.5
    bool completed = OutcomeDetector::ProcessBar(obs, 32, 98.0f, 97.0f, 97.51f);
    TEST_ASSERT(!completed, "Tolerance outside: 97.51 should NOT complete");
    TEST_ASSERT(obs.outcome == BehaviorOutcome::PENDING, "Tolerance outside: Still pending");
}

void test_history_tracker() {
    std::cout << "\n=== Test: History Tracker ===\n";

    BehaviorHistoryTracker tracker;

    // Initially no data - should return base multiplier
    float mult = tracker.GetConfidenceMultiplier(ProfileShape::P_SHAPED);
    TEST_ASSERT(std::abs(mult - 1.0f) < 0.001f, "History: Base multiplier when no data");

    // Add some sessions (need MIN_SAMPLES=10 before multiplier applies)
    for (int i = 0; i < 10; i++) {
        tracker.RecordSession(ProfileShape::P_SHAPED, i < 7);  // 7/10 = 70% hit rate
    }

    int attempts, matches;
    float hitRate;
    tracker.GetStats(ProfileShape::P_SHAPED, attempts, matches, hitRate);
    TEST_ASSERT(attempts == 10, "History: 10 attempts recorded");
    TEST_ASSERT(matches == 7, "History: 7 matches recorded");
    TEST_ASSERT(std::abs(hitRate - 0.7f) < 0.001f, "History: 70% hit rate");

    // Multiplier should now be: 0.8 + 0.7 * 0.4 = 1.08
    mult = tracker.GetConfidenceMultiplier(ProfileShape::P_SHAPED);
    TEST_ASSERT(std::abs(mult - 1.08f) < 0.001f, "History: Multiplier 1.08 for 70% hit rate");

    // Different shape should still have base multiplier
    mult = tracker.GetConfidenceMultiplier(ProfileShape::B_SHAPED);
    TEST_ASSERT(std::abs(mult - 1.0f) < 0.001f, "History: Base multiplier for untested shape");

    // Test reset
    tracker.Reset();
    tracker.GetStats(ProfileShape::P_SHAPED, attempts, matches, hitRate);
    TEST_ASSERT(attempts == 0, "History: Reset clears attempts");
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "========================================\n";
    std::cout << "AMT Behavior Mapping Tests (v1.2 Spec)\n";
    std::cout << "========================================\n";

    test_O1_basic_breakout();
    test_O1_failed_hold();
    test_O2_basic_breakout();
    test_O3_mean_revert_high();
    test_O4_mean_revert_low();
    test_O5_no_events();
    test_same_bar_collision();
    test_session_ends_before_hold();
    test_session_ends_touch_no_return();
    test_hypothesis_mapping();
    test_outcome_hypothesis_match();
    test_session_manager_integration();

    // Adversarial tests
    test_hold_violation_immediate_reset();
    test_repeated_triggers_no_overlap();
    test_hold_completes_on_final_bar();
    test_tolerance_edge_inclusive();
    test_tolerance_edge_exclusive_fail();

    // History tracker tests
    test_history_tracker();

    std::cout << "\n========================================\n";
    std::cout << "Results: " << g_testsPassed << "/" << g_testsRun << " tests passed\n";
    std::cout << "========================================\n";

    return (g_testsPassed == g_testsRun) ? 0 : 1;
}
