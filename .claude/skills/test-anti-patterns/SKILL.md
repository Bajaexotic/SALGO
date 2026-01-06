---
name: test-anti-patterns
description: Reference for testing anti-patterns to avoid. Use when reviewing tests or debugging test failures that seem wrong.
---

# Testing Anti-Patterns Reference

Avoid these patterns when writing tests for AMT Framework components.

## Core Principle

> **Test what the code does, not what the mocks do.**
> Mocks exist for isolation, not as test subjects.

## The Iron Laws

1. Never verify mock behavior
2. Avoid test-only methods in production classes
3. Don't mock without understanding dependencies

## 5 Major Anti-Patterns

### Anti-Pattern 1: Testing Mock Behavior

**BAD:**
```cpp
TEST(bad_tests_mock_not_real_code) {
    MockEngine mockEngine;
    mockEngine.SetReturn(42);

    // This only tests that the mock exists!
    ASSERT_EQ(mockEngine.GetValue(), 42);
}
```

**GOOD:**
```cpp
TEST(good_tests_real_engine) {
    VolatilityEngine engine;
    engine.SetEffortStore(&realStore);
    warmUpBaseline(engine, realData);

    // Tests actual engine behavior
    auto result = engine.Compute(100.0, 50.0);
    ASSERT_TRUE(result.IsReady());
    ASSERT_EQ(result.regime, VolatilityRegime::NORMAL);
}
```

### Anti-Pattern 2: Test-Only Methods in Production

**BAD:**
```cpp
// AMT_Engine.h - POLLUTED with test code
class Engine {
public:
    void Compute();
    void ResetForTesting();  // BAD: Only exists for tests
    void SetInternalStateForTesting(int x);  // BAD
};
```

**GOOD:**
```cpp
// AMT_Engine.h - Clean production code
class Engine {
public:
    void Compute();
    void Reset();  // Used by both production and tests
};

// test/test_helpers.h - Test utilities separate
void WarmUpEngine(Engine& e, const TestData& data);
```

### Anti-Pattern 3: Mocking Without Understanding

**BAD:**
```cpp
TEST(bad_over_mocked) {
    // Mock everything without understanding what's needed
    MockStore mockStore;
    MockWarmup mockWarmup;
    MockPhase mockPhase;

    Engine engine(&mockStore, &mockWarmup, &mockPhase);
    // What are we even testing?
}
```

**GOOD:**
```cpp
TEST(good_minimal_dependencies) {
    // Use real baseline store - it's lightweight
    EffortBaselineStore realStore;

    // Only mock what's truly external (SC API, file I/O, etc.)
    Engine engine;
    engine.SetEffortStore(&realStore);

    // Now we're testing real engine behavior
}
```

### Anti-Pattern 4: Incomplete Test Data

**BAD:**
```cpp
TEST(bad_partial_data) {
    Liq3Result result;
    result.liqState = LIQ_NORMAL;
    // Missing: depthRank, stressRank, spreadRank, validity flags...

    ProcessLiquidity(result);  // May crash or behave unexpectedly
}
```

**GOOD:**
```cpp
TEST(good_complete_data) {
    Liq3Result result;
    result.liqState = LIQ_NORMAL;
    result.liqValid = true;
    result.depthRank = 0.65;
    result.stressRank = 0.30;
    result.resilienceRank = 0.70;
    result.spreadRank = 0.25;
    result.compositeLIQ = 0.55;
    result.errorReason = LiquidityErrorReason::NONE;

    ProcessLiquidity(result);  // Behaves as in production
}
```

### Anti-Pattern 5: Tests as Afterthought

**BAD:**
```cpp
// Day 1: Write 500 lines of engine code
// Day 2: "Now let me add some tests"
// Result: Tests written to match buggy implementation
```

**GOOD:**
```cpp
// Write test first
TEST(volatility_compression_below_p25) {
    // This test defines the requirement
    Engine engine;
    warmUp(engine);
    auto result = engine.Compute(lowRangeValue);
    ASSERT_EQ(result.regime, COMPRESSION);
}

// Then implement to make it pass
```

## AMT-Specific Anti-Patterns

### Anti-Pattern 6: Ignoring NO-FALLBACK Contract

**BAD:**
```cpp
TEST(bad_assumes_always_ready) {
    Engine engine;
    // No warmup!
    auto result = engine.Compute(input);
    ASSERT_EQ(result.value, expectedValue);  // WRONG: result.IsReady() is false
}
```

**GOOD:**
```cpp
TEST(good_checks_validity) {
    Engine engine;
    auto result = engine.Compute(input);

    // Must check validity first
    ASSERT_FALSE(result.IsReady());
    ASSERT_EQ(result.errorReason, ErrorReason::WARMUP_BASELINE);
}

TEST(good_with_warmup) {
    Engine engine;
    warmUpBaseline(engine, sufficientData);

    auto result = engine.Compute(input);
    ASSERT_TRUE(result.IsReady());  // Now safe to check values
    ASSERT_EQ(result.value, expectedValue);
}
```

### Anti-Pattern 7: Testing SC-Dependent Code Directly

**BAD:**
```cpp
TEST(bad_needs_sierra_chart) {
    SCStudyInterfaceRef sc;  // Won't compile without SC
    ProcessBar(sc);
}
```

**GOOD:**
```cpp
// Extract testable logic into SC-independent functions
TEST(good_tests_pure_logic) {
    // AMT_Helpers.h functions are SC-independent
    double result = CalculatePercentile(value, distribution);
    ASSERT_NEAR(result, expected, 0.001);
}

// Integration testing happens in SC environment separately
```

### Anti-Pattern 8: Hardcoded Magic Numbers

**BAD:**
```cpp
TEST(bad_magic_numbers) {
    auto result = engine.Compute(4.5, 2.3, 0.7);
    ASSERT_EQ(result.state, 2);  // What is 2?
}
```

**GOOD:**
```cpp
TEST(good_named_constants) {
    const double barRange = 4.5;
    const double atrValue = 2.3;
    const double normalizedRange = barRange / atrValue;  // 1.96

    auto result = engine.Compute(barRange, atrValue, normalizedRange);
    ASSERT_EQ(result.regime, VolatilityRegime::EXPANSION);
}
```

## Why TDD Prevents These Anti-Patterns

| Anti-Pattern | How TDD Prevents It |
|--------------|---------------------|
| Testing mocks | You write test BEFORE code exists - must test real behavior |
| Test-only methods | Test defines public interface - no need for backdoors |
| Incomplete data | Test shows exactly what inputs are needed |
| Afterthought tests | Tests come first by definition |
| Ignoring validity | First test is "returns not ready without warmup" |

## Checklist When Reviewing Tests

- [ ] Tests exercise real code, not mocks
- [ ] No test-only methods in production headers
- [ ] Mocks used only for true external dependencies (file I/O, network, SC API)
- [ ] Test data matches production data structures completely
- [ ] Validity/IsReady() checked before accessing result values
- [ ] Named constants explain what values mean
- [ ] Tests define behavior, not confirm existing implementation
