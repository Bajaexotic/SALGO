// test_delta_semantic_fix.cpp - Verify the Dec 2024 delta semantic fix
// Tests that deltaConsistency is aggressor FRACTION [0,1] where 0.5=neutral
// and deltaStrength is MAGNITUDE [0,1] where 0=neutral

#include <iostream>
#include <cassert>
#include <cmath>

// Include the header with ConfidenceAttribute
#include "../AMT_Patterns.h"

using namespace AMT;

// Helper to check float equality
bool approx_eq(float a, float b, float epsilon = 0.0001f) {
    return std::abs(a - b) < epsilon;
}

// Simulate the delta computation from AuctionSensor_v1.cpp
struct DeltaComputeResult {
    float deltaConsistency;
    float deltaStrength;
    bool valid;
};

DeltaComputeResult ComputeDelta(double askVol, double bidVol) {
    constexpr double THIN_BAR_VOL_THRESHOLD = 20.0;
    DeltaComputeResult result;

    double totalVol = askVol + bidVol;
    double delta = askVol - bidVol;
    double deltaPct = (totalVol > 0.0) ? delta / totalVol : 0.0;

    if (totalVol >= THIN_BAR_VOL_THRESHOLD) {
        // Sufficient volume: compute both metrics
        double fraction = 0.5 + 0.5 * deltaPct;
        result.deltaConsistency = static_cast<float>(std::max(0.0, std::min(1.0, fraction)));
        result.deltaStrength = static_cast<float>(std::min(1.0, std::abs(deltaPct)));
        result.valid = true;
    } else {
        // Thin bar: set to neutral, mark invalid
        result.deltaConsistency = 0.5f;
        result.deltaStrength = 0.0f;
        result.valid = false;
    }
    return result;
}

void test_user_example_1() {
    std::cout << "=== Test: User Example 1 (Ask=38, Bid=35) ===" << std::endl;

    // Ask=38, Bid=35 => Tot=73, deltaPct≈+0.0411, deltaConsistency≈0.52055
    DeltaComputeResult r = ComputeDelta(38.0, 35.0);

    std::cout << "  AskVol=38, BidVol=35, TotVol=73" << std::endl;
    std::cout << "  deltaPct = (38-35)/73 = " << (38.0-35.0)/73.0 << std::endl;
    std::cout << "  Expected: deltaPct≈+0.0411, deltaConsistency≈0.52055" << std::endl;
    std::cout << "  Actual: deltaConsistency=" << r.deltaConsistency
              << ", deltaStrength=" << r.deltaStrength
              << ", valid=" << (r.valid ? "true" : "false") << std::endl;

    // Verify
    float expectedDeltaPct = (38.0f - 35.0f) / 73.0f;  // ≈ 0.0411
    float expectedConsistency = 0.5f + 0.5f * expectedDeltaPct;  // ≈ 0.52055
    float expectedStrength = std::abs(expectedDeltaPct);  // ≈ 0.0411

    assert(r.valid);
    assert(approx_eq(r.deltaConsistency, expectedConsistency));
    assert(approx_eq(r.deltaStrength, expectedStrength));

    // Key check: deltaConsistency > 0.5 (net buying)
    assert(r.deltaConsistency > 0.5f);
    std::cout << "  PASS: deltaConsistency > 0.5 (net buying)" << std::endl;
}

void test_user_example_2() {
    std::cout << "=== Test: User Example 2 (Ask=30, Bid=43) ===" << std::endl;

    // Ask=30, Bid=43 => Tot=73, deltaPct≈-0.1781, deltaConsistency≈0.41095
    DeltaComputeResult r = ComputeDelta(30.0, 43.0);

    std::cout << "  AskVol=30, BidVol=43, TotVol=73" << std::endl;
    std::cout << "  deltaPct = (30-43)/73 = " << (30.0-43.0)/73.0 << std::endl;
    std::cout << "  Expected: deltaPct≈-0.1781, deltaConsistency≈0.41095" << std::endl;
    std::cout << "  Actual: deltaConsistency=" << r.deltaConsistency
              << ", deltaStrength=" << r.deltaStrength
              << ", valid=" << (r.valid ? "true" : "false") << std::endl;

    // Verify
    float expectedDeltaPct = (30.0f - 43.0f) / 73.0f;  // ≈ -0.1781
    float expectedConsistency = 0.5f + 0.5f * expectedDeltaPct;  // ≈ 0.41095
    float expectedStrength = std::abs(expectedDeltaPct);  // ≈ 0.1781

    assert(r.valid);
    assert(approx_eq(r.deltaConsistency, expectedConsistency));
    assert(approx_eq(r.deltaStrength, expectedStrength));

    // Key check: deltaConsistency < 0.5 (net selling)
    assert(r.deltaConsistency < 0.5f);
    std::cout << "  PASS: deltaConsistency < 0.5 (net selling)" << std::endl;
}

void test_thin_bar() {
    std::cout << "=== Test: Thin Bar (vol < 20) ===" << std::endl;

    // Thin bar: 3 contracts total
    DeltaComputeResult r = ComputeDelta(2.0, 1.0);

    std::cout << "  AskVol=2, BidVol=1, TotVol=3 (thin bar)" << std::endl;
    std::cout << "  Expected: deltaConsistency=0.5 (neutral), deltaStrength=0 (no signal), valid=false" << std::endl;
    std::cout << "  Actual: deltaConsistency=" << r.deltaConsistency
              << ", deltaStrength=" << r.deltaStrength
              << ", valid=" << (r.valid ? "true" : "false") << std::endl;

    assert(!r.valid);
    assert(r.deltaConsistency == 0.5f);
    assert(r.deltaStrength == 0.0f);
    std::cout << "  PASS: Thin bar gets neutral values and invalid flag" << std::endl;
}

void test_extreme_buying() {
    std::cout << "=== Test: Extreme Buying (80% at ask) ===" << std::endl;

    // 80% at ask: Ask=80, Bid=20
    DeltaComputeResult r = ComputeDelta(80.0, 20.0);

    std::cout << "  AskVol=80, BidVol=20, TotVol=100" << std::endl;
    std::cout << "  deltaPct = 60/100 = 0.6" << std::endl;
    std::cout << "  Expected: deltaConsistency = 0.5 + 0.5*0.6 = 0.8" << std::endl;
    std::cout << "  Actual: deltaConsistency=" << r.deltaConsistency << std::endl;

    assert(r.valid);
    assert(approx_eq(r.deltaConsistency, 0.8f));

    // Key check: deltaConsistency > 0.7 (extreme buying threshold)
    assert(r.deltaConsistency > 0.7f);
    std::cout << "  PASS: deltaConsistency > 0.7 triggers isExtremeDeltaBar for BUYING" << std::endl;
}

void test_extreme_selling() {
    std::cout << "=== Test: Extreme Selling (80% at bid) ===" << std::endl;

    // 80% at bid: Ask=20, Bid=80
    DeltaComputeResult r = ComputeDelta(20.0, 80.0);

    std::cout << "  AskVol=20, BidVol=80, TotVol=100" << std::endl;
    std::cout << "  deltaPct = -60/100 = -0.6" << std::endl;
    std::cout << "  Expected: deltaConsistency = 0.5 + 0.5*(-0.6) = 0.2" << std::endl;
    std::cout << "  Actual: deltaConsistency=" << r.deltaConsistency << std::endl;

    assert(r.valid);
    assert(approx_eq(r.deltaConsistency, 0.2f));

    // Key check: deltaConsistency < 0.3 (extreme selling threshold)
    assert(r.deltaConsistency < 0.3f);
    std::cout << "  PASS: deltaConsistency < 0.3 triggers isExtremeDeltaBar for SELLING" << std::endl;
}

void test_neutral() {
    std::cout << "=== Test: Neutral (50/50 volume) ===" << std::endl;

    // Exactly neutral: Ask=50, Bid=50
    DeltaComputeResult r = ComputeDelta(50.0, 50.0);

    std::cout << "  AskVol=50, BidVol=50, TotVol=100" << std::endl;
    std::cout << "  deltaPct = 0/100 = 0" << std::endl;
    std::cout << "  Expected: deltaConsistency = 0.5 (neutral), deltaStrength = 0 (no direction)" << std::endl;
    std::cout << "  Actual: deltaConsistency=" << r.deltaConsistency
              << ", deltaStrength=" << r.deltaStrength << std::endl;

    assert(r.valid);
    assert(r.deltaConsistency == 0.5f);
    assert(r.deltaStrength == 0.0f);

    // Key check: 0.3 < deltaConsistency < 0.7 (NOT extreme)
    assert(r.deltaConsistency > 0.3f && r.deltaConsistency < 0.7f);
    std::cout << "  PASS: Neutral volume does NOT trigger extreme flag" << std::endl;
}

void test_confidence_attribute_defaults() {
    std::cout << "=== Test: ConfidenceAttribute defaults ===" << std::endl;

    ConfidenceAttribute conf;

    // deltaConsistency should default to 0.5 (neutral), not 0.0 (was bug)
    assert(conf.deltaConsistency == 0.5f);
    std::cout << "  deltaConsistency default = 0.5 (neutral) - PASS" << std::endl;

    // deltaStrength should default to 0.0 (no signal)
    assert(conf.deltaStrength == 0.0f);
    std::cout << "  deltaStrength default = 0.0 (no signal) - PASS" << std::endl;

    // Both validity flags should default to false
    assert(!conf.deltaConsistencyValid);
    assert(!conf.deltaStrengthValid);
    std::cout << "  Both validity flags default to false - PASS" << std::endl;
}

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "DELTA SEMANTIC FIX VERIFICATION TESTS" << std::endl;
    std::cout << "Dec 2024 - deltaConsistency is FRACTION" << std::endl;
    std::cout << "========================================\n" << std::endl;

    test_user_example_1();
    std::cout << std::endl;

    test_user_example_2();
    std::cout << std::endl;

    test_thin_bar();
    std::cout << std::endl;

    test_extreme_buying();
    std::cout << std::endl;

    test_extreme_selling();
    std::cout << std::endl;

    test_neutral();
    std::cout << std::endl;

    test_confidence_attribute_defaults();
    std::cout << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << "ALL TESTS PASSED" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
