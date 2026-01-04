// test_profile_baselines.cpp
// Tests for progress-conditioned profile baselines
// Verifies:
// 1. ProgressBucket enum and helper functions
// 2. ProfileFeatureSnapshot struct
// 3. ProfileMaturityResult and CheckProfileMaturity
// 4. HistoricalProfileBaseline storage and retrieval
//
// Build: g++ -std=c++17 -I.. -o test_profile_baselines.exe test_profile_baselines.cpp
// Run: ./test_profile_baselines.exe

#include <iostream>
#include <cassert>
#include <cmath>
#include <vector>
#include <string>

// Mock Sierra Chart types for standalone testing
#ifndef _SIERRACHART_H_
#define _SIERRACHART_H_
typedef float SCFloatArrayRef;
struct SCDateTime { double GetTimeInSeconds() const { return 0; } };
struct SCString {
    void Format(const char*, ...) {}
    const char* GetChars() const { return ""; }
};
#endif

// Include the RollingDist standalone (minimal version for testing)
namespace AMT {
    struct RollingDist {
        std::vector<double> samples;
        size_t maxSamples = 300;

        void reset(size_t max = 300) { samples.clear(); maxSamples = max; }
        void push(double val) {
            if (samples.size() >= maxSamples) samples.erase(samples.begin());
            samples.push_back(val);
        }
        size_t size() const { return samples.size(); }
        double mean() const {
            if (samples.empty()) return 0.0;
            double sum = 0;
            for (auto v : samples) sum += v;
            return sum / samples.size();
        }
        double percentileRank(double val) const {
            if (samples.empty()) return 50.0;
            int below = 0;
            for (auto v : samples) if (v < val) below++;
            return 100.0 * below / samples.size();
        }
    };
}

// Now include the profile baselines header
// Note: We need to define the enums and structs manually since we can't include the full header chain

namespace AMT {

// ProgressBucket enum
enum class ProgressBucket : int {
    BUCKET_15M = 0,
    BUCKET_30M = 1,
    BUCKET_60M = 2,
    BUCKET_120M = 3,
    BUCKET_EOD = 4,
    BUCKET_COUNT = 5
};

inline const char* ProgressBucketToString(ProgressBucket bucket) {
    switch (bucket) {
        case ProgressBucket::BUCKET_15M:  return "15m";
        case ProgressBucket::BUCKET_30M:  return "30m";
        case ProgressBucket::BUCKET_60M:  return "60m";
        case ProgressBucket::BUCKET_120M: return "120m";
        case ProgressBucket::BUCKET_EOD:  return "EOD";
        default: return "UNK";
    }
}

inline ProgressBucket GetProgressBucket(int minutesIntoSession) {
    if (minutesIntoSession >= 120) return ProgressBucket::BUCKET_120M;
    if (minutesIntoSession >= 60)  return ProgressBucket::BUCKET_60M;
    if (minutesIntoSession >= 30)  return ProgressBucket::BUCKET_30M;
    if (minutesIntoSession >= 15)  return ProgressBucket::BUCKET_15M;
    return ProgressBucket::BUCKET_15M;
}

// ProfileMaturity thresholds
// NO-FALLBACK POLICY: Volume is only checked when baseline available
namespace ProfileMaturity {
    constexpr int MIN_PRICE_LEVELS = 5;
    constexpr int MIN_BARS = 5;
    constexpr int MIN_MINUTES = 10;
    // Note: No absolute MIN_VOLUME - volume checked only when baseline available
}

// ProfileMaturityResult
struct ProfileMaturityResult {
    bool isMature = false;
    bool hasMinLevels = false;
    bool hasMinBars = false;
    bool hasMinMinutes = false;
    // Volume sufficiency (progress-conditioned, only applied when baseline ready)
    bool volumeSufficiencyValid = false;
    bool hasMinVolume = false;
    int priceLevels = 0;
    double totalVolume = 0.0;
    int sessionBars = 0;
    int sessionMinutes = 0;
    const char* gateFailedReason = nullptr;
};

// Simple version (NO baseline available - volume gate NOT applied)
inline ProfileMaturityResult CheckProfileMaturity(
    int priceLevels, double totalVolume, int sessionBars, int sessionMinutes)
{
    ProfileMaturityResult result;
    result.priceLevels = priceLevels;
    result.totalVolume = totalVolume;
    result.sessionBars = sessionBars;
    result.sessionMinutes = sessionMinutes;

    // Structural gates (always applied)
    result.hasMinLevels = (priceLevels >= ProfileMaturity::MIN_PRICE_LEVELS);
    result.hasMinBars = (sessionBars >= ProfileMaturity::MIN_BARS);
    result.hasMinMinutes = (sessionMinutes >= ProfileMaturity::MIN_MINUTES);

    // Volume sufficiency NOT AVAILABLE (no baseline in this simple version)
    result.volumeSufficiencyValid = false;
    result.hasMinVolume = false;

    // Maturity uses ONLY structural gates when no baseline
    result.isMature = result.hasMinLevels && result.hasMinBars && result.hasMinMinutes;

    if (!result.hasMinLevels) {
        result.gateFailedReason = "insufficient price levels";
    } else if (!result.hasMinBars) {
        result.gateFailedReason = "insufficient bars";
    } else if (!result.hasMinMinutes) {
        result.gateFailedReason = "insufficient minutes";
    }

    return result;
}

// ProfileFeatureSnapshot
struct ProfileFeatureSnapshot {
    ProgressBucket bucket = ProgressBucket::BUCKET_15M;
    int minutesIntoSession = 0;
    double vaWidthTicks = 0.0;
    double sessionRangeTicks = 0.0;
    double vaWidthRatio = 0.0;
    double pocShare = 0.0;
    bool valid = false;

    void ComputeDerived() {
        if (sessionRangeTicks > 0.0) {
            vaWidthRatio = vaWidthTicks / sessionRangeTicks;
        }
    }
};

// ProfileBaselineMinSamples
namespace ProfileBaselineMinSamples {
    constexpr size_t VA_WIDTH = 5;
    constexpr size_t POC_DOMINANCE = 5;
}

// HistoricalProfileBaseline
struct HistoricalProfileBaseline {
    RollingDist vaWidthTicks[static_cast<int>(ProgressBucket::BUCKET_COUNT)];
    RollingDist vaWidthRatio[static_cast<int>(ProgressBucket::BUCKET_COUNT)];
    RollingDist pocShare[static_cast<int>(ProgressBucket::BUCKET_COUNT)];
    int sessionsAccumulated = 0;
    bool initialized = false;

    void Reset(int maxSamples = 50) {
        for (int i = 0; i < static_cast<int>(ProgressBucket::BUCKET_COUNT); ++i) {
            vaWidthTicks[i].reset(maxSamples);
            vaWidthRatio[i].reset(maxSamples);
            pocShare[i].reset(maxSamples);
        }
        sessionsAccumulated = 0;
        initialized = true;
    }

    void PushSnapshot(const ProfileFeatureSnapshot& snap) {
        if (!snap.valid) return;
        const int idx = static_cast<int>(snap.bucket);
        if (idx < 0 || idx >= static_cast<int>(ProgressBucket::BUCKET_COUNT)) return;
        vaWidthTicks[idx].push(snap.vaWidthTicks);
        if (snap.vaWidthRatio > 0.0) vaWidthRatio[idx].push(snap.vaWidthRatio);
        if (snap.pocShare > 0.0) pocShare[idx].push(snap.pocShare);
    }

    bool IsReady(ProgressBucket bucket, size_t minSamples = ProfileBaselineMinSamples::VA_WIDTH) const {
        const int idx = static_cast<int>(bucket);
        return vaWidthTicks[idx].size() >= minSamples;
    }

    double GetVAWidthPercentile(ProgressBucket bucket, double currentWidthTicks) const {
        const int idx = static_cast<int>(bucket);
        if (vaWidthTicks[idx].size() < ProfileBaselineMinSamples::VA_WIDTH) return -1.0;
        return vaWidthTicks[idx].percentileRank(currentWidthTicks);
    }
};

} // namespace AMT

// ============================================================================
// TEST FUNCTIONS
// ============================================================================

void test_progress_bucket() {
    std::cout << "\n=== TEST: Progress Bucket ===\n";

    // Test bucket determination
    assert(AMT::GetProgressBucket(5) == AMT::ProgressBucket::BUCKET_15M);
    std::cout << "[PASS] 5 min -> BUCKET_15M\n";

    assert(AMT::GetProgressBucket(15) == AMT::ProgressBucket::BUCKET_15M);
    std::cout << "[PASS] 15 min -> BUCKET_15M\n";

    assert(AMT::GetProgressBucket(30) == AMT::ProgressBucket::BUCKET_30M);
    std::cout << "[PASS] 30 min -> BUCKET_30M\n";

    assert(AMT::GetProgressBucket(45) == AMT::ProgressBucket::BUCKET_30M);
    std::cout << "[PASS] 45 min -> BUCKET_30M\n";

    assert(AMT::GetProgressBucket(60) == AMT::ProgressBucket::BUCKET_60M);
    std::cout << "[PASS] 60 min -> BUCKET_60M\n";

    assert(AMT::GetProgressBucket(120) == AMT::ProgressBucket::BUCKET_120M);
    std::cout << "[PASS] 120 min -> BUCKET_120M\n";

    assert(AMT::GetProgressBucket(300) == AMT::ProgressBucket::BUCKET_120M);
    std::cout << "[PASS] 300 min -> BUCKET_120M (capped)\n";

    // Test string conversion
    assert(std::string(AMT::ProgressBucketToString(AMT::ProgressBucket::BUCKET_30M)) == "30m");
    std::cout << "[PASS] BUCKET_30M -> \"30m\"\n";
}

void test_profile_maturity() {
    std::cout << "\n=== TEST: Profile Maturity (Structural Gates Only) ===\n";
    // NO-FALLBACK POLICY: Simple CheckProfileMaturity only checks structural gates
    // Volume is only checked when baseline is available (separate function)

    // Test immature profile (insufficient levels)
    {
        auto result = AMT::CheckProfileMaturity(3, 5000.0, 10, 20);
        assert(!result.isMature);
        assert(!result.hasMinLevels);
        assert(result.hasMinBars);
        assert(result.hasMinMinutes);
        assert(!result.volumeSufficiencyValid);  // No baseline = volume not checked
        assert(std::string(result.gateFailedReason) == "insufficient price levels");
        std::cout << "[PASS] Immature: insufficient levels\n";
    }

    // Test immature profile (insufficient bars)
    {
        auto result = AMT::CheckProfileMaturity(10, 5000.0, 3, 20);
        assert(!result.isMature);
        assert(result.hasMinLevels);
        assert(!result.hasMinBars);
        assert(result.hasMinMinutes);
        assert(!result.volumeSufficiencyValid);  // No baseline = volume not checked
        assert(std::string(result.gateFailedReason) == "insufficient bars");
        std::cout << "[PASS] Immature: insufficient bars\n";
    }

    // Test immature profile (insufficient minutes)
    {
        auto result = AMT::CheckProfileMaturity(10, 5000.0, 10, 5);
        assert(!result.isMature);
        assert(result.hasMinLevels);
        assert(result.hasMinBars);
        assert(!result.hasMinMinutes);
        assert(!result.volumeSufficiencyValid);  // No baseline = volume not checked
        assert(std::string(result.gateFailedReason) == "insufficient minutes");
        std::cout << "[PASS] Immature: insufficient minutes\n";
    }

    // Test mature profile (structural gates only - volume not checked without baseline)
    {
        auto result = AMT::CheckProfileMaturity(10, 5000.0, 10, 20);
        assert(result.isMature);
        assert(result.hasMinLevels);
        assert(result.hasMinBars);
        assert(result.hasMinMinutes);
        assert(!result.volumeSufficiencyValid);  // No baseline = volume not checked
        assert(!result.hasMinVolume);            // Meaningless without baseline
        assert(result.gateFailedReason == nullptr);
        std::cout << "[PASS] Mature profile passes structural gates\n";
    }

    // Note: Volume is intentionally NOT checked here
    // When volumeSufficiencyValid=false, volume is excluded from maturity decision
    std::cout << "[INFO] Volume sufficiency requires baseline (not tested here)\n";
}

void test_profile_feature_snapshot() {
    std::cout << "\n=== TEST: ProfileFeatureSnapshot ===\n";

    AMT::ProfileFeatureSnapshot snap;
    snap.bucket = AMT::ProgressBucket::BUCKET_30M;
    snap.minutesIntoSession = 35;
    snap.vaWidthTicks = 20.0;
    snap.sessionRangeTicks = 40.0;
    snap.ComputeDerived();
    snap.valid = true;

    assert(snap.vaWidthRatio == 0.5);
    std::cout << "[PASS] VA width ratio computed correctly (20/40 = 0.5)\n";

    // Test zero range case
    AMT::ProfileFeatureSnapshot snap2;
    snap2.vaWidthTicks = 10.0;
    snap2.sessionRangeTicks = 0.0;  // Edge case
    snap2.ComputeDerived();
    assert(snap2.vaWidthRatio == 0.0);
    std::cout << "[PASS] Zero range handled (vaWidthRatio = 0)\n";
}

void test_historical_baseline() {
    std::cout << "\n=== TEST: HistoricalProfileBaseline ===\n";

    AMT::HistoricalProfileBaseline baseline;
    baseline.Reset(50);

    // Verify initial state
    assert(baseline.initialized);
    assert(baseline.sessionsAccumulated == 0);
    assert(!baseline.IsReady(AMT::ProgressBucket::BUCKET_30M));
    std::cout << "[PASS] Baseline starts empty and not ready\n";

    // Add snapshots for BUCKET_30M
    for (int i = 0; i < 5; ++i) {
        AMT::ProfileFeatureSnapshot snap;
        snap.bucket = AMT::ProgressBucket::BUCKET_30M;
        snap.vaWidthTicks = 20.0 + i * 2;  // 20, 22, 24, 26, 28
        snap.vaWidthRatio = 0.5;
        snap.valid = true;
        baseline.PushSnapshot(snap);
    }

    // Now should be ready
    assert(baseline.IsReady(AMT::ProgressBucket::BUCKET_30M));
    std::cout << "[PASS] Baseline ready after 5 samples\n";

    // Other buckets should still not be ready
    assert(!baseline.IsReady(AMT::ProgressBucket::BUCKET_60M));
    std::cout << "[PASS] Unsampled bucket not ready\n";

    // Test percentile calculation
    // Values: 20, 22, 24, 26, 28
    // 25 should be above 22 and 24, below 26 and 28 -> ~40% or 60%
    double pct = baseline.GetVAWidthPercentile(AMT::ProgressBucket::BUCKET_30M, 25.0);
    assert(pct >= 0.0 && pct <= 100.0);
    std::cout << "[PASS] Percentile calculation returns valid value: " << pct << "%\n";

    // Test percentile for value below all samples
    double lowPct = baseline.GetVAWidthPercentile(AMT::ProgressBucket::BUCKET_30M, 15.0);
    assert(lowPct == 0.0);
    std::cout << "[PASS] Value below all samples -> 0%\n";

    // Test percentile for value above all samples
    double highPct = baseline.GetVAWidthPercentile(AMT::ProgressBucket::BUCKET_30M, 35.0);
    assert(highPct == 100.0);
    std::cout << "[PASS] Value above all samples -> 100%\n";

    // Test unavailable bucket returns -1
    double unavail = baseline.GetVAWidthPercentile(AMT::ProgressBucket::BUCKET_60M, 25.0);
    assert(unavail == -1.0);
    std::cout << "[PASS] Unavailable bucket returns -1\n";
}

void test_invalid_snapshot_rejected() {
    std::cout << "\n=== TEST: Invalid Snapshot Rejected ===\n";

    AMT::HistoricalProfileBaseline baseline;
    baseline.Reset(50);

    // Push invalid snapshot
    AMT::ProfileFeatureSnapshot invalidSnap;
    invalidSnap.bucket = AMT::ProgressBucket::BUCKET_30M;
    invalidSnap.vaWidthTicks = 20.0;
    invalidSnap.valid = false;  // Invalid!
    baseline.PushSnapshot(invalidSnap);

    // Should not be added
    assert(!baseline.IsReady(AMT::ProgressBucket::BUCKET_30M));
    std::cout << "[PASS] Invalid snapshot not added to baseline\n";
}

int main() {
    std::cout << "=== PROFILE BASELINE TESTS ===\n";

    test_progress_bucket();
    test_profile_maturity();
    test_profile_feature_snapshot();
    test_historical_baseline();
    test_invalid_snapshot_rejected();

    std::cout << "\n=== ALL TESTS PASSED ===\n";
    return 0;
}
