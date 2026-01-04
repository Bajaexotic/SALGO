// ============================================================================
// Test: Tier 1 + Tier 2 Baseline Expansion
// Tests: avg_trade_size, abs_close_change, spreadTicks
// ============================================================================

#include <cassert>
#include <cstdio>
#include <cmath>
#include <deque>

// Minimal RollingDist for testing (mirrors AMT_Snapshots.h)
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
};

// Minimal DOMWarmup for testing (mirrors AMT_Snapshots.h)
struct DOMWarmup {
    static constexpr size_t MIN_SAMPLES = 5;

    RollingDist depthMassCore;
    RollingDist spreadTicks;

    void Reset(int window = 300) {
        depthMassCore.reset(window);
        spreadTicks.reset(window);
    }

    // Push spread data (execution friction proxy)
    // Spread >= 0 is valid (0 = locked market, positive = normal spread)
    void PushSpread(double spread) {
        if (spread >= 0.0) {
            spreadTicks.push(spread);
        }
        // Negative spread (crossed market) is rejected
    }

    bool IsReady() const {
        return depthMassCore.size() >= MIN_SAMPLES;
    }

    bool IsSpreadReady() const {
        return spreadTicks.size() >= MIN_SAMPLES;
    }
};

// Minimal EffortBucketDistribution for testing
struct EffortBucketDistribution {
    RollingDist vol_sec;
    RollingDist avg_trade_size;
    RollingDist abs_close_change;

    int sessionsContributed = 0;
    int totalBarsPushed = 0;

    static constexpr int REQUIRED_SESSIONS = 5;

    void Reset(int window = 1500) {
        vol_sec.reset(window);
        avg_trade_size.reset(window);
        abs_close_change.reset(window);
        sessionsContributed = 0;
        totalBarsPushed = 0;
    }

    bool IsReady() const {
        return sessionsContributed >= REQUIRED_SESSIONS;
    }
};

// ============================================================================
// Test 1: avg_trade_size - NOT pushed when numTrades == 0
// ============================================================================
void test_avg_trade_size_not_pushed_when_zero_trades() {
    printf("=== Test 1: avg_trade_size not pushed when numTrades == 0 ===\n");

    EffortBucketDistribution dist;
    dist.Reset(100);

    // Simulate bar with volume but zero trades
    double barVolume = 1000.0;
    double numTrades = 0.0;

    // This is the FIXED logic - only push when numTrades > 0
    if (numTrades > 0) {
        dist.avg_trade_size.push(barVolume / numTrades);
    }

    assert(dist.avg_trade_size.size() == 0);
    printf("  PASSED: avg_trade_size not pushed when numTrades == 0\n");
}

// ============================================================================
// Test 2: avg_trade_size - pushed correctly when numTrades > 0
// ============================================================================
void test_avg_trade_size_pushed_when_trades_exist() {
    printf("=== Test 2: avg_trade_size pushed when numTrades > 0 ===\n");

    EffortBucketDistribution dist;
    dist.Reset(100);

    double barVolume = 1000.0;
    double numTrades = 10.0;

    if (numTrades > 0) {
        dist.avg_trade_size.push(barVolume / numTrades);
    }

    assert(dist.avg_trade_size.size() == 1);
    assert(std::abs(dist.avg_trade_size.back() - 100.0) < 0.001);  // 1000/10 = 100
    printf("  PASSED: avg_trade_size = %.2f (expected 100.0)\n", dist.avg_trade_size.back());
}

// ============================================================================
// Test 3: abs_close_change - pushed even when value is 0.0
// ============================================================================
void test_abs_close_change_pushed_when_zero() {
    printf("=== Test 3: abs_close_change pushed even when value is 0.0 ===\n");

    EffortBucketDistribution dist;
    dist.Reset(100);

    // Simulate two bars with same close (zero change)
    double barClose = 100.0;
    double prevClose = 100.0;
    double tickSize = 0.25;
    int bar = 1;  // Not bar 0

    if (bar > 0 && tickSize > 0.0 && prevClose > 0.0) {
        double absCloseChange = std::abs(barClose - prevClose) / tickSize;
        dist.abs_close_change.push(absCloseChange);  // Should push 0.0
    }

    assert(dist.abs_close_change.size() == 1);
    assert(std::abs(dist.abs_close_change.back() - 0.0) < 0.001);
    printf("  PASSED: abs_close_change = %.2f (zero is valid)\n", dist.abs_close_change.back());
}

// ============================================================================
// Test 4: abs_close_change - correct tick conversion
// ============================================================================
void test_abs_close_change_tick_conversion() {
    printf("=== Test 4: abs_close_change correct tick conversion ===\n");

    EffortBucketDistribution dist;
    dist.Reset(100);

    double barClose = 100.50;
    double prevClose = 100.00;
    double tickSize = 0.25;
    int bar = 1;

    if (bar > 0 && tickSize > 0.0 && prevClose > 0.0) {
        double absCloseChange = std::abs(barClose - prevClose) / tickSize;
        dist.abs_close_change.push(absCloseChange);
    }

    // 0.50 / 0.25 = 2 ticks
    assert(dist.abs_close_change.size() == 1);
    assert(std::abs(dist.abs_close_change.back() - 2.0) < 0.001);
    printf("  PASSED: abs_close_change = %.2f ticks (expected 2.0)\n", dist.abs_close_change.back());
}

// ============================================================================
// Test 5: abs_close_change - bar 0 skipped (no prevClose)
// ============================================================================
void test_abs_close_change_bar0_skipped() {
    printf("=== Test 5: abs_close_change bar 0 skipped ===\n");

    EffortBucketDistribution dist;
    dist.Reset(100);

    int bar = 0;  // First bar - no prevClose
    double tickSize = 0.25;

    // This should NOT push because bar == 0
    if (bar > 0 && tickSize > 0.0) {
        dist.abs_close_change.push(0.0);
    }

    assert(dist.abs_close_change.size() == 0);
    printf("  PASSED: bar 0 skipped (no prevClose available)\n");
}

// ============================================================================
// Test 6: spreadTicks - not pushed when bid/ask invalid
// ============================================================================
void test_spread_not_pushed_invalid_inputs() {
    printf("=== Test 6: spreadTicks not pushed when inputs invalid ===\n");

    DOMWarmup warmup;
    warmup.Reset(100);

    double tickSize = 0.25;

    // Case 1: bestBid == 0
    double bestBid = 0.0;
    double bestAsk = 100.25;
    if (tickSize > 0.0 && bestBid > 0.0 && bestAsk > 0.0 && bestAsk >= bestBid) {
        warmup.PushSpread((bestAsk - bestBid) / tickSize);
    }
    assert(warmup.spreadTicks.size() == 0);
    printf("  Case 1: bestBid=0 -> not pushed\n");

    // Case 2: bestAsk == 0
    bestBid = 100.00;
    bestAsk = 0.0;
    if (tickSize > 0.0 && bestBid > 0.0 && bestAsk > 0.0 && bestAsk >= bestBid) {
        warmup.PushSpread((bestAsk - bestBid) / tickSize);
    }
    assert(warmup.spreadTicks.size() == 0);
    printf("  Case 2: bestAsk=0 -> not pushed\n");

    // Case 3: tickSize == 0
    bestBid = 100.00;
    bestAsk = 100.25;
    tickSize = 0.0;
    if (tickSize > 0.0 && bestBid > 0.0 && bestAsk > 0.0 && bestAsk >= bestBid) {
        warmup.PushSpread((bestAsk - bestBid) / tickSize);
    }
    assert(warmup.spreadTicks.size() == 0);
    printf("  Case 3: tickSize=0 -> not pushed\n");

    // Case 4: crossed market (bestAsk < bestBid)
    tickSize = 0.25;
    bestBid = 100.50;
    bestAsk = 100.00;  // Crossed!
    if (tickSize > 0.0 && bestBid > 0.0 && bestAsk > 0.0 && bestAsk >= bestBid) {
        warmup.PushSpread((bestAsk - bestBid) / tickSize);
    }
    assert(warmup.spreadTicks.size() == 0);
    printf("  Case 4: crossed market -> not pushed\n");

    printf("  PASSED: all invalid cases correctly rejected\n");
}

// ============================================================================
// Test 7: spreadTicks - pushed when bestAsk == bestBid (locked market, spread=0)
// ============================================================================
void test_spread_pushed_locked_market() {
    printf("=== Test 7: spreadTicks pushed when locked market (spread=0) ===\n");

    DOMWarmup warmup;
    warmup.Reset(100);

    double tickSize = 0.25;
    double bestBid = 100.00;
    double bestAsk = 100.00;  // Locked market

    if (tickSize > 0.0 && bestBid > 0.0 && bestAsk > 0.0 && bestAsk >= bestBid) {
        double spread = (bestAsk - bestBid) / tickSize;
        warmup.PushSpread(spread);
    }

    assert(warmup.spreadTicks.size() == 1);
    assert(std::abs(warmup.spreadTicks.back() - 0.0) < 0.001);
    printf("  PASSED: spread = 0.0 (locked market is valid)\n");
}

// ============================================================================
// Test 8: spreadTicks - pushed correctly when bestAsk > bestBid
// ============================================================================
void test_spread_pushed_normal_spread() {
    printf("=== Test 8: spreadTicks pushed correctly for normal spread ===\n");

    DOMWarmup warmup;
    warmup.Reset(100);

    double tickSize = 0.25;
    double bestBid = 100.00;
    double bestAsk = 100.50;  // 2 ticks spread

    if (tickSize > 0.0 && bestBid > 0.0 && bestAsk > 0.0 && bestAsk >= bestBid) {
        double spread = (bestAsk - bestBid) / tickSize;
        warmup.PushSpread(spread);
    }

    assert(warmup.spreadTicks.size() == 1);
    assert(std::abs(warmup.spreadTicks.back() - 2.0) < 0.001);
    printf("  PASSED: spread = %.2f ticks (expected 2.0)\n", warmup.spreadTicks.back());
}

// ============================================================================
// Test 9: Readiness - EffortBucketDistribution unchanged
// ============================================================================
void test_effort_readiness_unchanged() {
    printf("=== Test 9: EffortBucketDistribution readiness unchanged ===\n");

    EffortBucketDistribution dist;
    dist.Reset(100);

    // Readiness is based on sessionsContributed, NOT on avg_trade_size or abs_close_change
    assert(!dist.IsReady());

    // Push to new metrics - should NOT affect readiness
    dist.avg_trade_size.push(100.0);
    dist.abs_close_change.push(2.0);
    assert(!dist.IsReady());

    // Only sessionsContributed affects readiness
    dist.sessionsContributed = 5;
    assert(dist.IsReady());

    printf("  PASSED: new metrics are non-blocking for readiness\n");
}

// ============================================================================
// Test 10: Readiness - DOMWarmup unchanged
// ============================================================================
void test_dom_readiness_unchanged() {
    printf("=== Test 10: DOMWarmup readiness unchanged ===\n");

    DOMWarmup warmup;
    warmup.Reset(100);

    // Main readiness is based on depthMassCore, NOT spreadTicks
    assert(!warmup.IsReady());

    // Push spread - should NOT affect main IsReady()
    for (int i = 0; i < 10; i++) {
        warmup.PushSpread(1.0);
    }
    assert(!warmup.IsReady());  // Still not ready
    assert(warmup.IsSpreadReady());  // Spread has its own readiness

    // Only depthMassCore affects main readiness
    for (int i = 0; i < 5; i++) {
        warmup.depthMassCore.push(100.0);
    }
    assert(warmup.IsReady());

    printf("  PASSED: spreadTicks is non-blocking for main readiness\n");
}

// ============================================================================
// Main
// ============================================================================
int main() {
    printf("\n=== Tier 1 + Tier 2 Baseline Tests ===\n\n");

    // avg_trade_size tests
    test_avg_trade_size_not_pushed_when_zero_trades();
    test_avg_trade_size_pushed_when_trades_exist();

    // abs_close_change tests
    test_abs_close_change_pushed_when_zero();
    test_abs_close_change_tick_conversion();
    test_abs_close_change_bar0_skipped();

    // spreadTicks tests
    test_spread_not_pushed_invalid_inputs();
    test_spread_pushed_locked_market();
    test_spread_pushed_normal_spread();

    // Readiness tests
    test_effort_readiness_unchanged();
    test_dom_readiness_unchanged();

    printf("\n=== ALL TESTS PASSED ===\n\n");
    return 0;
}
