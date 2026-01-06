---
name: git-worktrees
description: Create isolated git worktrees for parallel development. Use when working on experimental features or needing to context-switch without stashing.
---

# Git Worktrees for AMT Framework

Create isolated workspaces for parallel development on multiple branches simultaneously.

## Usage

- `/git-worktrees` - Create worktree for current branch/feature
- `/git-worktrees <branch>` - Create worktree for specific branch
- `/git-worktrees list` - List existing worktrees

## What Are Worktrees?

Git worktrees let you check out multiple branches simultaneously in separate directories, sharing the same repository. Perfect for:

- Experimental features that might break things
- Comparing behavior between branches
- Quick context switches without stashing
- Running tests on one branch while developing another

## Directory Selection

### Priority Order

1. **Existing directory**: Check for `.worktrees/` or `worktrees/` in project
2. **CLAUDE.md preference**: Check for documented location
3. **User choice**: Offer local or global options

### Recommended Structure

```
E:\SierraChart\ACS_Source\           # Main working directory
E:\SierraChart\ACS_Source\.worktrees\
    feature-new-engine\              # Worktree 1
    bugfix-liquidity\                # Worktree 2
    experiment-hysteresis\           # Worktree 3
```

## Workflow

### 1. Create Worktree

```bash
# Extract project name
PROJECT_NAME=$(basename $(git rev-parse --show-toplevel))

# Create branch and worktree
git worktree add .worktrees/feature-name -b feature-name

# Or for existing branch
git worktree add .worktrees/existing-branch existing-branch
```

### 2. Verify .gitignore

**CRITICAL**: Ensure worktree directory is ignored before creating.

```bash
# Check if .worktrees is ignored
git check-ignore .worktrees/

# If not ignored, add to .gitignore first
echo ".worktrees/" >> .gitignore
git add .gitignore
git commit -m "chore: ignore worktrees directory"
```

### 3. Set Up Worktree

```bash
cd .worktrees/feature-name

# For AMT Framework - no package manager needed
# Just verify compilation works
g++ -std=c++17 -c -I.. AMT_Test.cpp
```

### 4. Run Baseline Tests

```bash
# Compile and run tests to verify clean state
cd test
g++ -std=c++17 -I.. -o test_ssot_invariants.exe test_ssot_invariants.cpp
./test_ssot_invariants.exe
```

### 5. Work in Worktree

```bash
# Navigate to worktree
cd E:\SierraChart\ACS_Source\.worktrees\feature-name

# Make changes, test, commit as normal
# Changes are isolated to this worktree
```

### 6. Clean Up When Done

```bash
# From main worktree
cd E:\SierraChart\ACS_Source

# Remove worktree (keeps branch)
git worktree remove .worktrees/feature-name

# Or remove worktree AND delete branch
git worktree remove .worktrees/feature-name
git branch -d feature-name
```

## AMT-Specific Considerations

### Header Dependencies

Worktrees share headers via `#include "../AMT_*.h"`. Relative paths work if worktree is one level deep:

```
ACS_Source/
    AMT_Volatility.h           # Shared headers
    .worktrees/
        feature/
            test/
                test_x.cpp     # #include "../../AMT_Volatility.h"
```

### Test Compilation

Each worktree can compile tests independently:

```bash
cd .worktrees/feature/test
g++ -std=c++17 -I../.. -o test_feature.exe test_feature.cpp
```

### Comparing Behavior

Run same test in both worktrees to compare:

```bash
# Terminal 1: Main
cd E:\SierraChart\ACS_Source\test
./test_volatility_engine.exe

# Terminal 2: Feature branch
cd E:\SierraChart\ACS_Source\.worktrees\feature\test
./test_volatility_engine.exe
```

## Safety Checklist

Before creating worktree:

- [ ] `.worktrees/` is in `.gitignore`
- [ ] No uncommitted changes in main worktree (or stash them)
- [ ] Branch name is descriptive and unique

Before removing worktree:

- [ ] All changes committed
- [ ] Changes pushed if needed
- [ ] Branch merged or preserved

## Common Mistakes to Avoid

1. **Skipping .gitignore check** - Worktree contents get tracked
2. **Assuming directory exists** - Always check/create
3. **Proceeding with failing tests** - Fix before continuing
4. **Hardcoding paths** - Use relative paths and git commands
5. **Forgetting to clean up** - Orphaned worktrees waste space

## Commands Reference

```bash
# List all worktrees
git worktree list

# Add worktree with new branch
git worktree add <path> -b <new-branch>

# Add worktree with existing branch
git worktree add <path> <existing-branch>

# Remove worktree
git worktree remove <path>

# Prune stale worktree references
git worktree prune
```

## Integration with Other Skills

After worktree setup:
- Use `/tdd` for test-driven development
- Use `/finish-branch` when ready to merge
- Use `/brainstorm` for design work in isolated environment
