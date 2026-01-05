// ============================================================================
// test_arbitration_ladder.cpp
// Implementation-true tests for M0 Arbitration Ladder
// Compiles standalone - no Sierra dependencies
// ============================================================================

#include <iostream>
#include <cassert>

// Include ONLY the seam (which includes amt_core.h for enums)
#include "../AMT_Arbitration_Seam.h"

using namespace AMT_Arb;

// ============================================================================
// TEST MACRO (simple, no framework needed)
// ============================================================================

static int g_testCount = 0;
static int g_passCount = 0;

#define TEST(name) \
    void test_##name(); \
    struct test_##name##_runner { \
        test_##name##_runner() { \
            g_testCount++; \
            std::cout << "TEST: " #name "... "; \
            try { test_##name(); g_passCount++; std::cout << "PASS\n"; } \
            catch (...) { std::cout << "FAIL (exception)\n"; } \
        } \
    } test_##name##_instance; \
    void test_##name()

// ============================================================================
// HELPER: Create valid baseline input
// ============================================================================

ArbitrationInput MakeValidInput() {
    ArbitrationInput in;
    in.pocId = 1;
    in.vahId = 2;
    in.valId = 3;
    in.pocValid = true;
    in.vahValid = true;
    in.valValid = true;
    in.zonesInitialized = true;
    in.vbpPoc = 5000.0;
    in.vbpVah = 5010.0;
    in.vbpVal = 4990.0;
    in.barsSinceLastCompute = 0;
    in.isDirectional = false;
    in.deltaConsistency = 0.5;
    in.deltaConsistencyValid = true;  // Default: bar has sufficient volume
    return in;
}

// ============================================================================
// INVALIDITY GATES (1-6)
// ============================================================================

TEST(Gate0_NegativePocId) {
    ArbitrationInput in = MakeValidInput();
    in.pocId = -1;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.arbReason == ARB_INVALID_ANCHOR_IDS);
    assert(out.useZones == false);
    assert(out.engagedZoneId == -1);
}

TEST(Gate0_NegativeVahId) {
    ArbitrationInput in = MakeValidInput();
    in.vahId = -1;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.arbReason == ARB_INVALID_ANCHOR_IDS);
}

TEST(Gate1_PocPtrNull) {
    ArbitrationInput in = MakeValidInput();
    in.pocValid = false;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.arbReason == ARB_INVALID_ZONE_PTRS);
}

TEST(Gate2_NotInitialized) {
    ArbitrationInput in = MakeValidInput();
    in.zonesInitialized = false;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.arbReason == ARB_NOT_READY);
}

TEST(Gate3_VbpPocZero) {
    ArbitrationInput in = MakeValidInput();
    in.vbpPoc = 0.0;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.arbReason == ARB_INVALID_VBP_PRICES);
}

TEST(Gate4_VahLeVal) {
    ArbitrationInput in = MakeValidInput();
    in.vbpVah = 4990.0;  // VAH < VAL
    in.vbpVal = 5010.0;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.arbReason == ARB_INVALID_VA_ORDER);
}

TEST(Gate5_Stale50) {
    ArbitrationInput in = MakeValidInput();
    in.barsSinceLastCompute = 50;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.arbReason == ARB_VBP_STALE);
}

// ============================================================================
// PRECEDENCE
// ============================================================================

TEST(Gate0_BeforeGate6) {
    ArbitrationInput in = MakeValidInput();
    in.pocId = -1;  // Gate 0 trigger
    in.vahProximity = AMT::ZoneProximity::AT_ZONE;  // Would trigger Gate 6
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.arbReason == ARB_INVALID_ANCHOR_IDS);  // Gate 0 wins
}

TEST(Gate1_BeforeGate6) {
    ArbitrationInput in = MakeValidInput();
    in.pocValid = false;  // Gate 1 trigger
    in.vahProximity = AMT::ZoneProximity::AT_ZONE;  // Would trigger Gate 6
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.arbReason == ARB_INVALID_ZONE_PTRS);  // Gate 1 wins
}

// ============================================================================
// ENGAGED (Gate 6)
// ============================================================================

TEST(Engaged_PocAtZone) {
    ArbitrationInput in = MakeValidInput();
    in.pocProximity = AMT::ZoneProximity::AT_ZONE;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.arbReason == ARB_ENGAGED);
    assert(out.useZones == true);
    assert(out.engagedZoneId == 1);  // pocId
    assert(out.pocProx == 2);  // AT_ZONE
}

TEST(Engaged_Priority_PocWins) {
    ArbitrationInput in = MakeValidInput();
    in.pocId = 101; in.vahId = 102; in.valId = 103;
    in.pocProximity = AMT::ZoneProximity::AT_ZONE;
    in.vahProximity = AMT::ZoneProximity::AT_ZONE;
    in.valProximity = AMT::ZoneProximity::AT_ZONE;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.engagedZoneId == 101);  // POC wins
}

TEST(Engaged_VahWinsIfPocNotAtZone) {
    ArbitrationInput in = MakeValidInput();
    in.pocId = 101; in.vahId = 102; in.valId = 103;
    in.pocProximity = AMT::ZoneProximity::APPROACHING;
    in.vahProximity = AMT::ZoneProximity::AT_ZONE;
    in.valProximity = AMT::ZoneProximity::AT_ZONE;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.engagedZoneId == 102);  // VAH wins
}

TEST(Engaged_ValWinsIfOthersNotAtZone) {
    ArbitrationInput in = MakeValidInput();
    in.pocId = 101; in.vahId = 102; in.valId = 103;
    in.pocProximity = AMT::ZoneProximity::INACTIVE;
    in.vahProximity = AMT::ZoneProximity::APPROACHING;
    in.valProximity = AMT::ZoneProximity::AT_ZONE;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.engagedZoneId == 103);  // VAL wins
}

// ============================================================================
// DIRECTIONAL (Gate 7)
// ============================================================================

TEST(Directional_True) {
    ArbitrationInput in = MakeValidInput();
    in.isDirectional = true;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.arbReason == ARB_DIRECTIONAL);
    assert(out.useZones == true);
    assert(out.engagedZoneId == -1);  // Not engaged
    assert(out.rawState == AMT::AMTMarketState::IMBALANCE);
}

TEST(Directional_EngagedTakesPrecedence) {
    ArbitrationInput in = MakeValidInput();
    in.isDirectional = true;
    in.pocProximity = AMT::ZoneProximity::AT_ZONE;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.arbReason == ARB_ENGAGED);  // Gate 6 before Gate 7
}

// ============================================================================
// BASELINE (Gates 8-9)
// ============================================================================

TEST(BaselineExtreme_HighDelta) {
    ArbitrationInput in = MakeValidInput();
    in.deltaConsistency = 0.75;  // isExtremeDeltaBar = true (>0.7)
    in.sessionDeltaValid = true;  // Enable session validation
    in.sessionDeltaPctile = 90.0;  // isExtremeDeltaSession = true (>=85)
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.arbReason == ARB_BASELINE_EXTREME);
    assert(out.isExtremeDelta == true);  // Requires BOTH bar and session extremity
    assert(out.useZones == false);
    assert(out.rawState == AMT::AMTMarketState::IMBALANCE);
}

// SEMANTIC FIX (Dec 2024): deltaConsistency is now aggressor FRACTION [0,1] where 0.5=neutral
// 0.25 = 25% at ask = 75% at bid = EXTREME SELLING (< 0.3 threshold)
TEST(ExtremeSelling_LowFraction) {
    ArbitrationInput in = MakeValidInput();
    in.deltaConsistency = 0.25;  // 75% selling (< 0.3 threshold)
    in.sessionDeltaValid = true;
    in.sessionDeltaPctile = 90.0;  // Session also extreme
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.isExtremeDeltaBar == true);   // < 0.3 = extreme selling
    assert(out.isExtremeDelta == true);      // Bar AND session extreme
    assert(out.rawState == AMT::AMTMarketState::IMBALANCE);
}

TEST(DefaultBaseline_NeutralDelta) {
    ArbitrationInput in = MakeValidInput();
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.arbReason == ARB_DEFAULT_BASELINE);
    assert(out.useZones == false);
    assert(out.rawState == AMT::AMTMarketState::BALANCE);
}

// ============================================================================
// BOUNDARY CONDITIONS
// ============================================================================

TEST(Stale_Boundary_49NotStale) {
    ArbitrationInput in = MakeValidInput();
    in.barsSinceLastCompute = 49;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.arbReason != ARB_VBP_STALE);
}

TEST(Stale_Boundary_50IsStale) {
    ArbitrationInput in = MakeValidInput();
    in.barsSinceLastCompute = 50;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.arbReason == ARB_VBP_STALE);
}

TEST(VaOrder_EqualIsInvalid) {
    ArbitrationInput in = MakeValidInput();
    in.vbpVah = 5000.0;
    in.vbpVal = 5000.0;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.arbReason == ARB_INVALID_VA_ORDER);
}

TEST(Extreme_Boundary_0_7_NotExtreme) {
    ArbitrationInput in = MakeValidInput();
    in.deltaConsistency = 0.7;  // Exactly 0.7
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.isExtremeDelta == false);  // > 0.7 required
}

// SEMANTIC FIX (Dec 2024): 0.1 = 10% at ask = 90% at bid = EXTREME SELLING
// But isExtremeDelta requires BOTH bar AND session extreme
TEST(ExtremeSelling_VeryLowFraction_NoSessionValid) {
    ArbitrationInput in = MakeValidInput();
    in.deltaConsistency = 0.1;  // 90% selling - isExtremeDeltaBar = true
    in.deltaConsistencyValid = true;  // Bar has sufficient volume
    in.sessionDeltaValid = false;  // Session not validated yet
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.isExtremeDeltaBar == true);   // < 0.3 = extreme selling
    assert(out.isExtremeDelta == false);     // Session validation required for combined flag
}

// VALIDITY GATE: Thin bar (insufficient volume) cannot trigger extreme
TEST(ThinBar_NoExtreme) {
    ArbitrationInput in = MakeValidInput();
    in.deltaConsistency = 0.1;  // Would be extreme if valid
    in.deltaConsistencyValid = false;  // But thin bar - invalid
    in.sessionDeltaValid = true;
    in.sessionDeltaPctile = 90.0;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.isExtremeDeltaBar == false);  // Cannot detect extreme from invalid data
    assert(out.isExtremeDelta == false);
    // NOTE: directionalCoherence removed from ArbitrationResult (Jan 2025)
    // Activity classification (Initiative/Responsive) is now SSOT from AMT_Signals.h
}

// ============================================================================
// RAWSTATE INVARIANT
// ============================================================================

TEST(RawState_DirectionalAlone) {
    ArbitrationInput in = MakeValidInput();
    in.isDirectional = true;
    in.deltaConsistency = 0.5;  // Not extreme
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.rawState == AMT::AMTMarketState::IMBALANCE);
}

TEST(RawState_ExtremeAlone) {
    ArbitrationInput in = MakeValidInput();
    in.isDirectional = false;
    in.deltaConsistency = 0.8;  // isExtremeDeltaBar = true (>0.7)
    in.sessionDeltaValid = true;  // Enable session validation
    in.sessionDeltaPctile = 90.0;  // isExtremeDeltaSession = true (>=85)
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.rawState == AMT::AMTMarketState::IMBALANCE);
}

TEST(RawState_Both) {
    ArbitrationInput in = MakeValidInput();
    in.isDirectional = true;
    in.deltaConsistency = 0.8;  // isExtremeDeltaBar = true (>0.7)
    in.sessionDeltaValid = true;  // Enable session validation
    in.sessionDeltaPctile = 90.0;  // isExtremeDeltaSession = true (>=85)
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.rawState == AMT::AMTMarketState::IMBALANCE);
}

TEST(RawState_Neither) {
    ArbitrationInput in = MakeValidInput();
    in.isDirectional = false;
    in.deltaConsistency = 0.5;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.rawState == AMT::AMTMarketState::BALANCE);
}

// ============================================================================
// POCPROX DERIVATION
// ============================================================================

TEST(PocProx_ValidZone) {
    ArbitrationInput in = MakeValidInput();
    in.pocProximity = AMT::ZoneProximity::APPROACHING;
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.pocProx == 1);  // APPROACHING = 1
}

TEST(PocProx_InvalidZone) {
    ArbitrationInput in = MakeValidInput();
    in.pocValid = false;  // Will fail at Gate 1
    ArbitrationResult out = EvaluateArbitrationLadder(in);
    assert(out.pocProx == -1);  // Invalid
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "\n========================================\n";
    std::cout << "M0 ARBITRATION LADDER TESTS\n";
    std::cout << "Implementation-True (No Sierra Dependencies)\n";
    std::cout << "========================================\n\n";

    // Tests run via static initialization

    std::cout << "\n========================================\n";
    std::cout << "RESULTS: " << g_passCount << "/" << g_testCount << " PASSED\n";
    std::cout << "========================================\n";

    return (g_passCount == g_testCount) ? 0 : 1;
}
