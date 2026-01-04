// ============================================================================
// test_phase_semantics_v2.cpp
// Tests for Phase System v2 semantic refinements:
// - DRIVING vs PULLBACK mutual exclusivity (approachingPOC gates DRIVING)
// - RANGE_EXTENSION vs PULLBACK mutual exclusivity
// - Per-phase confirmation (PULLBACK = 2 bars, others = 3)
// - FAILED_AUCTION admissibility inside VA
// ============================================================================

#define _USE_MATH_DEFINES
#include <cmath>
#include <iostream>
#include <string>
#include "test_sierrachart_mock.h"
#include "amt_core.h"
#include "AMT_Zones.h"
#include "AMT_Phase.h"

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

// Helper to create a time from bar index
SCDateTime MakeTime(int bar) {
    SCDateTime t;
    t.SetDateTime(2024, 1, 15, 9, 30 + (bar * 5) % 60, 0);
    return t;
}

// ============================================================================
// HELPER: Create minimal zone manager with VA zones
// ============================================================================

ZoneManager CreateTestZoneManager(double poc, double vah, double val, double tickSize) {
    ZoneManager zm;
    zm.config.trendingDistanceRatio = 0.8;
    zm.config.nearExtremeTicks = 3;
    zm.config.extremeUpdateWindowBars = 5;
    zm.config.directionalAfterglowBars = 10;
    zm.config.approachingPOCLookback = 2;
    zm.config.boundaryToleranceTicks = 1;

    // Set session extremes
    const double testHigh = vah + 20 * tickSize;
    const double testLow = val - 20 * tickSize;
    zm.structure.UpdateExtremes(testHigh, testLow, 0);

    // Create POC zone
    auto pocResult = zm.CreateZone(ZoneType::VPB_POC, poc, MakeTime(0), 0);
    zm.pocId = pocResult.zoneId;

    // Create VAH zone
    auto vahResult = zm.CreateZone(ZoneType::VPB_VAH, vah, MakeTime(0), 0);
    zm.vahId = vahResult.zoneId;

    // Create VAL zone
    auto valResult = zm.CreateZone(ZoneType::VPB_VAL, val, MakeTime(0), 0);
    zm.valId = valResult.zoneId;

    return zm;
}

// ============================================================================
// TEST: DRIVING vs PULLBACK mutual exclusivity
// DRIVING should NOT trigger when approachingPOC is true
// ============================================================================

bool test_trending_yields_to_pullback_when_approaching_poc() {
    double tickSize = 0.25;
    double poc = 5000.0;
    double vah = 5020.0;  // 80 ticks from POC
    double val = 4980.0;  // 80 ticks from POC
    // vaRangeTicks = 160, threshold = 160 * 0.8 = 128 ticks

    ZoneManager zm = CreateTestZoneManager(poc, vah, val, tickSize);
    zm.config.trendingDistanceRatio = 0.8;
    zm.config.approachingPOCLookback = 2;  // Need 2 consecutive contracting bars
    zm.config.directionalAfterglowBars = 10;
    PhaseTracker tracker;

    // BAR 0: Price far outside VA, trending away - 200 ticks from POC
    // dPOC = 200 > threshold 128, so DRIVING
    zm.currentBar = 0;
    double price0 = 5050.0;  // 200 ticks from POC
    PhaseSnapshot snap0 = BuildPhaseSnapshot(zm, price0, price0, tickSize, 0, tracker);
    TEST_ASSERT(snap0.rawPhase == CurrentPhase::DRIVING_UP, "Setup: Bar 0 should be DRIVING");

    // BAR 1: Continue trending further - 220 ticks from POC (expanding)
    zm.currentBar = 1;
    double price1 = 5055.0;  // 220 ticks from POC
    PhaseSnapshot snap1 = BuildPhaseSnapshot(zm, price1, price1, tickSize, 1, tracker);
    TEST_ASSERT(snap1.rawPhase == CurrentPhase::DRIVING_UP, "Setup: Bar 1 should be DRIVING");

    // BAR 2: Start retracement - 180 ticks from POC (contracting from 220)
    // approachingPOCLookback=2 needs 2 contracting bars, so this is first contracting bar
    zm.currentBar = 2;
    double price2 = 5045.0;  // 180 ticks from POC
    PhaseSnapshot snap2 = BuildPhaseSnapshot(zm, price2, price2, tickSize, 2, tracker);
    // Only 1 contracting bar so far, approachingPOC = false still
    // Should still be DRIVING

    // BAR 3: Continue retracement - 160 ticks from POC (contracting from 180)
    // Now we have 2 consecutive contracting bars: approachingPOC = true
    zm.currentBar = 3;
    double price3 = 5040.0;  // 160 ticks from POC (still above 128 threshold)
    PhaseSnapshot snap3 = BuildPhaseSnapshot(zm, price3, price3, tickSize, 3, tracker);

    // Key assertion: dPOC (160) > threshold (128), but approachingPOC is true
    // DRIVING should NOT trigger; should be PULLBACK (wasDirectionalRecently + approachingPOC)
    TEST_ASSERT(snap3.primitives.approachingPOC == true,
        "approachingPOC should be true after 2 contracting bars");
    TEST_ASSERT(snap3.rawPhase != CurrentPhase::DRIVING_UP,
        "DRIVING should NOT trigger when approachingPOC is true (retracement)");
    TEST_ASSERT(snap3.rawPhase == CurrentPhase::PULLBACK,
        "Should be PULLBACK during retracement (approachingPOC + wasDirectionalRecently)");

    TEST_PASSED("DRIVING yields to PULLBACK when approachingPOC");
}

// ============================================================================
// TEST: RANGE_EXTENSION vs PULLBACK mutual exclusivity
// RANGE_EXTENSION should NOT persist during retracement
// ============================================================================

bool test_range_extension_yields_to_pullback_when_approaching_poc() {
    double tickSize = 0.25;
    double poc = 5000.0;
    double vah = 5020.0;
    double val = 4980.0;

    ZoneManager zm = CreateTestZoneManager(poc, vah, val, tickSize);
    zm.config.nearExtremeTicks = 5;
    zm.config.extremeUpdateWindowBars = 3;
    PhaseTracker tracker;

    // Simulate being at session extreme with recent extension
    // Update session high to current price
    double extensionPrice = 5045.0;  // Above VAH, at/near extreme
    zm.structure.UpdateExtremes(5045.0, 4960.0, 0);  // Set session high = 5045

    PhaseSnapshot snap1 = BuildPhaseSnapshot(zm, extensionPrice, extensionPrice, tickSize, 0, tracker);
    // Note: madeNewExtremeRecently depends on bar tracking - may or may not be RANGE_EXTENSION

    // Simulate directional history for pullback eligibility
    tracker.history.Push(CurrentPhase::RANGE_EXTENSION);
    tracker.history.Push(CurrentPhase::RANGE_EXTENSION);

    // Now price retraces but still near extreme and extreme was recent
    // But approachingPOC is true (contracting)
    tracker.UpdatePOCDistance(200.0);  // First bar
    tracker.UpdatePOCDistance(180.0);  // Second bar - contracting

    double retracingPrice = 5040.0;  // Still near extreme but retracing
    zm.currentBar = 2;
    PhaseSnapshot snap2 = BuildPhaseSnapshot(zm, retracingPrice, retracingPrice, tickSize, 2, tracker);

    // With approachingPOC = true, RANGE_EXTENSION should yield
    // Since wasDirectionalRecently is true, should be PULLBACK
    TEST_ASSERT(snap2.rawPhase != CurrentPhase::RANGE_EXTENSION,
        "RANGE_EXTENSION should NOT persist when approachingPOC is true");

    TEST_PASSED("RANGE_EXTENSION yields during retracement");
}

// ============================================================================
// TEST: PULLBACK confirms with 2 bars (per-phase confirmation)
// ============================================================================

bool test_pullback_confirms_with_2_bars() {
    PhaseTracker tracker;
    tracker.minConfirmationBars = 3;       // Default for most phases
    tracker.pullbackConfirmationBars = 2;  // PULLBACK is transient

    // Create primitives for outside VA
    PhasePrimitives pOutside;
    pOutside.valid = true;
    pOutside.insideVA = false;
    pOutside.outsideLow = true;
    pOutside.outsideHigh = false;
    pOutside.atVAL = false;
    pOutside.atVAH = false;

    // Start with OUTSIDE_BALANCE
    CurrentPhase result = tracker.Update(CurrentPhase::OUTSIDE_BALANCE, pOutside);
    TEST_ASSERT(result == CurrentPhase::OUTSIDE_BALANCE, "Initial phase should be OUTSIDE_BALANCE");

    // First bar of PULLBACK
    result = tracker.Update(CurrentPhase::PULLBACK, pOutside);
    TEST_ASSERT(result == CurrentPhase::OUTSIDE_BALANCE, "1 bar PULLBACK should not confirm yet");
    TEST_ASSERT(tracker.candidateBars == 1, "candidateBars should be 1");

    // Second bar of PULLBACK - should confirm (2 bars for PULLBACK)
    result = tracker.Update(CurrentPhase::PULLBACK, pOutside);
    TEST_ASSERT(result == CurrentPhase::PULLBACK, "2 bars PULLBACK should confirm (per-phase threshold)");

    TEST_PASSED("PULLBACK confirms with 2 bars (per-phase confirmation)");
}

// ============================================================================
// TEST: Other phases still require 3 bars for confirmation
// ============================================================================

bool test_other_phases_require_3_bars() {
    PhaseTracker tracker;
    tracker.minConfirmationBars = 3;
    tracker.pullbackConfirmationBars = 2;

    PhasePrimitives pOutside;
    pOutside.valid = true;
    pOutside.insideVA = false;
    pOutside.outsideLow = true;
    pOutside.outsideHigh = false;

    // Start with OUTSIDE_BALANCE
    CurrentPhase result = tracker.Update(CurrentPhase::OUTSIDE_BALANCE, pOutside);

    // Try to confirm DRIVING (should need 3 bars)
    result = tracker.Update(CurrentPhase::DRIVING_UP, pOutside);
    TEST_ASSERT(result == CurrentPhase::OUTSIDE_BALANCE, "1 bar DRIVING should not confirm");

    result = tracker.Update(CurrentPhase::DRIVING_UP, pOutside);
    TEST_ASSERT(result == CurrentPhase::OUTSIDE_BALANCE, "2 bars DRIVING should not confirm");

    result = tracker.Update(CurrentPhase::DRIVING_UP, pOutside);
    TEST_ASSERT(result == CurrentPhase::DRIVING_UP, "3 bars DRIVING should confirm");

    TEST_PASSED("Other phases require 3 bars for confirmation");
}

// ============================================================================
// TEST: FAILED_AUCTION phase not admissible inside VA mid-range
// Even if failureRecent is true, phase should revert to ROTATION
// ============================================================================

bool test_failed_auction_not_admissible_inside_va_mid_range() {
    double tickSize = 0.25;
    double poc = 5000.0;
    double vah = 5020.0;
    double val = 4980.0;

    ZoneManager zm = CreateTestZoneManager(poc, vah, val, tickSize);
    zm.config.boundaryToleranceTicks = 2;  // 2 tick tolerance for boundary
    zm.config.failedAuctionRecencyBars = 10;
    PhaseTracker tracker;

    // Get VAH zone and simulate a failure event
    ZoneRuntime* vahZone = zm.GetZone(zm.vahId);
    vahZone->lastFailureBar = 0;  // Failure at bar 0

    // BAR 0-1: Price outside VA (establishes "was outside")
    zm.currentBar = 0;
    double outsidePrice = 5030.0;  // Above VAH
    BuildPhaseSnapshot(zm, outsidePrice, outsidePrice, tickSize, 0, tracker);

    zm.currentBar = 1;
    BuildPhaseSnapshot(zm, outsidePrice, outsidePrice, tickSize, 1, tracker);

    // BAR 2: Return inside VA
    zm.currentBar = 2;
    double insidePrice = 5010.0;  // Inside VA, not at boundary
    BuildPhaseSnapshot(zm, insidePrice, insidePrice, tickSize, 2, tracker);

    // BAR 3-7: Stay inside VA for 5+ bars to clear "justReturnedFromOutside"
    // Note: barsSinceReturnedToVA is read BEFORE update for current bar,
    // so we need one extra bar to ensure the counter exceeds threshold
    zm.currentBar = 3;
    BuildPhaseSnapshot(zm, insidePrice, insidePrice, tickSize, 3, tracker);

    zm.currentBar = 4;
    BuildPhaseSnapshot(zm, insidePrice, insidePrice, tickSize, 4, tracker);

    zm.currentBar = 5;
    BuildPhaseSnapshot(zm, insidePrice, insidePrice, tickSize, 5, tracker);

    zm.currentBar = 6;
    BuildPhaseSnapshot(zm, insidePrice, insidePrice, tickSize, 6, tracker);

    zm.currentBar = 7;
    double midValuePrice = 5005.0;  // Inside VA, not at boundary
    PhaseSnapshot snap = BuildPhaseSnapshot(zm, midValuePrice, midValuePrice, tickSize, 7, tracker);

    // failureRecent should still be true (bar 0 failure, bar 7 current, 7 < 10)
    TEST_ASSERT(snap.primitives.failureRecent == true,
        "Setup: failureRecent should be true (bar 7, failure at bar 0)");

    // justReturnedFromOutside should be false (counter=4 when read, > threshold=3)
    TEST_ASSERT(snap.primitives.justReturnedFromOutside == false,
        "Setup: justReturnedFromOutside should be false (5 bars since return)");

    // Key assertion: Despite failureRecent, phase should NOT be FAILED_AUCTION
    // because we're mid-value, not at boundary, and not just returned
    TEST_ASSERT(snap.rawPhase != CurrentPhase::FAILED_AUCTION,
        "FAILED_AUCTION should NOT be raw phase when inside VA mid-range");

    // Should be ROTATION (normal inside-VA behavior)
    TEST_ASSERT(snap.rawPhase == CurrentPhase::ROTATION,
        "Should be ROTATION when inside VA mid-range (failureRecent but not admissible)");

    TEST_PASSED("FAILED_AUCTION not admissible inside VA mid-range");
}

// ============================================================================
// TEST: FAILED_AUCTION IS admissible at boundary
// ============================================================================

bool test_failed_auction_admissible_at_boundary() {
    double tickSize = 0.25;
    double poc = 5000.0;
    double vah = 5020.0;
    double val = 4980.0;

    ZoneManager zm = CreateTestZoneManager(poc, vah, val, tickSize);
    zm.config.boundaryToleranceTicks = 2;
    zm.config.failedAuctionRecencyBars = 10;
    PhaseTracker tracker;

    // Set up failure event
    ZoneRuntime* vahZone = zm.GetZone(zm.vahId);
    vahZone->lastFailureBar = 0;

    // Price at VAH boundary
    double boundaryPrice = 5020.0;  // At VAH
    zm.currentBar = 3;

    PhaseSnapshot snap = BuildPhaseSnapshot(zm, boundaryPrice, boundaryPrice, tickSize, 3, tracker);

    // At boundary with failureRecent - should be FAILED_AUCTION
    TEST_ASSERT(snap.rawPhase == CurrentPhase::FAILED_AUCTION,
        "FAILED_AUCTION should be raw phase at boundary when failureRecent");

    TEST_PASSED("FAILED_AUCTION admissible at boundary");
}

// ============================================================================
// TEST: FAILED_AUCTION IS admissible when just returned from outside
// ============================================================================

bool test_failed_auction_admissible_just_returned() {
    double tickSize = 0.25;
    double poc = 5000.0;
    double vah = 5020.0;
    double val = 4980.0;

    ZoneManager zm = CreateTestZoneManager(poc, vah, val, tickSize);
    zm.config.boundaryToleranceTicks = 2;
    zm.config.failedAuctionRecencyBars = 10;
    PhaseTracker tracker;

    // Set up failure event
    ZoneRuntime* vahZone = zm.GetZone(zm.vahId);
    vahZone->lastFailureBar = 0;

    // Simulate price was outside, then returned
    tracker.UpdateOutsideClose(true);   // Bar 0: outside
    tracker.UpdateOutsideClose(true);   // Bar 1: outside
    tracker.UpdateOutsideClose(false);  // Bar 2: just returned inside

    // Price is now mid-value but JUST returned (within 3 bar threshold)
    double midValuePrice = 5010.0;  // Inside VA, not at boundary
    zm.currentBar = 2;

    PhaseSnapshot snap = BuildPhaseSnapshot(zm, midValuePrice, midValuePrice, tickSize, 2, tracker);

    // justReturnedFromOutside should be true (just returned this bar)
    TEST_ASSERT(snap.primitives.justReturnedFromOutside == true,
        "Setup: justReturnedFromOutside should be true");

    // Should be FAILED_AUCTION (admissible because just returned)
    TEST_ASSERT(snap.rawPhase == CurrentPhase::FAILED_AUCTION,
        "FAILED_AUCTION should be raw phase when just returned from outside");

    TEST_PASSED("FAILED_AUCTION admissible when just returned");
}

// ============================================================================
// TEST: Synthetic PULLBACK sequence - raw detection and confirmation
// ============================================================================

bool test_pullback_synthetic_sequence() {
    double tickSize = 0.25;
    double poc = 5000.0;
    double vah = 5020.0;
    double val = 4980.0;
    // vaRangeTicks = 160, threshold = 160 * 0.8 = 128 ticks

    ZoneManager zm = CreateTestZoneManager(poc, vah, val, tickSize);
    zm.config.trendingDistanceRatio = 0.8;
    zm.config.directionalAfterglowBars = 10;
    zm.config.approachingPOCLookback = 2;  // 2 contracting bars needed
    PhaseTracker tracker;
    tracker.pullbackConfirmationBars = 2;

    // BAR 0: Establish directional activity (DRIVING)
    // Price 200 ticks from POC, above threshold (128)
    zm.currentBar = 0;
    double price0 = 5050.0;  // 200 ticks from POC
    PhaseSnapshot snap0 = BuildPhaseSnapshot(zm, price0, price0, tickSize, 0, tracker);
    TEST_ASSERT(snap0.rawPhase == CurrentPhase::DRIVING_UP, "Setup: Bar 0 should be DRIVING");

    // BAR 1: Continue trending (distance expanding to 220)
    zm.currentBar = 1;
    double price1 = 5055.0;  // 220 ticks from POC
    PhaseSnapshot snap1 = BuildPhaseSnapshot(zm, price1, price1, tickSize, 1, tracker);
    TEST_ASSERT(snap1.rawPhase == CurrentPhase::DRIVING_UP, "Setup: Bar 1 should be DRIVING");

    // BAR 2: Start retracement - 180 ticks (first contracting bar)
    zm.currentBar = 2;
    double price2 = 5045.0;  // 180 ticks from POC
    PhaseSnapshot snap2 = BuildPhaseSnapshot(zm, price2, price2, tickSize, 2, tracker);
    // Only 1 contracting bar, approachingPOC not yet true

    // BAR 3: Continue retracement - 160 ticks (second contracting bar)
    // Now approachingPOC = true (2 consecutive contractions)
    zm.currentBar = 3;
    double price3 = 5040.0;  // 160 ticks from POC
    PhaseSnapshot snap3 = BuildPhaseSnapshot(zm, price3, price3, tickSize, 3, tracker);

    // Should now detect RAW=PULLBACK (approachingPOC + wasDirectionalRecently)
    TEST_ASSERT(snap3.primitives.approachingPOC == true,
        "approachingPOC should be true after 2 contracting bars");
    TEST_ASSERT(snap3.rawPhase == CurrentPhase::PULLBACK,
        "RAW=PULLBACK should trigger on second retracement bar");

    // BAR 4: Continue retracement - 140 ticks (third contracting bar)
    zm.currentBar = 4;
    double price4 = 5035.0;  // 140 ticks from POC
    PhaseSnapshot snap4 = BuildPhaseSnapshot(zm, price4, price4, tickSize, 4, tracker);

    TEST_ASSERT(snap4.rawPhase == CurrentPhase::PULLBACK,
        "RAW=PULLBACK should continue");

    // With pullbackConfirmationBars = 2, should now be confirmed
    TEST_ASSERT(snap4.phase == CurrentPhase::PULLBACK,
        "CONF=PULLBACK should confirm after 2 consecutive RAW=PULLBACK bars");

    TEST_PASSED("PULLBACK synthetic sequence - detection and confirmation");
}

// ============================================================================
// TEST: Per-phase confirmation getter returns correct values
// ============================================================================

bool test_per_phase_confirmation_getter() {
    PhaseTracker tracker;
    tracker.minConfirmationBars = 3;
    tracker.pullbackConfirmationBars = 2;

    // PULLBACK should return 2
    TEST_ASSERT(tracker.GetConfirmationBarsFor(CurrentPhase::PULLBACK) == 2,
        "PULLBACK confirmation should be 2 bars");

    // Other phases should return 3
    TEST_ASSERT(tracker.GetConfirmationBarsFor(CurrentPhase::DRIVING_UP) == 3,
        "DRIVING confirmation should be 3 bars");
    TEST_ASSERT(tracker.GetConfirmationBarsFor(CurrentPhase::ROTATION) == 3,
        "ROTATION confirmation should be 3 bars");
    TEST_ASSERT(tracker.GetConfirmationBarsFor(CurrentPhase::RANGE_EXTENSION) == 3,
        "RANGE_EXTENSION confirmation should be 3 bars");
    TEST_ASSERT(tracker.GetConfirmationBarsFor(CurrentPhase::OUTSIDE_BALANCE) == 3,
        "OUTSIDE_BALANCE confirmation should be 3 bars");

    TEST_PASSED("Per-phase confirmation getter returns correct values");
}

// ============================================================================
// TEST: JustReturnedFromOutside tracking
// ============================================================================

bool test_just_returned_from_outside_tracking() {
    OutsideCloseTracker tracker;

    // Start inside
    tracker.Update(false);  // Inside
    TEST_ASSERT(!tracker.JustReturnedFromOutside(), "Should not be 'just returned' when always inside");

    // Go outside
    tracker.Update(true);   // Outside
    TEST_ASSERT(!tracker.JustReturnedFromOutside(), "Should not be 'just returned' while outside");

    tracker.Update(true);   // Still outside
    TEST_ASSERT(!tracker.JustReturnedFromOutside(), "Should not be 'just returned' while outside");

    // Return inside - this is the "just returned" moment
    tracker.Update(false);  // Just returned
    TEST_ASSERT(tracker.JustReturnedFromOutside(3), "Should be 'just returned' (0 bars since return)");

    // One bar later
    tracker.Update(false);  // Still inside
    TEST_ASSERT(tracker.JustReturnedFromOutside(3), "Should still be 'just returned' (1 bar since return)");

    // Two bars later
    tracker.Update(false);  // Still inside
    TEST_ASSERT(tracker.JustReturnedFromOutside(3), "Should still be 'just returned' (2 bars since return)");

    // Three bars later
    tracker.Update(false);  // Still inside
    TEST_ASSERT(tracker.JustReturnedFromOutside(3), "Should still be 'just returned' (3 bars since return)");

    // Four bars later - outside threshold
    tracker.Update(false);  // Still inside
    TEST_ASSERT(!tracker.JustReturnedFromOutside(3), "Should NOT be 'just returned' (4 bars > 3 threshold)");

    TEST_PASSED("JustReturnedFromOutside tracking works correctly");
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "======================================" << std::endl;
    std::cout << "Phase Semantics v2 Tests" << std::endl;
    std::cout << "======================================" << std::endl;

    // DRIVING vs PULLBACK mutual exclusivity
    test_trending_yields_to_pullback_when_approaching_poc();

    // RANGE_EXTENSION vs PULLBACK mutual exclusivity
    test_range_extension_yields_to_pullback_when_approaching_poc();

    // Per-phase confirmation
    test_pullback_confirms_with_2_bars();
    test_other_phases_require_3_bars();
    test_per_phase_confirmation_getter();

    // FAILED_AUCTION admissibility
    test_failed_auction_not_admissible_inside_va_mid_range();
    test_failed_auction_admissible_at_boundary();
    test_failed_auction_admissible_just_returned();

    // Synthetic sequence
    test_pullback_synthetic_sequence();

    // Helper tracking
    test_just_returned_from_outside_tracking();

    std::cout << "======================================" << std::endl;
    std::cout << "Results: " << g_passed << " passed, " << g_failed << " failed" << std::endl;
    std::cout << "======================================" << std::endl;

    return g_failed > 0 ? 1 : 0;
}
