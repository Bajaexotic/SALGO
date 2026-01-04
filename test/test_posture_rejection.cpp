// ============================================================================
// test_posture_rejection.cpp
// Tests defense-in-depth posture gating inside CreateZoneExplicit()
// Proves: TPO zones rejected when enableTPO=false, no activeZones insert
// ============================================================================

#include <iostream>
#include <string>
#include "test_sierrachart_mock.h"
#include "amt_core.h"
#include "AMT_config.h"
#include "AMT_Zones.h"
#include "AMT_Bridge.h"

using namespace AMT;

// ============================================================================
// TEST INFRASTRUCTURE
// ============================================================================

static int g_passed = 0;
static int g_failed = 0;

#define TEST_ASSERT(cond, msg) \
    if (!(cond)) { \
        std::cout << "  FAIL: " << msg << std::endl; \
        g_failed++; \
        return false; \
    }

#define TEST_PASSED(name) \
    std::cout << "  PASS: " << name << std::endl; \
    g_passed++; \
    return true;

// ============================================================================
// TEST: TPO zone rejected by posture (DEFENSE-IN-DEPTH)
// ============================================================================
bool test_tpo_rejected_by_posture() {
    std::cout << "\n=== TEST: TPO zone rejected by posture ===" << std::endl;

    // Verify posture: TPO should be disabled
    std::cout << "  Posture: " << g_zonePosture.ToString() << std::endl;
    TEST_ASSERT(!g_zonePosture.enableTPO, "enableTPO should be false");
    TEST_ASSERT(!g_zonePosture.IsZoneTypeAllowed(ZoneType::TPO_POC),
                "TPO_POC should not be allowed");

    // Create ZoneManager
    ZoneManager zm;
    zm.config.tickSize = 0.25;

    // Capture initial state
    const int initialZoneCount = static_cast<int>(zm.activeZones.size());
    const int initialPostureRejections = zm.postureRejections;

    std::cout << "  Initial: activeZones=" << initialZoneCount
              << " postureRejections=" << initialPostureRejections << std::endl;

    // Attempt to create TPO_POC (should be rejected)
    SCDateTime time;
    time.SetToNow();

    std::cout << "  Attempting: CreateZone(TPO_POC, 6100.0)..." << std::endl;
    auto result = zm.CreateZone(ZoneType::TPO_POC, 6100.0, time, 0, true);

    // Verify rejection
    TEST_ASSERT(!result.ok, "TPO_POC creation should fail");
    TEST_ASSERT(result.failure == ZoneCreationFailure::POSTURE_DISALLOWED,
                "Failure reason should be POSTURE_DISALLOWED");
    TEST_ASSERT(result.zoneId == -1, "Zone ID should be -1 (invalid)");

    // Verify no zone was inserted
    TEST_ASSERT(zm.activeZones.size() == 0, "activeZones should remain empty");
    TEST_ASSERT(zm.postureRejections == 1, "postureRejections should be 1");

    // Log the rejection evidence
    std::cout << "  [POSTURE-REJECT] type=TPO_POC(7) failure=POSTURE_DISALLOWED"
              << " zoneId=-1 inserted=false" << std::endl;

    TEST_PASSED("TPO zone rejected by posture");
}

// ============================================================================
// TEST: VBP zone allowed by posture
// ============================================================================
bool test_vbp_allowed_by_posture() {
    std::cout << "\n=== TEST: VBP zone allowed by posture ===" << std::endl;

    // Verify posture: VBP should be enabled
    TEST_ASSERT(g_zonePosture.enableVBP, "enableVBP should be true");
    TEST_ASSERT(g_zonePosture.IsZoneTypeAllowed(ZoneType::VPB_POC),
                "VPB_POC should be allowed");

    // Create ZoneManager
    ZoneManager zm;
    zm.config.tickSize = 0.25;

    // Attempt to create VPB_POC (should succeed)
    SCDateTime time;
    time.SetToNow();

    std::cout << "  Attempting: CreateZone(VPB_POC, 6100.0)..." << std::endl;
    auto result = zm.CreateZone(ZoneType::VPB_POC, 6100.0, time, 0, true);

    // Verify success
    TEST_ASSERT(result.ok, "VPB_POC creation should succeed");
    TEST_ASSERT(result.failure == ZoneCreationFailure::NONE,
                "Failure reason should be NONE");
    TEST_ASSERT(result.zoneId >= 1, "Zone ID should be valid (>=1)");

    // Verify zone was inserted
    TEST_ASSERT(zm.activeZones.size() == 1, "activeZones should have 1 zone");
    TEST_ASSERT(zm.postureRejections == 0, "postureRejections should be 0");

    // Verify zone properties
    auto* zone = zm.GetZone(result.zoneId);
    TEST_ASSERT(zone != nullptr, "Zone should exist");
    TEST_ASSERT(zone->type == ZoneType::VPB_POC, "Zone type should be VPB_POC");

    std::cout << "  [ZONE-CREATED] id=" << result.zoneId
              << " type=VPB_POC(1) price=6100.0" << std::endl;

    TEST_PASSED("VBP zone allowed by posture");
}

// ============================================================================
// TEST: Structure zone rejected (createStructureZones=false)
// ============================================================================
bool test_structure_zone_rejected() {
    std::cout << "\n=== TEST: Structure zone rejected (track-only mode) ===" << std::endl;

    // Verify posture: structure tracking enabled but zone creation disabled
    TEST_ASSERT(g_zonePosture.enableStructure, "enableStructure should be true");
    TEST_ASSERT(!g_zonePosture.createStructureZones, "createStructureZones should be false");
    TEST_ASSERT(!g_zonePosture.IsZoneTypeAllowed(ZoneType::SESSION_HIGH),
                "SESSION_HIGH should not be allowed");

    // Create ZoneManager
    ZoneManager zm;
    zm.config.tickSize = 0.25;

    // Attempt to create SESSION_HIGH (should be rejected)
    SCDateTime time;
    time.SetToNow();

    std::cout << "  Attempting: CreateZone(SESSION_HIGH, 6150.0)..." << std::endl;
    auto result = zm.CreateZone(ZoneType::SESSION_HIGH, 6150.0, time, 0, true);

    // Verify rejection
    TEST_ASSERT(!result.ok, "SESSION_HIGH creation should fail");
    TEST_ASSERT(result.failure == ZoneCreationFailure::POSTURE_DISALLOWED,
                "Failure reason should be POSTURE_DISALLOWED");

    std::cout << "  [POSTURE-REJECT] type=SESSION_HIGH(12) failure=POSTURE_DISALLOWED" << std::endl;

    TEST_PASSED("Structure zone rejected (track-only mode)");
}

// ============================================================================
// TEST: ZONE-DUMP shows no TPO zones
// ============================================================================
bool test_zone_dump_no_tpo() {
    std::cout << "\n=== TEST: ZONE-DUMP shows no TPO zones ===" << std::endl;

    // Create ZoneManager with VBP zones only
    ZoneManager zm;
    zm.config.tickSize = 0.25;
    SCDateTime time;
    time.SetToNow();

    // Create allowed zones
    zm.CreateZone(ZoneType::VPB_POC, 6100.0, time, 0, true);
    zm.CreateZone(ZoneType::VPB_VAH, 6120.0, time, 0, true);
    zm.CreateZone(ZoneType::VPB_VAL, 6080.0, time, 0, true);

    // Attempt to create TPO zones (should all be rejected)
    zm.CreateZone(ZoneType::TPO_POC, 6100.0, time, 0, true);
    zm.CreateZone(ZoneType::TPO_VAH, 6120.0, time, 0, true);
    zm.CreateZone(ZoneType::TPO_VAL, 6080.0, time, 0, true);

    // ZONE-DUMP: Enumerate all zones and count TPO
    std::cout << "  [ZONE-DUMP] count=" << zm.activeZones.size() << " |" << std::endl;

    int tpoCount = 0;
    for (const auto& [id, zone] : zm.activeZones) {
        std::string typeName = ZoneTypeToString(zone.type);
        int typeVal = static_cast<int>(zone.type);

        std::cout << "    (id=" << id << ", type=" << typeName << "(" << typeVal << ")"
                  << ", price=" << zone.GetAnchorPrice() << ")" << std::endl;

        if (zone.type == ZoneType::TPO_POC ||
            zone.type == ZoneType::TPO_VAH ||
            zone.type == ZoneType::TPO_VAL) {
            tpoCount++;
        }
    }

    // Verify no TPO zones
    TEST_ASSERT(tpoCount == 0, "TPO count should be 0");
    TEST_ASSERT(zm.activeZones.size() == 3, "Should have exactly 3 VBP zones");
    TEST_ASSERT(zm.postureRejections == 3, "Should have 3 posture rejections (TPO attempts)");

    std::cout << "  [POSTURE-OK] TPO disabled, " << tpoCount << " TPO zones (correct)" << std::endl;
    std::cout << "  postureRejections=" << zm.postureRejections << " (3 TPO attempts rejected)" << std::endl;

    TEST_PASSED("ZONE-DUMP shows no TPO zones");
}

// ============================================================================
// TEST: PRIOR zones created alongside VBP (unambiguous naming)
// ============================================================================
bool test_prior_zones_with_vbp() {
    std::cout << "\n=== TEST: PRIOR zones created alongside VBP ===" << std::endl;

    // Verify posture: PRIOR should be enabled
    TEST_ASSERT(g_zonePosture.enablePrior, "enablePrior should be true");
    TEST_ASSERT(g_zonePosture.IsZoneTypeAllowed(ZoneType::PRIOR_POC),
                "PRIOR_POC should be allowed");

    // Create ZoneManager
    ZoneManager zm;
    zm.config.tickSize = 0.25;
    SCDateTime time;
    time.SetToNow();

    // Create VBP zones (current session)
    auto vbpPocResult = zm.CreateZone(ZoneType::VPB_POC, 6100.0, time, 0, true);
    auto vbpVahResult = zm.CreateZone(ZoneType::VPB_VAH, 6120.0, time, 0, true);
    auto vbpValResult = zm.CreateZone(ZoneType::VPB_VAL, 6080.0, time, 0, true);

    TEST_ASSERT(vbpPocResult.ok, "VPB_POC creation should succeed");
    TEST_ASSERT(vbpVahResult.ok, "VPB_VAH creation should succeed");
    TEST_ASSERT(vbpValResult.ok, "VPB_VAL creation should succeed");

    // Create PRIOR zones (prior session - different prices)
    auto priorPocResult = zm.CreateZone(ZoneType::PRIOR_POC, 6050.0, time, 0, true);
    auto priorVahResult = zm.CreateZone(ZoneType::PRIOR_VAH, 6070.0, time, 0, true);
    auto priorValResult = zm.CreateZone(ZoneType::PRIOR_VAL, 6030.0, time, 0, true);

    TEST_ASSERT(priorPocResult.ok, "PRIOR_POC creation should succeed");
    TEST_ASSERT(priorVahResult.ok, "PRIOR_VAH creation should succeed");
    TEST_ASSERT(priorValResult.ok, "PRIOR_VAL creation should succeed");

    // ZONE-DUMP: Enumerate all zones
    std::cout << "  [ZONE-DUMP] count=" << zm.activeZones.size() << " |" << std::endl;

    int vbpCount = 0;
    int priorCount = 0;
    for (const auto& [id, zone] : zm.activeZones) {
        std::string typeName = ZoneTypeToString(zone.type);
        int typeVal = static_cast<int>(zone.type);

        std::cout << "    (id=" << id << ", type=" << typeName << "(" << typeVal << ")"
                  << ", price=" << zone.GetAnchorPrice() << ")" << std::endl;

        // Count by family
        if (zone.type == ZoneType::VPB_POC ||
            zone.type == ZoneType::VPB_VAH ||
            zone.type == ZoneType::VPB_VAL) {
            vbpCount++;
            // Verify type names are unambiguous (contain VPB_ prefix)
            TEST_ASSERT(typeName.find("VPB_") == 0,
                        "VBP zone type name should start with VPB_");
        }
        if (zone.type == ZoneType::PRIOR_POC ||
            zone.type == ZoneType::PRIOR_VAH ||
            zone.type == ZoneType::PRIOR_VAL) {
            priorCount++;
            // Verify type names are unambiguous (contain PRIOR_ prefix)
            TEST_ASSERT(typeName.find("PRIOR_") == 0,
                        "PRIOR zone type name should start with PRIOR_");
        }
    }

    // Verify counts
    TEST_ASSERT(zm.activeZones.size() == 6, "Should have 6 zones total (3 VBP + 3 PRIOR)");
    TEST_ASSERT(vbpCount == 3, "Should have 3 VBP zones");
    TEST_ASSERT(priorCount == 3, "Should have 3 PRIOR zones");
    TEST_ASSERT(zm.postureRejections == 0, "No posture rejections for allowed types");

    std::cout << "  VBP zones: " << vbpCount << " | PRIOR zones: " << priorCount << std::endl;
    std::cout << "  Type names are unambiguous (VPB_* vs PRIOR_*)" << std::endl;

    TEST_PASSED("PRIOR zones created alongside VBP");
}

// ============================================================================
// TEST: Reset semantics for postureRejections
// ============================================================================
bool test_posture_rejections_reset() {
    std::cout << "\n=== TEST: postureRejections reset on ResetForSession ===" << std::endl;

    ZoneManager zm;
    zm.config.tickSize = 0.25;
    SCDateTime time;
    time.SetToNow();

    // Create some zones and trigger rejections
    zm.CreateZone(ZoneType::VPB_POC, 6100.0, time, 0, true);
    zm.CreateZone(ZoneType::TPO_POC, 6100.0, time, 0, true);  // Rejected
    zm.CreateZone(ZoneType::TPO_VAH, 6120.0, time, 0, true);  // Rejected

    std::cout << "  Before reset: activeZones=" << zm.activeZones.size()
              << " postureRejections=" << zm.postureRejections << std::endl;

    TEST_ASSERT(zm.activeZones.size() == 1, "Should have 1 zone before reset");
    TEST_ASSERT(zm.postureRejections == 2, "Should have 2 posture rejections before reset");

    // Reset for new session
    zm.ResetForSession(0, time);

    std::cout << "  After reset:  activeZones=" << zm.activeZones.size()
              << " postureRejections=" << zm.postureRejections << std::endl;

    TEST_ASSERT(zm.activeZones.size() == 0, "Should have 0 zones after reset");
    TEST_ASSERT(zm.postureRejections == 0, "postureRejections should be 0 after reset");

    TEST_PASSED("postureRejections reset on ResetForSession");
}

// ============================================================================
// TEST: Contract A - selection tolerance equals halo width
// ============================================================================
bool test_selection_tolerance_equals_halo() {
    std::cout << "\n=== TEST: Contract A - selection tolerance equals halo ===" << std::endl;

    ZoneManager zm;
    zm.config.tickSize = 0.25;
    zm.config.baseHaloTicks = 12;  // Set a specific halo width

    // Verify Contract A: GetSelectionTolerance() returns halo width
    int halo = zm.config.GetHaloWidth();
    int selTol = zm.GetSelectionTolerance();

    std::cout << "  config.GetHaloWidth() = " << halo << std::endl;
    std::cout << "  GetSelectionTolerance() = " << selTol << std::endl;

    TEST_ASSERT(selTol == halo, "Selection tolerance must equal halo width (Contract A)");

    // Create a zone
    SCDateTime time;
    time.SetToNow();
    auto result = zm.CreateZone(ZoneType::VPB_POC, 6100.0, time, 0, true);
    TEST_ASSERT(result.ok, "Zone creation should succeed");

    // Test selection at various distances
    double tickSize = 0.25;

    // Price at anchor - should find zone
    ZoneRuntime* found = zm.GetStrongestZoneAtPrice(6100.0, tickSize);
    TEST_ASSERT(found != nullptr, "Should find zone at anchor price");
    std::cout << "  At anchor (dist=0): found=" << (found ? "yes" : "no") << std::endl;

    // Price within halo - should find zone
    double priceInHalo = 6100.0 + (halo - 1) * tickSize;  // 1 tick inside halo
    found = zm.GetStrongestZoneAtPrice(priceInHalo, tickSize);
    TEST_ASSERT(found != nullptr, "Should find zone within halo");
    std::cout << "  Within halo (dist=" << (halo-1) << "): found=" << (found ? "yes" : "no") << std::endl;

    // Price at halo boundary - should find zone
    double priceAtHalo = 6100.0 + halo * tickSize;
    found = zm.GetStrongestZoneAtPrice(priceAtHalo, tickSize);
    TEST_ASSERT(found != nullptr, "Should find zone at halo boundary");
    std::cout << "  At halo (dist=" << halo << "): found=" << (found ? "yes" : "no") << std::endl;

    // Price outside halo - should NOT find zone
    double priceOutside = 6100.0 + (halo + 1) * tickSize;
    found = zm.GetStrongestZoneAtPrice(priceOutside, tickSize);
    TEST_ASSERT(found == nullptr, "Should NOT find zone outside halo");
    std::cout << "  Outside halo (dist=" << (halo+1) << "): found=" << (found ? "yes" : "no") << std::endl;

    std::cout << "  [CONTRACT-A] Selection tolerance = halo width = " << halo << " ticks" << std::endl;

    TEST_PASSED("Contract A - selection tolerance equals halo");
}

// ============================================================================
// MAIN
// ============================================================================
int main() {
    std::cout << "============================================================" << std::endl;
    std::cout << "POSTURE REJECTION TESTS (Defense-in-Depth)" << std::endl;
    std::cout << "============================================================" << std::endl;

    // Run tests
    test_tpo_rejected_by_posture();
    test_vbp_allowed_by_posture();
    test_structure_zone_rejected();
    test_zone_dump_no_tpo();
    test_prior_zones_with_vbp();
    test_posture_rejections_reset();
    test_selection_tolerance_equals_halo();

    // Summary
    std::cout << "\n============================================================" << std::endl;
    std::cout << "SUMMARY: " << g_passed << " passed, " << g_failed << " failed" << std::endl;
    std::cout << "============================================================" << std::endl;

    return g_failed > 0 ? 1 : 0;
}
