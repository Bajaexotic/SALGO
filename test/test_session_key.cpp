// ============================================================================
// test_session_key.cpp
// Tests for SessionKey computation and session transition detection
// Validates SSOT: SessionManager owns session recognition
// ============================================================================

#include <iostream>
#include <string>
#include "test_sierrachart_mock.h"
#include "amt_core.h"
#include "AMT_Session.h"

using namespace AMT;

// ============================================================================
// TEST INFRASTRUCTURE
// ============================================================================

static int g_passed = 0;
static int g_failed = 0;

#define TEST_ASSERT(cond, msg) \
    if (!(cond)) { \
        std::cout << "  FAIL: " << msg << std::endl; \
        g_failed++; \
        return false; \
    }

#define TEST_PASSED(name) \
    std::cout << "  PASS: " << name << std::endl; \
    g_passed++; \
    return true;

// ============================================================================
// Constants: ES futures RTH hours (09:30-16:15 ET)
// ============================================================================
const int RTH_START_SEC = 9 * 3600 + 30 * 60;   // 09:30 = 34200
const int RTH_END_SEC = 16 * 3600 + 15 * 60;    // 16:15 = 58500

// ============================================================================
// TEST: RTH session key computation
// ============================================================================
bool test_rth_session_key() {
    // 2024-12-23 at 10:00 (within RTH)
    int date = 20241223;
    int timeSec = 10 * 3600;  // 10:00

    SessionKey key = ComputeSessionKey(date, timeSec, RTH_START_SEC, RTH_END_SEC);

    TEST_ASSERT(key.IsRTH(), "10:00 should be RTH");
    TEST_ASSERT(key.tradingDay == 20241223, "Trading day should be 2024-12-23");
    TEST_ASSERT(key.ToString() == "20241223-RTH", "Key string should be 20241223-RTH");

    TEST_PASSED("RTH session key computation");
}

// ============================================================================
// TEST: Evening GLOBEX session key (after RTH close)
// ============================================================================
bool test_evening_globex_session_key() {
    // 2024-12-23 at 18:00 (after RTH close, evening Globex)
    int date = 20241223;
    int timeSec = 18 * 3600;  // 18:00

    SessionKey key = ComputeSessionKey(date, timeSec, RTH_START_SEC, RTH_END_SEC);

    TEST_ASSERT(key.IsGlobex(), "18:00 should be Globex");
    TEST_ASSERT(key.tradingDay == 20241223, "Evening Globex belongs to same day's RTH");
    TEST_ASSERT(key.ToString() == "20241223-GBX", "Key string should be 20241223-GBX");

    TEST_PASSED("Evening GLOBEX session key");
}

// ============================================================================
// TEST: Morning GLOBEX session key (before RTH open)
// ============================================================================
bool test_morning_globex_session_key() {
    // 2024-12-24 at 08:00 (before RTH open, morning Globex)
    int date = 20241224;
    int timeSec = 8 * 3600;  // 08:00

    SessionKey key = ComputeSessionKey(date, timeSec, RTH_START_SEC, RTH_END_SEC);

    TEST_ASSERT(key.IsGlobex(), "08:00 should be Globex");
    TEST_ASSERT(key.tradingDay == 20241223, "Morning Globex belongs to PREVIOUS day's RTH");
    TEST_ASSERT(key.ToString() == "20241223-GBX", "Key string should be 20241223-GBX");

    TEST_PASSED("Morning GLOBEX session key");
}

// ============================================================================
// TEST: RTH -> Globex boundary transition
// ============================================================================
bool test_rth_to_globex_transition() {
    SessionManager mgr;

    // Start with RTH bar at 15:00
    SessionKey rthKey = ComputeSessionKey(20241223, 15 * 3600, RTH_START_SEC, RTH_END_SEC);
    bool changed1 = mgr.UpdateSession(rthKey);
    // First call should not report change (no prior session)
    TEST_ASSERT(!changed1, "First session should not report change");
    TEST_ASSERT(mgr.IsRTH(), "Should be in RTH");

    // Move to Globex bar at 17:00 (after RTH close)
    SessionKey gbxKey = ComputeSessionKey(20241223, 17 * 3600, RTH_START_SEC, RTH_END_SEC);
    bool changed2 = mgr.UpdateSession(gbxKey);
    TEST_ASSERT(changed2, "RTH -> Globex should report session change");
    TEST_ASSERT(mgr.IsGlobex(), "Should now be in Globex");
    TEST_ASSERT(mgr.previousSession.IsRTH(), "Previous session should be RTH");

    TEST_PASSED("RTH -> Globex boundary transition");
}

// ============================================================================
// TEST: Globex overnight wrap (same session across midnight)
// ============================================================================
bool test_globex_overnight_wrap() {
    SessionManager mgr;

    // Evening Globex on 2024-12-23 at 22:00
    SessionKey eveningKey = ComputeSessionKey(20241223, 22 * 3600, RTH_START_SEC, RTH_END_SEC);
    mgr.UpdateSession(eveningKey);
    TEST_ASSERT(eveningKey.tradingDay == 20241223, "Evening belongs to 12-23");

    // Morning Globex on 2024-12-24 at 02:00 (after midnight, but same Globex session)
    SessionKey morningKey = ComputeSessionKey(20241224, 2 * 3600, RTH_START_SEC, RTH_END_SEC);
    bool changed = mgr.UpdateSession(morningKey);

    // IMPORTANT: Both keys should have same trading day (20241223)
    // so there should be NO session change
    TEST_ASSERT(morningKey.tradingDay == 20241223, "Morning Globex belongs to previous day");
    TEST_ASSERT(!changed, "Overnight Globex should be same session (no change)");

    TEST_PASSED("Globex overnight wrap");
}

// ============================================================================
// TEST: Globex -> RTH trading day roll
// ============================================================================
bool test_globex_to_rth_trading_day_roll() {
    SessionManager mgr;

    // Morning Globex on 2024-12-24 at 08:00 (belongs to 12-23 trading day)
    SessionKey gbxKey = ComputeSessionKey(20241224, 8 * 3600, RTH_START_SEC, RTH_END_SEC);
    mgr.UpdateSession(gbxKey);
    TEST_ASSERT(gbxKey.tradingDay == 20241223, "Pre-RTH Globex belongs to 12-23");

    // RTH open on 2024-12-24 at 09:30 (NEW trading day 12-24)
    SessionKey rthKey = ComputeSessionKey(20241224, RTH_START_SEC, RTH_START_SEC, RTH_END_SEC);
    bool changed = mgr.UpdateSession(rthKey);

    TEST_ASSERT(changed, "Globex -> RTH should trigger session change");
    TEST_ASSERT(rthKey.tradingDay == 20241224, "RTH belongs to 12-24");
    TEST_ASSERT(mgr.previousSession.tradingDay == 20241223, "Previous was 12-23");

    // This is a TRADING DAY ROLL
    bool tradingDayRolled = (mgr.previousSession.tradingDay != mgr.currentSession.tradingDay);
    TEST_ASSERT(tradingDayRolled, "Trading day should have rolled");

    TEST_PASSED("Globex -> RTH trading day roll");
}

// ============================================================================
// TEST: No change when same session continues
// ============================================================================
bool test_no_change_same_session() {
    SessionManager mgr;

    // Multiple RTH bars - no session change
    SessionKey key1 = ComputeSessionKey(20241223, 10 * 3600, RTH_START_SEC, RTH_END_SEC);
    SessionKey key2 = ComputeSessionKey(20241223, 11 * 3600, RTH_START_SEC, RTH_END_SEC);
    SessionKey key3 = ComputeSessionKey(20241223, 12 * 3600, RTH_START_SEC, RTH_END_SEC);

    mgr.UpdateSession(key1);  // First bar
    bool changed2 = mgr.UpdateSession(key2);
    bool changed3 = mgr.UpdateSession(key3);

    TEST_ASSERT(!changed2, "Same RTH session should not trigger change");
    TEST_ASSERT(!changed3, "Same RTH session should not trigger change");

    TEST_PASSED("No change when same session continues");
}

// ============================================================================
// TEST: DecrementDate handles month/year boundaries
// ============================================================================
bool test_decrement_date_boundaries() {
    // January 1st -> December 31st (year rollback)
    TEST_ASSERT(DecrementDate(20240101) == 20231231, "2024-01-01 should become 2023-12-31");

    // March 1st -> Feb 28th (non-leap year)
    TEST_ASSERT(DecrementDate(20230301) == 20230228, "2023-03-01 should become 2023-02-28");

    // March 1st -> Feb 29th (leap year)
    TEST_ASSERT(DecrementDate(20240301) == 20240229, "2024-03-01 should become 2024-02-29");

    // Regular day
    TEST_ASSERT(DecrementDate(20241215) == 20241214, "2024-12-15 should become 2024-12-14");

    TEST_PASSED("DecrementDate handles month/year boundaries");
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "======================================" << std::endl;
    std::cout << "SessionKey Transition Tests" << std::endl;
    std::cout << "======================================" << std::endl;

    test_rth_session_key();
    test_evening_globex_session_key();
    test_morning_globex_session_key();
    test_rth_to_globex_transition();
    test_globex_overnight_wrap();
    test_globex_to_rth_trading_day_roll();
    test_no_change_same_session();
    test_decrement_date_boundaries();

    std::cout << "======================================" << std::endl;
    std::cout << "Results: " << g_passed << " passed, " << g_failed << " failed" << std::endl;
    std::cout << "======================================" << std::endl;

    return g_failed > 0 ? 1 : 0;
}
