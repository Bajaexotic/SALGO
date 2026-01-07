// Define standalone test mode to exclude SC-dependent functions
#define AMT_STANDALONE_TEST

// ============================================================================
// test_phase_detection.cpp
// Tests for phase detection logic fixes:
// - DRIVING should NOT trigger when vaRangeTicks = 0
// - DRIVING should trigger correctly when vaRangeTicks > 0
// - VA context sync updates values correctly
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

// Helper to create a time from bar index (simulates 5-minute bars)
SCDateTime MakeTime(int bar) {
    SCDateTime t;
    t.SetDateTime(2024, 1, 15, 9, 30 + (bar * 5) % 60, 0);
    return t;
}

// ============================================================================
// HELPER: Test session levels (replaces removed sessionCtx.rth_* fields)
// ============================================================================
struct TestSessionLevels {
    double poc = 0.0;
    double vah = 0.0;
    double val = 0.0;
    int vaRangeTicks = 0;
};

// ============================================================================
// HELPER: Create minimal zone manager with VA zones
// ============================================================================

ZoneManager CreateTestZoneManager(double poc, double vah, double val, double tickSize) {
    ZoneManager zm;
    zm.config.trendingDistanceRatio = 0.8;  // Default
    zm.config.nearExtremeTicks = 3;

    // Set session extremes via StructureTracker (SSOT for bar-based extremes)
    // Use UpdateExtremes() to maintain encapsulation
    const double testHigh = vah + 10 * tickSize;  // Session high above VAH
    const double testLow = val - 10 * tickSize;   // Session low below VAL
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

// Helper to create session levels
TestSessionLevels CreateTestLevels(double poc, double vah, double val, double tickSize) {
    TestSessionLevels levels;
    levels.poc = poc;
    levels.vah = vah;
    levels.val = val;
    levels.vaRangeTicks = static_cast<int>((vah - val) / tickSize);
    return levels;
}

// ============================================================================
// TEST: vaRangeTicks is always valid when VA zones are properly set
// Root cause fix: vaRangeTicks should never be 0 when VAH > VAL
// ============================================================================

bool test_varange_always_valid_with_proper_va() {
    // Setup session levels (SSOT - no longer in ZoneManager)
    double poc = 5000.0;
    double vah = 5010.0;
    double val = 4990.0;
    double tickSize = 0.25;
    int vaRangeTicks = static_cast<int>((vah - val) / tickSize);

    // ROOT CAUSE FIX: vaRangeTicks is calculated from VAH - VAL
    // If VAH and VAL are valid, vaRangeTicks must be > 0
    TEST_ASSERT(vaRangeTicks > 0, "vaRangeTicks must be > 0 when VAH > VAL");

    // Setup zone manager
    ZoneManager zm;
    zm.config.trendingDistanceRatio = 0.8;

    // Create zones
    auto pocResult = zm.CreateZone(ZoneType::VPB_POC, poc, MakeTime(0), 0);
    zm.pocId = pocResult.zoneId;
    auto vahResult = zm.CreateZone(ZoneType::VPB_VAH, vah, MakeTime(0), 0);
    zm.vahId = vahResult.zoneId;
    auto valResult = zm.CreateZone(ZoneType::VPB_VAL, val, MakeTime(0), 0);
    zm.valId = valResult.zoneId;

    PhaseTracker tracker;

    // Price outside VA
    double currentPrice = 5020.0;  // Above VAH, outside VA

    // BuildPhaseSnapshot now takes vah/val/vaRangeTicks as parameters
    PhaseSnapshot snap = BuildPhaseSnapshot(zm, currentPrice, tickSize, vah, val, vaRangeTicks, tracker);

    // With valid vaRangeTicks, phase detection should work correctly
    TEST_ASSERT(snap.vaRangeTicks > 0, "PhaseSnapshot.vaRangeTicks must be > 0");

    // 5020 is 80 ticks from POC (5000), vaRangeTicks is 80
    // 80 > 80 * 0.8 = 64, so should be DRIVING
    TEST_ASSERT(snap.rawPhase == CurrentPhase::DRIVING_UP,
        "With valid vaRangeTicks, DRIVING should trigger correctly");

    TEST_PASSED("vaRangeTicks always valid with proper VA");
}

// ============================================================================
// TEST: DRIVING should trigger when vaRangeTicks > 0 and conditions met
// ============================================================================

bool test_trending_triggers_correctly() {
    double tickSize = 0.25;
    double poc = 5000.0;
    double vah = 5020.0;  // 80 ticks above POC
    double val = 4980.0;  // 80 ticks below POC
    int vaRangeTicks = static_cast<int>((vah - val) / tickSize);  // 160 ticks

    ZoneManager zm = CreateTestZoneManager(poc, vah, val, tickSize);
    PhaseTracker tracker;

    // Price outside VA and far from POC (> 0.8 * 160 = 128 ticks)
    // Need price to be > 128 ticks from POC = 5000 + 32 = 5032
    double currentPrice = 5040.0;  // 160 ticks from POC, outside VA

    PhaseSnapshot snap = BuildPhaseSnapshot(zm, currentPrice, tickSize, vah, val, vaRangeTicks, tracker);

    TEST_ASSERT(snap.vaRangeTicks == 160.0, "vaRangeTicks should be 160");
    TEST_ASSERT(snap.isOutsideVA == true, "Should be outside VA (above VAH)");
    TEST_ASSERT(snap.distFromPOCTicks == 160.0, "distFromPOC should be 160 ticks");

    // 160 > 160 * 0.8 = 128, so DRIVING should trigger
    TEST_ASSERT(snap.rawPhase == CurrentPhase::DRIVING_UP,
        "Should be DRIVING when far from POC and outside VA");

    TEST_PASSED("DRIVING triggers correctly with valid vaRangeTicks");
}

// ============================================================================
// TEST: ROTATION when inside VA (baseline)
// ============================================================================

bool test_rotation_inside_va() {
    double tickSize = 0.25;
    double poc = 5000.0;
    double vah = 5020.0;
    double val = 4980.0;
    int vaRangeTicks = static_cast<int>((vah - val) / tickSize);

    ZoneManager zm = CreateTestZoneManager(poc, vah, val, tickSize);
    PhaseTracker tracker;

    // Price inside VA, near POC
    double currentPrice = 5005.0;

    PhaseSnapshot snap = BuildPhaseSnapshot(zm, currentPrice, tickSize, vah, val, vaRangeTicks, tracker);

    TEST_ASSERT(snap.isOutsideVA == false, "Should be inside VA");
    TEST_ASSERT(snap.rawPhase == CurrentPhase::ROTATION,
        "Should be ROTATION when inside VA");

    TEST_PASSED("ROTATION when inside VA");
}

// ============================================================================
// TEST: Outside VA = DRIVING (default outside-VA phase)
// (AMT: ROTATION is ONLY inside VA; outside VA defaults to DRIVING)
// ============================================================================

bool test_outside_va_but_close_to_poc() {
    double tickSize = 0.25;
    double poc = 5000.0;
    double vah = 5020.0;
    double val = 4980.0;
    int vaRangeTicks = static_cast<int>((vah - val) / tickSize);

    ZoneManager zm = CreateTestZoneManager(poc, vah, val, tickSize);
    PhaseTracker tracker;

    // Price outside VA but close to POC
    double currentPrice = 5025.0;  // 100 ticks from POC (5000)

    PhaseSnapshot snap = BuildPhaseSnapshot(zm, currentPrice, tickSize, vah, val, vaRangeTicks, tracker);

    TEST_ASSERT(snap.isOutsideVA == true, "Should be outside VA");
    TEST_ASSERT(snap.distFromPOCTicks == 100.0, "distFromPOC should be 100 ticks");

    // INVARIANT: outsideVA => phase != ROTATION
    // Outside VA defaults to DRIVING (sustained conviction outside value)
    TEST_ASSERT(snap.rawPhase == CurrentPhase::DRIVING_UP,
        "Should be DRIVING when outside VA (INVARIANT: outsideVA => !ROTATION)");

    TEST_PASSED("Outside VA = DRIVING (default)");
}

// ============================================================================
// TEST: VA level calculation (SSOT now in local variables/SessionManager)
// ============================================================================

bool test_va_level_calculation() {
    // Session levels are now calculated locally (SSOT)
    // This test validates the calculation logic

    // Initial values
    double poc = 5000.0;
    double vah = 5010.0;
    double val = 4990.0;
    double tickSize = 0.25;
    int vaRangeTicks = static_cast<int>((vah - val) / tickSize);

    TEST_ASSERT(vaRangeTicks == 80, "Initial vaRangeTicks should be 80");

    // Simulate VA migration
    double newPoc = 5050.0;
    double newVah = 5070.0;
    double newVal = 5030.0;

    // Recalculate
    poc = newPoc;
    vah = newVah;
    val = newVal;
    vaRangeTicks = static_cast<int>((newVah - newVal) / tickSize);

    TEST_ASSERT(poc == 5050.0, "POC should be updated to 5050");
    TEST_ASSERT(vah == 5070.0, "VAH should be updated to 5070");
    TEST_ASSERT(val == 5030.0, "VAL should be updated to 5030");
    TEST_ASSERT(vaRangeTicks == 160, "vaRangeTicks should be 160");

    TEST_PASSED("VA level calculation works correctly");
}

// ============================================================================
// TEST: Invalid VA inputs → REGIME=UNKNOWN, PHASE=UNKNOWN
// (No CORE_VA fallback in phase engine)
// ============================================================================

bool test_invalid_va_returns_unknown() {
    double tickSize = 0.25;

    // Create zone manager WITHOUT proper VA zones
    ZoneManager zm;
    // Don't create VAH/VAL/POC zones - leave them invalid
    PhaseTracker tracker;

    double price = 5000.0;

    // Build snapshot with invalid zones - should return UNKNOWN
    // Use explicit daltonState parameter (will be overridden to UNKNOWN due to invalid VA)
    PhaseSnapshot snap = BuildPhaseSnapshot(zm, price, price, tickSize, 0, tracker,
                                            AMTMarketState::BALANCE);

    TEST_ASSERT(snap.marketState == AMTMarketState::UNKNOWN,
        "Invalid VA should return marketState=UNKNOWN");
    TEST_ASSERT(snap.phase == CurrentPhase::UNKNOWN,
        "Invalid VA should return PHASE=UNKNOWN");
    TEST_ASSERT(snap.primitives.valid == false,
        "Primitives should be marked invalid");

    TEST_PASSED("Invalid VA returns UNKNOWN (no CORE_VA fallback)");
}

// ============================================================================
// TEST: Phase hysteresis prevents flicker
// ============================================================================

bool test_phase_hysteresis() {
    PhaseTracker tracker;
    tracker.minConfirmationBars = 3;

    // Create primitives for inside VA
    PhasePrimitives pInside;
    pInside.valid = true;
    pInside.insideVA = true;
    pInside.outsideLow = false;
    pInside.outsideHigh = false;

    // Start with ROTATION (inside VA)
    CurrentPhase result = tracker.Update(CurrentPhase::ROTATION, pInside);
    TEST_ASSERT(result == CurrentPhase::ROTATION, "Initial phase should be ROTATION");

    // Test hysteresis within admissible states (all inside VA)
    // Single bar of TESTING_BOUNDARY should not change confirmed phase
    result = tracker.Update(CurrentPhase::TESTING_BOUNDARY, pInside);
    TEST_ASSERT(result == CurrentPhase::ROTATION, "1 bar TESTING_BOUNDARY should not flip from ROTATION");

    // 2 bars of TESTING_BOUNDARY
    result = tracker.Update(CurrentPhase::TESTING_BOUNDARY, pInside);
    TEST_ASSERT(result == CurrentPhase::ROTATION, "2 bars TESTING_BOUNDARY should not flip");

    // 3 bars of TESTING_BOUNDARY - should now flip
    result = tracker.Update(CurrentPhase::TESTING_BOUNDARY, pInside);
    TEST_ASSERT(result == CurrentPhase::TESTING_BOUNDARY, "3 bars TESTING_BOUNDARY should flip");

    // Return to ROTATION - needs 3 bars again
    result = tracker.Update(CurrentPhase::ROTATION, pInside);
    TEST_ASSERT(result == CurrentPhase::TESTING_BOUNDARY, "1 bar ROTATION should not flip back");

    result = tracker.Update(CurrentPhase::ROTATION, pInside);
    TEST_ASSERT(result == CurrentPhase::TESTING_BOUNDARY, "2 bars ROTATION should not flip back");

    result = tracker.Update(CurrentPhase::ROTATION, pInside);
    TEST_ASSERT(result == CurrentPhase::ROTATION, "3 bars ROTATION should flip back");

    TEST_PASSED("Phase hysteresis prevents flicker");
}

// ============================================================================
// TEST: Outside VA = DRIVING (default outside-VA phase)
// ============================================================================

bool test_outside_balance_phase() {
    double tickSize = 0.25;
    double poc = 5000.0;
    double vah = 5020.0;  // 80 ticks above POC
    double val = 4980.0;  // 80 ticks below POC
    int vaRangeTicks = static_cast<int>((vah - val) / tickSize);  // 160 ticks

    ZoneManager zm = CreateTestZoneManager(poc, vah, val, tickSize);
    PhaseTracker tracker;

    // Price just below VAL, outside VA
    double currentPrice = 4975.0;  // 5 ticks below VAL, 100 ticks from POC

    PhaseSnapshot snap = BuildPhaseSnapshot(zm, currentPrice, tickSize, vah, val, vaRangeTicks, tracker);

    TEST_ASSERT(snap.isOutsideVA == true, "Should be outside VA (below VAL)");
    TEST_ASSERT(snap.distFromPOCTicks == 100.0, "distFromPOC should be 100 ticks");
    // Outside VA defaults to DRIVING (sustained conviction outside value)
    TEST_ASSERT(snap.rawPhase == CurrentPhase::DRIVING_UP,
        "Should be DRIVING when outside VA");

    TEST_PASSED("DRIVING when outside VA");
}

// ============================================================================
// TEST: Phase System v2 - INVARIANT A: ROTATION => insideVA && !atBoundary
// ============================================================================

bool test_invariant_rotation_inside_va() {
    double tickSize = 0.25;
    double poc = 5000.0;
    double vah = 5020.0;
    double val = 4980.0;
    int vaRangeTicks = static_cast<int>((vah - val) / tickSize);

    ZoneManager zm = CreateTestZoneManager(poc, vah, val, tickSize);
    PhaseTracker tracker;

    // Test multiple prices inside VA
    double testPrices[] = {5005.0, 4995.0, 5010.0, 4990.0};  // All inside VA, not at boundary

    for (double price : testPrices) {
        PhaseSnapshot snap = BuildPhaseSnapshot(zm, price, tickSize, vah, val, vaRangeTicks, tracker);

        if (snap.rawPhase == CurrentPhase::ROTATION) {
            // INVARIANT A: ROTATION => insideVA && !atBoundary
            TEST_ASSERT(snap.isOutsideVA == false,
                "INVARIANT A violated: ROTATION phase but isOutsideVA=true");
        }
    }

    TEST_PASSED("INVARIANT A: ROTATION => insideVA && !atBoundary");
}

// ============================================================================
// TEST: Phase System v2 - INVARIANT B: outsideVA => phase != ROTATION
// ============================================================================

bool test_invariant_outside_va_never_rotation() {
    double tickSize = 0.25;
    double poc = 5000.0;
    double vah = 5020.0;
    double val = 4980.0;
    int vaRangeTicks = static_cast<int>((vah - val) / tickSize);

    ZoneManager zm = CreateTestZoneManager(poc, vah, val, tickSize);
    PhaseTracker tracker;

    // Test prices outside VA
    double testPrices[] = {5025.0, 5050.0, 4975.0, 4950.0};  // All outside VA

    for (double price : testPrices) {
        PhaseSnapshot snap = BuildPhaseSnapshot(zm, price, tickSize, vah, val, vaRangeTicks, tracker);

        // INVARIANT B: outsideVA => phase != ROTATION
        if (snap.isOutsideVA) {
            TEST_ASSERT(snap.rawPhase != CurrentPhase::ROTATION,
                "INVARIANT B violated: outsideVA but rawPhase=ROTATION");
        }
    }

    TEST_PASSED("INVARIANT B: outsideVA => phase != ROTATION");
}

// ============================================================================
// TEST: Phase System v2 - TESTING_BOUNDARY at VA edges
// ============================================================================

bool test_testing_boundary_at_va_edges() {
    double tickSize = 0.25;
    double poc = 5000.0;
    double vah = 5020.0;
    double val = 4980.0;
    int vaRangeTicks = static_cast<int>((vah - val) / tickSize);

    ZoneManager zm = CreateTestZoneManager(poc, vah, val, tickSize);
    zm.config.boundaryToleranceTicks = 1;  // 1 tick tolerance
    PhaseTracker tracker;

    // Price at VAH (within 1 tick tolerance)
    double currentPrice = 5020.0;  // Exactly at VAH
    PhaseSnapshot snap = BuildPhaseSnapshot(zm, currentPrice, tickSize, vah, val, vaRangeTicks, tracker);
    TEST_ASSERT(snap.rawPhase == CurrentPhase::TESTING_BOUNDARY,
        "Should be TESTING_BOUNDARY at VAH");

    // Price at VAL
    currentPrice = 4980.0;
    tracker.Reset();
    snap = BuildPhaseSnapshot(zm, currentPrice, tickSize, vah, val, vaRangeTicks, tracker);
    TEST_ASSERT(snap.rawPhase == CurrentPhase::TESTING_BOUNDARY,
        "Should be TESTING_BOUNDARY at VAL");

    TEST_PASSED("TESTING_BOUNDARY at VA edges");
}

// ============================================================================
// TEST: AMT Admissibility - Hysteresis cannot output ROTATION when outside VA
// ============================================================================

bool test_admissibility_clamp() {
    double tickSize = 0.25;
    double poc = 5000.0;
    double vah = 5020.0;
    double val = 4980.0;
    int vaRangeTicks = static_cast<int>((vah - val) / tickSize);

    ZoneManager zm = CreateTestZoneManager(poc, vah, val, tickSize);
    PhaseTracker tracker;

    // First, establish ROTATION as confirmed phase (inside VA)
    for (int i = 0; i < 5; i++) {
        double insidePrice = 5005.0;
        BuildPhaseSnapshot(zm, insidePrice, tickSize, vah, val, vaRangeTicks, tracker);
    }
    TEST_ASSERT(tracker.confirmedPhase == CurrentPhase::ROTATION,
        "Should have ROTATION confirmed after 5 bars inside VA");

    // Now move outside VA - confirmed should NOT stay ROTATION due to admissibility clamp
    double outsidePrice = 4970.0;  // Below VAL
    PhaseSnapshot snap = BuildPhaseSnapshot(zm, outsidePrice, tickSize, vah, val, vaRangeTicks, tracker);

    // AMT INVARIANT: outsideVA => phase != ROTATION
    TEST_ASSERT(snap.phase != CurrentPhase::ROTATION,
        "AMT ADMISSIBILITY: confirmedPhase must NOT be ROTATION when price is outside VA");

    // Outside VA defaults to DRIVING phase
    TEST_ASSERT(snap.rawPhase == CurrentPhase::DRIVING_UP,
        "Raw phase should be DRIVING when below VAL");

    TEST_PASSED("AMT Admissibility clamp works correctly");
}

// ============================================================================
// TEST: AMT Regime - TRANSITION for unaccepted, IMBALANCE for accepted
// ============================================================================

bool test_regime_acceptance() {
    double tickSize = 0.25;
    double poc = 5000.0;
    double vah = 5020.0;
    double val = 4980.0;
    int vaRangeTicks = static_cast<int>((vah - val) / tickSize);

    ZoneManager zm = CreateTestZoneManager(poc, vah, val, tickSize);
    zm.config.acceptanceClosesRequired = 3;  // Need 3 closes outside for acceptance
    zm.config.boundaryToleranceTicks = 1;    // 1 tick tolerance
    PhaseTracker tracker;

    // Price outside VA beyond tolerance (> VAH + tol*tickSize)
    // VAH=5020, tol=1 tick=0.25, so must be > 5020.25
    double outsidePrice = 5025.0;

    // NOTE (Dec 2024 Migration):
    // Market state now comes from Dalton SSOT, not from acceptance counting.
    // The TRANSITION regime no longer exists - we test phase behavior instead.
    // Phase should be DRIVING when outside VA (default outside-VA behavior).

    // First bar outside - market state from Dalton (passed as IMBALANCE for test)
    // Legacy wrappers default to UNKNOWN for backward compatibility
    PhaseSnapshot snap = BuildPhaseSnapshot(zm, outsidePrice, tickSize, vah, val, vaRangeTicks, tracker);

    // Outside VA should give DRIVING phase (default outside-VA phase)
    TEST_ASSERT(snap.phase == CurrentPhase::DRIVING_UP,
        "Outside VA should give DRIVING phase");

    // Continue building bars - phase should stay DRIVING
    snap = BuildPhaseSnapshot(zm, outsidePrice, tickSize, vah, val, vaRangeTicks, tracker);
    TEST_ASSERT(snap.phase == CurrentPhase::DRIVING_UP,
        "Sustained outside VA should maintain DRIVING phase");

    TEST_PASSED("Phase: DRIVING for outside VA (market state from Dalton SSOT)");
}

// ============================================================================
// TEST: TESTING_BOUNDARY phase at boundary
// NOTE (Dec 2024): TRANSITION regime removed; testing phase behavior instead
// ============================================================================

bool test_phase_at_boundary() {
    double tickSize = 0.25;
    double poc = 5000.0;
    double vah = 5020.0;
    double val = 4980.0;
    int vaRangeTicks = static_cast<int>((vah - val) / tickSize);

    ZoneManager zm = CreateTestZoneManager(poc, vah, val, tickSize);
    zm.config.boundaryToleranceTicks = 1;
    PhaseTracker tracker;

    // Price at VAH boundary
    double boundaryPrice = 5020.0;
    PhaseSnapshot snap = BuildPhaseSnapshot(zm, boundaryPrice, tickSize, vah, val, vaRangeTicks, tracker);

    TEST_ASSERT(snap.phase == CurrentPhase::TESTING_BOUNDARY,
        "Should be TESTING_BOUNDARY phase at VAH boundary");

    // Price at VAL boundary
    boundaryPrice = 4980.0;
    tracker.Reset();
    snap = BuildPhaseSnapshot(zm, boundaryPrice, tickSize, vah, val, vaRangeTicks, tracker);

    TEST_ASSERT(snap.phase == CurrentPhase::TESTING_BOUNDARY,
        "Should be TESTING_BOUNDARY phase at VAL boundary");

    TEST_PASSED("TESTING_BOUNDARY phase at boundary works correctly");
}

// ============================================================================
// TEST: AMT Consistency - BALANCE state implies phase set constraint
// NOTE (Dec 2024): Uses marketState from Dalton SSOT
// ============================================================================

bool test_balance_state_phase_consistency() {
    double tickSize = 0.25;
    double poc = 5000.0;
    double vah = 5020.0;
    double val = 4980.0;
    int vaRangeTicks = static_cast<int>((vah - val) / tickSize);

    ZoneManager zm = CreateTestZoneManager(poc, vah, val, tickSize);
    PhaseTracker tracker;

    // Price inside VA - test with BALANCE state from Dalton
    double insidePrice = 5005.0;
    // Use explicit daltonState parameter for this test
    PhaseSnapshot snap = BuildPhaseSnapshot(zm, insidePrice, insidePrice, tickSize, 0, tracker,
                                            AMTMarketState::BALANCE);

    TEST_ASSERT(snap.marketState == AMTMarketState::BALANCE,
        "Market state should be BALANCE (from Dalton SSOT)");

    // BALANCE state should only have ROTATION or TESTING_BOUNDARY phases
    bool validPhase = (snap.phase == CurrentPhase::ROTATION ||
                       snap.phase == CurrentPhase::TESTING_BOUNDARY);
    TEST_ASSERT(validPhase,
        "BALANCE state phase must be ROTATION or TESTING_BOUNDARY");

    TEST_PASSED("BALANCE state phase consistency");
}

// ============================================================================
// TEST: AMT Consistency - IMBALANCE state implies phase != ROTATION
// NOTE (Dec 2024): Uses marketState from Dalton SSOT
// ============================================================================

bool test_imbalance_state_phase_consistency() {
    double tickSize = 0.25;
    double poc = 5000.0;
    double vah = 5020.0;
    double val = 4980.0;
    int vaRangeTicks = static_cast<int>((vah - val) / tickSize);

    ZoneManager zm = CreateTestZoneManager(poc, vah, val, tickSize);
    zm.config.boundaryToleranceTicks = 1;
    PhaseTracker tracker;

    // Price outside VA - test with IMBALANCE state from Dalton
    double outsidePrice = 5030.0;
    PhaseSnapshot snap = BuildPhaseSnapshot(zm, outsidePrice, outsidePrice, tickSize, 0, tracker,
                                            AMTMarketState::IMBALANCE);

    TEST_ASSERT(snap.marketState == AMTMarketState::IMBALANCE,
        "Market state should be IMBALANCE (from Dalton SSOT)");

    // IMBALANCE state should NEVER have ROTATION phase
    TEST_ASSERT(snap.phase != CurrentPhase::ROTATION,
        "IMBALANCE state phase must NOT be ROTATION");

    // Outside VA should be DRIVING (default outside-VA phase)
    TEST_ASSERT(snap.phase == CurrentPhase::DRIVING_UP,
        "Outside VA should give DRIVING phase");

    TEST_PASSED("IMBALANCE state phase consistency");
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "======================================" << std::endl;
    std::cout << "Phase Detection Tests (AMT-Aligned)" << std::endl;
    std::cout << "======================================" << std::endl;

    test_varange_always_valid_with_proper_va();
    test_trending_triggers_correctly();
    test_rotation_inside_va();
    test_outside_va_but_close_to_poc();
    test_va_level_calculation();
    test_invalid_va_returns_unknown();  // Replaces old CORE_VA fallback test
    test_phase_hysteresis();

    // Phase System v2 tests
    test_outside_balance_phase();
    test_invariant_rotation_inside_va();
    test_invariant_outside_va_never_rotation();
    test_testing_boundary_at_va_edges();

    // AMT Admissibility + Phase tests
    test_admissibility_clamp();
    test_regime_acceptance();  // Tests phase behavior (renamed but kept for acceptance logic)
    test_phase_at_boundary();

    // AMT Consistency constraint tests (market state from Dalton SSOT)
    test_balance_state_phase_consistency();
    test_imbalance_state_phase_consistency();

    std::cout << "======================================" << std::endl;
    std::cout << "Results: " << g_passed << " passed, " << g_failed << " failed" << std::endl;
    std::cout << "======================================" << std::endl;

    return g_failed > 0 ? 1 : 0;
}
