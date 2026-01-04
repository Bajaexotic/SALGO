// ============================================================================
// AMT_Updates.h
// Zone update logic - volume profile, engagement tracking, rotation
// ============================================================================

#ifndef AMT_UPDATES_H
#define AMT_UPDATES_H

// Only include sierrachart.h if not already mocked (for standalone testing)
#ifndef SIERRACHART_H
#include "sierrachart.h"
#endif

#include "amt_core.h"
#include "AMT_config.h"
#include "AMT_Helpers.h"
#include "AMT_Zones.h"
#include <algorithm>
#include <cmath>
#include <climits>

namespace AMT {

// ============================================================================
// VOLUME PROFILE INTEGRATION
// ============================================================================

/**
 * Find POC (Point of Control) from Sierra Chart volume profile
 * Returns the price with highest volume
 */
inline double FindPOC(const s_VolumeAtPriceV2* volumeProfile, 
                     int numPrices,
                     double tickSize)
{
    if (!volumeProfile || numPrices == 0) return 0.0;
    
    int maxVolumeIndex = 0;
    double maxVolume = volumeProfile[0].Volume;
    
    for (int i = 1; i < numPrices; i++) {
        if (volumeProfile[i].Volume > maxVolume) {
            maxVolume = volumeProfile[i].Volume;
            maxVolumeIndex = i;
        }
    }
    
    return volumeProfile[maxVolumeIndex].PriceInTicks * tickSize;
}

/**
 * Find Value Area High (VAH)
 * Value area contains 70% of volume, VAH is the upper boundary
 */
inline double FindVAH(const s_VolumeAtPriceV2* volumeProfile,
                     int numPrices,
                     double tickSize,
                     double poc)
{
    if (!volumeProfile || numPrices == 0) return 0.0;
    
    // Calculate total volume
    double totalVolume = 0.0;
    for (int i = 0; i < numPrices; i++) {
        totalVolume += volumeProfile[i].Volume;
    }
    
    double targetVolume = totalVolume * 0.70;  // 70% value area
    
    // Find POC index
    int pocIndex = -1;
    for (int i = 0; i < numPrices; i++) {
        double price = volumeProfile[i].PriceInTicks * tickSize;
        if (std::fabs(price - poc) < tickSize * 0.1) {
            pocIndex = i;
            break;
        }
    }
    
    if (pocIndex == -1) return poc;
    
    // Expand symmetrically from POC until we capture 70% of volume
    int lowerIndex = pocIndex;
    int upperIndex = pocIndex;
    double vaVolume = volumeProfile[pocIndex].Volume;
    
    while (vaVolume < targetVolume) {
        bool canExpandLower = (lowerIndex > 0);
        bool canExpandUpper = (upperIndex < numPrices - 1);
        
        if (!canExpandLower && !canExpandUpper) break;
        
        double lowerVol = canExpandLower ? volumeProfile[lowerIndex - 1].Volume : 0.0;
        double upperVol = canExpandUpper ? volumeProfile[upperIndex + 1].Volume : 0.0;
        
        // Expand to side with more volume
        if (lowerVol >= upperVol && canExpandLower) {
            lowerIndex--;
            vaVolume += lowerVol;
        } else if (canExpandUpper) {
            upperIndex++;
            vaVolume += upperVol;
        } else if (canExpandLower) {
            lowerIndex--;
            vaVolume += lowerVol;
        }
    }
    
    return volumeProfile[upperIndex].PriceInTicks * tickSize;
}

/**
 * Find Value Area Low (VAL)
 */
inline double FindVAL(const s_VolumeAtPriceV2* volumeProfile,
                     int numPrices,
                     double tickSize,
                     double poc)
{
    if (!volumeProfile || numPrices == 0) return 0.0;
    
    // Calculate total volume
    double totalVolume = 0.0;
    for (int i = 0; i < numPrices; i++) {
        totalVolume += volumeProfile[i].Volume;
    }
    
    double targetVolume = totalVolume * 0.70;
    
    // Find POC index
    int pocIndex = -1;
    for (int i = 0; i < numPrices; i++) {
        double price = volumeProfile[i].PriceInTicks * tickSize;
        if (std::fabs(price - poc) < tickSize * 0.1) {
            pocIndex = i;
            break;
        }
    }
    
    if (pocIndex == -1) return poc;
    
    // Expand symmetrically from POC
    int lowerIndex = pocIndex;
    int upperIndex = pocIndex;
    double vaVolume = volumeProfile[pocIndex].Volume;
    
    while (vaVolume < targetVolume) {
        bool canExpandLower = (lowerIndex > 0);
        bool canExpandUpper = (upperIndex < numPrices - 1);
        
        if (!canExpandLower && !canExpandUpper) break;
        
        double lowerVol = canExpandLower ? volumeProfile[lowerIndex - 1].Volume : 0.0;
        double upperVol = canExpandUpper ? volumeProfile[upperIndex + 1].Volume : 0.0;
        
        if (lowerVol >= upperVol && canExpandLower) {
            lowerIndex--;
            vaVolume += lowerVol;
        } else if (canExpandUpper) {
            upperIndex++;
            vaVolume += upperVol;
        } else if (canExpandLower) {
            lowerIndex--;
            vaVolume += lowerVol;
        }
    }
    
    return volumeProfile[lowerIndex].PriceInTicks * tickSize;
}

/**
 * Update zone volume characteristics from Sierra Chart volume profile
 */
inline void UpdateZoneVolume(ZoneRuntime& zone,
                            const s_VolumeAtPriceV2* volumeProfile,
                            int numPrices,
                            double tickSize,
                            double sessionAvgVolumePerTick)
{
    if (!volumeProfile || numPrices == 0) return;
    
    // Find closest price level in volume profile (tick-based - SSOT)
    int closestIndex = -1;
    long long minDistTicks = LLONG_MAX;
    const long long zoneAnchorTicks = zone.GetAnchorTicks();

    for (int i = 0; i < numPrices; i++) {
        // PriceInTicks is already in tick units from Sierra Chart
        const long long priceTicks = static_cast<long long>(volumeProfile[i].PriceInTicks);
        const long long distTicks = std::abs(priceTicks - zoneAnchorTicks);
        if (distTicks < minDistTicks) {
            minDistTicks = distTicks;
            closestIndex = i;
        }
    }

    if (closestIndex == -1) return;
    
    const s_VolumeAtPriceV2& level = volumeProfile[closestIndex];
    
    // Update raw volume metrics
    zone.levelProfile.absoluteVolume = level.Volume;
    zone.levelProfile.bidVolume = level.BidVolume;
    zone.levelProfile.askVolume = level.AskVolume;
    
    // Calculate delta
    zone.levelProfile.cumulativeDelta = level.AskVolume - level.BidVolume;
    
    double totalVol = level.BidVolume + level.AskVolume;
    if (totalVol > 0.0) {
        zone.levelProfile.deltaRatio = zone.levelProfile.cumulativeDelta / totalVol;
    }
    
    // Calculate volume ratio (vs session average)
    if (sessionAvgVolumePerTick > 0.0) {
        zone.levelProfile.volumeRatio = level.Volume / sessionAvgVolumePerTick;
    }
    
    // Count bars at level (approximate from volume profile data)
    zone.levelProfile.barsAtLevel = static_cast<int>(level.NumberOfTrades);
    
    // Rank by volume (would need full profile, set to 1 for POC placeholder)
    if (zone.type == ZoneType::VPB_POC) {
        zone.levelProfile.rankByVolume = 1;
    }
}

// ============================================================================
// ENGAGEMENT TRACKING
// ============================================================================

/**
 * Update current engagement metrics (called every bar while at zone)
 */
inline void UpdateEngagementMetrics(ZoneRuntime& zone,
                                   double currentPrice,
                                   double currentVolume,
                                   double currentDelta,
                                   double tickSize,
                                   int bar,
                                   SCDateTime time)
{
    if (zone.proximity != ZoneProximity::AT_ZONE) return;
    
    EngagementMetrics& eng = zone.currentEngagement;
    
    // Accumulate volume and delta
    eng.cumulativeVolume += currentVolume;
    eng.cumulativeDelta += currentDelta;
    
    // Update duration
    eng.barsEngaged++;
    if (eng.startTime.GetAsDouble() > 0.0) {
        eng.secondsEngaged = GetElapsedSeconds(eng.startTime, time);
    }
    
    // Track peak penetration (how far beyond anchor)
    double distTicks = GetExactTickDistance(currentPrice, zone.GetAnchorPrice(), tickSize);
    int penetrationTicks = static_cast<int>(std::ceil(distTicks));
    if (penetrationTicks > eng.peakPenetrationTicks) {
        eng.peakPenetrationTicks = penetrationTicks;
    }
    
    // Update average close price
    eng.avgClosePrice = (eng.avgClosePrice * (eng.barsEngaged - 1) + currentPrice) 
                       / eng.barsEngaged;
}

/**
 * Update rotation metrics (tracks higher highs, lower lows)
 */
inline void UpdateRotationMetrics(RotationMetrics& rotation,
                                 double currentHigh,
                                 double currentLow,
                                 double priorHigh,
                                 double priorLow)
{
    // Higher high detection
    if (currentHigh > priorHigh) {
        rotation.consecutiveHigherHighs++;
        rotation.consecutiveLowerHighs = 0;
    } else if (currentHigh < priorHigh) {
        rotation.consecutiveLowerHighs++;
        rotation.consecutiveHigherHighs = 0;
    }
    
    // Lower low detection
    if (currentLow < priorLow) {
        rotation.consecutiveLowerLows++;
        rotation.consecutiveHigherLows = 0;
    } else if (currentLow > priorLow) {
        rotation.consecutiveHigherLows++;
        rotation.consecutiveLowerLows = 0;
    }
    
    // Absorption pattern: Higher lows + Lower highs (selling into rally)
    rotation.isAbsorption = (rotation.consecutiveHigherLows >= 3 && 
                            rotation.consecutiveLowerHighs >= 2);
    
    // Exhaustion pattern: Lower highs + Higher lows (buying into decline)
    rotation.isExhaustion = (rotation.consecutiveLowerLows >= 3 &&
                            rotation.consecutiveHigherHighs >= 2);
}

/**
 * Classify engagement outcome (acceptance vs rejection)
 */
inline AuctionOutcome ClassifyEngagementOutcome(
    const EngagementMetrics& engagement,
    const ZoneConfig& cfg)
{
    // Not enough data yet
    if (engagement.barsEngaged < 2) {
        return AuctionOutcome::PENDING;
    }
    
    // ACCEPTANCE criteria:
    // - Sustained time at level
    // - High volume
    // - Price settled near zone
    bool longDuration = (engagement.barsEngaged >= cfg.acceptanceMinBars);
    bool highVolume = (engagement.volumeRatio >= cfg.acceptanceVolRatio);
    
    if (longDuration && highVolume) {
        return AuctionOutcome::ACCEPTED;
    }
    
    // REJECTION criteria:
    // - Quick reversal
    // - Deep penetration then return
    bool quickReversal = (engagement.barsEngaged <= cfg.acceptanceMinBars &&
                         engagement.peakPenetrationTicks > 5);
    
    if (quickReversal) {
        return AuctionOutcome::REJECTED;
    }
    
    // Default: still pending
    return AuctionOutcome::PENDING;
}

/**
 * Detect failed auction pattern
 * Price broke beyond boundary but quickly returned = failed auction
 *
 * HIGH PRIORITY FIX: Now uses proper boundary tracking instead of barsSinceTouch.
 * The zone's UpdateBoundaryTracking() must be called each bar to update state.
 */
inline bool DetectFailedAuction(const ZoneRuntime& zone,
                               double /* currentPrice */,
                               double /* tickSize */,
                               const ZoneSessionState& /* ctx */,
                               const ZoneConfig& cfg)
{
    // Only check boundary zones (VAH/VAL)
    if (zone.role != ZoneRole::VALUE_BOUNDARY) return false;

    // Use the proper boundary tracking via IsFailedAuction()
    // This checks: wasOutsideBoundary && returned within threshold bars
    return zone.IsFailedAuction(cfg.failedAuctionMaxBars);
}

/**
 * Classify volume characteristics
 * Determines if level is HVN, LVN, responsive, initiative, etc.
 * SSOT: Uses classification.density from cached thresholds (not ratio-based)
 */
inline void ClassifyVolumeCharacteristics(ZoneRuntime& zone,
                                         const ZoneConfig& cfg)
{
    VolumeCharacteristics& vol = zone.levelProfile;

    // Already classified via GetNodeType() accessor or ClassifyFromThresholds()
    // Just update cluster detection

    // Cluster width detection (if adjacent prices also HVN)
    // SSOT: Use IsHVN_SSOT() instead of IsHVN(cfg) for consistency
    // This would require full volume profile - for now use placeholder
    vol.clusterWidthTicks = vol.IsHVN_SSOT() ? 3 : 1;
}

/**
 * Update all engagement flags based on metrics
 * SSOT: Uses VolumeThresholds when available; falls back to ratio-based if not provided
 */
inline void UpdateEngagementFlags(EngagementMetrics& eng,
                                 const ZoneConfig& cfg,
                                 const VolumeThresholds* ssotThresholds = nullptr)
{
    // High/Low volume engagement - SSOT: Use cached thresholds when available
    if (ssotThresholds != nullptr && ssotThresholds->valid) {
        // SSOT classification using sigma-based thresholds on cumulative volume
        VAPDensityClass density = ssotThresholds->ClassifyVolume(eng.cumulativeVolume);
        eng.wasHighVolume = (density == VAPDensityClass::HIGH);
        eng.wasLowVolume = (density == VAPDensityClass::LOW);
    } else {
        // Fallback to ratio-based (deprecated path)
        eng.wasHighVolume = (eng.volumeRatio >= cfg.hvnThreshold);
        eng.wasLowVolume = (eng.volumeRatio <= cfg.lvnThreshold);
    }

    // Delta aligned (delta matches price direction)
    double avgDeltaRatio = (eng.cumulativeVolume > 0.0) ?
                          (eng.cumulativeDelta / eng.cumulativeVolume) : 0.0;
    eng.wasDeltaAligned = (std::fabs(avgDeltaRatio) > cfg.buyingNodeThreshold);

    // Responsive defense (high volume + opposite delta)
    bool highVol = eng.wasHighVolume;
    bool oppositeDelta = (std::fabs(avgDeltaRatio) > cfg.sellingNodeThreshold);
    eng.wasResponsiveDefense = (highVol && oppositeDelta);
}

/**
 * Complete zone update pipeline (called every bar for each zone)
 * SSOT: Optional VolumeThresholds parameter for sigma-based classification
 *
 * MEDIUM PRIORITY FIX: Added priorHigh/priorLow for rotation metric tracking.
 * Caller must provide prior bar's OHLC for proper higher-high/lower-low detection.
 */
inline void UpdateZoneComplete(ZoneRuntime& zone,
                              double currentPrice,
                              double currentHigh,
                              double currentLow,
                              double priorHigh,   // Prior bar high (for rotation)
                              double priorLow,    // Prior bar low (for rotation)
                              double currentVolume,
                              double currentDelta,
                              double tickSize,
                              int bar,
                              SCDateTime time,
                              const s_VolumeAtPriceV2* volumeProfile,
                              int numPrices,
                              const ZoneSessionState& ctx,
                              const ZoneConfig& cfg,
                              double vah,        // SSOT: from SessionManager.GetVAH()
                              double val,        // SSOT: from SessionManager.GetVAL()
                              int sessionStartBar,  // SSOT: from SessionManager.sessionStartBar
                              const VolumeThresholds* ssotThresholds = nullptr)
{
    // NOTE: UpdateZoneProximity is already called by ZoneManager::UpdateZones()
    // before this function. Calling it again here would corrupt priorProximity.
    // The proximity state is already current when we reach this point.

    // 1. Update bars since touch
    if (zone.lastTouchBar >= 0) {
        zone.barsSinceTouch = bar - zone.lastTouchBar;
    }

    // 2.5 Update boundary tracking (for failed auction detection)
    // Only relevant for VALUE_BOUNDARY zones (VAH/VAL)
    if (zone.role == ZoneRole::VALUE_BOUNDARY) {
        ValueAreaRegion currentRegion = CalculateVARegion(currentPrice, vah, val);

        bool isOutsideBoundary = false;
        if (zone.type == ZoneType::VPB_VAH) {
            isOutsideBoundary = (currentRegion == ValueAreaRegion::OUTSIDE_ABOVE);
        } else if (zone.type == ZoneType::VPB_VAL) {
            isOutsideBoundary = (currentRegion == ValueAreaRegion::OUTSIDE_BELOW);
        }

        bool isInsideVA = (currentRegion == ValueAreaRegion::UPPER_VA ||
                           currentRegion == ValueAreaRegion::CORE_VA ||
                           currentRegion == ValueAreaRegion::LOWER_VA);

        zone.UpdateBoundaryTracking(bar, time, isOutsideBoundary, isInsideVA);
    }

    // 3. Update volume profile data
    UpdateZoneVolume(zone, volumeProfile, numPrices, tickSize,
                    ctx.avgVolumePerTick);

    // 4. Classify volume characteristics
    // SSOT: If thresholds provided, use them for classification
    if (ssotThresholds != nullptr && ssotThresholds->valid) {
        zone.levelProfile.ClassifyFromThresholds(*ssotThresholds);
    }
    ClassifyVolumeCharacteristics(zone, cfg);

    // 5. If at zone, update engagement metrics
    if (zone.proximity == ZoneProximity::AT_ZONE) {
        UpdateEngagementMetrics(zone, currentPrice, currentVolume,
                               currentDelta, tickSize, bar, time);

        // Update rotation metrics (now uses proper prior bar OHLC)
        UpdateRotationMetrics(zone.currentEngagement.rotation,
                             currentHigh, currentLow,
                             priorHigh, priorLow);

        // Compute volumeRatio for acceptance classification
        // volumeRatio = average volume per bar during engagement / session average per bar
        // We compute the true session average dynamically from the volume profile.
        EngagementMetrics& eng = zone.currentEngagement;
        if (eng.barsEngaged > 0 && volumeProfile != nullptr && numPrices > 0) {
            // Sum total session volume from the profile
            double sessionTotalVolume = 0.0;
            for (int i = 0; i < numPrices; i++) {
                sessionTotalVolume += volumeProfile[i].Volume;
            }

            // Compute session bar count (current bar - session start + 1)
            // SSOT: sessionStartBar comes from SessionManager (not ZoneSessionState)
            int sessionBars = bar - sessionStartBar + 1;
            if (sessionBars <= 0) sessionBars = 1;  // Guard against division by zero

            // Compute true average volume per bar
            double trueAvgVolumePerBar = sessionTotalVolume / sessionBars;

            // Compute volumeRatio: engagement avg / session avg
            if (trueAvgVolumePerBar > 0.0) {
                double engagementAvgVolPerBar = eng.cumulativeVolume / eng.barsEngaged;
                eng.volumeRatio = engagementAvgVolPerBar / trueAvgVolumePerBar;
            }
        }

        // Update engagement flags - SSOT: Pass thresholds for sigma-based classification
        UpdateEngagementFlags(zone.currentEngagement, cfg, ssotThresholds);
    }

    // 6. Check for outcome changes
    if (zone.currentEngagement.outcome == AuctionOutcome::PENDING) {
        zone.currentEngagement.outcome = ClassifyEngagementOutcome(
            zone.currentEngagement, cfg);
    }

    // 7. Detect failed auction
    bool isFailedAuction = DetectFailedAuction(zone, currentPrice,
                                               tickSize, ctx, cfg);
    if (isFailedAuction) {
        zone.currentEngagement.wasFailedAuction = true;
    }

    // 8. Update strength score and tier
    zone.strengthScore = CalculateStrengthScore(zone, bar);
    zone.strengthTier = ClassifyStrength(zone.strengthScore, zone.touchCount);
}

// ============================================================================
// DYNAMIC ZONE WIDTH (Phase 1B)
// DOM-aware zone widths based on order book liquidity
// ============================================================================

/**
 * Update zone core and halo widths from DOM-derived liquidity.
 *
 * Phase 1B: Applies DOM-computed core ticks to AMT zones, running in parallel
 * with legacy ComputeLiquidityCoreTicks() to validate equivalence.
 *
 * @param zone The zone to update (typically POC only, matching legacy behavior)
 * @param coreTicksFromDOM Core width in ticks computed from DOM liquidity
 * @param haloMultiplier Multiplier to compute halo from core (e.g., 2.0)
 *
 * Invariants enforced:
 * - coreWidthTicks >= 2 (minimum core is 2 ticks)
 * - haloWidthTicks >= coreWidthTicks (halo at least as wide as core)
 * - std::round() uses half-away-from-zero semantics
 */
inline void UpdateZoneDynamicWidths(ZoneRuntime& zone,
                                    int coreTicksFromDOM,
                                    double haloMultiplier) {
    // Enforce invariants (parentheses prevent Windows min/max macro interference)
    int newCore = (std::max)(2, coreTicksFromDOM);
    int newHalo = (std::max)(newCore, static_cast<int>(std::round(newCore * haloMultiplier)));

    zone.coreWidthTicks = newCore;
    zone.haloWidthTicks = newHalo;
}

// ============================================================================
// SESSION INITIALIZATION
// ============================================================================

/**
 * Compute POC/VAH/VAL from volume profile
 * NOTE: POC/VAH/VAL are now stored in SessionManager (SSOT), not ZoneSessionState.
 * This function returns the computed values; caller must call SessionManager.UpdateLevels().
 */
inline void ComputeLevelsFromProfile(const s_VolumeAtPriceV2* volumeProfile,
                                     int numPrices,
                                     double tickSize,
                                     double& outPoc,
                                     double& outVah,
                                     double& outVal)
{
    outPoc = outVah = outVal = 0.0;
    if (!volumeProfile || numPrices == 0) return;

    // Find POC first
    outPoc = FindPOC(volumeProfile, numPrices, tickSize);

    // Find VAH and VAL
    outVah = FindVAH(volumeProfile, numPrices, tickSize, outPoc);
    outVal = FindVAL(volumeProfile, numPrices, tickSize, outPoc);

    // Validate: VAH must be greater than VAL
    if (outVah <= outVal || outVah <= 0.0 || outVal <= 0.0)
    {
        // Fallback: use price range from volume profile
        double minPrice = volumeProfile[0].PriceInTicks * tickSize;
        double maxPrice = volumeProfile[0].PriceInTicks * tickSize;
        for (int i = 1; i < numPrices; i++) {
            double price = volumeProfile[i].PriceInTicks * tickSize;
            if (price < minPrice) minPrice = price;
            if (price > maxPrice) maxPrice = price;
        }
        outVal = minPrice;
        outVah = maxPrice;
    }
}

/**
 * Initialize zone session state (volume metrics only - NOT levels or timing)
 * NOTE: POC/VAH/VAL are now in SessionManager. Use SessionManager.UpdateLevels() for those.
 * NOTE: sessionStartBar is now in SessionManager (SSOT), set at session transition.
 */
inline void InitializeZoneSessionState(ZoneSessionState& ctx,
                                    const s_VolumeAtPriceV2* volumeProfile,
                                    int numPrices,
                                    int bar)
{
    if (!volumeProfile || numPrices == 0) return;

    // Calculate total session volume (for display/analytics purposes)
    // NOTE: This is the TOTAL session volume at zone creation time, not average per bar.
    // For volumeRatio calculations, we compute the true average dynamically in
    // UpdateZoneComplete() using (totalVolume / sessionBars) for correctness.
    double totalVolume = 0.0;
    for (int i = 0; i < numPrices; i++) {
        totalVolume += volumeProfile[i].Volume;
    }

    // Store session total volume (renamed from historical "avgVolumePerBar" misnomer).
    // Consumers must divide by sessionBars to compute true per-bar average.
    ctx.sessionTotalVolume = totalVolume;

    // Calculate average volume per tick
    if (numPrices > 0) {
        ctx.avgVolumePerTick = totalVolume / numPrices;
    }

    // SSOT INVARIANT: Record this write to session context (Follow-through 1)
    // Single-writer enforcement - tracks all writes and asserts on duplicates
    ctx.RecordWrite(bar);
}

/**
 * Create zones from volume profile
 * NOTE: POC/VAH/VAL are now passed in from SessionManager (SSOT).
 * The caller MUST call SessionManager.UpdateLevels() BEFORE calling this function.
 *
 * @param poc POC from SessionManager.GetPOC()
 * @param vah VAH from SessionManager.GetVAH()
 * @param val VAL from SessionManager.GetVAL()
 */
inline void CreateZonesFromProfile(ZoneManager& zm,
                                  const s_VolumeAtPriceV2* volumeProfile,
                                  int numPrices,
                                  double tickSize,
                                  SCDateTime time,
                                  int bar,
                                  double poc,
                                  double vah,
                                  double val)
{
    if (!volumeProfile || numPrices == 0) return;

    // Initialize zone session state (volume metrics only - timing is in SessionManager)
    InitializeZoneSessionState(zm.sessionCtx, volumeProfile, numPrices, bar);

    // Helper lambda to find existing zone by type and anchor
    // SSOT FIX: Use tick-based comparison (not floating-point)
    auto findExistingZone = [&zm, tickSize](ZoneType type, double anchor) -> int {
        // Convert search anchor to ticks using canonical function
        const long long searchTicks = PriceToTicks(anchor, tickSize);
        for (const auto& [id, zone] : zm.activeZones) {
            if (zone.type == type && zone.GetAnchorTicks() == searchTicks) {
                return id;
            }
        }
        return -1;
    };

    // ========================================================================
    // VBP ZONES (current session profile)
    // ========================================================================
    if (g_zonePosture.enableVBP) {
        // Create POC zone (role/mechanism/source auto-derived from type)
        // SSOT FIX: If zone already exists at this anchor, preserve reference instead of -1
        if (poc > 0.0) {
            auto result = zm.CreateZone(ZoneType::VPB_POC, poc,
                                         time, bar, true /*isRTH*/);
            if (result.ok) {
                zm.pocId = result.zoneId;
            } else if (result.failure == ZoneCreationFailure::DUPLICATE_ANCHOR) {
                // Zone exists - find and preserve reference to existing zone
                zm.pocId = findExistingZone(ZoneType::VPB_POC, poc);
            } else {
                zm.pocId = -1;  // Other failure - explicitly invalid
            }
        }

        // Create VAH zone (role/mechanism/source auto-derived from type)
        if (vah > 0.0) {
            auto result = zm.CreateZone(ZoneType::VPB_VAH, vah,
                                         time, bar, true /*isRTH*/);
            if (result.ok) {
                zm.vahId = result.zoneId;
            } else if (result.failure == ZoneCreationFailure::DUPLICATE_ANCHOR) {
                zm.vahId = findExistingZone(ZoneType::VPB_VAH, vah);
            } else {
                zm.vahId = -1;
            }
        }

        // Create VAL zone (role/mechanism/source auto-derived from type)
        if (val > 0.0) {
            auto result = zm.CreateZone(ZoneType::VPB_VAL, val,
                                         time, bar, true /*isRTH*/);
            if (result.ok) {
                zm.valId = result.zoneId;
            } else if (result.failure == ZoneCreationFailure::DUPLICATE_ANCHOR) {
                zm.valId = findExistingZone(ZoneType::VPB_VAL, val);
            } else {
                zm.valId = -1;
            }
        }
    }

    // ========================================================================
    // PRIOR SESSION ZONES (Tri-State Contract)
    // ========================================================================
    // Created once per session from zm.sessionCtx.prior_* values
    //
    // Tri-State Contract:
    //   - PRIOR_MISSING: hasPriorProfile=false, insufficient history (not a bug)
    //   - PRIOR_VALID: hasPriorProfile=true AND prior differs from current
    //   - PRIOR_DUPLICATES_CURRENT: hasPriorProfile=true BUT all three match (defect)
    // ========================================================================
    if (!zm.sessionCtx.hasPriorProfile) {
        // PRIOR_MISSING: First session or insufficient history
        // This is NOT a bug - just degraded mode with no prior zones
        zm.sessionCtx.priorVBPState = PriorVBPState::PRIOR_MISSING;
        zm.priorPocId = zm.priorVahId = zm.priorValId = -1;
    }
    else if (g_zonePosture.enablePrior) {
        const double priorPoc = zm.sessionCtx.prior_poc;
        const double priorVah = zm.sessionCtx.prior_vah;
        const double priorVal = zm.sessionCtx.prior_val;

        // ====================================================================
        // DUPLICATE DETECTION: Check if PRIOR matches current VBP
        // ====================================================================
        const double halfTick = tickSize * 0.5;
        const bool pocMatch = (std::abs(priorPoc - poc) < halfTick);
        const bool vahMatch = (std::abs(priorVah - vah) < halfTick);
        const bool valMatch = (std::abs(priorVal - val) < halfTick);

        if (pocMatch && vahMatch && valMatch) {
            // PRIOR_DUPLICATES_CURRENT: All three match - this IS a defect
            // Prior should exist and differ; same values indicate capture bug
            zm.sessionCtx.priorVBPState = PriorVBPState::PRIOR_DUPLICATES_CURRENT;
            zm.priorPocId = zm.priorVahId = zm.priorValId = -1;
            // NOTE: Caller should log as BUG with diagnostic context
        } else {
            // PRIOR_VALID: Prior exists and differs from current
            zm.sessionCtx.priorVBPState = PriorVBPState::PRIOR_VALID;

            // Create PRIOR_POC zone
            if (priorPoc > 0.0) {
                auto result = zm.CreateZone(ZoneType::PRIOR_POC, priorPoc,
                                             time, bar, true /*isRTH*/);
                if (result.ok) {
                    zm.priorPocId = result.zoneId;
                } else if (result.failure == ZoneCreationFailure::DUPLICATE_ANCHOR) {
                    zm.priorPocId = findExistingZone(ZoneType::PRIOR_POC, priorPoc);
                } else {
                    zm.priorPocId = -1;
                }
            }

            // Create PRIOR_VAH zone
            if (priorVah > 0.0) {
                auto result = zm.CreateZone(ZoneType::PRIOR_VAH, priorVah,
                                             time, bar, true /*isRTH*/);
                if (result.ok) {
                    zm.priorVahId = result.zoneId;
                } else if (result.failure == ZoneCreationFailure::DUPLICATE_ANCHOR) {
                    zm.priorVahId = findExistingZone(ZoneType::PRIOR_VAH, priorVah);
                } else {
                    zm.priorVahId = -1;
                }
            }

            // Create PRIOR_VAL zone
            if (priorVal > 0.0) {
                auto result = zm.CreateZone(ZoneType::PRIOR_VAL, priorVal,
                                             time, bar, true /*isRTH*/);
                if (result.ok) {
                    zm.priorValId = result.zoneId;
                } else if (result.failure == ZoneCreationFailure::DUPLICATE_ANCHOR) {
                    zm.priorValId = findExistingZone(ZoneType::PRIOR_VAL, priorVal);
                } else {
                    zm.priorValId = -1;
                }
            }
        }
    }

    // ========================================================================
    // TPO ZONES - DISABLED BY POSTURE
    // ========================================================================
    // NOTE: g_zonePosture.enableTPO is false by default.
    // No TPO zones are created. This is intentional.

    // ========================================================================
    // Update zone volume characteristics for VBP zones
    // ========================================================================
    ZoneRuntime* pocZone = zm.GetPOC();
    ZoneRuntime* vahZone = zm.GetVAH();
    ZoneRuntime* valZone = zm.GetVAL();

    if (pocZone) {
        UpdateZoneVolume(*pocZone, volumeProfile, numPrices, tickSize,
                        zm.sessionCtx.avgVolumePerTick);
    }
    if (vahZone) {
        UpdateZoneVolume(*vahZone, volumeProfile, numPrices, tickSize,
                        zm.sessionCtx.avgVolumePerTick);
    }
    if (valZone) {
        UpdateZoneVolume(*valZone, volumeProfile, numPrices, tickSize,
                        zm.sessionCtx.avgVolumePerTick);
    }

    // PRIOR zones don't get volume characteristics updated (they're historical)
}

} // namespace AMT

#endif // AMT_UPDATES_H
