// test_validation.cpp
// Standalone test for Phase 3 validation logic
// Compile: g++ -std=c++17 -DVALIDATE_ZONE_MIGRATION -I.. -o test_validation.exe test_validation.cpp

#include <iostream>
#include <iomanip>
#include <cmath>
#include <cassert>

// Use shared Sierra Chart mock for standalone testing
#include "test_sierrachart_mock.h"

// Enable validation for this test
#define VALIDATE_ZONE_MIGRATION

#include "AMT_Zones.h"

// Test counters
int g_passed = 0;
int g_failed = 0;

#define TEST(name) std::cout << "  Testing " << name << "... "
#define PASS() do { std::cout << "[PASS]" << std::endl; g_passed++; } while(0)
#define FAIL(msg) do { std::cout << "[FAIL] " << msg << std::endl; g_failed++; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

void test_validation_episode_matching() {
    TEST("ValidationEpisode::CouldMatch");

    const double tickSize = 0.25;

    AMT::ValidationEpisode legacy;
    legacy.isLegacy = true;
    legacy.zoneType = AMT::ZoneType::VPB_POC;
    legacy.anchorPrice = 5000.0;
    legacy.entryBar = 100;
    legacy.exitBar = 110;

    AMT::ValidationEpisode amt;
    amt.isLegacy = false;
    amt.zoneType = AMT::ZoneType::VPB_POC;
    amt.anchorPrice = 5000.0;
    amt.entryBar = 100;
    amt.exitBar = 110;

    // Same source should not match
    AMT::ValidationEpisode legacy2 = legacy;
    CHECK(!legacy.CouldMatch(legacy2, tickSize), "Same source should not match");

    // Exact match should work
    CHECK(legacy.CouldMatch(amt, tickSize), "Exact match should work");

    // Entry bar within tolerance should match
    amt.entryBar = 101;  // 1 bar off
    CHECK(legacy.CouldMatch(amt, tickSize, 1), "Entry within tolerance should match");

    // Entry bar outside tolerance should not match
    amt.entryBar = 103;  // 3 bars off
    CHECK(!legacy.CouldMatch(amt, tickSize, 1), "Entry outside tolerance should not match");

    // Different zone type should not match
    amt.entryBar = 100;
    amt.zoneType = AMT::ZoneType::VPB_VAH;
    CHECK(!legacy.CouldMatch(amt, tickSize), "Different zone type should not match");

    // Different anchor should not match
    amt.zoneType = AMT::ZoneType::VPB_POC;
    amt.anchorPrice = 5010.0;  // 40 ticks away
    CHECK(!legacy.CouldMatch(amt, tickSize), "Different anchor should not match");

    PASS();
}

void test_validation_state_episode_buffers() {
    TEST("ValidationState episode buffers");

    const double tickSize = 0.25;
    AMT::ValidationState vs;
    vs.StartSession(0);

    CHECK(vs.legacyEpisodes.empty(), "Should start empty");
    CHECK(vs.amtEpisodes.empty(), "Should start empty");

    // Add legacy episode
    AMT::ValidationEpisode legEp;
    legEp.zoneId = 0;
    legEp.zoneType = AMT::ZoneType::VPB_POC;
    legEp.anchorPrice = 5000.0;
    legEp.entryBar = 100;
    legEp.exitBar = 110;
    legEp.barsEngaged = 11;
    legEp.entryPrice = 5000.0;
    legEp.exitPrice = 5005.0;
    legEp.escapeVelocity = 1.818;  // 20 ticks / 11 bars
    legEp.coreWidthTicks = 3;
    legEp.haloWidthTicks = 5;

    vs.AddLegacyEpisode(legEp, tickSize);
    CHECK(vs.legacyEpisodes.size() == 1, "Should have 1 legacy episode");
    CHECK(vs.counters.legacyFinalizedCount == 1, "Counter should increment");
    CHECK(!vs.legacyEpisodes[0].matched, "Should not be matched yet");

    // Add matching AMT episode
    AMT::ValidationEpisode amtEp = legEp;
    amtEp.isLegacy = false;

    vs.AddAmtEpisode(amtEp, tickSize);
    CHECK(vs.amtEpisodes.size() == 1, "Should have 1 AMT episode");
    CHECK(vs.counters.amtFinalizedCount == 1, "Counter should increment");
    CHECK(vs.counters.matchedCount == 1, "Should have matched");
    CHECK(vs.amtEpisodes[0].matched, "AMT episode should be marked matched");
    CHECK(vs.legacyEpisodes[0].matched, "Legacy episode should be marked matched");

    PASS();
}

void test_validation_compare_episodes() {
    TEST("ValidationState::CompareEpisodes");

    AMT::ValidationState vs;
    vs.StartSession(0);

    AMT::ValidationEpisode legacy;
    legacy.entryBar = 100;
    legacy.exitBar = 110;
    legacy.barsEngaged = 11;
    legacy.escapeVelocity = 2.0;
    legacy.coreWidthTicks = 3;
    legacy.haloWidthTicks = 5;

    AMT::ValidationEpisode amt = legacy;

    // Exact match
    CHECK(vs.CompareEpisodes(legacy, amt) == AMT::ValidationMismatchReason::NONE,
          "Exact match should return NONE");

    // Entry bar difference beyond tolerance
    amt.entryBar = 103;
    CHECK(vs.CompareEpisodes(legacy, amt) == AMT::ValidationMismatchReason::ENTRY_BAR_DIFF,
          "Entry bar diff should be detected");
    amt.entryBar = 100;

    // Exit bar difference beyond tolerance
    amt.exitBar = 115;
    CHECK(vs.CompareEpisodes(legacy, amt) == AMT::ValidationMismatchReason::EXIT_BAR_DIFF,
          "Exit bar diff should be detected");
    amt.exitBar = 110;

    // Bars engaged difference
    amt.barsEngaged = 15;
    CHECK(vs.CompareEpisodes(legacy, amt) == AMT::ValidationMismatchReason::BARS_ENGAGED_DIFF,
          "Bars engaged diff should be detected");
    amt.barsEngaged = 11;

    // Escape velocity difference
    amt.escapeVelocity = 3.0;
    CHECK(vs.CompareEpisodes(legacy, amt) == AMT::ValidationMismatchReason::ESC_VEL_DIFF,
          "Escape velocity diff should be detected");
    amt.escapeVelocity = 2.0;

    // Core width difference
    amt.coreWidthTicks = 4;
    CHECK(vs.CompareEpisodes(legacy, amt) == AMT::ValidationMismatchReason::WIDTH_CORE_DIFF,
          "Core width diff should be detected");
    amt.coreWidthTicks = 3;

    // Halo width difference
    amt.haloWidthTicks = 7;
    CHECK(vs.CompareEpisodes(legacy, amt) == AMT::ValidationMismatchReason::WIDTH_HALO_DIFF,
          "Halo width diff should be detected");

    PASS();
}

void test_validation_counters() {
    TEST("ValidationCounters increment");

    AMT::ValidationCounters vc;

    CHECK(vc.mismatchCount == 0, "Should start at 0");

    vc.IncrementForReason(AMT::ValidationMismatchReason::ENTRY_BAR_DIFF);
    CHECK(vc.entryBarDiffCount == 1, "Entry bar count should increment");

    vc.IncrementForReason(AMT::ValidationMismatchReason::ESC_VEL_DIFF);
    vc.IncrementForReason(AMT::ValidationMismatchReason::ESC_VEL_DIFF);
    CHECK(vc.escVelDiffCount == 2, "Escape vel count should be 2");

    vc.IncrementForReason(AMT::ValidationMismatchReason::WIDTH_CORE_DIFF);
    CHECK(vc.widthCoreDiffCount == 1, "Width core count should increment");

    vc.Reset();
    CHECK(vc.entryBarDiffCount == 0, "Should reset to 0");
    CHECK(vc.escVelDiffCount == 0, "Should reset to 0");

    PASS();
}

void test_width_parity_state() {
    TEST("WidthParityState tracking");

    AMT::WidthParityState ws;

    CHECK(ws.lastLegacyLiqTicks == -1, "Should start at -1");
    CHECK(ws.lastAmtCoreTicks == -1, "Should start at -1");

    ws.RecordLegacyUpdate(5, 100);
    CHECK(ws.lastLegacyLiqTicks == 5, "Legacy liq ticks should update");
    CHECK(ws.lastUpdateBar == 100, "Update bar should update");

    ws.RecordAmtUpdate(5, 8, 100);
    CHECK(ws.lastAmtCoreTicks == 5, "AMT core should update");
    CHECK(ws.lastAmtHaloTicks == 8, "AMT halo should update");

    ws.Reset();
    CHECK(ws.lastLegacyLiqTicks == -1, "Should reset");
    CHECK(ws.lastAmtCoreTicks == -1, "Should reset");

    PASS();
}

void test_mismatch_reason_strings() {
    TEST("GetMismatchReasonString");

    CHECK(std::string(AMT::GetMismatchReasonString(AMT::ValidationMismatchReason::NONE)) == "NONE",
          "NONE string");
    CHECK(std::string(AMT::GetMismatchReasonString(AMT::ValidationMismatchReason::ENTRY_BAR_DIFF)) == "ENTRY_BAR_DIFF",
          "ENTRY_BAR_DIFF string");
    CHECK(std::string(AMT::GetMismatchReasonString(AMT::ValidationMismatchReason::ESC_VEL_DIFF)) == "ESC_VEL_DIFF",
          "ESC_VEL_DIFF string");
    CHECK(std::string(AMT::GetMismatchReasonString(AMT::ValidationMismatchReason::WIDTH_UNEXPECTED_CHANGE)) == "WIDTH_UNEXPECTED_CHANGE",
          "WIDTH_UNEXPECTED_CHANGE string");

    PASS();
}

void test_count_unmatched() {
    TEST("ValidationState::CountUnmatched");

    const double tickSize = 0.25;
    AMT::ValidationState vs;
    vs.StartSession(0);

    // Add unmatched legacy episode
    AMT::ValidationEpisode legEp;
    legEp.zoneType = AMT::ZoneType::VPB_POC;
    legEp.anchorPrice = 5000.0;
    legEp.entryBar = 100;
    legEp.exitBar = 110;
    vs.AddLegacyEpisode(legEp, tickSize);

    // Add unmatched AMT episode (different anchor)
    AMT::ValidationEpisode amtEp;
    amtEp.zoneType = AMT::ZoneType::VPB_POC;
    amtEp.anchorPrice = 5100.0;  // Different anchor
    amtEp.entryBar = 200;
    amtEp.exitBar = 210;
    vs.AddAmtEpisode(amtEp, tickSize);

    CHECK(vs.counters.matchedCount == 0, "Should have 0 matches");
    CHECK(!vs.legacyEpisodes[0].matched, "Legacy should be unmatched");
    CHECK(!vs.amtEpisodes[0].matched, "AMT should be unmatched");

    vs.CountUnmatched();
    CHECK(vs.counters.missingAmtCount == 1, "Should have 1 missing AMT");
    CHECK(vs.counters.missingLegacyCount == 1, "Should have 1 missing legacy");

    PASS();
}

void test_ring_buffer_behavior() {
    TEST("Episode ring buffer (MAX_EPISODES)");

    const double tickSize = 0.25;
    AMT::ValidationState vs;
    vs.StartSession(0);

    // Add MAX_EPISODES + 10 episodes
    for (int i = 0; i < AMT::ValidationState::MAX_EPISODES + 10; i++) {
        AMT::ValidationEpisode ep;
        ep.zoneType = AMT::ZoneType::VPB_POC;
        ep.anchorPrice = 5000.0 + i;  // Different anchors
        ep.entryBar = i * 10;
        ep.exitBar = i * 10 + 5;
        vs.AddLegacyEpisode(ep, tickSize);
    }

    CHECK(vs.legacyEpisodes.size() == AMT::ValidationState::MAX_EPISODES,
          "Buffer should cap at MAX_EPISODES");

    // First episode should be at offset 10 (first 10 were evicted)
    CHECK(vs.legacyEpisodes[0].entryBar == 100,
          "First episode should have entryBar=100 (first 10 evicted)");

    PASS();
}

void test_full_validation_flow() {
    TEST("Full validation flow simulation");

    const double tickSize = 0.25;
    AMT::ValidationState vs;
    vs.StartSession(0);

    // Simulate a session with 3 engagements

    // Engagement 1: Perfect match
    {
        AMT::ValidationEpisode legEp;
        legEp.zoneType = AMT::ZoneType::VPB_POC;
        legEp.anchorPrice = 5000.0;
        legEp.entryBar = 100;
        legEp.exitBar = 110;
        legEp.barsEngaged = 11;
        legEp.escapeVelocity = 2.0;
        legEp.coreWidthTicks = 3;
        legEp.haloWidthTicks = 5;
        vs.AddLegacyEpisode(legEp, tickSize);

        AMT::ValidationEpisode amtEp = legEp;
        amtEp.isLegacy = false;
        vs.AddAmtEpisode(amtEp, tickSize);
    }

    // Engagement 2: Entry bar off by 1 (within tolerance)
    {
        AMT::ValidationEpisode legEp;
        legEp.zoneType = AMT::ZoneType::VPB_POC;
        legEp.anchorPrice = 5000.0;
        legEp.entryBar = 200;
        legEp.exitBar = 215;
        legEp.barsEngaged = 16;
        legEp.escapeVelocity = 1.5;
        legEp.coreWidthTicks = 4;
        legEp.haloWidthTicks = 6;
        vs.AddLegacyEpisode(legEp, tickSize);

        AMT::ValidationEpisode amtEp = legEp;
        amtEp.isLegacy = false;
        amtEp.entryBar = 201;  // 1 bar off (within tolerance)
        vs.AddAmtEpisode(amtEp, tickSize);
    }

    // Engagement 3: Width mismatch
    {
        AMT::ValidationEpisode legEp;
        legEp.zoneType = AMT::ZoneType::VPB_POC;
        legEp.anchorPrice = 5000.0;
        legEp.entryBar = 300;
        legEp.exitBar = 310;
        legEp.barsEngaged = 11;
        legEp.escapeVelocity = 2.5;
        legEp.coreWidthTicks = 5;
        legEp.haloWidthTicks = 8;
        vs.AddLegacyEpisode(legEp, tickSize);

        AMT::ValidationEpisode amtEp = legEp;
        amtEp.isLegacy = false;
        amtEp.coreWidthTicks = 6;  // Width mismatch
        vs.AddAmtEpisode(amtEp, tickSize);
    }

    CHECK(vs.counters.legacyFinalizedCount == 3, "Should have 3 legacy");
    CHECK(vs.counters.amtFinalizedCount == 3, "Should have 3 AMT");
    CHECK(vs.counters.matchedCount == 3, "All 3 should match");

    // Check for mismatches
    int mismatchCount = 0;
    for (size_t i = 0; i < vs.amtEpisodes.size(); i++) {
        const auto& amtEp = vs.amtEpisodes[i];
        const AMT::ValidationEpisode* legEp = vs.FindMatchingLegacy(amtEp, tickSize);
        if (legEp) {
            auto reason = vs.CompareEpisodes(*legEp, amtEp);
            if (reason != AMT::ValidationMismatchReason::NONE) {
                mismatchCount++;
                vs.counters.mismatchCount++;
                vs.counters.IncrementForReason(reason);
            }
        }
    }

    CHECK(mismatchCount == 1, "Should have 1 mismatch (width)");
    CHECK(vs.counters.widthCoreDiffCount == 1, "Should be core width diff");

    // Simulate summary output
    std::cout << std::endl;
    std::cout << "    [VAL-SUMMARY] legacyFin=" << vs.counters.legacyFinalizedCount
              << " amtFin=" << vs.counters.amtFinalizedCount
              << " matched=" << vs.counters.matchedCount
              << " mismatches=" << vs.counters.mismatchCount
              << " widthMismatch=" << vs.counters.widthMismatchCount << std::endl;

    if (vs.counters.mismatchCount > 0) {
        std::cout << "    [VAL-DETAIL] entryBar=" << vs.counters.entryBarDiffCount
                  << " exitBar=" << vs.counters.exitBarDiffCount
                  << " barsEngaged=" << vs.counters.barsEngagedDiffCount
                  << " escVel=" << vs.counters.escVelDiffCount
                  << " coreWidth=" << vs.counters.widthCoreDiffCount
                  << " haloWidth=" << vs.counters.widthHaloDiffCount << std::endl;
    }

    std::cout << "  ";
    PASS();
}

int main() {
    std::cout << "=== Phase 3 Validation Logic Tests ===" << std::endl;
    std::cout << "Testing episode matching, comparison, and logging infrastructure" << std::endl << std::endl;

    test_validation_episode_matching();
    test_validation_state_episode_buffers();
    test_validation_compare_episodes();
    test_validation_counters();
    test_width_parity_state();
    test_mismatch_reason_strings();
    test_count_unmatched();
    test_ring_buffer_behavior();
    test_full_validation_flow();

    std::cout << std::endl << "=== Results ===" << std::endl;
    std::cout << "Passed: " << g_passed << std::endl;
    std::cout << "Failed: " << g_failed << std::endl;

    if (g_failed == 0) {
        std::cout << std::endl << "All validation tests PASSED!" << std::endl;
        return 0;
    } else {
        std::cout << std::endl << "Some tests FAILED!" << std::endl;
        return 1;
    }
}
