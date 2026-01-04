// test_baseline_readiness.cpp - Verify baseline readiness contract (Stage 1)
// Tests the IsReady() and GetReadiness() methods on RollingDist
#include <iostream>
#include <cassert>

// Include required headers
#include "test_sierrachart_mock.h"
#include "../AMT_Snapshots.h"
#include "../AMT_config.h"

using namespace AMT;

// ============================================================================
// TEST: BaselineReadiness enum values
// ============================================================================

void test_readiness_enum_values()
{
    std::cout << "=== Test: BaselineReadiness enum values ===" << std::endl;

    assert(static_cast<int>(BaselineReadiness::READY) == 0);
    assert(static_cast<int>(BaselineReadiness::WARMUP) == 1);
    assert(static_cast<int>(BaselineReadiness::STALE) == 2);
    assert(static_cast<int>(BaselineReadiness::UNAVAILABLE) == 3);

    // Test string conversion
    assert(std::string(BaselineReadinessToString(BaselineReadiness::READY)) == "READY");
    assert(std::string(BaselineReadinessToString(BaselineReadiness::WARMUP)) == "WARMUP");
    assert(std::string(BaselineReadinessToString(BaselineReadiness::STALE)) == "STALE");
    assert(std::string(BaselineReadinessToString(BaselineReadiness::UNAVAILABLE)) == "UNAVAILABLE");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: RollingDist.GetReadiness() states
// ============================================================================

void test_rolling_dist_readiness_unavailable()
{
    std::cout << "=== Test: Empty RollingDist returns UNAVAILABLE ===" << std::endl;

    RollingDist dist;
    dist.reset(300);

    assert(dist.size() == 0);
    assert(dist.GetReadiness(10) == BaselineReadiness::UNAVAILABLE);
    assert(dist.GetReadiness(1) == BaselineReadiness::UNAVAILABLE);
    assert(!dist.IsReady(10));
    assert(!dist.IsReady(1));

    std::cout << "  PASSED" << std::endl;
}

void test_rolling_dist_readiness_warmup()
{
    std::cout << "=== Test: Partial RollingDist returns WARMUP ===" << std::endl;

    RollingDist dist;
    dist.reset(300);

    // Add 5 samples (below threshold of 10)
    for (int i = 0; i < 5; i++) {
        dist.push(100.0 + i);
    }

    assert(dist.size() == 5);
    assert(dist.GetReadiness(10) == BaselineReadiness::WARMUP);
    assert(!dist.IsReady(10));

    // But READY for threshold of 5
    assert(dist.GetReadiness(5) == BaselineReadiness::READY);
    assert(dist.IsReady(5));

    std::cout << "  PASSED" << std::endl;
}

void test_rolling_dist_readiness_ready()
{
    std::cout << "=== Test: Full RollingDist returns READY ===" << std::endl;

    RollingDist dist;
    dist.reset(300);

    // Add exactly 10 samples
    for (int i = 0; i < 10; i++) {
        dist.push(100.0 + i);
    }

    assert(dist.size() == 10);
    assert(dist.GetReadiness(10) == BaselineReadiness::READY);
    assert(dist.IsReady(10));

    // Add more samples
    for (int i = 0; i < 10; i++) {
        dist.push(110.0 + i);
    }

    assert(dist.size() == 20);
    assert(dist.GetReadiness(10) == BaselineReadiness::READY);
    assert(dist.GetReadiness(20) == BaselineReadiness::READY);
    assert(dist.IsReady(20));

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Boundary conditions
// ============================================================================

void test_readiness_boundary_conditions()
{
    std::cout << "=== Test: Readiness boundary conditions ===" << std::endl;

    RollingDist dist;
    dist.reset(300);

    // Boundary: exactly at threshold
    for (int i = 0; i < 9; i++) {
        dist.push(100.0 + i);
    }
    assert(dist.size() == 9);
    assert(dist.GetReadiness(10) == BaselineReadiness::WARMUP);
    std::cout << "  samples=9, threshold=10: WARMUP - OK" << std::endl;

    // Add one more to hit threshold
    dist.push(109.0);
    assert(dist.size() == 10);
    assert(dist.GetReadiness(10) == BaselineReadiness::READY);
    std::cout << "  samples=10, threshold=10: READY - OK" << std::endl;

    // Add one more past threshold
    dist.push(110.0);
    assert(dist.size() == 11);
    assert(dist.GetReadiness(10) == BaselineReadiness::READY);
    std::cout << "  samples=11, threshold=10: READY - OK" << std::endl;

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: BaselineMinSamples constants
// ============================================================================

void test_baseline_min_samples_constants()
{
    std::cout << "=== Test: BaselineMinSamples constants ===" << std::endl;

    // Model types
    assert(BaselineMinSamples::ROBUST_CONTINUOUS == 20);
    assert(BaselineMinSamples::BOUNDED_RATIO == 10);
    assert(BaselineMinSamples::POSITIVE_SKEW == 10);
    assert(BaselineMinSamples::COUNT_MODEL == 10);

    std::cout << "  ROBUST_CONTINUOUS = " << BaselineMinSamples::ROBUST_CONTINUOUS << std::endl;
    std::cout << "  BOUNDED_RATIO = " << BaselineMinSamples::BOUNDED_RATIO << std::endl;
    std::cout << "  POSITIVE_SKEW = " << BaselineMinSamples::POSITIVE_SKEW << std::endl;
    std::cout << "  COUNT_MODEL = " << BaselineMinSamples::COUNT_MODEL << std::endl;

    // Metric-specific (should match their model type)
    assert(BaselineMinSamples::VOL_SEC == BaselineMinSamples::ROBUST_CONTINUOUS);
    assert(BaselineMinSamples::DELTA_PCT == BaselineMinSamples::BOUNDED_RATIO);
    assert(BaselineMinSamples::DEPTH_MASS_CORE == BaselineMinSamples::POSITIVE_SKEW);
    assert(BaselineMinSamples::TRADES_SEC == BaselineMinSamples::COUNT_MODEL);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Integration with BaselineEngine
// ============================================================================

void test_baseline_engine_readiness()
{
    std::cout << "=== Test: BaselineEngine readiness integration ===" << std::endl;

    BaselineEngine be;
    be.reset(300);

    // Initially all baselines should be UNAVAILABLE
    assert(be.vol_sec.GetReadiness(BaselineMinSamples::VOL_SEC) == BaselineReadiness::UNAVAILABLE);
    assert(be.delta_pct.GetReadiness(BaselineMinSamples::DELTA_PCT) == BaselineReadiness::UNAVAILABLE);
    assert(be.depth_mass_core.GetReadiness(BaselineMinSamples::DEPTH_MASS_CORE) == BaselineReadiness::UNAVAILABLE);

    // Populate vol_sec with 15 samples (below ROBUST_CONTINUOUS threshold of 20)
    for (int i = 0; i < 15; i++) {
        be.vol_sec.push(50.0 + i);
    }
    assert(be.vol_sec.GetReadiness(BaselineMinSamples::VOL_SEC) == BaselineReadiness::WARMUP);

    // Populate delta_pct with 10 samples (exactly at BOUNDED_RATIO threshold)
    for (int i = 0; i < 10; i++) {
        be.delta_pct.push(0.1 * i);
    }
    assert(be.delta_pct.GetReadiness(BaselineMinSamples::DELTA_PCT) == BaselineReadiness::READY);

    // Complete vol_sec
    for (int i = 0; i < 5; i++) {
        be.vol_sec.push(65.0 + i);
    }
    assert(be.vol_sec.GetReadiness(BaselineMinSamples::VOL_SEC) == BaselineReadiness::READY);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Existing RollingDist behavior unchanged when READY
// ============================================================================

void test_existing_behavior_preserved()
{
    std::cout << "=== Test: Existing RollingDist behavior preserved when READY ===" << std::endl;

    RollingDist dist;
    dist.reset(300);

    // Populate with known values
    for (int i = 0; i < 20; i++) {
        dist.push(100.0 + i * 10);  // 100, 110, 120, ..., 290
    }

    assert(dist.IsReady(20));

    // Test mean() - should be (100 + 290) / 2 = 195
    double m = dist.mean();
    std::cout << "  mean() = " << m << " (expected ~195)" << std::endl;
    assert(std::abs(m - 195.0) < 0.01);

    // Test median()
    double med = dist.median();
    std::cout << "  median() = " << med << " (expected ~195)" << std::endl;
    assert(std::abs(med - 195.0) < 0.01);

    // Test percentile() - value at median should be ~50%
    double pct = dist.percentile(195.0);
    std::cout << "  percentile(195.0) = " << pct << " (expected ~50)" << std::endl;
    assert(pct >= 45.0 && pct <= 55.0);

    // Test percentileRank() - should work when READY
    double pctRank = dist.percentileRank(195.0);
    std::cout << "  percentileRank(195.0) = " << pctRank << " (expected ~50)" << std::endl;
    assert(pctRank >= 45.0 && pctRank <= 55.0);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// MAIN
// ============================================================================

int main()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "Baseline Readiness Tests (Stage 1)" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // Enum tests
    test_readiness_enum_values();

    // RollingDist readiness tests
    test_rolling_dist_readiness_unavailable();
    test_rolling_dist_readiness_warmup();
    test_rolling_dist_readiness_ready();
    test_readiness_boundary_conditions();

    // Constants tests
    test_baseline_min_samples_constants();

    // Integration tests
    test_baseline_engine_readiness();
    test_existing_behavior_preserved();

    std::cout << "\n========================================" << std::endl;
    std::cout << "All Stage 1 tests PASSED!" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
}
