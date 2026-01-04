# AMT Context Capsule
<!-- MAX 60 LINES. When switching features: archive to artifacts/context_archive.md -->

## Objective
Sierra Chart study implementing Auction Market Theory zone analysis with session-gated market state classification.

## Current Scope
**Feature**: Market State Estimator - session-gated, prior-influenced BALANCE/IMBALANCE classification.

## Invariants (Non-Negotiable)
1. **Session-start freshness**: History alone cannot produce non-UNDEFINED state at session start
2. **No double-lag**: Single smoothing layer (prior-blending + hysteresis, not stacked)
3. **SSOT for confirmed state**: `MarketStateTracker.confirmedState` is authoritative for all consumers
4. **Prior bounded**: Prior influence decays as `priorMass / (sessionBars + priorMass)`

## Working Set

| File | Symbol/Function | Role | Anchor |
|------|-----------------|------|--------|
| `AMT_Analytics.h` | `MarketStateTracker` | Tracker struct | :145-382 |
| `AMT_Analytics.h` | `Update()` | Per-bar confirmation | :192-266 |
| `AMT_Analytics.h` | `ResetForSession()` | Session roll handler | :276-290 |
| `AMT_Analytics.h` | `UpdatePrior()` | Prior EWMA update | :298-311 |
| `AMT_config.h` | `marketState*` tunables | Configuration | :342-366 |
| `AuctionSensor_v1.cpp` | rawState computation | Instantaneous classifier | :3274-3276 |
| `AuctionSensor_v1.cpp` | `marketStateTracker.Update()` | Per-bar call site | :3321-3322 |
| `AuctionSensor_v1.cpp` | Session roll block | Prior update + reset | :1821-1824 |
| `AuctionSensor_v1.cpp` | `amtPhaseHistory.clear()` | Session roll trigger | :1810 |
| `AMT_Session.h` | `SessionManager.DidSessionChange()` | Session boundary SSOT | :317 |

## How to Resume
1. **Read `docs/STATUS.md` only** — single entrypoint
2. **Do one step** — execute Next action, store evidence in `artifacts/`
3. **Update STATUS** — then `/clear`

## Operating Rules
- Read `docs/STATUS.md` only (≤25 lines, overwrite only).
- One step per loop → update STATUS → `/clear`.
- Long outputs → `artifacts/` with anchor summary.

## File Roles (lazy-load all except STATUS)
| Category | Path | Read when? |
|----------|------|------------|
| Control plane | `docs/STATUS.md` | Every session |
| Working state | `docs/CONTEXT,DECISIONS,NEXT.md` | If STATUS references |
| Evidence | `artifacts/*.md` | To verify claims |
| SSOT reference | `CLAUDE.md` | Anchor lookups only |
| Source code | `*.h`, `*.cpp` | Targeted edits only |
| SC reference | `studies_help/` | SC syntax questions |
| Tests | `test/` | Running tests only |

## Rotation Rules
| File | Cap | Trigger | Archive to |
|------|-----|---------|------------|
| STATUS.md | 25 | — | overwrite only |
| NEXT.md | 50 | PASS items | `artifacts/verified_log.md` |
| DECISIONS.md | 100 | feature complete | `artifacts/decision_archive.md` |
| CONTEXT.md | 60 | feature switch | `artifacts/context_archive.md` |

Archive = append to archive file, remove from source. Zero deletion.
