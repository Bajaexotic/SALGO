---
name: tdd
description: Test-Driven Development for AMT Framework. Use when implementing new features, fixing bugs, or refactoring engines. Write tests BEFORE implementation code.
---

# Test-Driven Development (TDD) for AMT Framework

Apply TDD methodology when developing AMT Framework components. Tests live in `test/` directory as standalone C++ executables.

## Usage

- `/tdd` - Start TDD workflow for current task
- `/tdd <component>` - Start TDD for specific engine/component

## The Iron Law

> **No production code without a failing test first.**
> Code written before tests must be deleted and reimplemented from scratch.

## Red-Green-Refactor Cycle

```
1. RED     - Write ONE minimal failing test
2. VERIFY  - Confirm test fails for the RIGHT reason
3. GREEN   - Write SIMPLEST code to pass
4. VERIFY  - All tests pass
5. REFACTOR - Clean up while staying green
6. REPEAT  - Next behavior
```

## AMT Framework Test Structure

### Test File Template

```cpp
// test/test_<component>.cpp
#include <iostream>
#include <cassert>
#include "../AMT_<Component>.h"

// Test helpers (no SC dependencies)
#define TEST(name) void test_##name()
#define RUN(name) do { std::cout << "  " #name "..."; test_##name(); std::cout << " OK\n"; } while(0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) { std::cerr << "\n    FAIL: " << #a << " != " << #b << "\n"; std::exit(1); } } while(0)
#define ASSERT_TRUE(x) do { if (!(x)) { std::cerr << "\n    FAIL: " << #x << "\n"; std::exit(1); } } while(0)

// ============ TESTS ============

TEST(component_initializes_correctly) {
    // Arrange
    ComponentEngine engine;

    // Act
    engine.Reset();

    // Assert
    ASSERT_TRUE(!engine.IsReady());
}

TEST(component_returns_not_ready_without_baseline) {
    // Arrange
    ComponentEngine engine;

    // Act
    auto result = engine.Compute(/* minimal inputs */);

    // Assert
    ASSERT_TRUE(!result.IsReady());
    ASSERT_EQ(result.errorReason, ErrorReason::WARMUP_BASELINE);
}

// ============ MAIN ============

int main() {
    std::cout << "test_<component>\n";
    RUN(component_initializes_correctly);
    RUN(component_returns_not_ready_without_baseline);
    std::cout << "All tests passed!\n";
    return 0;
}
```

### Compilation (Windows/g++)

```bash
cd test
g++ -std=c++17 -I.. -o test_<component>.exe test_<component>.cpp
./test_<component>.exe
```

## Good Test Characteristics

| Property | Description |
|----------|-------------|
| **Minimal** | One behavior per test |
| **Named** | Describes what SHOULD happen |
| **Real** | Uses real engine code, minimal mocks |
| **Behavioral** | Tests WHAT not HOW |
| **Independent** | No shared state between tests |

## AMT-Specific Test Patterns

### 1. NO-FALLBACK Contract Tests

```cpp
TEST(returns_not_ready_when_baseline_insufficient) {
    Engine engine;
    // Don't warm up baseline
    auto result = engine.Compute(validInputs);
    ASSERT_TRUE(!result.IsReady());
    // Must NOT have fake/default values
}
```

### 2. Phase-Aware Baseline Tests

```cpp
TEST(uses_phase_specific_baseline) {
    Engine engine;
    engine.SetPhase(SessionPhase::INITIAL_BALANCE);
    // Warm up IB baseline
    for (int i = 0; i < 20; i++) {
        engine.PreWarm(ibData[i], SessionPhase::INITIAL_BALANCE);
    }
    auto result = engine.Compute(testInput);
    ASSERT_TRUE(result.IsReady());
}
```

### 3. Hysteresis Tests

```cpp
TEST(regime_change_requires_confirmation_bars) {
    Engine engine;
    warmUpBaseline(engine);

    // Single bar at new regime - should NOT change
    auto r1 = engine.Compute(newRegimeInput);
    ASSERT_EQ(r1.confirmedRegime, OriginalRegime);
    ASSERT_TRUE(r1.isTransitioning);

    // After confirmation bars - should change
    for (int i = 0; i < MIN_CONFIRMATION_BARS; i++) {
        engine.Compute(newRegimeInput);
    }
    auto r2 = engine.Compute(newRegimeInput);
    ASSERT_EQ(r2.confirmedRegime, NewRegime);
}
```

### 4. SSOT Contract Tests

```cpp
TEST(ssot_owner_is_authoritative) {
    // The SSOT owner's value must be used, not derived/cached copies
    SessionManager mgr;
    mgr.sessionPOC = 5000.0;

    // Consumer must read from SSOT
    ASSERT_EQ(GetPOC(mgr), 5000.0);

    // Changing SSOT updates consumers
    mgr.sessionPOC = 5001.0;
    ASSERT_EQ(GetPOC(mgr), 5001.0);
}
```

## When to Apply TDD

**Always use for:**
- New engine features (VolatilityEngine, LiquidityEngine, etc.)
- Bug fixes (reproduce bug in test first)
- Refactoring (tests prove behavior unchanged)
- SSOT contract changes

**Exceptions (require explicit approval):**
- Throwaway prototypes
- SC-dependent code (use integration tests separately)
- Configuration changes

## Common Rationalizations (REJECTED)

| Excuse | Reality |
|--------|---------|
| "I'll test after" | You won't. And you don't know if test works. |
| "Too simple to test" | Simple code has simple tests. Write them. |
| "Manual testing is enough" | Manual tests aren't repeatable or automated. |
| "Sunk cost - already wrote it" | Delete it. Rewrite with test first. |

## Verification Checklist

Before marking implementation complete:

- [ ] Every public method has at least one test
- [ ] Each test failed initially for the correct reason
- [ ] Only minimal code was written to pass each test
- [ ] All tests pass with clean output
- [ ] No test-only code in production headers

## Workflow

1. **Identify** the next behavior to implement
2. **Write** a failing test for that behavior
3. **Run** test, verify it fails correctly
4. **Implement** minimum code to pass
5. **Run** all tests, verify green
6. **Refactor** if needed (stay green)
7. **Commit** with test + implementation together
8. **Repeat** for next behavior
