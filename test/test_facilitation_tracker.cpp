// test_facilitation_tracker.cpp - Unit tests for FacilitationTracker temporal persistence
// Tests asymmetric hysteresis, persistence counting, and transition detection

#include <iostream>
#include <cassert>

// Define standalone test mode to exclude SC-dependent functions
#define AMT_STANDALONE_TEST

// Mock SC types for standalone testing
#ifndef SIERRACHART_H
#define SIERRACHART_H
struct SCDateTime {
    double value = 0.0;
    double GetAsDouble() const { return value; }
    int GetHour() const { return 0; }
    int GetMinute() const { return 0; }
    int GetSecond() const { return 0; }
    SCDateTime operator+(double d) const { SCDateTime r; r.value = value + d; return r; }
};
struct SCFloatArray { int GetArraySize() const { return 0; } float operator[](int) const { return 0.0f; } };
#endif

#include "../AMT_Helpers.h"

using namespace AMT;

static int passed = 0;
static int failed = 0;

#define TEST(name) std::cout << "  " << name << "... " << std::flush
#define PASS() do { std::cout << "PASSED" << std::endl; passed++; } while(0)
#define FAIL(msg) do { std::cout << "FAILED: " << msg << std::endl; failed++; } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

// Test 1: Basic state persistence
void test_basic_persistence() {
    TEST("Basic state persistence");

    FacilitationTracker tracker;
    tracker.Reset();

    // Initially UNKNOWN
    ASSERT(tracker.confirmedState == AuctionFacilitation::UNKNOWN, "Should start UNKNOWN");
    ASSERT(!tracker.IsReady(), "Should not be ready initially");

    // First EFFICIENT bar - danger states enter fast (1 bar), but EFFICIENT also enters on first valid
    tracker.Update(AuctionFacilitation::EFFICIENT, 0);
    ASSERT(tracker.confirmedState == AuctionFacilitation::EFFICIENT, "EFFICIENT should confirm immediately from UNKNOWN");
    ASSERT(tracker.barsInConfirmed == 1, "Should have 1 bar in confirmed");
    ASSERT(tracker.IsReady(), "Should be ready after first valid state");

    // Second EFFICIENT bar
    tracker.Update(AuctionFacilitation::EFFICIENT, 1);
    ASSERT(tracker.barsInConfirmed == 2, "Should have 2 bars in confirmed");

    PASS();
}

// Test 2: Asymmetric hysteresis - danger enters fast
void test_asymmetric_danger_fast() {
    TEST("Asymmetric hysteresis - danger enters fast");

    FacilitationTracker tracker;
    tracker.Reset();

    // Start with EFFICIENT
    tracker.Update(AuctionFacilitation::EFFICIENT, 0);
    tracker.Update(AuctionFacilitation::EFFICIENT, 1);
    ASSERT(tracker.confirmedState == AuctionFacilitation::EFFICIENT, "Should be EFFICIENT");

    // LABORED should enter on first bar (danger signal)
    tracker.Update(AuctionFacilitation::LABORED, 2);
    ASSERT(tracker.confirmedState == AuctionFacilitation::LABORED, "LABORED should confirm immediately (danger)");
    ASSERT(tracker.stateJustChanged, "State should have just changed");
    ASSERT(tracker.priorConfirmedState == AuctionFacilitation::EFFICIENT, "Prior should be EFFICIENT");

    PASS();
}

// Test 3: Asymmetric hysteresis - calm exits slow
void test_asymmetric_calm_slow() {
    TEST("Asymmetric hysteresis - calm exits slow");

    FacilitationTracker tracker;
    tracker.Reset();

    // Start with LABORED
    tracker.Update(AuctionFacilitation::LABORED, 0);
    ASSERT(tracker.confirmedState == AuctionFacilitation::LABORED, "Should be LABORED");

    // First EFFICIENT bar - should NOT confirm yet (need 2 bars)
    tracker.Update(AuctionFacilitation::EFFICIENT, 1);
    ASSERT(tracker.confirmedState == AuctionFacilitation::LABORED, "Should still be LABORED (need 2 bars for EFFICIENT)");
    ASSERT(tracker.candidateState == AuctionFacilitation::EFFICIENT, "Candidate should be EFFICIENT");
    ASSERT(tracker.barsInCandidate == 1, "Should have 1 bar in candidate");

    // Second EFFICIENT bar - NOW should confirm
    tracker.Update(AuctionFacilitation::EFFICIENT, 2);
    ASSERT(tracker.confirmedState == AuctionFacilitation::EFFICIENT, "Should now be EFFICIENT");
    ASSERT(tracker.stateJustChanged, "State should have just changed");

    PASS();
}

// Test 4: FAILED enters immediately
void test_failed_enters_fast() {
    TEST("FAILED enters fast");

    FacilitationTracker tracker;
    tracker.Reset();

    tracker.Update(AuctionFacilitation::EFFICIENT, 0);
    tracker.Update(AuctionFacilitation::EFFICIENT, 1);

    // FAILED should enter on first bar
    tracker.Update(AuctionFacilitation::FAILED, 2);
    ASSERT(tracker.confirmedState == AuctionFacilitation::FAILED, "FAILED should confirm immediately");

    PASS();
}

// Test 5: INEFFICIENT enters immediately
void test_inefficient_enters_fast() {
    TEST("INEFFICIENT enters fast");

    FacilitationTracker tracker;
    tracker.Reset();

    tracker.Update(AuctionFacilitation::EFFICIENT, 0);

    // INEFFICIENT should enter on first bar
    tracker.Update(AuctionFacilitation::INEFFICIENT, 1);
    ASSERT(tracker.confirmedState == AuctionFacilitation::INEFFICIENT, "INEFFICIENT should confirm immediately");

    PASS();
}

// Test 6: Persistence counting
void test_persistence_counting() {
    TEST("Persistence counting");

    FacilitationTracker tracker;
    tracker.Reset();

    // Build up persistence
    for (int i = 0; i < 10; i++) {
        tracker.Update(AuctionFacilitation::LABORED, i);
    }

    ASSERT(tracker.barsInConfirmed == 10, "Should have 10 bars in confirmed");
    ASSERT(tracker.IsLaboredPersistent(), "Should be labored persistent (>= 5 bars)");
    ASSERT(tracker.IsPersistent(5), "Should pass IsPersistent(5)");
    ASSERT(tracker.IsPersistent(10), "Should pass IsPersistent(10)");
    ASSERT(!tracker.IsPersistent(11), "Should not pass IsPersistent(11)");

    PASS();
}

// Test 7: JustEntered / JustExited helpers
void test_transition_helpers() {
    TEST("JustEntered / JustExited helpers");

    FacilitationTracker tracker;
    tracker.Reset();

    tracker.Update(AuctionFacilitation::EFFICIENT, 0);
    tracker.Update(AuctionFacilitation::EFFICIENT, 1);

    // Transition to LABORED
    tracker.Update(AuctionFacilitation::LABORED, 2);
    ASSERT(tracker.JustEntered(AuctionFacilitation::LABORED), "Should have just entered LABORED");
    ASSERT(tracker.JustExited(AuctionFacilitation::EFFICIENT), "Should have just exited EFFICIENT");
    ASSERT(!tracker.JustEntered(AuctionFacilitation::EFFICIENT), "Should NOT have just entered EFFICIENT");

    // Next bar - no longer "just" changed
    tracker.Update(AuctionFacilitation::LABORED, 3);
    ASSERT(!tracker.JustChanged(), "Should not have just changed");
    ASSERT(!tracker.JustEntered(AuctionFacilitation::LABORED), "Should not have just entered anymore");

    PASS();
}

// Test 8: IsDangerState helper
void test_danger_state_helper() {
    TEST("IsDangerState helper");

    FacilitationTracker tracker;
    tracker.Reset();

    tracker.Update(AuctionFacilitation::EFFICIENT, 0);
    ASSERT(!tracker.IsDangerState(), "EFFICIENT is not danger");

    tracker.Update(AuctionFacilitation::LABORED, 1);
    ASSERT(tracker.IsDangerState(), "LABORED is danger");

    tracker.Update(AuctionFacilitation::FAILED, 2);
    ASSERT(tracker.IsDangerState(), "FAILED is danger");

    tracker.Update(AuctionFacilitation::INEFFICIENT, 3);
    ASSERT(tracker.IsDangerState(), "INEFFICIENT is danger");

    PASS();
}

// Test 9: GetStateWithPersistence
void test_state_with_persistence() {
    TEST("GetStateWithPersistence output");

    FacilitationTracker tracker;
    tracker.Reset();

    for (int i = 0; i < 5; i++) {
        tracker.Update(AuctionFacilitation::LABORED, i);
    }

    char buf[32];
    tracker.GetStateWithPersistence(buf, sizeof(buf));

    std::string result(buf);
    ASSERT(result == "LABORED(5)", "Should be 'LABORED(5)' but got: " + result);

    PASS();
}

// Test 10: UNKNOWN propagates immediately and resets
void test_unknown_propagates() {
    TEST("UNKNOWN propagates immediately");

    FacilitationTracker tracker;
    tracker.Reset();

    tracker.Update(AuctionFacilitation::LABORED, 0);
    tracker.Update(AuctionFacilitation::LABORED, 1);
    tracker.Update(AuctionFacilitation::LABORED, 2);
    ASSERT(tracker.barsInConfirmed == 3, "Should have 3 bars");

    // UNKNOWN should propagate immediately and reset counts
    tracker.Update(AuctionFacilitation::UNKNOWN, 3);
    ASSERT(tracker.confirmedState == AuctionFacilitation::UNKNOWN, "Should be UNKNOWN");
    ASSERT(tracker.barsInConfirmed == 0, "Should reset bars count");
    ASSERT(!tracker.IsReady(), "Should not be ready");

    PASS();
}

// Test 11: Reset clears all state
void test_reset() {
    TEST("Reset clears all state");

    FacilitationTracker tracker;

    // Build up some state
    tracker.Update(AuctionFacilitation::LABORED, 0);
    tracker.Update(AuctionFacilitation::LABORED, 1);
    tracker.lastVolPctile = 80.0;
    tracker.lastRangePctile = 20.0;

    // Reset
    tracker.Reset();

    ASSERT(tracker.confirmedState == AuctionFacilitation::UNKNOWN, "Should be UNKNOWN");
    ASSERT(tracker.candidateState == AuctionFacilitation::UNKNOWN, "Candidate should be UNKNOWN");
    ASSERT(tracker.barsInConfirmed == 0, "barsInConfirmed should be 0");
    ASSERT(tracker.barsInCandidate == 0, "barsInCandidate should be 0");
    ASSERT(!tracker.stateJustChanged, "stateJustChanged should be false");
    ASSERT(tracker.lastTransitionBar == -1, "lastTransitionBar should be -1");
    ASSERT(tracker.lastVolPctile == 0.0, "lastVolPctile should be 0");
    ASSERT(tracker.lastRangePctile == 0.0, "lastRangePctile should be 0");

    PASS();
}

// Test 12: Candidate flip-flop doesn't confirm
void test_candidate_flipflop() {
    TEST("Candidate flip-flop doesn't confirm");

    FacilitationTracker tracker;
    tracker.Reset();

    tracker.Update(AuctionFacilitation::LABORED, 0);  // LABORED confirmed

    // Flip-flop between EFFICIENT candidates (need 2 to confirm)
    tracker.Update(AuctionFacilitation::EFFICIENT, 1);  // 1 bar EFFICIENT
    ASSERT(tracker.confirmedState == AuctionFacilitation::LABORED, "Still LABORED");

    tracker.Update(AuctionFacilitation::FAILED, 2);     // Switch to FAILED (danger, confirms immediately!)
    ASSERT(tracker.confirmedState == AuctionFacilitation::FAILED, "FAILED confirms immediately as danger");

    // Now try flip-flop with EFFICIENT (needs 2)
    tracker.Update(AuctionFacilitation::EFFICIENT, 3);  // 1 bar EFFICIENT
    ASSERT(tracker.confirmedState == AuctionFacilitation::FAILED, "Still FAILED");

    tracker.Update(AuctionFacilitation::LABORED, 4);    // Switch candidate (also danger, confirms)
    ASSERT(tracker.confirmedState == AuctionFacilitation::LABORED, "LABORED confirms immediately");

    PASS();
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "FacilitationTracker Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    test_basic_persistence();
    test_asymmetric_danger_fast();
    test_asymmetric_calm_slow();
    test_failed_enters_fast();
    test_inefficient_enters_fast();
    test_persistence_counting();
    test_transition_helpers();
    test_danger_state_helper();
    test_state_with_persistence();
    test_unknown_propagates();
    test_reset();
    test_candidate_flipflop();

    std::cout << "========================================" << std::endl;
    std::cout << "RESULTS: " << passed << " passed, " << failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    return failed > 0 ? 1 : 0;
}
