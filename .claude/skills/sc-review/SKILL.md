---
name: sc-review
description: Review code for Sierra Chart ACSIL protocol compliance and AMT Framework conventions. Use when checking code quality against SSOT contracts, Windows compatibility, and DRY patterns.
---

# Sierra Chart Code Review Skill

Review code for Sierra Chart ACSIL protocol compliance and AMT Framework conventions.

## Usage

- `/review` - Review all changed files (git diff)
- `/review <file>` - Review a specific file
- `/review all` - Review entire codebase

## Review Checklist

### 1. ACSIL Basics

- [ ] `SetDefaults` properly configured (study name, inputs, outputs, `sc.AutoLoop`)
- [ ] Proper use of `sc.*` API functions
- [ ] Input validation on each bar (not just initialization)
- [ ] Correct bar indexing (`sc.Index`, `sc.ArraySize`)
- [ ] Proper handling of `sc.SetDefaults` vs runtime code paths

### 2. AMT Framework SSOT Contracts

Reference `.claude/rules/` for authoritative documentation:

- [ ] **Session Phase**: Use `st->SyncSessionPhase()` helper, not manual 3-line sync
- [ ] **Zone Anchors**: VbP study is SSOT for POC/VAH/VAL, not calculated values
- [ ] **Session Extremes**: `StructureTracker` owns high/low, use `zm.GetSessionHigh/Low()`
- [ ] **Market State**: `DaltonEngine.phase` is SSOT, not accumulator-based state
- [ ] **CurrentPhase**: `DaltonState.DeriveCurrentPhase()` is SSOT
- [ ] **Liquidity**: `lastLiqSnap` is SSOT, not staging locations
- [ ] **Zone Clearing**: Use `zm.ClearZonesOnly()` atomic helper

### 3. Windows Compatibility

- [ ] `(std::min)` and `(std::max)` with parentheses to prevent macro interference
- [ ] No POSIX-only functions without Windows alternatives
- [ ] Proper path handling (backslashes vs forward slashes)

### 4. Memory & Performance

- [ ] No heap allocations in hot paths (per-bar processing)
- [ ] No `std::string` temporaries with `.c_str()` causing dangling pointers
- [ ] Prefer stack allocation or pre-allocated buffers
- [ ] Avoid redundant calculations - cache expensive results
- [ ] Check loop bounds to prevent buffer overruns

### 5. Data Source Integrity

- [ ] **Closed Bar Policy**: `deltaConsistency` and `liquidityAvailability` use closed bar (curBarIdx - 1)
- [ ] **Baseline Validity**: Always check `IsReady()` before using baseline values
- [ ] **NO-FALLBACK Contract**: Missing baselines produce "not ready", not fake values
- [ ] **Rate vs Total**: Don't compare rate signals against total baselines
- [ ] **Historical Depth API**: Use `GetLastDominantSide()` before `GetLastBidQuantity/GetLastAskQuantity`

### 6. DRY Violations (from patterns.md)

- [ ] No string temporaries with `.c_str()` on same line
- [ ] No duplicate session phase storage
- [ ] Zone creation through `CreateZonesFromProfile()` only
- [ ] Use `BaselineEngine` members, not nonexistent `depth`
- [ ] Zone clearing via `ClearZonesOnly()` helper

### 7. Common ACSIL Patterns

- [ ] `sc.MaintainHistoricalMarketDepthData = 1` if using historical depth
- [ ] `sc.SetUseMarketDepthPullingStackingData(1)` if using stack/pull
- [ ] Proper study input references (check study exists before reading)
- [ ] Error logging rate-limited (not spamming on every bar)

## Output Format

For each issue found, report:
```
[SEVERITY] file:line - Description
  Problem: What's wrong
  Fix: How to fix it
  Reference: Relevant .claude/rules/ file if applicable
```

Severity levels:
- **ERROR**: Will cause bugs or crashes
- **WARN**: Violates conventions, potential issues
- **INFO**: Style/optimization suggestions

## Instructions

1. If a specific file is provided, review only that file
2. If "all" is specified, review all `.cpp` and `.h` files in the project
3. Otherwise, get changed files from `git diff --name-only` and review those
4. Read each file and check against all checklist items
5. Report findings grouped by file
6. Summarize total issues at the end
