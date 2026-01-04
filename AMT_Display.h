// ============================================================================
// AMT_Display.h
// Drawing and visualization helpers for Sierra Chart
// Week 4: Beautiful zone visualization
// ============================================================================

#ifndef AMT_DISPLAY_H
#define AMT_DISPLAY_H

#include "sierrachart.h"
#include "amt_core.h"
#include "AMT_Zones.h"
#include <string>

namespace AMT {

    // ============================================================================
    // UTILITY FUNCTIONS
    // ============================================================================

    /**
     * Get zone health assessment
     */
    inline std::string GetZoneHealth(const ZoneRuntime& zone) {
        if (zone.strengthTier == ZoneStrength::VIRGIN) return "PRISTINE";
        if (zone.strengthTier == ZoneStrength::STRONG) return "HEALTHY";
        if (zone.strengthTier == ZoneStrength::MODERATE) return "WORN";
        if (zone.strengthTier == ZoneStrength::WEAK) return "FRAGILE";
        if (zone.strengthTier == ZoneStrength::EXPIRED) return "DEAD";
        return "UNKNOWN";
    }

    /**
     * Get detailed zone description
     */
    inline std::string GetZoneDescription(const ZoneRuntime& zone,
        double currentPrice,
        double tickSize)
    {
        std::string desc;

        // Basic info
        desc += ZoneTypeToString(zone.type);
        desc += " @ " + std::to_string(zone.GetAnchorPrice());

        // Distance
        double distTicks = GetExactTickDistance(currentPrice, zone.GetAnchorPrice(), tickSize);
        desc += " (" + std::to_string(static_cast<int>(distTicks)) + " ticks away)";

        // Proximity
        desc += std::string(" | ") + ZoneProximityToString(zone.proximity);

        // Health
        desc += " | " + GetZoneHealth(zone);

        // Activity
        desc += " | Touches: " + std::to_string(zone.touchCount);

        // Outcome
        if (zone.outcome != AuctionOutcome::PENDING) {
            desc += " | Last: " + AuctionOutcomeToString(zone.outcome);
        }

        return desc;
    }

    // ============================================================================
    // COLOR SCHEMES
    // ============================================================================

    /**
     * Get color for zone type
     */
    inline COLORREF GetZoneColor(ZoneType type) {
        switch (type) {
        case ZoneType::VPB_POC:
        case ZoneType::PRIOR_POC:
            return RGB(0, 255, 0);  // Green (center)

        case ZoneType::VPB_VAH:
        case ZoneType::PRIOR_VAH:
            return RGB(255, 0, 0);  // Red (upper boundary)

        case ZoneType::VPB_VAL:
        case ZoneType::PRIOR_VAL:
            return RGB(0, 0, 255);  // Blue (lower boundary)

        case ZoneType::IB_HIGH:
            return RGB(255, 128, 0);  // Orange

        case ZoneType::IB_LOW:
            return RGB(0, 128, 255);  // Light blue

        case ZoneType::VWAP:
            return RGB(255, 255, 0);  // Yellow

        default:
            return RGB(128, 128, 128);  // Gray
        }
    }

    /**
     * Get color for zone strength
     */
    inline COLORREF GetStrengthColor(ZoneStrength strength) {
        switch (strength) {
        case ZoneStrength::VIRGIN:
            return RGB(0, 255, 0);  // Bright green
        case ZoneStrength::STRONG:
            return RGB(100, 255, 100);  // Light green
        case ZoneStrength::MODERATE:
            return RGB(255, 255, 0);  // Yellow
        case ZoneStrength::WEAK:
            return RGB(255, 128, 0);  // Orange
        case ZoneStrength::EXPIRED:
            return RGB(255, 0, 0);  // Red
        default:
            return RGB(128, 128, 128);  // Gray
        }
    }

    /**
     * Get color for proximity state
     * All 4 states explicitly handled - no silent default
     */
    inline COLORREF GetProximityColor(ZoneProximity proximity) {
        switch (proximity) {
        case ZoneProximity::AT_ZONE:
            return RGB(255, 0, 0);    // Red (active engagement)
        case ZoneProximity::APPROACHING:
            return RGB(255, 255, 0);  // Yellow (warning)
        case ZoneProximity::DEPARTED:
            return RGB(255, 165, 0);  // Orange (recently left, cooling off)
        case ZoneProximity::INACTIVE:
            return RGB(100, 100, 100);  // Gray (dormant)
        // No default: compiler warns on missing enum values
        }
        assert(false && "GetProximityColor: unhandled ZoneProximity value");
        return RGB(128, 128, 128);
    }

    /**
     * Get color for phase
     */
    inline COLORREF GetPhaseColor(CurrentPhase phase) {
        switch (phase) {
        case CurrentPhase::ROTATION:
            return RGB(100, 100, 255);  // Blue (balanced)
        case CurrentPhase::TESTING_BOUNDARY:
            return RGB(255, 255, 0);  // Yellow (caution)
        case CurrentPhase::RANGE_EXTENSION:
            return RGB(0, 255, 0);  // Green (trending)
        case CurrentPhase::PULLBACK:
            return RGB(255, 128, 0);  // Orange (retracement)
        case CurrentPhase::FAILED_AUCTION:
            return RGB(255, 0, 0);  // Red (reversal)
        default:
            return RGB(128, 128, 128);
        }
    }

    // ============================================================================
    // DRAWING FUNCTIONS
    // ============================================================================

    /**
     * Draw zone line with strength-based styling
     */
    inline void DrawZoneLine(SCStudyInterfaceRef sc,
        SCSubgraphRef subgraph,
        const ZoneRuntime& zone,
        int barIndex)
    {
        subgraph[barIndex] = static_cast<float>(zone.GetAnchorPrice());

        // Color by strength
        subgraph.DataColor[barIndex] = GetStrengthColor(zone.strengthTier);

        // Line width by role
        int lineWidth = 1;
        switch (zone.role) {
        case ZoneRole::VALUE_BOUNDARY: lineWidth = 2; break;
        case ZoneRole::VALUE_CORE: lineWidth = 3; break;
        case ZoneRole::RANGE_BOUNDARY: lineWidth = 1; break;
        case ZoneRole::MEAN_REFERENCE: lineWidth = 1; break;
        }
        subgraph.LineWidth = lineWidth;
    }

    /**
     * Draw zone core/halo bands
     */
    inline void DrawZoneBands(SCStudyInterfaceRef sc,
        SCSubgraphRef coreUpper,
        SCSubgraphRef coreLower,
        SCSubgraphRef haloUpper,
        SCSubgraphRef haloLower,
        const ZoneRuntime& zone,
        double tickSize,
        int barIndex)
    {
        // SSOT: Use tick arithmetic for band calculation
        // All positions derived from anchorTicks + widthTicks, converted to price at edge
        const long long anchorTicks = zone.GetAnchorTicks();
        const long long coreTicks = static_cast<long long>(zone.coreWidthTicks);
        const long long haloTicks = static_cast<long long>(zone.haloWidthTicks);

        // Core bands (anchor ± coreWidthTicks)
        coreUpper[barIndex] = static_cast<float>(TicksToPrice(anchorTicks + coreTicks, tickSize));
        coreLower[barIndex] = static_cast<float>(TicksToPrice(anchorTicks - coreTicks, tickSize));

        // Halo bands (anchor ± haloWidthTicks)
        haloUpper[barIndex] = static_cast<float>(TicksToPrice(anchorTicks + haloTicks, tickSize));
        haloLower[barIndex] = static_cast<float>(TicksToPrice(anchorTicks - haloTicks, tickSize));

        // Style bands
        COLORREF bandColor = GetZoneColor(zone.type);
        coreUpper.DataColor[barIndex] = bandColor;
        coreLower.DataColor[barIndex] = bandColor;
        haloUpper.DataColor[barIndex] = bandColor;
        haloLower.DataColor[barIndex] = bandColor;
    }

    /**
     * Draw proximity indicator bars
     */
    inline void DrawProximityBars(SCStudyInterfaceRef sc,
        SCSubgraphRef subgraph,
        const ZoneRuntime& zone,
        int barIndex)
    {
        float height = 0.0f;

        switch (zone.proximity) {
        case ZoneProximity::AT_ZONE: height = 3.0f; break;
        case ZoneProximity::APPROACHING: height = 2.0f; break;
        case ZoneProximity::DEPARTED: height = 1.5f; break;  // Recently exited, awaiting resolution
        case ZoneProximity::INACTIVE: height = 1.0f; break;
        // No default: compiler warns on missing enum values
        }

        subgraph[barIndex] = height;
        subgraph.DataColor[barIndex] = GetProximityColor(zone.proximity);
    }

    /**
     * Draw phase background
     */
    inline void DrawPhaseBackground(SCStudyInterfaceRef sc,
        SCSubgraphRef subgraph,
        CurrentPhase phase,
        int barIndex)
    {
        // Map phase to integer for bar height
        float phaseValue = static_cast<float>(static_cast<int>(phase));

        subgraph[barIndex] = phaseValue;
        subgraph.DataColor[barIndex] = GetPhaseColor(phase);
    }

    /**
     * Draw touch markers
     */
    inline void DrawTouchMarkers(SCStudyInterfaceRef sc,
        SCSubgraphRef subgraph,
        const ZoneRuntime& zone,
        int barIndex,
        bool wasTouch)
    {
        if (wasTouch) {
            subgraph[barIndex] = static_cast<float>(zone.GetAnchorPrice());

            // Color by touch type (would need last touch type)
            subgraph.DataColor[barIndex] = RGB(255, 255, 0);  // Yellow marker
        }
    }

    /**
     * Add text drawing for zone labels
     */
    inline void AddZoneLabel(SCStudyInterfaceRef sc,
        const ZoneRuntime& zone,
        double currentPrice,
        double tickSize,
        int barIndex)
    {
        s_UseTool tool;
        tool.Clear();

        tool.ChartNumber = sc.ChartNumber;
        tool.DrawingType = DRAWING_TEXT;
        tool.BeginIndex = barIndex;
        tool.BeginValue = static_cast<float>(zone.GetAnchorPrice());
        tool.UseRelativeVerticalValues = 0;

        // Build label text
        std::string label = ZoneTypeToString(zone.type);
        label += " " + GetZoneHealth(zone);
        label += " (" + std::to_string(zone.touchCount) + ")";

        tool.Text = label.c_str();
        tool.Color = GetZoneColor(zone.type);
        tool.FontSize = 8;
        tool.FontBold = (zone.proximity == ZoneProximity::AT_ZONE);

        tool.AddMethod = UTAM_ADD_OR_ADJUST;
        tool.LineNumber = zone.zoneId;  // Unique ID for updates

        sc.UseTool(tool);
    }

} // namespace AMT

#endif // AMT_DISPLAY_H
