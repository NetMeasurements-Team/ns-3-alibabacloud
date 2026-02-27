// SPDX-License-Identifier: GPL-2.0-only
/**
 * @file ub-test.cc
 * @brief Test suite for the unified-bus module
 * 
 * This file contains unit tests for the unified-bus functionality,
 * including basic object creation, configuration, and core features.
 */

#include "ns3/test.h"
#include "ns3/ub-app.h"
#include "ns3/ub-traffic-gen.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/config.h"
#include "ns3/rng-seed-manager.h"
#include "ns3/node-container.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("UbTest");

/**
 * @brief Unified-bus functionality test
 * 
 * Tests basic unified-bus module functionality including:
 * - Object creation and initialization
 * - Singleton pattern verification
 * - Configuration system integration
 * - Basic API functionality
 */
class UbFunctionalityTest : public TestCase
{
public:
    UbFunctionalityTest();
    void DoRun() override;
    
private:
    void DoSetup() override;
    void DoTeardown() override;
};

UbFunctionalityTest::UbFunctionalityTest()
    : TestCase("UnifiedBus - Core functionality test")
{
}

void UbFunctionalityTest::DoSetup()
{
    Config::Reset();
    RngSeedManager::SetSeed(12345);
}

void UbFunctionalityTest::DoTeardown()
{
    // Minimal cleanup
    if (!Simulator::IsFinished()) {
        Simulator::Destroy();
    }
}

void UbFunctionalityTest::DoRun()
{
    NS_LOG_FUNCTION(this);
    
    // Test 1: UbTrafficGen singleton
    UbTrafficGen& gen1 = UbTrafficGen::GetInstance();
    UbTrafficGen& gen2 = UbTrafficGen::GetInstance();
    NS_TEST_ASSERT_MSG_EQ(&gen1, &gen2, "UbTrafficGen should be singleton");
    
    // Test 2: Initial state
    NS_TEST_ASSERT_MSG_EQ(gen1.IsCompleted(), true, "UbTrafficGen should be completed initially");
    
    // Test 3: UbApp creation
    Ptr<UbApp> app = CreateObject<UbApp>();
    NS_TEST_ASSERT_MSG_NE(app, nullptr, "UbApp creation should succeed");
    
    // Test 4: Node creation
    NodeContainer nodes;
    nodes.Create(2);
    NS_TEST_ASSERT_MSG_EQ(nodes.GetN(), 2, "Should create 2 nodes");
    
    // Test 5: Configuration setting (without getting)
    Config::SetDefault("ns3::UbApp::EnableMultiPath", BooleanValue(false));
    Config::SetDefault("ns3::UbPort::UbDataRate", StringValue("400Gbps"));
    
    NS_LOG_INFO("All basic tests completed successfully");
}

/**
 * @brief Unified-bus test suite
 */
class UbTestSuite : public TestSuite
{
public:
    UbTestSuite();
};

UbTestSuite::UbTestSuite()
    : TestSuite("unified-bus", Type::UNIT)
{
    AddTestCase(new UbFunctionalityTest(), TestCase::Duration::QUICK);
}

// Register the test suite
static UbTestSuite g_ubTestSuite;