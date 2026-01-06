---
name: review-impl
description: Process and implement code review feedback systematically. Use when PR feedback, review comments, or improvement suggestions need to be addressed.
---

# Review Implementation Skill

Systematically process and implement code review feedback with progress tracking.

## Usage

- `/review-impl` - Start implementing review feedback
- `/review-impl <PR#>` - Implement feedback from specific PR
- `/review-impl paste` - Paste review comments to process

## Workflow

### Step 1: Parse Feedback

Break reviewer notes into individual items:

```markdown
## Review Feedback Received

1. [ ] "Add validity check before accessing result.percentile"
   - File: AMT_Volatility.h
   - Line: 234
   - Type: Bug fix

2. [ ] "Consider using early return pattern here"
   - File: AMT_Liquidity.h
   - Line: 156-170
   - Type: Style improvement

3. [ ] "Missing test for edge case when baseline empty"
   - File: test/test_volatility.cpp
   - Type: Test coverage

4. [ ] "SSOT comment outdated, update to match new owner"
   - File: AMT_Session.h
   - Line: 45
   - Type: Documentation
```

**Clarify ambiguous items** before proceeding.

### Step 2: Create Tasks

Convert to TodoWrite tasks:

```
TodoWrite([
  { content: "Add validity check in AMT_Volatility.h:234", status: "in_progress" },
  { content: "Refactor to early return in AMT_Liquidity.h:156", status: "pending" },
  { content: "Add empty baseline test", status: "pending" },
  { content: "Update SSOT comment in AMT_Session.h", status: "pending" },
  { content: "Run tests and verify", status: "pending" }
])
```

**First task starts as `in_progress`.**

### Step 3: Implement Changes

For each feedback item:

```
1. Locate relevant code (Read tool)
2. Make modification (Edit tool)
3. Verify correctness (compile, run affected test)
4. Mark complete, move to next
```

### Step 4: Handle Different Feedback Types

| Type | Approach |
|------|----------|
| **Bug fix** | Fix, add regression test if missing |
| **Style** | Apply project conventions |
| **Missing test** | Write test using `/tdd` approach |
| **Documentation** | Update comments/docs |
| **Refactor** | Ensure tests pass before and after |
| **Architecture** | May need `/brainstorm` first |

### Step 5: Validate

After all changes:

```bash
# Compile
g++ -std=c++17 -c AMT_Volatility.h  # Check for errors

# Run affected tests
cd test
./test_volatility_engine.exe
./test_liquidity_engine.exe

# Run full suite
for f in test_*.exe; do ./$f; done
```

### Step 6: Communicate

Update reviewer:
- Progress on each item
- Questions that arose
- Any blockers encountered
- Deferred items with rationale

## AMT-Specific Review Patterns

### SSOT Contract Feedback

**Reviewer:** "This reads from wrong SSOT"

```cpp
// BEFORE (wrong)
const double poc = sessionCtx.rth_poc;  // Stale cache

// AFTER (correct)
const double poc = st->sessionMgr.sessionPOC;  // SSOT owner
```

**Verify:** Check `.claude/rules/architecture.md` for correct SSOT.

### NO-FALLBACK Feedback

**Reviewer:** "Missing IsReady() check"

```cpp
// BEFORE (unsafe)
double pct = result.percentile;  // May be invalid

// AFTER (safe)
if (!result.IsReady()) {
    return;  // Handle not-ready case
}
double pct = result.percentile;  // Now safe
```

### Hysteresis Feedback

**Reviewer:** "State changes too quickly, add hysteresis"

```cpp
// BEFORE (whipsaw)
m_state = newState;

// AFTER (with hysteresis)
if (newState != m_confirmedState) {
    if (newState == m_candidateState) {
        m_confirmationBars++;
        if (m_confirmationBars >= MIN_CONFIRMATION_BARS) {
            m_confirmedState = newState;
        }
    } else {
        m_candidateState = newState;
        m_confirmationBars = 1;
    }
}
```

### Windows Compatibility Feedback

**Reviewer:** "Use macro-safe min/max"

```cpp
// BEFORE (breaks on Windows)
int val = std::min(a, b);

// AFTER (Windows-safe)
int val = (std::min)(a, b);
```

### Test Coverage Feedback

**Reviewer:** "Add test for this edge case"

```cpp
// Add new test case
TEST(handles_empty_baseline) {
    Engine engine;
    // No warmup - baseline empty

    auto result = engine.Compute(validInput);

    ASSERT_FALSE(result.IsReady());
    ASSERT_EQ(result.errorReason, WARMUP_BASELINE);
}
```

## Response Templates

### Acknowledging Feedback

```markdown
Thanks for the review! I'll address the following:

1. ✅ Validity check - Will add
2. ✅ Early return - Will refactor
3. ✅ Missing test - Will add
4. ✅ SSOT comment - Will update

Starting now, will update when complete.
```

### Requesting Clarification

```markdown
Question on item #2:

You mentioned "consider early return pattern" - should I:
A) Refactor just this function
B) Also update similar patterns in the file
C) Create a follow-up issue for broader cleanup

Let me know and I'll proceed accordingly.
```

### Completion Update

```markdown
All feedback addressed:

1. ✅ Added validity check (line 234)
2. ✅ Refactored to early return (lines 156-170)
3. ✅ Added test_handles_empty_baseline
4. ✅ Updated SSOT comment

All tests pass. Ready for re-review.
```

## Tracking Progress

### TodoWrite Updates

```
// As you complete each item:

Mark current as completed → Mark next as in_progress

Only ONE task in_progress at a time
Mark completed IMMEDIATELY when done
```

### Git Commits

One commit per logical change:

```bash
git commit -m "fix: add validity check before accessing result.percentile

Addresses review feedback on PR #123.

Generated with Claude Code"
```

Or batch related changes:

```bash
git commit -m "fix: address review feedback from PR #123

- Add validity check in AMT_Volatility.h:234
- Refactor to early return in AMT_Liquidity.h:156
- Add empty baseline test
- Update SSOT comment

Generated with Claude Code"
```

## Integration with Other Skills

- Use `/tdd` when adding tests for feedback
- Use `/sc-review` to self-check before re-submission
- Use `/kaizen` if feedback reveals deeper issues
- Use `/brainstorm` for architectural feedback
