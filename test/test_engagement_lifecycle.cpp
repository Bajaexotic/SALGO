// Define standalone test mode to exclude SC-dependent functions
#define AMT_STANDALONE_TEST

// ============================================================================
// test_engagement_lifecycle.cpp
// Unit tests for SSOT lifetime counters, engagement lifecycle, and coherence
// Tests: TouchType, UnresolvedReason, FinalizeEngagement, ForceFinalize,
//        SSOT counter invariants, ring buffer survival, coherence rules
// ============================================================================

#include "test_sierrachart_mock.h"
#include "AMT_Zones.h"
#include "AMT_Analytics.h"
#include "AMT_Session.h"  // For SessionEngagementAccumulator
#include <iostream>
#include <cassert>
#include <vector>
#include <cmath>

using namespace AMT;

// ============================================================================
// HELPER: Create a test zone with sensible defaults
// ============================================================================

ZoneRuntime CreateTestZone(int zoneId, double anchorPrice, int creationBar, int haloWidth = 8) {
    SCDateTime creationTime;
    creationTime.SetDateTime(2024, 1, 15, 9, 30, 0);

    return ZoneRuntime(
        zoneId,
        ZoneType::VPB_VAH,           // Using actual enum value
        ZoneRole::VALUE_BOUNDARY,    // Using actual enum value
        AnchorMechanism::VOLUME_PROFILE,
        ZoneSource::CURRENT_RTH,     // Using actual enum value
        anchorPrice,
        creationTime,
        creationBar,
        haloWidth
    );
}

ZoneConfig CreateTestConfig() {
    ZoneConfig cfg;
    cfg.baseCoreTicks = 3;        // Correct field name
    cfg.baseHaloTicks = 8;        // Correct field name
    cfg.acceptanceMinBars = 10;
    // Note: maxZoneAgeBars doesn't exist in ZoneConfig
    return cfg;
}

// ============================================================================
// TEST 1: ENUM TO_STRING HELPERS
// ============================================================================

void TestTouchTypeToString() {
    std::cout << "Testing TouchTypeToString()..." << std::endl;

    assert(std::string(TouchTypeToString(TouchType::TAG)) == "TAG");
    assert(std::string(TouchTypeToString(TouchType::PROBE)) == "PROBE");
    assert(std::string(TouchTypeToString(TouchType::TEST)) == "TEST");
    assert(std::string(TouchTypeToString(TouchType::ACCEPTANCE)) == "ACCEPTANCE");
    assert(std::string(TouchTypeToString(TouchType::UNRESOLVED)) == "UNRESOLVED");

    // Unknown value
    assert(std::string(TouchTypeToString(static_cast<TouchType>(99))) == "UNKNOWN");

    std::cout << "  All TouchType strings correct [PASS]" << std::endl;
}

void TestUnresolvedReasonToString() {
    std::cout << "\nTesting UnresolvedReasonToString()..." << std::endl;

    assert(std::string(UnresolvedReasonToString(UnresolvedReason::NONE)) == "NONE");
    assert(std::string(UnresolvedReasonToString(UnresolvedReason::SESSION_ROLL)) == "SESSION_ROLL");
    assert(std::string(UnresolvedReasonToString(UnresolvedReason::ZONE_EXPIRY)) == "ZONE_EXPIRY");
    assert(std::string(UnresolvedReasonToString(UnresolvedReason::CHART_RESET)) == "CHART_RESET");
    assert(std::string(UnresolvedReasonToString(UnresolvedReason::TIMEOUT)) == "TIMEOUT");

    // Unknown value
    assert(std::string(UnresolvedReasonToString(static_cast<UnresolvedReason>(99))) == "UNKNOWN");

    std::cout << "  All UnresolvedReason strings correct [PASS]" << std::endl;
}

// ============================================================================
// TEST 2: HAS_PENDING_ENGAGEMENT
// ============================================================================

void TestHasPendingEngagement() {
    std::cout << "\nTesting HasPendingEngagement()..." << std::endl;

    ZoneRuntime zone = CreateTestZone(1, 5000.0, 0);

    // Initially no pending engagement
    assert(zone.HasPendingEngagement() == false);
    std::cout << "  Initial state: no pending [PASS]" << std::endl;

    // Start engagement
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);
    zone.StartEngagement(100, time, 5000.25);

    assert(zone.HasPendingEngagement() == true);
    std::cout << "  After StartEngagement: pending [PASS]" << std::endl;

    // Finalize engagement
    ZoneConfig cfg = CreateTestConfig();
    zone.currentEngagement.barsEngaged = 3;  // Set bars for classification
    zone.currentEngagement.peakPenetrationTicks = 2;  // Within core = TAG
    zone.FinalizeEngagement(103, time, 5000.50, 0.25, cfg);

    assert(zone.HasPendingEngagement() == false);
    std::cout << "  After FinalizeEngagement: no pending [PASS]" << std::endl;
}

// ============================================================================
// TEST 3: START_ENGAGEMENT
// ============================================================================

void TestStartEngagement() {
    std::cout << "\nTesting StartEngagement()..." << std::endl;

    ZoneRuntime zone = CreateTestZone(1, 5000.0, 0);
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);

    assert(zone.touchCount == 0);
    assert(zone.lastTouchBar == -1);

    zone.StartEngagement(100, time, 5000.25);

    assert(zone.touchCount == 1);
    assert(zone.lastTouchBar == 100);
    assert(zone.lastInsideBar == 100);
    assert(zone.currentEngagement.startBar == 100);

    std::cout << "  touchCount incremented [PASS]" << std::endl;
    std::cout << "  lastTouchBar updated [PASS]" << std::endl;
    std::cout << "  engagement started [PASS]" << std::endl;
}

// ============================================================================
// TEST 4: FINALIZE_ENGAGEMENT - TAG CLASSIFICATION
// ============================================================================

void TestFinalizeEngagement_TAG() {
    std::cout << "\nTesting FinalizeEngagement() -> TAG..." << std::endl;

    ZoneRuntime zone = CreateTestZone(1, 5000.0, 0);
    ZoneConfig cfg = CreateTestConfig();
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);

    // Start engagement
    zone.StartEngagement(100, time, 5000.25);

    // Configure for TAG: brief contact (<=2 bars), within core
    // barsEngaged will be recalculated as endBar - startBar + 1
    // For 2 bars: startBar=100, endBar=101 -> 101-100+1 = 2
    zone.currentEngagement.peakPenetrationTicks = 2;  // <= coreWidthTicks(3)
    zone.currentEngagement.outcome = AuctionOutcome::PENDING;  // Will be forced to REJECTED

    auto finalized = zone.FinalizeEngagement(101, time, 5000.50, 0.25, cfg);  // 2 bars

    assert(finalized);  // FinalizationResult evaluates to true
    assert(zone.lifetimeTags == 1);
    assert(zone.lifetimeRejections == 0);  // TAGs don't count as rejections
    assert(zone.lifetimeAcceptances == 0);
    assert(zone.lifetimeUnresolved == 0);
    assert(zone.lastRejectionBar == -1);  // TAGs don't update recency

    // Check touch record
    assert(zone.touchHistory.size() == 1);
    assert(zone.touchHistory[0].type == TouchType::TAG);
    assert(zone.touchHistory[0].outcome == AuctionOutcome::REJECTED);

    std::cout << "  lifetimeTags incremented [PASS]" << std::endl;
    std::cout << "  lastRejectionBar NOT updated (noise) [PASS]" << std::endl;
    std::cout << "  TouchRecord coherent [PASS]" << std::endl;
}

// ============================================================================
// TEST 5: FINALIZE_ENGAGEMENT - PROBE CLASSIFICATION
// ============================================================================

void TestFinalizeEngagement_PROBE() {
    std::cout << "\nTesting FinalizeEngagement() -> PROBE..." << std::endl;

    ZoneRuntime zone = CreateTestZone(1, 5000.0, 0);
    ZoneConfig cfg = CreateTestConfig();
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);

    zone.StartEngagement(100, time, 5000.25);

    // Configure for PROBE: penetrated core, quick rejection (<=5 bars)
    // For 4 bars: endBar = startBar + 4 - 1 = 103
    zone.currentEngagement.peakPenetrationTicks = 5;  // > coreWidthTicks(3)
    zone.currentEngagement.outcome = AuctionOutcome::REJECTED;

    auto finalized = zone.FinalizeEngagement(103, time, 5000.50, 0.25, cfg);  // 4 bars

    assert(finalized);  // FinalizationResult evaluates to true
    assert(zone.lifetimeProbes == 1);
    assert(zone.lifetimeRejections == 1);  // Probes count as rejections
    assert(zone.lifetimeTags == 0);
    assert(zone.lastRejectionBar == 103);  // Probes update recency

    // Check touch record
    assert(zone.touchHistory.size() == 1);
    assert(zone.touchHistory[0].type == TouchType::PROBE);
    assert(zone.touchHistory[0].outcome == AuctionOutcome::REJECTED);

    std::cout << "  lifetimeProbes incremented [PASS]" << std::endl;
    std::cout << "  lifetimeRejections incremented [PASS]" << std::endl;
    std::cout << "  lastRejectionBar updated [PASS]" << std::endl;
}

// ============================================================================
// TEST 6: FINALIZE_ENGAGEMENT - TEST CLASSIFICATION
// ============================================================================

void TestFinalizeEngagement_TEST() {
    std::cout << "\nTesting FinalizeEngagement() -> TEST..." << std::endl;

    ZoneRuntime zone = CreateTestZone(1, 5000.0, 0);
    ZoneConfig cfg = CreateTestConfig();
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);

    zone.StartEngagement(100, time, 5000.25);

    // Configure for TEST: sustained engagement (>5 bars, <acceptanceMinBars*2)
    // For 12 bars: endBar = startBar + 12 - 1 = 111
    zone.currentEngagement.peakPenetrationTicks = 6;
    zone.currentEngagement.outcome = AuctionOutcome::REJECTED;

    auto finalized = zone.FinalizeEngagement(111, time, 5000.50, 0.25, cfg);

    assert(finalized);  // FinalizationResult evaluates to true
    assert(zone.lifetimeTests == 1);
    assert(zone.lifetimeRejections == 1);  // Tests count as rejections
    assert(zone.lifetimeProbes == 0);
    assert(zone.lastRejectionBar == 111);  // Tests update recency

    // Check touch record
    assert(zone.touchHistory.size() == 1);
    assert(zone.touchHistory[0].type == TouchType::TEST);
    assert(zone.touchHistory[0].outcome == AuctionOutcome::REJECTED);

    std::cout << "  lifetimeTests incremented [PASS]" << std::endl;
    std::cout << "  lifetimeRejections incremented [PASS]" << std::endl;
    std::cout << "  lastRejectionBar updated [PASS]" << std::endl;
}

// ============================================================================
// TEST 7: FINALIZE_ENGAGEMENT - ACCEPTANCE CLASSIFICATION
// ============================================================================

void TestFinalizeEngagement_ACCEPTANCE() {
    std::cout << "\nTesting FinalizeEngagement() -> ACCEPTANCE..." << std::endl;

    ZoneRuntime zone = CreateTestZone(1, 5000.0, 0);
    ZoneConfig cfg = CreateTestConfig();
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);

    zone.StartEngagement(100, time, 5000.25);

    // Configure for ACCEPTANCE: met acceptance criteria
    // For 15 bars: endBar = startBar + 15 - 1 = 114
    zone.currentEngagement.peakPenetrationTicks = 10;
    zone.currentEngagement.outcome = AuctionOutcome::ACCEPTED;

    auto finalized = zone.FinalizeEngagement(114, time, 5000.50, 0.25, cfg);

    assert(finalized);  // FinalizationResult evaluates to true
    assert(zone.lifetimeAcceptances == 1);
    assert(zone.lifetimeRejections == 0);
    assert(zone.lifetimeTags == 0);
    assert(zone.lastAcceptanceBar == 114);
    assert(zone.lastRejectionBar == -1);  // Not a rejection

    // Check touch record
    assert(zone.touchHistory.size() == 1);
    assert(zone.touchHistory[0].type == TouchType::ACCEPTANCE);
    assert(zone.touchHistory[0].outcome == AuctionOutcome::ACCEPTED);

    std::cout << "  lifetimeAcceptances incremented [PASS]" << std::endl;
    std::cout << "  lastAcceptanceBar updated [PASS]" << std::endl;
    std::cout << "  TouchRecord coherent [PASS]" << std::endl;
}

// ============================================================================
// TEST 8: FORCE_FINALIZE
// ============================================================================

void TestForceFinalize() {
    std::cout << "\nTesting ForceFinalize()..." << std::endl;

    ZoneRuntime zone = CreateTestZone(1, 5000.0, 0);
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);

    // Start engagement
    zone.StartEngagement(100, time, 5000.25);
    zone.currentEngagement.barsEngaged = 5;
    zone.currentEngagement.peakPenetrationTicks = 4;

    // Force-finalize due to session roll
    auto finalized = zone.ForceFinalize(105, time, UnresolvedReason::SESSION_ROLL);

    assert(finalized);  // FinalizationResult has operator bool()
    assert(zone.lifetimeUnresolved == 1);
    assert(zone.lifetimeAcceptances == 0);
    assert(zone.lifetimeRejections == 0);
    assert(zone.lifetimeTags == 0);
    assert(zone.HasPendingEngagement() == false);

    // Check touch record
    assert(zone.touchHistory.size() == 1);
    assert(zone.touchHistory[0].type == TouchType::UNRESOLVED);
    assert(zone.touchHistory[0].outcome == AuctionOutcome::PENDING);
    assert(zone.touchHistory[0].unresolvedReason == UnresolvedReason::SESSION_ROLL);

    // Recency trackers should NOT be updated
    assert(zone.lastRejectionBar == -1);
    assert(zone.lastAcceptanceBar == -1);

    std::cout << "  lifetimeUnresolved incremented [PASS]" << std::endl;
    std::cout << "  engagement cleared [PASS]" << std::endl;
    std::cout << "  TouchRecord has UNRESOLVED type [PASS]" << std::endl;
    std::cout << "  unresolvedReason set correctly [PASS]" << std::endl;
    std::cout << "  recency trackers NOT updated [PASS]" << std::endl;
}

// ============================================================================
// TEST 9: FORCE_FINALIZE WITH DIFFERENT REASONS
// ============================================================================

void TestForceFinalize_AllReasons() {
    std::cout << "\nTesting ForceFinalize() with all reasons..." << std::endl;

    UnresolvedReason reasons[] = {
        UnresolvedReason::SESSION_ROLL,
        UnresolvedReason::ZONE_EXPIRY,
        UnresolvedReason::CHART_RESET,
        UnresolvedReason::TIMEOUT
    };

    for (UnresolvedReason reason : reasons) {
        ZoneRuntime zone = CreateTestZone(1, 5000.0, 0);
        SCDateTime time;
        time.SetDateTime(2024, 1, 15, 10, 0, 0);

        zone.StartEngagement(100, time, 5000.25);
        zone.ForceFinalize(105, time, reason);

        assert(zone.touchHistory.size() == 1);
        assert(zone.touchHistory[0].unresolvedReason == reason);

        std::cout << "  " << UnresolvedReasonToString(reason) << " recorded correctly [PASS]" << std::endl;
    }
}

// ============================================================================
// TEST 10: EDGE CASE - FINALIZE WITHOUT PENDING ENGAGEMENT
// ============================================================================

void TestFinalizeWithoutPendingEngagement() {
    std::cout << "\nTesting FinalizeEngagement() without pending engagement..." << std::endl;

    ZoneRuntime zone = CreateTestZone(1, 5000.0, 0);
    ZoneConfig cfg = CreateTestConfig();
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);

    // No StartEngagement called - should return false
    auto finalized = zone.FinalizeEngagement(100, time, 5000.50, 0.25, cfg);

    assert(!finalized);  // FinalizationResult::None() evaluates to false
    assert(zone.touchHistory.size() == 0);
    assert(zone.lifetimeTags == 0);
    assert(zone.lifetimeRejections == 0);

    std::cout << "  Returns false, no side effects [PASS]" << std::endl;
}

// ============================================================================
// TEST 11: EDGE CASE - FORCE_FINALIZE WITHOUT PENDING ENGAGEMENT
// ============================================================================

void TestForceFinalize_NoPendingEngagement() {
    std::cout << "\nTesting ForceFinalize() without pending engagement..." << std::endl;

    ZoneRuntime zone = CreateTestZone(1, 5000.0, 0);
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);

    // No StartEngagement called - should return None (evaluates to false)
    auto finalized = zone.ForceFinalize(100, time, UnresolvedReason::SESSION_ROLL);

    assert(!finalized);  // FinalizationResult::None() evaluates to false
    assert(zone.touchHistory.size() == 0);
    assert(zone.lifetimeUnresolved == 0);

    std::cout << "  Returns false, no side effects [PASS]" << std::endl;
}

// ============================================================================
// TEST 12: EDGE CASE - DOUBLE FINALIZE (EXACTLY-ONCE GUARD)
// ============================================================================

void TestDoubleFinalizeGuard() {
    std::cout << "\nTesting double finalize guard..." << std::endl;

    ZoneRuntime zone = CreateTestZone(1, 5000.0, 0);
    ZoneConfig cfg = CreateTestConfig();
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);

    zone.StartEngagement(100, time, 5000.25);
    // For TAG: barsEngaged <= 2, penetration <= coreWidth
    zone.currentEngagement.peakPenetrationTicks = 2;

    // First finalize should succeed
    auto first = zone.FinalizeEngagement(101, time, 5000.50, 0.25, cfg);
    assert(first);  // FinalizationResult evaluates to true
    assert(zone.touchHistory.size() == 1);

    // Second finalize should fail (already finalized)
    auto second = zone.FinalizeEngagement(104, time, 5000.50, 0.25, cfg);
    assert(!second);  // FinalizationResult::None() evaluates to false
    assert(zone.touchHistory.size() == 1);  // No new record
    assert(zone.lifetimeTags == 1);  // Counter not incremented again

    std::cout << "  First finalize succeeds [PASS]" << std::endl;
    std::cout << "  Second finalize blocked [PASS]" << std::endl;
    std::cout << "  Counters not double-incremented [PASS]" << std::endl;
}

// ============================================================================
// TEST 13: SSOT COUNTER INVARIANT
// ============================================================================

void TestSSOTCounterInvariant() {
    std::cout << "\nTesting SSOT counter invariant..." << std::endl;

    ZoneRuntime zone = CreateTestZone(1, 5000.0, 0);
    ZoneConfig cfg = CreateTestConfig();
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);

    // Generate various touch types
    // Touch 1: TAG
    zone.StartEngagement(100, time, 5000.25);
    // For TAG: 2 bars, within core
    zone.currentEngagement.peakPenetrationTicks = 2;
    zone.FinalizeEngagement(101, time, 5000.50, 0.25, cfg);

    // Touch 2: PROBE
    zone.StartEngagement(110, time, 5000.25);
    // For PROBE: 5 bars (110 to 114)
    zone.currentEngagement.peakPenetrationTicks = 5;
    zone.currentEngagement.outcome = AuctionOutcome::REJECTED;
    zone.FinalizeEngagement(114, time, 5000.50, 0.25, cfg);

    // Touch 3: TEST
    zone.StartEngagement(120, time, 5000.25);
    // For 12 bars: endBar = startBar + 12 - 1 = 111
    zone.currentEngagement.peakPenetrationTicks = 6;
    zone.currentEngagement.outcome = AuctionOutcome::REJECTED;
    zone.FinalizeEngagement(132, time, 5000.50, 0.25, cfg);

    // Touch 4: ACCEPTANCE
    zone.StartEngagement(140, time, 5000.25);
    // For 15 bars: endBar = startBar + 15 - 1 = 114
    zone.currentEngagement.peakPenetrationTicks = 10;
    zone.currentEngagement.outcome = AuctionOutcome::ACCEPTED;
    zone.FinalizeEngagement(155, time, 5000.50, 0.25, cfg);

    // Touch 5: Start but force-finalize (UNRESOLVED)
    zone.StartEngagement(160, time, 5000.25);
    zone.ForceFinalize(165, time, UnresolvedReason::SESSION_ROLL);

    // Verify invariant: touchCount == sum of all outcome counters
    int pending = zone.HasPendingEngagement() ? 1 : 0;
    int expectedSum = zone.lifetimeAcceptances + zone.lifetimeRejections +
                      zone.lifetimeTags + zone.lifetimeUnresolved + pending;

    assert(zone.touchCount == 5);
    assert(zone.touchCount == expectedSum);

    // Verify rejection subtype invariant
    assert(zone.lifetimeRejections == zone.lifetimeProbes + zone.lifetimeTests + zone.lifetimeRejectionsOther);

    std::cout << "  touchCount == 5 [PASS]" << std::endl;
    std::cout << "  touchCount == sum of outcome counters [PASS]" << std::endl;
    std::cout << "  rejection subtype invariant holds [PASS]" << std::endl;

    // Verify individual counts
    assert(zone.lifetimeTags == 1);
    assert(zone.lifetimeProbes == 1);
    assert(zone.lifetimeTests == 1);
    assert(zone.lifetimeAcceptances == 1);
    assert(zone.lifetimeUnresolved == 1);

    std::cout << "  Individual counters correct [PASS]" << std::endl;
}

// ============================================================================
// TEST 14: RING BUFFER SURVIVAL - COUNTERS PERSIST AFTER TRUNCATION
// ============================================================================

void TestRingBufferSurvival() {
    std::cout << "\nTesting ring buffer survival (SSOT counters persist)..." << std::endl;

    ZoneRuntime zone = CreateTestZone(1, 5000.0, 0);
    ZoneConfig cfg = CreateTestConfig();
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);

    // Generate more touches than MAX_TOUCH_HISTORY (50)
    int totalTouches = 60;
    for (int i = 0; i < totalTouches; i++) {
        zone.StartEngagement(100 + i * 10, time, 5000.25);
        // For TAG: 2 bars
        zone.currentEngagement.peakPenetrationTicks = 2;
        zone.FinalizeEngagement(101 + i * 10, time, 5000.50, 0.25, cfg);
    }

    // Ring buffer should be at MAX size
    assert(zone.touchHistory.size() == MAX_TOUCH_HISTORY);

    // But SSOT counter should have full count!
    assert(zone.lifetimeTags == totalTouches);
    assert(zone.touchCount == totalTouches);

    std::cout << "  touchHistory capped at " << MAX_TOUCH_HISTORY << " [PASS]" << std::endl;
    std::cout << "  lifetimeTags == " << totalTouches << " (survived truncation) [PASS]" << std::endl;
    std::cout << "  touchCount == " << totalTouches << " (survived truncation) [PASS]" << std::endl;
}

// ============================================================================
// TEST 15: MULTIPLE ENGAGEMENTS WITH MIXED OUTCOMES
// ============================================================================

void TestMixedOutcomeSequence() {
    std::cout << "\nTesting mixed outcome sequence..." << std::endl;

    ZoneRuntime zone = CreateTestZone(1, 5000.0, 0);
    ZoneConfig cfg = CreateTestConfig();
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);

    // Sequence: TAG, PROBE, PROBE, TEST, ACCEPTANCE, TAG, UNRESOLVED

    // TAG
    zone.StartEngagement(100, time, 5000.25);
    zone.currentEngagement.barsEngaged = 1;
    zone.currentEngagement.peakPenetrationTicks = 1;
    zone.FinalizeEngagement(101, time, 5000.50, 0.25, cfg);

    // PROBE 1
    zone.StartEngagement(110, time, 5000.25);
    zone.currentEngagement.barsEngaged = 3;
    zone.currentEngagement.peakPenetrationTicks = 5;
    zone.currentEngagement.outcome = AuctionOutcome::REJECTED;
    zone.FinalizeEngagement(113, time, 5000.50, 0.25, cfg);

    // PROBE 2
    zone.StartEngagement(120, time, 5000.25);
    zone.currentEngagement.barsEngaged = 4;
    zone.currentEngagement.peakPenetrationTicks = 6;
    zone.currentEngagement.outcome = AuctionOutcome::REJECTED;
    zone.FinalizeEngagement(124, time, 5000.50, 0.25, cfg);

    // TEST
    zone.StartEngagement(130, time, 5000.25);
    zone.currentEngagement.barsEngaged = 8;
    zone.currentEngagement.peakPenetrationTicks = 7;
    zone.currentEngagement.outcome = AuctionOutcome::REJECTED;
    zone.FinalizeEngagement(138, time, 5000.50, 0.25, cfg);

    // ACCEPTANCE
    zone.StartEngagement(140, time, 5000.25);
    // For 12 bars: endBar = startBar + 12 - 1 = 111
    zone.currentEngagement.peakPenetrationTicks = 8;
    zone.currentEngagement.outcome = AuctionOutcome::ACCEPTED;
    zone.FinalizeEngagement(152, time, 5000.50, 0.25, cfg);

    // TAG
    zone.StartEngagement(160, time, 5000.25);
    // For TAG: 2 bars
    zone.currentEngagement.peakPenetrationTicks = 2;
    zone.FinalizeEngagement(161, time, 5000.50, 0.25, cfg);

    // UNRESOLVED (session roll)
    zone.StartEngagement(170, time, 5000.25);
    zone.ForceFinalize(175, time, UnresolvedReason::SESSION_ROLL);

    // Verify counts
    assert(zone.lifetimeTags == 2);
    assert(zone.lifetimeProbes == 2);
    assert(zone.lifetimeTests == 1);
    assert(zone.lifetimeAcceptances == 1);
    assert(zone.lifetimeUnresolved == 1);
    assert(zone.lifetimeRejections == 3);  // 2 probes + 1 test
    assert(zone.touchCount == 7);

    std::cout << "  lifetimeTags == 2 [PASS]" << std::endl;
    std::cout << "  lifetimeProbes == 2 [PASS]" << std::endl;
    std::cout << "  lifetimeTests == 1 [PASS]" << std::endl;
    std::cout << "  lifetimeAcceptances == 1 [PASS]" << std::endl;
    std::cout << "  lifetimeUnresolved == 1 [PASS]" << std::endl;
    std::cout << "  lifetimeRejections == 3 [PASS]" << std::endl;
    std::cout << "  touchCount == 7 [PASS]" << std::endl;
}

// ============================================================================
// TEST 16: COHERENCE - TouchType ↔ AuctionOutcome MAPPING
// ============================================================================

void TestCoherenceRules() {
    std::cout << "\nTesting coherence rules..." << std::endl;

    ZoneRuntime zone = CreateTestZone(1, 5000.0, 0);
    ZoneConfig cfg = CreateTestConfig();
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);

    // Test TAG -> REJECTED
    zone.StartEngagement(100, time, 5000.25);
    // For TAG: 2 bars
    zone.currentEngagement.peakPenetrationTicks = 2;
    zone.FinalizeEngagement(101, time, 5000.50, 0.25, cfg);
    assert(zone.touchHistory.back().type == TouchType::TAG);
    assert(zone.touchHistory.back().outcome == AuctionOutcome::REJECTED);
    std::cout << "  TAG -> REJECTED [PASS]" << std::endl;

    // Test PROBE -> REJECTED
    zone.StartEngagement(110, time, 5000.25);
    zone.currentEngagement.barsEngaged = 4;
    zone.currentEngagement.peakPenetrationTicks = 5;
    zone.currentEngagement.outcome = AuctionOutcome::REJECTED;
    zone.FinalizeEngagement(114, time, 5000.50, 0.25, cfg);
    assert(zone.touchHistory.back().type == TouchType::PROBE);
    assert(zone.touchHistory.back().outcome == AuctionOutcome::REJECTED);
    std::cout << "  PROBE -> REJECTED [PASS]" << std::endl;

    // Test TEST -> REJECTED
    zone.StartEngagement(120, time, 5000.25);
    // For 12 bars: endBar = startBar + 12 - 1 = 111
    zone.currentEngagement.peakPenetrationTicks = 6;
    zone.currentEngagement.outcome = AuctionOutcome::REJECTED;
    zone.FinalizeEngagement(132, time, 5000.50, 0.25, cfg);
    assert(zone.touchHistory.back().type == TouchType::TEST);
    assert(zone.touchHistory.back().outcome == AuctionOutcome::REJECTED);
    std::cout << "  TEST -> REJECTED [PASS]" << std::endl;

    // Test ACCEPTANCE -> ACCEPTED
    zone.StartEngagement(140, time, 5000.25);
    // For 15 bars: endBar = startBar + 15 - 1 = 114
    zone.currentEngagement.peakPenetrationTicks = 10;
    zone.currentEngagement.outcome = AuctionOutcome::ACCEPTED;
    zone.FinalizeEngagement(155, time, 5000.50, 0.25, cfg);
    assert(zone.touchHistory.back().type == TouchType::ACCEPTANCE);
    assert(zone.touchHistory.back().outcome == AuctionOutcome::ACCEPTED);
    std::cout << "  ACCEPTANCE -> ACCEPTED [PASS]" << std::endl;

    // Test UNRESOLVED -> PENDING
    zone.StartEngagement(160, time, 5000.25);
    zone.ForceFinalize(165, time, UnresolvedReason::SESSION_ROLL);
    assert(zone.touchHistory.back().type == TouchType::UNRESOLVED);
    assert(zone.touchHistory.back().outcome == AuctionOutcome::PENDING);
    std::cout << "  UNRESOLVED -> PENDING [PASS]" << std::endl;
}

// ============================================================================
// TEST 17: RECENCY TRACKER ISOLATION (TAGs don't pollute)
// ============================================================================

void TestRecencyTrackerIsolation() {
    std::cout << "\nTesting recency tracker isolation..." << std::endl;

    ZoneRuntime zone = CreateTestZone(1, 5000.0, 0);
    ZoneConfig cfg = CreateTestConfig();
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);

    // First: A real PROBE rejection at bar 100
    zone.StartEngagement(100, time, 5000.25);
    zone.currentEngagement.barsEngaged = 4;
    zone.currentEngagement.peakPenetrationTicks = 5;
    zone.currentEngagement.outcome = AuctionOutcome::REJECTED;
    zone.FinalizeEngagement(104, time, 5000.50, 0.25, cfg);

    assert(zone.lastRejectionBar == 104);

    // Now: Multiple TAGs that should NOT update lastRejectionBar
    for (int i = 0; i < 5; i++) {
        zone.StartEngagement(200 + i * 10, time, 5000.25);
        // For TAG: 2 bars
        zone.currentEngagement.peakPenetrationTicks = 2;
        zone.FinalizeEngagement(201 + i * 10, time, 5000.50, 0.25, cfg);
    }

    // lastRejectionBar should still be 104 (not polluted by TAGs)
    assert(zone.lastRejectionBar == 104);
    std::cout << "  lastRejectionBar preserved through TAGs [PASS]" << std::endl;

    // Now a real TEST rejection at bar 300
    zone.StartEngagement(300, time, 5000.25);
    // For 12 bars: endBar = startBar + 12 - 1 = 111
    zone.currentEngagement.peakPenetrationTicks = 6;
    zone.currentEngagement.outcome = AuctionOutcome::REJECTED;
    zone.FinalizeEngagement(312, time, 5000.50, 0.25, cfg);

    assert(zone.lastRejectionBar == 312);
    std::cout << "  lastRejectionBar updated by TEST [PASS]" << std::endl;
}

// ============================================================================
// TEST 18: HALO WIDTH AT CREATION
// ============================================================================

void TestHaloWidthAtCreation() {
    std::cout << "\nTesting haloWidthTicks at creation..." << std::endl;

    // Create zones with different halo widths
    ZoneRuntime zone1 = CreateTestZone(1, 5000.0, 0, 8);
    ZoneRuntime zone2 = CreateTestZone(2, 5000.0, 0, 12);
    ZoneRuntime zone3 = CreateTestZone(3, 5000.0, 0, 5);

    assert(zone1.creationHaloWidthTicks == 8);
    assert(zone2.creationHaloWidthTicks == 12);
    assert(zone3.creationHaloWidthTicks == 5);

    std::cout << "  creationHaloWidthTicks set correctly [PASS]" << std::endl;
}

// ============================================================================
// TEST 19: SESSION STATISTICS AGGREGATION
// ============================================================================

void TestSessionStatisticsAggregation() {
    std::cout << "\nTesting SessionStatistics aggregation from engagement accumulator..." << std::endl;

    ZoneManager zm;
    zm.config = CreateTestConfig();
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);

    // Create engagement accumulator (SSOT for stats)
    SessionEngagementAccumulator accum;

    // Wire callback to accumulator
    zm.onEngagementFinalized = [&accum](
        const ZoneRuntime& zone,
        const FinalizationResult& result)
    {
        accum.RecordEngagement(zone.type, result.touchRecord.type);
    };

    // Create VAH zone with touches (using simple CreateZone signature)
    auto vahResult = zm.CreateZone(
        ZoneType::VPB_VAH,  // Zone type determines role
        5100.0, time, 0, true  // anchor, time, bar, isRTH
    );
    assert(vahResult.ok);
    zm.vahId = vahResult.zoneId;  // Set anchor for CalculateSessionStats

    ZoneRuntime* vah = zm.GetZone(vahResult.zoneId);
    assert(vah != nullptr);

    // Add various touches to VAH
    // NOTE: Since we're calling FinalizeEngagement directly, we need to invoke
    // the callback manually with the result to populate the accumulator
    vah->StartEngagement(100, time, 5100.25);
    vah->currentEngagement.peakPenetrationTicks = 10;
    vah->currentEngagement.outcome = AuctionOutcome::ACCEPTED;
    auto result1 = vah->FinalizeEngagement(114, time, 5100.50, 0.25, zm.config);
    if (result1 && zm.onEngagementFinalized) zm.onEngagementFinalized(*vah, result1);

    vah->StartEngagement(120, time, 5100.25);
    vah->currentEngagement.peakPenetrationTicks = 5;
    vah->currentEngagement.outcome = AuctionOutcome::REJECTED;
    auto result2 = vah->FinalizeEngagement(124, time, 5100.50, 0.25, zm.config);
    if (result2 && zm.onEngagementFinalized) zm.onEngagementFinalized(*vah, result2);

    vah->StartEngagement(130, time, 5100.25);
    vah->currentEngagement.peakPenetrationTicks = 2;
    auto result3 = vah->FinalizeEngagement(131, time, 5100.50, 0.25, zm.config);
    if (result3 && zm.onEngagementFinalized) zm.onEngagementFinalized(*vah, result3);

    // Create VAL zone with touches (using simple CreateZone signature)
    auto valResult = zm.CreateZone(
        ZoneType::VPB_VAL,  // Zone type determines role
        4900.0, time, 0, true  // anchor, time, bar, isRTH
    );
    assert(valResult.ok);
    zm.valId = valResult.zoneId;  // Set anchor for CalculateSessionStats

    ZoneRuntime* val = zm.GetZone(valResult.zoneId);
    assert(val != nullptr);

    // Add touches to VAL
    val->StartEngagement(140, time, 4900.25);
    val->currentEngagement.barsEngaged = 12;
    val->currentEngagement.peakPenetrationTicks = 6;
    val->currentEngagement.outcome = AuctionOutcome::REJECTED;
    auto result4 = val->FinalizeEngagement(152, time, 4900.50, 0.25, zm.config);
    if (result4 && zm.onEngagementFinalized) zm.onEngagementFinalized(*val, result4);

    val->StartEngagement(160, time, 4900.25);
    auto result5 = val->ForceFinalize(165, time, UnresolvedReason::SESSION_ROLL);
    if (result5 && zm.onEngagementFinalized) zm.onEngagementFinalized(*val, result5);

    // Calculate stats from accumulator (SSOT)
    std::vector<CurrentPhase> history;
    SessionStatistics stats = CalculateSessionStats(zm, accum, CurrentPhase::ROTATION, 200, history);

    // Verify VAH stats (1 acceptance, 1 probe rejection, 1 tag)
    assert(stats.vahTouches == 3);
    assert(stats.vahAcceptances == 1);
    assert(stats.vahRejections == 1);  // Only PROBE counts, not TAG
    assert(stats.vahTags == 1);
    assert(stats.vahProbeRejections == 1);
    assert(stats.vahTestRejections == 0);
    std::cout << "  VAH stats aggregated correctly [PASS]" << std::endl;

    // Verify VAL stats (1 test rejection, 1 unresolved)
    assert(stats.valTouches == 2);
    assert(stats.valAcceptances == 0);
    assert(stats.valRejections == 1);  // TEST counts as rejection
    assert(stats.valTestRejections == 1);
    assert(stats.valUnresolved == 1);
    std::cout << "  VAL stats aggregated correctly [PASS]" << std::endl;

    // Verify totals
    assert(stats.totalAcceptances == 1);
    assert(stats.totalRejections == 2);  // 1 VAH probe + 1 VAL test
    assert(stats.totalTags == 1);
    assert(stats.totalUnresolved == 1);
    std::cout << "  Total stats aggregated correctly [PASS]" << std::endl;

    // Verify acceptance rates
    // VAH: 1 acceptance, 1 rejection, 1 tag -> rate of attempts = 1/3, rate of decisions = 1/2
    double expectedVahRateOfAttempts = 1.0 / 3.0;
    double expectedVahRateOfDecisions = 1.0 / 2.0;
    assert(std::abs(stats.vahAcceptanceRateOfAttempts - expectedVahRateOfAttempts) < 0.001);
    assert(std::abs(stats.vahAcceptanceRateOfDecisions - expectedVahRateOfDecisions) < 0.001);
    std::cout << "  VAH acceptance rates calculated correctly [PASS]" << std::endl;
}

// ============================================================================
// TEST 20: EDGE CASE - PENDING ENGAGEMENT DURING INVARIANT CHECK
// ============================================================================

void TestPendingEngagementInvariant() {
    std::cout << "\nTesting invariant with pending engagement..." << std::endl;

    ZoneRuntime zone = CreateTestZone(1, 5000.0, 0);
    ZoneConfig cfg = CreateTestConfig();
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);

    // Complete some engagements
    zone.StartEngagement(100, time, 5000.25);
    // For TAG: 2 bars
    zone.currentEngagement.peakPenetrationTicks = 2;
    zone.FinalizeEngagement(102, time, 5000.50, 0.25, cfg);  // TAG

    zone.StartEngagement(110, time, 5000.25);
    zone.currentEngagement.barsEngaged = 4;
    zone.currentEngagement.peakPenetrationTicks = 5;
    zone.currentEngagement.outcome = AuctionOutcome::REJECTED;
    zone.FinalizeEngagement(114, time, 5000.50, 0.25, cfg);  // PROBE

    // Start a new engagement but DON'T finalize
    zone.StartEngagement(120, time, 5000.25);

    // Verify invariant holds even with pending engagement
    int pending = zone.HasPendingEngagement() ? 1 : 0;
    int expectedSum = zone.lifetimeAcceptances + zone.lifetimeRejections +
                      zone.lifetimeTags + zone.lifetimeUnresolved + pending;

    assert(pending == 1);
    assert(zone.touchCount == 3);  // 2 finalized + 1 pending
    assert(zone.touchCount == expectedSum);

    std::cout << "  Pending engagement counted in invariant [PASS]" << std::endl;
    std::cout << "  touchCount == " << zone.touchCount << " [PASS]" << std::endl;
    std::cout << "  Invariant holds with pending [PASS]" << std::endl;
}

// ============================================================================
// TEST 20: BACKFILL STABILITY
// Two identical full recalcs must produce identical session stats.
// This validates that stats survive zone clearing because they live in the
// accumulator, not in zone objects.
// ============================================================================

void TestBackfillStability() {
    std::cout << "\nTesting backfill stability (stats survive zone clearing)..." << std::endl;

    // Helper to simulate a processing run
    auto simulateRun = [](ZoneManager& zm, SessionEngagementAccumulator& accum) {
        SCDateTime time;
        time.SetDateTime(2024, 1, 15, 10, 0, 0);

        // Wire callback
        zm.onEngagementFinalized = [&accum](
            const ZoneRuntime& zone,
            const FinalizationResult& result)
        {
            accum.RecordEngagement(zone.type, result.touchRecord.type);
        };

        // Create VAH zone
        auto vahResult = zm.CreateZone(ZoneType::VPB_VAH, 5100.0, time, 0, true);
        zm.vahId = vahResult.zoneId;
        ZoneRuntime* vah = zm.GetZone(vahResult.zoneId);

        // Engagement 1: ACCEPTANCE
        vah->StartEngagement(100, time, 5100.25);
        vah->currentEngagement.peakPenetrationTicks = 10;
        vah->currentEngagement.outcome = AuctionOutcome::ACCEPTED;
        auto r1 = vah->FinalizeEngagement(114, time, 5100.50, 0.25, zm.config);
        if (r1 && zm.onEngagementFinalized) zm.onEngagementFinalized(*vah, r1);

        // Engagement 2: PROBE (rejection)
        vah->StartEngagement(120, time, 5100.25);
        vah->currentEngagement.peakPenetrationTicks = 5;
        vah->currentEngagement.outcome = AuctionOutcome::REJECTED;
        auto r2 = vah->FinalizeEngagement(124, time, 5100.50, 0.25, zm.config);
        if (r2 && zm.onEngagementFinalized) zm.onEngagementFinalized(*vah, r2);
    };

    // === FIRST RUN ===
    ZoneManager zm1;
    zm1.config = CreateTestConfig();
    SessionEngagementAccumulator accum1;
    simulateRun(zm1, accum1);

    std::vector<CurrentPhase> history1;
    SessionStatistics stats1 = CalculateSessionStats(zm1, accum1, CurrentPhase::ROTATION, 130, history1);

    // Capture zone counts BEFORE clearing
    int activeZonesBefore = stats1.activeZones;
    assert(activeZonesBefore == 1);  // We created 1 zone
    std::cout << "  Zone count before clear: " << activeZonesBefore << " [PASS]" << std::endl;

    // === SIMULATE BACKFILL: Clear zones, keep accumulator ===
    zm1.activeZones.clear();
    zm1.vahId = -1;
    zm1.pocId = -1;
    zm1.valId = -1;
    // NOTE: accum1 is NOT cleared - this is the key!

    // Stats should still be correct after zone clearing
    SessionStatistics statsAfterClear = CalculateSessionStats(zm1, accum1, CurrentPhase::ROTATION, 130, history1);

    // === KEY VERIFICATION: Zone counts reset, session truth persists ===
    // Zone-derived counts SHOULD reset (zones were cleared)
    assert(statsAfterClear.activeZones == 0);  // Zone count reset
    std::cout << "  Zone count after clear: " << statsAfterClear.activeZones << " (reset to 0) [PASS]" << std::endl;

    // Accumulator-derived stats SHOULD persist (session truth)
    assert(statsAfterClear.vahTouches == stats1.vahTouches);
    assert(statsAfterClear.vahAcceptances == stats1.vahAcceptances);
    assert(statsAfterClear.vahRejections == stats1.vahRejections);
    assert(statsAfterClear.totalAcceptances == stats1.totalAcceptances);
    assert(statsAfterClear.totalRejections == stats1.totalRejections);
    std::cout << "  Session truth (accumulator-derived) unchanged after zone clear [PASS]" << std::endl;

    // === SECOND RUN (fresh ZoneManager, same accumulator) ===
    // Simulating a recalc that recreates zones but keeps the same accumulator
    ZoneManager zm2;
    zm2.config = CreateTestConfig();
    SessionEngagementAccumulator accum2;
    simulateRun(zm2, accum2);

    std::vector<CurrentPhase> history2;
    SessionStatistics stats2 = CalculateSessionStats(zm2, accum2, CurrentPhase::ROTATION, 130, history2);

    // === VERIFY: Both runs produce identical stats ===
    assert(stats1.vahTouches == stats2.vahTouches);
    assert(stats1.vahAcceptances == stats2.vahAcceptances);
    assert(stats1.vahRejections == stats2.vahRejections);
    assert(stats1.totalAcceptances == stats2.totalAcceptances);
    assert(stats1.totalRejections == stats2.totalRejections);
    std::cout << "  Two identical runs produce identical stats [PASS]" << std::endl;

    // === VERIFY: Stats survive zone clearing ===
    assert(statsAfterClear.vahTouches == stats1.vahTouches);
    assert(statsAfterClear.vahAcceptances == stats1.vahAcceptances);
    assert(statsAfterClear.vahRejections == stats1.vahRejections);
    std::cout << "  Stats survive zone clearing [PASS]" << std::endl;

    // === VERIFY: Zone clearing doesn't affect accumulator ===
    assert(accum1.vah.touchCount == 2);
    assert(accum1.vah.acceptances == 1);
    assert(accum1.vah.rejections == 1);
    std::cout << "  Accumulator preserves stats after zone clear [PASS]" << std::endl;
}

// ============================================================================
// TEST 21: MID-RUN ZONE REBUILD STABILITY
// Simulates the exact scenario: replay/backfill, forcibly clear/rebuild zones
// mid-run, and confirms that session totals remain unchanged while zone counts
// reset and can accumulate new values.
// ============================================================================

void TestMidRunZoneRebuildStability() {
    std::cout << "\nTesting mid-run zone rebuild stability..." << std::endl;

    ZoneManager zm;
    zm.config = CreateTestConfig();
    SessionEngagementAccumulator accum;
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);

    // Wire callback
    zm.onEngagementFinalized = [&accum](
        const ZoneRuntime& zone,
        const FinalizationResult& result)
    {
        accum.RecordEngagement(zone.type, result.touchRecord.type);
    };

    // === PHASE 1: Initial run with some engagements ===
    auto vahResult = zm.CreateZone(ZoneType::VPB_VAH, 5100.0, time, 0, true);
    zm.vahId = vahResult.zoneId;
    ZoneRuntime* vah = zm.GetZone(vahResult.zoneId);

    // 2 engagements on VAH
    vah->StartEngagement(100, time, 5100.25);
    vah->currentEngagement.peakPenetrationTicks = 10;
    vah->currentEngagement.outcome = AuctionOutcome::ACCEPTED;
    auto r1 = vah->FinalizeEngagement(114, time, 5100.50, 0.25, zm.config);
    if (r1 && zm.onEngagementFinalized) zm.onEngagementFinalized(*vah, r1);

    vah->StartEngagement(120, time, 5100.25);
    vah->currentEngagement.peakPenetrationTicks = 5;
    vah->currentEngagement.outcome = AuctionOutcome::REJECTED;
    auto r2 = vah->FinalizeEngagement(124, time, 5100.50, 0.25, zm.config);
    if (r2 && zm.onEngagementFinalized) zm.onEngagementFinalized(*vah, r2);

    // Capture state after phase 1
    std::vector<CurrentPhase> history;
    SessionStatistics statsPhase1 = CalculateSessionStats(zm, accum, CurrentPhase::ROTATION, 130, history);

    assert(statsPhase1.activeZones == 1);
    assert(statsPhase1.vahTouches == 2);
    assert(statsPhase1.vahAcceptances == 1);
    assert(statsPhase1.vahRejections == 1);
    std::cout << "  Phase 1: 1 zone, 2 touches, 1 acceptance, 1 rejection [PASS]" << std::endl;

    // === PHASE 2: SIMULATE MID-RUN BACKFILL - Clear and rebuild zones ===
    // This simulates what happens during chart recalc/backfill
    zm.activeZones.clear();
    zm.vahId = -1;
    zm.pocId = -1;
    zm.valId = -1;
    // CRITICAL: accum is NOT cleared (persists through backfill)

    // Verify zone count is now 0 but session truth persists
    SessionStatistics statsAfterClear = CalculateSessionStats(zm, accum, CurrentPhase::ROTATION, 130, history);
    assert(statsAfterClear.activeZones == 0);  // Zone count: 0
    assert(statsAfterClear.vahTouches == 2);   // Session truth: unchanged
    std::cout << "  After clear: 0 zones, session truth unchanged [PASS]" << std::endl;

    // === PHASE 3: Rebuild zones at different prices (simulating profile recalc) ===
    auto vahResult2 = zm.CreateZone(ZoneType::VPB_VAH, 5105.0, time, 130, true);  // New VAH price
    zm.vahId = vahResult2.zoneId;
    ZoneRuntime* vah2 = zm.GetZone(vahResult2.zoneId);

    auto valResult = zm.CreateZone(ZoneType::VPB_VAL, 4900.0, time, 130, true);  // Added VAL
    zm.valId = valResult.zoneId;
    ZoneRuntime* val = zm.GetZone(valResult.zoneId);

    // === PHASE 4: New engagements on rebuilt zones ===
    // These will add to the accumulator (not replace)
    vah2->StartEngagement(140, time, 5105.25);
    vah2->currentEngagement.peakPenetrationTicks = 2;
    auto r3 = vah2->FinalizeEngagement(141, time, 5105.50, 0.25, zm.config);  // TAG
    if (r3 && zm.onEngagementFinalized) zm.onEngagementFinalized(*vah2, r3);

    val->StartEngagement(150, time, 4900.25);
    val->currentEngagement.peakPenetrationTicks = 6;
    val->currentEngagement.outcome = AuctionOutcome::REJECTED;
    auto r4 = val->FinalizeEngagement(162, time, 4900.50, 0.25, zm.config);  // TEST
    if (r4 && zm.onEngagementFinalized) zm.onEngagementFinalized(*val, r4);

    // Final stats
    SessionStatistics statsFinal = CalculateSessionStats(zm, accum, CurrentPhase::ROTATION, 170, history);

    // === KEY VERIFICATIONS ===
    // Zone counts reflect CURRENT state (rebuilt zones)
    assert(statsFinal.activeZones == 2);  // 2 new zones
    std::cout << "  Final: 2 active zones (rebuilt) [PASS]" << std::endl;

    // Session truth reflects ALL engagements across the session
    assert(statsFinal.vahTouches == 3);      // 2 from phase 1 + 1 from phase 4
    assert(statsFinal.vahAcceptances == 1);  // 1 from phase 1
    assert(statsFinal.vahRejections == 1);   // 1 from phase 1 (TAG doesn't count)
    assert(statsFinal.vahTags == 1);         // 1 from phase 4
    assert(statsFinal.valTouches == 1);      // 1 from phase 4
    assert(statsFinal.valRejections == 1);   // TEST counts as rejection
    std::cout << "  Session totals: VAH=3 touches, VAL=1 touch (accumulated) [PASS]" << std::endl;

    // Total session stats
    assert(statsFinal.totalAcceptances == 1);
    assert(statsFinal.totalRejections == 2);  // 1 VAH probe + 1 VAL test
    assert(statsFinal.totalTags == 1);
    std::cout << "  Session totals correct across rebuild [PASS]" << std::endl;

    // Verify accumulator internal state
    assert(accum.vah.touchCount == 3);
    assert(accum.val.touchCount == 1);
    assert(accum.GetTotalTouches() == 4);
    std::cout << "  Accumulator internal state correct [PASS]" << std::endl;
}

// ============================================================================
// TEST 22: ZONE TYPE CANNOT BE NONE
// Ensures that ZoneManager rejects creation of zones with ZoneType::NONE
// and that all successfully created zones have valid (non-NONE) types.
// ============================================================================

void TestZoneTypeCannotBeNone() {
    std::cout << "\nTesting ZoneType cannot be NONE..." << std::endl;

    ZoneManager zm;
    zm.config = CreateTestConfig();
    SCDateTime time;
    time.SetDateTime(2024, 1, 15, 10, 0, 0);

    // === TEST 1: Attempt to create zone with ZoneType::NONE should FAIL ===
    auto noneResult = zm.CreateZone(ZoneType::NONE, 5000.0, time, 0, true);
    assert(!noneResult.ok);
    assert(noneResult.failure == ZoneCreationFailure::INVALID_ZONE_TYPE);
    std::cout << "  CreateZone(NONE) correctly rejected [PASS]" << std::endl;

    // === TEST 2: Valid zone types should succeed ===
    auto vahResult = zm.CreateZone(ZoneType::VPB_VAH, 5100.0, time, 0, true);
    assert(vahResult.ok);
    ZoneRuntime* vah = zm.GetZone(vahResult.zoneId);
    assert(vah != nullptr);
    assert(vah->type != ZoneType::NONE);
    assert(vah->type == ZoneType::VPB_VAH);
    std::cout << "  CreateZone(VPB_VAH) succeeds with correct type [PASS]" << std::endl;

    auto pocResult = zm.CreateZone(ZoneType::VPB_POC, 5050.0, time, 0, true);
    assert(pocResult.ok);
    ZoneRuntime* poc = zm.GetZone(pocResult.zoneId);
    assert(poc != nullptr);
    assert(poc->type != ZoneType::NONE);
    assert(poc->type == ZoneType::VPB_POC);
    std::cout << "  CreateZone(VPB_POC) succeeds with correct type [PASS]" << std::endl;

    auto valResult = zm.CreateZone(ZoneType::VPB_VAL, 4900.0, time, 0, true);
    assert(valResult.ok);
    ZoneRuntime* val = zm.GetZone(valResult.zoneId);
    assert(val != nullptr);
    assert(val->type != ZoneType::NONE);
    assert(val->type == ZoneType::VPB_VAL);
    std::cout << "  CreateZone(VPB_VAL) succeeds with correct type [PASS]" << std::endl;

    // === TEST 3: Verify ALL zones in manager have non-NONE types ===
    for (const auto& [id, zone] : zm.activeZones) {
        assert(zone.type != ZoneType::NONE);
    }
    std::cout << "  All zones in ZoneManager have valid (non-NONE) types [PASS]" << std::endl;

    // === TEST 4: Creation stats should reflect the rejection ===
    // INVALID_ZONE_TYPE = 3 in ZoneCreationFailure enum
    int invalidTypeIdx = static_cast<int>(ZoneCreationFailure::INVALID_ZONE_TYPE);
    assert(zm.creationStats.failuresByReason[invalidTypeIdx] == 1);  // One NONE attempt
    assert(zm.creationStats.totalSuccesses == 3);   // Three valid zones
    assert(zm.creationStats.totalFailures == 1);    // One failure
    std::cout << "  CreationStats tracks INVALID_ZONE_TYPE rejection [PASS]" << std::endl;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "============================================" << std::endl;
    std::cout << "ENGAGEMENT LIFECYCLE & SSOT COUNTER TESTS" << std::endl;
    std::cout << "============================================" << std::endl;

    // Enum tests
    TestTouchTypeToString();
    TestUnresolvedReasonToString();

    // Basic lifecycle tests
    TestHasPendingEngagement();
    TestStartEngagement();

    // Finalize tests by classification
    TestFinalizeEngagement_TAG();
    TestFinalizeEngagement_PROBE();
    TestFinalizeEngagement_TEST();
    TestFinalizeEngagement_ACCEPTANCE();

    // ForceFinalize tests
    TestForceFinalize();
    TestForceFinalize_AllReasons();

    // Edge cases
    TestFinalizeWithoutPendingEngagement();
    TestForceFinalize_NoPendingEngagement();
    TestDoubleFinalizeGuard();

    // SSOT counter tests
    TestSSOTCounterInvariant();
    TestRingBufferSurvival();
    TestMixedOutcomeSequence();

    // Coherence tests
    TestCoherenceRules();
    TestRecencyTrackerIsolation();

    // Schema tests
    TestHaloWidthAtCreation();

    // Statistics tests
    TestSessionStatisticsAggregation();

    // Backfill stability tests
    TestBackfillStability();
    TestMidRunZoneRebuildStability();

    // Pending engagement invariant
    TestPendingEngagementInvariant();

    // Zone type validation
    TestZoneTypeCannotBeNone();

    std::cout << "\n============================================" << std::endl;
    std::cout << "ALL TESTS PASSED!" << std::endl;
    std::cout << "============================================" << std::endl;

    return 0;
}
