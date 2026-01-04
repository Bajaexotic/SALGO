// ============================================================================
// test_single_prints.cpp
// Unit tests for Single Print Detection (AMT_Signals.h + AMT_VolumeProfile.h)
// ============================================================================

#include <iostream>
#include <cmath>
#include <vector>
#include <map>

// Mock Sierra Chart types
#include "test_sierrachart_mock.h"

// Include headers under test
#include "amt_core.h"
#include "AMT_Signals.h"

using namespace AMT;

// Test utilities
static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "[FAIL] " << msg << " (line " << __LINE__ << ")\n"; \
        g_testsFailed++; \
    } else { \
        std::cout << "[PASS] " << msg << "\n"; \
        g_testsPassed++; \
    } \
} while(0)

#define TEST_SECTION(name) std::cout << "\n=== " << name << " ===\n"

// ============================================================================
// MOCK VOLUME PROFILE HELPERS
// ============================================================================

// Create a volume profile with a single print zone
std::vector<double> CreateProfileWithSinglePrint(
    int numLevels,
    double avgVolume,
    int spStartIdx,    // Single print start index
    int spWidthTicks,  // Single print width in ticks
    double thinRatio = 0.05  // Thin volume ratio
) {
    std::vector<double> profile(numLevels, avgVolume);

    // Create thin zone (single print)
    for (int i = spStartIdx; i < spStartIdx + spWidthTicks && i < numLevels; ++i) {
        profile[i] = avgVolume * thinRatio;
    }

    return profile;
}

// Create a profile with tail at high (thin volume at top)
std::vector<double> CreateProfileWithTailAtHigh(
    int numLevels,
    double avgVolume,
    int tailTicks,
    double thinRatio = 0.05
) {
    std::vector<double> profile(numLevels, avgVolume);

    // Create thin tail at high (top of profile)
    for (int i = numLevels - tailTicks; i < numLevels; ++i) {
        profile[i] = avgVolume * thinRatio;
    }

    return profile;
}

// Create a profile with tail at low (thin volume at bottom)
std::vector<double> CreateProfileWithTailAtLow(
    int numLevels,
    double avgVolume,
    int tailTicks,
    double thinRatio = 0.05
) {
    std::vector<double> profile(numLevels, avgVolume);

    // Create thin tail at low (bottom of profile)
    for (int i = 0; i < tailTicks && i < numLevels; ++i) {
        profile[i] = avgVolume * thinRatio;
    }

    return profile;
}

// ============================================================================
// SINGLE PRINT DETECTOR TESTS
// ============================================================================

void test_single_print_basic_detection() {
    TEST_SECTION("SinglePrintDetector - Basic Detection");

    SinglePrintDetector detector;
    const double tickSize = 0.25;
    const double priceStart = 6090.00;
    const int numLevels = 100;
    const double avgVolume = 1000.0;

    // Create profile with single print zone at indices 40-47 (8 ticks)
    auto profile = CreateProfileWithSinglePrint(numLevels, avgVolume, 40, 8);

    auto zones = detector.DetectFromProfile(
        profile.data(), priceStart, tickSize, numLevels, avgVolume, 100);

    TEST_ASSERT(zones.size() == 1, "Should detect exactly one single print zone");

    if (!zones.empty()) {
        TEST_ASSERT(zones[0].widthTicks == 8, "Single print should be 8 ticks wide");
        TEST_ASSERT(zones[0].valid, "Zone should be valid");

        // Check prices
        double expectedLow = priceStart + 40 * tickSize;
        double expectedHigh = priceStart + 47 * tickSize;

        TEST_ASSERT(std::abs(zones[0].lowPrice - expectedLow) < 0.01,
            "Low price should match");
        TEST_ASSERT(std::abs(zones[0].highPrice - expectedHigh) < 0.01,
            "High price should match");
    }
}

void test_single_print_multiple_zones() {
    TEST_SECTION("SinglePrintDetector - Multiple Zones");

    SinglePrintDetector detector;
    const double tickSize = 0.25;
    const double priceStart = 6090.00;
    const int numLevels = 100;
    const double avgVolume = 1000.0;

    // Create profile with two single print zones
    std::vector<double> profile(numLevels, avgVolume);

    // Zone 1: indices 20-25 (6 ticks)
    for (int i = 20; i < 26; ++i) profile[i] = avgVolume * 0.05;

    // Zone 2: indices 60-68 (9 ticks)
    for (int i = 60; i < 69; ++i) profile[i] = avgVolume * 0.05;

    auto zones = detector.DetectFromProfile(
        profile.data(), priceStart, tickSize, numLevels, avgVolume, 100);

    TEST_ASSERT(zones.size() == 2, "Should detect two single print zones");

    if (zones.size() == 2) {
        // Zones are returned in order (first zone has lower price)
        TEST_ASSERT(zones[0].widthTicks == 6, "First zone should be 6 ticks");
        TEST_ASSERT(zones[1].widthTicks == 9, "Second zone should be 9 ticks");
    }
}

void test_single_print_too_narrow() {
    TEST_SECTION("SinglePrintDetector - Too Narrow (< 3 ticks)");

    SinglePrintDetector detector;
    const double tickSize = 0.25;
    const double priceStart = 6090.00;
    const int numLevels = 100;
    const double avgVolume = 1000.0;

    // Create profile with thin zone only 2 ticks wide (below minimum)
    auto profile = CreateProfileWithSinglePrint(numLevels, avgVolume, 40, 2);

    auto zones = detector.DetectFromProfile(
        profile.data(), priceStart, tickSize, numLevels, avgVolume, 100);

    TEST_ASSERT(zones.empty(), "Should not detect zone narrower than minimum (3 ticks)");
}

void test_single_print_fill_progress() {
    TEST_SECTION("SinglePrintDetector - Fill Progress Tracking");

    SinglePrintDetector detector;
    const double tickSize = 0.25;
    const double priceStart = 6090.00;
    const int numLevels = 100;
    const double avgVolume = 1000.0;

    // Initial detection
    auto profile = CreateProfileWithSinglePrint(numLevels, avgVolume, 40, 10);  // 10 ticks

    auto zones = detector.DetectFromProfile(
        profile.data(), priceStart, tickSize, numLevels, avgVolume, 100);

    TEST_ASSERT(zones.size() == 1, "Should detect one zone initially");
    TEST_ASSERT(zones[0].fillProgress < 0.01, "Initial fill progress should be ~0");

    // Fill half the zone
    for (int i = 40; i < 45; ++i) {
        profile[i] = avgVolume;  // Fill 5 of 10 ticks
    }

    detector.UpdateFillProgress(zones, profile.data(), priceStart, tickSize, numLevels, avgVolume);

    TEST_ASSERT(zones[0].fillProgress >= 0.45 && zones[0].fillProgress <= 0.55,
        "Fill progress should be ~50% after half filled");
    TEST_ASSERT(zones[0].valid, "Zone should still be valid at 50% fill");

    // Fill the rest
    for (int i = 45; i < 50; ++i) {
        profile[i] = avgVolume;
    }

    detector.UpdateFillProgress(zones, profile.data(), priceStart, tickSize, numLevels, avgVolume);

    TEST_ASSERT(!zones[0].valid, "Zone should be invalid after complete fill");
}

// ============================================================================
// EXCESS DETECTOR WITH TAIL TESTS
// ============================================================================

void test_excess_with_real_tail() {
    TEST_SECTION("ExcessDetector - With Real Tail Data");

    ExcessDetector detector;
    const double tickSize = 0.25;
    const double sessionHigh = 6120.00;
    const double sessionLow = 6080.00;

    ActivityClassification activity;
    activity.valid = true;
    activity.activityType = AMTActivityType::RESPONSIVE;

    // Simulate: touch high with 5-tick tail, then move away
    double tailAtHigh = 5.0;  // 5 tick tail from profile

    // Touch high
    ExcessType result = detector.UpdateHigh(sessionHigh, sessionHigh, tickSize, 100, activity, tailAtHigh);
    TEST_ASSERT(result == ExcessType::NONE, "No excess immediately at touch");

    // Move away for several bars
    for (int i = 0; i < 5; ++i) {
        result = detector.UpdateHigh(sessionHigh, sessionHigh - (i + 1) * 2.0, tickSize, 101 + i, activity, tailAtHigh);
    }

    TEST_ASSERT(result == ExcessType::EXCESS_HIGH,
        "Should detect EXCESS_HIGH with tail + responsive + rejection");

    // Check state
    const auto& state = detector.GetHighState();
    TEST_ASSERT(state.tailDetected, "Tail should be detected");
    TEST_ASSERT(state.confirmedExcess, "Excess should be confirmed");
}

void test_excess_no_tail() {
    TEST_SECTION("ExcessDetector - No Tail (Poor High)");

    ExcessDetector detector;
    const double tickSize = 0.25;
    const double sessionHigh = 6120.00;

    ActivityClassification activity;
    activity.valid = true;
    activity.activityType = AMTActivityType::INITIATIVE;  // Not responsive

    // Touch high with NO tail
    double tailAtHigh = 0.0;

    detector.UpdateHigh(sessionHigh, sessionHigh, tickSize, 100, activity, tailAtHigh);

    // Move away
    ExcessType result = ExcessType::NONE;
    for (int i = 0; i < 5; ++i) {
        result = detector.UpdateHigh(sessionHigh, sessionHigh - (i + 1) * 2.0, tickSize, 101 + i, activity, tailAtHigh);
    }

    TEST_ASSERT(result == ExcessType::POOR_HIGH,
        "Should detect POOR_HIGH when rejected without tail");

    const auto& state = detector.GetHighState();
    TEST_ASSERT(!state.tailDetected, "Tail should NOT be detected");
    TEST_ASSERT(!state.confirmedExcess, "Should NOT be confirmed excess");
}

void test_excess_tail_at_low() {
    TEST_SECTION("ExcessDetector - Tail at Low");

    ExcessDetector detector;
    const double tickSize = 0.25;
    const double sessionLow = 6080.00;

    ActivityClassification activity;
    activity.valid = true;
    activity.activityType = AMTActivityType::RESPONSIVE;

    // Simulate: touch low with 4-tick tail, then move away
    double tailAtLow = 4.0;

    // Touch low
    detector.UpdateLow(sessionLow, sessionLow, tickSize, 100, activity, tailAtLow);

    // Move away upward
    ExcessType result = ExcessType::NONE;
    for (int i = 0; i < 5; ++i) {
        result = detector.UpdateLow(sessionLow, sessionLow + (i + 1) * 2.0, tickSize, 101 + i, activity, tailAtLow);
    }

    TEST_ASSERT(result == ExcessType::EXCESS_LOW,
        "Should detect EXCESS_LOW with tail at low + responsive + rejection");
}

// ============================================================================
// INTEGRATION: Single Prints + Excess Detection
// ============================================================================

void test_full_integration() {
    TEST_SECTION("Full Integration - Single Prints + Excess");

    AMTSignalEngine engine;
    const double tickSize = 0.25;
    const double poc = 6100.00;
    const double vah = 6110.00;
    const double val = 6090.00;
    double sessionHigh = 6118.00;
    double sessionLow = 6082.00;

    // Create profile with tail at high (thin volume at session high)
    const int numLevels = 160;  // 40 points = 160 ticks
    const double priceStart = 6080.00;
    const double avgVolume = 1000.0;

    // Profile with 6-tick tail at high
    auto profile = CreateProfileWithTailAtHigh(numLevels, avgVolume, 6);

    // Simulate trading sequence approaching high then rejecting
    double prices[] = {6100.00, 6105.00, 6110.00, 6115.00, 6118.00, 6115.00, 6110.00, 6105.00};
    double deltas[] = {0.05, 0.15, 0.20, 0.25, -0.10, -0.20, -0.25, -0.15};  // Responsive at high

    StateEvidence lastEvidence;
    for (int i = 1; i < 8; ++i) {
        // Calculate tail at high from profile
        // High is at tick index (6118 - 6080) / 0.25 = 152
        // With 6-tick tail, ticks 154-159 are thin
        double tailAtHigh = (i >= 4) ? 6.0 : 0.0;  // Tail visible after we touch high
        double tailAtLow = 0.0;

        lastEvidence = engine.ProcessBar(
            prices[i],
            prices[i-1],
            poc, vah, val,
            deltas[i],
            tickSize,
            sessionHigh,
            sessionLow,
            i,
            tailAtHigh,
            tailAtLow
        );
    }

    // After rejection from high with responsive activity, should detect excess
    TEST_ASSERT(lastEvidence.excessDetected == ExcessType::EXCESS_HIGH ||
                lastEvidence.excessDetected == ExcessType::POOR_HIGH,
        "Should detect excess or poor high after rejection sequence");

    // Detect single prints
    auto zones = engine.DetectSinglePrints(
        profile.data(), priceStart, tickSize, numLevels, avgVolume, 100);

    TEST_ASSERT(!zones.empty(), "Should detect single print zone (tail at high)");

    if (!zones.empty()) {
        // The tail zone should be at the top of the profile
        double highestZoneTop = 0.0;
        for (const auto& z : zones) {
            if (z.highPrice > highestZoneTop) {
                highestZoneTop = z.highPrice;
            }
        }

        // Highest zone should be near session high
        TEST_ASSERT(std::abs(highestZoneTop - (priceStart + (numLevels - 1) * tickSize)) < tickSize * 2,
            "Single print zone should be near top of profile (session high area)");
    }
}

// ============================================================================
// CONTAINS() METHOD TEST
// ============================================================================

void test_single_print_contains() {
    TEST_SECTION("SinglePrintZone - Contains() Method");

    SinglePrintZone zone;
    zone.lowPrice = 6100.00;
    zone.highPrice = 6105.00;
    zone.widthTicks = 20;
    zone.valid = true;

    const double tickSize = 0.25;

    // Price inside zone
    TEST_ASSERT(zone.Contains(6102.50), "Price 6102.50 should be inside zone");
    TEST_ASSERT(zone.Contains(6100.00), "Price at low boundary should be inside");
    TEST_ASSERT(zone.Contains(6105.00), "Price at high boundary should be inside");

    // Price outside zone
    TEST_ASSERT(!zone.Contains(6099.00), "Price 6099.00 should be outside zone");
    TEST_ASSERT(!zone.Contains(6106.00), "Price 6106.00 should be outside zone");

    // With tolerance
    TEST_ASSERT(zone.Contains(6099.75, tickSize), "Price 6099.75 should be inside with 1-tick tolerance");
    TEST_ASSERT(zone.Contains(6105.25, tickSize), "Price 6105.25 should be inside with 1-tick tolerance");
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "Single Print Detection Test Suite\n";
    std::cout << "==================================\n";

    // Basic detection tests
    test_single_print_basic_detection();
    test_single_print_multiple_zones();
    test_single_print_too_narrow();
    test_single_print_fill_progress();

    // Excess detection with tail tests
    test_excess_with_real_tail();
    test_excess_no_tail();
    test_excess_tail_at_low();

    // Integration test
    test_full_integration();

    // Contains test
    test_single_print_contains();

    // Summary
    std::cout << "\n=== SUMMARY ===\n";
    std::cout << "Tests passed: " << g_testsPassed << "\n";
    std::cout << "Tests failed: " << g_testsFailed << "\n";

    if (g_testsFailed == 0) {
        std::cout << "\n[SUCCESS] All tests passed!\n";
        return 0;
    } else {
        std::cout << "\n[FAILURE] Some tests failed.\n";
        return 1;
    }
}
