// test_tpo_acceptance.cpp - Verify tpoAcceptance computation
// Tests the acceptance formula, validity handling, scoring integration, and edge cases
#include "test_sierrachart_mock.h"
#include "../amt_core.h"
#include "../AMT_config.h"
#include "../AMT_Patterns.h"
#include "../AMT_Helpers.h"

#include <iostream>
#include <cassert>
#include <cmath>
#include <algorithm>

// ============================================================================
// COPIED FROM AMT_VolumeProfile.h - TPOAcceptanceResult and ComputeTPOAcceptance
// ============================================================================

// Config constants (from AMT_config.h ZoneConfig defaults)
constexpr int TPO_ALIGNMENT_MAX_DIVERGENCE_TICKS = 12;  // 3 ES points
constexpr int TPO_COMPACTNESS_MAX_WIDTH_TICKS = 100;    // 25 ES points

struct TPOAcceptanceResult
{
    float acceptance = 0.0f;        // Final composite score [0, 1]
    bool valid = false;             // True if computation succeeded

    // Component scores - USE ACCESSORS FOR READS (direct access banned except assignment)
    float vaBalance = 0.0f;         // [0, 1] POC position symmetry within VA
    float tpoVbpAlignment_ = 0.0f;  // PRIVATE: use GetTpoVbpAlignment()
    float vaCompactness = 0.0f;     // [0, 1] how narrow VA is

    // Component validity flags (no-fallback policy)
    bool alignmentValid = false;    // True if VBP POC was available for alignment calc

    // Raw inputs for diagnostics
    double tpoPOC = 0.0;
    double tpoVAH = 0.0;
    double tpoVAL = 0.0;
    double vbpPOC = 0.0;            // May be 0 if VBP unavailable
    int vaWidthTicks = 0;
    int pocDivergenceTicks = 0;

    // GUARDED ACCESSOR: asserts validity before returning dead-value field
    float GetTpoVbpAlignment() const
    {
        assert(alignmentValid && "BUG: reading tpoVbpAlignment without validity check");
        return tpoVbpAlignment_;
    }
};

inline TPOAcceptanceResult ComputeTPOAcceptance(
    double tpoPOC,
    double tpoVAH,
    double tpoVAL,
    double vbpPOC,
    double tickSize,
    int alignmentMaxDivergenceTicks = TPO_ALIGNMENT_MAX_DIVERGENCE_TICKS,
    int compactnessMaxWidthTicks = TPO_COMPACTNESS_MAX_WIDTH_TICKS)
{
    TPOAcceptanceResult result;

    // Validity checks
    if (tickSize <= 0.0)
        return result;

    if (!AMT::IsValidPrice(tpoPOC) || tpoPOC <= 0.0)
        return result;

    if (!AMT::IsValidPrice(tpoVAH) || tpoVAH <= 0.0)
        return result;

    if (!AMT::IsValidPrice(tpoVAL) || tpoVAL <= 0.0)
        return result;

    if (tpoVAH <= tpoVAL)
        return result;

    // Store raw inputs for diagnostics
    result.tpoPOC = tpoPOC;
    result.tpoVAH = tpoVAH;
    result.tpoVAL = tpoVAL;
    result.vbpPOC = vbpPOC;

    const double vaWidth = tpoVAH - tpoVAL;
    result.vaWidthTicks = static_cast<int>(vaWidth / tickSize);

    // Component 1: VA Balance (40% weight)
    {
        const double pocRelPos = (tpoPOC - tpoVAL) / vaWidth;
        const double clampedPos = (std::max)(0.0, (std::min)(1.0, pocRelPos));
        const double distFromCenter = std::abs(clampedPos - 0.5) * 2.0;
        result.vaBalance = static_cast<float>(1.0 - distFromCenter);
    }

    // Component 2: TPO-VBP Alignment (35% weight when valid)
    // NO-FALLBACK POLICY: If VBP unavailable, alignment EXCLUDED from blend
    {
        const double thresholdTicks = static_cast<double>(alignmentMaxDivergenceTicks);

        if (AMT::IsValidPrice(vbpPOC) && vbpPOC > 0.0)
        {
            const double divergence = std::abs(tpoPOC - vbpPOC);
            const double divergenceTicks = divergence / tickSize;
            result.pocDivergenceTicks = static_cast<int>(divergenceTicks);

            const double rawAlignment = 1.0 - (divergenceTicks / thresholdTicks);
            result.tpoVbpAlignment__ = static_cast<float>(
                (std::max)(0.0, (std::min)(1.0, rawAlignment)));
            result.alignmentValid = true;
        }
        else
        {
            // VBP unavailable - alignment EXCLUDED (no fallback)
            result.tpoVbpAlignment__ = 0.0f;  // Dead value - accessor asserts validity
            result.alignmentValid = false;
            result.pocDivergenceTicks = -1;
        }
    }

    // Component 3: VA Compactness (25% weight)
    {
        const double maxWidthTicks = static_cast<double>(compactnessMaxWidthTicks);
        const double vaWidthTicks = static_cast<double>(result.vaWidthTicks);
        const double rawCompactness = 1.0 - (vaWidthTicks / maxWidthTicks);

        result.vaCompactness = static_cast<float>(
            (std::max)(0.0, (std::min)(1.0, rawCompactness)));
    }

    // Composite acceptance score (with renormalization for missing components)
    {
        constexpr float W_BALANCE = 0.40f;
        constexpr float W_ALIGNMENT = 0.35f;
        constexpr float W_COMPACTNESS = 0.25f;

        float score = 0.0f;
        float totalWeight = 0.0f;

        // Balance: always included
        score += W_BALANCE * result.vaBalance;
        totalWeight += W_BALANCE;

        // Alignment: only included if VBP POC was available
        if (result.alignmentValid)
        {
            score += W_ALIGNMENT * result.GetTpoVbpAlignment();  // Accessor asserts validity
            totalWeight += W_ALIGNMENT;
        }

        // Compactness: always included
        score += W_COMPACTNESS * result.vaCompactness;
        totalWeight += W_COMPACTNESS;

        // Renormalize
        if (totalWeight > 0.0f)
        {
            result.acceptance = (std::max)(0.0f, (std::min)(1.0f, score / totalWeight));
        }
        else
        {
            result.acceptance = 0.0f;
        }

        result.valid = true;
    }

    return result;
}

// ============================================================================
// TEST: Bounds check - acceptance always in [0, 1]
// ============================================================================

void test_bounds_always_0_to_1() {
    std::cout << "=== Test: Bounds check - acceptance always in [0, 1] ===" << std::endl;

    const double tickSize = 0.25;  // ES tick size

    // Test extreme values
    struct TestCase {
        double tpoPOC;
        double tpoVAH;
        double tpoVAL;
        double vbpPOC;
        const char* description;
    };

    TestCase cases[] = {
        // Normal cases
        {6100.00, 6105.00, 6095.00, 6100.00, "Normal balanced profile"},
        {6100.00, 6150.00, 6050.00, 6100.00, "Wide VA profile"},
        {6100.00, 6101.00, 6099.00, 6100.00, "Very tight VA profile"},

        // POC at edges
        {6095.00, 6105.00, 6095.00, 6100.00, "POC at VAL edge"},
        {6105.00, 6105.00, 6095.00, 6100.00, "POC at VAH edge"},

        // Large VBP divergence
        {6100.00, 6105.00, 6095.00, 6150.00, "Large POC divergence (50 pts)"},
        {6100.00, 6105.00, 6095.00, 6050.00, "Large POC divergence (negative)"},

        // VBP unavailable
        {6100.00, 6105.00, 6095.00, 0.0, "VBP unavailable"},
    };

    for (const auto& tc : cases) {
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            tc.tpoPOC, tc.tpoVAH, tc.tpoVAL, tc.vbpPOC, tickSize);

        if (result.valid) {
            std::cout << "  " << tc.description << ": acceptance=" << result.acceptance << std::endl;
            assert(result.acceptance >= 0.0f && result.acceptance <= 1.0f);
            assert(result.vaBalance >= 0.0f && result.vaBalance <= 1.0f);
            assert(result.tpoVbpAlignment_ >= 0.0f && result.tpoVbpAlignment_ <= 1.0f);
            assert(result.vaCompactness >= 0.0f && result.vaCompactness <= 1.0f);
        }
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Clear balanced profile => high acceptance
// ============================================================================

void test_high_acceptance_balanced_profile() {
    std::cout << "=== Test: High acceptance - balanced, aligned, compact profile ===" << std::endl;

    const double tickSize = 0.25;

    // POC perfectly centered, tight VA, TPO and VBP aligned
    const double tpoPOC = 6100.00;
    const double tpoVAH = 6102.50;  // 10 ticks above POC
    const double tpoVAL = 6097.50;  // 10 ticks below POC
    const double vbpPOC = 6100.00;  // Perfect alignment

    TPOAcceptanceResult result = ComputeTPOAcceptance(
        tpoPOC, tpoVAH, tpoVAL, vbpPOC, tickSize);

    std::cout << "  Result valid: " << result.valid << std::endl;
    std::cout << "  Acceptance: " << result.acceptance << std::endl;
    std::cout << "  Components: bal=" << result.vaBalance
              << " align=" << result.tpoVbpAlignment_
              << " compact=" << result.vaCompactness << std::endl;

    assert(result.valid == true);
    assert(result.acceptance > 0.7f);  // Should be high
    assert(result.vaBalance > 0.9f);   // Perfectly centered
    assert(result.tpoVbpAlignment_ == 1.0f);  // Perfect alignment
    assert(result.vaCompactness > 0.7f);  // Tight VA (20 ticks = 5 pts)

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Rejection-tailed / messy profile => low acceptance
// ============================================================================

void test_low_acceptance_messy_profile() {
    std::cout << "=== Test: Low acceptance - skewed, divergent, wide profile ===" << std::endl;

    const double tickSize = 0.25;

    // POC at edge of VA (skewed), wide VA, large TPO-VBP divergence
    const double tpoPOC = 6145.00;  // Near VAH (skewed)
    const double tpoVAH = 6150.00;
    const double tpoVAL = 6050.00;  // Wide: 100 points = 400 ticks
    const double vbpPOC = 6100.00;  // 45 pts divergence (180 ticks)

    TPOAcceptanceResult result = ComputeTPOAcceptance(
        tpoPOC, tpoVAH, tpoVAL, vbpPOC, tickSize);

    std::cout << "  Result valid: " << result.valid << std::endl;
    std::cout << "  Acceptance: " << result.acceptance << std::endl;
    std::cout << "  Components: bal=" << result.vaBalance
              << " align=" << result.tpoVbpAlignment_
              << " compact=" << result.vaCompactness << std::endl;
    std::cout << "  VA width: " << result.vaWidthTicks << " ticks" << std::endl;
    std::cout << "  POC divergence: " << result.pocDivergenceTicks << " ticks" << std::endl;

    assert(result.valid == true);
    assert(result.acceptance < 0.4f);  // Should be low
    assert(result.vaBalance < 0.3f);   // POC near edge
    assert(result.tpoVbpAlignment_ == 0.0f);  // Large divergence (>12 ticks)
    assert(result.vaCompactness == 0.0f);  // Very wide VA (400 ticks > 100 max)

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Missing TPO data => invalid
// ============================================================================

void test_invalid_missing_tpo_data() {
    std::cout << "=== Test: Missing TPO data => invalid ===" << std::endl;

    const double tickSize = 0.25;
    const double vbpPOC = 6100.00;

    // Test: Zero POC
    {
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            0.0, 6105.00, 6095.00, vbpPOC, tickSize);
        std::cout << "  Zero POC: valid=" << result.valid << std::endl;
        assert(result.valid == false);
    }

    // Test: Zero VAH
    {
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            6100.00, 0.0, 6095.00, vbpPOC, tickSize);
        std::cout << "  Zero VAH: valid=" << result.valid << std::endl;
        assert(result.valid == false);
    }

    // Test: Zero VAL
    {
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            6100.00, 6105.00, 0.0, vbpPOC, tickSize);
        std::cout << "  Zero VAL: valid=" << result.valid << std::endl;
        assert(result.valid == false);
    }

    // Test: Negative prices (NaN-like)
    {
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            -1.0, 6105.00, 6095.00, vbpPOC, tickSize);
        std::cout << "  Negative POC: valid=" << result.valid << std::endl;
        assert(result.valid == false);
    }

    // Test: Incoherent VA (VAH <= VAL)
    {
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            6100.00, 6095.00, 6105.00, vbpPOC, tickSize);
        std::cout << "  VAH <= VAL: valid=" << result.valid << std::endl;
        assert(result.valid == false);
    }

    // Test: Zero tick size
    {
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            6100.00, 6105.00, 6095.00, vbpPOC, 0.0);
        std::cout << "  Zero tick size: valid=" << result.valid << std::endl;
        assert(result.valid == false);
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: VBP unavailable => alignment EXCLUDED, blend renormalized (no fallback)
// ============================================================================

void test_vbp_unavailable_alignment_excluded() {
    std::cout << "=== Test: VBP unavailable => alignment excluded, renormalized ===" << std::endl;

    const double tickSize = 0.25;

    // Good TPO profile but no VBP
    const double tpoPOC = 6100.00;
    const double tpoVAH = 6102.50;  // 10 ticks above
    const double tpoVAL = 6097.50;  // 10 ticks below
    const double vbpPOC = 0.0;  // VBP unavailable

    TPOAcceptanceResult result = ComputeTPOAcceptance(
        tpoPOC, tpoVAH, tpoVAL, vbpPOC, tickSize);

    std::cout << "  Result valid: " << result.valid << std::endl;
    std::cout << "  Alignment valid: " << result.alignmentValid << std::endl;
    std::cout << "  Alignment component: " << result.tpoVbpAlignment_ << std::endl;
    std::cout << "  POC divergence ticks: " << result.pocDivergenceTicks << std::endl;

    // Core assertions: NO FALLBACK
    assert(result.valid == true);
    assert(result.alignmentValid == false);  // Alignment EXCLUDED
    assert(result.tpoVbpAlignment_ == 0.0f);  // Not 0.5 (no fallback)
    assert(result.pocDivergenceTicks == -1);  // Indicates VBP unavailable

    // Verify renormalized blend: (0.40*balance + 0.25*compactness) / 0.65
    // POC centered => balance = 1.0
    // 20-tick VA => compactness = 1.0 - 20/100 = 0.8
    const float expectedBalance = 1.0f;
    const float expectedCompactness = 0.8f;
    const float expectedAcceptance = (0.40f * expectedBalance + 0.25f * expectedCompactness) / 0.65f;

    std::cout << "  Balance: " << result.vaBalance << " (expected " << expectedBalance << ")" << std::endl;
    std::cout << "  Compactness: " << result.vaCompactness << " (expected " << expectedCompactness << ")" << std::endl;
    std::cout << "  Acceptance: " << result.acceptance << " (expected " << expectedAcceptance << ")" << std::endl;

    assert(std::abs(result.vaBalance - expectedBalance) < 0.001f);
    assert(std::abs(result.vaCompactness - expectedCompactness) < 0.001f);
    assert(std::abs(result.acceptance - expectedAcceptance) < 0.001f);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Scoring integration - valid TPO contributes
// ============================================================================

void test_scoring_with_valid_tpo() {
    std::cout << "=== Test: calculate_score includes valid TPO ===" << std::endl;

    AMT::ConfidenceWeights w;
    AMT::ConfidenceAttribute conf;

    conf.domStrength = 0.8f;
    conf.domStrengthValid = true;
    conf.deltaConsistency = 0.6f;
    conf.deltaConsistencyValid = true;
    conf.volumeProfileClarity = 0.7f;
    conf.volumeProfileClarityValid = true;
    conf.tpoAcceptance = 0.9f;
    conf.tpoAcceptanceValid = true;  // VALID
    conf.liquidityAvailability = 0.5f;
    conf.liquidityAvailabilityValid = true;

    AMT::ScoreResult result = conf.calculate_score(w);
    assert(result.scoreValid);
    std::cout << "  Score with valid TPO: " << result.score << std::endl;

    // All metrics valid: full weighted sum
    float expected = (0.8f*w.dom + 0.6f*w.delta + 0.7f*w.profile + 0.9f*w.tpo + 0.5f*w.liquidity);
    std::cout << "  Expected: " << expected << std::endl;

    assert(std::abs(result.score - expected) < 0.01f);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Scoring integration - invalid TPO excluded (no weight dilution)
// ============================================================================

void test_scoring_with_invalid_tpo() {
    std::cout << "=== Test: calculate_score excludes invalid TPO (no dilution) ===" << std::endl;

    AMT::ConfidenceWeights w;
    AMT::ConfidenceAttribute conf;

    conf.domStrength = 0.8f;
    conf.domStrengthValid = true;
    conf.deltaConsistency = 0.6f;
    conf.deltaConsistencyValid = true;
    conf.volumeProfileClarity = 0.7f;
    conf.volumeProfileClarityValid = true;
    conf.tpoAcceptance = 0.0f;  // Would be 0 if invalid
    conf.tpoAcceptanceValid = false;  // INVALID
    conf.liquidityAvailability = 0.5f;
    conf.liquidityAvailabilityValid = true;

    AMT::ScoreResult result = conf.calculate_score(w);
    assert(result.scoreValid);
    std::cout << "  Score with invalid TPO: " << result.score << std::endl;

    // TPO excluded: sum of (dom + delta + profile + liquidity), renormalized
    float activeWeight = w.dom + w.delta + w.profile + w.liquidity;
    float expected = (0.8f*w.dom + 0.6f*w.delta + 0.7f*w.profile + 0.5f*w.liquidity) / activeWeight;
    std::cout << "  Expected (TPO excluded, renormalized): " << expected << std::endl;

    assert(std::abs(result.score - expected) < 0.01f);

    // Verify no dilution: score without TPO should be HIGHER than if TPO contributed 0
    AMT::ConfidenceAttribute confWithZeroTPO = conf;
    confWithZeroTPO.tpoAcceptanceValid = true;  // Force inclusion at 0.0
    confWithZeroTPO.tpoAcceptance = 0.0f;
    AMT::ScoreResult resultWithZero = confWithZeroTPO.calculate_score(w);
    assert(resultWithZero.scoreValid);

    std::cout << "  Score if TPO were valid but 0.0: " << resultWithZero.score << std::endl;
    std::cout << "  Renormalized score (TPO excluded): " << result.score << std::endl;
    assert(result.score > resultWithZero.score);  // Renormalized should be higher

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Default validity is false
// ============================================================================

void test_default_validity_false() {
    std::cout << "=== Test: tpoAcceptanceValid default is false ===" << std::endl;

    AMT::ConfidenceAttribute conf;

    std::cout << "  tpoAcceptanceValid (default): " << conf.tpoAcceptanceValid << std::endl;
    std::cout << "  tpoAcceptance (default): " << conf.tpoAcceptance << std::endl;

    assert(conf.tpoAcceptanceValid == false);
    assert(conf.tpoAcceptance == 0.0f);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Component formulas verification
// ============================================================================

void test_component_formula_va_balance() {
    std::cout << "=== Test: VA Balance formula verification ===" << std::endl;

    const double tickSize = 0.25;

    // POC exactly centered: balance = 1.0
    {
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            6100.00, 6110.00, 6090.00, 0.0, tickSize);
        std::cout << "  Centered POC: balance=" << result.vaBalance << std::endl;
        assert(std::abs(result.vaBalance - 1.0f) < 0.001f);
    }

    // POC at VAL edge: balance = 0.0
    {
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            6090.00, 6110.00, 6090.00, 0.0, tickSize);
        std::cout << "  POC at VAL: balance=" << result.vaBalance << std::endl;
        assert(std::abs(result.vaBalance - 0.0f) < 0.001f);
    }

    // POC at VAH edge: balance = 0.0
    {
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            6110.00, 6110.00, 6090.00, 0.0, tickSize);
        std::cout << "  POC at VAH: balance=" << result.vaBalance << std::endl;
        assert(std::abs(result.vaBalance - 0.0f) < 0.001f);
    }

    // POC at 75% (towards VAH): balance = 0.5
    {
        // VA is 6090-6110 (20 pts), 75% = 6090 + 15 = 6105
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            6105.00, 6110.00, 6090.00, 0.0, tickSize);
        std::cout << "  POC at 75%: balance=" << result.vaBalance << std::endl;
        assert(std::abs(result.vaBalance - 0.5f) < 0.001f);
    }

    std::cout << "  PASSED" << std::endl;
}

void test_component_formula_tpo_vbp_alignment() {
    std::cout << "=== Test: TPO-VBP Alignment formula verification ===" << std::endl;

    const double tickSize = 0.25;
    const double tpoVAH = 6110.00;
    const double tpoVAL = 6090.00;

    // Perfect alignment: 1.0
    {
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            6100.00, tpoVAH, tpoVAL, 6100.00, tickSize);
        std::cout << "  0-tick divergence: align=" << result.tpoVbpAlignment_ << std::endl;
        assert(std::abs(result.tpoVbpAlignment_ - 1.0f) < 0.001f);
    }

    // 6-tick divergence (half threshold): 0.5
    {
        // 6 ticks = 1.5 pts
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            6100.00, tpoVAH, tpoVAL, 6101.50, tickSize);
        std::cout << "  6-tick divergence: align=" << result.tpoVbpAlignment_
                  << " (divTicks=" << result.pocDivergenceTicks << ")" << std::endl;
        assert(std::abs(result.tpoVbpAlignment_ - 0.5f) < 0.001f);
    }

    // 12-tick divergence (at threshold): 0.0
    {
        // 12 ticks = 3 pts
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            6100.00, tpoVAH, tpoVAL, 6103.00, tickSize);
        std::cout << "  12-tick divergence: align=" << result.tpoVbpAlignment_ << std::endl;
        assert(std::abs(result.tpoVbpAlignment_ - 0.0f) < 0.001f);
    }

    // Beyond threshold: clamped to 0.0
    {
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            6100.00, tpoVAH, tpoVAL, 6120.00, tickSize);
        std::cout << "  80-tick divergence: align=" << result.tpoVbpAlignment_ << std::endl;
        assert(result.tpoVbpAlignment_ == 0.0f);
    }

    std::cout << "  PASSED" << std::endl;
}

void test_component_formula_va_compactness() {
    std::cout << "=== Test: VA Compactness formula verification ===" << std::endl;

    const double tickSize = 0.25;

    // Very tight VA (10 ticks = 2.5 pts): 1.0 - 10/100 = 0.9
    {
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            6100.00, 6101.25, 6098.75, 0.0, tickSize);
        std::cout << "  10-tick VA: compact=" << result.vaCompactness
                  << " (width=" << result.vaWidthTicks << ")" << std::endl;
        assert(std::abs(result.vaCompactness - 0.9f) < 0.001f);
    }

    // Moderate VA (50 ticks = 12.5 pts): 1.0 - 50/100 = 0.5
    {
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            6100.00, 6106.25, 6093.75, 0.0, tickSize);
        std::cout << "  50-tick VA: compact=" << result.vaCompactness << std::endl;
        assert(std::abs(result.vaCompactness - 0.5f) < 0.001f);
    }

    // At max threshold (100 ticks = 25 pts): 0.0
    {
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            6100.00, 6112.50, 6087.50, 0.0, tickSize);
        std::cout << "  100-tick VA: compact=" << result.vaCompactness << std::endl;
        assert(std::abs(result.vaCompactness - 0.0f) < 0.001f);
    }

    // Beyond max (clamped to 0.0)
    {
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            6100.00, 6150.00, 6050.00, 0.0, tickSize);
        std::cout << "  400-tick VA: compact=" << result.vaCompactness << std::endl;
        assert(result.vaCompactness == 0.0f);
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Composite weights verify to expected values
// ============================================================================

void test_composite_weights() {
    std::cout << "=== Test: Composite weights (40% bal + 35% align + 25% compact) ===" << std::endl;

    const double tickSize = 0.25;

    // All components at 1.0: acceptance = 1.0
    {
        // POC centered, aligned, tight VA
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            6100.00, 6102.50, 6097.50, 6100.00, tickSize);

        std::cout << "  All 1.0: bal=" << result.vaBalance
                  << " align=" << result.tpoVbpAlignment_
                  << " compact=" << result.vaCompactness
                  << " => accept=" << result.acceptance << std::endl;

        // With 20-tick VA (5 pts), compactness = 0.8
        // Expected: 0.4*1.0 + 0.35*1.0 + 0.25*0.8 = 0.95
        float expected = 0.40f * result.vaBalance +
                        0.35f * result.tpoVbpAlignment_ +
                        0.25f * result.vaCompactness;
        assert(std::abs(result.acceptance - expected) < 0.001f);
    }

    // All components at 0.0: acceptance = 0.0
    {
        // POC at edge, max divergence, wide VA
        TPOAcceptanceResult result = ComputeTPOAcceptance(
            6050.00, 6150.00, 6050.00, 6200.00, tickSize);

        std::cout << "  All 0.0: bal=" << result.vaBalance
                  << " align=" << result.tpoVbpAlignment_
                  << " compact=" << result.vaCompactness
                  << " => accept=" << result.acceptance << std::endl;

        assert(result.acceptance == 0.0f);
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "TPO Acceptance Tests" << std::endl;
    std::cout << "========================================\n" << std::endl;

    std::cout << "--- Bounds and Range Tests ---\n" << std::endl;
    test_bounds_always_0_to_1();

    std::cout << "\n--- Acceptance Quality Tests ---\n" << std::endl;
    test_high_acceptance_balanced_profile();
    test_low_acceptance_messy_profile();

    std::cout << "\n--- Validity and Error Handling Tests ---\n" << std::endl;
    test_invalid_missing_tpo_data();
    test_vbp_unavailable_alignment_excluded();

    std::cout << "\n--- Scoring Integration Tests ---\n" << std::endl;
    test_scoring_with_valid_tpo();
    test_scoring_with_invalid_tpo();
    test_default_validity_false();

    std::cout << "\n--- Component Formula Tests ---\n" << std::endl;
    test_component_formula_va_balance();
    test_component_formula_tpo_vbp_alignment();
    test_component_formula_va_compactness();
    test_composite_weights();

    std::cout << "\n========================================" << std::endl;
    std::cout << "All tests PASSED!" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
}
