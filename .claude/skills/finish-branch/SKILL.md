---
name: finish-branch
description: Complete development work on a branch with proper verification and integration options. Use when done with feature/bugfix implementation.
---

# Finishing a Development Branch

Guide completion of development work with verification and structured integration options.

## Usage

- `/finish-branch` - Finish current branch
- `/finish-branch <branch>` - Finish specific branch

## Workflow

### Step 1: Verify Tests Pass

**REQUIRED before proceeding.**

```bash
# Run all relevant tests
cd test
./test_ssot_invariants.exe
./test_volatility_engine.exe
./test_liquidity_engine.exe
# ... any tests related to changes
```

**If tests fail: STOP.** Fix issues before continuing.

### Step 2: Determine Base Branch

```bash
# Find the base branch (usually main or master)
git log --oneline --decorate | head -20

# Or check tracking
git branch -vv
```

For AMT Framework, base is typically `main`.

### Step 3: Present Integration Options

Choose ONE of the following:

---

#### Option A: Merge Locally

**When to use:** Small changes, sole developer, want clean history.

```bash
# Switch to base
git checkout main

# Get latest
git pull origin main

# Merge feature
git merge feature-branch

# Verify tests still pass!
cd test && ./test_ssot_invariants.exe

# Delete feature branch
git branch -d feature-branch

# Clean up worktree if used
git worktree remove .worktrees/feature-branch
```

---

#### Option B: Create Pull Request

**When to use:** Code review needed, CI/CD integration, team collaboration.

```bash
# Push branch to remote
git push -u origin feature-branch

# Create PR via GitHub CLI
gh pr create --title "feat: add new volatility detection" --body "$(cat <<'EOF'
## Summary
- Added compression detection to VolatilityEngine
- Implemented hysteresis for regime changes
- Added phase-aware baselines

## Test plan
- [x] test_volatility_engine.exe passes
- [x] test_ssot_invariants.exe passes
- [ ] Manual verification in SC

Generated with Claude Code
EOF
)"
```

**Keep worktree** for potential follow-up changes from review.

---

#### Option C: Keep As-Is

**When to use:** Work paused, waiting for dependencies, need more time.

Branch and worktree preserved. Resume later with:

```bash
cd .worktrees/feature-branch
# Continue work...
```

---

#### Option D: Discard Work

**When to use:** Experiment failed, approach abandoned, starting over.

**DESTRUCTIVE - Requires confirmation.**

```bash
# Type "discard" to confirm
read -p "Type 'discard' to permanently delete: " confirm
if [ "$confirm" = "discard" ]; then
    git worktree remove .worktrees/feature-branch --force
    git branch -D feature-branch
    echo "Branch and worktree deleted."
fi
```

---

## Decision Tree

```
Tests pass?
├── No → Fix tests first, do not proceed
└── Yes → Choose integration method:
    ├── Need review? → Option B (PR)
    ├── Ready to merge? → Option A (Local merge)
    ├── Not finished? → Option C (Keep)
    └── Failed experiment? → Option D (Discard)
```

## Critical Rules

1. **Never merge with failing tests**
2. **Verify tests on merged result** (not just feature branch)
3. **Only clean up worktree for merge or discard** (not PR/keep)
4. **Require explicit confirmation for destructive actions**

## Post-Merge Verification

After local merge (Option A):

```bash
# Ensure merged code works
cd test
./test_ssot_invariants.exe
./test_volatility_engine.exe

# Check for any regressions
git diff HEAD~1 --stat

# Push if everything passes
git push origin main
```

## AMT Framework Checklist

Before finishing any branch:

- [ ] All new code follows SSOT contracts
- [ ] Windows compatibility maintained (`(std::min)`, etc.)
- [ ] NO-FALLBACK contract honored (IsReady() checks)
- [ ] Hysteresis patterns correct (if applicable)
- [ ] Tests added for new functionality
- [ ] Tests pass in isolation AND after merge

## Commit Message Format

For merge commits:

```
feat: add volatility regime detection with hysteresis

- Implement 4-regime classification (COMPRESSION/NORMAL/EXPANSION/EVENT)
- Add phase-aware baselines via EffortBaselineStore
- Include TradabilityRules for downstream gating
- Add test_volatility_engine.exe with 15 test cases

Closes #123

Generated with Claude Code

Co-Authored-By: Claude <noreply@anthropic.com>
```

## Integration with Other Skills

- After `/tdd` workflow completes
- After `/review-impl` addresses feedback
- Leads to `/git-worktrees` cleanup
