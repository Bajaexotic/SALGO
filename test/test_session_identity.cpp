/**
 * test_session_identity.cpp
 *
 * Tests for SessionKey from amt_core.h (SSOT for session identity).
 * NO UNKNOWN state - session is always determinable from bar time.
 *
 * Compile: g++ -std=c++17 -o test_session_identity.exe test_session_identity.cpp
 * Run: ./test_session_identity.exe
 */

#include <iostream>
#include <cassert>
#include <string>

// SSOT: Use amt_core.h for SessionKey and ComputeSessionKey
#include "../amt_core.h"

// Simulated time helpers
struct BarTime {
    int date;      // YYYYMMDD
    int timeOfDay; // Seconds since midnight

    // RTH is 9:30 AM - 4:00 PM Eastern = 34200 - 57600 seconds
    static constexpr int RTH_START = 34200;  // 9:30 AM
    static constexpr int RTH_END = 57600;    // 4:00 PM
};

/**
 * Wrapper that uses SSOT ComputeSessionKey from amt_core.h
 */
inline AMT::SessionKey ComputeSessionKeyFromBarTime(const BarTime& barTime) {
    return AMT::ComputeSessionKey(barTime.date, barTime.timeOfDay, BarTime::RTH_START, BarTime::RTH_END);
}

// ============================================================================
// CLEAN ZONE MANAGER - Uses SessionKey (SSOT from amt_core.h)
// ============================================================================

struct CleanZoneState {
    AMT::SessionKey currentSession;
    bool initialized = false;
    int zonesClearedCount = 0;
    int totalTouches = 0;

    void ProcessBar(const BarTime& barTime) {
        AMT::SessionKey newSession = ComputeSessionKeyFromBarTime(barTime);

        // Session change = identity changed (day changed OR RTH/GBX changed)
        bool sessionChanged = initialized && (newSession != currentSession);

        // First bar OR session change -> clear and recreate zones
        if (!initialized || sessionChanged) {
            zonesClearedCount++;
            totalTouches = 0;  // Reset touches for new session
        }

        currentSession = newSession;
        initialized = true;
    }

    void SimulateTouch() {
        totalTouches++;
    }
};

// ============================================================================
// TESTS
// ============================================================================

int testsPassed = 0;
int testsFailed = 0;

void CHECK(bool condition, const std::string& testName) {
    if (condition) {
        std::cout << "[PASS] " << testName << std::endl;
        testsPassed++;
    } else {
        std::cout << "[FAIL] " << testName << std::endl;
        testsFailed++;
    }
}

// Test 1: First bar initializes session
void test_first_bar() {
    CleanZoneState state;

    BarTime bar1{20241222, 10000};  // 2:46 AM - GLOBEX
    state.ProcessBar(bar1);

    CHECK(state.initialized == true, "First bar: initialized");
    // Note: At 2:46 AM (before RTH open), trading day is PREVIOUS day
    // Morning GLOBEX belongs to the prior RTH per ComputeSessionKey logic
    CHECK(state.currentSession.tradingDay == 20241221, "First bar: correct date (prior trading day)");
    CHECK(state.currentSession.IsGlobex(), "First bar: GLOBEX");
    CHECK(state.zonesClearedCount == 1, "First bar: zones created (1 clear)");
}

// Test 2: Same session, different times - NO clear
void test_same_session_no_clear() {
    CleanZoneState state;

    // GLOBEX morning
    state.ProcessBar({20241222, 10000});   // 2:46 AM
    state.SimulateTouch();
    state.SimulateTouch();

    state.ProcessBar({20241222, 20000});   // 5:33 AM (London open area)
    state.SimulateTouch();

    state.ProcessBar({20241222, 30000});   // 8:20 AM (pre-market)
    state.SimulateTouch();

    CHECK(state.zonesClearedCount == 1, "Same GLOBEX session: only 1 clear (init)");
    CHECK(state.totalTouches == 4, "Same GLOBEX session: touches preserved");
}

// Test 3: GLOBEX -> RTH transition clears
void test_globex_to_rth() {
    CleanZoneState state;

    // GLOBEX
    state.ProcessBar({20241222, 30000});  // 8:20 AM
    state.SimulateTouch();
    state.SimulateTouch();

    CHECK(state.totalTouches == 2, "Pre-RTH: 2 touches");

    // RTH starts (9:30 AM = 34200 sec)
    state.ProcessBar({20241222, 34200});

    CHECK(state.currentSession.IsRTH(), "Now in RTH");
    CHECK(state.zonesClearedCount == 2, "GLOBEX->RTH: zones cleared");
    CHECK(state.totalTouches == 0, "GLOBEX->RTH: touches reset");
}

// Test 4: RTH -> GLOBEX (post-close) clears
void test_rth_to_globex() {
    CleanZoneState state;

    // RTH
    state.ProcessBar({20241222, 40000});  // ~11 AM RTH
    state.SimulateTouch();

    // Post-close (4:00 PM = 57600 sec)
    state.ProcessBar({20241222, 57600});

    CHECK(state.currentSession.IsGlobex(), "Now in GLOBEX (post-close)");
    CHECK(state.zonesClearedCount == 2, "RTH->GLOBEX: zones cleared");
}

// Test 5: Within RTH - NO clear
void test_within_rth_no_clear() {
    CleanZoneState state;

    // RTH start
    state.ProcessBar({20241222, 34200});  // 9:30 AM
    state.SimulateTouch();

    // Mid RTH
    state.ProcessBar({20241222, 45000});  // 12:30 PM
    state.SimulateTouch();
    state.SimulateTouch();

    // Late RTH
    state.ProcessBar({20241222, 55000});  // 3:16 PM
    state.SimulateTouch();

    CHECK(state.zonesClearedCount == 1, "Within RTH: only 1 clear (init)");
    CHECK(state.totalTouches == 4, "Within RTH: all touches preserved");
}

// Test 6: Day change triggers clear
void test_day_change() {
    CleanZoneState state;

    // Day 1 GLOBEX
    state.ProcessBar({20241222, 10000});
    state.SimulateTouch();

    // Day 2 GLOBEX (same time of day, different date)
    state.ProcessBar({20241223, 10000});

    CHECK(state.zonesClearedCount == 2, "Day change: zones cleared");
    CHECK(state.totalTouches == 0, "Day change: touches reset");
}

// Test 7: Full 24-hour cycle
void test_full_cycle() {
    CleanZoneState state;
    std::cout << "\n  === Full 24-Hour Cycle ===\n";

    // GLOBEX overnight (Dec 22)
    state.ProcessBar({20241222, 3600});   // 1 AM
    state.SimulateTouch();
    std::cout << "  1 AM GBX: " << state.currentSession.ToString()
              << " clears=" << state.zonesClearedCount << "\n";

    // Pre-market
    state.ProcessBar({20241222, 32400});  // 9 AM
    state.SimulateTouch();
    std::cout << "  9 AM GBX: " << state.currentSession.ToString()
              << " clears=" << state.zonesClearedCount << "\n";

    // RTH open
    state.ProcessBar({20241222, 34200});  // 9:30 AM
    state.SimulateTouch();
    std::cout << "  9:30 AM RTH: " << state.currentSession.ToString()
              << " clears=" << state.zonesClearedCount << "\n";

    // RTH mid
    state.ProcessBar({20241222, 45000});  // 12:30 PM
    state.SimulateTouch();
    std::cout << "  12:30 PM RTH: " << state.currentSession.ToString()
              << " clears=" << state.zonesClearedCount << "\n";

    // RTH close
    state.ProcessBar({20241222, 57000});  // 3:50 PM
    state.SimulateTouch();
    std::cout << "  3:50 PM RTH: " << state.currentSession.ToString()
              << " clears=" << state.zonesClearedCount << "\n";

    // Post-close (GLOBEX)
    state.ProcessBar({20241222, 57600});  // 4:00 PM
    state.SimulateTouch();
    std::cout << "  4:00 PM GBX: " << state.currentSession.ToString()
              << " clears=" << state.zonesClearedCount << "\n";

    // Evening GLOBEX
    state.ProcessBar({20241222, 72000});  // 8 PM
    state.SimulateTouch();
    std::cout << "  8 PM GBX: " << state.currentSession.ToString()
              << " clears=" << state.zonesClearedCount << "\n";

    // Expect: 3 clears total (init + GBX->RTH + RTH->GBX)
    CHECK(state.zonesClearedCount == 3, "Full cycle: exactly 3 clears");
}

// Test 8: NO UNKNOWN STATE ANYWHERE
void test_no_unknown_state() {
    // This test verifies there's no "unknown" concept in the clean design

    AMT::SessionKey id1 = ComputeSessionKeyFromBarTime({20241222, 0});      // Midnight
    AMT::SessionKey id2 = ComputeSessionKeyFromBarTime({20241222, 43200});  // Noon
    AMT::SessionKey id3 = ComputeSessionKeyFromBarTime({20241222, 86399});  // 11:59:59 PM

    CHECK(id1.IsValid(), "Midnight: valid session key");
    CHECK(id2.IsValid(), "Noon: valid session key");
    CHECK(id3.IsValid(), "11:59 PM: valid session key");

    // SessionType is always deterministic (never "unknown")
    CHECK(id1.IsGlobex(), "Midnight: GLOBEX");
    CHECK(id2.IsRTH(), "Noon: RTH");
    CHECK(id3.IsGlobex(), "11:59 PM: GLOBEX");

    std::cout << "  No UNKNOWN state - session always determinable!\n";
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "=== Clean Session Identity Tests (No UNKNOWN) ===\n\n";

    test_first_bar();
    std::cout << std::endl;

    test_same_session_no_clear();
    std::cout << std::endl;

    test_globex_to_rth();
    std::cout << std::endl;

    test_rth_to_globex();
    std::cout << std::endl;

    test_within_rth_no_clear();
    std::cout << std::endl;

    test_day_change();
    std::cout << std::endl;

    test_full_cycle();
    std::cout << std::endl;

    test_no_unknown_state();
    std::cout << std::endl;

    std::cout << "==========================================\n";
    std::cout << "PASSED: " << testsPassed << "\n";
    std::cout << "FAILED: " << testsFailed << "\n";
    std::cout << "==========================================\n";

    if (testsFailed == 0) {
        std::cout << "\nCLEAN DESIGN BENEFITS:\n";
        std::cout << "1. No UNKNOWN state - session always determinable from bar time\n";
        std::cout << "2. SessionKey = (TradingDay, SessionType) - simple and deterministic\n";
        std::cout << "3. Reset() cannot break session tracking (no UNKNOWN to trigger false changes)\n";
        std::cout << "4. Zone clears ONLY on actual session boundaries\n";
    }

    return testsFailed > 0 ? 1 : 0;
}
