// Define standalone test mode to exclude SC-dependent functions
#define AMT_STANDALONE_TEST

// test_validation_comprehensive.cpp
// Comprehensive tests for Phase 3 validation system
// Compile: g++ -std=c++17 -I. -o test_validation_comprehensive.exe test_validation_comprehensive.cpp

#include <iostream>
#include <iomanip>
#include <vector>
#include <cmath>
#include <string>
#include <random>

// Mock Sierra Chart types
#ifndef SIERRACHART_H
#define SIERRACHART_H
struct SCDateTime {
    double m_dt = 0.0;
    SCDateTime() = default;
    SCDateTime(double d) : m_dt(d) {}
    double GetAsDouble() const { return m_dt; }
    SCDateTime operator+(double d) const { return SCDateTime(m_dt + d); }
};
#endif

#define VALIDATE_ZONE_MIGRATION
#include "AMT_Zones.h"

int g_passed = 0;
int g_failed = 0;
int g_testNum = 0;

#define TEST_SECTION(name) std::cout << "\n=== " << name << " ===" << std::endl
#define TEST(name) do { g_testNum++; std::cout << "  " << g_testNum << ". " << name << "... "; } while(0)
#define PASS() do { std::cout << "[PASS]" << std::endl; g_passed++; } while(0)
#define FAIL(msg) do { std::cout << "[FAIL] " << msg << std::endl; g_failed++; } while(0)

// =============================================================================
// TEST: Episode Matching Edge Cases
// =============================================================================

void test_episode_matching_same_anchor() {
    TEST("Same anchor, same type matches");

    const double tickSize = 0.25;
    AMT::ValidationEpisode leg, amt;
    leg.isLegacy = true;
    leg.zoneType = AMT::ZoneType::VPB_POC;
    leg.anchorPrice = 5000.0;
    leg.entryBar = 100;
    leg.exitBar = 110;

    amt.isLegacy = false;
    amt.zoneType = AMT::ZoneType::VPB_POC;
    amt.anchorPrice = 5000.0;
    amt.entryBar = 100;
    amt.exitBar = 110;

    if (leg.CouldMatch(amt, tickSize)) {
        PASS();
    } else {
        FAIL("Should match with identical anchors");
    }
}

void test_episode_matching_anchor_off_by_half_tick() {
    TEST("Anchor off by 0.1 tick still matches (rounds to same tick)");

    const double tickSize = 0.25;
    AMT::ValidationEpisode leg, amt;
    leg.isLegacy = true;
    leg.zoneType = AMT::ZoneType::VPB_POC;
    leg.anchorPrice = 5000.0;
    leg.entryBar = 100;

    amt.isLegacy = false;
    amt.zoneType = AMT::ZoneType::VPB_POC;
    amt.anchorPrice = 5000.0 + tickSize * 0.1;  // 0.025 off
    amt.entryBar = 100;

    // Both should round to same tick
    if (leg.GetAnchorInTicks(tickSize) == amt.GetAnchorInTicks(tickSize)) {
        PASS();
    } else {
        FAIL("Should round to same tick");
    }
}

void test_episode_matching_anchor_off_by_one_tick() {
    TEST("Anchor off by 1 tick does NOT match");

    const double tickSize = 0.25;
    AMT::ValidationEpisode leg, amt;
    leg.isLegacy = true;
    leg.zoneType = AMT::ZoneType::VPB_POC;
    leg.anchorPrice = 5000.0;
    leg.entryBar = 100;

    amt.isLegacy = false;
    amt.zoneType = AMT::ZoneType::VPB_POC;
    amt.anchorPrice = 5000.0 + tickSize;  // 1 tick off
    amt.entryBar = 100;

    if (!leg.CouldMatch(amt, tickSize)) {
        PASS();
    } else {
        FAIL("Should NOT match with different anchors");
    }
}

void test_episode_matching_different_types() {
    TEST("Same anchor but different zone type does NOT match");

    const double tickSize = 0.25;
    AMT::ValidationEpisode leg, amt;
    leg.isLegacy = true;
    leg.zoneType = AMT::ZoneType::VPB_POC;
    leg.anchorPrice = 5000.0;
    leg.entryBar = 100;

    amt.isLegacy = false;
    amt.zoneType = AMT::ZoneType::VPB_VAH;  // Different type
    amt.anchorPrice = 5000.0;
    amt.entryBar = 100;

    if (!leg.CouldMatch(amt, tickSize)) {
        PASS();
    } else {
        FAIL("Should NOT match with different zone types");
    }
}

void test_episode_matching_entry_within_tolerance() {
    TEST("Entry bar within tolerance (±1) matches");

    const double tickSize = 0.25;
    AMT::ValidationEpisode leg, amt;
    leg.isLegacy = true;
    leg.zoneType = AMT::ZoneType::VPB_POC;
    leg.anchorPrice = 5000.0;
    leg.entryBar = 100;
    leg.exitBar = 110;

    amt.isLegacy = false;
    amt.zoneType = AMT::ZoneType::VPB_POC;
    amt.anchorPrice = 5000.0;
    amt.entryBar = 101;  // 1 bar off
    amt.exitBar = 111;

    if (leg.CouldMatch(amt, tickSize, 1)) {
        PASS();
    } else {
        FAIL("Should match within tolerance");
    }
}

void test_episode_matching_entry_outside_tolerance() {
    TEST("Entry bar outside tolerance (>2) does NOT match");

    const double tickSize = 0.25;
    AMT::ValidationEpisode leg, amt;
    leg.isLegacy = true;
    leg.zoneType = AMT::ZoneType::VPB_POC;
    leg.anchorPrice = 5000.0;
    leg.entryBar = 100;
    leg.exitBar = 110;

    amt.isLegacy = false;
    amt.zoneType = AMT::ZoneType::VPB_POC;
    amt.anchorPrice = 5000.0;
    amt.entryBar = 105;  // 5 bars off
    amt.exitBar = 115;

    if (!leg.CouldMatch(amt, tickSize, 2)) {
        PASS();
    } else {
        FAIL("Should NOT match outside tolerance");
    }
}

void test_episode_matching_no_overlap() {
    TEST("Non-overlapping intervals do NOT match");

    const double tickSize = 0.25;
    AMT::ValidationEpisode leg, amt;
    leg.isLegacy = true;
    leg.zoneType = AMT::ZoneType::VPB_POC;
    leg.anchorPrice = 5000.0;
    leg.entryBar = 100;
    leg.exitBar = 110;

    amt.isLegacy = false;
    amt.zoneType = AMT::ZoneType::VPB_POC;
    amt.anchorPrice = 5000.0;
    amt.entryBar = 200;  // Completely different time
    amt.exitBar = 210;

    if (!leg.CouldMatch(amt, tickSize, 2)) {
        PASS();
    } else {
        FAIL("Should NOT match non-overlapping intervals");
    }
}

// =============================================================================
// TEST: Comparison Logic
// =============================================================================

void test_compare_exact_match() {
    TEST("Identical episodes compare as NONE (no mismatch)");

    AMT::ValidationState vs;
    AMT::ValidationEpisode leg, amt;
    leg.entryBar = 100;
    leg.exitBar = 110;
    leg.barsEngaged = 10;
    leg.escapeVelocity = 2.5;
    leg.coreWidthTicks = 3;
    leg.haloWidthTicks = 5;

    amt = leg;

    if (vs.CompareEpisodes(leg, amt) == AMT::ValidationMismatchReason::NONE) {
        PASS();
    } else {
        FAIL("Should be NONE for identical episodes");
    }
}

void test_compare_entry_bar_diff() {
    TEST("Entry bar diff beyond tolerance returns ENTRY_BAR_DIFF");

    AMT::ValidationState vs;
    vs.tolerances.barTolerance = 1;

    AMT::ValidationEpisode leg, amt;
    leg.entryBar = 100;
    leg.exitBar = 110;
    leg.barsEngaged = 10;
    leg.escapeVelocity = 2.5;
    leg.coreWidthTicks = 3;
    leg.haloWidthTicks = 5;

    amt = leg;
    amt.entryBar = 103;  // 3 bars off, beyond tolerance of 1

    if (vs.CompareEpisodes(leg, amt) == AMT::ValidationMismatchReason::ENTRY_BAR_DIFF) {
        PASS();
    } else {
        FAIL("Should detect ENTRY_BAR_DIFF");
    }
}

void test_compare_bars_engaged_diff() {
    TEST("Different barsEngaged returns BARS_ENGAGED_DIFF");

    AMT::ValidationState vs;
    AMT::ValidationEpisode leg, amt;
    leg.entryBar = 100;
    leg.exitBar = 110;
    leg.barsEngaged = 10;
    leg.escapeVelocity = 2.5;
    leg.coreWidthTicks = 3;
    leg.haloWidthTicks = 5;

    amt = leg;
    amt.barsEngaged = 12;  // Different

    if (vs.CompareEpisodes(leg, amt) == AMT::ValidationMismatchReason::BARS_ENGAGED_DIFF) {
        PASS();
    } else {
        FAIL("Should detect BARS_ENGAGED_DIFF");
    }
}

void test_compare_escape_vel_within_epsilon() {
    TEST("Escape velocity within epsilon is OK");

    AMT::ValidationState vs;
    vs.tolerances.escVelEpsilon = 1e-6;

    AMT::ValidationEpisode leg, amt;
    leg.entryBar = 100;
    leg.exitBar = 110;
    leg.barsEngaged = 10;
    leg.escapeVelocity = 2.5;
    leg.coreWidthTicks = 3;
    leg.haloWidthTicks = 5;

    amt = leg;
    amt.escapeVelocity = 2.5 + 1e-7;  // Within epsilon

    if (vs.CompareEpisodes(leg, amt) == AMT::ValidationMismatchReason::NONE) {
        PASS();
    } else {
        FAIL("Should be NONE for escape vel within epsilon");
    }
}

void test_compare_escape_vel_outside_epsilon() {
    TEST("Escape velocity outside epsilon returns ESC_VEL_DIFF");

    AMT::ValidationState vs;
    vs.tolerances.escVelEpsilon = 1e-6;

    AMT::ValidationEpisode leg, amt;
    leg.entryBar = 100;
    leg.exitBar = 110;
    leg.barsEngaged = 10;
    leg.escapeVelocity = 2.5;
    leg.coreWidthTicks = 3;
    leg.haloWidthTicks = 5;

    amt = leg;
    amt.escapeVelocity = 2.6;  // 0.1 diff, way outside epsilon

    if (vs.CompareEpisodes(leg, amt) == AMT::ValidationMismatchReason::ESC_VEL_DIFF) {
        PASS();
    } else {
        FAIL("Should detect ESC_VEL_DIFF");
    }
}

void test_compare_width_core_diff() {
    TEST("Different core width returns WIDTH_CORE_DIFF");

    AMT::ValidationState vs;
    AMT::ValidationEpisode leg, amt;
    leg.entryBar = 100;
    leg.exitBar = 110;
    leg.barsEngaged = 10;
    leg.escapeVelocity = 2.5;
    leg.coreWidthTicks = 3;
    leg.haloWidthTicks = 5;

    amt = leg;
    amt.coreWidthTicks = 4;  // Different

    if (vs.CompareEpisodes(leg, amt) == AMT::ValidationMismatchReason::WIDTH_CORE_DIFF) {
        PASS();
    } else {
        FAIL("Should detect WIDTH_CORE_DIFF");
    }
}

void test_compare_width_halo_diff() {
    TEST("Different halo width returns WIDTH_HALO_DIFF");

    AMT::ValidationState vs;
    AMT::ValidationEpisode leg, amt;
    leg.entryBar = 100;
    leg.exitBar = 110;
    leg.barsEngaged = 10;
    leg.escapeVelocity = 2.5;
    leg.coreWidthTicks = 3;
    leg.haloWidthTicks = 5;

    amt = leg;
    amt.haloWidthTicks = 7;  // Different

    if (vs.CompareEpisodes(leg, amt) == AMT::ValidationMismatchReason::WIDTH_HALO_DIFF) {
        PASS();
    } else {
        FAIL("Should detect WIDTH_HALO_DIFF");
    }
}

// =============================================================================
// TEST: Auto-Matching in AddEpisode
// =============================================================================

void test_auto_matching_legacy_first() {
    TEST("Legacy added first, then AMT auto-matches");

    const double tickSize = 0.25;
    AMT::ValidationState vs;
    vs.StartSession(0);

    AMT::ValidationEpisode legEp;
    legEp.zoneType = AMT::ZoneType::VPB_POC;
    legEp.anchorPrice = 5000.0;
    legEp.entryBar = 100;
    legEp.exitBar = 110;

    vs.AddLegacyEpisode(legEp, tickSize);

    if (vs.legacyEpisodes[0].matched) {
        FAIL("Legacy should NOT be matched yet");
        return;
    }

    AMT::ValidationEpisode amtEp = legEp;
    vs.AddAmtEpisode(amtEp, tickSize);

    if (vs.legacyEpisodes[0].matched && vs.amtEpisodes[0].matched && vs.counters.matchedCount == 1) {
        PASS();
    } else {
        FAIL("Both should be matched after adding AMT");
    }
}

void test_auto_matching_amt_first() {
    TEST("AMT added first, then legacy auto-matches");

    const double tickSize = 0.25;
    AMT::ValidationState vs;
    vs.StartSession(0);

    AMT::ValidationEpisode amtEp;
    amtEp.zoneType = AMT::ZoneType::VPB_POC;
    amtEp.anchorPrice = 5000.0;
    amtEp.entryBar = 100;
    amtEp.exitBar = 110;

    vs.AddAmtEpisode(amtEp, tickSize);

    if (vs.amtEpisodes[0].matched) {
        FAIL("AMT should NOT be matched yet");
        return;
    }

    AMT::ValidationEpisode legEp = amtEp;
    vs.AddLegacyEpisode(legEp, tickSize);

    if (vs.legacyEpisodes[0].matched && vs.amtEpisodes[0].matched && vs.counters.matchedCount == 1) {
        PASS();
    } else {
        FAIL("Both should be matched after adding legacy");
    }
}

void test_no_double_matching() {
    TEST("Already-matched episode doesn't match again");

    const double tickSize = 0.25;
    AMT::ValidationState vs;
    vs.StartSession(0);

    // Add two identical legacy episodes
    AMT::ValidationEpisode legEp;
    legEp.zoneType = AMT::ZoneType::VPB_POC;
    legEp.anchorPrice = 5000.0;
    legEp.entryBar = 100;
    legEp.exitBar = 110;

    vs.AddLegacyEpisode(legEp, tickSize);
    vs.AddLegacyEpisode(legEp, tickSize);  // Second identical

    // Add one AMT
    AMT::ValidationEpisode amtEp = legEp;
    vs.AddAmtEpisode(amtEp, tickSize);

    // Only one should match
    int matchedLegacy = 0;
    for (const auto& ep : vs.legacyEpisodes) {
        if (ep.matched) matchedLegacy++;
    }

    if (matchedLegacy == 1 && vs.counters.matchedCount == 1) {
        PASS();
    } else {
        FAIL("Only one legacy should match");
    }
}

// =============================================================================
// TEST: Counter Accuracy
// =============================================================================

void test_counters_increment() {
    TEST("Counters increment correctly for each reason");

    AMT::ValidationCounters vc;

    vc.IncrementForReason(AMT::ValidationMismatchReason::ENTRY_BAR_DIFF);
    vc.IncrementForReason(AMT::ValidationMismatchReason::ENTRY_BAR_DIFF);
    vc.IncrementForReason(AMT::ValidationMismatchReason::EXIT_BAR_DIFF);
    vc.IncrementForReason(AMT::ValidationMismatchReason::ESC_VEL_DIFF);
    vc.IncrementForReason(AMT::ValidationMismatchReason::WIDTH_CORE_DIFF);
    vc.IncrementForReason(AMT::ValidationMismatchReason::WIDTH_HALO_DIFF);
    vc.IncrementForReason(AMT::ValidationMismatchReason::MISSING_LEGACY_EPISODE);
    vc.IncrementForReason(AMT::ValidationMismatchReason::MISSING_AMT_EPISODE);

    bool ok = (vc.entryBarDiffCount == 2) &&
              (vc.exitBarDiffCount == 1) &&
              (vc.escVelDiffCount == 1) &&
              (vc.widthCoreDiffCount == 1) &&
              (vc.widthHaloDiffCount == 1) &&
              (vc.missingLegacyCount == 1) &&
              (vc.missingAmtCount == 1);

    if (ok) {
        PASS();
    } else {
        FAIL("Counters don't match expected values");
    }
}

void test_counters_reset() {
    TEST("Counters reset to zero");

    AMT::ValidationCounters vc;
    vc.entryBarDiffCount = 5;
    vc.mismatchCount = 10;
    vc.legacyFinalizedCount = 20;

    vc.Reset();

    if (vc.entryBarDiffCount == 0 && vc.mismatchCount == 0 && vc.legacyFinalizedCount == 0) {
        PASS();
    } else {
        FAIL("Counters not reset");
    }
}

// =============================================================================
// TEST: Ring Buffer
// =============================================================================

void test_ring_buffer_eviction() {
    TEST("Ring buffer evicts oldest when full");

    const double tickSize = 0.25;
    AMT::ValidationState vs;
    vs.StartSession(0);

    // Add MAX_EPISODES + 5 episodes
    for (int i = 0; i < AMT::ValidationState::MAX_EPISODES + 5; i++) {
        AMT::ValidationEpisode ep;
        ep.zoneType = AMT::ZoneType::VPB_POC;
        ep.anchorPrice = 5000.0 + i * tickSize;
        ep.entryBar = i * 10;
        vs.AddLegacyEpisode(ep, tickSize);
    }

    bool sizeOk = (vs.legacyEpisodes.size() == AMT::ValidationState::MAX_EPISODES);
    bool firstOk = (vs.legacyEpisodes[0].entryBar == 50);  // First 5 evicted

    if (sizeOk && firstOk) {
        PASS();
    } else {
        FAIL("Ring buffer eviction failed");
    }
}

// =============================================================================
// TEST: Session Management
// =============================================================================

void test_session_start_clears_state() {
    TEST("StartSession clears all state");

    const double tickSize = 0.25;
    AMT::ValidationState vs;

    // Add some data
    AMT::ValidationEpisode ep;
    ep.zoneType = AMT::ZoneType::VPB_POC;
    ep.anchorPrice = 5000.0;
    vs.AddLegacyEpisode(ep, tickSize);
    vs.counters.mismatchCount = 5;

    // Start new session
    vs.StartSession(100);

    bool cleared = vs.legacyEpisodes.empty() &&
                   vs.amtEpisodes.empty() &&
                   vs.counters.mismatchCount == 0 &&
                   vs.sessionStartBar == 100 &&
                   vs.sessionActive;

    if (cleared) {
        PASS();
    } else {
        FAIL("Session start didn't clear state");
    }
}

// =============================================================================
// TEST: Width Parity State
// =============================================================================

void test_width_parity_tracking() {
    TEST("WidthParityState tracks updates correctly");

    AMT::WidthParityState ws;

    ws.RecordLegacyUpdate(5, 100);
    ws.RecordAmtUpdate(5, 8, 100);

    bool ok = (ws.lastLegacyLiqTicks == 5) &&
              (ws.lastAmtCoreTicks == 5) &&
              (ws.lastAmtHaloTicks == 8) &&
              (ws.lastUpdateBar == 100);

    if (ok) {
        PASS();
    } else {
        FAIL("Width tracking incorrect");
    }
}

// =============================================================================
// TEST: Full Session Simulation
// =============================================================================

void test_full_session_simulation() {
    TEST("Full session with 10 engagements");

    const double tickSize = 0.25;
    AMT::ValidationState vs;
    vs.StartSession(0);

    // Simulate 10 matching engagements
    for (int i = 0; i < 10; i++) {
        AMT::ValidationEpisode ep;
        ep.zoneType = AMT::ZoneType::VPB_POC;
        ep.anchorPrice = 5000.0;
        ep.entryBar = i * 100;
        ep.exitBar = i * 100 + 20;
        ep.barsEngaged = 20;
        ep.escapeVelocity = 2.0 + i * 0.1;
        ep.coreWidthTicks = 3;
        ep.haloWidthTicks = 5;

        vs.AddLegacyEpisode(ep, tickSize);
        vs.AddAmtEpisode(ep, tickSize);
    }

    vs.CountUnmatched();

    bool ok = (vs.counters.legacyFinalizedCount == 10) &&
              (vs.counters.amtFinalizedCount == 10) &&
              (vs.counters.matchedCount == 10) &&
              (vs.counters.missingLegacyCount == 0) &&
              (vs.counters.missingAmtCount == 0);

    if (ok) {
        PASS();
    } else {
        FAIL("Session counts incorrect");
    }
}

void test_session_with_mismatches() {
    TEST("Session with intentional mismatches");

    const double tickSize = 0.25;
    AMT::ValidationState vs;
    vs.StartSession(0);

    // Add 5 matching pairs
    for (int i = 0; i < 5; i++) {
        AMT::ValidationEpisode ep;
        ep.zoneType = AMT::ZoneType::VPB_POC;
        ep.anchorPrice = 5000.0;
        ep.entryBar = i * 100;
        ep.exitBar = i * 100 + 20;
        ep.barsEngaged = 20;
        ep.escapeVelocity = 2.0;
        ep.coreWidthTicks = 3;
        ep.haloWidthTicks = 5;

        vs.AddLegacyEpisode(ep, tickSize);
        vs.AddAmtEpisode(ep, tickSize);
    }

    // Add 2 unmatched legacy (no AMT equivalent)
    for (int i = 0; i < 2; i++) {
        AMT::ValidationEpisode ep;
        ep.zoneType = AMT::ZoneType::VPB_POC;
        ep.anchorPrice = 6000.0;  // Different anchor
        ep.entryBar = 1000 + i * 100;
        vs.AddLegacyEpisode(ep, tickSize);
    }

    // Add 1 unmatched AMT (no legacy equivalent)
    {
        AMT::ValidationEpisode ep;
        ep.zoneType = AMT::ZoneType::VPB_POC;
        ep.anchorPrice = 7000.0;  // Different anchor
        ep.entryBar = 2000;
        vs.AddAmtEpisode(ep, tickSize);
    }

    vs.CountUnmatched();

    bool ok = (vs.counters.matchedCount == 5) &&
              (vs.counters.missingAmtCount == 2) &&    // Legacy without AMT
              (vs.counters.missingLegacyCount == 1);   // AMT without legacy

    if (ok) {
        PASS();
    } else {
        std::cout << "matched=" << vs.counters.matchedCount
                  << " missingAmt=" << vs.counters.missingAmtCount
                  << " missingLeg=" << vs.counters.missingLegacyCount << " ";
        FAIL("Mismatch counts incorrect");
    }
}

// =============================================================================
// TEST: Reason String Coverage
// =============================================================================

void test_all_reason_strings() {
    TEST("All reason codes have non-empty strings");

    std::vector<AMT::ValidationMismatchReason> reasons = {
        AMT::ValidationMismatchReason::NONE,
        AMT::ValidationMismatchReason::ENTRY_BAR_DIFF,
        AMT::ValidationMismatchReason::EXIT_BAR_DIFF,
        AMT::ValidationMismatchReason::BARS_ENGAGED_DIFF,
        AMT::ValidationMismatchReason::ENTRY_PRICE_DIFF,
        AMT::ValidationMismatchReason::EXIT_PRICE_DIFF,
        AMT::ValidationMismatchReason::ESC_VEL_DIFF,
        AMT::ValidationMismatchReason::WIDTH_CORE_DIFF,
        AMT::ValidationMismatchReason::WIDTH_HALO_DIFF,
        AMT::ValidationMismatchReason::MISSING_LEGACY_EPISODE,
        AMT::ValidationMismatchReason::MISSING_AMT_EPISODE,
        AMT::ValidationMismatchReason::WIDTH_UNEXPECTED_CHANGE
    };

    bool allOk = true;
    for (auto reason : reasons) {
        const char* str = AMT::GetMismatchReasonString(reason);
        if (str == nullptr || str[0] == '\0') {
            allOk = false;
            break;
        }
    }

    if (allOk) {
        PASS();
    } else {
        FAIL("Some reason strings are empty");
    }
}

// =============================================================================
// TEST: Edge Cases
// =============================================================================

void test_zero_tick_size() {
    TEST("Zero tick size handled gracefully");

    AMT::ValidationEpisode ep;
    ep.anchorPrice = 5000.0;

    int ticks = ep.GetAnchorInTicks(0.0);

    if (ticks == 0) {
        PASS();
    } else {
        FAIL("Should return 0 for zero tick size");
    }
}

void test_negative_prices() {
    TEST("Negative prices handled (futures can have negative)");

    const double tickSize = 0.01;
    AMT::ValidationEpisode leg, amt;
    leg.isLegacy = true;
    leg.zoneType = AMT::ZoneType::VPB_POC;
    leg.anchorPrice = -50.0;  // Negative (like oil futures 2020)
    leg.entryBar = 100;
    leg.exitBar = 110;

    amt = leg;
    amt.isLegacy = false;

    if (leg.CouldMatch(amt, tickSize)) {
        PASS();
    } else {
        FAIL("Should match negative prices");
    }
}

void test_very_large_bar_numbers() {
    TEST("Large bar numbers (multi-year data)");

    AMT::ValidationState vs;
    AMT::ValidationEpisode leg, amt;
    leg.entryBar = 1000000;
    leg.exitBar = 1000100;
    leg.barsEngaged = 100;
    leg.escapeVelocity = 2.5;
    leg.coreWidthTicks = 3;
    leg.haloWidthTicks = 5;

    amt = leg;

    if (vs.CompareEpisodes(leg, amt) == AMT::ValidationMismatchReason::NONE) {
        PASS();
    } else {
        FAIL("Should handle large bar numbers");
    }
}

// =============================================================================
// MAIN
// =============================================================================

int main() {
    std::cout << "=== Comprehensive Validation System Tests ===" << std::endl;

    TEST_SECTION("Episode Matching Edge Cases");
    test_episode_matching_same_anchor();
    test_episode_matching_anchor_off_by_half_tick();
    test_episode_matching_anchor_off_by_one_tick();
    test_episode_matching_different_types();
    test_episode_matching_entry_within_tolerance();
    test_episode_matching_entry_outside_tolerance();
    test_episode_matching_no_overlap();

    TEST_SECTION("Comparison Logic");
    test_compare_exact_match();
    test_compare_entry_bar_diff();
    test_compare_bars_engaged_diff();
    test_compare_escape_vel_within_epsilon();
    test_compare_escape_vel_outside_epsilon();
    test_compare_width_core_diff();
    test_compare_width_halo_diff();

    TEST_SECTION("Auto-Matching");
    test_auto_matching_legacy_first();
    test_auto_matching_amt_first();
    test_no_double_matching();

    TEST_SECTION("Counters");
    test_counters_increment();
    test_counters_reset();

    TEST_SECTION("Ring Buffer");
    test_ring_buffer_eviction();

    TEST_SECTION("Session Management");
    test_session_start_clears_state();

    TEST_SECTION("Width Parity");
    test_width_parity_tracking();

    TEST_SECTION("Full Session Simulation");
    test_full_session_simulation();
    test_session_with_mismatches();

    TEST_SECTION("Reason Strings");
    test_all_reason_strings();

    TEST_SECTION("Edge Cases");
    test_zero_tick_size();
    test_negative_prices();
    test_very_large_bar_numbers();

    std::cout << "\n=== RESULTS ===" << std::endl;
    std::cout << "Passed: " << g_passed << "/" << (g_passed + g_failed) << std::endl;
    std::cout << "Failed: " << g_failed << std::endl;

    if (g_failed == 0) {
        std::cout << "\nALL TESTS PASSED!" << std::endl;
        return 0;
    } else {
        std::cout << "\nSOME TESTS FAILED!" << std::endl;
        return 1;
    }
}
