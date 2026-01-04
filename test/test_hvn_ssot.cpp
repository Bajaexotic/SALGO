// Test file for HVN/LVN SSOT refactor verification
// Compile: g++ -std=c++17 -I. test_hvn_ssot.cpp -o test_hvn_ssot.exe

#include "amt_core.h"
#include "AMT_config.h"
#include <iostream>
#include <vector>
#include <cassert>

using namespace AMT;

// Test VolumeThresholds SSOT
void TestVolumeThresholds() {
    std::cout << "Testing VolumeThresholds..." << std::endl;

    VolumeThresholds thresholds;

    // Initialize with test data
    thresholds.mean = 1000.0;
    thresholds.stddev = 200.0;
    thresholds.hvnThreshold = thresholds.mean + 1.5 * thresholds.stddev;  // 1300
    thresholds.lvnThreshold = thresholds.mean - 0.5 * thresholds.stddev;  // 900
    thresholds.sampleSize = 50;
    thresholds.totalVolume = 50000.0;
    thresholds.valid = true;
    thresholds.computedAtBar = 100;

    // Test classification
    assert(thresholds.ClassifyVolume(1500.0) == VAPDensityClass::HIGH);  // Above hvnThreshold
    assert(thresholds.ClassifyVolume(1000.0) == VAPDensityClass::NORMAL); // Between
    assert(thresholds.ClassifyVolume(800.0) == VAPDensityClass::LOW);     // Below lvnThreshold
    assert(thresholds.ClassifyVolume(0.0) == VAPDensityClass::NORMAL);    // Zero (not LOW)

    // Test refresh check
    assert(thresholds.NeedsRefresh(100, 25) == false);  // Same bar
    assert(thresholds.NeedsRefresh(124, 25) == false);  // 24 bars later
    assert(thresholds.NeedsRefresh(125, 25) == true);   // 25 bars later (refresh needed)

    std::cout << "  VolumeThresholds: PASS" << std::endl;
}

// Test orthogonal types
void TestOrthogonalTypes() {
    std::cout << "Testing Orthogonal Types..." << std::endl;

    // Test VolumeNodeClassification
    VolumeNodeClassification hvnResponsive;
    hvnResponsive.density = VAPDensityClass::HIGH;
    hvnResponsive.intent = FlowIntent::RESPONSIVE;
    hvnResponsive.flags = NodeFlags::NONE;

    assert(hvnResponsive.IsHVN() == true);
    assert(hvnResponsive.IsLVN() == false);
    assert(hvnResponsive.ToLegacyType() == VolumeNodeType::HVN_RESPONSIVE);

    VolumeNodeClassification lvnSinglePrint;
    lvnSinglePrint.density = VAPDensityClass::LOW;
    lvnSinglePrint.intent = FlowIntent::NEUTRAL;
    lvnSinglePrint.flags = NodeFlags::SINGLE_PRINT;

    assert(lvnSinglePrint.IsLVN() == true);
    assert(lvnSinglePrint.IsSinglePrint() == true);
    assert(lvnSinglePrint.ToLegacyType() == VolumeNodeType::LVN_SINGLE_PRINT);

    // Test NodeFlags bitwise operations
    NodeFlags combined = NodeFlags::PLATEAU | NodeFlags::CLUSTER_PEAK;
    assert(HasFlag(combined, NodeFlags::PLATEAU) == true);
    assert(HasFlag(combined, NodeFlags::CLUSTER_PEAK) == true);
    assert(HasFlag(combined, NodeFlags::SINGLE_PRINT) == false);

    std::cout << "  Orthogonal Types: PASS" << std::endl;
}

// Test VolumeCluster
void TestVolumeClusters() {
    std::cout << "Testing VolumeCluster..." << std::endl;

    VolumeCluster cluster;
    cluster.lowPrice = 5918.00;
    cluster.highPrice = 5920.00;
    cluster.peakPrice = 5919.00;
    cluster.peakVolume = 1500.0;
    cluster.widthTicks = 8;  // (5920 - 5918) / 0.25 = 8 ticks
    cluster.density = VAPDensityClass::HIGH;
    cluster.flags = NodeFlags::CLUSTER_PEAK;

    double tickSize = 0.25;
    assert(cluster.Contains(5919.00, tickSize) == true);
    assert(cluster.Contains(5917.50, tickSize) == false);
    assert(cluster.GetCenter() == 5919.00);

    std::cout << "  VolumeCluster: PASS" << std::endl;
}

// Test PriorSessionNode
void TestPriorSessionNode() {
    std::cout << "Testing PriorSessionNode..." << std::endl;

    PriorSessionNode node;
    node.price = 5920.00;
    node.density = VAPDensityClass::HIGH;
    node.strengthAtClose = 1.0;
    node.touchCount = 3;
    node.sessionAge = 1;

    // Test decay function
    double relevance0 = node.GetRelevance(0);
    double relevance500 = node.GetRelevance(500);
    double relevance1000 = node.GetRelevance(1000);

    assert(relevance0 > relevance500);
    assert(relevance500 > relevance1000);
    assert(std::abs(relevance0 - 1.0) < 0.001);  // Should be ~1.0 at bar 0
    assert(std::abs(relevance500 - 0.3679) < 0.01);  // e^(-1) ≈ 0.368

    std::cout << "  PriorSessionNode: PASS" << std::endl;
}

// Test ZoneConfig new fields
void TestZoneConfigNewFields() {
    std::cout << "Testing ZoneConfig new fields..." << std::endl;

    ZoneConfig cfg;

    // Verify new SSOT fields exist with defaults
    assert(cfg.hvnSigmaCoeff == 1.5);
    assert(cfg.lvnSigmaCoeff == 0.5);
    assert(cfg.minProfileLevels == 10);
    assert(cfg.hvnLvnRefreshIntervalBars == 25);
    assert(cfg.hvnConfirmationBars == 3);
    assert(cfg.hvnDemotionBars == 5);
    assert(cfg.maxClusterGapTicks == 2);

    // Legacy fields still exist
    assert(cfg.hvnThreshold == 1.5);
    assert(cfg.lvnThreshold == 0.5);

    std::cout << "  ZoneConfig: PASS" << std::endl;
}

int main() {
    std::cout << "\n=== HVN/LVN SSOT Refactor Tests ===" << std::endl;

    TestVolumeThresholds();
    TestOrthogonalTypes();
    TestVolumeClusters();
    TestPriorSessionNode();
    TestZoneConfigNewFields();

    std::cout << "\n=== ALL TESTS PASSED ===" << std::endl;
    return 0;
}
