// test_boundary_masking.cpp
// Diagnostic: Does vaBoundaryTicks cause excessive TESTING_BOUNDARY?
// Tests whether narrow VA + 2-tick tolerance masks RANGE_EXTENSION opportunities.

#include <cstdio>
#include <cmath>
#include <vector>

// Minimal type stubs
enum class AMTMarketState { UNKNOWN = 0, BALANCE, IMBALANCE };
enum class AMTActivityType { NEUTRAL = 0, INITIATIVE, RESPONSIVE };
enum class ValueLocation { INSIDE_VALUE = 0, AT_POC, AT_VAH, AT_VAL, ABOVE_VALUE, BELOW_VALUE };
enum class RangeExtensionType { NONE = 0, BUYING, SELLING, BOTH };
enum class CurrentPhase {
    UNKNOWN = 0, ROTATION = 1, TESTING_BOUNDARY = 2, DRIVING_UP = 3,
    DRIVING_DOWN = 4, RANGE_EXTENSION = 5, PULLBACK = 6, FAILED_AUCTION = 7
};

// Location classification (from AMT_Signals.h)
ValueLocation DetermineLocation(
    double price, double poc, double vah, double val,
    double tickSize, int pocToleranceTicks, int vaBoundaryTicks
) {
    const double distFromPOC = std::abs(price - poc) / tickSize;
    const double distFromVAH = (price - vah) / tickSize;
    const double distFromVAL = (price - val) / tickSize;

    if (distFromPOC <= pocToleranceTicks) return ValueLocation::AT_POC;
    if (std::abs(distFromVAH) <= vaBoundaryTicks) return ValueLocation::AT_VAH;
    if (std::abs(distFromVAL) <= vaBoundaryTicks) return ValueLocation::AT_VAL;
    if (price > vah) return ValueLocation::ABOVE_VALUE;
    if (price < val) return ValueLocation::BELOW_VALUE;
    return ValueLocation::INSIDE_VALUE;
}

// Phase derivation (simplified)
CurrentPhase DerivePhase(
    AMTMarketState state, ValueLocation location,
    AMTActivityType activity, RangeExtensionType extension
) {
    if (state == AMTMarketState::BALANCE) {
        if (location == ValueLocation::AT_VAH || location == ValueLocation::AT_VAL)
            return CurrentPhase::TESTING_BOUNDARY;
        return CurrentPhase::ROTATION;
    }
    if (state == AMTMarketState::IMBALANCE) {
        if ((location == ValueLocation::AT_VAH || location == ValueLocation::AT_VAL) &&
            activity == AMTActivityType::RESPONSIVE)
            return CurrentPhase::FAILED_AUCTION;
        if (extension != RangeExtensionType::NONE &&
            activity == AMTActivityType::INITIATIVE)
            return CurrentPhase::RANGE_EXTENSION;
        if (activity == AMTActivityType::RESPONSIVE)
            return CurrentPhase::PULLBACK;
        return CurrentPhase::DRIVING_UP;
    }
    return CurrentPhase::UNKNOWN;
}

const char* LocationToString(ValueLocation l) {
    switch (l) {
        case ValueLocation::INSIDE_VALUE: return "INSIDE";
        case ValueLocation::AT_POC: return "AT_POC";
        case ValueLocation::AT_VAH: return "AT_VAH";
        case ValueLocation::AT_VAL: return "AT_VAL";
        case ValueLocation::ABOVE_VALUE: return "ABOVE";
        case ValueLocation::BELOW_VALUE: return "BELOW";
    }
    return "?";
}

const char* PhaseToString(CurrentPhase p) {
    switch (p) {
        case CurrentPhase::ROTATION: return "ROTATION";
        case CurrentPhase::TESTING_BOUNDARY: return "TEST_BOUND";
        case CurrentPhase::DRIVING_UP: return "DRIVING";
        case CurrentPhase::RANGE_EXTENSION: return "RANGE_EXT";
        case CurrentPhase::PULLBACK: return "PULLBACK";
        case CurrentPhase::FAILED_AUCTION: return "FAILED";
        default: return "UNKNOWN";
    }
}

int main() {
    printf("=== TESTING_BOUNDARY Masking Analysis ===\n\n");

    // ES tick size
    const double tickSize = 0.25;
    const int pocTolerance = 2;

    // Simulate a narrow Value Area (common in low-vol sessions)
    // VAH = 6100.00, VAL = 6095.00, POC = 6097.50
    // VA width = 20 ticks (5 points)
    const double vah = 6100.00;
    const double val = 6095.00;
    const double poc = 6097.50;
    const double ibHigh = 6098.00;  // IB within VA

    printf("Value Area: VAH=%.2f VAL=%.2f POC=%.2f (width=%.0f ticks)\n",
           vah, val, poc, (vah - val) / tickSize);
    printf("IB High: %.2f (inside VA)\n\n", ibHigh);

    // Test different vaBoundaryTicks values
    std::vector<int> tolerances = {1, 2, 3, 4};

    for (int vaBoundaryTicks : tolerances) {
        printf("=== vaBoundaryTicks = %d (%.2f points) ===\n",
               vaBoundaryTicks, vaBoundaryTicks * tickSize);

        // Simulate price moving from VAH through breakout
        // Count how many ticks are classified as each location
        int atBoundary = 0;
        int aboveValue = 0;
        int insideValue = 0;

        printf("\nPrice walk from VAH-4 to VAH+8 ticks:\n");
        printf("Price     | Location    | State=BAL Phase | State=IMB+EXT Phase\n");
        printf("----------|-------------|-----------------|--------------------\n");

        for (int offset = -4; offset <= 8; offset++) {
            double price = vah + (offset * tickSize);
            ValueLocation loc = DetermineLocation(price, poc, vah, val, tickSize,
                                                   pocTolerance, vaBoundaryTicks);

            // Determine if IB is broken at this price
            RangeExtensionType ext = (price > ibHigh) ?
                RangeExtensionType::BUYING : RangeExtensionType::NONE;

            // Phase in BALANCE state
            CurrentPhase phaseBalance = DerivePhase(
                AMTMarketState::BALANCE, loc,
                AMTActivityType::INITIATIVE, ext);

            // Phase in IMBALANCE state with extension
            CurrentPhase phaseImbalance = DerivePhase(
                AMTMarketState::IMBALANCE, loc,
                AMTActivityType::INITIATIVE, ext);

            printf("%.2f   | %-11s | %-15s | %-18s %s\n",
                   price, LocationToString(loc),
                   PhaseToString(phaseBalance),
                   PhaseToString(phaseImbalance),
                   (phaseImbalance == CurrentPhase::RANGE_EXTENSION) ? "<-- EXT!" : "");

            if (loc == ValueLocation::AT_VAH || loc == ValueLocation::AT_VAL)
                atBoundary++;
            else if (loc == ValueLocation::ABOVE_VALUE || loc == ValueLocation::BELOW_VALUE)
                aboveValue++;
            else
                insideValue++;
        }

        // Calculate percentage
        int total = 13;  // -4 to +8 = 13 prices
        printf("\nDistribution: AT_BOUNDARY=%d/%d (%.0f%%) OUTSIDE=%d/%d (%.0f%%)\n",
               atBoundary, total, 100.0 * atBoundary / total,
               aboveValue, total, 100.0 * aboveValue / total);

        // Key insight: how many ticks above VAH before ABOVE_VALUE?
        int ticksUntilAbove = vaBoundaryTicks + 1;
        printf("Ticks above VAH before ABOVE_VALUE: %d (%.2f points)\n",
               ticksUntilAbove, ticksUntilAbove * tickSize);

        printf("\n");
    }

    // Summary
    printf("=== KEY FINDING ===\n\n");
    printf("With vaBoundaryTicks=2 (default), price must be >2 ticks above VAH\n");
    printf("to be classified as ABOVE_VALUE. This means:\n\n");
    printf("  - Price at VAH+0.25 (1 tick above): AT_VAH -> TESTING_BOUNDARY\n");
    printf("  - Price at VAH+0.50 (2 ticks above): AT_VAH -> TESTING_BOUNDARY\n");
    printf("  - Price at VAH+0.75 (3 ticks above): ABOVE_VALUE -> can be RANGE_EXT\n\n");

    printf("If IB High is at or near VAH, the breakout zone is masked by\n");
    printf("TESTING_BOUNDARY classification for the first 2 ticks of extension.\n\n");

    printf("=== RECOMMENDATIONS ===\n\n");
    printf("1. If TESTING_BOUNDARY is excessive (>30%%), consider:\n");
    printf("   - Reduce vaBoundaryTicks to 1 (tighter boundary)\n");
    printf("   - Or: Accept that narrow VA sessions have more boundary testing\n\n");

    printf("2. RANGE_EXTENSION still fires if:\n");
    printf("   - state=IMBALANCE (1TF pattern)\n");
    printf("   - extension!=NONE (IB broken)\n");
    printf("   - activity=INITIATIVE (delta aligned)\n");
    printf("   - location=ABOVE_VALUE (outside the boundary tolerance)\n\n");

    printf("3. The 55.6%% TESTING_BOUNDARY in your log suggests:\n");
    printf("   - Narrow Value Area, OR\n");
    printf("   - Price oscillating near VAH/VAL, OR\n");
    printf("   - state=BALANCE most of the session (expected in 2TF)\n");

    return 0;
}
