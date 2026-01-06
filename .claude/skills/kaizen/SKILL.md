---
name: kaizen
description: Continuous improvement methodology for AMT Framework. Use for root cause analysis, incremental refinement, and quality-focused development.
---

# Kaizen - Continuous Improvement

Apply Lean manufacturing problem-solving techniques to AMT Framework development.

## Usage

- `/kaizen` - Start improvement analysis
- `/kaizen why` - Five Whys root cause analysis
- `/kaizen trace` - Bug tracing through execution
- `/kaizen fishbone` - Cause-and-effect analysis
- `/kaizen pdca` - Plan-Do-Check-Act cycle

## Four Core Pillars

### 1. Continuous Improvement (Kaizen)

> Small, frequent changes compound into significant improvements.

**Phases of improvement:**
1. First make it **work** (functional)
2. Then make it **clear** (readable)
3. Finally make it **fast** (optimized)

**Never do all three at once.**

**Practices:**
- Leave code better than you found it
- Refactor during normal work (small bits)
- Remove dead code when encountered
- Fix small issues immediately

### 2. Poka-Yoke (Error Proofing)

> Design systems that prevent errors at compile time, not runtime.

**AMT Applications:**

```cpp
// BAD: Runtime check for invalid state
void Process(int state) {
    if (state < 0 || state > 4) {
        LogError("Invalid state");
        return;
    }
}

// GOOD: Compile-time enforcement via enum
enum class VolatilityRegime { COMPRESSION, NORMAL, EXPANSION, EVENT };
void Process(VolatilityRegime regime) {
    // Invalid states are impossible
}
```

```cpp
// BAD: Runtime validity check
struct Result {
    double value;
    bool isValid;
};
// Caller might forget to check isValid

// GOOD: Make invalid states unrepresentable
struct Result {
    bool IsReady() const;  // MUST call first
    double GetValue() const {
        assert(IsReady());  // Enforced
        return m_value;
    }
private:
    double m_value;
};
```

**Principles:**
- Use type system to constrain possibilities
- Validate at system boundaries (SC inputs)
- Guard clauses at function entry
- Make invalid states unrepresentable

### 3. Standardized Work

> Consistency over cleverness.

**For AMT Framework:**
- Follow existing patterns in codebase
- Use established engine structure
- Automate via tests (not manual checks)
- Document conventions in `.claude/rules/`

**Pattern Checklist:**
- [ ] Follows SSOT contract pattern
- [ ] Uses NO-FALLBACK result pattern
- [ ] Implements hysteresis where needed
- [ ] Phase-aware baselines if applicable
- [ ] Consistent naming with existing engines

### 4. Just-In-Time (JIT)

> Build only what's needed now.

**YAGNI in practice:**
- Don't add features "for later"
- Don't optimize without measurements
- Don't abstract until pattern repeats 3+ times
- Complexity emerges from actual needs

**Red flags:**
- "We might need this later"
- "Let's make it configurable just in case"
- "I'll add these extra methods for completeness"

## Problem-Solving Commands

### /kaizen why - Five Whys

Drill from symptom to root cause:

```
Problem: test_volatility_engine fails intermittently

Why 1: Percentile calculation returns NaN sometimes
Why 2: Baseline has zero samples when queried
Why 3: PreWarm isn't called before Compute
Why 4: Session boundary resets baseline but doesn't re-warm
Why 5: ResetForSession clears data but FinalizeSession wasn't called first

Root cause: Session transition sequence incorrect
Fix: Ensure FinalizeSession → ResetForSession → PreWarm sequence
```

### /kaizen trace - Root Cause Tracing

Trace bugs backward through execution:

```
Symptom: LIQ_VOID reported incorrectly

Trace backward:
1. liqState set in Compute() at line 234
2. Composite LIQ calculated from depthRank at line 220
3. depthRank from baseline.percentile() at line 215
4. Baseline fed by PreWarm() with historical data
5. Historical data from GetLastDominantSide() API
6. FOUND: Using BSE_BUY/BSE_SELL incorrectly for both sides

Root: API misuse in historical depth extraction
```

### /kaizen fishbone - Cause and Effect

Explore causes across categories:

```
Problem: Volatility detection unreliable

         ┌─ People ──────────────┐
         │  - Misunderstanding   │
         │    of requirements    │
         │                       │
┌─ Process ──────────────────────┴─ Technology ─────────┐
│  - Phase boundaries           │  - Baseline warmup   │
│    not handled                │    insufficient      │
│  - Session transitions        │  - Hysteresis not    │
│    incomplete                 │    implemented       │
│                               │                      │
└─ Environment ──────────────────┴─ Methods ───────────┘
   - GLOBEX vs RTH differ       │  - Single threshold  │
     dramatically               │    not robust        │
   - Holiday sessions           │  - Missing edge      │
     anomalous                  │    cases             │
```

### /kaizen pdca - Plan-Do-Check-Act

Iterative improvement cycle:

```
┌─────────────────────────────────────────────────────────┐
│ PLAN                                                    │
│ - Define problem clearly                                │
│ - Analyze current baseline                              │
│ - Develop hypothesis                                    │
│ - Design small-scale test                               │
├─────────────────────────────────────────────────────────┤
│ DO                                                      │
│ - Implement change in isolated scope                    │
│ - Document what was changed                             │
│ - Keep change small and reversible                      │
├─────────────────────────────────────────────────────────┤
│ CHECK                                                   │
│ - Run tests, measure results                            │
│ - Compare to baseline                                   │
│ - Identify unexpected effects                           │
├─────────────────────────────────────────────────────────┤
│ ACT                                                     │
│ - If successful: standardize (add tests, document)      │
│ - If failed: analyze why, refine hypothesis             │
│ - Start next PDCA cycle                                 │
└─────────────────────────────────────────────────────────┘
```

## Red Flags (Anti-Kaizen)

| Red Flag | Problem | Kaizen Response |
|----------|---------|-----------------|
| "Fix it later" | Technical debt accumulates | Fix now, small increment |
| "Works on my machine" | Environment dependency | Standardize, document |
| "It's always been that way" | Stagnation | Question, improve |
| "Too risky to change" | Fear of regression | Add tests first |
| "We need a big rewrite" | All-or-nothing thinking | Incremental improvement |

## Integration with AMT Framework

### Applying Kaizen to Engines

```cpp
// Before: Rushed implementation
double VolatilityEngine::GetPercentile() {
    return baseline.percentile(range);  // No validity check
}

// After Kaizen iteration 1: Make it work (add validity)
double VolatilityEngine::GetPercentile() {
    if (!baseline.isReady()) return -1.0;
    return baseline.percentile(range);
}

// After Kaizen iteration 2: Make it clear (proper pattern)
VolatilityResult VolatilityEngine::Compute() {
    if (!baseline.isReady()) {
        return { .errorReason = WARMUP_BASELINE };
    }
    return { .percentile = baseline.percentile(range) };
}

// After Kaizen iteration 3: Make it fast (if needed)
// Profile first! Only optimize measured bottlenecks.
```

### Daily Kaizen Questions

At end of each work session:
- What did I learn today?
- What small improvement did I make?
- What dead code did I remove?
- What could be clearer?
- What will I improve tomorrow?

## Integration with Other Skills

- Use `/tdd` to enable safe refactoring
- Use `/sc-review` to verify standards
- Use `/brainstorm` for larger improvements
- Use `/software-architecture` for structural kaizen
