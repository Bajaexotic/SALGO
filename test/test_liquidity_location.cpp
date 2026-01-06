// ============================================================================
// Test: LiquidityEngine Location-Aware Behavior
// ============================================================================
// Tests the LiquidityLocationContext and location-aware compute methods
// added to LiquidityEngine for AMT value-relative awareness.
//
// Scenarios:
//   1. Location context at VAH (atValueEdge = true)
//   2. Location context inside value (2TF, rotationExpected = true)
//   3. Location context outside value (1TF, stressContextMultiplier < 1.0)
//   4. Computation gating skip (deep in rotation)
//   5. Spatial profile wall significance adjustment
// ============================================================================

#include <iostream>
#include <cmath>
#include "test_sierrachart_mock.h"
#include "../amt_core.h"
#include "../AMT_Liquidity.h"

static int g_passed = 0;
static int g_failed = 0;

void check(bool condition, const char* testName) {
    if (condition) {
        std::cout << "[PASS] " << testName << "\n";
        g_passed++;
    } else {
        std::cout << "[FAIL] " << testName << "\n";
        g_failed++;
    }
}

// Helper to create a valid ValueLocationResult for testing
// NOTE: zone is SSOT - helpers (IsInsideValue, etc.) derive all state from zone
AMT::ValueLocationResult createMockValueLocationResult(
    AMT::ValueZone zone,
    double distPOC = 0.0, double distVAH = 0.0, double distVAL = 0.0)
{
    AMT::ValueLocationResult result;
    result.confirmedZone = zone;
    result.zone = zone;  // SSOT for location classification
    result.distFromPOCTicks = distPOC;
    result.distFromVAHTicks = distVAH;
    result.distFromVALTicks = distVAL;
    result.errorReason = AMT::ValueLocationErrorReason::NONE;  // Makes IsReady() true
    // NOTE: location field is deprecated - helpers use zone (SSOT) only
    return result;
}

// ============================================================================
// Test: LiquidityLocationContext Build and Helpers
// ============================================================================
void testLocationContextBuild() {
    std::cout << "\n--- Test: LiquidityLocationContext Build ---\n";

    // Create a mock ValueLocationResult
    AMT::ValueLocationResult valLocResult = createMockValueLocationResult(
        AMT::ValueZone::AT_VAH, 8.0, 0.5, 16.0);

    // Build context from SSOT
    const double sessionHigh = 6100.0, sessionLow = 6050.0;
    const double ibHigh = 6080.0, ibLow = 6060.0;
    const double currentPrice = 6078.0;  // Near VAH
    const double tickSize = 0.25;

    AMT::LiquidityLocationContext ctx = AMT::LiquidityLocationContext::BuildFromValueLocation(
        valLocResult,
        AMT::AMTMarketState::BALANCE,  // 2TF
        AMT::VolatilityRegime::NORMAL,
        sessionHigh, sessionLow, ibHigh, ibLow,
        currentPrice, tickSize
    );

    check(ctx.isValid, "Context is valid after build");
    check(ctx.zone == AMT::ValueZone::AT_VAH, "Zone is AT_VAH");
    check(ctx.atValueEdge, "atValueEdge is true for AT_VAH");
    check(!ctx.insideValue, "insideValue is false for AT_VAH");
    check(!ctx.outsideValue, "outsideValue is false for AT_VAH");
    check(ctx.is2TF, "is2TF is true for BALANCE state");
    check(!ctx.is1TF, "is1TF is false for BALANCE state");
    check(ctx.IsAtMeaningfulLevel(), "IsAtMeaningfulLevel() returns true");
}

// ============================================================================
// Test: Location Context at Value Edges
// ============================================================================
void testLocationContextEdges() {
    std::cout << "\n--- Test: Location Context at Value Edges ---\n";

    // Test AT_VAH
    auto valLocVAH = createMockValueLocationResult(AMT::ValueZone::AT_VAH);
    auto ctxVAH = AMT::LiquidityLocationContext::BuildFromValueLocation(
        valLocVAH, AMT::AMTMarketState::IMBALANCE, AMT::VolatilityRegime::NORMAL,
        6100.0, 6050.0, 6080.0, 6060.0, 6078.0, 0.25
    );
    check(ctxVAH.atValueEdge, "AT_VAH -> atValueEdge = true");

    // Test AT_VAL
    auto valLocVAL = createMockValueLocationResult(AMT::ValueZone::AT_VAL);
    auto ctxVAL = AMT::LiquidityLocationContext::BuildFromValueLocation(
        valLocVAL, AMT::AMTMarketState::IMBALANCE, AMT::VolatilityRegime::NORMAL,
        6100.0, 6050.0, 6080.0, 6060.0, 6062.0, 0.25
    );
    check(ctxVAL.atValueEdge, "AT_VAL -> atValueEdge = true");

    // Test UPPER_VALUE (inside value, not edge)
    auto valLocUpper = createMockValueLocationResult(AMT::ValueZone::UPPER_VALUE);
    auto ctxUpper = AMT::LiquidityLocationContext::BuildFromValueLocation(
        valLocUpper, AMT::AMTMarketState::BALANCE, AMT::VolatilityRegime::NORMAL,
        6100.0, 6050.0, 6080.0, 6060.0, 6075.0, 0.25
    );
    check(!ctxUpper.atValueEdge, "UPPER_VALUE -> atValueEdge = false");
    check(ctxUpper.insideValue, "UPPER_VALUE -> insideValue = true");
}

// ============================================================================
// Test: Location Context Outside Value (Discovery)
// ============================================================================
void testLocationContextDiscovery() {
    std::cout << "\n--- Test: Location Context Outside Value (Discovery) ---\n";

    // Test FAR_ABOVE_VALUE (discovery)
    auto valLocFar = createMockValueLocationResult(
        AMT::ValueZone::FAR_ABOVE_VALUE, 20.0, 12.0, 0.0);

    auto ctx = AMT::LiquidityLocationContext::BuildFromValueLocation(
        valLocFar, AMT::AMTMarketState::IMBALANCE, AMT::VolatilityRegime::EXPANSION,
        6100.0, 6050.0, 6080.0, 6060.0, 6105.0, 0.25
    );

    check(ctx.outsideValue, "FAR_ABOVE_VALUE -> outsideValue = true");
    check(!ctx.insideValue, "FAR_ABOVE_VALUE -> insideValue = false");
    check(!ctx.atValueEdge, "FAR_ABOVE_VALUE -> atValueEdge = false");
    check(ctx.is1TF, "IMBALANCE -> is1TF = true");
    check(ctx.isExpansion, "EXPANSION -> isExpansion = true");
    check(ctx.IsInDiscovery(), "FAR_ABOVE_VALUE + IMBALANCE -> IsInDiscovery() = true");
}

// ============================================================================
// Test: Session Extreme Proximity
// ============================================================================
void testSessionExtremeProximity() {
    std::cout << "\n--- Test: Session Extreme Proximity ---\n";

    auto valLoc = createMockValueLocationResult(AMT::ValueZone::FAR_ABOVE_VALUE);

    // Price at session high (within 2 tick tolerance)
    auto ctx = AMT::LiquidityLocationContext::BuildFromValueLocation(
        valLoc, AMT::AMTMarketState::IMBALANCE, AMT::VolatilityRegime::NORMAL,
        6100.0, 6050.0, 6080.0, 6060.0,
        6099.75,  // 1 tick from session high
        0.25
    );
    check(ctx.atSessionExtreme, "1 tick from session high -> atSessionExtreme = true");
    check(ctx.IsAtMeaningfulLevel(), "At session extreme -> IsAtMeaningfulLevel() = true");

    // Price not at session extreme
    auto ctx2 = AMT::LiquidityLocationContext::BuildFromValueLocation(
        valLoc, AMT::AMTMarketState::IMBALANCE, AMT::VolatilityRegime::NORMAL,
        6100.0, 6050.0, 6080.0, 6060.0,
        6090.0,  // 40 ticks from session high
        0.25
    );
    check(!ctx2.atSessionExtreme, "40 ticks from extremes -> atSessionExtreme = false");
}

// ============================================================================
// Test: IB Boundary Proximity
// ============================================================================
void testIBBoundaryProximity() {
    std::cout << "\n--- Test: IB Boundary Proximity ---\n";

    auto valLoc = createMockValueLocationResult(AMT::ValueZone::UPPER_VALUE);

    // Price at IB high (within 2 tick tolerance)
    auto ctx = AMT::LiquidityLocationContext::BuildFromValueLocation(
        valLoc, AMT::AMTMarketState::BALANCE, AMT::VolatilityRegime::NORMAL,
        6100.0, 6050.0, 6080.0, 6060.0,
        6079.75,  // 1 tick from IB high
        0.25
    );
    check(ctx.atIBBoundary, "1 tick from IB high -> atIBBoundary = true");
    check(ctx.IsAtMeaningfulLevel(), "At IB boundary -> IsAtMeaningfulLevel() = true");

    // Price at IB low
    auto ctx2 = AMT::LiquidityLocationContext::BuildFromValueLocation(
        valLoc, AMT::AMTMarketState::BALANCE, AMT::VolatilityRegime::NORMAL,
        6100.0, 6050.0, 6080.0, 6060.0,
        6060.25,  // 1 tick from IB low
        0.25
    );
    check(ctx2.atIBBoundary, "1 tick from IB low -> atIBBoundary = true");
}

// ============================================================================
// Test: Volatility Regime Flags
// ============================================================================
void testVolatilityRegimeFlags() {
    std::cout << "\n--- Test: Volatility Regime Flags ---\n";

    auto valLoc = createMockValueLocationResult(AMT::ValueZone::AT_POC);

    // Test COMPRESSION
    auto ctxComp = AMT::LiquidityLocationContext::BuildFromValueLocation(
        valLoc, AMT::AMTMarketState::BALANCE, AMT::VolatilityRegime::COMPRESSION,
        6100.0, 6050.0, 6080.0, 6060.0, 6070.0, 0.25
    );
    check(ctxComp.isCompression, "COMPRESSION -> isCompression = true");
    check(!ctxComp.isExpansion, "COMPRESSION -> isExpansion = false");

    // Test EXPANSION
    auto ctxExp = AMT::LiquidityLocationContext::BuildFromValueLocation(
        valLoc, AMT::AMTMarketState::IMBALANCE, AMT::VolatilityRegime::EXPANSION,
        6100.0, 6050.0, 6080.0, 6060.0, 6070.0, 0.25
    );
    check(!ctxExp.isCompression, "EXPANSION -> isCompression = false");
    check(ctxExp.isExpansion, "EXPANSION -> isExpansion = true");

    // Test EVENT (also counts as expansion)
    auto ctxEvt = AMT::LiquidityLocationContext::BuildFromValueLocation(
        valLoc, AMT::AMTMarketState::IMBALANCE, AMT::VolatilityRegime::EVENT,
        6100.0, 6050.0, 6080.0, 6060.0, 6070.0, 0.25
    );
    check(ctxEvt.isExpansion, "EVENT -> isExpansion = true");
}

// ============================================================================
// Test: Invalid ValueLocationResult Handling
// ============================================================================
void testInvalidInput() {
    std::cout << "\n--- Test: Invalid Input Handling ---\n";

    // Not valid (error set)
    AMT::ValueLocationResult invalidResult;
    invalidResult.errorReason = AMT::ValueLocationErrorReason::WARMUP_PROFILE;

    auto ctx = AMT::LiquidityLocationContext::BuildFromValueLocation(
        invalidResult, AMT::AMTMarketState::BALANCE, AMT::VolatilityRegime::NORMAL,
        6100.0, 6050.0, 6080.0, 6060.0, 6070.0, 0.25
    );
    check(!ctx.isValid, "Invalid input (warmup) -> context isValid = false");

    // Invalid tick size
    auto validResult = createMockValueLocationResult(AMT::ValueZone::AT_POC);
    auto ctx2 = AMT::LiquidityLocationContext::BuildFromValueLocation(
        validResult, AMT::AMTMarketState::BALANCE, AMT::VolatilityRegime::NORMAL,
        6100.0, 6050.0, 6080.0, 6060.0, 6070.0,
        0.0  // Invalid tick size
    );
    check(!ctx2.isValid, "Invalid tick size -> context isValid = false");
}

// ============================================================================
// Test: Liq3Result Location Fields
// ============================================================================
void testLiq3ResultLocationFields() {
    std::cout << "\n--- Test: Liq3Result Location Fields ---\n";

    AMT::Liq3Result result;

    // Default values
    check(!result.hasLocationContext, "Default hasLocationContext = false");
    check(std::abs(result.stressContextMultiplier - 1.0) < 0.001, "Default stressContextMultiplier = 1.0");
    check(std::abs(result.depthContextMultiplier - 1.0) < 0.001, "Default depthContextMultiplier = 1.0");
    check(!result.rotationExpected, "Default rotationExpected = false");

    // Set location context
    result.locationContext.is2TF = true;
    result.locationContext.insideValue = true;
    result.locationContext.isValid = true;
    result.hasLocationContext = true;
    result.rotationExpected = true;

    check(result.hasLocationContext, "hasLocationContext set to true");
    check(result.IsRotationContext(), "IsRotationContext() returns true for 2TF inside value");

    // Check trend context
    result.locationContext.is2TF = false;
    result.locationContext.is1TF = true;
    result.locationContext.outsideValue = true;
    result.locationContext.insideValue = false;

    check(result.IsTrendContext(), "IsTrendContext() returns true for 1TF outside value");
}

// ============================================================================
// Test: Spatial Profile Gating (when enabled)
// ============================================================================
void testSpatialGating() {
    std::cout << "\n--- Test: Spatial Profile Gating ---\n";

    AMT::LiquidityEngine engine;

    // Enable spatial gating
    engine.config.enableSpatialGating = true;

    // Create context for deep rotation (should skip)
    AMT::LiquidityLocationContext rotationCtx;
    rotationCtx.isValid = true;
    rotationCtx.is2TF = true;
    rotationCtx.insideValue = true;
    rotationCtx.atValueEdge = false;
    rotationCtx.atSessionExtreme = false;
    rotationCtx.atIBBoundary = false;

    // Empty levels (just testing gating logic)
    std::vector<std::pair<double, double>> bidLevels, askLevels;

    auto profile = engine.ComputeSpatialProfileWithLocation(
        bidLevels, askLevels, 6070.0, 0.25, 100, rotationCtx
    );

    check(profile.skipped, "Deep rotation with gating enabled -> skipped = true");
    check(!profile.valid, "Skipped profile -> valid = false");
    check(profile.WasSkipped(), "WasSkipped() returns true");
    check(profile.skippedReason != nullptr, "skippedReason is set");

    // Now test with gating disabled
    engine.config.enableSpatialGating = false;

    auto profile2 = engine.ComputeSpatialProfileWithLocation(
        bidLevels, askLevels, 6070.0, 0.25, 100, rotationCtx
    );

    check(!profile2.skipped, "Gating disabled -> skipped = false");

    // Test at value edge (should NOT skip even with gating enabled)
    engine.config.enableSpatialGating = true;
    AMT::LiquidityLocationContext edgeCtx;
    edgeCtx.isValid = true;
    edgeCtx.is2TF = true;
    edgeCtx.insideValue = false;
    edgeCtx.atValueEdge = true;
    edgeCtx.atSessionExtreme = false;
    edgeCtx.atIBBoundary = false;

    auto profile3 = engine.ComputeSpatialProfileWithLocation(
        bidLevels, askLevels, 6078.0, 0.25, 100, edgeCtx
    );

    check(!profile3.skipped, "At value edge -> not skipped (even with gating enabled)");
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::cout << "========================================\n";
    std::cout << "  LiquidityEngine Location-Aware Tests\n";
    std::cout << "========================================\n";

    testLocationContextBuild();
    testLocationContextEdges();
    testLocationContextDiscovery();
    testSessionExtremeProximity();
    testIBBoundaryProximity();
    testVolatilityRegimeFlags();
    testInvalidInput();
    testLiq3ResultLocationFields();
    testSpatialGating();

    std::cout << "\n========================================\n";
    std::cout << "  Results: " << g_passed << " passed, " << g_failed << " failed\n";
    std::cout << "========================================\n";

    return (g_failed == 0) ? 0 : 1;
}
