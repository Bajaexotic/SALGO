# AMT Decision Log
<!-- MAX 100 LINES. When full: archive to artifacts/decision_archive.md -->

## D001: Market State Estimator Architecture (2024-12-25)

### Decision
Extend `MarketStateTracker` with session-gated, prior-influenced confirmation logic. Do NOT modify rawState computation.

### Alternatives Considered

| Alternative | Rejected Because |
|-------------|------------------|
| Modify rawState directly | rawState is per-bar instantaneous; adding gates there would violate SSOT separation |
| Separate prior tracker | Would create two smoothing layers (double-lag); prior must blend into single confirmation |
| Session-only (no prior) | User requirement: historical context should inform early-session estimates |
| Prior-only (no session gate) | Violates freshness invariant: history could produce confident labels before session evidence accumulates |

### Evidence Anchors

| Concept | File | Line(s) | Code/Pattern |
|---------|------|---------|--------------|
| rawState unchanged | `AuctionSensor_v1.cpp` | 3274-3276 | `(isTrending \|\| isExtremeDelta) ? IMBALANCE : BALANCE` |
| Session sufficiency gate | `AMT_Analytics.h` | 206-211 | `if (sessionBars < minSessionBars) { confirmedState = UNDEFINED }` |
| Prior decay formula | `AMT_Analytics.h` | 217-218 | `priorWeight = priorMass / (sessionBars + priorMass)` |
| Blended confirmation | `AMT_Analytics.h` | 222 | `blendedBalance = sessionWeight * sessionRatio + priorWeight * priorBalance` |
| Confirmation margin | `AMT_Analytics.h` | 227-232 | `>= 0.5 + margin` or `<= 0.5 - margin` |
| Hysteresis | `AMT_Analytics.h` | 250-256 | `candidateBars >= minConfirmationBars` |
| Session roll: UpdatePrior | `AuctionSensor_v1.cpp` | 1821 | `st->marketStateTracker.UpdatePrior()` |
| Session roll: ResetForSession | `AuctionSensor_v1.cpp` | 1824 | `st->marketStateTracker.ResetForSession()` |
| Tunables location | `AMT_config.h` | 342-366 | `marketStateMinSessionBars`, `marketStatePriorMass`, etc. |

### Risks / Open Questions

1. **Threshold semantics**: `confirmationMargin=0.1` means need 60% blended probability. Is this too high/low for typical sessions? UNVERIFIED empirically.
2. **Prior update quality gate**: `priorUpdateMinBars=100` may exclude short sessions. Acceptable tradeoff for prior stability.
3. **Prior initialization**: Starts at 0.5 (uninformed). First few sessions may be noisy until prior converges.

### Rollback Path
If design proves wrong:
1. Remove `UpdatePrior()` and `ResetForSession()` calls from session roll block (lines 1821-1824)
2. Revert `MarketStateTracker` to original simple hysteresis (backup in git history)
3. Remove tunables from `AMT_config.h` lines 342-366
4. Restore original log format at lines 3013-3039
