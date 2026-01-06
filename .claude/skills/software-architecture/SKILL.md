---
name: software-architecture
description: Software architecture principles adapted for AMT Framework. Use when designing new components or reviewing architectural decisions.
---

# Software Architecture for AMT Framework

Quality-focused architecture principles based on Clean Architecture and Domain-Driven Design, adapted for Sierra Chart ACSIL development.

## Usage

- `/software-architecture` - Review current architecture decisions
- `/software-architecture <component>` - Analyze specific component

## Core Principles

### 1. Early Returns Over Nesting

**BAD:**
```cpp
void ProcessBar(const BarData& bar) {
    if (bar.isValid) {
        if (bar.volume > 0) {
            if (phase != UNKNOWN) {
                // Deep nesting - hard to follow
                DoActualWork(bar);
            }
        }
    }
}
```

**GOOD:**
```cpp
void ProcessBar(const BarData& bar) {
    if (!bar.isValid) return;
    if (bar.volume <= 0) return;
    if (phase == UNKNOWN) return;

    DoActualWork(bar);  // Clear path to main logic
}
```

### 2. Size Limits

| Element | Limit | Action |
|---------|-------|--------|
| Function | 50 lines | Decompose into helpers |
| Nesting | 3 levels | Use early returns |
| File | 200 lines | Split into focused files |
| Component | 80 lines | Extract sub-components |

### 3. Domain-Driven Naming

**BAD:**
```cpp
namespace Utils { ... }
class Helpers { ... }
void DoStuff();
double Calculate(double x);
```

**GOOD:**
```cpp
namespace AMT { ... }
class VolatilityEngine { ... }
void ClassifyRegime();
double ComputePercentile(double value, const RollingDist& baseline);
```

## AMT Framework Architecture

### SSOT (Single Source of Truth) Pattern

Every piece of data has ONE authoritative owner:

```cpp
// SSOT Owner Table (from architecture.md)
| Data              | SSOT Owner        | Location          |
|-------------------|-------------------|-------------------|
| Session Phase     | phaseCoordinator  | StudyState        |
| POC/VAH/VAL       | SessionManager    | AMT_Session.h     |
| Market State      | DaltonEngine      | AMT_Analytics.h   |
| Liquidity         | LiquidityEngine   | AMT_Liquidity.h   |
| Volatility        | VolatilityEngine  | AMT_Volatility.h  |
```

**Consumer Pattern:**
```cpp
// Consumers READ from SSOT, never maintain copies
const auto liqState = st->lastLiqSnap.liqState;  // Read from SSOT
// NOT: localLiqState = ComputeLiquidity();      // Don't recompute
```

### Engine Pattern

All engines follow consistent structure:

```cpp
class SomeEngine {
public:
    // Configuration
    void Reset();
    void SetPhase(SessionPhase phase);
    void SetConfig(const SomeConfig& config);

    // SSOT wiring (dependency injection)
    void SetEffortStore(EffortBaselineStore* store);
    void SetDOMWarmup(DOMWarmup* warmup);

    // Pre-warm baseline from historical data
    void PreWarm(const HistoricalData& data, SessionPhase phase);

    // Main computation - returns result with validity
    SomeResult Compute(/* inputs */);

    // Session boundary
    void ResetForSession();
    void FinalizeSession();

private:
    // Phase-aware baselines
    std::array<RollingDist, NUM_PHASES> m_baselines;

    // Hysteresis state
    SomeState m_confirmedState;
    SomeState m_candidateState;
    int m_confirmationBars;
};
```

### Result Pattern (NO-FALLBACK)

```cpp
struct SomeResult {
    // Validity MUST be checked first
    bool IsReady() const { return errorReason == ErrorReason::NONE; }

    // Computed values (only valid if IsReady())
    SomeState state;
    double score;

    // Error information
    ErrorReason errorReason;

    // Helpers
    bool IsError() const { return errorReason != NONE && !IsWarmup(); }
    bool IsWarmup() const { return errorReason == WARMUP_BASELINE; }
};
```

### Separation of Concerns

```
┌─────────────────────────────────────────────────────────────┐
│ AuctionSensor_v1.cpp - Orchestration Layer                  │
│   - SC API interaction                                       │
│   - Input reading                                            │
│   - Engine wiring                                            │
│   - Output writing                                           │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│ AMT_*.h - Domain Layer (SC-independent where possible)      │
│   - Pure computation                                         │
│   - Business logic                                           │
│   - State management                                         │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│ test/*.cpp - Test Layer                                     │
│   - Unit tests (no SC dependency)                           │
│   - Contract verification                                    │
│   - Regression prevention                                    │
└─────────────────────────────────────────────────────────────┘
```

## Anti-Patterns to Eliminate

### 1. NIH (Not Invented Here)

**BAD:** Building custom solution when Sierra Chart provides one.

```cpp
// BAD: Custom percentile calculation
double ComputePercentileManually(std::vector<double>& data, double value);

// GOOD: Use SC's built-in if available, or our tested RollingDist
RollingDist baseline;
double percentile = baseline.percentile(value);
```

### 2. Mixed Concerns

**BAD:**
```cpp
void VolatilityEngine::Compute() {
    // Business logic
    double percentile = baseline.percentile(range);

    // SC API call - doesn't belong here!
    sc.Subgraph[5].Data[idx] = percentile;

    // Logging in computation
    sc.AddMessageToLog("Percentile computed", 0);
}
```

**GOOD:**
```cpp
// Engine: Pure computation
VolatilityResult VolatilityEngine::Compute(double range) {
    return { baseline.percentile(range), /* ... */ };
}

// Orchestration (AuctionSensor_v1.cpp): SC interaction
auto result = engine.Compute(range);
sc.Subgraph[5].Data[idx] = result.percentile;
logManager.Log(/* ... */);
```

### 3. Generic Catch-All Files

**BAD:**
```
AMT_Utils.h       // What's in here?
AMT_Common.h      // Everything and nothing
AMT_Misc.h        // Dumping ground
```

**GOOD:**
```
AMT_Helpers.h     // TimeToSeconds, PriceToTicks (pure utilities)
AMT_Volatility.h  // VolatilityEngine (focused domain)
AMT_Liquidity.h   // LiquidityEngine (focused domain)
```

### 4. Spaghetti Dependencies

**BAD:**
```cpp
// Circular or unclear dependencies
#include "AMT_A.h"  // A includes B
#include "AMT_B.h"  // B includes A
```

**GOOD:**
```cpp
// Clear dependency hierarchy (see architecture.md)
amt_core.h
    └── AMT_Helpers.h
            └── AMT_Snapshots.h
                    └── AMT_Session.h
```

## Decision Checklist

When adding new code, ask:

- [ ] Does this data already have an SSOT? (Don't duplicate)
- [ ] Is this the right file for this code? (Check architecture.md)
- [ ] Can this be tested without SC? (Prefer pure functions)
- [ ] Is the function under 50 lines? (Decompose if not)
- [ ] Is nesting under 3 levels? (Early return if not)
- [ ] Is naming domain-specific? (Avoid Utils/Helpers/Common)

## Integration with Other Skills

- Use `/brainstorm` before architectural decisions
- Use `/kaizen` for continuous improvement
- Use `/sc-review` to verify compliance
