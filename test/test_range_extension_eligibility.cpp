// test_range_extension_eligibility.cpp
// Diagnostic test: Why RANGE_EXTENSION never fires?
// This test verifies all three gates and identifies which is blocking.

#include <cstdio>
#include <cassert>
#include <cstring>
#include <vector>
#include <string>

// Minimal type stubs for standalone testing
enum class AMTMarketState { UNKNOWN = 0, BALANCE, IMBALANCE };
enum class AMTActivityType { NEUTRAL = 0, INITIATIVE, RESPONSIVE };
// Use 9-state ValueZone (matches production code)
enum class ValueZone {
    UNKNOWN = -1,
    FAR_BELOW_VALUE = 0, NEAR_BELOW_VALUE, AT_VAL, LOWER_VALUE,
    AT_POC, UPPER_VALUE, AT_VAH, NEAR_ABOVE_VALUE, FAR_ABOVE_VALUE
};
enum class RangeExtensionType { NONE = 0, BUYING, SELLING, BOTH };
enum class CurrentPhase {
    UNKNOWN = 0,
    ROTATION = 1,
    TESTING_BOUNDARY = 2,
    DRIVING_UP = 3,
    DRIVING_DOWN = 4,
    RANGE_EXTENSION = 5,
    PULLBACK = 6,
    FAILED_AUCTION = 7
};

// Helper functions for 9-state ValueZone
bool IsAtBoundary(ValueZone z) {
    return z == ValueZone::AT_VAH || z == ValueZone::AT_VAL;
}

// Phase derivation logic (from DaltonState.DeriveCurrentPhase)
CurrentPhase DeriveCurrentPhase(
    AMTMarketState phase,
    ValueZone zone,
    AMTActivityType activity,
    RangeExtensionType extension
) {
    // BALANCE state
    if (phase == AMTMarketState::BALANCE) {
        if (IsAtBoundary(zone)) {
            return CurrentPhase::TESTING_BOUNDARY;
        }
        return CurrentPhase::ROTATION;
    }

    // IMBALANCE state
    if (phase == AMTMarketState::IMBALANCE) {
        // At boundary with responsive = rejection
        if (IsAtBoundary(zone) && activity == AMTActivityType::RESPONSIVE) {
            return CurrentPhase::FAILED_AUCTION;
        }

        // RANGE_EXTENSION: IB broken + INITIATIVE
        if (extension != RangeExtensionType::NONE &&
            activity == AMTActivityType::INITIATIVE) {
            return CurrentPhase::RANGE_EXTENSION;
        }

        // Responsive = pullback
        if (activity == AMTActivityType::RESPONSIVE) {
            return CurrentPhase::PULLBACK;
        }

        // Default directional
        return CurrentPhase::DRIVING_UP;  // or DOWN based on 1TF direction
    }

    return CurrentPhase::UNKNOWN;
}

const char* PhaseToString(CurrentPhase p) {
    switch (p) {
        case CurrentPhase::ROTATION: return "ROTATION";
        case CurrentPhase::TESTING_BOUNDARY: return "TESTING_BOUNDARY";
        case CurrentPhase::DRIVING_UP: return "DRIVING_UP";
        case CurrentPhase::DRIVING_DOWN: return "DRIVING_DOWN";
        case CurrentPhase::RANGE_EXTENSION: return "RANGE_EXTENSION";
        case CurrentPhase::PULLBACK: return "PULLBACK";
        case CurrentPhase::FAILED_AUCTION: return "FAILED_AUCTION";
        default: return "UNKNOWN";
    }
}

const char* StateToString(AMTMarketState s) {
    switch (s) {
        case AMTMarketState::BALANCE: return "BALANCE";
        case AMTMarketState::IMBALANCE: return "IMBALANCE";
        default: return "UNKNOWN";
    }
}

const char* ActivityToString(AMTActivityType a) {
    switch (a) {
        case AMTActivityType::NEUTRAL: return "NEUTRAL";
        case AMTActivityType::INITIATIVE: return "INITIATIVE";
        case AMTActivityType::RESPONSIVE: return "RESPONSIVE";
    }
    return "?";
}

const char* ExtToString(RangeExtensionType e) {
    switch (e) {
        case RangeExtensionType::NONE: return "NONE";
        case RangeExtensionType::BUYING: return "BUYING";
        case RangeExtensionType::SELLING: return "SELLING";
        case RangeExtensionType::BOTH: return "BOTH";
    }
    return "?";
}

// Test scenario
struct Scenario {
    const char* name;
    AMTMarketState state;
    RangeExtensionType extension;
    AMTActivityType activity;
    ValueZone zone;
    CurrentPhase expected;
};

int main() {
    printf("=== RANGE_EXTENSION Eligibility Diagnostic ===\n\n");

    // Define test scenarios that should/shouldn't produce RANGE_EXTENSION
    std::vector<Scenario> scenarios = {
        // SHOULD produce RANGE_EXTENSION
        {"IMBALANCE + IB_BREAK + INITIATIVE = RANGE_EXTENSION",
         AMTMarketState::IMBALANCE, RangeExtensionType::BUYING,
         AMTActivityType::INITIATIVE, ValueZone::NEAR_ABOVE_VALUE,
         CurrentPhase::RANGE_EXTENSION},

        {"IMBALANCE + SELLING + INITIATIVE = RANGE_EXTENSION",
         AMTMarketState::IMBALANCE, RangeExtensionType::SELLING,
         AMTActivityType::INITIATIVE, ValueZone::NEAR_BELOW_VALUE,
         CurrentPhase::RANGE_EXTENSION},

        // SHOULD NOT produce RANGE_EXTENSION (gate failures)
        {"GATE 1 FAIL: BALANCE + IB_BREAK + INITIATIVE = ROTATION (not EXT)",
         AMTMarketState::BALANCE, RangeExtensionType::BUYING,
         AMTActivityType::INITIATIVE, ValueZone::NEAR_ABOVE_VALUE,
         CurrentPhase::ROTATION},

        {"GATE 2 FAIL: IMBALANCE + NO_BREAK + INITIATIVE = DRIVING (not EXT)",
         AMTMarketState::IMBALANCE, RangeExtensionType::NONE,
         AMTActivityType::INITIATIVE, ValueZone::NEAR_ABOVE_VALUE,
         CurrentPhase::DRIVING_UP},

        {"GATE 3 FAIL: IMBALANCE + IB_BREAK + RESPONSIVE = PULLBACK (not EXT)",
         AMTMarketState::IMBALANCE, RangeExtensionType::BUYING,
         AMTActivityType::RESPONSIVE, ValueZone::NEAR_ABOVE_VALUE,
         CurrentPhase::PULLBACK},

        {"ALL GATES FAIL: BALANCE + NO_BREAK + NEUTRAL = ROTATION",
         AMTMarketState::BALANCE, RangeExtensionType::NONE,
         AMTActivityType::NEUTRAL, ValueZone::UPPER_VALUE,
         CurrentPhase::ROTATION},
    };

    int passed = 0;
    int failed = 0;

    printf("Gate Analysis for RANGE_EXTENSION:\n");
    printf("  Gate 1: state == IMBALANCE (1TF detected)\n");
    printf("  Gate 2: extension != NONE (IB broken)\n");
    printf("  Gate 3: activity == INITIATIVE (delta aligned with price)\n\n");

    for (const auto& s : scenarios) {
        CurrentPhase result = DeriveCurrentPhase(s.state, s.zone, s.activity, s.extension);

        bool gate1 = (s.state == AMTMarketState::IMBALANCE);
        bool gate2 = (s.extension != RangeExtensionType::NONE);
        bool gate3 = (s.activity == AMTActivityType::INITIATIVE);

        bool pass = (result == s.expected);
        printf("%s %s\n", pass ? "[PASS]" : "[FAIL]", s.name);
        printf("       state=%s ext=%s act=%s\n",
               StateToString(s.state), ExtToString(s.extension), ActivityToString(s.activity));
        printf("       Gates: [1=%s 2=%s 3=%s] -> Result: %s (expected: %s)\n",
               gate1 ? "PASS" : "FAIL",
               gate2 ? "PASS" : "FAIL",
               gate3 ? "PASS" : "FAIL",
               PhaseToString(result),
               PhaseToString(s.expected));
        printf("\n");

        if (pass) passed++; else failed++;
    }

    printf("=== RANGE_EXTENSION Eligibility Summary ===\n");
    printf("Results: %d passed, %d failed\n\n", passed, failed);

    // Diagnostic for live data interpretation
    printf("=== How to Diagnose EXT=0%% in Live Data ===\n");
    printf("\n1. Enable diagLevel >= 2 in study inputs\n");
    printf("2. Look for log lines: 'DALTON: TF=... phase=... act=... ext=...'\n");
    printf("3. Check which gate is failing:\n\n");

    printf("   If ext=NONE all session:\n");
    printf("      -> IB never broken (price stayed within IB range)\n");
    printf("      -> Check IB: values in log - is session high > ibHigh?\n\n");

    printf("   If ext=BUYING/SELLING but act=RESPONSIVE:\n");
    printf("      -> IB broken but activity isn't aligned\n");
    printf("      -> Delta opposes price direction (absorption, not attack)\n");
    printf("      -> This is actually PULLBACK phase\n\n");

    printf("   If ext=BUYING/SELLING and act=INITIATIVE but phase=BALANCE:\n");
    printf("      -> 2TF pattern detected (rotational market)\n");
    printf("      -> RANGE_EXTENSION requires IMBALANCE state\n");
    printf("      -> Check if market is actually trending (1TF)\n\n");

    printf("=== TESTING_BOUNDARY at 55.6%% ===\n");
    printf("\n If TEST=55.6%% is suspiciously high, check:\n");
    printf("   - vaBoundaryTicks config (default: 2 ticks = 0.50 points for ES)\n");
    printf("   - Narrow Value Area = more time 'at boundary'\n");
    printf("   - Consider increasing tolerance if VA is tight\n");

    return failed;
}
