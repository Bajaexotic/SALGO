// test_new_baselines.cpp
// Standalone tests for new bucket-based baseline components (Dec 2024)
// Tests: RollingDist Try* APIs, EffortBaselineStore, SessionDeltaBaseline, DOMWarmup
//
// Build: g++ -std=c++17 -I.. -o test_new_baselines.exe test_new_baselines.cpp
// Run: ./test_new_baselines.exe

#include <iostream>
#include <cassert>
#include <cmath>
#include <deque>
#include <vector>
#include <algorithm>
#include <string>

// ============================================================================
// MOCK SIERRA CHART TYPES (for standalone compilation)
// ============================================================================
#ifndef _SIERRACHART_H_
#define _SIERRACHART_H_
struct SCDateTime { double GetTimeInSeconds() const { return 0; } };
struct SCString {
    void Format(const char*, ...) {}
    const char* GetChars() const { return ""; }
};
#endif

// ============================================================================
// INLINE DEFINITIONS FROM amt_core.h
// ============================================================================

namespace AMT {

enum class BaselineReadiness : int {
    READY = 0,
    WARMUP = 1,
    STALE = 2,
    UNAVAILABLE = 3
};

enum class BucketBaselineState : int {
    READY = 0,
    INSUFFICIENT_SESSIONS = 1,
    INSUFFICIENT_COVERAGE = 2,
    NOT_APPLICABLE = 3
};

enum class SessionBaselineState : int {
    READY = 0,
    INSUFFICIENT_SESSIONS = 1,
    NOT_APPLICABLE = 2
};

enum class DOMBaselineState : int {
    WARMUP_PENDING = 0,
    READY = 1
};

enum class EffortBucket : int {
    OPEN = 0,
    MID = 1,
    POWER = 2,
    COUNT = 3,
    OUTSIDE_RTH = -1
};

struct PercentileResult {
    double value = 0.0;
    bool valid = false;
    static PercentileResult Valid(double v) { return {v, true}; }
    static PercentileResult Invalid() { return {0.0, false}; }
};

struct MeanResult {
    double value = 0.0;
    bool valid = false;
    static MeanResult Valid(double v) { return {v, true}; }
    static MeanResult Invalid() { return {0.0, false}; }
};

inline int GetExpectedBarsInBucket(EffortBucket bucket, int rthStartSec, int rthEndSec, int barIntervalSeconds) {
    if (bucket == EffortBucket::OUTSIDE_RTH || barIntervalSeconds <= 0)
        return 0;
    int bucketDurationSec = 0;
    switch (bucket) {
        case EffortBucket::OPEN:  bucketDurationSec = 3600; break;
        case EffortBucket::POWER: bucketDurationSec = 3600; break;
        case EffortBucket::MID:   bucketDurationSec = (rthEndSec - rthStartSec) - 7200; break;
        default: return 0;
    }
    return bucketDurationSec / barIntervalSeconds;
}

// ============================================================================
// ROLLING DISTRIBUTION (from AMT_Snapshots.h)
// ============================================================================

struct RollingDist {
    std::deque<double> values;
    int window = 300;

    void reset(int w) {
        window = w;
        values.clear();
    }

    void push(double v) {
        if (!std::isfinite(v)) return;
        values.push_back(v);
        if (values.size() > static_cast<size_t>(window))
            values.pop_front();
    }

    size_t size() const { return values.size(); }

    double median() const {
        if (values.empty()) return 0.0;
        std::vector<double> sorted(values.begin(), values.end());
        std::sort(sorted.begin(), sorted.end());
        const size_t n = sorted.size();
        if (n % 2 == 0)
            return (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
        else
            return sorted[n / 2];
    }

    double mad() const {
        if (values.size() < 2) return 0.0;
        const double med = median();
        std::vector<double> absDevs;
        absDevs.reserve(values.size());
        for (double v : values)
            absDevs.push_back(std::abs(v - med));
        std::sort(absDevs.begin(), absDevs.end());
        const size_t n = absDevs.size();
        if (n % 2 == 0)
            return (absDevs[n / 2 - 1] + absDevs[n / 2]) / 2.0;
        else
            return absDevs[n / 2];
    }

    BaselineReadiness GetReadiness(size_t minSamples) const {
        if (values.empty()) return BaselineReadiness::UNAVAILABLE;
        if (values.size() < minSamples) return BaselineReadiness::WARMUP;
        return BaselineReadiness::READY;
    }

    bool IsReady(size_t minSamples) const {
        return GetReadiness(minSamples) == BaselineReadiness::READY;
    }

    // TRY* APIs (No-Fallback Contract)
    PercentileResult TryPercentile(double val) const {
        if (values.empty())
            return PercentileResult::Invalid();
        int countBelow = 0;
        for (double v : values)
            if (v < val) ++countBelow;
        const double pct = static_cast<double>(countBelow) /
            static_cast<double>(values.size()) * 100.0;
        return PercentileResult::Valid(pct);
    }

    PercentileResult TryPercentileRank(double val) const {
        if (values.empty())
            return PercentileResult::Invalid();
        const double med = median();
        const double m = mad();
        double pctRank;
        if (m < 1e-9) {
            pctRank = (val >= med) ? 75.0 : 25.0;
        } else {
            const double z = (val - med) / (m * 1.4826);
            const double p = 0.5 * (1.0 + std::erf(z / std::sqrt(2.0)));
            pctRank = p * 100.0;
        }
        return PercentileResult::Valid(pctRank);
    }

    MeanResult TryMean() const {
        if (values.empty())
            return MeanResult::Invalid();
        double sum = 0.0;
        for (double v : values) sum += v;
        return MeanResult::Valid(sum / static_cast<double>(values.size()));
    }

    MeanResult TryMedian() const {
        if (values.empty())
            return MeanResult::Invalid();
        return MeanResult::Valid(median());
    }
};

// ============================================================================
// EFFORT BUCKET DISTRIBUTION
// ============================================================================

struct EffortBucketDistribution {
    RollingDist vol_sec;
    RollingDist trades_sec;
    RollingDist delta_pct;
    RollingDist bar_range;

    int sessionsContributed = 0;
    int totalBarsPushed = 0;
    int expectedBarsPerSession = 0;

    static constexpr int REQUIRED_SESSIONS = 5;
    static constexpr double MIN_COVERAGE_RATIO = 0.5;

    void Reset(int window = 1500) {
        vol_sec.reset(window);
        trades_sec.reset(window);
        delta_pct.reset(window);
        bar_range.reset(window);
        sessionsContributed = 0;
        totalBarsPushed = 0;
        expectedBarsPerSession = 0;
    }

    void SetExpectedBarsPerSession(int expected) {
        expectedBarsPerSession = expected;
    }

    int GetMinBarsPerSession() const {
        if (expectedBarsPerSession <= 0) return 10;
        return static_cast<int>(expectedBarsPerSession * MIN_COVERAGE_RATIO);
    }

    BucketBaselineState GetState() const {
        if (sessionsContributed < REQUIRED_SESSIONS)
            return BucketBaselineState::INSUFFICIENT_SESSIONS;
        const int minTotalBars = REQUIRED_SESSIONS * GetMinBarsPerSession();
        if (totalBarsPushed < minTotalBars)
            return BucketBaselineState::INSUFFICIENT_COVERAGE;
        return BucketBaselineState::READY;
    }

    bool IsReady() const { return GetState() == BucketBaselineState::READY; }
};

// ============================================================================
// EFFORT BASELINE STORE
// ============================================================================

struct EffortBaselineStore {
    EffortBucketDistribution buckets[static_cast<int>(EffortBucket::COUNT)];

    void Reset(int window = 1500) {
        for (int i = 0; i < static_cast<int>(EffortBucket::COUNT); ++i)
            buckets[i].Reset(window);
    }

    EffortBucketDistribution& Get(EffortBucket b) {
        const int idx = static_cast<int>(b);
        if (idx < 0 || idx >= static_cast<int>(EffortBucket::COUNT))
            return buckets[0];
        return buckets[idx];
    }

    const EffortBucketDistribution& Get(EffortBucket b) const {
        const int idx = static_cast<int>(b);
        if (idx < 0 || idx >= static_cast<int>(EffortBucket::COUNT))
            return buckets[0];
        return buckets[idx];
    }

    void SetExpectedBarsPerSession(int rthStartSec, int rthEndSec, int barIntervalSeconds) {
        for (int i = 0; i < static_cast<int>(EffortBucket::COUNT); ++i) {
            const EffortBucket bucket = static_cast<EffortBucket>(i);
            const int expected = GetExpectedBarsInBucket(bucket, rthStartSec, rthEndSec, barIntervalSeconds);
            buckets[i].SetExpectedBarsPerSession(expected);
        }
    }

    bool AllBucketsReady() const {
        for (int i = 0; i < static_cast<int>(EffortBucket::COUNT); ++i) {
            if (!buckets[i].IsReady()) return false;
        }
        return true;
    }
};

// ============================================================================
// SESSION DELTA BASELINE
// ============================================================================

struct SessionDeltaBaseline {
    RollingDist session_delta_ratio;
    int sessionsContributed = 0;
    static constexpr int REQUIRED_SESSIONS = 5;

    void Reset(int window = 50) {
        session_delta_ratio.reset(window);
        sessionsContributed = 0;
    }

    void PushSessionAggregate(double sessionDeltaRatio) {
        session_delta_ratio.push(std::abs(sessionDeltaRatio));
    }

    void IncrementSessionCount() {
        sessionsContributed++;
    }

    SessionBaselineState GetState() const {
        if (sessionsContributed < REQUIRED_SESSIONS)
            return SessionBaselineState::INSUFFICIENT_SESSIONS;
        return SessionBaselineState::READY;
    }

    bool IsReady() const { return GetState() == SessionBaselineState::READY; }

    PercentileResult TryGetPercentile(double sessionDeltaRatio) const {
        if (!IsReady())
            return PercentileResult::Invalid();
        return session_delta_ratio.TryPercentile(std::abs(sessionDeltaRatio));
    }
};

// ============================================================================
// DOM WARMUP
// ============================================================================

struct DOMWarmup {
    static constexpr int WARMUP_DURATION_SEC = 15 * 60;

    RollingDist stackRate;
    RollingDist pullRate;
    RollingDist depthMassCore;

    int warmupStartTimeSec = 0;
    bool frozen = false;
    DOMBaselineState state = DOMBaselineState::WARMUP_PENDING;

    void Reset(int window = 300) {
        stackRate.reset(window);
        pullRate.reset(window);
        depthMassCore.reset(window);
        warmupStartTimeSec = 0;
        frozen = false;
        state = DOMBaselineState::WARMUP_PENDING;
    }

    void StartWarmup(int actualBarTimeSec) {
        warmupStartTimeSec = actualBarTimeSec;
        frozen = false;
        state = DOMBaselineState::WARMUP_PENDING;
        stackRate.reset(300);
        pullRate.reset(300);
        depthMassCore.reset(300);
    }

    void PushIfWarmup(double stack, double pull, double depth, int currentBarTimeSec) {
        if (frozen) return;
        stackRate.push(stack);
        pullRate.push(pull);
        depthMassCore.push(depth);
        const int elapsed = currentBarTimeSec - warmupStartTimeSec;
        if (elapsed >= WARMUP_DURATION_SEC) {
            frozen = true;
            state = DOMBaselineState::READY;
        }
    }

    int GetWarmupRemainingSeconds(int currentBarTimeSec) const {
        if (frozen) return 0;
        const int elapsed = currentBarTimeSec - warmupStartTimeSec;
        return std::max(0, WARMUP_DURATION_SEC - elapsed);
    }

    bool IsReady() const { return state == DOMBaselineState::READY; }

    PercentileResult TryStackPercentile(double val) const {
        if (!IsReady()) return PercentileResult::Invalid();
        return stackRate.TryPercentileRank(val);
    }

    PercentileResult TryPullPercentile(double val) const {
        if (!IsReady()) return PercentileResult::Invalid();
        return pullRate.TryPercentileRank(val);
    }

    PercentileResult TryDepthPercentile(double val) const {
        if (!IsReady()) return PercentileResult::Invalid();
        return depthMassCore.TryPercentileRank(val);
    }
};

} // namespace AMT

// ============================================================================
// TEST FUNCTIONS
// ============================================================================

int g_passed = 0;
int g_failed = 0;

#define TEST_ASSERT(cond, msg) \
    if (!(cond)) { \
        std::cout << "[FAIL] " << msg << " (line " << __LINE__ << ")\n"; \
        g_failed++; \
    } else { \
        std::cout << "[PASS] " << msg << "\n"; \
        g_passed++; \
    }

// ============================================================================
// TEST: RollingDist Try* APIs
// ============================================================================
void test_rolling_dist_try_apis() {
    std::cout << "\n=== TEST: RollingDist Try* APIs ===\n";

    AMT::RollingDist dist;
    dist.reset(100);

    // Empty distribution should return invalid
    {
        auto result = dist.TryPercentile(50.0);
        TEST_ASSERT(!result.valid, "TryPercentile returns invalid when empty");
    }
    {
        auto result = dist.TryMean();
        TEST_ASSERT(!result.valid, "TryMean returns invalid when empty");
    }
    {
        auto result = dist.TryMedian();
        TEST_ASSERT(!result.valid, "TryMedian returns invalid when empty");
    }

    // Push some values
    for (int i = 1; i <= 10; ++i) {
        dist.push(static_cast<double>(i));
    }
    // Values: 1,2,3,4,5,6,7,8,9,10

    // TryPercentile should return valid
    {
        auto result = dist.TryPercentile(5.5);  // 5 values below
        TEST_ASSERT(result.valid, "TryPercentile returns valid with data");
        TEST_ASSERT(result.value == 50.0, "TryPercentile(5.5) = 50%");
    }

    // TryMean should work
    {
        auto result = dist.TryMean();
        TEST_ASSERT(result.valid, "TryMean returns valid with data");
        TEST_ASSERT(std::abs(result.value - 5.5) < 0.01, "TryMean = 5.5");
    }

    // TryMedian should work
    {
        auto result = dist.TryMedian();
        TEST_ASSERT(result.valid, "TryMedian returns valid with data");
        TEST_ASSERT(std::abs(result.value - 5.5) < 0.01, "TryMedian = 5.5");
    }

    // TryPercentileRank (robust method using MAD)
    {
        auto result = dist.TryPercentileRank(5.5);
        TEST_ASSERT(result.valid, "TryPercentileRank returns valid with data");
        // Should be around 50% for median value
        TEST_ASSERT(result.value >= 40.0 && result.value <= 60.0,
            "TryPercentileRank(median) is near 50%");
    }

    // Readiness check
    {
        TEST_ASSERT(dist.GetReadiness(10) == AMT::BaselineReadiness::READY,
            "GetReadiness(10) = READY when size=10");
        TEST_ASSERT(dist.GetReadiness(20) == AMT::BaselineReadiness::WARMUP,
            "GetReadiness(20) = WARMUP when size=10");
        TEST_ASSERT(dist.IsReady(10), "IsReady(10) = true when size=10");
        TEST_ASSERT(!dist.IsReady(20), "IsReady(20) = false when size=10");
    }
}

// ============================================================================
// TEST: EffortBaselineStore
// ============================================================================
void test_effort_baseline_store() {
    std::cout << "\n=== TEST: EffortBaselineStore ===\n";

    AMT::EffortBaselineStore store;
    store.Reset();

    // Initially not ready
    TEST_ASSERT(!store.AllBucketsReady(), "Initially all buckets not ready");

    // Set expected bars per session (simulate 5-minute chart, RTH 9:30-16:00)
    const int rthStart = 9*3600 + 30*60;   // 09:30 = 34200
    const int rthEnd = 16*3600;             // 16:00 = 57600
    const int barInterval = 5*60;           // 5 minutes = 300 sec
    store.SetExpectedBarsPerSession(rthStart, rthEnd, barInterval);

    // Check expected bars for OPEN bucket (60 min = 12 bars)
    TEST_ASSERT(store.Get(AMT::EffortBucket::OPEN).expectedBarsPerSession == 12,
        "OPEN bucket expects 12 bars per session");

    // Simulate pushing bars from 5 sessions for OPEN bucket
    auto& openBucket = store.Get(AMT::EffortBucket::OPEN);
    for (int session = 0; session < 5; ++session) {
        for (int bar = 0; bar < 12; ++bar) {
            openBucket.vol_sec.push(100.0 + bar);
            openBucket.trades_sec.push(10.0 + bar);
            openBucket.delta_pct.push(0.01 * bar);
            openBucket.bar_range.push(5.0 + bar);
            openBucket.totalBarsPushed++;
        }
        openBucket.sessionsContributed++;
    }

    // OPEN bucket should now be ready
    TEST_ASSERT(openBucket.IsReady(), "OPEN bucket ready after 5 sessions");
    TEST_ASSERT(openBucket.GetState() == AMT::BucketBaselineState::READY,
        "OPEN bucket state is READY");

    // AllBucketsReady still false (MID and POWER empty)
    TEST_ASSERT(!store.AllBucketsReady(), "AllBucketsReady false (MID/POWER empty)");

    // Test TryPercentile on OPEN bucket's vol_sec
    {
        auto result = openBucket.vol_sec.TryPercentile(105.0);
        TEST_ASSERT(result.valid, "vol_sec TryPercentile returns valid");
        // 105 should be in the middle of the distribution
        TEST_ASSERT(result.value > 0.0 && result.value < 100.0,
            "vol_sec percentile is in valid range");
    }
}

// ============================================================================
// TEST: SessionDeltaBaseline
// ============================================================================
void test_session_delta_baseline() {
    std::cout << "\n=== TEST: SessionDeltaBaseline ===\n";

    AMT::SessionDeltaBaseline baseline;
    baseline.Reset();

    // Initially not ready
    TEST_ASSERT(!baseline.IsReady(), "Initially not ready");
    TEST_ASSERT(baseline.GetState() == AMT::SessionBaselineState::INSUFFICIENT_SESSIONS,
        "Initial state is INSUFFICIENT_SESSIONS");

    // TryGetPercentile should return invalid when not ready
    {
        auto result = baseline.TryGetPercentile(0.05);
        TEST_ASSERT(!result.valid, "TryGetPercentile invalid when not ready");
    }

    // Push session aggregates from 5 sessions
    // Simulate different session delta ratios (magnitude)
    baseline.PushSessionAggregate(0.02);   // 2% net delta
    baseline.IncrementSessionCount();
    baseline.PushSessionAggregate(-0.03);  // -3% (stored as abs = 3%)
    baseline.IncrementSessionCount();
    baseline.PushSessionAggregate(0.01);   // 1%
    baseline.IncrementSessionCount();
    baseline.PushSessionAggregate(-0.05);  // 5%
    baseline.IncrementSessionCount();
    baseline.PushSessionAggregate(0.04);   // 4%
    baseline.IncrementSessionCount();

    // Now should be ready
    TEST_ASSERT(baseline.IsReady(), "Ready after 5 sessions");
    TEST_ASSERT(baseline.GetState() == AMT::SessionBaselineState::READY,
        "State is READY");

    // TryGetPercentile should now return valid
    {
        auto result = baseline.TryGetPercentile(0.03);  // 3% delta
        TEST_ASSERT(result.valid, "TryGetPercentile valid when ready");
        // 3% should be in the middle (values stored: 0.02, 0.03, 0.01, 0.05, 0.04)
        // Sorted: 0.01, 0.02, 0.03, 0.04, 0.05
        // 0.03 has 2 below it out of 5 = 40%
        TEST_ASSERT(std::abs(result.value - 40.0) < 0.1,
            "TryGetPercentile(0.03) = 40%");
    }

    // Extreme value should give high percentile
    {
        auto result = baseline.TryGetPercentile(0.10);  // 10% (above all)
        TEST_ASSERT(result.valid, "TryGetPercentile valid for extreme");
        TEST_ASSERT(result.value == 100.0, "TryGetPercentile(0.10) = 100%");
    }
}

// ============================================================================
// TEST: DOMWarmup
// ============================================================================
void test_dom_warmup() {
    std::cout << "\n=== TEST: DOMWarmup ===\n";

    AMT::DOMWarmup warmup;
    warmup.Reset();

    // Initially not ready
    TEST_ASSERT(!warmup.IsReady(), "Initially not ready");
    TEST_ASSERT(warmup.state == AMT::DOMBaselineState::WARMUP_PENDING,
        "Initial state is WARMUP_PENDING");

    // TryDepthPercentile should return invalid
    {
        auto result = warmup.TryDepthPercentile(100.0);
        TEST_ASSERT(!result.valid, "TryDepthPercentile invalid during warmup");
    }

    // Start warmup at RTH open (simulate time = 34200 = 09:30)
    const int rthOpenTimeSec = 34200;
    warmup.StartWarmup(rthOpenTimeSec);

    // Push bars during warmup (simulate 15-min = 900 sec with 30-sec bars = 30 bars)
    for (int i = 0; i < 30; ++i) {
        const int barTime = rthOpenTimeSec + i * 30;
        warmup.PushIfWarmup(50.0 + i, 20.0 + i, 100.0 + i * 2, barTime);
    }

    // Should still be in warmup (only 14.5 minutes elapsed)
    TEST_ASSERT(!warmup.IsReady(), "Not ready after 14.5 minutes");
    TEST_ASSERT(!warmup.frozen, "Not frozen yet");

    // Check remaining time
    {
        int remaining = warmup.GetWarmupRemainingSeconds(rthOpenTimeSec + 870);
        TEST_ASSERT(remaining == 30, "30 seconds remaining at 14.5 min mark");
    }

    // Push one more bar at 15-min mark
    warmup.PushIfWarmup(80.0, 50.0, 160.0, rthOpenTimeSec + 900);

    // Now should be frozen and ready
    TEST_ASSERT(warmup.IsReady(), "Ready after 15 minutes");
    TEST_ASSERT(warmup.frozen, "Frozen after 15 minutes");
    TEST_ASSERT(warmup.state == AMT::DOMBaselineState::READY, "State is READY");

    // Try* APIs should now work
    {
        auto result = warmup.TryDepthPercentile(130.0);
        TEST_ASSERT(result.valid, "TryDepthPercentile valid after warmup");
    }
    {
        auto result = warmup.TryStackPercentile(65.0);
        TEST_ASSERT(result.valid, "TryStackPercentile valid after warmup");
    }
    {
        auto result = warmup.TryPullPercentile(35.0);
        TEST_ASSERT(result.valid, "TryPullPercentile valid after warmup");
    }

    // Remaining time should be 0
    TEST_ASSERT(warmup.GetWarmupRemainingSeconds(rthOpenTimeSec + 1000) == 0,
        "Remaining time is 0 after freeze");

    // Pushing more data should have no effect (frozen)
    const size_t sizeBeforePush = warmup.depthMassCore.size();
    warmup.PushIfWarmup(999.0, 999.0, 999.0, rthOpenTimeSec + 1200);
    TEST_ASSERT(warmup.depthMassCore.size() == sizeBeforePush,
        "No new data after freeze");
}

// ============================================================================
// TEST: No-Fallback Contract (Invalid Must Not Be Used)
// ============================================================================
void test_no_fallback_contract() {
    std::cout << "\n=== TEST: No-Fallback Contract ===\n";

    // Verify that invalid results have valid=false and using them is wrong
    {
        AMT::PercentileResult invalid = AMT::PercentileResult::Invalid();
        TEST_ASSERT(!invalid.valid, "Invalid result has valid=false");
        // The value field is 0.0 but MUST NOT be used
    }

    // Verify that valid results have valid=true
    {
        AMT::PercentileResult valid = AMT::PercentileResult::Valid(75.0);
        TEST_ASSERT(valid.valid, "Valid result has valid=true");
        TEST_ASSERT(valid.value == 75.0, "Valid result preserves value");
    }

    // Simulate consumer checking validity correctly
    {
        AMT::SessionDeltaBaseline empty;
        empty.Reset();

        auto result = empty.TryGetPercentile(0.05);

        // CORRECT CONSUMER PATTERN:
        if (!result.valid) {
            // Handle NO_EVIDENCE - exclude metric from scoring
            std::cout << "[INFO] Correct: Detected invalid and excluded from scoring\n";
            g_passed++;
        } else {
            // WRONG - should not reach here for empty baseline
            std::cout << "[FAIL] Incorrect: Used invalid result\n";
            g_failed++;
        }
    }
}

// ============================================================================
// TEST: Edge Cases
// ============================================================================
void test_edge_cases() {
    std::cout << "\n=== TEST: Edge Cases ===\n";

    // NaN/Inf values should be rejected
    {
        AMT::RollingDist dist;
        dist.reset(100);
        dist.push(std::nan(""));
        dist.push(std::numeric_limits<double>::infinity());
        dist.push(-std::numeric_limits<double>::infinity());
        dist.push(1.0);  // Only this should be added
        TEST_ASSERT(dist.size() == 1, "NaN/Inf values rejected, size=1");
    }

    // Degenerate case: all identical values (MAD = 0)
    {
        AMT::RollingDist dist;
        dist.reset(100);
        for (int i = 0; i < 10; ++i)
            dist.push(42.0);

        // TryPercentileRank should still work (fallback to 25/75 split)
        auto result = dist.TryPercentileRank(42.0);
        TEST_ASSERT(result.valid, "TryPercentileRank valid with identical values");
        TEST_ASSERT(result.value == 75.0, "Identical values: val>=med gives 75%");
    }

    // Window overflow (pushes beyond max size)
    {
        AMT::RollingDist dist;
        dist.reset(5);  // Small window
        for (int i = 0; i < 10; ++i)
            dist.push(static_cast<double>(i));
        TEST_ASSERT(dist.size() == 5, "Window size respected, size=5");
        // Should contain last 5 values: 5,6,7,8,9
        auto result = dist.TryMean();
        TEST_ASSERT(result.valid && std::abs(result.value - 7.0) < 0.01,
            "Mean of [5,6,7,8,9] = 7.0");
    }
}

// ============================================================================
// MAIN
// ============================================================================
int main() {
    std::cout << "=== NEW BASELINE COMPONENTS TEST SUITE ===\n";
    std::cout << "Testing: RollingDist, EffortBaselineStore, SessionDeltaBaseline, DOMWarmup\n";

    test_rolling_dist_try_apis();
    test_effort_baseline_store();
    test_session_delta_baseline();
    test_dom_warmup();
    test_no_fallback_contract();
    test_edge_cases();

    std::cout << "\n=== SUMMARY ===\n";
    std::cout << "Passed: " << g_passed << "\n";
    std::cout << "Failed: " << g_failed << "\n";

    if (g_failed == 0) {
        std::cout << "\n=== ALL TESTS PASSED ===\n";
        return 0;
    } else {
        std::cout << "\n=== SOME TESTS FAILED ===\n";
        return 1;
    }
}
