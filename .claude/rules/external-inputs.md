# AMT Framework - External Study Input Dependencies

## Required Studies & Inputs

| Input | Source Study | Purpose |
|-------|--------------|---------|
| **VbP Study** (Input 10) | Volume-by-Price study | POC/VAH/VAL (authoritative) |
| **Numbers Bars** (Inputs 11-13) | Numbers Bars study | Delta, Volume, Trades per bar |
| **DOM Data** (Inputs 14-17) | Sierra Chart DOM | Bid/Ask depth for liquidity |

---

## VbP Study (Input 10)

- **Required for**: Zone anchor prices (POC/VAH/VAL)
- **Data flow**: VbP -> `PopulateFromVbPStudy()` -> `sessionVolumeProfile.session_*`
- **Validation**: Study must be on same chart, correct period (RTH/GLOBEX)

### Reading VbP Study Data

```cpp
const bool success = st->sessionVolumeProfile.PopulateFromVbPStudy(
    sc, vbpStudyId, isCurRTH, rthStartSec, rthEndSec, diagLevel, isLiveBar);
// After success: session_poc, session_vah, session_val, volume_profile are populated

// Load native Peaks/Valleys (HVN/LVN) from VbP study
st->sessionVolumeProfile.PopulatePeaksValleysFromVbP(sc, vbpStudyId, 0, diagLevel);
// After success: session_hvn, session_lvn are populated from SC's Draw Peaks/Valleys
```

---

## Numbers Bars (Inputs 70-79)

### Rate Signals (Inputs 70-71)

- **Input 70**: NB SG53 - Bid Volume Per Second
- **Input 71**: NB SG54 - Ask Volume Per Second

### Footprint Diagonal Delta (Inputs 76-77)

- **Input 76**: NB SG43 - Diagonal Positive Delta Sum
- **Input 77**: NB SG44 - Diagonal Negative Delta Sum
- Compares bid volume at price N vs ask volume at price N+1
- Positive net = aggressive buying (lifting offers)
- Negative net = aggressive selling (hitting bids)

### Average Trade Size (Inputs 78-79)

- **Input 78**: NB SG51 - Average Bid Trade Size
- **Input 79**: NB SG52 - Average Ask Trade Size
- Large avg = institutional activity
- Small avg = retail/HFT activity
- Ratio (ask/bid) > 1 = larger trades lifting offers

### Usage

```cpp
// Diagonal delta reveals footprint imbalance patterns
snap.effort.diagonalNetDelta = diagPosSum - diagNegSum;  // + = bullish

// Average trade size ratio indicates institutional vs retail
snap.effort.avgTradeSizeRatio = avgAskTrade / avgBidTrade;  // >1 = institutional buying
```

---

## DOM Data (Inputs 14-17)

- **Input 14**: Max Depth Levels (for liquidity scan)
- **Input 15**: Max Band Ticks (search radius around POC)
- **Input 16**: Target Depth Mass % (for dynamic width)
- **Input 17**: Core mass percentage threshold
- **Limitation**: DOM data only exists on live bars

---

## Input Validation

Inputs are validated each bar (not just at initialization):
```cpp
statsInputsValid = (nbStudyId > 0 && deltaSubgraphIdx >= 0 && nbTradeIdx >= 0);
domInputsValid = (maxDepthLevels > 0 && maxBandTicks > 0 && targetDepthMassPct > 0.0);
```
