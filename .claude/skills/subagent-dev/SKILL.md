---
name: subagent-dev
description: Dispatch subagents for parallel investigation of independent tasks. Use for multiple unrelated bugs, test failures, or parallel feature research.
---

# Subagent-Driven Development

Execute implementation plans by dispatching independent subagents with code review checkpoints.

## Usage

- `/subagent-dev` - Start subagent workflow for current tasks
- `/subagent-dev parallel` - Force parallel investigation mode
- `/subagent-dev sequential` - Force sequential execution mode

## When to Use

**Use subagent approach when:**
- 3+ independent tasks without shared dependencies
- Multiple test failures in different subsystems
- Parallel investigation needed (different engines, files)
- Code review checkpoints desired between tasks

**Don't use when:**
- Tasks are tightly coupled
- Single focused task
- Changes affect same files

## Execution Models

### Sequential Execution

**When:** Tasks are coupled or must be done in order.

```
┌──────────────────────────────────────────────────────────┐
│ 1. Load plan, create task list                           │
│    └─► TodoWrite with all tasks                         │
├──────────────────────────────────────────────────────────┤
│ 2. For each task:                                        │
│    a. Dispatch implementation subagent                   │
│    b. Code review via reviewer subagent                  │
│    c. Fix Critical/Important issues                      │
│    d. Mark complete, proceed to next                     │
├──────────────────────────────────────────────────────────┤
│ 3. Final architectural review                            │
├──────────────────────────────────────────────────────────┤
│ 4. Use /finish-branch to complete                        │
└──────────────────────────────────────────────────────────┘
```

### Parallel Investigation

**When:** Independent failures in separate domains.

```
┌──────────────────────────────────────────────────────────┐
│ 1. Group failures by subsystem                           │
│    - VolatilityEngine failures                           │
│    - LiquidityEngine failures                            │
│    - ImbalanceEngine failures                            │
├──────────────────────────────────────────────────────────┤
│ 2. Create focused task for each group                    │
│    - Specific scope boundaries                           │
│    - Clear success criteria                              │
├──────────────────────────────────────────────────────────┤
│ 3. Dispatch agents concurrently                          │
│    Task tool with run_in_background=true                 │
├──────────────────────────────────────────────────────────┤
│ 4. Collect results, verify no conflicts                  │
├──────────────────────────────────────────────────────────┤
│ 5. Integrated review of all changes                      │
└──────────────────────────────────────────────────────────┘
```

## AMT Framework Task Templates

### Test Failure Investigation

```markdown
## Task: Fix [Engine] Test Failures

**Scope:** AMT_[Engine].h and test/test_[engine].cpp only

**Failures:**
1. test_[name]_1 - [error message]
2. test_[name]_2 - [error message]

**Constraints:**
- Do NOT modify other engines
- Do NOT change SSOT contracts
- Maintain NO-FALLBACK behavior

**Success Criteria:**
- All [engine] tests pass
- No new warnings
- Changes limited to scope
```

### Feature Implementation

```markdown
## Task: Implement [Feature] in [Engine]

**Scope:** AMT_[Engine].h

**Requirements:**
1. [Specific requirement]
2. [Specific requirement]

**SSOT Contract:**
- Owner: [Engine]
- Consumers: [List]

**Constraints:**
- Follow hysteresis pattern (N confirmation bars)
- Include NO-FALLBACK handling
- Phase-aware if needed

**Tests Required:**
- test_returns_not_ready_without_warmup
- test_hysteresis_prevents_whipsaw
- test_phase_aware_baselines
```

## Critical Rules

### Never Skip Code Review

Between each sequential task:
```
Implementation → Review → Fix Issues → Next Task
```

Review checks:
- [ ] SSOT contracts maintained
- [ ] NO-FALLBACK honored
- [ ] Windows compatibility
- [ ] No unintended side effects

### Handle Blockers Properly

**When subagent is blocked:**

```
DON'T: Try to fix manually in main context
DO: Dispatch fix-specific subagent with narrow scope
```

```markdown
## Task: Unblock [Original Task]

**Blocker:** [Description of what's blocking]

**Investigation scope:** [Specific files to check]

**Resolution options:**
1. [Potential fix 1]
2. [Potential fix 2]
```

### Parallel Safety

**Avoid parallel when:**
- Failures are related (same root cause)
- Changes would conflict (same files)
- Full system context needed

**Safe parallel candidates:**
- Different engines (VolatilityEngine vs LiquidityEngine)
- Different test files
- Different feature areas

## Workflow Example

### Multiple Test Failures

```
Initial state: 5 test failures across 3 engines

1. Analyze and group:
   - VolatilityEngine: 2 failures (related to hysteresis)
   - LiquidityEngine: 2 failures (related to depth calculation)
   - ImbalanceEngine: 1 failure (unrelated)

2. Determine execution mode:
   - Different engines = PARALLEL safe

3. Create task list:
   □ Fix VolatilityEngine hysteresis tests
   □ Fix LiquidityEngine depth tests
   □ Fix ImbalanceEngine test

4. Dispatch parallel agents:
   - Agent 1: VolatilityEngine scope
   - Agent 2: LiquidityEngine scope
   - Agent 3: ImbalanceEngine scope

5. Collect results:
   - Agent 1: Fixed via [change]
   - Agent 2: Fixed via [change]
   - Agent 3: Fixed via [change]

6. Verify no conflicts:
   - Run all tests together
   - Check for overlapping changes

7. Integrated review:
   - Review all changes together
   - Ensure consistency

8. Complete:
   - All tests pass
   - Use /finish-branch
```

## Integration with Other Skills

- Use `/tdd` within each subagent for test-first approach
- Use `/test-fix` methodology for systematic fixing
- Use `/finish-branch` when all tasks complete
- Use `/sc-review` for final quality check

## Task Tracking

Always use TodoWrite to track:

```cpp
// Initial setup
TodoWrite([
  { content: "Fix VolatilityEngine tests", status: "pending" },
  { content: "Fix LiquidityEngine tests", status: "pending" },
  { content: "Fix ImbalanceEngine tests", status: "pending" },
  { content: "Integrated review", status: "pending" },
  { content: "Run full test suite", status: "pending" }
])

// As work progresses
// Mark in_progress when starting
// Mark completed immediately when done
// Only ONE task in_progress at a time
```
