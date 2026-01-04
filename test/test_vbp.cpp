// ============================================================================
// test_vbp.cpp
// Unit tests for VBP (Volume By Price) functionality
// Tests: VbPLevelContext, GetVbPContextAtPrice, SessionVolumeProfile thresholds
// ============================================================================

#include "test_sierrachart_mock.h"
#include "amt_core.h"
#include "AMT_config.h"  // SSOT: PriceToTicks
#include <iostream>
#include <cassert>
#include <cmath>
#include <map>
#include <vector>
#include <algorithm>

// --- VolumeAtPrice: Use mock from test_sierrachart_mock.h ---
using VolumeAtPrice = s_VolumeAtPriceV2;

// ============================================================================
// VbPLevelContext (copied from AuctionSensor_v1.cpp for testing)
// ============================================================================

struct VbPLevelContext
{
    bool valid = false;

    // Location relative to Value Area
    bool insideValueArea = false;
    bool atPOC = false;
    bool aboveVAH = false;
    bool belowVAL = false;

    // SSOT classification (orthogonal outputs)
    AMT::VolumeNodeClassification classification;

    // Legacy accessors (delegate to classification for backward compatibility)
    bool isHVN = false;
    bool isLVN = false;
    double volumeAtPrice = 0.0;
    double volumePercentile = 0.0;

    // Nearby structure
    double nearestHVN = 0.0;
    double nearestLVN = 0.0;
    double distToHVNTicks = 0.0;
    double distToLVNTicks = 0.0;

    // Sync legacy fields from classification
    void SyncFromClassification() {
        isHVN = classification.IsHVN();
        isLVN = classification.IsLVN();
    }
};

// ============================================================================
// SessionVolumeProfile (minimal version for testing)
// ============================================================================

struct SessionVolumeProfile
{
    std::map<int, VolumeAtPrice> volume_profile;
    double tick_size = 0.0;

    double session_poc = 0.0;
    double session_vah = 0.0;
    double session_val = 0.0;
    std::vector<double> session_hvn;
    std::vector<double> session_lvn;

    AMT::VolumeThresholds cachedThresholds;

    void Reset(double ts)
    {
        volume_profile.clear();
        tick_size = ts;
        session_poc = 0.0;
        session_vah = 0.0;
        session_val = 0.0;
        session_hvn.clear();
        session_lvn.clear();
        cachedThresholds.Reset();
    }

    // Compute and cache SSOT thresholds from current profile
    void ComputeThresholds(int currentBar, double hvnSigmaCoeff = 1.5, double lvnSigmaCoeff = 0.5)
    {
        cachedThresholds.Reset();

        if (volume_profile.empty())
            return;

        const size_t numLevels = volume_profile.size();
        double totalVol = 0.0;
        double maxVol = 0.0;

        for (const auto& kv : volume_profile)
        {
            const double vol = static_cast<double>(kv.second.Volume);
            totalVol += vol;
            if (vol > maxVol) maxVol = vol;
        }

        if (totalVol <= 0.0 || numLevels == 0)
            return;

        const double mean = totalVol / static_cast<double>(numLevels);

        double variance = 0.0;
        for (const auto& kv : volume_profile)
        {
            double diff = static_cast<double>(kv.second.Volume) - mean;
            variance += diff * diff;
        }
        const double stddev = std::sqrt(variance / static_cast<double>(numLevels));

        cachedThresholds.mean = mean;
        cachedThresholds.stddev = stddev;
        cachedThresholds.hvnThreshold = mean + hvnSigmaCoeff * stddev;
        cachedThresholds.lvnThreshold = mean - lvnSigmaCoeff * stddev;
        cachedThresholds.sampleSize = static_cast<int>(numLevels);
        cachedThresholds.totalVolume = totalVol;
        cachedThresholds.maxLevelVolume = maxVol;
        cachedThresholds.computedAtBar = currentBar;
        cachedThresholds.valid = true;
    }

    // Helper: Add a price level with volume
    void AddLevel(int priceTick, unsigned int volume, unsigned int bidVol = 0, unsigned int askVol = 0)
    {
        VolumeAtPrice vap;
        vap.PriceInTicks = priceTick;
        vap.Volume = volume;
        vap.BidVolume = bidVol;
        vap.AskVolume = askVol;
        volume_profile[priceTick] = vap;
    }
};

// ============================================================================
// Helper: IsValidPrice
// ============================================================================

inline bool IsValidPrice(double price)
{
    return price > 0.0 && std::isfinite(price);
}

// ============================================================================
// GetVbPContextAtPrice (copied from AuctionSensor_v1.cpp for testing)
// ============================================================================

inline VbPLevelContext GetVbPContextAtPrice(
    const SessionVolumeProfile& profile,
    double queryPrice,
    double tickSize,
    double hvnSigmaCoeff = 1.5,
    double lvnSigmaCoeff = 0.5)
{
    VbPLevelContext ctx;

    if (profile.volume_profile.empty() || tickSize <= 0.0 || !IsValidPrice(queryPrice))
        return ctx;

    ctx.valid = true;
    const int queryTick = static_cast<int>(AMT::PriceToTicks(queryPrice, tickSize));

    // --- Value Area position ---
    ctx.atPOC = std::abs(queryPrice - profile.session_poc) < tickSize * 0.5;
    ctx.insideValueArea = (queryPrice >= profile.session_val && queryPrice <= profile.session_vah);
    ctx.aboveVAH = (queryPrice > profile.session_vah);
    ctx.belowVAL = (queryPrice < profile.session_val);

    // --- Volume at this price ---
    auto it = profile.volume_profile.find(queryTick);
    if (it != profile.volume_profile.end())
    {
        ctx.volumeAtPrice = static_cast<double>(it->second.Volume);
    }

    // --- Calculate volume percentile (use cached maxVol if available) ---
    double maxVol = profile.cachedThresholds.valid ? profile.cachedThresholds.maxLevelVolume : 0.0;
    if (maxVol <= 0.0)
    {
        // Fallback: compute maxVol if cache not valid
        for (const auto& kv : profile.volume_profile)
        {
            maxVol = std::max(maxVol, static_cast<double>(kv.second.Volume));
        }
    }

    if (maxVol > 0.0)
    {
        ctx.volumePercentile = ctx.volumeAtPrice / maxVol;
    }

    // --- HVN/LVN classification using SSOT cached thresholds ---
    if (profile.cachedThresholds.valid)
    {
        ctx.classification.density = profile.cachedThresholds.ClassifyVolume(ctx.volumeAtPrice);

        if (ctx.classification.density == AMT::VAPDensityClass::LOW &&
            ctx.volumeAtPrice > 0 && ctx.volumeAtPrice <= profile.cachedThresholds.mean * 0.3) {
            ctx.classification.flags = ctx.classification.flags | AMT::NodeFlags::SINGLE_PRINT;
        }

        ctx.SyncFromClassification();
    }
    else
    {
        // Fallback: compute inline if cache not valid
        const size_t numLevels = profile.volume_profile.size();
        if (numLevels > 0)
        {
            double totalVol = 0.0;
            for (const auto& kv : profile.volume_profile)
            {
                totalVol += kv.second.Volume;
            }
            const double mean = totalVol / numLevels;
            double variance = 0.0;
            for (const auto& kv : profile.volume_profile)
            {
                double diff = static_cast<double>(kv.second.Volume) - mean;
                variance += diff * diff;
            }
            const double stddev = std::sqrt(variance / numLevels);

            const double hvnThreshold = mean + hvnSigmaCoeff * stddev;
            const double lvnThreshold = mean - lvnSigmaCoeff * stddev;

            if (ctx.volumeAtPrice > hvnThreshold) {
                ctx.classification.density = AMT::VAPDensityClass::HIGH;
            } else if (ctx.volumeAtPrice < lvnThreshold && ctx.volumeAtPrice > 0) {
                ctx.classification.density = AMT::VAPDensityClass::LOW;
            }
            ctx.SyncFromClassification();
        }
    }

    // --- Find nearest HVN ---
    double minHVNDist = 1e9;
    for (const double hvn : profile.session_hvn)
    {
        double dist = std::abs(queryPrice - hvn);
        if (dist < minHVNDist)
        {
            minHVNDist = dist;
            ctx.nearestHVN = hvn;
        }
    }
    ctx.distToHVNTicks = (minHVNDist < 1e8) ? (minHVNDist / tickSize) : 1e9;

    // --- Find nearest LVN ---
    double minLVNDist = 1e9;
    for (const double lvn : profile.session_lvn)
    {
        double dist = std::abs(queryPrice - lvn);
        if (dist < minLVNDist)
        {
            minLVNDist = dist;
            ctx.nearestLVN = lvn;
        }
    }
    ctx.distToLVNTicks = (minLVNDist < 1e8) ? (minLVNDist / tickSize) : 1e9;

    return ctx;
}

// ============================================================================
// TEST 1: VbPLevelContext Basic Construction
// ============================================================================

void TestVbPLevelContextDefaults() {
    std::cout << "Testing VbPLevelContext defaults..." << std::endl;

    VbPLevelContext ctx;

    assert(ctx.valid == false);
    assert(ctx.insideValueArea == false);
    assert(ctx.atPOC == false);
    assert(ctx.aboveVAH == false);
    assert(ctx.belowVAL == false);
    assert(ctx.isHVN == false);
    assert(ctx.isLVN == false);
    assert(ctx.volumeAtPrice == 0.0);
    assert(ctx.volumePercentile == 0.0);
    std::cout << "  Default values correct [PASS]" << std::endl;
}

void TestVbPLevelContextSyncFromClassification() {
    std::cout << "\nTesting VbPLevelContext SyncFromClassification..." << std::endl;

    VbPLevelContext ctx;

    // Test HVN sync
    ctx.classification.density = AMT::VAPDensityClass::HIGH;
    ctx.SyncFromClassification();
    assert(ctx.isHVN == true);
    assert(ctx.isLVN == false);
    std::cout << "  HVN sync correct [PASS]" << std::endl;

    // Test LVN sync
    ctx.classification.density = AMT::VAPDensityClass::LOW;
    ctx.SyncFromClassification();
    assert(ctx.isHVN == false);
    assert(ctx.isLVN == true);
    std::cout << "  LVN sync correct [PASS]" << std::endl;

    // Test NORMAL sync
    ctx.classification.density = AMT::VAPDensityClass::NORMAL;
    ctx.SyncFromClassification();
    assert(ctx.isHVN == false);
    assert(ctx.isLVN == false);
    std::cout << "  NORMAL sync correct [PASS]" << std::endl;
}

// ============================================================================
// TEST 2: VolumeThresholds Classification
// ============================================================================

void TestVolumeThresholdsClassification() {
    std::cout << "\nTesting VolumeThresholds classification..." << std::endl;

    AMT::VolumeThresholds thresholds;
    thresholds.mean = 1000.0;
    thresholds.stddev = 200.0;
    thresholds.hvnThreshold = 1300.0;  // mean + 1.5 * stddev
    thresholds.lvnThreshold = 900.0;   // mean - 0.5 * stddev
    thresholds.sampleSize = 50;
    thresholds.valid = true;

    // Test HIGH classification
    assert(thresholds.ClassifyVolume(1500) == AMT::VAPDensityClass::HIGH);
    assert(thresholds.ClassifyVolume(1301) == AMT::VAPDensityClass::HIGH);
    std::cout << "  HIGH classification correct [PASS]" << std::endl;

    // Test NORMAL classification
    assert(thresholds.ClassifyVolume(1000) == AMT::VAPDensityClass::NORMAL);
    assert(thresholds.ClassifyVolume(1100) == AMT::VAPDensityClass::NORMAL);
    assert(thresholds.ClassifyVolume(950) == AMT::VAPDensityClass::NORMAL);
    std::cout << "  NORMAL classification correct [PASS]" << std::endl;

    // Test LOW classification
    assert(thresholds.ClassifyVolume(800) == AMT::VAPDensityClass::LOW);
    assert(thresholds.ClassifyVolume(100) == AMT::VAPDensityClass::LOW);
    std::cout << "  LOW classification correct [PASS]" << std::endl;

    // Test zero volume returns NORMAL (not LOW)
    assert(thresholds.ClassifyVolume(0) == AMT::VAPDensityClass::NORMAL);
    std::cout << "  Zero volume returns NORMAL [PASS]" << std::endl;

    // Test invalid thresholds return NORMAL
    AMT::VolumeThresholds invalid;
    assert(invalid.ClassifyVolume(1500) == AMT::VAPDensityClass::NORMAL);
    std::cout << "  Invalid thresholds return NORMAL [PASS]" << std::endl;
}

void TestVolumeThresholdsNeedsRefresh() {
    std::cout << "\nTesting VolumeThresholds NeedsRefresh..." << std::endl;

    AMT::VolumeThresholds thresholds;

    // Invalid thresholds always need refresh
    assert(thresholds.NeedsRefresh(100, 10) == true);
    std::cout << "  Invalid thresholds need refresh [PASS]" << std::endl;

    // Make valid
    thresholds.valid = true;
    thresholds.computedAtBar = 100;

    // Same bar - no refresh needed
    assert(thresholds.NeedsRefresh(100, 10) == false);
    std::cout << "  Same bar - no refresh [PASS]" << std::endl;

    // Within interval - no refresh
    assert(thresholds.NeedsRefresh(105, 10) == false);
    std::cout << "  Within interval - no refresh [PASS]" << std::endl;

    // At interval - needs refresh
    assert(thresholds.NeedsRefresh(110, 10) == true);
    std::cout << "  At interval - needs refresh [PASS]" << std::endl;
}

// ============================================================================
// TEST 3: SessionVolumeProfile ComputeThresholds
// ============================================================================

void TestSessionVolumeProfileThresholds() {
    std::cout << "\nTesting SessionVolumeProfile ComputeThresholds..." << std::endl;

    SessionVolumeProfile profile;
    profile.tick_size = 0.25;

    // Add some volume levels with known distribution
    // Mean should be 1000, with clear outliers
    profile.AddLevel(20000, 500);   // Low volume
    profile.AddLevel(20001, 800);   // Below mean
    profile.AddLevel(20002, 1000);  // At mean
    profile.AddLevel(20003, 1000);  // At mean
    profile.AddLevel(20004, 1200);  // Above mean
    profile.AddLevel(20005, 1500);  // High volume

    profile.ComputeThresholds(100);

    assert(profile.cachedThresholds.valid == true);
    assert(profile.cachedThresholds.sampleSize == 6);
    std::cout << "  Thresholds computed and valid [PASS]" << std::endl;

    // Mean should be 1000
    double expectedMean = (500 + 800 + 1000 + 1000 + 1200 + 1500) / 6.0;
    assert(std::abs(profile.cachedThresholds.mean - expectedMean) < 0.01);
    std::cout << "  Mean = " << profile.cachedThresholds.mean << " [PASS]" << std::endl;

    // Verify stddev is positive
    assert(profile.cachedThresholds.stddev > 0);
    std::cout << "  StdDev = " << profile.cachedThresholds.stddev << " [PASS]" << std::endl;

    // Verify thresholds
    assert(profile.cachedThresholds.hvnThreshold > profile.cachedThresholds.mean);
    assert(profile.cachedThresholds.lvnThreshold < profile.cachedThresholds.mean);
    std::cout << "  HVN threshold = " << profile.cachedThresholds.hvnThreshold << std::endl;
    std::cout << "  LVN threshold = " << profile.cachedThresholds.lvnThreshold << " [PASS]" << std::endl;

    // Test classification using cached thresholds
    assert(profile.cachedThresholds.ClassifyVolume(1500) == AMT::VAPDensityClass::HIGH);
    assert(profile.cachedThresholds.ClassifyVolume(500) == AMT::VAPDensityClass::LOW);
    std::cout << "  Classification using thresholds correct [PASS]" << std::endl;
}

void TestSessionVolumeProfileEmptyProfile() {
    std::cout << "\nTesting SessionVolumeProfile with empty profile..." << std::endl;

    SessionVolumeProfile profile;
    profile.tick_size = 0.25;

    profile.ComputeThresholds(100);

    assert(profile.cachedThresholds.valid == false);
    std::cout << "  Empty profile: thresholds not valid [PASS]" << std::endl;
}

// ============================================================================
// TEST 4: GetVbPContextAtPrice - Value Area Position
// ============================================================================

void TestGetVbPContextValueAreaPosition() {
    std::cout << "\nTesting GetVbPContextAtPrice Value Area position..." << std::endl;

    SessionVolumeProfile profile;
    profile.tick_size = 0.25;
    profile.session_poc = 5000.00;
    profile.session_vah = 5010.00;
    profile.session_val = 4990.00;

    // Add volume data
    for (int tick = 19960; tick <= 20040; ++tick) {
        profile.AddLevel(tick, 1000);
    }
    profile.ComputeThresholds(100);

    // Test at POC
    VbPLevelContext ctx1 = GetVbPContextAtPrice(profile, 5000.00, 0.25);
    assert(ctx1.valid == true);
    assert(ctx1.atPOC == true);
    assert(ctx1.insideValueArea == true);
    assert(ctx1.aboveVAH == false);
    assert(ctx1.belowVAL == false);
    std::cout << "  At POC: correct position flags [PASS]" << std::endl;

    // Test inside value area (not at POC)
    VbPLevelContext ctx2 = GetVbPContextAtPrice(profile, 5005.00, 0.25);
    assert(ctx2.valid == true);
    assert(ctx2.atPOC == false);
    assert(ctx2.insideValueArea == true);
    assert(ctx2.aboveVAH == false);
    assert(ctx2.belowVAL == false);
    std::cout << "  Inside VA: correct position flags [PASS]" << std::endl;

    // Test above VAH
    VbPLevelContext ctx3 = GetVbPContextAtPrice(profile, 5015.00, 0.25);
    assert(ctx3.valid == true);
    assert(ctx3.atPOC == false);
    assert(ctx3.insideValueArea == false);
    assert(ctx3.aboveVAH == true);
    assert(ctx3.belowVAL == false);
    std::cout << "  Above VAH: correct position flags [PASS]" << std::endl;

    // Test below VAL
    VbPLevelContext ctx4 = GetVbPContextAtPrice(profile, 4985.00, 0.25);
    assert(ctx4.valid == true);
    assert(ctx4.atPOC == false);
    assert(ctx4.insideValueArea == false);
    assert(ctx4.aboveVAH == false);
    assert(ctx4.belowVAL == true);
    std::cout << "  Below VAL: correct position flags [PASS]" << std::endl;
}

// ============================================================================
// TEST 5: GetVbPContextAtPrice - Volume Classification
// ============================================================================

void TestGetVbPContextVolumeClassification() {
    std::cout << "\nTesting GetVbPContextAtPrice volume classification..." << std::endl;

    SessionVolumeProfile profile;
    profile.tick_size = 0.25;
    profile.session_poc = 5000.00;
    profile.session_vah = 5010.00;
    profile.session_val = 4990.00;

    // Create a profile with varied volume
    // Most levels have volume 1000 (normal)
    for (int tick = 19960; tick <= 20040; ++tick) {
        profile.AddLevel(tick, 1000);
    }
    // Add HVN at tick 20000 (price 5000.00)
    profile.volume_profile[20000].Volume = 5000;
    // Add LVN at tick 20020 (price 5005.00)
    profile.volume_profile[20020].Volume = 100;

    profile.ComputeThresholds(100);

    // Test HVN detection
    VbPLevelContext ctxHVN = GetVbPContextAtPrice(profile, 5000.00, 0.25);
    assert(ctxHVN.valid == true);
    assert(ctxHVN.isHVN == true);
    assert(ctxHVN.isLVN == false);
    assert(ctxHVN.volumeAtPrice == 5000);
    std::cout << "  HVN detection correct [PASS]" << std::endl;

    // Test LVN detection
    VbPLevelContext ctxLVN = GetVbPContextAtPrice(profile, 5005.00, 0.25);
    assert(ctxLVN.valid == true);
    assert(ctxLVN.isHVN == false);
    assert(ctxLVN.isLVN == true);
    assert(ctxLVN.volumeAtPrice == 100);
    std::cout << "  LVN detection correct [PASS]" << std::endl;

    // Test NORMAL volume
    VbPLevelContext ctxNormal = GetVbPContextAtPrice(profile, 4995.00, 0.25);
    assert(ctxNormal.valid == true);
    assert(ctxNormal.isHVN == false);
    assert(ctxNormal.isLVN == false);
    std::cout << "  NORMAL volume detection correct [PASS]" << std::endl;
}

// ============================================================================
// TEST 6: GetVbPContextAtPrice - Volume Percentile
// ============================================================================

void TestGetVbPContextVolumePercentile() {
    std::cout << "\nTesting GetVbPContextAtPrice volume percentile..." << std::endl;

    SessionVolumeProfile profile;
    profile.tick_size = 0.25;

    // Create a simple profile
    profile.AddLevel(20000, 1000);  // Max volume
    profile.AddLevel(20001, 500);   // 50% of max
    profile.AddLevel(20002, 250);   // 25% of max

    profile.session_poc = 5000.00;
    profile.session_vah = 5001.00;
    profile.session_val = 4999.00;
    profile.ComputeThresholds(100);

    // Test percentile at max volume
    VbPLevelContext ctx1 = GetVbPContextAtPrice(profile, 5000.00, 0.25);
    assert(std::abs(ctx1.volumePercentile - 1.0) < 0.01);
    std::cout << "  Percentile at max = 1.0 [PASS]" << std::endl;

    // Test percentile at 50% volume
    VbPLevelContext ctx2 = GetVbPContextAtPrice(profile, 5000.25, 0.25);
    assert(std::abs(ctx2.volumePercentile - 0.5) < 0.01);
    std::cout << "  Percentile at 50% = 0.5 [PASS]" << std::endl;

    // Test percentile at 25% volume
    VbPLevelContext ctx3 = GetVbPContextAtPrice(profile, 5000.50, 0.25);
    assert(std::abs(ctx3.volumePercentile - 0.25) < 0.01);
    std::cout << "  Percentile at 25% = 0.25 [PASS]" << std::endl;
}

// ============================================================================
// TEST 7: GetVbPContextAtPrice - Nearest HVN/LVN
// ============================================================================

void TestGetVbPContextNearestNodes() {
    std::cout << "\nTesting GetVbPContextAtPrice nearest HVN/LVN..." << std::endl;

    SessionVolumeProfile profile;
    profile.tick_size = 0.25;
    profile.session_poc = 5000.00;
    profile.session_vah = 5020.00;
    profile.session_val = 4980.00;

    // Add volume data
    for (int tick = 19900; tick <= 20100; ++tick) {
        profile.AddLevel(tick, 1000);
    }
    profile.ComputeThresholds(100);

    // Set HVN and LVN levels
    profile.session_hvn = {5000.00, 5010.00, 5020.00};
    profile.session_lvn = {4990.00, 5015.00};

    // Test from price 5005.00
    VbPLevelContext ctx = GetVbPContextAtPrice(profile, 5005.00, 0.25);
    assert(ctx.valid == true);

    // Nearest HVN should be 5000.00 (5 ticks away) or 5010.00 (5 ticks away)
    assert(ctx.nearestHVN == 5000.00 || ctx.nearestHVN == 5010.00);
    assert(std::abs(ctx.distToHVNTicks - 20.0) < 0.1);  // 5.00 / 0.25 = 20 ticks
    std::cout << "  Nearest HVN found at " << ctx.nearestHVN << " (" << ctx.distToHVNTicks << " ticks) [PASS]" << std::endl;

    // Nearest LVN should be 5015.00 (10 ticks away) closer than 4990.00 (15 ticks away)
    assert(ctx.nearestLVN == 5015.00);
    assert(std::abs(ctx.distToLVNTicks - 40.0) < 0.1);  // 10.00 / 0.25 = 40 ticks
    std::cout << "  Nearest LVN found at " << ctx.nearestLVN << " (" << ctx.distToLVNTicks << " ticks) [PASS]" << std::endl;
}

// ============================================================================
// TEST 8: GetVbPContextAtPrice - Edge Cases
// ============================================================================

void TestGetVbPContextEdgeCases() {
    std::cout << "\nTesting GetVbPContextAtPrice edge cases..." << std::endl;

    SessionVolumeProfile profile;
    profile.tick_size = 0.25;

    // Test empty profile
    VbPLevelContext ctx1 = GetVbPContextAtPrice(profile, 5000.00, 0.25);
    assert(ctx1.valid == false);
    std::cout << "  Empty profile returns invalid context [PASS]" << std::endl;

    // Add data
    profile.AddLevel(20000, 1000);
    profile.session_poc = 5000.00;
    profile.session_vah = 5010.00;
    profile.session_val = 4990.00;

    // Test invalid tick size
    VbPLevelContext ctx2 = GetVbPContextAtPrice(profile, 5000.00, 0.0);
    assert(ctx2.valid == false);
    std::cout << "  Zero tick size returns invalid context [PASS]" << std::endl;

    VbPLevelContext ctx3 = GetVbPContextAtPrice(profile, 5000.00, -0.25);
    assert(ctx3.valid == false);
    std::cout << "  Negative tick size returns invalid context [PASS]" << std::endl;

    // Test invalid price
    VbPLevelContext ctx4 = GetVbPContextAtPrice(profile, 0.0, 0.25);
    assert(ctx4.valid == false);
    std::cout << "  Zero price returns invalid context [PASS]" << std::endl;

    VbPLevelContext ctx5 = GetVbPContextAtPrice(profile, -5000.0, 0.25);
    assert(ctx5.valid == false);
    std::cout << "  Negative price returns invalid context [PASS]" << std::endl;
}

// ============================================================================
// TEST 9: GetVbPContextAtPrice - Fallback Classification (no cached thresholds)
// ============================================================================

void TestGetVbPContextFallbackClassification() {
    std::cout << "\nTesting GetVbPContextAtPrice fallback classification..." << std::endl;

    SessionVolumeProfile profile;
    profile.tick_size = 0.25;
    profile.session_poc = 5000.00;
    profile.session_vah = 5010.00;
    profile.session_val = 4990.00;

    // Add volume data but DON'T compute thresholds (test fallback path)
    for (int tick = 19960; tick <= 20040; ++tick) {
        profile.AddLevel(tick, 1000);
    }
    // Add HVN
    profile.volume_profile[20000].Volume = 5000;
    // Add LVN
    profile.volume_profile[20020].Volume = 100;

    // Ensure thresholds are NOT valid
    assert(profile.cachedThresholds.valid == false);

    // Test HVN detection with fallback path
    VbPLevelContext ctxHVN = GetVbPContextAtPrice(profile, 5000.00, 0.25);
    assert(ctxHVN.valid == true);
    assert(ctxHVN.isHVN == true);
    std::cout << "  Fallback HVN detection correct [PASS]" << std::endl;

    // Test LVN detection with fallback path
    VbPLevelContext ctxLVN = GetVbPContextAtPrice(profile, 5005.00, 0.25);
    assert(ctxLVN.valid == true);
    assert(ctxLVN.isLVN == true);
    std::cout << "  Fallback LVN detection correct [PASS]" << std::endl;
}

// ============================================================================
// TEST 10: Single Print Flag Detection
// ============================================================================

void TestSinglePrintFlagDetection() {
    std::cout << "\nTesting Single Print flag detection..." << std::endl;

    SessionVolumeProfile profile;
    profile.tick_size = 0.25;
    profile.session_poc = 5000.00;
    profile.session_vah = 5010.00;
    profile.session_val = 4990.00;

    // Create profile with normal volume ~1000
    for (int tick = 19960; tick <= 20040; ++tick) {
        profile.AddLevel(tick, 1000);
    }
    // Add single print (very low volume, < 30% of mean)
    profile.volume_profile[20020].Volume = 50;  // Very low - should trigger SINGLE_PRINT

    profile.ComputeThresholds(100);

    VbPLevelContext ctx = GetVbPContextAtPrice(profile, 5005.00, 0.25);
    assert(ctx.valid == true);
    assert(ctx.isLVN == true);
    // Check if SINGLE_PRINT flag is set
    bool hasSinglePrint = AMT::HasFlag(ctx.classification.flags, AMT::NodeFlags::SINGLE_PRINT);
    assert(hasSinglePrint == true);
    std::cout << "  Single print flag detected [PASS]" << std::endl;
}

// ============================================================================
// TEST 11: VolumeNodeClassification ToLegacyType
// ============================================================================

void TestVolumeNodeClassificationLegacy() {
    std::cout << "\nTesting VolumeNodeClassification ToLegacyType..." << std::endl;

    AMT::VolumeNodeClassification cls;

    // HVN + Responsive
    cls.density = AMT::VAPDensityClass::HIGH;
    cls.intent = AMT::FlowIntent::RESPONSIVE;
    assert(cls.ToLegacyType() == AMT::VolumeNodeType::HVN_RESPONSIVE);
    std::cout << "  HVN + RESPONSIVE = HVN_RESPONSIVE [PASS]" << std::endl;

    // HVN + Initiative
    cls.intent = AMT::FlowIntent::INITIATIVE;
    assert(cls.ToLegacyType() == AMT::VolumeNodeType::HVN_INITIATIVE);
    std::cout << "  HVN + INITIATIVE = HVN_INITIATIVE [PASS]" << std::endl;

    // HVN + Neutral
    cls.intent = AMT::FlowIntent::NEUTRAL;
    assert(cls.ToLegacyType() == AMT::VolumeNodeType::HVN_BALANCED);
    std::cout << "  HVN + NEUTRAL = HVN_BALANCED [PASS]" << std::endl;

    // LVN + Single Print
    cls.density = AMT::VAPDensityClass::LOW;
    cls.flags = AMT::NodeFlags::SINGLE_PRINT;
    assert(cls.ToLegacyType() == AMT::VolumeNodeType::LVN_SINGLE_PRINT);
    std::cout << "  LVN + SINGLE_PRINT = LVN_SINGLE_PRINT [PASS]" << std::endl;

    // LVN without Single Print
    cls.flags = AMT::NodeFlags::NONE;
    assert(cls.ToLegacyType() == AMT::VolumeNodeType::LVN_GAP);
    std::cout << "  LVN = LVN_GAP [PASS]" << std::endl;

    // Normal
    cls.density = AMT::VAPDensityClass::NORMAL;
    assert(cls.ToLegacyType() == AMT::VolumeNodeType::NORMAL);
    std::cout << "  NORMAL = NORMAL [PASS]" << std::endl;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "=== VBP (Volume By Price) Unit Tests ===" << std::endl;
    std::cout << "Tests VbPLevelContext, VolumeThresholds, GetVbPContextAtPrice\n" << std::endl;

    // VbPLevelContext tests
    TestVbPLevelContextDefaults();
    TestVbPLevelContextSyncFromClassification();

    // VolumeThresholds tests
    TestVolumeThresholdsClassification();
    TestVolumeThresholdsNeedsRefresh();

    // SessionVolumeProfile tests
    TestSessionVolumeProfileThresholds();
    TestSessionVolumeProfileEmptyProfile();

    // GetVbPContextAtPrice tests
    TestGetVbPContextValueAreaPosition();
    TestGetVbPContextVolumeClassification();
    TestGetVbPContextVolumePercentile();
    TestGetVbPContextNearestNodes();
    TestGetVbPContextEdgeCases();
    TestGetVbPContextFallbackClassification();
    TestSinglePrintFlagDetection();

    // Legacy compatibility tests
    TestVolumeNodeClassificationLegacy();

    std::cout << "\n=== All VBP tests passed! ===" << std::endl;
    return 0;
}
