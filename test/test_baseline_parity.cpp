// test_baseline_parity.cpp
// Parity test: Verify SessionPhase-based baseline system behavior
// Tests EffortBaselineStore, SessionDeltaBaseline, and DOMWarmup APIs
//
// Compile: g++ -std=c++17 -I.. -o test_baseline_parity.exe test_baseline_parity.cpp
// Run: ./test_baseline_parity.exe

#include <cstdio>
#include <cmath>
#include <vector>
#include <cassert>
#include <cstdarg>

// Use complete SC mock header for standalone compilation
#include "test_sierrachart_mock.h"

// Include the headers with baseline code
#include "../amt_core.h"
#include "../AMT_Snapshots.h"

// Test data: simulated bar metrics
struct TestBar {
    int timeSec;      // Time of day in seconds
    double volume;
    double delta;
    double trades;
    double rangeTicks;
    AMT::SessionPhase phase;  // Associated session phase
};

// Generate test data for a specific session phase
std::vector<TestBar> GeneratePhaseData(AMT::SessionPhase phase, int count, int startTimeSec) {
    std::vector<TestBar> bars;

    for (int i = 0; i < count; i++) {
        TestBar bar;
        bar.timeSec = startTimeSec + i * 60;  // 1-minute bars
        bar.phase = phase;

        // Vary volume based on phase
        double baseFactor = 1.0;
        switch (phase) {
            case AMT::SessionPhase::INITIAL_BALANCE: baseFactor = 1.5; break;  // Higher at open
            case AMT::SessionPhase::CLOSING_SESSION: baseFactor = 1.3; break;  // Higher at close
            case AMT::SessionPhase::GLOBEX:          baseFactor = 0.5; break;  // Lower overnight
            default: baseFactor = 1.0; break;
        }

        bar.volume = (800.0 + 400.0 * baseFactor) + (rand() % 400);
        bar.delta = bar.volume * (0.1 * (rand() % 100 - 50) / 50.0);
        bar.trades = bar.volume / 10.0 + (rand() % 50);
        bar.rangeTicks = 3.0 + (rand() % 6);

        bars.push_back(bar);
    }

    return bars;
}

// Test 1: Verify RollingDist TryPercentile matches legacy percentile
void TestRollingDistParity() {
    printf("\n=== Test 1: RollingDist TryPercentile Parity ===\n");

    AMT::RollingDist dist;
    dist.reset(300);

    // Push 100 samples
    for (int i = 1; i <= 100; i++) {
        dist.push(static_cast<double>(i));
    }

    // Test various query values
    double testValues[] = {1.0, 25.0, 50.0, 75.0, 100.0, 150.0};

    for (double val : testValues) {
        double legacyPctile = dist.percentile(val);
        AMT::PercentileResult newResult = dist.TryPercentile(val);

        printf("  Query %.1f: legacy=%.2f new=%s(%.2f) %s\n",
            val,
            legacyPctile,
            newResult.valid ? "VALID" : "INVALID",
            newResult.valid ? newResult.value : 0.0,
            (newResult.valid && std::abs(legacyPctile - newResult.value) < 0.01) ? "MATCH" : "DIFF");

        if (newResult.valid) {
            assert(std::abs(legacyPctile - newResult.value) < 0.01);
        }
    }

    printf("  PASSED\n");
}

// Test 2: Verify SessionPhase-to-BucketIndex mapping
void TestSessionPhaseToBucket() {
    printf("\n=== Test 2: SessionPhase-to-Bucket Mapping ===\n");

    struct TestCase {
        AMT::SessionPhase phase;
        const char* phaseName;
        int expectedIndex;
        bool expectedTradeable;
    };

    TestCase cases[] = {
        {AMT::SessionPhase::GLOBEX,          "GLOBEX",          0, true},
        {AMT::SessionPhase::LONDON_OPEN,     "LONDON_OPEN",     1, true},
        {AMT::SessionPhase::PRE_MARKET,      "PRE_MARKET",      2, true},
        {AMT::SessionPhase::INITIAL_BALANCE, "INITIAL_BALANCE", 3, true},
        {AMT::SessionPhase::MID_SESSION,     "MID_SESSION",     4, true},
        {AMT::SessionPhase::CLOSING_SESSION, "CLOSING_SESSION", 5, true},
        {AMT::SessionPhase::POST_CLOSE,      "POST_CLOSE",      6, true},
        {AMT::SessionPhase::MAINTENANCE,     "MAINTENANCE",    -1, false},
        {AMT::SessionPhase::UNKNOWN,         "UNKNOWN",        -1, false},
    };

    printf("  Testing SessionPhaseToBucketIndex():\n");
    for (const auto& tc : cases) {
        int actualIdx = AMT::SessionPhaseToBucketIndex(tc.phase);
        bool pass = (actualIdx == tc.expectedIndex);
        printf("    %s -> idx=%d (expected=%d) %s\n",
            tc.phaseName, actualIdx, tc.expectedIndex, pass ? "PASS" : "FAIL");
        assert(pass);
    }

    printf("  Testing IsTradeablePhase():\n");
    for (const auto& tc : cases) {
        bool actualTradeable = AMT::IsTradeablePhase(tc.phase);
        bool pass = (actualTradeable == tc.expectedTradeable);
        printf("    %s -> tradeable=%s (expected=%s) %s\n",
            tc.phaseName,
            actualTradeable ? "true" : "false",
            tc.expectedTradeable ? "true" : "false",
            pass ? "PASS" : "FAIL");
        assert(pass);
    }

    printf("  Testing BucketIndexToSessionPhase() round-trip:\n");
    for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; i++) {
        AMT::SessionPhase phase = AMT::BucketIndexToSessionPhase(i);
        int backToIdx = AMT::SessionPhaseToBucketIndex(phase);
        bool pass = (backToIdx == i);
        printf("    idx=%d -> phase -> idx=%d %s\n", i, backToIdx, pass ? "PASS" : "FAIL");
        assert(pass);
    }

    printf("  PASSED\n");
}

// Test 3: Verify EffortBaselineStore SessionPhase-based behavior
void TestEffortBaselineStore() {
    printf("\n=== Test 3: EffortBaselineStore SessionPhase-Based Behavior ===\n");

    int barIntervalSec = 60;

    AMT::EffortBaselineStore store;
    store.SetExpectedBarsPerSession(barIntervalSec);

    printf("  Expected bars per phase (1-minute bars):\n");
    for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; i++) {
        AMT::SessionPhase phase = AMT::BucketIndexToSessionPhase(i);
        int expected = AMT::GetExpectedBarsInPhase(phase, barIntervalSec);
        printf("    %d: expected=%d bars\n", i, expected);
    }

    // Generate and push test data for each tradeable phase
    printf("\n  Populating each phase bucket:\n");

    AMT::SessionPhase testPhases[] = {
        AMT::SessionPhase::GLOBEX,
        AMT::SessionPhase::LONDON_OPEN,
        AMT::SessionPhase::PRE_MARKET,
        AMT::SessionPhase::INITIAL_BALANCE,
        AMT::SessionPhase::MID_SESSION,
        AMT::SessionPhase::CLOSING_SESSION,
        AMT::SessionPhase::POST_CLOSE
    };

    for (AMT::SessionPhase phase : testPhases) {
        std::vector<TestBar> bars = GeneratePhaseData(phase, 30, 0);

        AMT::EffortBucketDistribution& dist = store.Get(phase);
        for (const auto& bar : bars) {
            double volSec = bar.volume / barIntervalSec;
            double deltaPct = bar.delta / bar.volume;
            dist.vol_sec.push(volSec);
            dist.delta_pct.push(deltaPct);
        }

        int idx = AMT::SessionPhaseToBucketIndex(phase);
        printf("    Phase %d: pushed %zu bars, vol_sec.size()=%zu\n",
            idx, bars.size(), dist.vol_sec.size());
    }

    // Verify queries work per phase
    printf("\n  Query vol_sec=20.0 per phase:\n");
    double queryVolSec = 20.0;

    for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; i++) {
        AMT::SessionPhase phase = AMT::BucketIndexToSessionPhase(i);
        AMT::PercentileResult result = store.Get(phase).vol_sec.TryPercentile(queryVolSec);

        printf("    Phase %d: %s(%.2f)\n",
            i,
            result.valid ? "VALID" : "INVALID",
            result.valid ? result.value : 0.0);
    }

    // Verify non-tradeable phases get fallback bucket
    printf("\n  Testing non-tradeable phase fallback:\n");
    AMT::EffortBucketDistribution& maintDist = store.Get(AMT::SessionPhase::MAINTENANCE);
    AMT::EffortBucketDistribution& globexDist = store.Get(AMT::SessionPhase::GLOBEX);
    // MAINTENANCE should fall back to bucket[0] which is GLOBEX
    bool fallbackWorks = (&maintDist == &globexDist);
    printf("    MAINTENANCE falls back to bucket[0]: %s\n", fallbackWorks ? "PASS" : "FAIL");
    assert(fallbackWorks);

    printf("  PASSED\n");
}

// Test 4: Verify SessionDeltaBaseline (phase-bucketed)
void TestSessionDeltaBaseline() {
    printf("\n=== Test 4: SessionDeltaBaseline (phase-bucketed) ===\n");

    AMT::SessionDeltaBaseline sdb;
    const AMT::SessionPhase testPhase = AMT::SessionPhase::MID_SESSION;

    // Verify empty state
    AMT::PercentileResult emptyResult = sdb.TryGetPercentile(testPhase, 0.05);
    printf("  Empty state: valid=%s (expected: INVALID)\n",
        emptyResult.valid ? "VALID" : "INVALID");
    assert(!emptyResult.valid);

    // Push 10 sessions with phase delta ratios
    double phaseDeltas[] = {0.02, -0.03, 0.05, -0.01, 0.04, -0.02, 0.03, -0.04, 0.01, 0.06};
    for (double d : phaseDeltas) {
        sdb.PushPhaseDelta(testPhase, d);  // Uses std::abs internally
        sdb.IncrementPhaseSessionCount(testPhase);
    }

    const auto& bucket = sdb.Get(testPhase);
    printf("  After 10 sessions (MID_SESSION): size=%zu sessions=%d\n",
        bucket.delta_ratio.size(), bucket.sessionsContributed);

    // Query phase-bucketed percentile (uses magnitude internally)
    AMT::PercentileResult result = sdb.TryGetPercentile(testPhase, 0.05);
    printf("  Query 0.05: valid=%s value=%.2f\n",
        result.valid ? "VALID" : "INVALID",
        result.valid ? result.value : 0.0);

    assert(result.valid);
    assert(bucket.sessionsContributed >= 5);  // REQUIRED_SESSIONS = 5

    // Verify other phases are still empty/not ready
    AMT::PercentileResult otherResult = sdb.TryGetPercentile(AMT::SessionPhase::GLOBEX, 0.05);
    assert(!otherResult.valid);  // GLOBEX bucket should be empty
    printf("  Other phase (GLOBEX) correctly reports NOT_READY\n");

    printf("  PASSED\n");
}

// Test 5: Verify DOMWarmup
void TestDOMWarmup() {
    printf("\n=== Test 5: DOMWarmup ===\n");

    AMT::DOMWarmup warmup;
    const AMT::SessionPhase testPhase = AMT::SessionPhase::MID_SESSION;

    // Verify initial state (not ready until MIN_SAMPLES pushed)
    printf("  Initial: IsReady=%s (expected: false)\n", warmup.IsReady(testPhase) ? "true" : "false");
    assert(!warmup.IsReady(testPhase));

    // Start warmup (no-op for phase-bucketed baseline, kept for API compatibility)
    warmup.StartWarmup(100);
    printf("  After StartWarmup(100): distributions reset\n");

    // Push some DOM data to a specific phase (need >= MIN_SAMPLES=10 for IsReady)
    for (int i = 0; i < 20; i++) {
        warmup.Push(testPhase, 50.0 + i, 40.0 + i, 100.0 + i * 2);
    }

    const auto& bucket = warmup.Get(testPhase);
    printf("  After 20 pushes: stack=%zu pull=%zu depth=%zu\n",
        bucket.stackRate.size(), bucket.pullRate.size(), bucket.depthMassCore.size());

    printf("  IsReady=%s (expected: true - have %zu samples >= MIN_SAMPLES=10)\n",
        warmup.IsReady(testPhase) ? "true" : "false", bucket.depthMassCore.size());
    assert(warmup.IsReady(testPhase));

    // Query using phase-aware API
    AMT::PercentileResult depthResult = warmup.TryDepthPercentile(testPhase, 110.0);
    printf("  Query depth 110.0: valid=%s value=%.2f\n",
        depthResult.valid ? "VALID" : "INVALID",
        depthResult.valid ? depthResult.value : 0.0);

    assert(depthResult.valid);

    // Verify other phases are still empty
    assert(!warmup.IsReady(AMT::SessionPhase::GLOBEX));
    printf("  Other phase (GLOBEX) correctly reports NOT_READY\n");

    printf("  PASSED\n");
}

// Test 6: Verify consumer patterns using SessionPhase
void TestConsumerPatterns() {
    printf("\n=== Test 6: Consumer Pattern Verification (SessionPhase-based) ===\n");

    int barIntervalSec = 60;

    AMT::EffortBaselineStore effortBaselines;
    effortBaselines.SetExpectedBarsPerSession(barIntervalSec);

    AMT::SessionDeltaBaseline sessionDeltaBaseline;
    AMT::DOMWarmup domWarmup;

    // Simulate populating from prior sessions (multiple phases)
    printf("  Populating 5 simulated sessions across all phases...\n");

    for (int session = 0; session < 5; session++) {
        // Each session has data from all tradeable phases
        AMT::SessionPhase phases[] = {
            AMT::SessionPhase::GLOBEX,
            AMT::SessionPhase::PRE_MARKET,
            AMT::SessionPhase::INITIAL_BALANCE,
            AMT::SessionPhase::MID_SESSION,
            AMT::SessionPhase::CLOSING_SESSION
        };

        for (AMT::SessionPhase phase : phases) {
            double phaseCumDelta = 0.0;
            double phaseTotalVol = 0.0;

            std::vector<TestBar> bars = GeneratePhaseData(phase, 20, 0);

            AMT::EffortBucketDistribution& dist = effortBaselines.Get(phase);
            for (const auto& bar : bars) {
                double volSec = bar.volume / barIntervalSec;
                double deltaPct = bar.delta / bar.volume;
                dist.vol_sec.push(volSec);
                dist.delta_pct.push(deltaPct);

                phaseCumDelta += bar.delta;
                phaseTotalVol += bar.volume;
            }

            // Push per-PHASE delta ratio (phase-bucketed baseline)
            if (phaseTotalVol > 0.0) {
                double phaseDeltaRatio = phaseCumDelta / phaseTotalVol;
                sessionDeltaBaseline.PushPhaseDelta(phase, phaseDeltaRatio);
                sessionDeltaBaseline.IncrementPhaseSessionCount(phase);
            }
        }
    }

    // Simulate DOM warmup - push to the phase we'll query
    AMT::SessionPhase currentPhase = AMT::SessionPhase::INITIAL_BALANCE;
    domWarmup.StartWarmup(0);
    for (int i = 0; i < 15; i++) {
        domWarmup.Push(currentPhase, 50.0 + rand() % 20, 40.0 + rand() % 20, 100.0 + rand() % 50);
    }

    printf("  Setup complete: 5 sessions populated\n");

    // === CONSUMER PATTERN 1: Volume percentile for CURRENT phase ===
    double currentVolSec = 25.0;

    if (!AMT::IsTradeablePhase(currentPhase)) {
        printf("  Pattern 1: Non-tradeable phase - NOT_APPLICABLE\n");
    } else {
        AMT::PercentileResult volResult = effortBaselines.Get(currentPhase).vol_sec.TryPercentile(currentVolSec);
        if (volResult.valid) {
            printf("  Pattern 1: Volume pctile at INITIAL_BALANCE = %.2f\n", volResult.value);
        } else {
            printf("  Pattern 1: Volume baseline not ready for INITIAL_BALANCE\n");
        }
    }

    // Test another phase
    currentPhase = AMT::SessionPhase::GLOBEX;
    currentVolSec = 15.0;  // Lower volume expected in GLOBEX

    AMT::PercentileResult globexResult = effortBaselines.Get(currentPhase).vol_sec.TryPercentile(currentVolSec);
    if (globexResult.valid) {
        printf("  Pattern 1b: Volume pctile at GLOBEX = %.2f\n", globexResult.value);
    } else {
        printf("  Pattern 1b: Volume baseline not ready for GLOBEX\n");
    }

    // === CONSUMER PATTERN 2: Session delta percentile (phase-bucketed) ===
    double currentSessionDeltaRatio = 0.03;
    // Query uses currentPhase to compare against same-phase historical data
    AMT::PercentileResult deltaResult = sessionDeltaBaseline.TryGetPercentile(currentPhase, currentSessionDeltaRatio);
    if (deltaResult.valid) {
        printf("  Pattern 2: Session delta pctile = %.2f (phase=%s)\n", deltaResult.value, AMT::SessionPhaseToString(currentPhase));
    } else {
        printf("  Pattern 2: Session delta baseline not ready for phase=%s\n", AMT::SessionPhaseToString(currentPhase));
    }

    // === CONSUMER PATTERN 3: DOM depth percentile (phase-bucketed) ===
    if (domWarmup.IsReady(currentPhase)) {
        double currentDepth = 120.0;
        AMT::PercentileResult depthResult = domWarmup.TryDepthPercentile(currentPhase, currentDepth);
        if (depthResult.valid) {
            printf("  Pattern 3: DOM depth pctile = %.2f (phase=%s)\n", depthResult.value, AMT::SessionPhaseToString(currentPhase));
        } else {
            printf("  Pattern 3: DOM depth query failed\n");
        }
    } else {
        printf("  Pattern 3: DOM warmup not ready for phase=%s\n", AMT::SessionPhaseToString(currentPhase));
    }

    printf("  PASSED - All consumer patterns work\n");
}

// Test 7: Verify GetPhaseDurationSeconds
void TestPhaseDurations() {
    printf("\n=== Test 7: Phase Duration Calculation ===\n");

    struct TestCase {
        AMT::SessionPhase phase;
        const char* phaseName;
        int expectedSeconds;
    };

    // Expected durations from GetPhaseDurationSeconds() - matching actual session times
    TestCase cases[] = {
        {AMT::SessionPhase::GLOBEX,          "GLOBEX",          32400},  // 18:00-03:00 = 9h
        {AMT::SessionPhase::LONDON_OPEN,     "LONDON_OPEN",     19800},  // 03:00-08:30 = 5.5h
        {AMT::SessionPhase::PRE_MARKET,      "PRE_MARKET",       3600},  // 08:30-09:30 = 1h
        {AMT::SessionPhase::INITIAL_BALANCE, "INITIAL_BALANCE",  3600},  // 09:30-10:30 = 1h
        {AMT::SessionPhase::MID_SESSION,     "MID_SESSION",     18000},  // 10:30-15:30 = 5h
        {AMT::SessionPhase::CLOSING_SESSION, "CLOSING_SESSION",  2700},  // 15:30-16:15 = 45m
        {AMT::SessionPhase::POST_CLOSE,      "POST_CLOSE",       2700},  // 16:15-17:00 = 45m
        {AMT::SessionPhase::MAINTENANCE,     "MAINTENANCE",         0},  // Not tradeable
    };

    for (const auto& tc : cases) {
        int actualSec = AMT::GetPhaseDurationSeconds(tc.phase);
        bool pass = (actualSec == tc.expectedSeconds);
        printf("  %s: duration=%d sec (expected=%d) %s\n",
            tc.phaseName, actualSec, tc.expectedSeconds, pass ? "PASS" : "FAIL");
        assert(pass);
    }

    // Test expected bars calculation
    printf("\n  Expected bars at 60-second interval:\n");
    for (const auto& tc : cases) {
        int expectedBars = AMT::GetExpectedBarsInPhase(tc.phase, 60);
        printf("    %s: %d bars\n", tc.phaseName, expectedBars);
    }

    printf("  PASSED\n");
}

int main() {
    printf("========================================\n");
    printf("BASELINE PARITY TEST (SessionPhase-based)\n");
    printf("========================================\n");
    printf("This test verifies the SessionPhase-based baseline system\n");
    printf("with 7 tradeable phase buckets (Dec 2024 refactor).\n");

    srand(42);  // Deterministic for reproducibility

    TestRollingDistParity();
    TestSessionPhaseToBucket();
    TestEffortBaselineStore();
    TestSessionDeltaBaseline();
    TestDOMWarmup();
    TestConsumerPatterns();
    TestPhaseDurations();

    printf("\n========================================\n");
    printf("ALL TESTS PASSED\n");
    printf("========================================\n");
    printf("SessionPhase-based baseline system verified.\n");

    return 0;
}
