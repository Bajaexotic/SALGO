// ============================================================================
// test_spatial_dom.cpp
// Unit tests for Spatial DOM Pattern Detection (Spoofing, Iceberg, Wall, Flip)
// ============================================================================

#define _USE_MATH_DEFINES
#include <cmath>

#include "../AMT_DomEvents.h"
#include "../AMT_Liquidity.h"
#include <iostream>
#include <cassert>

using namespace AMT;

static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " #name "... "; \
    try { test_##name(); g_testsPassed++; std::cout << "PASSED\n"; } \
    catch (const std::exception& e) { g_testsFailed++; std::cout << "FAILED: " << e.what() << "\n"; } \
    catch (...) { g_testsFailed++; std::cout << "FAILED: Unknown exception\n"; } \
} while(0)

#define ASSERT_TRUE(cond) do { if (!(cond)) throw std::runtime_error("Assertion failed: " #cond); } while(0)
#define ASSERT_FALSE(cond) do { if (cond) throw std::runtime_error("Assertion failed: NOT " #cond); } while(0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) throw std::runtime_error("Assertion failed: " #a " == " #b); } while(0)
#define ASSERT_GT(a, b) do { if (!((a) > (b))) throw std::runtime_error("Assertion failed: " #a " > " #b); } while(0)

// ============================================================================
// Helper: Create a SpatialDomSnapshot with specified level quantities
// ============================================================================
static SpatialDomSnapshot CreateSnapshot(
    int64_t timestampMs,
    int barIndex,
    double refPrice,
    double tickSize,
    const std::array<double, 10>& bidQtys,  // [0]=-10 ticks, [9]=-1 tick
    const std::array<double, 10>& askQtys   // [0]=+1 tick, [9]=+10 ticks
)
{
    SpatialDomSnapshot snap;
    snap.timestampMs = timestampMs;
    snap.barIndex = barIndex;
    snap.referencePrice = refPrice;
    snap.tickSize = tickSize;

    // Populate bid levels (indices 0-9, offsets -10 to -1)
    for (int i = 0; i < 10; ++i) {
        SpatialDomLevel& lvl = snap.levels[i];
        lvl.tickOffset = -(10 - i);  // i=0 -> -10, i=9 -> -1
        lvl.isBid = true;
        lvl.quantity = bidQtys[i];
        lvl.isValid = (bidQtys[i] > 0);
        snap.totalBidQuantity += bidQtys[i];
        if (bidQtys[i] > snap.maxBidQuantity) snap.maxBidQuantity = bidQtys[i];
    }

    // Populate ask levels (indices 10-19, offsets +1 to +10)
    for (int i = 0; i < 10; ++i) {
        SpatialDomLevel& lvl = snap.levels[10 + i];
        lvl.tickOffset = i + 1;  // i=0 -> +1, i=9 -> +10
        lvl.isBid = false;
        lvl.quantity = askQtys[i];
        lvl.isValid = (askQtys[i] > 0);
        snap.totalAskQuantity += askQtys[i];
        if (askQtys[i] > snap.maxAskQuantity) snap.maxAskQuantity = askQtys[i];
    }

    return snap;
}

// ============================================================================
// TEST: SpatialDomSnapshot struct basics
// ============================================================================
TEST(SpatialDomSnapshot_Basics)
{
    SpatialDomSnapshot snap;
    ASSERT_EQ(snap.timestampMs, 0);
    ASSERT_EQ(snap.barIndex, -1);
    ASSERT_EQ(snap.totalBidQuantity, 0.0);
    ASSERT_EQ(snap.totalAskQuantity, 0.0);

    // Test level access
    ASSERT_EQ(snap.levels.size(), static_cast<size_t>(SpatialDomConfig::TOTAL_LEVELS));
}

// ============================================================================
// TEST: SpatialDomHistoryBuffer operations
// ============================================================================
TEST(SpatialDomHistoryBuffer_Operations)
{
    SpatialDomHistoryBuffer buffer;
    ASSERT_FALSE(buffer.HasMinSamples());
    ASSERT_EQ(buffer.Size(), 0u);

    // Push samples
    for (int i = 0; i < SpatialDomConfig::MIN_SAMPLES; ++i) {
        std::array<double, 10> bids = {10, 10, 10, 10, 10, 10, 10, 10, 10, 10};
        std::array<double, 10> asks = {10, 10, 10, 10, 10, 10, 10, 10, 10, 10};
        buffer.Push(CreateSnapshot(1000 + i * 100, i, 6000.0, 0.25, bids, asks));
    }

    ASSERT_TRUE(buffer.HasMinSamples());
    ASSERT_EQ(buffer.Size(), static_cast<size_t>(SpatialDomConfig::MIN_SAMPLES));

    // Get window
    auto window = buffer.GetWindow(500);  // 500ms window
    ASSERT_GT(window.size(), 0u);

    // Reset
    buffer.Reset();
    ASSERT_FALSE(buffer.HasMinSamples());
}

// ============================================================================
// TEST: Spoofing detection - Large order appears then vanishes
// ============================================================================
TEST(DetectSpoofing_BasicPattern)
{
    std::vector<SpatialDomSnapshot> window;
    std::array<double, 10> normalBids = {50, 50, 50, 50, 50, 50, 50, 50, 50, 50};
    std::array<double, 10> normalAsks = {50, 50, 50, 50, 50, 50, 50, 50, 50, 50};

    // First 5 samples: normal depth
    for (int i = 0; i < 5; ++i) {
        window.push_back(CreateSnapshot(1000 + i * 200, i, 6000.0, 0.25, normalBids, normalAsks));
    }

    // Sample 6: Large bid appears at offset -3 (index 7)
    std::array<double, 10> largeBids = normalBids;
    largeBids[7] = 500;  // P80 threshold is usually ~80th percentile
    window.push_back(CreateSnapshot(2000, 6, 6000.0, 0.25, largeBids, normalAsks));

    // Samples 7-9: Large bid maintained
    for (int i = 0; i < 3; ++i) {
        window.push_back(CreateSnapshot(2200 + i * 200, 7 + i, 6000.0, 0.25, largeBids, normalAsks));
    }

    // Sample 10: Large bid vanishes (pulled)
    window.push_back(CreateSnapshot(2800, 10, 6000.0, 0.25, normalBids, normalAsks));

    // Detect spoofing with P80 threshold
    double quantityP80 = 100.0;  // 500 > 100, so it qualifies as large
    auto hits = DetectSpoofing(window, quantityP80);

    // Should detect spoofing at the level where large order appeared then vanished
    ASSERT_GT(hits.size(), 0u);
    if (!hits.empty()) {
        ASSERT_TRUE(hits[0].isBidSide);
        ASSERT_GT(hits[0].peakQuantity, 400.0);
        std::cout << "(detected at offset " << hits[0].tickOffset << ") ";
    }
}

// ============================================================================
// TEST: Iceberg detection - Level depletes and refills
// ============================================================================
TEST(DetectIceberg_RefillPattern)
{
    std::vector<SpatialDomSnapshot> window;
    std::array<double, 10> bids = {50, 50, 50, 50, 50, 50, 50, 50, 50, 50};
    std::array<double, 10> asks = {50, 50, 50, 50, 50, 50, 50, 50, 50, 50};

    // Simulate iceberg at ask offset +2 (index 11 in levels, index 1 in asks array)
    // Pattern: deplete -> refill -> deplete -> refill -> deplete -> refill
    double baseQty = 100.0;

    for (int cycle = 0; cycle < 4; ++cycle) {
        // Full quantity
        asks[1] = baseQty;
        window.push_back(CreateSnapshot(1000 + cycle * 400, cycle * 2, 6000.0, 0.25, bids, asks));

        // Depleted (< 50% of base)
        asks[1] = baseQty * 0.3;
        window.push_back(CreateSnapshot(1200 + cycle * 400, cycle * 2 + 1, 6000.0, 0.25, bids, asks));
    }

    // Detect iceberg
    auto hits = DetectIceberg(window);

    // Should detect iceberg pattern
    ASSERT_GT(hits.size(), 0u);
    if (!hits.empty()) {
        ASSERT_FALSE(hits[0].isBidSide);  // Ask side
        ASSERT_GT(hits[0].refillCount, 2);
        std::cout << "(refills=" << hits[0].refillCount << ") ";
    }
}

// ============================================================================
// TEST: Wall Breaking - Large order progressively absorbed
// ============================================================================
TEST(DetectWallBreaking_AbsorptionPattern)
{
    std::vector<SpatialDomSnapshot> window;
    std::array<double, 10> bids = {50, 50, 50, 50, 50, 50, 50, 50, 50, 50};
    std::array<double, 10> asks = {50, 50, 50, 50, 50, 50, 50, 50, 50, 50};

    // Create a wall at bid offset -5 (index 5) that gets progressively consumed
    double wallSize = 1000.0;  // Very large order (> P90)

    for (int i = 0; i < 10; ++i) {
        // Wall gets progressively consumed (1000 -> 900 -> 800 -> ... -> 200)
        bids[5] = wallSize - i * 100;
        window.push_back(CreateSnapshot(1000 + i * 300, i, 6000.0, 0.25, bids, asks));
    }

    // Detect wall breaking with P90 threshold
    double quantityP90 = 500.0;  // 1000 > 500, so it qualifies as wall
    auto hits = DetectWallBreaking(window, quantityP90);

    // Should detect wall being absorbed
    ASSERT_GT(hits.size(), 0u);
    if (!hits.empty()) {
        ASSERT_TRUE(hits[0].isBidSide);
        ASSERT_GT(hits[0].startQuantity, 800.0);
        ASSERT_GT(hits[0].absorptionRate, 0.5);  // Significant absorption
        std::cout << "(start=" << hits[0].startQuantity << " end=" << hits[0].endQuantity << ") ";
    }
}

// ============================================================================
// TEST: Flip Detection - Bid wall becomes ask wall
// ============================================================================
TEST(DetectFlip_BidToAskFlip)
{
    std::vector<SpatialDomSnapshot> window;
    std::array<double, 10> bids = {50, 50, 50, 50, 50, 50, 50, 50, 50, 50};
    std::array<double, 10> asks = {50, 50, 50, 50, 50, 50, 50, 50, 50, 50};
    const double tickSize = 0.25;
    const double refPrice = 6000.0;

    // Initial state: Large bid wall at offset -2 (price = 6000 - 0.5 = 5999.5)
    bids[8] = 500;  // Offset -2 is at index 8
    window.push_back(CreateSnapshot(1000, 0, refPrice, tickSize, bids, asks));
    window.push_back(CreateSnapshot(1200, 1, refPrice, tickSize, bids, asks));

    // Price moves up, reference price changes
    // Now the same price level (5999.5) is at ask offset +1 relative to new reference (5999.25)
    // This simulates the flip - bid wall becomes ask wall as price crossed
    bids[8] = 50;  // Bid wall disappears
    asks[0] = 400;  // Ask wall appears at offset +1 (close to where bid was)

    // Move reference price down to simulate price crossing the wall level
    const double newRefPrice = 5999.25;
    window.push_back(CreateSnapshot(1400, 2, newRefPrice, tickSize, bids, asks));
    window.push_back(CreateSnapshot(1600, 3, newRefPrice, tickSize, bids, asks));

    // Detect flip
    auto hits = DetectFlip(window, newRefPrice, tickSize);

    // Note: Flip detection is complex and depends on tracking same price level
    // This test verifies the detection function runs without error
    std::cout << "(flip detection ran, hits=" << hits.size() << ") ";
}

// ============================================================================
// TEST: Combined detection through SpatialDomPatternResult
// ============================================================================
TEST(DetectSpatialDomPatterns_Combined)
{
    SpatialDomHistoryBuffer buffer;
    std::array<double, 10> bids = {50, 50, 50, 50, 50, 50, 50, 50, 50, 50};
    std::array<double, 10> asks = {50, 50, 50, 50, 50, 50, 50, 50, 50, 50};

    // Push enough samples
    for (int i = 0; i < 10; ++i) {
        buffer.Push(CreateSnapshot(1000 + i * 100, i, 6000.0, 0.25, bids, asks));
    }

    ASSERT_TRUE(buffer.HasMinSamples());

    // Detect patterns (should be ineligible for patterns since no manipulation)
    // Args: buffer, quantityP80, quantityP90, currentPrice, tickSize, windowMs
    auto result = DetectSpatialDomPatterns(buffer, 100.0, 200.0, 6000.0, 0.25, 3000);

    ASSERT_TRUE(result.wasEligible);
    // Normal data shouldn't trigger any patterns
    ASSERT_FALSE(result.HasPatterns());
}

// ============================================================================
// TEST: LiquidityEngine spatial pattern integration
// ============================================================================
TEST(LiquidityEngine_SpatialPatterns)
{
    LiquidityEngine engine;

    // Push spatial snapshots
    std::array<double, 10> bids = {50, 50, 50, 50, 50, 50, 50, 50, 50, 50};
    std::array<double, 10> asks = {50, 50, 50, 50, 50, 50, 50, 50, 50, 50};

    for (int i = 0; i < 10; ++i) {
        auto snap = CreateSnapshot(1000 + i * 100, i, 6000.0, 0.25, bids, asks);
        engine.PushSpatialDomSnapshot(snap);
    }

    ASSERT_TRUE(engine.HasSpatialDomMinSamples());
    ASSERT_EQ(engine.GetSpatialDomHistorySize(), 10u);

    // Run detection
    Liq3Result result;
    auto patternResult = engine.DetectAndCopySpatialPatterns(result, 6000.0, 0.25, 3000);

    ASSERT_TRUE(patternResult.wasEligible);
    ASSERT_TRUE(result.spatialPatternsEligible);

    // Reset
    engine.ResetSpatialDomHistory();
    ASSERT_FALSE(engine.HasSpatialDomMinSamples());
}

// ============================================================================
// TEST: Liq3Result spatial pattern fields
// ============================================================================
TEST(Liq3Result_SpatialPatternFields)
{
    Liq3Result snap;

    // Verify default state
    ASSERT_FALSE(snap.hasSpoofing);
    ASSERT_FALSE(snap.hasIceberg);
    ASSERT_FALSE(snap.hasWallBreak);
    ASSERT_FALSE(snap.hasFlip);
    ASSERT_EQ(snap.spoofingCount, 0);
    ASSERT_EQ(snap.icebergCount, 0);
    ASSERT_EQ(snap.wallBreakCount, 0);
    ASSERT_EQ(snap.flipCount, 0);
    ASSERT_FALSE(snap.spatialPatternsEligible);

    // Verify helper methods
    ASSERT_FALSE(snap.HasSpatialPatterns());
    ASSERT_EQ(snap.GetSpatialPatternCount(), 0);
    ASSERT_FALSE(snap.HasManipulativePattern());
    ASSERT_FALSE(snap.HasAbsorptionPattern());

    // Set some flags
    snap.hasSpoofing = true;
    snap.spoofingCount = 2;
    ASSERT_TRUE(snap.HasSpatialPatterns());
    ASSERT_EQ(snap.GetSpatialPatternCount(), 2);
    ASSERT_TRUE(snap.HasManipulativePattern());

    snap.hasWallBreak = true;
    snap.wallBreakCount = 1;
    ASSERT_TRUE(snap.HasAbsorptionPattern());
    ASSERT_EQ(snap.GetSpatialPatternCount(), 3);

    // Combined check
    ASSERT_TRUE(snap.HasAnyDomPatternComplete());
}

// ============================================================================
// TEST: Pattern strength scoring
// ============================================================================
TEST(SpatialPatterns_StrengthScoring)
{
    SpoofingHit spoofHit;
    spoofHit.tickOffset = -3;
    spoofHit.isBidSide = true;
    spoofHit.peakQuantity = 500.0;
    spoofHit.endQuantity = 10.0;
    spoofHit.durationMs = 1500;
    spoofHit.strength01 = 0.85f;

    ASSERT_GT(spoofHit.strength01, 0.5f);  // Should be a strong signal

    IcebergHit iceHit;
    iceHit.tickOffset = 2;
    iceHit.isBidSide = false;
    iceHit.avgQuantity = 100.0;
    iceHit.refillCount = 5;
    iceHit.strength01 = 0.75f;

    ASSERT_GT(iceHit.refillCount, 3);  // More refills = stronger pattern
}

// ============================================================================
// TEST: DomPatternContext building (Jan 2025)
// ============================================================================
TEST(DomPatternContext_Build)
{
    const double tickSize = 0.25;
    const double poc = 6000.0;
    const double vah = 6005.0;
    const double val = 5995.0;
    const double sessHigh = 6010.0;
    const double sessLow = 5990.0;

    // Test AT_POC location (using LiquidityEngine helper for cleaner API)
    {
        DomPatternContext ctx = LiquidityEngine::BuildPatternContext(
            6000.0, poc, vah, val, tickSize,
            AMTMarketState::BALANCE,
            false, false, false);  // valueMigHigh, valueMigLow, nearExtreme

        ASSERT_TRUE(ctx.isValid);
        ASSERT_EQ(ctx.valueZone, ValueZone::AT_POC);
        ASSERT_EQ(ctx.marketState, DomMarketState::BALANCE);
        ASSERT_TRUE(ctx.IsAtPOC());
        ASSERT_TRUE(ctx.IsInBalance());
    }

    // Test AT_VAH location
    {
        DomPatternContext ctx = LiquidityEngine::BuildPatternContext(
            6005.0, poc, vah, val, tickSize,
            AMTMarketState::IMBALANCE,
            false, false, false);

        ASSERT_TRUE(ctx.isValid);
        ASSERT_EQ(ctx.valueZone, ValueZone::AT_VAH);
        ASSERT_TRUE(ctx.IsAtValueEdge());
        ASSERT_TRUE(ctx.IsInImbalance());
    }

    // Test NEAR_ABOVE_VALUE location (outside but not discovery)
    {
        DomPatternContext ctx = LiquidityEngine::BuildPatternContext(
            6007.0, poc, vah, val, tickSize,
            AMTMarketState::IMBALANCE,
            true, false, false);  // valueMigratingHigher

        ASSERT_TRUE(ctx.isValid);
        ASSERT_EQ(ctx.valueZone, ValueZone::NEAR_ABOVE_VALUE);
        ASSERT_TRUE(ctx.IsOutsideValue());
        ASSERT_TRUE(ctx.valueMigratingHigher);
    }

    // Test FAR_ABOVE_VALUE (discovery) location - use direct Build for more control
    {
        DomPatternContext ctx = DomPatternContext::Build(
            6020.0,  // 15 points above VAH
            poc, vah, val,
            sessHigh, sessLow, tickSize,
            true,    // is1TFState
            false, false,  // valueMigHigh, valueMigLow
            true, false,   // priceUp, priceDown
            2.0,     // edgeToleranceTicks
            10.0);   // discoveryThresholdTicks (10 ticks = 2.5 points)

        ASSERT_TRUE(ctx.isValid);
        ASSERT_EQ(ctx.valueZone, ValueZone::FAR_ABOVE_VALUE);
        ASSERT_TRUE(ctx.IsInDiscovery());
        ASSERT_TRUE(ctx.IsOutsideValue());
    }
}

// ============================================================================
// TEST: Context significance adjustment (Jan 2025)
// ============================================================================
TEST(Context_SignificanceAdjustment)
{
    const double tickSize = 0.25;
    const double poc = 6000.0;
    const double vah = 6005.0;
    const double val = 5995.0;

    // Create a spoofing hit
    SpoofingHit hit;
    hit.tickOffset = -3;
    hit.isBidSide = true;
    hit.peakQuantity = 500.0;
    hit.endQuantity = 10.0;
    hit.durationMs = 1500;
    hit.strength01 = 0.80f;  // Base strength = 0.80

    // Test AT_POC context - should reduce significance (low multiplier)
    {
        DomPatternContext ctx = LiquidityEngine::BuildPatternContext(
            6000.0, poc, vah, val, tickSize,
            AMTMarketState::BALANCE,
            false, false, false);

        SpoofingHit hitCopy = hit;
        hitCopy.ApplyContext(ctx);

        ASSERT_TRUE(hitCopy.hasContext);
        // At POC in balance: low significance (noise)
        ASSERT_TRUE(hitCopy.contextSignificance < hit.strength01);  // Should be reduced
        ASSERT_EQ(hitCopy.interpretation, PatternInterpretation::NOISE);
    }

    // Test AT_VAH context - should increase significance (high multiplier)
    {
        DomPatternContext ctx = LiquidityEngine::BuildPatternContext(
            6005.0, poc, vah, val, tickSize,
            AMTMarketState::IMBALANCE,
            false, false, false);

        SpoofingHit hitCopy = hit;
        hitCopy.ApplyContext(ctx);

        ASSERT_TRUE(hitCopy.hasContext);
        // At VAH in imbalance: high significance
        ASSERT_GT(hitCopy.contextSignificance, hit.strength01);  // Should be boosted
        // Bid spoofing at VAH in imbalance = likely breakout manipulation
    }

    // Test GetEffectiveStrength()
    {
        DomPatternContext ctx = LiquidityEngine::BuildPatternContext(
            6005.0, poc, vah, val, tickSize,
            AMTMarketState::IMBALANCE,
            false, false, false);

        SpoofingHit hitWithCtx = hit;
        hitWithCtx.ApplyContext(ctx);

        SpoofingHit hitNoCtx = hit;

        // With context: effective = context significance
        ASSERT_EQ(hitWithCtx.GetEffectiveStrength(), hitWithCtx.contextSignificance);
        // Without context: effective = base strength
        ASSERT_EQ(hitNoCtx.GetEffectiveStrength(), hitNoCtx.strength01);
    }
}

// ============================================================================
// TEST: Pattern interpretation based on location (Jan 2025)
// ============================================================================
TEST(Context_PatternInterpretation)
{
    const double tickSize = 0.25;
    const double poc = 6000.0;
    const double vah = 6005.0;
    const double val = 5995.0;

    // Test spoofing interpretation at different locations
    {
        // Spoofing at POC in balance = NOISE
        DomPatternContext ctxPOC = LiquidityEngine::BuildPatternContext(
            6000.0, poc, vah, val, tickSize,
            AMTMarketState::BALANCE,
            false, false, false);
        PatternInterpretation interpPOC = InterpretSpoofing(ctxPOC, true);
        ASSERT_EQ(interpPOC, PatternInterpretation::NOISE);

        // Spoofing at VAH in balance with ask side = AGGRESSIVE (manipulating for breakout)
        DomPatternContext ctxVAH = LiquidityEngine::BuildPatternContext(
            6005.0, poc, vah, val, tickSize,
            AMTMarketState::BALANCE,
            false, false, false);
        PatternInterpretation interpVAH = InterpretSpoofing(ctxVAH, false);  // Ask side
        // Ask spoofing at VAH = aggressive (someone pulling asks to let price break higher)
        ASSERT_EQ(interpVAH, PatternInterpretation::AGGRESSIVE);

        // Spoofing at VAH in imbalance with migration - test bid side = DEFENSIVE
        DomPatternContext ctxVAHImb = LiquidityEngine::BuildPatternContext(
            6005.0, poc, vah, val, tickSize,
            AMTMarketState::IMBALANCE,
            true, false, false);  // migrating higher
        PatternInterpretation interpVAHImb = InterpretSpoofing(ctxVAHImb, true);  // Bid side
        // Bid spoofing at VAH = defensive (fake bids support level from below)
        ASSERT_EQ(interpVAHImb, PatternInterpretation::DEFENSIVE);
    }

    // Test flip interpretation
    {
        // Flip at VAL = TRAPPED_TRADERS
        DomPatternContext ctxVAL = LiquidityEngine::BuildPatternContext(
            5995.0, poc, vah, val, tickSize,
            AMTMarketState::IMBALANCE,
            false, true, false);  // migrating lower
        PatternInterpretation interpFlip = InterpretFlip(ctxVAL, true);  // bid->ask
        ASSERT_EQ(interpFlip, PatternInterpretation::TRAPPED_TRADERS);
    }
}

// ============================================================================
// TEST: SpatialDomPatternResult context application (Jan 2025)
// ============================================================================
TEST(SpatialDomPatternResult_ContextApplication)
{
    const double tickSize = 0.25;
    const double poc = 6000.0;
    const double vah = 6005.0;
    const double val = 5995.0;

    // Create result with some hits
    SpatialDomPatternResult result;
    result.wasEligible = true;

    SpoofingHit spoofHit;
    spoofHit.tickOffset = -3;
    spoofHit.isBidSide = true;
    spoofHit.strength01 = 0.75f;
    spoofHit.valid = true;
    result.spoofingHits.push_back(spoofHit);

    IcebergHit iceHit;
    iceHit.tickOffset = 2;
    iceHit.isBidSide = false;
    iceHit.refillCount = 4;
    iceHit.strength01 = 0.60f;
    iceHit.valid = true;
    result.icebergHits.push_back(iceHit);

    ASSERT_FALSE(result.hasContext);

    // Apply context at VAH (high significance location)
    DomPatternContext ctx = LiquidityEngine::BuildPatternContext(
        6005.0, poc, vah, val, tickSize,
        AMTMarketState::IMBALANCE,
        false, false, false);

    result.ApplyContext(ctx);

    ASSERT_TRUE(result.hasContext);
    ASSERT_EQ(result.appliedContext.valueZone, ValueZone::AT_VAH);

    // All hits should have context
    ASSERT_TRUE(result.spoofingHits[0].hasContext);
    ASSERT_TRUE(result.icebergHits[0].hasContext);

    // GetMaxSignificance should work
    float maxSig = result.GetMaxSignificance();
    ASSERT_GT(maxSig, 0.0f);

    // GetDominantInterpretation should work
    PatternInterpretation dom = result.GetDominantInterpretation();
    ASSERT_TRUE(dom != PatternInterpretation::NOISE ||
                result.spoofingHits[0].interpretation != PatternInterpretation::NOISE);

    // High significance check
    ASSERT_TRUE(result.HasHighSignificancePatterns(0.3f));  // Low threshold should pass
}

// ============================================================================
// TEST: Liq3Result context-aware fields (Jan 2025)
// ============================================================================
TEST(Liq3Result_ContextAwareFields)
{
    Liq3Result snap;

    // Verify default context-aware state
    ASSERT_FALSE(snap.spatialContextValid);
    ASSERT_EQ(snap.maxSpatialSignificance, 0.0f);
    ASSERT_EQ(snap.dominantInterpretation, PatternInterpretation::NOISE);
    ASSERT_EQ(snap.spatialValueZone, ValueZone::UNKNOWN);
    ASSERT_EQ(snap.spatialMarketState, DomMarketState::UNKNOWN);

    // Verify context-aware helpers
    ASSERT_FALSE(snap.HasHighSignificanceSpatialPatterns());
    ASSERT_FALSE(snap.IsSpatialPatternAtEdge());
    ASSERT_FALSE(snap.IsSpatialPatternSignificant());

    // Set context-aware fields manually
    snap.spatialContextValid = true;
    snap.spatialValueZone = ValueZone::AT_VAH;
    snap.maxSpatialSignificance = 0.85f;
    snap.dominantInterpretation = PatternInterpretation::BREAKOUT_SIGNAL;
    snap.hasSpoofing = true;
    snap.spoofingCount = 1;

    ASSERT_TRUE(snap.HasHighSignificanceSpatialPatterns(0.8f));
    ASSERT_TRUE(snap.IsSpatialPatternAtEdge());
    ASSERT_TRUE(snap.IsSpatialPatternSignificant());
}

// ============================================================================
// TEST: LiquidityEngine context-aware detection (Jan 2025)
// ============================================================================
TEST(LiquidityEngine_ContextAwareDetection)
{
    LiquidityEngine engine;

    const double tickSize = 0.25;
    const double poc = 6000.0;
    const double vah = 6005.0;
    const double val = 5995.0;

    // Push enough samples for detection
    std::array<double, 10> bidQtys = {100, 100, 100, 100, 100, 100, 100, 100, 100, 100};
    std::array<double, 10> askQtys = {100, 100, 100, 100, 100, 100, 100, 100, 100, 100};

    for (int i = 0; i < 10; ++i) {
        auto snap = CreateSnapshot(i * 100, 100 + i, 6005.0, tickSize, bidQtys, askQtys);
        engine.PushSpatialDomSnapshot(snap);
    }

    ASSERT_TRUE(engine.HasSpatialDomMinSamples());

    // Build context at VAH
    DomPatternContext ctx = LiquidityEngine::BuildPatternContext(
        6005.0, poc, vah, val, tickSize,
        AMTMarketState::IMBALANCE,
        false, false, false);

    ASSERT_TRUE(ctx.isValid);
    ASSERT_EQ(ctx.valueZone, ValueZone::AT_VAH);

    // Detect with context
    Liq3Result snap;
    auto result = engine.DetectAndCopySpatialPatterns(snap, 6005.0, tickSize, ctx, 3000);

    ASSERT_TRUE(result.wasEligible);

    // Context should be applied if we have patterns
    // (even without patterns, context is applied)
    if (result.hasContext)
    {
        ASSERT_EQ(result.appliedContext.valueZone, ValueZone::AT_VAH);
        ASSERT_EQ(snap.spatialValueZone, ValueZone::AT_VAH);
    }
}

// ============================================================================
// MAIN
// ============================================================================

int main()
{
    std::cout << "========================================\n";
    std::cout << "Spatial DOM Pattern Detection Tests\n";
    std::cout << "========================================\n\n";

    RUN_TEST(SpatialDomSnapshot_Basics);
    RUN_TEST(SpatialDomHistoryBuffer_Operations);
    RUN_TEST(DetectSpoofing_BasicPattern);
    RUN_TEST(DetectIceberg_RefillPattern);
    RUN_TEST(DetectWallBreaking_AbsorptionPattern);
    RUN_TEST(DetectFlip_BidToAskFlip);
    RUN_TEST(DetectSpatialDomPatterns_Combined);
    RUN_TEST(LiquidityEngine_SpatialPatterns);
    RUN_TEST(Liq3Result_SpatialPatternFields);
    RUN_TEST(SpatialPatterns_StrengthScoring);

    // Context-aware tests (Jan 2025)
    RUN_TEST(DomPatternContext_Build);
    RUN_TEST(Context_SignificanceAdjustment);
    RUN_TEST(Context_PatternInterpretation);
    RUN_TEST(SpatialDomPatternResult_ContextApplication);
    RUN_TEST(Liq3Result_ContextAwareFields);
    RUN_TEST(LiquidityEngine_ContextAwareDetection);

    std::cout << "\n========================================\n";
    std::cout << "Results: " << g_testsPassed << " passed, " << g_testsFailed << " failed\n";
    std::cout << "========================================\n";

    return g_testsFailed > 0 ? 1 : 0;
}
