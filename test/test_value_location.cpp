// ============================================================================
// test_value_location.cpp - Unit Tests for ValueLocationEngine
// ============================================================================
// Tests:
//   1. Zone classification from price vs POC/VAH/VAL
//   2. VA overlap calculation and state classification
//   3. Hysteresis state machine (transition confirmation)
//   4. Reference level building and sorting
//   5. Strategy gating logic
//   6. Validity gating (warmup, errors)
//   7. Event detection (entry/exit/crossing)
//
// Compile: g++ -std=c++17 -I.. -o test_value_location.exe test_value_location.cpp
// Run: ./test_value_location.exe
// ============================================================================

#include <iostream>
#include <cassert>
#include <cmath>
#include <string>
#include <vector>

// Include proper Sierra Chart mocks for standalone testing
#include "test_sierrachart_mock.h"

#include "../amt_core.h"
#include "../AMT_Zones.h"
#include "../AMT_ValueLocation.h"

using namespace AMT;

// ============================================================================
// TEST UTILITIES
// ============================================================================

static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "[FAIL] " << msg << " (line " << __LINE__ << ")\n"; \
        g_testsFailed++; \
    } else { \
        g_testsPassed++; \
    } \
} while(0)

#define TEST_SECTION(name) std::cout << "\n=== " << name << " ===\n"

// Helper: Create a minimal StructureTracker with session/IB levels
StructureTracker CreateTestStructure(double sessHigh, double sessLow,
                                      double ibHigh, double ibLow, bool ibFrozen = true) {
    StructureTracker st;
    st.Reset();

    // Set session extremes using public interface
    st.UpdateExtremes(sessHigh, sessLow, 100);

    // Set IB levels - first start the IB window, then update, then optionally freeze
    if (ibHigh > 0.0 && ibLow > 0.0) {
        SCDateTime startTime;
        startTime.SetDateTime(2024, 1, 15, 9, 30, 0);  // 9:30 AM RTH start
        st.UpdateIB(ibHigh, ibLow, startTime, 1, true);  // isRTH = true, initializes IB

        if (ibFrozen) {
            // Simulate IB window closing by calling CheckIBFreeze with time 61 min later
            SCDateTime freezeTime;
            freezeTime.SetDateTime(2024, 1, 15, 10, 31, 0);  // 10:31 AM (61 min after start)
            st.CheckIBFreeze(freezeTime, 60);
        }
    }

    return st;
}

// Helper: Create a minimal ZoneManager (empty zones for basic tests)
ZoneManager CreateTestZoneManager() {
    ZoneManager zm;
    return zm;
}

// ============================================================================
// TEST: Zone Classification
// ============================================================================

void TestZoneClassification() {
    TEST_SECTION("Zone Classification");

    ValueLocationEngine engine;
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Test profile: POC=100, VAH=105, VAL=95 (10 tick VA width)
    const double poc = 100.0;
    const double vah = 105.0;
    const double val = 95.0;
    const double tickSize = 0.25;

    auto structure = CreateTestStructure(108.0, 92.0, 106.0, 94.0);
    auto zm = CreateTestZoneManager();

    // Test AT_POC (within 2 tick tolerance)
    {
        double price = 100.25;  // 1 tick from POC
        auto result = engine.Compute(price, tickSize, 1, poc, vah, val,
                                     0, 0, 0, structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(result.IsReady(), "AT_POC: Result should be ready");
        TEST_ASSERT(result.zone == ValueZone::AT_POC, "AT_POC: Should detect AT_POC zone");
        TEST_ASSERT(result.IsAtPOC(), "AT_POC: IsAtPOC() should be true");
        TEST_ASSERT(result.GetCoarseLocation() == ValueLocation::AT_POC, "AT_POC: Coarse location should match");
    }

    engine.ResetForSession();

    // Test AT_VAH (within 3 tick tolerance)
    {
        double price = 105.50;  // 2 ticks from VAH
        auto result = engine.Compute(price, tickSize, 1, poc, vah, val,
                                     0, 0, 0, structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(result.IsReady(), "AT_VAH: Result should be ready");
        TEST_ASSERT(result.zone == ValueZone::AT_VAH, "AT_VAH: Should detect AT_VAH zone");
        TEST_ASSERT(result.IsAtVAH(), "AT_VAH: IsAtVAH() should be true");
        TEST_ASSERT(result.GetCoarseLocation() == ValueLocation::AT_VAH, "AT_VAH: Coarse location should match");
    }

    engine.ResetForSession();

    // Test AT_VAL (within 3 tick tolerance)
    {
        double price = 94.50;  // 2 ticks from VAL
        auto result = engine.Compute(price, tickSize, 1, poc, vah, val,
                                     0, 0, 0, structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(result.IsReady(), "AT_VAL: Result should be ready");
        TEST_ASSERT(result.zone == ValueZone::AT_VAL, "AT_VAL: Should detect AT_VAL zone");
        TEST_ASSERT(result.IsAtVAL(), "AT_VAL: IsAtVAL() should be true");
        TEST_ASSERT(result.GetCoarseLocation() == ValueLocation::AT_VAL, "AT_VAL: Coarse location should match");
    }

    engine.ResetForSession();

    // Test UPPER_VALUE (between POC and VAH)
    {
        double price = 102.50;  // 10 ticks above POC, 10 ticks below VAH
        auto result = engine.Compute(price, tickSize, 1, poc, vah, val,
                                     0, 0, 0, structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(result.IsReady(), "UPPER_VALUE: Result should be ready");
        TEST_ASSERT(result.zone == ValueZone::UPPER_VALUE, "UPPER_VALUE: Should detect UPPER_VALUE zone");
        TEST_ASSERT(result.IsInsideValue(), "UPPER_VALUE: IsInsideValue() should be true");
        TEST_ASSERT(result.GetCoarseLocation() == ValueLocation::INSIDE_VALUE, "UPPER_VALUE: Coarse location should be INSIDE_VALUE");
    }

    engine.ResetForSession();

    // Test LOWER_VALUE (between VAL and POC)
    {
        double price = 97.50;  // 10 ticks below POC, 10 ticks above VAL
        auto result = engine.Compute(price, tickSize, 1, poc, vah, val,
                                     0, 0, 0, structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(result.IsReady(), "LOWER_VALUE: Result should be ready");
        TEST_ASSERT(result.zone == ValueZone::LOWER_VALUE, "LOWER_VALUE: Should detect LOWER_VALUE zone");
        TEST_ASSERT(result.IsInsideValue(), "LOWER_VALUE: IsInsideValue() should be true");
        TEST_ASSERT(result.GetCoarseLocation() == ValueLocation::INSIDE_VALUE, "LOWER_VALUE: Coarse location should be INSIDE_VALUE");
    }

    engine.ResetForSession();

    // Test NEAR_ABOVE_VALUE (above VAH but within extension threshold)
    {
        double price = 106.50;  // 6 ticks above VAH (< 8 tick extension threshold)
        auto result = engine.Compute(price, tickSize, 1, poc, vah, val,
                                     0, 0, 0, structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(result.IsReady(), "NEAR_ABOVE: Result should be ready");
        TEST_ASSERT(result.zone == ValueZone::NEAR_ABOVE_VALUE, "NEAR_ABOVE: Should detect NEAR_ABOVE_VALUE zone");
        TEST_ASSERT(result.IsAboveValue(), "NEAR_ABOVE: IsAboveValue() should be true");
        TEST_ASSERT(result.GetCoarseLocation() == ValueLocation::ABOVE_VALUE, "NEAR_ABOVE: Coarse location should be ABOVE_VALUE");
    }

    engine.ResetForSession();

    // Test FAR_ABOVE_VALUE (way above VAH)
    {
        double price = 110.0;  // 20 ticks above VAH (> 8 tick extension threshold)
        auto result = engine.Compute(price, tickSize, 1, poc, vah, val,
                                     0, 0, 0, structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(result.IsReady(), "FAR_ABOVE: Result should be ready");
        TEST_ASSERT(result.zone == ValueZone::FAR_ABOVE_VALUE, "FAR_ABOVE: Should detect FAR_ABOVE_VALUE zone");
        TEST_ASSERT(result.IsAboveValue(), "FAR_ABOVE: IsAboveValue() should be true");
        TEST_ASSERT(result.IsFarOutside(), "FAR_ABOVE: IsFarOutside() should be true");
        TEST_ASSERT(result.GetCoarseLocation() == ValueLocation::ABOVE_VALUE, "FAR_ABOVE: Coarse location should be ABOVE_VALUE");
    }

    engine.ResetForSession();

    // Test FAR_BELOW_VALUE (way below VAL)
    {
        double price = 90.0;  // 20 ticks below VAL (> 8 tick extension threshold)
        auto result = engine.Compute(price, tickSize, 1, poc, vah, val,
                                     0, 0, 0, structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(result.IsReady(), "FAR_BELOW: Result should be ready");
        TEST_ASSERT(result.zone == ValueZone::FAR_BELOW_VALUE, "FAR_BELOW: Should detect FAR_BELOW_VALUE zone");
        TEST_ASSERT(result.IsBelowValue(), "FAR_BELOW: IsBelowValue() should be true");
        TEST_ASSERT(result.IsFarOutside(), "FAR_BELOW: IsFarOutside() should be true");
        TEST_ASSERT(result.GetCoarseLocation() == ValueLocation::BELOW_VALUE, "FAR_BELOW: Coarse location should be BELOW_VALUE");
    }

    std::cout << "Zone classification tests complete\n";
}

// ============================================================================
// TEST: VA Percentile Calculation
// ============================================================================

void TestVAPercentile() {
    TEST_SECTION("VA Percentile Calculation");

    ValueLocationEngine engine;
    engine.SetPhase(SessionPhase::MID_SESSION);

    const double poc = 100.0;
    const double vah = 110.0;
    const double val = 90.0;  // 20 point VA width
    const double tickSize = 0.25;

    auto structure = CreateTestStructure(115.0, 85.0, 112.0, 88.0);
    auto zm = CreateTestZoneManager();

    // At VAL = 0%
    {
        auto result = engine.Compute(90.0, tickSize, 1, poc, vah, val,
                                     0, 0, 0, structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(result.vaPercentileValid, "VAL: Percentile should be valid");
        TEST_ASSERT(std::abs(result.vaPercentile - 0.0) < 1.0, "VAL: Percentile should be ~0%");
    }

    engine.ResetForSession();

    // At midpoint = 50%
    {
        auto result = engine.Compute(100.0, tickSize, 1, poc, vah, val,
                                     0, 0, 0, structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(result.vaPercentileValid, "MID: Percentile should be valid");
        TEST_ASSERT(std::abs(result.vaPercentile - 50.0) < 1.0, "MID: Percentile should be ~50%");
    }

    engine.ResetForSession();

    // At VAH = 100%
    {
        auto result = engine.Compute(110.0, tickSize, 1, poc, vah, val,
                                     0, 0, 0, structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(result.vaPercentileValid, "VAH: Percentile should be valid");
        TEST_ASSERT(std::abs(result.vaPercentile - 100.0) < 1.0, "VAH: Percentile should be ~100%");
    }

    std::cout << "VA percentile tests complete\n";
}

// ============================================================================
// TEST: VA Overlap Calculation
// ============================================================================

void TestVAOverlap() {
    TEST_SECTION("VA Overlap Calculation");

    ValueLocationEngine engine;
    engine.SetPhase(SessionPhase::MID_SESSION);

    const double tickSize = 0.25;
    auto structure = CreateTestStructure(115.0, 85.0, 112.0, 88.0);
    auto zm = CreateTestZoneManager();

    // Test OVERLAPPING (>50% overlap)
    {
        // Current: VAH=110, VAL=90 (20 point width)
        // Prior: VAH=108, VAL=92 (16 point width)
        // Overlap: 108-92 = 16 points / min(20,16) = 100%
        auto result = engine.Compute(100.0, tickSize, 1,
                                     100.0, 110.0, 90.0,   // current POC/VAH/VAL
                                     100.0, 108.0, 92.0,   // prior POC/VAH/VAL
                                     structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(result.IsReady(), "OVERLAP: Result should be ready");
        TEST_ASSERT(result.overlapState == VAOverlapState::OVERLAPPING ||
                    result.overlapState == VAOverlapState::CONTAINED,
                    "OVERLAP: Should detect overlapping/contained state");
        TEST_ASSERT(result.vaOverlapPct > 0.5, "OVERLAP: Overlap % should be > 50%");
    }

    engine.ResetForSession();

    // Test SEPARATED_ABOVE (<30% overlap, current above prior)
    {
        // Current: VAH=120, VAL=110 (10 point width)
        // Prior: VAH=100, VAL=90 (10 point width)
        // Overlap: 0 points (no overlap)
        auto result = engine.Compute(115.0, tickSize, 1,
                                     115.0, 120.0, 110.0,   // current POC/VAH/VAL
                                     95.0, 100.0, 90.0,     // prior POC/VAH/VAL
                                     structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(result.IsReady(), "SEP_ABOVE: Result should be ready");
        TEST_ASSERT(result.overlapState == VAOverlapState::SEPARATED_ABOVE,
                    "SEP_ABOVE: Should detect separated above state");
        TEST_ASSERT(result.vaOverlapPct < 0.3, "SEP_ABOVE: Overlap % should be < 30%");
    }

    engine.ResetForSession();

    // Test SEPARATED_BELOW (<30% overlap, current below prior)
    {
        // Current: VAH=90, VAL=80 (10 point width)
        // Prior: VAH=110, VAL=100 (10 point width)
        // Overlap: 0 points (no overlap)
        auto result = engine.Compute(85.0, tickSize, 1,
                                     85.0, 90.0, 80.0,      // current POC/VAH/VAL
                                     105.0, 110.0, 100.0,   // prior POC/VAH/VAL
                                     structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(result.IsReady(), "SEP_BELOW: Result should be ready");
        TEST_ASSERT(result.overlapState == VAOverlapState::SEPARATED_BELOW,
                    "SEP_BELOW: Should detect separated below state");
    }

    engine.ResetForSession();

    // Test CONTAINED (current VA inside prior VA)
    {
        // Current: VAH=105, VAL=95 (10 point width)
        // Prior: VAH=110, VAL=90 (20 point width)
        auto result = engine.Compute(100.0, tickSize, 1,
                                     100.0, 105.0, 95.0,    // current POC/VAH/VAL
                                     100.0, 110.0, 90.0,    // prior POC/VAH/VAL
                                     structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(result.IsReady(), "CONTAINED: Result should be ready");
        TEST_ASSERT(result.overlapState == VAOverlapState::CONTAINED,
                    "CONTAINED: Should detect contained state");
        TEST_ASSERT(result.isVAContracting, "CONTAINED: Should detect VA contracting");
    }

    std::cout << "VA overlap tests complete\n";
}

// ============================================================================
// TEST: Hysteresis State Machine
// ============================================================================

void TestHysteresis() {
    TEST_SECTION("Hysteresis State Machine");

    ValueLocationEngine engine;
    engine.SetPhase(SessionPhase::MID_SESSION);

    const double poc = 100.0;
    const double vah = 105.0;
    const double val = 95.0;
    const double tickSize = 0.25;

    auto structure = CreateTestStructure(108.0, 92.0, 106.0, 94.0);
    auto zm = CreateTestZoneManager();

    // Start at POC
    auto r1 = engine.Compute(100.0, tickSize, 1, poc, vah, val,
                             0, 0, 0, structure, zm, nullptr, nullptr,
                             AMTMarketState::BALANCE);
    TEST_ASSERT(r1.zone == ValueZone::AT_POC, "Bar 1: Should be AT_POC");
    TEST_ASSERT(r1.confirmedZone == ValueZone::UNKNOWN, "Bar 1: Confirmed should still be UNKNOWN (first bar)");

    // Stay at POC - should confirm
    auto r2 = engine.Compute(100.25, tickSize, 2, poc, vah, val,
                             0, 0, 0, structure, zm, nullptr, nullptr,
                             AMTMarketState::BALANCE);
    TEST_ASSERT(r2.zone == ValueZone::AT_POC, "Bar 2: Should be AT_POC");
    // After 2 bars, should confirm (minConfirmationBars = 2)
    TEST_ASSERT(r2.confirmedZone == ValueZone::AT_POC, "Bar 2: Should confirm AT_POC");

    // Move to UPPER_VALUE - should be transitioning
    auto r3 = engine.Compute(102.5, tickSize, 3, poc, vah, val,
                             0, 0, 0, structure, zm, nullptr, nullptr,
                             AMTMarketState::BALANCE);
    TEST_ASSERT(r3.zone == ValueZone::UPPER_VALUE, "Bar 3: Raw zone should be UPPER_VALUE");
    TEST_ASSERT(r3.isTransitioning, "Bar 3: Should be transitioning");
    TEST_ASSERT(r3.confirmedZone == ValueZone::AT_POC, "Bar 3: Confirmed should still be AT_POC");

    // Stay at UPPER_VALUE - should confirm transition
    auto r4 = engine.Compute(102.75, tickSize, 4, poc, vah, val,
                             0, 0, 0, structure, zm, nullptr, nullptr,
                             AMTMarketState::BALANCE);
    TEST_ASSERT(r4.confirmedZone == ValueZone::UPPER_VALUE, "Bar 4: Should confirm UPPER_VALUE");
    TEST_ASSERT(r4.zoneChanged, "Bar 4: Should signal zone changed");

    std::cout << "Hysteresis tests complete\n";
}

// ============================================================================
// TEST: Reference Level Building
// ============================================================================

void TestReferenceLevels() {
    TEST_SECTION("Reference Level Building");

    ValueLocationEngine engine;
    engine.SetPhase(SessionPhase::MID_SESSION);

    const double poc = 100.0;
    const double vah = 105.0;
    const double val = 95.0;
    const double tickSize = 0.25;

    // Prior session levels
    const double priorPOC = 98.0;
    const double priorVAH = 103.0;
    const double priorVAL = 93.0;

    auto structure = CreateTestStructure(108.0, 92.0, 106.0, 94.0);
    auto zm = CreateTestZoneManager();

    // HVN/LVN levels
    std::vector<double> hvnLevels = {100.0, 102.0};  // HVNs at POC and 102
    std::vector<double> lvnLevels = {97.0};          // LVN at 97

    // Price at 100 (AT_POC)
    auto result = engine.Compute(100.0, tickSize, 1, poc, vah, val,
                                 priorPOC, priorVAH, priorVAL,
                                 structure, zm, &hvnLevels, &lvnLevels,
                                 AMTMarketState::BALANCE);

    TEST_ASSERT(result.IsReady(), "REF: Result should be ready");
    TEST_ASSERT(!result.nearbyLevels.empty(), "REF: Should have nearby levels");

    // Should detect at HVN (100 is in hvnLevels)
    TEST_ASSERT(result.atHVN, "REF: Should be at HVN");
    TEST_ASSERT(!result.atLVN, "REF: Should not be at LVN");

    // Should have multiple levels within range
    TEST_ASSERT(result.levelsWithin5Ticks >= 1, "REF: Should have levels within 5 ticks");
    TEST_ASSERT(result.levelsWithin10Ticks >= 2, "REF: Should have levels within 10 ticks");

    // Check distances to structure levels
    TEST_ASSERT(result.distToSessionHighTicks < 0, "REF: Should be below session high (negative distance)");
    TEST_ASSERT(result.distToSessionLowTicks > 0, "REF: Should be above session low (positive distance)");

    // Prior levels should be populated
    TEST_ASSERT(std::abs(result.distToPriorPOCTicks - 8.0) < 1.0, "REF: Distance to prior POC should be ~8 ticks");

    std::cout << "Reference level tests complete\n";
}

// ============================================================================
// TEST: Strategy Gating
// ============================================================================

void TestStrategyGating() {
    TEST_SECTION("Strategy Gating");

    ValueLocationEngine engine;
    engine.SetPhase(SessionPhase::MID_SESSION);

    const double poc = 100.0;
    const double vah = 105.0;
    const double val = 95.0;
    const double tickSize = 0.25;

    auto structure = CreateTestStructure(108.0, 92.0, 106.0, 94.0);
    auto zm = CreateTestZoneManager();

    // Test AT_POC gating (neutral zone)
    {
        auto result = engine.Compute(100.0, tickSize, 1, poc, vah, val,
                                     100.0, 105.0, 95.0,  // Overlapping prior
                                     structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(result.gating.isNeutralZone, "POC: Should be neutral zone");
        TEST_ASSERT(result.gating.fadeConfidenceMultiplier < 1.0, "POC: Fade mult should be reduced");
        TEST_ASSERT(result.gating.breakoutConfidenceMultiplier < 1.0, "POC: Breakout mult should be reduced");
    }

    engine.ResetForSession();

    // Test AT_VAH in BALANCE (fade favorable)
    {
        auto result = engine.Compute(105.0, tickSize, 1, poc, vah, val,
                                     100.0, 105.0, 95.0,  // Overlapping prior
                                     structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(result.gating.fadeConfidenceMultiplier >= 1.0, "VAH+BAL: Fade mult should be boosted");
        TEST_ASSERT(result.gating.preferShortSide, "VAH+BAL: Should prefer short side");
    }

    engine.ResetForSession();

    // Test FAR_ABOVE in IMBALANCE (trend favorable)
    {
        auto result = engine.Compute(112.0, tickSize, 1, poc, vah, val,
                                     100.0, 105.0, 95.0,
                                     structure, zm, nullptr, nullptr,
                                     AMTMarketState::IMBALANCE);
        TEST_ASSERT(!result.gating.allowMeanReversion, "FAR+IMB: Should not allow mean reversion");
        TEST_ASSERT(result.gating.allowTrend, "FAR+IMB: Should allow trend following");
        TEST_ASSERT(result.gating.preferLongSide, "FAR+IMB: Should prefer long side (above value)");
    }

    std::cout << "Strategy gating tests complete\n";
}

// ============================================================================
// TEST: Event Detection
// ============================================================================

void TestEventDetection() {
    TEST_SECTION("Event Detection");

    ValueLocationEngine engine;
    engine.SetPhase(SessionPhase::MID_SESSION);

    const double poc = 100.0;
    const double vah = 105.0;
    const double val = 95.0;
    const double tickSize = 0.25;

    auto structure = CreateTestStructure(108.0, 92.0, 106.0, 94.0);
    auto zm = CreateTestZoneManager();

    // Start inside value
    auto r1 = engine.Compute(100.0, tickSize, 1, poc, vah, val,
                             0, 0, 0, structure, zm, nullptr, nullptr,
                             AMTMarketState::BALANCE);
    TEST_ASSERT(!r1.enteredValue, "Bar 1: Should not trigger enteredValue (first bar)");
    TEST_ASSERT(!r1.exitedValue, "Bar 1: Should not trigger exitedValue");

    // Move above value (exit)
    auto r2 = engine.Compute(107.0, tickSize, 2, poc, vah, val,
                             0, 0, 0, structure, zm, nullptr, nullptr,
                             AMTMarketState::BALANCE);
    TEST_ASSERT(r2.exitedValue, "Bar 2: Should trigger exitedValue");
    TEST_ASSERT(!r2.enteredValue, "Bar 2: Should not trigger enteredValue");

    // Move back inside (entry)
    auto r3 = engine.Compute(102.0, tickSize, 3, poc, vah, val,
                             0, 0, 0, structure, zm, nullptr, nullptr,
                             AMTMarketState::BALANCE);
    TEST_ASSERT(r3.enteredValue, "Bar 3: Should trigger enteredValue");
    TEST_ASSERT(!r3.exitedValue, "Bar 3: Should not trigger exitedValue");

    // Cross POC (from above to below)
    auto r4 = engine.Compute(98.0, tickSize, 4, poc, vah, val,
                             0, 0, 0, structure, zm, nullptr, nullptr,
                             AMTMarketState::BALANCE);
    TEST_ASSERT(r4.crossedPOC, "Bar 4: Should trigger crossedPOC");

    std::cout << "Event detection tests complete\n";
}

// ============================================================================
// TEST: Validity Gating (Error Handling)
// ============================================================================

void TestValidityGating() {
    TEST_SECTION("Validity Gating");

    ValueLocationEngine engine;
    engine.SetPhase(SessionPhase::MID_SESSION);

    auto structure = CreateTestStructure(108.0, 92.0, 106.0, 94.0);
    auto zm = CreateTestZoneManager();

    // Test invalid price (zero)
    {
        auto result = engine.Compute(0.0, 0.25, 1, 100.0, 105.0, 95.0,
                                     0, 0, 0, structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(!result.IsReady(), "ZERO_PRICE: Should not be ready");
        TEST_ASSERT(result.errorReason == ValueLocationErrorReason::ERR_INVALID_PRICE,
                    "ZERO_PRICE: Should have ERR_INVALID_PRICE");
    }

    // Test invalid tick size
    {
        auto result = engine.Compute(100.0, 0.0, 1, 100.0, 105.0, 95.0,
                                     0, 0, 0, structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(!result.IsReady(), "ZERO_TICK: Should not be ready");
        TEST_ASSERT(result.errorReason == ValueLocationErrorReason::ERR_INVALID_TICK,
                    "ZERO_TICK: Should have ERR_INVALID_TICK");
    }

    // Test invalid VA (VAH <= VAL)
    {
        auto result = engine.Compute(100.0, 0.25, 1, 100.0, 95.0, 105.0,  // Inverted
                                     0, 0, 0, structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(!result.IsReady(), "INVERTED_VA: Should not be ready");
        TEST_ASSERT(result.errorReason == ValueLocationErrorReason::ERR_INVALID_VA,
                    "INVERTED_VA: Should have ERR_INVALID_VA");
    }

    // Test valid case
    {
        auto result = engine.Compute(100.0, 0.25, 1, 100.0, 105.0, 95.0,
                                     0, 0, 0, structure, zm, nullptr, nullptr,
                                     AMTMarketState::BALANCE);
        TEST_ASSERT(result.IsReady(), "VALID: Should be ready");
        TEST_ASSERT(result.errorReason == ValueLocationErrorReason::NONE,
                    "VALID: Should have no error");
    }

    std::cout << "Validity gating tests complete\n";
}

// ============================================================================
// TEST: Log Formatting
// ============================================================================

void TestLogFormatting() {
    TEST_SECTION("Log Formatting");

    ValueLocationEngine engine;
    engine.SetPhase(SessionPhase::MID_SESSION);

    auto structure = CreateTestStructure(108.0, 92.0, 106.0, 94.0);
    auto zm = CreateTestZoneManager();

    auto result = engine.Compute(102.0, 0.25, 1, 100.0, 105.0, 95.0,
                                 98.0, 103.0, 93.0,
                                 structure, zm, nullptr, nullptr,
                                 AMTMarketState::BALANCE);

    // Test log formatting doesn't crash
    std::string mainLog = result.FormatForLog();
    std::string structLog = result.FormatStructureForLog();
    std::string sessLog = result.FormatSessionForLog();
    std::string refLog = result.FormatReferencesForLog();
    std::string gateLog = result.FormatGatingForLog();

    TEST_ASSERT(!mainLog.empty(), "Main log should not be empty");
    TEST_ASSERT(!structLog.empty(), "Structure log should not be empty");
    TEST_ASSERT(!sessLog.empty(), "Session log should not be empty");
    TEST_ASSERT(!refLog.empty(), "Reference log should not be empty");
    TEST_ASSERT(!gateLog.empty(), "Gating log should not be empty");

    // Print sample output
    std::cout << "Sample log output:\n";
    std::cout << "  [VAL-LOC] " << mainLog << "\n";
    std::cout << "  [VAL-STRUCT] " << structLog << "\n";
    std::cout << "  [VAL-SESS] " << sessLog << "\n";
    std::cout << "  [VAL-REF] " << refLog << "\n";
    std::cout << "  [VAL-GATE] " << gateLog << "\n";

    std::cout << "Log formatting tests complete\n";
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "========================================\n";
    std::cout << "ValueLocationEngine Unit Tests\n";
    std::cout << "========================================\n";

    TestZoneClassification();
    TestVAPercentile();
    TestVAOverlap();
    TestHysteresis();
    TestReferenceLevels();
    TestStrategyGating();
    TestEventDetection();
    TestValidityGating();
    TestLogFormatting();

    std::cout << "\n========================================\n";
    std::cout << "RESULTS: " << g_testsPassed << " passed, " << g_testsFailed << " failed\n";
    std::cout << "========================================\n";

    return g_testsFailed > 0 ? 1 : 0;
}
