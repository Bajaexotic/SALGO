// ============================================================================
// test_session_phase.cpp
// Boundary tests for SessionPhase enum and helper functions
//
// Tests verify:
// - SessionPhase enum values are correct
// - IsRTHSession helper correctly identifies RTH phases
// - IsEveningSession helper correctly identifies EVENING phases
// - SessionPhaseToString returns expected values
// - Thresholds are set correctly (IB=60min, CLOSING=45min)
// ============================================================================

#include <iostream>
#include <string>
#include "test_sierrachart_mock.h"  // Mock SC types (must come before AMT headers)
#include "amt_core.h"
#include "AMT_Helpers.h"  // For DetermineExactPhase

using namespace AMT;

// ============================================================================
// TEST INFRASTRUCTURE
// ============================================================================

#define TEST_ASSERT(cond, msg) \
    if (!(cond)) { \
        std::cout << "  FAIL: " << msg << std::endl; \
        return false; \
    }

#define TEST_PASSED(name) \
    std::cout << "  PASS: " << name << std::endl; \
    return true;

// ============================================================================
// TEST: SessionPhase enum values
// ============================================================================

bool test_session_phase_enum_values() {
    // Verify enum integer values match specification
    TEST_ASSERT(static_cast<int>(SessionPhase::UNKNOWN) == -1, "UNKNOWN should be -1");
    TEST_ASSERT(static_cast<int>(SessionPhase::GLOBEX) == 0, "GLOBEX should be 0");
    TEST_ASSERT(static_cast<int>(SessionPhase::LONDON_OPEN) == 1, "LONDON_OPEN should be 1");
    TEST_ASSERT(static_cast<int>(SessionPhase::PRE_MARKET) == 2, "PRE_MARKET should be 2");
    TEST_ASSERT(static_cast<int>(SessionPhase::INITIAL_BALANCE) == 3, "INITIAL_BALANCE should be 3");
    TEST_ASSERT(static_cast<int>(SessionPhase::MID_SESSION) == 4, "MID_SESSION should be 4");
    TEST_ASSERT(static_cast<int>(SessionPhase::CLOSING_SESSION) == 5, "CLOSING_SESSION should be 5");
    TEST_ASSERT(static_cast<int>(SessionPhase::POST_CLOSE) == 6, "POST_CLOSE should be 6");
    TEST_ASSERT(static_cast<int>(SessionPhase::MAINTENANCE) == 7, "MAINTENANCE should be 7");

    TEST_PASSED("SessionPhase enum values");
}

// ============================================================================
// TEST: Legacy aliases
// ============================================================================

bool test_legacy_aliases() {
    // Verify legacy aliases point to correct phases
    TEST_ASSERT(OPENING_DRIVE == SessionPhase::INITIAL_BALANCE, "OPENING_DRIVE alias should be INITIAL_BALANCE");
    TEST_ASSERT(IB_CONFIRMATION == SessionPhase::INITIAL_BALANCE, "IB_CONFIRMATION alias should be INITIAL_BALANCE");

    TEST_PASSED("Legacy aliases");
}

// ============================================================================
// TEST: IsRTHSession helper
// ============================================================================

bool test_is_rth_session() {
    // RTH phases should return true
    TEST_ASSERT(IsRTHSession(SessionPhase::INITIAL_BALANCE) == true, "INITIAL_BALANCE is RTH");
    TEST_ASSERT(IsRTHSession(SessionPhase::MID_SESSION) == true, "MID_SESSION is RTH");
    TEST_ASSERT(IsRTHSession(SessionPhase::CLOSING_SESSION) == true, "CLOSING_SESSION is RTH");

    // EVENING phases should return false
    TEST_ASSERT(IsRTHSession(SessionPhase::GLOBEX) == false, "GLOBEX is not RTH");
    TEST_ASSERT(IsRTHSession(SessionPhase::LONDON_OPEN) == false, "LONDON_OPEN is not RTH");
    TEST_ASSERT(IsRTHSession(SessionPhase::PRE_MARKET) == false, "PRE_MARKET is not RTH");
    TEST_ASSERT(IsRTHSession(SessionPhase::POST_CLOSE) == false, "POST_CLOSE is not RTH");
    TEST_ASSERT(IsRTHSession(SessionPhase::MAINTENANCE) == false, "MAINTENANCE is not RTH");

    // UNKNOWN should return false
    TEST_ASSERT(IsRTHSession(SessionPhase::UNKNOWN) == false, "UNKNOWN is not RTH");

    TEST_PASSED("IsRTHSession helper");
}

// ============================================================================
// TEST: IsGlobexSession helper (covers all non-RTH phases)
// ============================================================================

bool test_is_globex_session() {
    // Non-RTH phases should return true (called "Globex" but includes all evening phases)
    TEST_ASSERT(IsGlobexSession(SessionPhase::GLOBEX) == true, "GLOBEX is Globex");
    TEST_ASSERT(IsGlobexSession(SessionPhase::LONDON_OPEN) == true, "LONDON_OPEN is Globex");
    TEST_ASSERT(IsGlobexSession(SessionPhase::PRE_MARKET) == true, "PRE_MARKET is Globex");
    TEST_ASSERT(IsGlobexSession(SessionPhase::POST_CLOSE) == true, "POST_CLOSE is Globex");
    TEST_ASSERT(IsGlobexSession(SessionPhase::MAINTENANCE) == true, "MAINTENANCE is Globex");

    // RTH phases should return false
    TEST_ASSERT(IsGlobexSession(SessionPhase::INITIAL_BALANCE) == false, "INITIAL_BALANCE is not Globex");
    TEST_ASSERT(IsGlobexSession(SessionPhase::MID_SESSION) == false, "MID_SESSION is not Globex");
    TEST_ASSERT(IsGlobexSession(SessionPhase::CLOSING_SESSION) == false, "CLOSING_SESSION is not Globex");

    // UNKNOWN returns false (explicitly checked in function)
    TEST_ASSERT(IsGlobexSession(SessionPhase::UNKNOWN) == false, "UNKNOWN is not Globex");

    TEST_PASSED("IsGlobexSession helper");
}

// ============================================================================
// TEST: SessionPhaseToString
// ============================================================================

bool test_session_phase_to_string() {
    // Note: SessionPhaseToString returns abbreviated display names
    TEST_ASSERT(std::string(SessionPhaseToString(SessionPhase::UNKNOWN)) == "UNKNOWN", "UNKNOWN string");
    TEST_ASSERT(std::string(SessionPhaseToString(SessionPhase::GLOBEX)) == "GLOBEX", "GLOBEX string");
    TEST_ASSERT(std::string(SessionPhaseToString(SessionPhase::LONDON_OPEN)) == "LONDON", "LONDON_OPEN string");
    TEST_ASSERT(std::string(SessionPhaseToString(SessionPhase::PRE_MARKET)) == "PRE_MKT", "PRE_MARKET string");
    TEST_ASSERT(std::string(SessionPhaseToString(SessionPhase::INITIAL_BALANCE)) == "IB", "INITIAL_BALANCE string");
    TEST_ASSERT(std::string(SessionPhaseToString(SessionPhase::MID_SESSION)) == "MID_SESS", "MID_SESSION string");
    TEST_ASSERT(std::string(SessionPhaseToString(SessionPhase::CLOSING_SESSION)) == "CLOSING", "CLOSING_SESSION string");
    TEST_ASSERT(std::string(SessionPhaseToString(SessionPhase::POST_CLOSE)) == "POST_CLOSE", "POST_CLOSE string");
    TEST_ASSERT(std::string(SessionPhaseToString(SessionPhase::MAINTENANCE)) == "MAINT", "MAINTENANCE string");

    TEST_PASSED("SessionPhaseToString");
}

// ============================================================================
// TEST: Thresholds
// ============================================================================

bool test_thresholds() {
    // Initial Balance = first 60 min (not 30)
    TEST_ASSERT(Thresholds::PHASE_IB_COMPLETE == 60, "IB should be 60 minutes");

    // Closing Window = 45 min
    TEST_ASSERT(Thresholds::PHASE_CLOSING_WINDOW == 45, "Closing window should be 45 minutes");

    // Evening phase boundaries (seconds from midnight, ET)
    TEST_ASSERT(Thresholds::POST_CLOSE_END_SEC == 61200, "POST_CLOSE ends at 17:00:00 (61200 sec)");
    TEST_ASSERT(Thresholds::MAINTENANCE_END_SEC == 64800, "MAINTENANCE ends at 18:00:00 (64800 sec)");
    TEST_ASSERT(Thresholds::LONDON_OPEN_SEC == 10800, "LONDON_OPEN starts at 03:00:00 (10800 sec)");
    TEST_ASSERT(Thresholds::PRE_MARKET_START_SEC == 30600, "PRE_MARKET starts at 08:30:00 (30600 sec)");

    TEST_PASSED("Thresholds");
}

// ============================================================================
// TEST: Phase ordering (RTH phases should be contiguous)
// ============================================================================

bool test_phase_ordering() {
    // RTH phases should be consecutive: INITIAL_BALANCE(3), MID_SESSION(4), CLOSING_SESSION(5)
    int ib = static_cast<int>(SessionPhase::INITIAL_BALANCE);
    int mid = static_cast<int>(SessionPhase::MID_SESSION);
    int closing = static_cast<int>(SessionPhase::CLOSING_SESSION);

    TEST_ASSERT(mid == ib + 1, "MID_SESSION should follow INITIAL_BALANCE");
    TEST_ASSERT(closing == mid + 1, "CLOSING_SESSION should follow MID_SESSION");

    TEST_PASSED("Phase ordering (RTH contiguous)");
}

// ============================================================================
// TEST: RTH/Globex symmetry
// ============================================================================

bool test_rth_globex_symmetry() {
    // Every valid phase (excluding UNKNOWN) should be either RTH or Globex, never both, never neither
    SessionPhase allPhases[] = {
        SessionPhase::GLOBEX,
        SessionPhase::LONDON_OPEN,
        SessionPhase::PRE_MARKET,
        SessionPhase::INITIAL_BALANCE,
        SessionPhase::MID_SESSION,
        SessionPhase::CLOSING_SESSION,
        SessionPhase::POST_CLOSE,
        SessionPhase::MAINTENANCE
    };

    for (SessionPhase p : allPhases) {
        bool isRth = IsRTHSession(p);
        bool isGlobex = IsGlobexSession(p);

        // XOR: exactly one must be true for valid phases
        TEST_ASSERT(isRth != isGlobex, "Phase must be RTH xor Globex");
    }

    // UNKNOWN is a special case: neither RTH nor Globex
    TEST_ASSERT(IsRTHSession(SessionPhase::UNKNOWN) == false, "UNKNOWN is not RTH");
    TEST_ASSERT(IsGlobexSession(SessionPhase::UNKNOWN) == false, "UNKNOWN is not Globex");

    TEST_PASSED("RTH/Globex symmetry");
}

// ============================================================================
// TEST: DetermineExactPhase RTH boundary behavior
// ============================================================================
// This test validates the P0 fix for phase boundary inconsistency.
// The bug: some call sites passed rthEndSec (58499) instead of rthEndSec+1 (58500),
// causing the last RTH second (16:14:59) to be misclassified as POST_CLOSE.
// ============================================================================

bool test_determine_exact_phase_rth_boundary() {
    // Standard ES RTH boundaries
    const int rthStartSec = 34200;  // 09:30:00
    const int rthEndIncl = 58499;   // 16:14:59 (Input[1] value - INCLUSIVE last RTH second)
    const int rthEndExcl = 58500;   // 16:15:00 (EXCLUSIVE boundary for DetermineExactPhase)
    const int gbxStartSec = 58500;  // = rthEndExcl

    // === TEST 1: Verify correct boundary behavior (EXCLUSIVE end) ===
    // At 16:14:59 (58499 sec) with CORRECT boundary (58500):
    SessionPhase at_16_14_59_correct = DetermineExactPhase(58499, rthStartSec, rthEndExcl, gbxStartSec);
    TEST_ASSERT(at_16_14_59_correct == SessionPhase::CLOSING_SESSION,
        "16:14:59 with EXCLUSIVE end (58500) should be CLOSING_SESSION, not POST_CLOSE");

    // At 16:15:00 (58500 sec) with CORRECT boundary (58500):
    SessionPhase at_16_15_00_correct = DetermineExactPhase(58500, rthStartSec, rthEndExcl, gbxStartSec);
    TEST_ASSERT(at_16_15_00_correct == SessionPhase::POST_CLOSE,
        "16:15:00 with EXCLUSIVE end (58500) should be POST_CLOSE");

    // At 16:15:01 (58501 sec):
    SessionPhase at_16_15_01 = DetermineExactPhase(58501, rthStartSec, rthEndExcl, gbxStartSec);
    TEST_ASSERT(at_16_15_01 == SessionPhase::POST_CLOSE,
        "16:15:01 should be POST_CLOSE");

    // === TEST 2: Demonstrate the BUG (WRONG boundary - what was happening before fix) ===
    // At 16:14:59 (58499 sec) with WRONG boundary (58499):
    // This was the bug: 58499 < 58499 = FALSE, so it returned POST_CLOSE incorrectly
    SessionPhase at_16_14_59_bug = DetermineExactPhase(58499, rthStartSec, rthEndIncl, gbxStartSec);
    TEST_ASSERT(at_16_14_59_bug == SessionPhase::POST_CLOSE,
        "BUG DEMO: 16:14:59 with INCLUSIVE end (58499) incorrectly returns POST_CLOSE");

    // === TEST 3: Verify other RTH boundaries ===
    // 09:29:59 should be PRE_MARKET
    SessionPhase at_09_29_59 = DetermineExactPhase(34199, rthStartSec, rthEndExcl, gbxStartSec);
    TEST_ASSERT(at_09_29_59 == SessionPhase::PRE_MARKET,
        "09:29:59 should be PRE_MARKET");

    // 09:30:00 should be INITIAL_BALANCE
    SessionPhase at_09_30_00 = DetermineExactPhase(34200, rthStartSec, rthEndExcl, gbxStartSec);
    TEST_ASSERT(at_09_30_00 == SessionPhase::INITIAL_BALANCE,
        "09:30:00 should be INITIAL_BALANCE");

    // 10:29:59 should be INITIAL_BALANCE (still within 60 min)
    SessionPhase at_10_29_59 = DetermineExactPhase(37799, rthStartSec, rthEndExcl, gbxStartSec);
    TEST_ASSERT(at_10_29_59 == SessionPhase::INITIAL_BALANCE,
        "10:29:59 should be INITIAL_BALANCE (elapsedMin=59)");

    // 10:30:00 should be MID_SESSION (elapsedMin=60)
    SessionPhase at_10_30_00 = DetermineExactPhase(37800, rthStartSec, rthEndExcl, gbxStartSec);
    TEST_ASSERT(at_10_30_00 == SessionPhase::MID_SESSION,
        "10:30:00 should be MID_SESSION (elapsedMin=60)");

    // 15:29:59 should be MID_SESSION (before closingStartSec)
    SessionPhase at_15_29_59 = DetermineExactPhase(55799, rthStartSec, rthEndExcl, gbxStartSec);
    TEST_ASSERT(at_15_29_59 == SessionPhase::MID_SESSION,
        "15:29:59 should be MID_SESSION");

    // 15:30:00 should be CLOSING_SESSION (closingStartSec = 58500 - 2700 = 55800, inclusive)
    // Direct second comparison: tSec >= closingStartSec
    SessionPhase at_15_30_00 = DetermineExactPhase(55800, rthStartSec, rthEndExcl, gbxStartSec);
    TEST_ASSERT(at_15_30_00 == SessionPhase::CLOSING_SESSION,
        "15:30:00 should be CLOSING_SESSION (inclusive boundary)");

    TEST_PASSED("DetermineExactPhase RTH boundary behavior");
}

// ============================================================================
// TEST: Complete boundary coverage for all phase transitions
// ============================================================================
// Tests all critical boundary timestamps per user requirements:
// - 09:29:59 / 09:30:00 (PRE_MARKET → INITIAL_BALANCE)
// - 10:29:59 / 10:30:00 (INITIAL_BALANCE → MID_SESSION)
// - 15:29:59 / 15:30:00 (MID_SESSION → CLOSING_SESSION) - direct second boundary
// - 16:14:59 / 16:15:00 (CLOSING_SESSION → POST_CLOSE)
// - 16:59:59 / 17:00:00 (POST_CLOSE → MAINTENANCE)
// - 17:59:59 / 18:00:00 (MAINTENANCE → GLOBEX)
// - 23:59:59 → 00:00:00 (midnight wrap within GLOBEX)
// - 02:59:59 / 03:00:00 (GLOBEX → LONDON_OPEN)
// ============================================================================

bool test_all_phase_boundaries() {
    const int rthStartSec = 34200;  // 09:30:00
    const int rthEndExcl = 58500;   // 16:15:00 (EXCLUSIVE)
    const int gbxStartSec = 58500;

    // === PRE_MARKET → INITIAL_BALANCE boundary ===
    TEST_ASSERT(DetermineExactPhase(34199, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::PRE_MARKET,
        "09:29:59 (34199) should be PRE_MARKET");
    TEST_ASSERT(DetermineExactPhase(34200, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::INITIAL_BALANCE,
        "09:30:00 (34200) should be INITIAL_BALANCE");

    // === INITIAL_BALANCE → MID_SESSION boundary (60 min elapsed) ===
    // At 10:29:59: elapsedMin = (37799 - 34200) / 60 = 59
    TEST_ASSERT(DetermineExactPhase(37799, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::INITIAL_BALANCE,
        "10:29:59 (37799) should be INITIAL_BALANCE (elapsedMin=59)");
    // At 10:30:00: elapsedMin = (37800 - 34200) / 60 = 60
    TEST_ASSERT(DetermineExactPhase(37800, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::MID_SESSION,
        "10:30:00 (37800) should be MID_SESSION (elapsedMin=60)");

    // === MID_SESSION → CLOSING_SESSION boundary (direct second comparison) ===
    // closingStartSec = 58500 - (45 * 60) = 55800
    // At 15:29:59 (55799): tSec < closingStartSec → MID_SESSION
    TEST_ASSERT(DetermineExactPhase(55799, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::MID_SESSION,
        "15:29:59 (55799) should be MID_SESSION");
    // At 15:30:00 (55800): tSec >= closingStartSec → CLOSING_SESSION (inclusive boundary)
    TEST_ASSERT(DetermineExactPhase(55800, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::CLOSING_SESSION,
        "15:30:00 (55800) should be CLOSING_SESSION (inclusive boundary)");

    // === CLOSING_SESSION → POST_CLOSE boundary ===
    TEST_ASSERT(DetermineExactPhase(58499, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::CLOSING_SESSION,
        "16:14:59 (58499) should be CLOSING_SESSION");
    TEST_ASSERT(DetermineExactPhase(58500, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::POST_CLOSE,
        "16:15:00 (58500) should be POST_CLOSE");

    // === POST_CLOSE → MAINTENANCE boundary ===
    // POST_CLOSE_END_SEC = 61200 (17:00:00)
    TEST_ASSERT(DetermineExactPhase(61199, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::POST_CLOSE,
        "16:59:59 (61199) should be POST_CLOSE");
    TEST_ASSERT(DetermineExactPhase(61200, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::MAINTENANCE,
        "17:00:00 (61200) should be MAINTENANCE");

    // === MAINTENANCE → GLOBEX boundary ===
    // MAINTENANCE_END_SEC = 64800 (18:00:00)
    TEST_ASSERT(DetermineExactPhase(64799, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::MAINTENANCE,
        "17:59:59 (64799) should be MAINTENANCE");
    TEST_ASSERT(DetermineExactPhase(64800, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::GLOBEX,
        "18:00:00 (64800) should be GLOBEX");

    // === Midnight wrap-around (within GLOBEX) ===
    TEST_ASSERT(DetermineExactPhase(86399, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::GLOBEX,
        "23:59:59 (86399) should be GLOBEX");
    TEST_ASSERT(DetermineExactPhase(0, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::GLOBEX,
        "00:00:00 (0) should be GLOBEX");
    TEST_ASSERT(DetermineExactPhase(1, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::GLOBEX,
        "00:00:01 (1) should be GLOBEX");

    // === GLOBEX → LONDON_OPEN boundary ===
    // LONDON_OPEN_SEC = 10800 (03:00:00)
    TEST_ASSERT(DetermineExactPhase(10799, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::GLOBEX,
        "02:59:59 (10799) should be GLOBEX");
    TEST_ASSERT(DetermineExactPhase(10800, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::LONDON_OPEN,
        "03:00:00 (10800) should be LONDON_OPEN");

    // === LONDON_OPEN → PRE_MARKET boundary ===
    // PRE_MARKET_START_SEC = 30600 (08:30:00)
    TEST_ASSERT(DetermineExactPhase(30599, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::LONDON_OPEN,
        "08:29:59 (30599) should be LONDON_OPEN");
    TEST_ASSERT(DetermineExactPhase(30600, rthStartSec, rthEndExcl, gbxStartSec) == SessionPhase::PRE_MARKET,
        "08:30:00 (30600) should be PRE_MARKET");

    TEST_PASSED("All phase boundaries");
}

// ============================================================================
// TEST: DetermineSessionPhase wrapper (drift-proof)
// ============================================================================
// Tests the new DetermineSessionPhase wrapper that accepts INCLUSIVE end time
// and internally converts to EXCLUSIVE, making drift structurally impossible.
// ============================================================================

bool test_determine_session_phase_wrapper() {
    const int rthStartSec = 34200;  // 09:30:00
    const int rthEndIncl = 58499;   // 16:14:59 (INCLUSIVE - as stored in sc.Input[1])

    // The wrapper should produce correct results with INCLUSIVE end
    TEST_ASSERT(DetermineSessionPhase(58499, rthStartSec, rthEndIncl) == SessionPhase::CLOSING_SESSION,
        "Wrapper: 16:14:59 with INCLUSIVE end (58499) should be CLOSING_SESSION");

    TEST_ASSERT(DetermineSessionPhase(58500, rthStartSec, rthEndIncl) == SessionPhase::POST_CLOSE,
        "Wrapper: 16:15:00 with INCLUSIVE end (58499) should be POST_CLOSE");

    // Test other boundaries using wrapper
    TEST_ASSERT(DetermineSessionPhase(34199, rthStartSec, rthEndIncl) == SessionPhase::PRE_MARKET,
        "Wrapper: 09:29:59 should be PRE_MARKET");
    TEST_ASSERT(DetermineSessionPhase(34200, rthStartSec, rthEndIncl) == SessionPhase::INITIAL_BALANCE,
        "Wrapper: 09:30:00 should be INITIAL_BALANCE");
    TEST_ASSERT(DetermineSessionPhase(37800, rthStartSec, rthEndIncl) == SessionPhase::MID_SESSION,
        "Wrapper: 10:30:00 should be MID_SESSION");
    TEST_ASSERT(DetermineSessionPhase(55800, rthStartSec, rthEndIncl) == SessionPhase::CLOSING_SESSION,
        "Wrapper: 15:30:00 should be CLOSING_SESSION (inclusive boundary)");

    TEST_PASSED("DetermineSessionPhase wrapper (drift-proof)");
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "=== Session/Phase Classification Tests ===" << std::endl;
    std::cout << "Testing SessionPhase enum and helper functions" << std::endl;
    std::cout << "IB = 60 min, CLOSING_WINDOW = 45 min" << std::endl;
    std::cout << std::endl;

    int passed = 0;
    int failed = 0;

    #define RUN_TEST(test) \
        if (test()) { passed++; } else { failed++; }

    RUN_TEST(test_session_phase_enum_values);
    RUN_TEST(test_legacy_aliases);
    RUN_TEST(test_is_rth_session);
    RUN_TEST(test_is_globex_session);
    RUN_TEST(test_session_phase_to_string);
    RUN_TEST(test_thresholds);
    RUN_TEST(test_phase_ordering);
    RUN_TEST(test_rth_globex_symmetry);
    RUN_TEST(test_determine_exact_phase_rth_boundary);  // P0 boundary fix verification
    RUN_TEST(test_all_phase_boundaries);                // Comprehensive boundary coverage
    RUN_TEST(test_determine_session_phase_wrapper);     // Drift-proof wrapper test

    std::cout << std::endl;
    std::cout << "=== Results ===" << std::endl;
    std::cout << "Passed: " << passed << std::endl;
    std::cout << "Failed: " << failed << std::endl;

    if (failed == 0) {
        std::cout << std::endl;
        std::cout << "All session/phase tests PASSED!" << std::endl;
        return 0;
    } else {
        std::cout << std::endl;
        std::cout << "Some tests FAILED. Review implementation." << std::endl;
        return 1;
    }
}
