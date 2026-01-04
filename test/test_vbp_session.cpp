/**
 * test_vbp_session.cpp
 *
 * Test for VBP loading and session change logic.
 * Tests the simple RTH vs GLOBEX session detection.
 *
 * Compile: g++ -std=c++17 -o test_vbp_session.exe test_vbp_session.cpp
 * Run: ./test_vbp_session.exe
 */

#include <iostream>
#include <cassert>
#include <string>
#include <vector>

// SSOT: Use amt_core.h for SessionPhase enum and helpers
#include "../amt_core.h"

// ============================================================================
// MOCK VBP PROFILE
// ============================================================================

struct VBPProfile {
    double poc = 0.0;
    double vah = 0.0;
    double val = 0.0;
    bool valid = false;

    void Load(double p, double h, double l) {
        poc = p;
        vah = h;
        val = l;
        valid = true;
    }

    void Clear() {
        poc = vah = val = 0.0;
        valid = false;
    }
};

// ============================================================================
// MOCK SESSION STATE (matching reverted logic)
// ============================================================================

struct SessionState {
    AMT::SessionPhase prevPhase = AMT::SessionPhase::UNKNOWN;
    AMT::SessionPhase curPhase = AMT::SessionPhase::UNKNOWN;

    VBPProfile vbpProfile;
    bool amtZonesInitialized = false;

    int zonesClearedCount = 0;
    int zonesCreatedCount = 0;
    int totalTouches = 0;
    int vbpLoadCount = 0;
};

/**
 * REVERTED ProcessBar - uses simple isCurRTH != isPrevRTH
 */
void ProcessBar(SessionState& state, AMT::SessionPhase newPhase, double poc, double vah, double val) {
    state.curPhase = newPhase;

    const bool isCurRTH = AMT::IsRTHSession(state.curPhase);
    const bool isPrevRTH = AMT::IsRTHSession(state.prevPhase);

    // REVERTED: Simple RTH vs GLOBEX comparison
    const bool sessionChanged = (isCurRTH != isPrevRTH);

    if (sessionChanged) {
        // Clear zones on session change
        state.zonesClearedCount++;
        state.totalTouches = 0;
        state.amtZonesInitialized = false;
        state.vbpProfile.Clear();
    }

    // Zone creation: first time or after session change
    if (!state.amtZonesInitialized) {
        // Load VBP profile
        state.vbpProfile.Load(poc, vah, val);
        state.vbpLoadCount++;
        state.zonesCreatedCount++;
        state.amtZonesInitialized = true;
    }

    // Update prev phase for next iteration
    state.prevPhase = state.curPhase;
}

void SimulateTouch(SessionState& state) {
    state.totalTouches++;
}

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

// Test 1: First bar loads VBP
void test_first_bar_loads_vbp() {
    std::cout << "\n=== Test: First bar loads VBP ===" << std::endl;
    SessionState state;

    ProcessBar(state, AMT::SessionPhase::GLOBEX, 6000.0, 6010.0, 5990.0);

    CHECK(state.vbpProfile.valid, "VBP profile is valid");
    CHECK(state.vbpProfile.poc == 6000.0, "VBP POC loaded correctly");
    CHECK(state.vbpProfile.vah == 6010.0, "VBP VAH loaded correctly");
    CHECK(state.vbpProfile.val == 5990.0, "VBP VAL loaded correctly");
    CHECK(state.vbpLoadCount == 1, "VBP loaded once");
    CHECK(state.amtZonesInitialized, "Zones initialized");
}

// Test 2: GLOBEX phases don't reload VBP
void test_globex_no_reload() {
    std::cout << "\n=== Test: GLOBEX phases don't reload VBP ===" << std::endl;
    SessionState state;

    // First bar
    ProcessBar(state, AMT::SessionPhase::GLOBEX, 6000.0, 6010.0, 5990.0);
    SimulateTouch(state);
    SimulateTouch(state);

    int initialLoads = state.vbpLoadCount;
    int initialTouches = state.totalTouches;

    // More GLOBEX bars - should NOT reload
    ProcessBar(state, AMT::SessionPhase::LONDON_OPEN, 6001.0, 6011.0, 5991.0);
    ProcessBar(state, AMT::SessionPhase::PRE_MARKET, 6002.0, 6012.0, 5992.0);
    ProcessBar(state, AMT::SessionPhase::PRE_MARKET, 6003.0, 6013.0, 5993.0);

    CHECK(state.vbpLoadCount == initialLoads, "VBP not reloaded during GLOBEX");
    CHECK(state.totalTouches == initialTouches, "Touches preserved during GLOBEX");
    CHECK(state.vbpProfile.poc == 6000.0, "VBP POC unchanged (first load value)");
}

// Test 3: GLOBEX->RTH triggers reload
void test_globex_to_rth_reloads() {
    std::cout << "\n=== Test: GLOBEX->RTH triggers VBP reload ===" << std::endl;
    SessionState state;

    // GLOBEX session
    ProcessBar(state, AMT::SessionPhase::PRE_MARKET, 6000.0, 6010.0, 5990.0);
    SimulateTouch(state);
    SimulateTouch(state);

    CHECK(state.totalTouches == 2, "Pre-RTH: 2 touches");
    CHECK(state.vbpLoadCount == 1, "Pre-RTH: 1 VBP load");

    // Transition to RTH - should reload
    ProcessBar(state, AMT::SessionPhase::INITIAL_BALANCE, 6050.0, 6060.0, 6040.0);

    CHECK(state.vbpLoadCount == 2, "RTH: VBP reloaded");
    CHECK(state.vbpProfile.poc == 6050.0, "RTH: New POC value");
    CHECK(state.totalTouches == 0, "RTH: Touches reset");
    CHECK(state.zonesClearedCount == 1, "RTH: Zones cleared once");
}

// Test 4: RTH phases don't reload VBP
void test_rth_no_reload() {
    std::cout << "\n=== Test: RTH phases don't reload VBP ===" << std::endl;
    SessionState state;

    // Start in RTH
    ProcessBar(state, AMT::SessionPhase::INITIAL_BALANCE, 6000.0, 6010.0, 5990.0);
    SimulateTouch(state);
    SimulateTouch(state);
    SimulateTouch(state);

    int initialLoads = state.vbpLoadCount;

    // More RTH bars
    ProcessBar(state, AMT::SessionPhase::MID_SESSION, 6001.0, 6011.0, 5991.0);
    ProcessBar(state, AMT::SessionPhase::CLOSING_SESSION, 6002.0, 6012.0, 5992.0);

    CHECK(state.vbpLoadCount == initialLoads, "VBP not reloaded during RTH");
    CHECK(state.totalTouches == 3, "Touches preserved during RTH");
}

// Test 5: RTH->GLOBEX triggers reload
void test_rth_to_globex_reloads() {
    std::cout << "\n=== Test: RTH->GLOBEX triggers VBP reload ===" << std::endl;
    SessionState state;

    // RTH session
    ProcessBar(state, AMT::SessionPhase::CLOSING_SESSION, 6000.0, 6010.0, 5990.0);
    SimulateTouch(state);

    // Transition to GLOBEX
    ProcessBar(state, AMT::SessionPhase::POST_CLOSE, 6100.0, 6110.0, 6090.0);

    CHECK(state.vbpLoadCount == 2, "GLOBEX: VBP reloaded");
    CHECK(state.vbpProfile.poc == 6100.0, "GLOBEX: New POC value");
    CHECK(state.totalTouches == 0, "GLOBEX: Touches reset");
}

// Test 6: UNKNOWN phase handling
void test_unknown_phase() {
    std::cout << "\n=== Test: UNKNOWN phase handling ===" << std::endl;
    SessionState state;

    // Start with UNKNOWN (simulates startup)
    // prevPhase = UNKNOWN (default), curPhase = GLOBEX
    // IsRTH(UNKNOWN) = false, IsRTH(GLOBEX) = false
    // sessionChanged = (false != false) = false
    ProcessBar(state, AMT::SessionPhase::GLOBEX, 6000.0, 6010.0, 5990.0);

    CHECK(state.vbpLoadCount == 1, "First bar: VBP loaded");
    CHECK(state.zonesClearedCount == 0, "First bar: No session change (UNKNOWN->GLOBEX both non-RTH)");

    SimulateTouch(state);

    // Another UNKNOWN shouldn't cause reload
    ProcessBar(state, AMT::SessionPhase::UNKNOWN, 6001.0, 6011.0, 5991.0);

    CHECK(state.vbpLoadCount == 1, "UNKNOWN: VBP not reloaded");
    CHECK(state.totalTouches == 1, "UNKNOWN: Touch preserved");
}

// Test 7: Full session cycle
void test_full_session_cycle() {
    std::cout << "\n=== Test: Full session cycle ===" << std::endl;
    SessionState state;

    // Evening GLOBEX
    ProcessBar(state, AMT::SessionPhase::GLOBEX, 6000.0, 6010.0, 5990.0);
    SimulateTouch(state);
    ProcessBar(state, AMT::SessionPhase::LONDON_OPEN, 6001.0, 6011.0, 5991.0);
    SimulateTouch(state);
    ProcessBar(state, AMT::SessionPhase::PRE_MARKET, 6002.0, 6012.0, 5992.0);
    SimulateTouch(state);

    CHECK(state.totalTouches == 3, "End GLOBEX: 3 touches");
    CHECK(state.vbpLoadCount == 1, "End GLOBEX: 1 VBP load");

    // RTH
    ProcessBar(state, AMT::SessionPhase::INITIAL_BALANCE, 6050.0, 6060.0, 6040.0);
    CHECK(state.totalTouches == 0, "RTH start: Touches reset");
    CHECK(state.vbpLoadCount == 2, "RTH start: VBP reloaded");

    SimulateTouch(state);
    SimulateTouch(state);
    ProcessBar(state, AMT::SessionPhase::MID_SESSION, 6051.0, 6061.0, 6041.0);
    ProcessBar(state, AMT::SessionPhase::CLOSING_SESSION, 6052.0, 6062.0, 6042.0);

    CHECK(state.totalTouches == 2, "End RTH: 2 touches preserved");
    CHECK(state.vbpLoadCount == 2, "End RTH: No extra VBP loads");

    // Post-close GLOBEX
    ProcessBar(state, AMT::SessionPhase::POST_CLOSE, 6100.0, 6110.0, 6090.0);
    CHECK(state.vbpLoadCount == 3, "Post-close: VBP reloaded");
    CHECK(state.totalTouches == 0, "Post-close: Touches reset");

    std::cout << "  Total VBP loads: " << state.vbpLoadCount << " (expected: 3)" << std::endl;
    std::cout << "  Total zone clears: " << state.zonesClearedCount << " (expected: 2)" << std::endl;
}

// Test 8: Midnight crossing (no session change within GLOBEX)
void test_midnight_crossing() {
    std::cout << "\n=== Test: Midnight crossing (GLOBEX continuous) ===" << std::endl;
    SessionState state;

    // Evening GLOBEX (before midnight)
    ProcessBar(state, AMT::SessionPhase::GLOBEX, 6000.0, 6010.0, 5990.0);
    SimulateTouch(state);
    SimulateTouch(state);

    // Simulate bars crossing midnight - still GLOBEX phases
    ProcessBar(state, AMT::SessionPhase::PRE_MARKET, 6001.0, 6011.0, 5991.0);
    SimulateTouch(state);
    ProcessBar(state, AMT::SessionPhase::PRE_MARKET, 6002.0, 6012.0, 5992.0);
    SimulateTouch(state);

    CHECK(state.vbpLoadCount == 1, "Midnight: VBP NOT reloaded (same GLOBEX session)");
    CHECK(state.totalTouches == 4, "Midnight: All 4 touches preserved");
    CHECK(state.zonesClearedCount == 0, "Midnight: No zone clears");
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "=== VBP Session Load Tests (Reverted Logic) ===" << std::endl;

    test_first_bar_loads_vbp();
    test_globex_no_reload();
    test_globex_to_rth_reloads();
    test_rth_no_reload();
    test_rth_to_globex_reloads();
    test_unknown_phase();
    test_full_session_cycle();
    test_midnight_crossing();

    std::cout << "\n==========================================" << std::endl;
    std::cout << "PASSED: " << testsPassed << std::endl;
    std::cout << "FAILED: " << testsFailed << std::endl;
    std::cout << "==========================================" << std::endl;

    if (testsFailed == 0) {
        std::cout << "\nREVERTED LOGIC VERIFIED:" << std::endl;
        std::cout << "- VBP loads on first bar" << std::endl;
        std::cout << "- VBP reloads ONLY on RTH<->GLOBEX transitions" << std::endl;
        std::cout << "- Touches persist within session" << std::endl;
        std::cout << "- Midnight crossing does NOT trigger reload" << std::endl;
    }

    return testsFailed > 0 ? 1 : 0;
}
