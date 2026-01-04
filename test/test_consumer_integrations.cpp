// ============================================================================
// Test: Consumer Integrations for Tier 1 + Tier 2 Baselines
// Tests: marketComposition, ExecutionFriction, 2D Volatility
// ============================================================================

#include <cassert>
#include <cstdio>
#include <cmath>
#include <deque>

// ============================================================================
// Minimal RollingDist for testing (mirrors AMT_Snapshots.h)
// ============================================================================
struct RollingDist {
    std::deque<double> values;
    int window = 300;

    void reset(int w) { window = w; values.clear(); }
    void push(double v) {
        values.push_back(v);
        while (static_cast<int>(values.size()) > window)
            values.pop_front();
    }
    size_t size() const { return values.size(); }
    double back() const { return values.empty() ? 0.0 : values.back(); }

    // Simple percentile calculation for testing
    struct PercentileResult {
        double value = 0.0;
        bool valid = false;
        static PercentileResult Invalid() { return {0.0, false}; }
    };

    PercentileResult TryPercentile(double val) const {
        if (values.size() < 5) return PercentileResult::Invalid();
        // Count how many values are <= val
        int count = 0;
        for (double v : values) {
            if (v <= val) count++;
        }
        double pctile = 100.0 * static_cast<double>(count) / static_cast<double>(values.size());
        return {pctile, true};
    }
};

// ============================================================================
// Enums (mirrors amt_core.h)
// ============================================================================
enum class VolatilityState : int {
    LOW = 1,
    NORMAL = 2,
    HIGH = 3,
    EXTREME = 4
};

enum class ExecutionFriction : int {
    UNKNOWN = 0,
    TIGHT = 1,
    NORMAL = 2,
    WIDE = 3,
    LOCKED = 4
};

// ============================================================================
// Minimal ConfidenceAttribute for testing (mirrors AMT_Patterns.h)
// ============================================================================
struct ConfidenceAttribute {
    float marketComposition = 0.0f;
    bool marketCompositionValid = false;
};

// ============================================================================
// ClassifyVolatility (mirrors AMT_ContextBuilder.h - 3-parameter version)
// ============================================================================
inline VolatilityState ClassifyVolatility(double rangePctile,
                                          double closeChangePctile,
                                          bool closeChangeValid) {
    // If close change baseline not ready, fall back to range-only
    if (!closeChangeValid) {
        if (rangePctile >= 90.0) return VolatilityState::EXTREME;
        if (rangePctile >= 75.0) return VolatilityState::HIGH;
        if (rangePctile <= 25.0) return VolatilityState::LOW;
        return VolatilityState::NORMAL;
    }

    // Two-dimensional classification
    const bool highRange = (rangePctile >= 75.0);
    const bool lowRange = (rangePctile <= 25.0);
    const bool highTravel = (closeChangePctile >= 75.0);
    const bool lowTravel = (closeChangePctile <= 25.0);

    if (rangePctile >= 90.0 && highTravel) return VolatilityState::EXTREME;
    if (highRange && highTravel) return VolatilityState::HIGH;
    if (lowRange && lowTravel) return VolatilityState::LOW;

    // Refinement cases
    if (highRange && lowTravel) {
        return VolatilityState::HIGH;  // INDECISIVE
    }
    if (lowRange && highTravel) {
        return VolatilityState::NORMAL;  // BREAKOUT_POTENTIAL
    }

    return VolatilityState::NORMAL;
}

// Backward-compatible overload (range-only)
inline VolatilityState ClassifyVolatility(double rangePctile) {
    return ClassifyVolatility(rangePctile, 50.0, false);
}

// ============================================================================
// ClassifyFriction (mirrors consumer logic in AuctionSensor_v1.cpp)
// ============================================================================
inline ExecutionFriction ClassifyFriction(double curSpreadTicks, double spreadPctile, bool baselineReady) {
    if (!baselineReady) return ExecutionFriction::UNKNOWN;

    if (curSpreadTicks == 0.0) {
        return ExecutionFriction::LOCKED;
    } else if (spreadPctile <= 25.0) {
        return ExecutionFriction::TIGHT;
    } else if (spreadPctile >= 75.0) {
        return ExecutionFriction::WIDE;
    } else {
        return ExecutionFriction::NORMAL;
    }
}

// ============================================================================
// Test 1: marketCompositionValid is false when numTrades == 0
// ============================================================================
void test_market_composition_invalid_when_zero_trades() {
    printf("=== Test 1: marketCompositionValid false when numTrades == 0 ===\n");

    ConfidenceAttribute conf;
    double numTrades = 0.0;
    double barVolume = 1000.0;

    // Simulate consumer logic
    if (numTrades > 0 && barVolume > 0) {
        conf.marketComposition = 0.5f;  // Would be set from percentile
        conf.marketCompositionValid = true;
    } else {
        conf.marketCompositionValid = false;
    }

    assert(!conf.marketCompositionValid);
    printf("  PASSED: marketCompositionValid = false when numTrades == 0\n");
}

// ============================================================================
// Test 2: marketCompositionValid is true when numTrades > 0
// ============================================================================
void test_market_composition_valid_when_trades_exist() {
    printf("=== Test 2: marketCompositionValid true when numTrades > 0 ===\n");

    RollingDist avg_trade_size;
    avg_trade_size.reset(100);

    // Push enough samples for baseline to be ready
    for (int i = 1; i <= 10; i++) {
        avg_trade_size.push(100.0 * i);  // 100, 200, 300, ... 1000
    }

    ConfidenceAttribute conf;
    double numTrades = 10.0;
    double barVolume = 1000.0;
    double curAvgTradeSize = barVolume / numTrades;  // 100.0

    // Simulate consumer logic
    if (numTrades > 0 && barVolume > 0) {
        auto atsResult = avg_trade_size.TryPercentile(curAvgTradeSize);
        if (atsResult.valid) {
            conf.marketComposition = static_cast<float>(atsResult.value / 100.0);
            conf.marketCompositionValid = true;
        }
    }

    assert(conf.marketCompositionValid);
    assert(conf.marketComposition >= 0.0f && conf.marketComposition <= 1.0f);
    printf("  PASSED: marketCompositionValid = true, composition = %.2f\n", conf.marketComposition);
}

// ============================================================================
// Test 3: marketComposition scaling equals pctile/100
// ============================================================================
void test_market_composition_scaling() {
    printf("=== Test 3: marketComposition scaling equals pctile/100 ===\n");

    RollingDist avg_trade_size;
    avg_trade_size.reset(100);

    // Push 10 samples: 10, 20, 30, ... 100
    for (int i = 1; i <= 10; i++) {
        avg_trade_size.push(10.0 * i);
    }

    // Query value of 50.0 - should be at 50th percentile (5 values <= 50)
    auto atsResult = avg_trade_size.TryPercentile(50.0);
    assert(atsResult.valid);

    float composition = static_cast<float>(atsResult.value / 100.0);

    // Expected: 50th percentile / 100 = 0.5
    assert(std::abs(composition - 0.5f) < 0.01f);
    printf("  PASSED: composition = %.2f (expected ~0.50)\n", composition);
}

// ============================================================================
// Test 4: ExecutionFriction LOCKED when spread == 0 and baseline ready
// ============================================================================
void test_friction_locked_when_spread_zero() {
    printf("=== Test 4: ExecutionFriction LOCKED when spread == 0 ===\n");

    ExecutionFriction friction = ClassifyFriction(0.0, 50.0, true);

    assert(friction == ExecutionFriction::LOCKED);
    printf("  PASSED: friction = LOCKED when spread = 0\n");
}

// ============================================================================
// Test 5: ExecutionFriction TIGHT when percentile <= 25
// ============================================================================
void test_friction_tight_when_low_percentile() {
    printf("=== Test 5: ExecutionFriction TIGHT when percentile <= 25 ===\n");

    ExecutionFriction friction = ClassifyFriction(1.0, 20.0, true);

    assert(friction == ExecutionFriction::TIGHT);
    printf("  PASSED: friction = TIGHT when pctile = 20\n");
}

// ============================================================================
// Test 6: ExecutionFriction WIDE when percentile >= 75
// ============================================================================
void test_friction_wide_when_high_percentile() {
    printf("=== Test 6: ExecutionFriction WIDE when percentile >= 75 ===\n");

    ExecutionFriction friction = ClassifyFriction(3.0, 80.0, true);

    assert(friction == ExecutionFriction::WIDE);
    printf("  PASSED: friction = WIDE when pctile = 80\n");
}

// ============================================================================
// Test 7: ExecutionFriction NORMAL when percentile in middle
// ============================================================================
void test_friction_normal_when_middle_percentile() {
    printf("=== Test 7: ExecutionFriction NORMAL when percentile in middle ===\n");

    ExecutionFriction friction = ClassifyFriction(2.0, 50.0, true);

    assert(friction == ExecutionFriction::NORMAL);
    printf("  PASSED: friction = NORMAL when pctile = 50\n");
}

// ============================================================================
// Test 8: ExecutionFriction UNKNOWN when baseline not ready
// ============================================================================
void test_friction_unknown_when_not_ready() {
    printf("=== Test 8: ExecutionFriction UNKNOWN when baseline not ready ===\n");

    ExecutionFriction friction = ClassifyFriction(2.0, 50.0, false);

    assert(friction == ExecutionFriction::UNKNOWN);
    printf("  PASSED: friction = UNKNOWN when baseline not ready\n");
}

// ============================================================================
// Test 9: Volatility range-only when close-change not ready
// ============================================================================
void test_volatility_range_only_when_close_change_invalid() {
    printf("=== Test 9: Volatility range-only when close-change not ready ===\n");

    // Test each range-only case
    assert(ClassifyVolatility(95.0, 0.0, false) == VolatilityState::EXTREME);
    assert(ClassifyVolatility(80.0, 0.0, false) == VolatilityState::HIGH);
    assert(ClassifyVolatility(50.0, 0.0, false) == VolatilityState::NORMAL);
    assert(ClassifyVolatility(20.0, 0.0, false) == VolatilityState::LOW);

    printf("  PASSED: Range-only logic works when closeChangeValid = false\n");
}

// ============================================================================
// Test 10: Volatility 2D - high range + low travel = HIGH (INDECISIVE)
// ============================================================================
void test_volatility_2d_high_range_low_travel() {
    printf("=== Test 10: 2D Volatility - high range + low travel ===\n");

    // High range (80th pctile) + low travel (20th pctile) = INDECISIVE -> HIGH
    VolatilityState vol = ClassifyVolatility(80.0, 20.0, true);

    assert(vol == VolatilityState::HIGH);
    printf("  PASSED: high range + low travel = HIGH (INDECISIVE character)\n");
}

// ============================================================================
// Test 11: Volatility 2D - low range + high travel = NORMAL (BREAKOUT_POTENTIAL)
// ============================================================================
void test_volatility_2d_low_range_high_travel() {
    printf("=== Test 11: 2D Volatility - low range + high travel ===\n");

    // Low range (20th pctile) + high travel (80th pctile) = BREAKOUT_POTENTIAL -> NORMAL
    VolatilityState vol = ClassifyVolatility(20.0, 80.0, true);

    assert(vol == VolatilityState::NORMAL);
    printf("  PASSED: low range + high travel = NORMAL (BREAKOUT_POTENTIAL character)\n");
}

// ============================================================================
// Test 12: Volatility 2D - high range + high travel = HIGH (TRENDING)
// ============================================================================
void test_volatility_2d_high_range_high_travel() {
    printf("=== Test 12: 2D Volatility - high range + high travel ===\n");

    VolatilityState vol = ClassifyVolatility(80.0, 80.0, true);

    assert(vol == VolatilityState::HIGH);
    printf("  PASSED: high range + high travel = HIGH (TRENDING character)\n");
}

// ============================================================================
// Test 13: Volatility 2D - low range + low travel = LOW (COMPRESSED)
// ============================================================================
void test_volatility_2d_low_range_low_travel() {
    printf("=== Test 13: 2D Volatility - low range + low travel ===\n");

    VolatilityState vol = ClassifyVolatility(20.0, 20.0, true);

    assert(vol == VolatilityState::LOW);
    printf("  PASSED: low range + low travel = LOW (COMPRESSED character)\n");
}

// ============================================================================
// Test 14: Volatility 2D - EXTREME requires high range AND high travel
// ============================================================================
void test_volatility_2d_extreme() {
    printf("=== Test 14: 2D Volatility - EXTREME requires both >=90 range + high travel ===\n");

    // Both high range (>=90) and high travel
    VolatilityState vol = ClassifyVolatility(95.0, 80.0, true);
    assert(vol == VolatilityState::EXTREME);

    // Very high range but low travel -> not EXTREME, just HIGH (INDECISIVE)
    vol = ClassifyVolatility(95.0, 20.0, true);
    assert(vol == VolatilityState::HIGH);

    printf("  PASSED: EXTREME requires >=90 range AND high travel\n");
}

// ============================================================================
// Test 15: Backward compatibility - 1-parameter overload
// ============================================================================
void test_volatility_backward_compatible() {
    printf("=== Test 15: Volatility backward-compatible 1-parameter overload ===\n");

    assert(ClassifyVolatility(95.0) == VolatilityState::EXTREME);
    assert(ClassifyVolatility(80.0) == VolatilityState::HIGH);
    assert(ClassifyVolatility(50.0) == VolatilityState::NORMAL);
    assert(ClassifyVolatility(20.0) == VolatilityState::LOW);

    printf("  PASSED: 1-parameter overload produces same results as range-only\n");
}

// ============================================================================
// Main
// ============================================================================
int main() {
    printf("\n=== Consumer Integration Tests ===\n\n");

    // marketComposition tests
    test_market_composition_invalid_when_zero_trades();
    test_market_composition_valid_when_trades_exist();
    test_market_composition_scaling();

    // ExecutionFriction tests
    test_friction_locked_when_spread_zero();
    test_friction_tight_when_low_percentile();
    test_friction_wide_when_high_percentile();
    test_friction_normal_when_middle_percentile();
    test_friction_unknown_when_not_ready();

    // Volatility tests
    test_volatility_range_only_when_close_change_invalid();
    test_volatility_2d_high_range_low_travel();
    test_volatility_2d_low_range_high_travel();
    test_volatility_2d_high_range_high_travel();
    test_volatility_2d_low_range_low_travel();
    test_volatility_2d_extreme();
    test_volatility_backward_compatible();

    printf("\n=== ALL TESTS PASSED ===\n\n");
    return 0;
}
