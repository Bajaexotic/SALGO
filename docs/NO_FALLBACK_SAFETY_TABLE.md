# No-Fallback Policy: Short-Circuit Safety Table

> **Contract**: Invalid components are EXCLUDED from calculations and remaining weights are renormalized. No numeric surrogates (0.0, 0.5, -1.0) are ever used as fallback values.

## Metric-Level Validity

| Metric | Valid Flag | Invalid Condition | On Invalid |
|--------|------------|-------------------|------------|
| `deltaConsistency` | `deltaConsistencyValid` | NB inputs unavailable (nbStudyId <= 0) | Excluded from `calculate_score()`, weight renormalized |
| `liquidityAvailability` | `liquidityAvailabilityValid` | DOM inputs invalid OR baseline < 10 samples | Excluded from `calculate_score()`, weight renormalized |
| `domStrength` | `domStrengthValid` | DOM inputs unavailable (maxDepthLevels <= 0) | Excluded from `calculate_score()`, weight renormalized |
| `tpoAcceptance` | `tpoAcceptanceValid` | TPO disabled OR TPO profile invalid | Excluded from `calculate_score()`, weight renormalized |
| `volumeProfileClarity` | `volumeProfileClarityValid` | VBP study unavailable OR profile < 5 levels | Excluded from `calculate_score()`, weight renormalized |

## Component-Level Validity (Within Metrics)

| Metric | Component | Valid Flag | Invalid Condition | On Invalid |
|--------|-----------|------------|-------------------|------------|
| `tpoAcceptance` | `tpoVbpAlignment` | `alignmentValid` | VBP POC unavailable (vbpPOC <= 0) | Excluded from tpoAcceptance blend, weights renormalized |
| `volumeProfileClarity` | `pocDominance` | `pocDominanceValid` | Profile sample size < 10 price levels | Excluded from clarity blend, weights renormalized |
| `domStrength` | `freshness` | `freshnessValid` (in DOMQualityTracker) | No DOM history yet (tracker never updated) | Excluded from domStrength blend, weights renormalized |

## calculate_score() Renormalization

```cpp
// From AMT_Patterns.h calculate_score()
// Only valid metrics contribute; invalid = excluded + renormalized
float totalWeight = 0.0f;
float score = 0.0f;

if (deltaConsistencyValid) {
    score += W_DELTA * deltaConsistency;
    totalWeight += W_DELTA;
}
if (liquidityAvailabilityValid) {
    score += W_LIQUIDITY * liquidityAvailability;
    totalWeight += W_LIQUIDITY;
}
// ... etc for all 5 metrics

return (totalWeight > 0.0f) ? score / totalWeight : 0.0f;
```

## Baseline Readiness Requirements

| Baseline | Min Samples | Flag | Source |
|----------|-------------|------|--------|
| `depth_mass_core` | 10 | `liquidityBaselineMinSamples` | AMT_Snapshots.h |
| `depth_mass_halo` | 10 | `liquidityBaselineMinSamples` | AMT_Snapshots.h |
| `cachedThresholds` | 10 | `Z_SCORE_MIN_SAMPLES` | AMT_VolumeProfile.h |
| DOM tracker history | 1 update | `freshnessValid` | AMT_Snapshots.h |

## Dead Value Pattern

When a component is invalid, its numeric field may contain a "dead value" that is never read:

```cpp
// Example from ComputeTPOAcceptance()
if (!vbpPOCValid)
{
    result.tpoVbpAlignment = 0.0f;  // Dead value - never read
    result.alignmentValid = false;  // This gates usage
}
```

**Guarantee**: Dead values are never used because:
1. The validity flag gates all access at blend point
2. External consumers only see the blended metric value
3. Component values are internal computation intermediates

## Session Stats Log Output

When metrics are invalid, they display as `N/A`:

```
State: ZONE=POC | SESS=RTH_AM | FACIL=BALANCED | DELTA=N/A | LIQ=0.65
```

This provides operational visibility that a metric is excluded (not that it has value 0.0).

## DOM Hashing Performance

| Aspect | Value |
|--------|-------|
| **What's hashed** | 6 scalars: bidLevelCount, askLevelCount, bestBid, bestAsk, bidNonZeroCount, askNonZeroCount |
| **Algorithm** | FNV-1a (64-bit) |
| **Big-O** | O(1) - fixed 6 mix operations |
| **Memory** | Single uint64_t accumulator, no allocations |
| **Call frequency** | Once per bar via `DOMQualityTracker.Update()` |
| **Purpose** | Detect DOM staleness (frozen = stale market data) |

**Note**: We hash aggregate metrics, not individual price levels. This keeps the hash O(1) regardless of configured DOM depth.
