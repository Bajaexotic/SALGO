---
name: brainstorm
description: Structured brainstorming for AMT Framework design decisions. Use before implementing new engines, major refactors, or architectural changes.
---

# Brainstorming Skill for AMT Framework

Transform ideas into fully-formed designs through structured dialogue BEFORE implementation.

## Usage

- `/brainstorm` - Start brainstorming session for current idea
- `/brainstorm <topic>` - Brainstorm specific topic (e.g., "new imbalance detection")

## 3-Phase Process

### Phase 1: Understand the Idea

1. Examine project context (read relevant `.claude/rules/` files)
2. Ask clarifying questions **ONE AT A TIME**
3. Identify: purpose, constraints, success criteria

**Question Style:**
- Prefer multiple choice when possible
- Open-ended when needed
- Never ask more than one question per message

**Example Questions:**
```
Which engine will this feature belong to?
A) VolatilityEngine
B) LiquidityEngine
C) ImbalanceEngine
D) New engine

What's the primary use case?
A) Gating other signals (context gate)
B) Generating direct trade signals
C) Providing diagnostic/logging info
D) Something else (please describe)
```

### Phase 2: Explore Approaches

1. Propose 2-3 alternatives with trade-offs
2. Present options conversationally with reasoning
3. Lead with recommended approach

**Template:**
```
I see three approaches for this:

**Option A: [Name] (Recommended)**
- How: [Brief description]
- Pros: [List]
- Cons: [List]
- Fits AMT patterns: [Yes/No - why]

**Option B: [Name]**
- How: [Brief description]
- Trade-offs vs A: [List]

**Option C: [Name]**
- How: [Brief description]
- When to prefer: [Conditions]

Which resonates with your goals?
```

### Phase 3: Present the Design

Break into 200-300 word sections:
1. Architecture overview
2. Data flow / SSOT ownership
3. Component interactions
4. Error handling / NO-FALLBACK behavior
5. Hysteresis / state management

**Validate after each section** before proceeding.

## AMT-Specific Design Questions

### SSOT Ownership
```
Who owns this data?
A) New struct (becomes SSOT)
B) Existing engine (extend it)
C) Derived/computed (no owner needed)
```

### Integration Pattern
```
How will this integrate with existing engines?
A) Consumer of existing SSOT (reads from other engines)
B) Producer for existing consumers (other engines read from this)
C) Standalone (no engine dependencies)
D) Bidirectional (needs careful ordering)
```

### Phase Awareness
```
Does this need phase-aware baselines?
A) Yes - behavior differs significantly GLOBEX vs RTH
B) No - behavior is phase-independent
C) Partial - only some components need phase awareness
```

### Hysteresis Requirements
```
Does state need hysteresis?
A) Yes - prevent whipsaw between states
B) No - immediate transitions are correct
C) Configurable - user should control
```

## Design Document Template

```markdown
# [Feature Name] Design

## Problem Statement
[What problem does this solve? 2-3 sentences]

## Success Criteria
- [ ] [Measurable criterion 1]
- [ ] [Measurable criterion 2]

## SSOT Ownership

| Data | Owner | Location |
|------|-------|----------|
| [Data 1] | [Struct/Engine] | [Header file] |

## Data Flow

```
[Source] -> [Transform] -> [Consumer]
```

## API Design

```cpp
struct NewResult {
    bool IsReady() const;
    // ...
};

class NewEngine {
    void SetPhase(SessionPhase phase);
    NewResult Compute(/* inputs */);
};
```

## NO-FALLBACK Contract
- Returns `IsReady() = false` when: [conditions]
- Error reasons: [enum values]

## Hysteresis
- Confirmation bars: [N]
- Persistence bars: [M]
- Transition detection: [how]

## Test Plan
1. [ ] Test: returns not ready without warmup
2. [ ] Test: phase-aware baseline separation
3. [ ] Test: hysteresis prevents whipsaw
4. [ ] Test: SSOT contract enforced

## Files to Modify
- [ ] `AMT_NewEngine.h` - New header
- [ ] `AuctionSensor_v1.cpp` - Integration
- [ ] `test/test_new_engine.cpp` - Tests
```

## YAGNI Checkpoint

Before finalizing design, ruthlessly ask:

- [ ] Is every feature actually needed NOW?
- [ ] Can we start simpler and extend later?
- [ ] Are we building for real requirements or imagined ones?
- [ ] What's the minimum viable implementation?

## Output

After brainstorming completes:
1. Write design to `docs/plans/YYYY-MM-DD_<topic>.md`
2. Commit to git with message: `docs: design for <topic>`
3. If implementation follows, transition to `/tdd` workflow

## Key Principles

- One question per message
- Multiple choice when possible
- Explore alternatives before deciding
- Validate incrementally
- Apply YAGNI ruthlessly
- Document decisions and rationale
