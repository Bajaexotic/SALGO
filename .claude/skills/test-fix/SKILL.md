---
name: test-fix
description: Systematic approach to fixing failing tests. Use when test suite has failures that need methodical resolution.
---

# Test Fixing Skill

Systematically identify and repair failing tests using intelligent error grouping.

## Usage

- `/test-fix` - Start test fixing workflow
- `/test-fix <test_file>` - Fix specific test file
- `/test-fix all` - Fix all failing tests

## 5-Phase Methodology

### Phase 1: Diagnosis

Run test suite and catalog all failures:

```bash
# Run all tests and capture output
cd test
for f in test_*.exe; do
    echo "=== $f ===" >> test_results.txt
    ./$f >> test_results.txt 2>&1 || echo "FAILED: $f" >> failures.txt
done
```

Note patterns:
- Error types (assertion, crash, timeout)
- Affected modules
- Potential root causes

### Phase 2: Strategic Grouping

Group failures by:

| Grouping | Examples |
|----------|----------|
| **Error Type** | AssertionError, SegFault, WARMUP errors |
| **Module/File** | VolatilityEngine, LiquidityEngine |
| **Root Cause** | Missing baseline, API change, refactor impact |

**Priority Order:**
1. Infrastructure (imports, dependencies, build)
2. API changes (signatures, contracts)
3. Logic defects (assertions, edge cases)

### Phase 3: Iterative Resolution

For each priority group:

```
┌─────────────────────────────────────────────────────────┐
│ 1. Identify underlying cause                            │
│    - Read test code                                     │
│    - Read implementation                                │
│    - Check recent changes (git log, git diff)          │
├─────────────────────────────────────────────────────────┤
│ 2. Apply targeted fix                                   │
│    - Minimal change                                     │
│    - Follow project conventions                         │
│    - Maintain SSOT contracts                            │
├─────────────────────────────────────────────────────────┤
│ 3. Validate with focused test run                       │
│    - Run only the affected test first                   │
│    - Then run related tests                             │
├─────────────────────────────────────────────────────────┤
│ 4. Advance only after group passes                      │
│    - Don't move on with failures                        │
│    - One group at a time                                │
└─────────────────────────────────────────────────────────┘
```

### Phase 4: Fix Hierarchy

Address in this order:

**Level 1: Infrastructure**
```cpp
// Missing include
#include "../AMT_NewHeader.h"

// Build configuration
g++ -std=c++17 -I.. ...  // Ensure C++17
```

**Level 2: API Changes**
```cpp
// Function signature changed
// OLD: engine.Compute(a, b)
// NEW: engine.Compute(a, b, c)

// Update test to match new API
auto result = engine.Compute(range, atr, normalizedRange);
```

**Level 3: Logic Defects**
```cpp
// Assertion value wrong
ASSERT_EQ(result.regime, COMPRESSION);  // Was NORMAL

// Edge case not handled
if (baseline.size() == 0) return NOT_READY;
```

### Phase 5: Comprehensive Validation

After all groups fixed:

```bash
# Run complete test suite
cd test
for f in test_*.exe; do
    echo "Running $f..."
    ./$f || { echo "STILL FAILING: $f"; exit 1; }
done
echo "All tests pass!"
```

## AMT Framework Test Patterns

### Common Failure: WARMUP_BASELINE

**Symptom:** Test expects ready result but gets WARMUP error.

**Diagnosis:**
```cpp
TEST(some_test) {
    Engine engine;
    auto result = engine.Compute(input);
    ASSERT_TRUE(result.IsReady());  // FAILS - no warmup
}
```

**Fix:**
```cpp
TEST(some_test) {
    Engine engine;
    warmUpBaseline(engine, testData, 20);  // Add warmup

    auto result = engine.Compute(input);
    ASSERT_TRUE(result.IsReady());  // Now passes
}
```

### Common Failure: Phase Mismatch

**Symptom:** Percentile values unexpected.

**Diagnosis:** Test uses IB data but engine set to GLOBEX phase.

**Fix:**
```cpp
TEST(phase_test) {
    Engine engine;
    engine.SetPhase(SessionPhase::INITIAL_BALANCE);  // Match data
    warmUpBaseline(engine, ibData, SessionPhase::INITIAL_BALANCE);

    auto result = engine.Compute(ibInput);
    ASSERT_NEAR(result.percentile, expected, 0.01);
}
```

### Common Failure: Hysteresis Timing

**Symptom:** State change expected but not happening.

**Diagnosis:** Not enough confirmation bars.

**Fix:**
```cpp
TEST(hysteresis_test) {
    Engine engine;
    warmUp(engine);

    // Single bar won't change state
    engine.Compute(newStateInput);
    ASSERT_EQ(engine.GetState(), OLD_STATE);  // Still old

    // Need MIN_CONFIRMATION_BARS
    for (int i = 0; i < MIN_CONFIRMATION_BARS; i++) {
        engine.Compute(newStateInput);
    }
    ASSERT_EQ(engine.GetState(), NEW_STATE);  // Now changed
}
```

### Common Failure: Float Comparison

**Symptom:** Values "equal" but assertion fails.

**Fix:**
```cpp
// BAD
ASSERT_EQ(result.percentile, 0.75);  // Float comparison

// GOOD
ASSERT_NEAR(result.percentile, 0.75, 0.001);  // With tolerance
```

## Test Helper Patterns

### Warmup Helper

```cpp
void warmUpBaseline(Engine& engine, int samples = 20) {
    for (int i = 0; i < samples; i++) {
        engine.PreWarm(generateTestData(i), SessionPhase::INITIAL_BALANCE);
    }
}
```

### Assert Near Helper

```cpp
#define ASSERT_NEAR(a, b, tol) do { \
    if (std::abs((a) - (b)) > (tol)) { \
        std::cerr << "\n    FAIL: " << #a << "=" << (a) \
                  << " not near " << #b << "=" << (b) << "\n"; \
        std::exit(1); \
    } \
} while(0)
```

### Diagnostic Output

```cpp
#define ASSERT_EQ_VERBOSE(a, b) do { \
    if ((a) != (b)) { \
        std::cerr << "\n    FAIL: " << #a << " = " << (a) \
                  << ", expected " << #b << " = " << (b) << "\n"; \
        std::exit(1); \
    } \
} while(0)
```

## Workflow Tracking

Use TodoWrite throughout:

```
Initial: 5 test failures

□ Group 1: Infrastructure (1 failure)
  - test_new_engine missing include
□ Group 2: API changes (2 failures)
  - test_volatility signature change
  - test_liquidity signature change
□ Group 3: Logic (2 failures)
  - test_hysteresis wrong expected value
  - test_percentile float comparison
□ Final validation: Run all tests
```

## Integration with Other Skills

- Use `/tdd` to add missing tests
- Use `/kaizen trace` for hard-to-find root causes
- Use `/subagent-dev` for parallel test fixing across engines
- Use `/sc-review` after fixes to verify quality
