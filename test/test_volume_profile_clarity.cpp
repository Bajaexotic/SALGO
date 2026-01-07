// Define standalone test mode to exclude SC-dependent functions
#define AMT_STANDALONE_TEST

// test_volume_profile_clarity.cpp - Verify volumeProfileClarity computation
// Tests the clarity formula, validity handling, and edge cases
#include "test_sierrachart_mock.h"
#include "../amt_core.h"
#include "../AMT_config.h"
#include "../AMT_Patterns.h"
#include "../AMT_Helpers.h"

#include <iostream>
#include <cassert>
#include <cmath>
#include <map>
#include <vector>
#include <algorithm>
#include <climits>
#include <cstdint>

using VolumeAtPrice = s_VolumeAtPriceV2;

// VolumeThresholds already defined in amt_core.h

// ============================================================================
// MINIMAL SessionVolumeProfile for testing
// ============================================================================

struct SessionVolumeProfile
{
    std::map<int, VolumeAtPrice> volume_profile;  // price_tick -> data
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

    void ComputeThresholds(int currentBar, double hvnSigmaCoeff = 1.5, double lvnSigmaCoeff = 0.5)
    {
        cachedThresholds.Reset();

        if (volume_profile.size() < 5)
            return;

        double totalVol = 0.0;
        double maxVol = 0.0;
        for (const auto& kv : volume_profile)
        {
            const double vol = static_cast<double>(kv.second.Volume);
            totalVol += vol;
            if (vol > maxVol) maxVol = vol;
        }

        const size_t numLevels = volume_profile.size();
        const double mean = totalVol / numLevels;

        double variance = 0.0;
        for (const auto& kv : volume_profile)
        {
            double diff = static_cast<double>(kv.second.Volume) - mean;
            variance += diff * diff;
        }
        const double stddev = std::sqrt(variance / numLevels);

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
};

// ============================================================================
// COPIED FROM AMT_VolumeProfile.h - ProfileClarityResult and ComputeVolumeProfileClarity
// ============================================================================

struct ProfileClarityResult
{
    float clarity = 0.0f;
    bool valid = false;
    float pocDominance = 0.0f;
    float vaCompactness = 0.0f;
    float unimodality = 0.0f;
    double pocVolume = 0.0;
    double meanVolume = 0.0;
    double stddevVolume = 0.0;
    int vaWidthTicks = 0;
    int profileRangeTicks = 0;
    int hvnCount = 0;
};

inline ProfileClarityResult ComputeVolumeProfileClarity(
    const SessionVolumeProfile& profile,
    double tickSize)
{
    ProfileClarityResult result;

    if (tickSize <= 0.0)
        return result;

    if (profile.volume_profile.size() < 5)
        return result;

    if (!profile.cachedThresholds.valid)
        return result;

    if (!AMT::IsValidPrice(profile.session_poc))
        return result;

    if (!AMT::IsValidPrice(profile.session_vah) || !AMT::IsValidPrice(profile.session_val))
        return result;

    if (profile.session_vah < profile.session_val)
        return result;

    const double mean = profile.cachedThresholds.mean;
    const double stddev = profile.cachedThresholds.stddev;
    const double maxVol = profile.cachedThresholds.maxLevelVolume;

    if (mean <= 0.0 || stddev <= 0.0 || maxVol <= 0.0)
        return result;

    const int pocTick = static_cast<int>(AMT::PriceToTicks(profile.session_poc, tickSize));
    double pocVol = 0.0;

    auto it = profile.volume_profile.find(pocTick);
    if (it != profile.volume_profile.end())
    {
        pocVol = static_cast<double>(it->second.Volume);
    }
    else
    {
        for (int offset = -1; offset <= 1; ++offset)
        {
            auto nearby = profile.volume_profile.find(pocTick + offset);
            if (nearby != profile.volume_profile.end())
            {
                if (static_cast<double>(nearby->second.Volume) > pocVol)
                    pocVol = static_cast<double>(nearby->second.Volume);
            }
        }
    }

    if (pocVol <= 0.0)
        return result;

    int minTick = INT_MAX, maxTick = INT_MIN;
    for (const auto& kv : profile.volume_profile)
    {
        if (kv.first < minTick) minTick = kv.first;
        if (kv.first > maxTick) maxTick = kv.first;
    }

    const int profileRangeTicks = maxTick - minTick + 1;
    if (profileRangeTicks < 3)
        return result;

    const int vahTick = static_cast<int>(AMT::PriceToTicks(profile.session_vah, tickSize));
    const int valTick = static_cast<int>(AMT::PriceToTicks(profile.session_val, tickSize));
    const int vaWidthTicks = vahTick - valTick + 1;

    if (vaWidthTicks < 1)
        return result;

    result.pocVolume = pocVol;
    result.meanVolume = mean;
    result.stddevVolume = stddev;
    result.vaWidthTicks = vaWidthTicks;
    result.profileRangeTicks = profileRangeTicks;
    result.hvnCount = static_cast<int>(profile.session_hvn.size());

    // POC Dominance
    {
        constexpr double DOMINANCE_SIGMA_SCALE = 3.0;
        const double zScore = (pocVol - mean) / stddev;
        const double rawDominance = zScore / DOMINANCE_SIGMA_SCALE;
        result.pocDominance = static_cast<float>(
            (std::max)(0.0, (std::min)(1.0, rawDominance)));
    }

    // VA Compactness
    {
        constexpr double COMPACTNESS_TARGET_RATIO = 0.70;
        const double vaRatio = static_cast<double>(vaWidthTicks) / profileRangeTicks;
        const double rawCompactness = 1.0 - (vaRatio / COMPACTNESS_TARGET_RATIO);
        result.vaCompactness = static_cast<float>(
            (std::max)(0.0, (std::min)(1.0, rawCompactness)));
    }

    // Unimodality
    {
        constexpr int MAX_PENALTY_PEAKS = 3;
        const int hvnCount = static_cast<int>(profile.session_hvn.size());
        const int excessPeaks = (std::max)(0, hvnCount - 1);
        const double penaltyRatio = static_cast<double>(excessPeaks) / MAX_PENALTY_PEAKS;
        const double rawUnimodality = 1.0 - (std::min)(1.0, penaltyRatio);
        result.unimodality = static_cast<float>(rawUnimodality);
    }

    // Composite
    {
        constexpr float W_DOMINANCE = 0.40f;
        constexpr float W_COMPACTNESS = 0.35f;
        constexpr float W_UNIMODALITY = 0.25f;

        const float rawClarity =
            (W_DOMINANCE * result.pocDominance) +
            (W_COMPACTNESS * result.vaCompactness) +
            (W_UNIMODALITY * result.unimodality);

        result.clarity = (std::max)(0.0f, (std::min)(1.0f, rawClarity));
        result.valid = true;
    }

    return result;
}

// ============================================================================
// HELPER: Create synthetic profile for testing
// ============================================================================

struct SyntheticProfile {
    SessionVolumeProfile profile;
    double tickSize = 0.25;

    void reset() {
        profile.Reset(tickSize);
    }

    void addLevel(int priceTick, uint64_t volume) {
        VolumeAtPrice vap;
        vap.PriceInTicks = priceTick;
        vap.Volume = volume;
        vap.BidVolume = volume / 2;
        vap.AskVolume = volume - vap.BidVolume;
        profile.volume_profile[priceTick] = vap;
    }

    void setPOC(double price) { profile.session_poc = price; }
    void setVAH(double price) { profile.session_vah = price; }
    void setVAL(double price) { profile.session_val = price; }
    void addHVN(double price) { profile.session_hvn.push_back(price); }
    void addLVN(double price) { profile.session_lvn.push_back(price); }
    void computeThresholds() { profile.ComputeThresholds(0); }
};

// ============================================================================
// TEST: Clear single-peak narrow-VA profile => high clarity
// ============================================================================

void test_high_clarity_single_peak_narrow_va() {
    std::cout << "=== Test: High clarity - single peak, narrow VA ===" << std::endl;

    SyntheticProfile sp;
    sp.reset();

    for (int tick = 24000; tick <= 24040; tick++) {
        int distFromPOC = std::abs(tick - 24020);
        uint64_t vol = static_cast<uint64_t>(1000 - distFromPOC * 20);
        if (vol < 100) vol = 100;
        sp.addLevel(tick, vol);
    }

    sp.setPOC(24020 * sp.tickSize);
    sp.setVAH(24022 * sp.tickSize);
    sp.setVAL(24018 * sp.tickSize);
    sp.addHVN(24020 * sp.tickSize);
    sp.computeThresholds();

    ProfileClarityResult result = ComputeVolumeProfileClarity(sp.profile, sp.tickSize);

    std::cout << "  Result valid: " << result.valid << std::endl;
    std::cout << "  Clarity: " << result.clarity << std::endl;
    std::cout << "  Components: dom=" << result.pocDominance
              << " compact=" << result.vaCompactness
              << " uni=" << result.unimodality << std::endl;

    assert(result.valid == true);
    assert(result.clarity > 0.5f);
    assert(result.pocDominance > 0.3f);
    assert(result.vaCompactness > 0.5f);
    assert(result.unimodality > 0.9f);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Flat/broad profile => low clarity
// ============================================================================

void test_low_clarity_flat_profile() {
    std::cout << "=== Test: Low clarity - nearly flat profile ===" << std::endl;

    SyntheticProfile sp;
    sp.reset();

    // Create a nearly-flat profile with slight random noise
    // (pure flat would have stddev=0 which is invalid)
    for (int tick = 24000; tick <= 24040; tick++) {
        // Add small variation so POC at center has slightly higher volume
        int distFromCenter = std::abs(tick - 24020);
        uint64_t vol = 500 + (20 - distFromCenter);  // 500-520 range, very flat
        sp.addLevel(tick, vol);
    }

    sp.setPOC(24020 * sp.tickSize);  // POC at center (slight advantage)
    sp.setVAH(24035 * sp.tickSize);  // Wide VA spanning most of range
    sp.setVAL(24005 * sp.tickSize);
    sp.computeThresholds();

    ProfileClarityResult result = ComputeVolumeProfileClarity(sp.profile, sp.tickSize);

    std::cout << "  Result valid: " << result.valid << std::endl;
    std::cout << "  Clarity: " << result.clarity << std::endl;
    std::cout << "  Components: dom=" << result.pocDominance
              << " compact=" << result.vaCompactness
              << " uni=" << result.unimodality << std::endl;

    assert(result.valid == true);
    // Nearly-flat profile should have low-to-moderate clarity
    // Note: Even with minimal variation, POC can still show dominance
    // The key indicator of "flatness" is the VA compactness (should be low)
    assert(result.clarity < 0.6f);
    // VA is wide - this is the key indicator of unclear acceptance
    assert(result.vaCompactness < 0.15f);  // Wide VA = low compactness

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Multi-peak distributed profile => penalized
// ============================================================================

void test_multimodal_penalized() {
    std::cout << "=== Test: Multimodal profile penalized ===" << std::endl;

    SyntheticProfile sp;
    sp.reset();

    for (int tick = 24000; tick <= 24040; tick++) {
        int dist1 = std::abs(tick - 24010);
        int dist2 = std::abs(tick - 24030);
        int minDist = std::min(dist1, dist2);
        uint64_t vol = static_cast<uint64_t>(1000 - minDist * 40);
        if (vol < 100) vol = 100;
        sp.addLevel(tick, vol);
    }

    sp.setPOC(24010 * sp.tickSize);
    sp.setVAH(24035 * sp.tickSize);
    sp.setVAL(24005 * sp.tickSize);
    sp.addHVN(24010 * sp.tickSize);
    sp.addHVN(24030 * sp.tickSize);
    sp.computeThresholds();

    ProfileClarityResult result = ComputeVolumeProfileClarity(sp.profile, sp.tickSize);

    std::cout << "  Result valid: " << result.valid << std::endl;
    std::cout << "  Clarity: " << result.clarity << std::endl;
    std::cout << "  Unimodality: " << result.unimodality << " (hvnCount=" << result.hvnCount << ")" << std::endl;

    assert(result.valid == true);
    assert(result.unimodality < 1.0f);
    assert(result.hvnCount == 2);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Missing profile data => invalid
// ============================================================================

void test_invalid_empty_profile() {
    std::cout << "=== Test: Empty profile => invalid ===" << std::endl;

    SyntheticProfile sp;
    sp.reset();

    ProfileClarityResult result = ComputeVolumeProfileClarity(sp.profile, sp.tickSize);

    std::cout << "  Result valid: " << result.valid << std::endl;
    assert(result.valid == false);
    assert(result.clarity == 0.0f);

    std::cout << "  PASSED" << std::endl;
}

void test_invalid_no_poc() {
    std::cout << "=== Test: No POC => invalid ===" << std::endl;

    SyntheticProfile sp;
    sp.reset();

    for (int tick = 24000; tick <= 24010; tick++) {
        sp.addLevel(tick, 500);
    }
    sp.computeThresholds();

    ProfileClarityResult result = ComputeVolumeProfileClarity(sp.profile, sp.tickSize);

    std::cout << "  Result valid: " << result.valid << std::endl;
    assert(result.valid == false);

    std::cout << "  PASSED" << std::endl;
}

void test_invalid_incoherent_va() {
    std::cout << "=== Test: VAH < VAL => invalid ===" << std::endl;

    SyntheticProfile sp;
    sp.reset();

    for (int tick = 24000; tick <= 24020; tick++) {
        sp.addLevel(tick, 500);
    }
    sp.setPOC(24010 * sp.tickSize);
    sp.setVAH(24005 * sp.tickSize);
    sp.setVAL(24015 * sp.tickSize);
    sp.computeThresholds();

    ProfileClarityResult result = ComputeVolumeProfileClarity(sp.profile, sp.tickSize);

    std::cout << "  Result valid: " << result.valid << std::endl;
    assert(result.valid == false);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Scoring integration
// ============================================================================

void test_scoring_with_valid_profile_clarity() {
    std::cout << "=== Test: calculate_score includes valid profile clarity ===" << std::endl;

    AMT::ConfidenceWeights w;
    AMT::ConfidenceAttribute conf;

    conf.domStrength = 0.8f;
    conf.domStrengthValid = true;
    conf.deltaConsistency = 0.6f;
    conf.deltaConsistencyValid = true;
    conf.volumeProfileClarity = 0.9f;
    conf.volumeProfileClarityValid = true;
    conf.tpoAcceptance = 0.5f;
    conf.tpoAcceptanceValid = true;
    conf.liquidityAvailability = 0.7f;
    conf.liquidityAvailabilityValid = true;

    AMT::ScoreResult result = conf.calculate_score(w);
    assert(result.scoreValid);
    std::cout << "  Score with valid profile clarity: " << result.score << std::endl;

    // All 5 metrics are valid, so full weighted sum with total weight = 1.0
    float expected = (0.8f*w.dom + 0.6f*w.delta + 0.9f*w.profile + 0.5f*w.tpo + 0.7f*w.liquidity);
    std::cout << "  Expected: " << expected << std::endl;

    assert(std::abs(result.score - expected) < 0.01f);

    std::cout << "  PASSED" << std::endl;
}

void test_scoring_with_invalid_profile_clarity() {
    std::cout << "=== Test: calculate_score excludes invalid profile clarity ===" << std::endl;

    AMT::ConfidenceWeights w;
    AMT::ConfidenceAttribute conf;

    conf.domStrength = 0.8f;
    conf.domStrengthValid = true;
    conf.deltaConsistency = 0.6f;
    conf.deltaConsistencyValid = true;
    conf.volumeProfileClarity = 0.9f;
    conf.volumeProfileClarityValid = false;  // Profile excluded
    conf.tpoAcceptance = 0.5f;
    conf.tpoAcceptanceValid = true;
    conf.liquidityAvailability = 0.7f;
    conf.liquidityAvailabilityValid = true;

    AMT::ScoreResult result = conf.calculate_score(w);
    assert(result.scoreValid);
    std::cout << "  Score with invalid profile clarity: " << result.score << std::endl;

    // Profile excluded, other 4 valid, so renormalize by their weight
    float expected = (0.8f*w.dom + 0.6f*w.delta + 0.5f*w.tpo + 0.7f*w.liquidity)
                   / (w.dom + w.delta + w.tpo + w.liquidity);
    std::cout << "  Expected (profile excluded): " << expected << std::endl;

    assert(std::abs(result.score - expected) < 0.01f);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Component ranges
// ============================================================================

void test_component_ranges() {
    std::cout << "=== Test: Component ranges [0, 1] ===" << std::endl;

    SyntheticProfile sp;

    // Test extreme high-clarity profile
    sp.reset();
    for (int tick = 24000; tick <= 24040; tick++) {
        int distFromPOC = std::abs(tick - 24020);
        uint64_t vol = (distFromPOC == 0) ? 10000 : 100;
        sp.addLevel(tick, vol);
    }
    sp.setPOC(24020 * sp.tickSize);
    sp.setVAH(24021 * sp.tickSize);
    sp.setVAL(24019 * sp.tickSize);
    sp.addHVN(24020 * sp.tickSize);
    sp.computeThresholds();

    ProfileClarityResult resultHigh = ComputeVolumeProfileClarity(sp.profile, sp.tickSize);

    std::cout << "  High clarity profile:" << std::endl;
    std::cout << "    clarity=" << resultHigh.clarity << std::endl;

    assert(resultHigh.pocDominance >= 0.0f && resultHigh.pocDominance <= 1.0f);
    assert(resultHigh.vaCompactness >= 0.0f && resultHigh.vaCompactness <= 1.0f);
    assert(resultHigh.unimodality >= 0.0f && resultHigh.unimodality <= 1.0f);
    assert(resultHigh.clarity >= 0.0f && resultHigh.clarity <= 1.0f);

    // Test extreme low-clarity profile (nearly flat, wide VA, many peaks)
    sp.reset();
    for (int tick = 24000; tick <= 24040; tick++) {
        // Small variation to avoid stddev=0
        int distFromCenter = std::abs(tick - 24020);
        uint64_t vol = 500 + (20 - distFromCenter);
        sp.addLevel(tick, vol);
    }
    sp.setPOC(24020 * sp.tickSize);
    sp.setVAH(24038 * sp.tickSize);
    sp.setVAL(24002 * sp.tickSize);
    sp.addHVN(24005 * sp.tickSize);
    sp.addHVN(24015 * sp.tickSize);
    sp.addHVN(24025 * sp.tickSize);
    sp.addHVN(24035 * sp.tickSize);
    sp.computeThresholds();

    ProfileClarityResult resultLow = ComputeVolumeProfileClarity(sp.profile, sp.tickSize);

    std::cout << "  Low clarity profile:" << std::endl;
    std::cout << "    clarity=" << resultLow.clarity << std::endl;

    assert(resultLow.pocDominance >= 0.0f && resultLow.pocDominance <= 1.0f);
    assert(resultLow.vaCompactness >= 0.0f && resultLow.vaCompactness <= 1.0f);
    assert(resultLow.unimodality >= 0.0f && resultLow.unimodality <= 1.0f);
    assert(resultLow.clarity >= 0.0f && resultLow.clarity <= 1.0f);

    std::cout << "  High clarity > Low clarity: "
              << (resultHigh.clarity > resultLow.clarity ? "YES" : "NO") << std::endl;
    assert(resultHigh.clarity > resultLow.clarity);

    std::cout << "  PASSED" << std::endl;
}

void test_default_validity_false() {
    std::cout << "=== Test: volumeProfileClarityValid default is false ===" << std::endl;

    AMT::ConfidenceAttribute conf;

    std::cout << "  volumeProfileClarityValid (default): " << conf.volumeProfileClarityValid << std::endl;
    std::cout << "  volumeProfileClarity (default): " << conf.volumeProfileClarity << std::endl;

    assert(conf.volumeProfileClarityValid == false);
    assert(conf.volumeProfileClarity == 0.0f);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Volume Profile Clarity Tests" << std::endl;
    std::cout << "========================================\n" << std::endl;

    std::cout << "--- Clarity Computation Tests ---\n" << std::endl;
    test_high_clarity_single_peak_narrow_va();
    test_low_clarity_flat_profile();
    test_multimodal_penalized();

    std::cout << "\n--- Invalid Data Tests ---\n" << std::endl;
    test_invalid_empty_profile();
    test_invalid_no_poc();
    test_invalid_incoherent_va();

    std::cout << "\n--- Scoring Integration Tests ---\n" << std::endl;
    test_scoring_with_valid_profile_clarity();
    test_scoring_with_invalid_profile_clarity();

    std::cout << "\n--- Range and Boundary Tests ---\n" << std::endl;
    test_component_ranges();
    test_default_validity_false();

    std::cout << "\n========================================" << std::endl;
    std::cout << "All tests PASSED!" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
}
