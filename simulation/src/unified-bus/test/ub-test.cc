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
#include "ns3/ub-utils.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-congestion-control.h"
#include "ns3/data-rate.h"
#include "ns3/ub-dcqcn.h"
#include "ns3/ub-flow-control.h"
#include "ns3/ub-function.h"
#include "ns3/ub-link.h"
#include "ns3/ub-port.h"
#include "ns3/ub-queue-manager.h"
#include "ns3/ub-routing-process.h"
#include "ns3/ub-sliding-bitmap-window.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-tag.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-datalink.h"

#include <chrono>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("UbTest");

namespace {

constexpr uint32_t kUrmaWriteRegressionJettyNum = 0;
constexpr uint32_t kUrmaWriteRegressionTaskId = 9001;
constexpr uint32_t kUrmaReadRegressionJettyNum = 1;
constexpr uint32_t kUrmaReadRegressionTaskId = 9002;
constexpr uint32_t kUrmaReadMultiPacketTaskId = 9003;
constexpr uint32_t kUrmaWriteRegressionSenderTpn = 101;
constexpr uint32_t kUrmaWriteRegressionReceiverTpn = 202;
constexpr auto kUrmaWriteRegressionPriority = UB_PRIORITY_DEFAULT;

struct LocalTpTopology
{
    Ptr<Node> sender;
    Ptr<Node> switch0;
    Ptr<Node> switch1;
    Ptr<Node> receiver;
    Ptr<UbPort> senderPort;
    Ptr<UbPort> switch0DevicePort;
    Ptr<UbPort> switch0CorePort;
    Ptr<UbPort> switch1CorePort;
    Ptr<UbPort> switch1DevicePort;
    Ptr<UbPort> receiverPort;
};

Ptr<UbPort>
CreatePort(Ptr<Node> node)
{
    Ptr<UbPort> port = CreateObject<UbPort>();
    port->SetAddress(Mac48Address::Allocate());
    node->AddDevice(port);
    return port;
}

void
InitNode(Ptr<Node> node, UbNodeType_t nodeType, uint32_t portCount)
{
    Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
    node->AggregateObject(sw);
    sw->SetNodeType(nodeType);

    if (nodeType == UB_DEVICE)
    {
        Ptr<UbController> controller = CreateObject<UbController>();
        node->AggregateObject(controller);
        controller->CreateUbFunction();
        controller->CreateUbTransaction();
    }

    for (uint32_t index = 0; index < portCount; ++index)
    {
        CreatePort(node);
    }

    sw->Init();
    Ptr<UbCongestionControl> congestionCtrl = UbCongestionControl::Create(UB_SWITCH);
    congestionCtrl->OnSwitchAttached(sw);
}

void
AddShortestRoute(Ptr<Node> node, uint32_t destNodeId, uint32_t destPortId, uint16_t outPort)
{
    std::vector<uint16_t> outPorts = {outPort};
    Ptr<UbRoutingProcess> routing = node->GetObject<UbSwitch>()->GetRoutingProcess();
    routing->AddShortestRoute(NodeIdToIp(destNodeId).Get(), outPorts);
    routing->AddShortestRoute(NodeIdToIp(destNodeId, destPortId).Get(), outPorts);
}

LocalTpTopology
BuildLocalTpTopology(bool addReverseRoutes = true)
{
    LocalTpTopology topo;
    topo.sender = CreateObject<Node>(0);
    topo.switch0 = CreateObject<Node>(0);
    topo.switch1 = CreateObject<Node>(0);
    topo.receiver = CreateObject<Node>(0);

    InitNode(topo.sender, UB_DEVICE, 1);
    InitNode(topo.switch0, UB_SWITCH, 2);
    InitNode(topo.switch1, UB_SWITCH, 2);
    InitNode(topo.receiver, UB_DEVICE, 1);

    topo.senderPort = DynamicCast<UbPort>(topo.sender->GetDevice(0));
    topo.switch0DevicePort = DynamicCast<UbPort>(topo.switch0->GetDevice(0));
    topo.switch0CorePort = DynamicCast<UbPort>(topo.switch0->GetDevice(1));
    topo.switch1CorePort = DynamicCast<UbPort>(topo.switch1->GetDevice(0));
    topo.switch1DevicePort = DynamicCast<UbPort>(topo.switch1->GetDevice(1));
    topo.receiverPort = DynamicCast<UbPort>(topo.receiver->GetDevice(0));

    Ptr<UbLink> leftLink = CreateObject<UbLink>();
    topo.senderPort->Attach(leftLink);
    topo.switch0DevicePort->Attach(leftLink);

    Ptr<UbLink> coreLink = CreateObject<UbLink>();
    topo.switch0CorePort->Attach(coreLink);
    topo.switch1CorePort->Attach(coreLink);

    Ptr<UbLink> rightLink = CreateObject<UbLink>();
    topo.switch1DevicePort->Attach(rightLink);
    topo.receiverPort->Attach(rightLink);

    AddShortestRoute(topo.sender,
                     topo.receiver->GetId(),
                     topo.receiverPort->GetIfIndex(),
                     topo.senderPort->GetIfIndex());
    AddShortestRoute(topo.switch0,
                     topo.receiver->GetId(),
                     topo.receiverPort->GetIfIndex(),
                     topo.switch0CorePort->GetIfIndex());
    AddShortestRoute(topo.switch1,
                     topo.receiver->GetId(),
                     topo.receiverPort->GetIfIndex(),
                     topo.switch1DevicePort->GetIfIndex());

    if (addReverseRoutes)
    {
        AddShortestRoute(topo.receiver,
                         topo.sender->GetId(),
                         topo.senderPort->GetIfIndex(),
                         topo.receiverPort->GetIfIndex());
        AddShortestRoute(topo.switch1,
                         topo.sender->GetId(),
                         topo.senderPort->GetIfIndex(),
                         topo.switch1CorePort->GetIfIndex());
        AddShortestRoute(topo.switch0,
                         topo.sender->GetId(),
                         topo.senderPort->GetIfIndex(),
                         topo.switch0DevicePort->GetIfIndex());
    }

    return topo;
}

void
InstallStaticTpPair(const LocalTpTopology& topo)
{
    Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
    Ptr<UbController> receiverCtrl = topo.receiver->GetObject<UbController>();

    if (!senderCtrl->IsTPExists(kUrmaWriteRegressionSenderTpn))
    {
        Ptr<UbCongestionControl> senderCc = UbCongestionControl::Create(UB_DEVICE);
        senderCtrl->CreateTp(topo.sender->GetId(),
                             topo.receiver->GetId(),
                             topo.senderPort->GetIfIndex(),
                             topo.receiverPort->GetIfIndex(),
                             kUrmaWriteRegressionPriority,
                             kUrmaWriteRegressionSenderTpn,
                             kUrmaWriteRegressionReceiverTpn,
                             senderCc);
    }

    if (!receiverCtrl->IsTPExists(kUrmaWriteRegressionReceiverTpn))
    {
        Ptr<UbCongestionControl> receiverCc = UbCongestionControl::Create(UB_DEVICE);
        receiverCtrl->CreateTp(topo.receiver->GetId(),
                               topo.sender->GetId(),
                               topo.receiverPort->GetIfIndex(),
                               topo.senderPort->GetIfIndex(),
                               kUrmaWriteRegressionPriority,
                               kUrmaWriteRegressionReceiverTpn,
                               kUrmaWriteRegressionSenderTpn,
                               receiverCc);
    }
}

} // namespace

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

class UbDcqcnFactoryCreatesHostAndSwitchTest : public TestCase
{
public:
    UbDcqcnFactoryCreatesHostAndSwitchTest()
        : TestCase("UnifiedBus - DCQCN factory creates host and switch implementations")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));

        Ptr<UbCongestionControl> host = UbCongestionControl::Create(UB_DEVICE);
        Ptr<UbCongestionControl> sw = UbCongestionControl::Create(UB_SWITCH);

        NS_TEST_ASSERT_MSG_NE(host, nullptr, "Host DCQCN factory should return an object");
        NS_TEST_ASSERT_MSG_NE(sw, nullptr, "Switch DCQCN factory should return an object");
        NS_TEST_ASSERT_MSG_EQ(host->GetCongestionAlgo(),
                              DCQCN,
                              "Host object should report DCQCN");
        NS_TEST_ASSERT_MSG_EQ(sw->GetCongestionAlgo(),
                              DCQCN,
                              "Switch object should report DCQCN");
    }
};

class UbDcqcnHostAckCeTphDefaultTest : public TestCase
{
public:
    UbDcqcnHostAckCeTphDefaultTest()
        : TestCase("UnifiedBus - DCQCN host default ACK CETPH generation does not throw")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));

        Ptr<UbCongestionControl> host = UbCongestionControl::Create(UB_DEVICE);
        NS_TEST_ASSERT_MSG_NE(host, nullptr, "Host DCQCN factory should return an object");

        bool exceptionThrown = false;
        UbCongestionExtTph cetph;
        try
        {
            cetph = host->OnReceiverPrepareAckCongestionHeader(10, 12);
        }
        catch (const std::runtime_error&)
        {
            exceptionThrown = true;
        }

        NS_TEST_ASSERT_MSG_EQ(exceptionThrown,
                              false,
                              "DCQCN host should not throw when generating a default ACK CETPH");
        NS_TEST_ASSERT_MSG_EQ(cetph.GetAckSequence(), 0u, "Default ACK sequence should be zero");
        NS_TEST_ASSERT_MSG_EQ(cetph.GetC(), 0u, "Default C field should be zero");
        NS_TEST_ASSERT_MSG_EQ(cetph.GetI(), false, "Default I field should be false");
        NS_TEST_ASSERT_MSG_EQ(cetph.GetHint(), 0u, "Default hint should be zero");
    }
};

class UbCongestionControlDisabledFactoryStillReturnsObjectTest : public TestCase
{
public:
    UbCongestionControlDisabledFactoryStillReturnsObjectTest()
        : TestCase("UnifiedBus - disabled congestion control factory still returns usable objects")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));

        Ptr<UbCongestionControl> host = UbCongestionControl::Create(UB_DEVICE);
        Ptr<UbCongestionControl> sw = UbCongestionControl::Create(UB_SWITCH);

        NS_TEST_ASSERT_MSG_NE(host, nullptr, "Disabled host congestion control should still return an object");
        NS_TEST_ASSERT_MSG_NE(sw, nullptr, "Disabled switch congestion control should still return an object");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint32_t>(host->GetAckOpcode()),
                              static_cast<uint32_t>(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH),
                              "Disabled congestion control should use ACK without CETPH");
        NS_TEST_ASSERT_MSG_EQ(host->IsCcLimited(4096),
                              false,
                              "Disabled congestion control should never block transport progress");

        bool exceptionThrown = false;
        UbCongestionExtTph cetph;
        try
        {
            cetph = host->OnReceiverPrepareAckCongestionHeader(1, 2);
        }
        catch (const std::runtime_error&)
        {
            exceptionThrown = true;
        }

        NS_TEST_ASSERT_MSG_EQ(exceptionThrown,
                              false,
                              "Disabled congestion control should still provide a no-op ACK congestion header");
        NS_TEST_ASSERT_MSG_EQ(cetph.GetAckSequence(), 0u, "No-op ACK sequence should stay zero");
        NS_TEST_ASSERT_MSG_EQ(cetph.GetC(), 0u, "No-op ACK C field should stay zero");
        NS_TEST_ASSERT_MSG_EQ(cetph.GetI(), false, "No-op ACK I field should stay zero");
        NS_TEST_ASSERT_MSG_EQ(cetph.GetHint(), 0u, "No-op ACK hint should stay zero");
    }
};

class UbCaqmHostRttUsesSmoothedEstimateTest : public TestCase
{
public:
    UbCaqmHostRttUsesSmoothedEstimateTest()
        : TestCase("UnifiedBus - CAQM host RTT estimate uses EWMA instead of latching min RTT")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("CAQM"));
        Config::SetDefault("ns3::UbHostCaqm::RttEwmaGain", DoubleValue(0.125));

        Ptr<UbHostCaqm> host = DynamicCast<UbHostCaqm>(UbCongestionControl::Create(UB_DEVICE));
        NS_TEST_ASSERT_MSG_NE(host, nullptr, "CAQM host factory should return a concrete host object");

        host->ApplyRttSampleForTest(NanoSeconds(100));
        NS_TEST_ASSERT_MSG_EQ(host->GetRttForTest().GetNanoSeconds(),
                              100,
                              "First RTT sample should seed the smoothed RTT estimate");

        host->ApplyRttSampleForTest(NanoSeconds(500));
        NS_TEST_ASSERT_MSG_GT(host->GetRttForTest().GetNanoSeconds(),
                              100,
                              "A larger second RTT sample should increase the smoothed RTT estimate");
        NS_TEST_ASSERT_MSG_LT(host->GetRttForTest().GetNanoSeconds(),
                              500,
                              "EWMA RTT estimate should smooth toward the sample rather than jump to it");

        Config::Reset();
    }
};

class UbSwitchAttachDisabledCongestionControlTest : public TestCase
{
public:
    UbSwitchAttachDisabledCongestionControlTest()
        : TestCase("UnifiedBus - disabled switch congestion control still attaches uniformly")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);
        node->AddDevice(CreateObject<UbPort>());
        sw->Init();

        Ptr<UbCongestionControl> congestionCtrl = UbCongestionControl::Create(UB_SWITCH);
        NS_TEST_ASSERT_MSG_NE(congestionCtrl,
                              nullptr,
                              "Disabled switch congestion control factory should still return an object");

        congestionCtrl->OnSwitchAttached(sw);

        NS_TEST_ASSERT_MSG_EQ(sw->GetCongestionCtrl(),
                              congestionCtrl,
                              "Switch should always keep a congestion-control object after attach");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSwitchAttachEnabledCongestionControlTest : public TestCase
{
public:
    UbSwitchAttachEnabledCongestionControlTest()
        : TestCase("UnifiedBus - enabled switch congestion control attaches via shared base semantics")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);
        node->AddDevice(CreateObject<UbPort>());
        sw->Init();

        Ptr<UbCongestionControl> congestionCtrl = UbCongestionControl::Create(UB_SWITCH);
        NS_TEST_ASSERT_MSG_NE(congestionCtrl,
                              nullptr,
                              "Enabled switch congestion control factory should return an object");

        congestionCtrl->OnSwitchAttached(sw);

        NS_TEST_ASSERT_MSG_EQ(sw->GetCongestionCtrl(),
                              congestionCtrl,
                              "Shared base attach semantics should bind the switch to the concrete congestion-control object");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnCnpHeaderRoundTripTest : public TestCase
{
public:
    UbDcqcnCnpHeaderRoundTripTest()
        : TestCase("UnifiedBus - DCQCN CNP header round-trips ECN/location in spec layout")
    {
    }

    void DoRun() override
    {
        UbCnpExtTph cnpOut;
        cnpOut.SetEcn(0x3);
        cnpOut.SetLocation(true);

        Ptr<Packet> packet = Create<Packet>();
        packet->AddHeader(cnpOut);

        NS_TEST_ASSERT_MSG_EQ(packet->GetSize(),
                              16u,
                              "Spec-aligned CNP CETPH should occupy 16 bytes");

        uint8_t serialized[16] = {};
        packet->CopyData(serialized, sizeof(serialized));
        const uint32_t word0 = (static_cast<uint32_t>(serialized[0]) << 24) |
                               (static_cast<uint32_t>(serialized[1]) << 16) |
                               (static_cast<uint32_t>(serialized[2]) << 8) |
                               static_cast<uint32_t>(serialized[3]);
        NS_TEST_ASSERT_MSG_EQ(word0,
                              0xE0000000u,
                              "CNP ECN/location bits should live in the first serialized word");
        for (uint32_t i = 4; i < sizeof(serialized); ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(serialized[i],
                                  0u,
                                  "Remaining spec-reserved CNP CETPH bytes should serialize as zero");
        }

        UbCnpExtTph cnpIn;
        packet->RemoveHeader(cnpIn);
        NS_TEST_ASSERT_MSG_EQ(cnpIn.GetEcn(), 0x3u, "ECN bits should survive CNP header serialization");
        NS_TEST_ASSERT_MSG_EQ(cnpIn.GetLocation(),
                              true,
                              "Location bit should survive CNP header serialization");
    }
};

class UbDcqcnSwitchMarksFecnTest : public TestCase
{
public:
    UbDcqcnSwitchMarksFecnTest()
        : TestCase("UnifiedBus - DCQCN switch marks FECN when packet enters congested outport backlog")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbSwitchDcqcn::KminBytes", UintegerValue(1024));
        Config::SetDefault("ns3::UbSwitchDcqcn::KmaxBytes", UintegerValue(2048));
        Config::SetDefault("ns3::UbSwitchDcqcn::Pmax", DoubleValue(1.0));

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbSwitch> sw = topo.switch0->GetObject<UbSwitch>();
        Ptr<Packet> packet = Create<Packet>(4096);
        UbIpBasedNetworkHeader nth;
        nth.SetMode(0);
        nth.SetC(0);
        nth.SetI(0);
        nth.SetHint(0);
        packet->AddHeader(nth);

        UbDatalinkPacketHeader dl;
        dl.SetConfig(static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_IPV4));
        dl.SetPacketVL(1);
        packet->AddHeader(dl);

        sw->SendPacket(packet,
                       topo.switch0DevicePort->GetIfIndex(),
                       topo.switch0CorePort->GetIfIndex(),
                       1);

        packet->RemoveHeader(dl);
        packet->RemoveHeader(nth);
        NS_TEST_ASSERT_MSG_EQ(nth.GetMode(), 0b100, "DCQCN should switch NTH to FECN mode");
        NS_TEST_ASSERT_MSG_NE(nth.GetFecn(),
                              0u,
                              "DCQCN should mark packet as soon as it enters congested outport backlog");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnSwitchNoMarkPreservesPacketHeadersTest : public TestCase
{
public:
    UbDcqcnSwitchNoMarkPreservesPacketHeadersTest()
        : TestCase("UnifiedBus - DCQCN switch preserves headers when enqueue does not mark")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbSwitchDcqcn::KminBytes", UintegerValue(0));
        Config::SetDefault("ns3::UbSwitchDcqcn::KmaxBytes", UintegerValue(1));
        Config::SetDefault("ns3::UbSwitchDcqcn::Pmax", DoubleValue(0.0));

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbSwitch> sw = topo.switch0->GetObject<UbSwitch>();
        Ptr<Packet> packet = Create<Packet>(4096);

        UbIpBasedNetworkHeader nth;
        nth.SetMode(0);
        nth.SetLocation(true);
        nth.SetC(0x2a);
        nth.SetI(1);
        nth.SetHint(0x5b);
        packet->AddHeader(nth);

        UbDatalinkPacketHeader dl;
        dl.SetConfig(static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_IPV4));
        dl.SetPacketVL(3);
        dl.SetLoadBalanceMode(true);
        dl.SetRoutingPolicy(true);
        packet->AddHeader(dl);

        const uint32_t originalSize = packet->GetSize();
        std::vector<uint8_t> originalBytes(originalSize);
        packet->CopyData(originalBytes.data(), originalBytes.size());

        sw->SendPacket(packet,
                       topo.switch0DevicePort->GetIfIndex(),
                       topo.switch0CorePort->GetIfIndex(),
                       3);

        std::vector<uint8_t> restoredBytes(packet->GetSize());
        packet->CopyData(restoredBytes.data(), restoredBytes.size());
        const bool sameBytes = restoredBytes == originalBytes;
        NS_TEST_ASSERT_MSG_EQ(sameBytes,
                              true,
                              "DCQCN no-mark path should restore packet bytes exactly");

        packet->RemoveHeader(dl);
        packet->RemoveHeader(nth);
        NS_TEST_ASSERT_MSG_EQ(dl.GetConfig(),
                              static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_IPV4),
                              "DCQCN no-mark path should preserve datalink config");
        NS_TEST_ASSERT_MSG_EQ(dl.GetPacketVL(), 3u, "DCQCN no-mark path should preserve packet VL");
        NS_TEST_ASSERT_MSG_EQ(dl.GetLoadBalanceMode(),
                              true,
                              "DCQCN no-mark path should preserve load-balance mode");
        NS_TEST_ASSERT_MSG_EQ(dl.GetRoutingPolicy(),
                              true,
                              "DCQCN no-mark path should preserve routing policy");
        NS_TEST_ASSERT_MSG_EQ(nth.GetMode(), 0u, "DCQCN no-mark path should preserve NTH mode");
        NS_TEST_ASSERT_MSG_EQ(nth.GetLocation(),
                              true,
                              "DCQCN no-mark path should preserve NTH location");
        NS_TEST_ASSERT_MSG_EQ(nth.GetC(), 1u, "DCQCN no-mark path should preserve NTH C field");
        NS_TEST_ASSERT_MSG_EQ(nth.GetI(), 1u, "DCQCN no-mark path should preserve NTH I field");
        NS_TEST_ASSERT_MSG_EQ(nth.GetHint(), 0x5bu, "DCQCN no-mark path should preserve NTH hint");
        NS_TEST_ASSERT_MSG_EQ(packet->GetSize(),
                              4096u,
                              "DCQCN no-mark path should preserve payload size");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnSwitchDoesNotRemarkMarkedFecnTest : public TestCase
{
public:
    UbDcqcnSwitchDoesNotRemarkMarkedFecnTest()
        : TestCase("UnifiedBus - DCQCN switch leaves already-marked FECN packets unchanged")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbSwitchDcqcn::KminBytes", UintegerValue(0));
        Config::SetDefault("ns3::UbSwitchDcqcn::KmaxBytes", UintegerValue(1));
        Config::SetDefault("ns3::UbSwitchDcqcn::Pmax", DoubleValue(1.0));

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbSwitch> sw = topo.switch0->GetObject<UbSwitch>();
        Ptr<Packet> packet = Create<Packet>(4096);

        UbIpBasedNetworkHeader nth;
        nth.SetMode(0b100);
        nth.SetLocation(true);
        nth.SetFecn(0x2);
        packet->AddHeader(nth);

        UbDatalinkPacketHeader dl;
        dl.SetConfig(static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_IPV4));
        dl.SetPacketVL(1);
        packet->AddHeader(dl);

        sw->SendPacket(packet,
                       topo.switch0DevicePort->GetIfIndex(),
                       topo.switch0CorePort->GetIfIndex(),
                       1);

        packet->RemoveHeader(dl);
        packet->RemoveHeader(nth);
        NS_TEST_ASSERT_MSG_EQ(nth.GetMode(), 0b100, "Marked packet should stay in FECN mode");
        NS_TEST_ASSERT_MSG_EQ(nth.GetLocation(), true, "Already-marked location bit should not be overwritten");
        NS_TEST_ASSERT_MSG_EQ(nth.GetFecn(), 0x2u, "Already-marked FECN level should not be overwritten");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnReceiverSuppressesBurstCnpTest : public TestCase
{
public:
    UbDcqcnReceiverSuppressesBurstCnpTest()
        : TestCase("UnifiedBus - DCQCN receiver emits at most one CNP per suppression window")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::CnpInterval", TimeValue(MicroSeconds(50)));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> rxTp =
            topo.receiver->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionReceiverTpn);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(rxTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Receiver TP should bind a host DCQCN instance");

        UbIpBasedNetworkHeader marked;
        marked.SetMode(0b100);
        marked.SetFecn(0x1);
        marked.SetLocation(false);

        cc->OnReceiverDataPacketReceived(1, 256, marked);
        cc->OnReceiverDataPacketReceived(2, 256, marked);

        NS_TEST_ASSERT_MSG_EQ(rxTp->GetPendingCnpCountForTest(),
                              1u,
                              "Receiver should queue only one immediate CNP inside suppression window");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnControlPriorityPrefersCnpTest : public TestCase
{
public:
    UbDcqcnControlPriorityPrefersCnpTest()
        : TestCase("UnifiedBus - transport dequeues CNP before ACK")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> tp =
            topo.receiver->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionReceiverTpn);

        Ptr<Packet> ackPacket = Create<Packet>(0);
        UbTransportHeader ackHeader;
        ackHeader.SetTPOpcode(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH);
        ackPacket->AddHeader(ackHeader);
        tp->EnqueueAckForTest(ackPacket);

        Ptr<Packet> cnpPacket = Create<Packet>(0);
        UbTransportHeader cnpHeader;
        cnpHeader.SetTPOpcode(TpOpcode::TP_OPCODE_CNP);
        cnpPacket->AddHeader(cnpHeader);
        tp->EnqueueCnpForTest(cnpPacket);

        Ptr<Packet> first = tp->GetNextPacketForTest();
        NS_TEST_ASSERT_MSG_NE(first, nullptr, "Transport should return queued control work");

        UbTransportHeader tph;
        first->RemoveHeader(tph);
        NS_TEST_ASSERT_MSG_EQ(tph.GetTPOpcode(),
                              static_cast<uint8_t>(TpOpcode::TP_OPCODE_CNP),
                              "CNP should be sent before ACK");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnCnpOpcodeIsValidTransportOpcodeTest : public TestCase
{
public:
    UbDcqcnCnpOpcodeIsValidTransportOpcodeTest()
        : TestCase("UnifiedBus - DCQCN CNP is a valid transport opcode")
    {
    }

    void DoRun() override
    {
        UbTransportHeader header;
        header.SetTPOpcode(TpOpcode::TP_OPCODE_CNP);
        NS_TEST_ASSERT_MSG_EQ(header.IsValidOpcode(), true, "CNP opcode must pass transport header validation");
    }
};

class UbAckWithoutCetphCarriesNoCetphHeaderTest : public TestCase
{
public:
    UbAckWithoutCetphCarriesNoCetphHeaderTest()
        : TestCase("UnifiedBus - ACK_WITHOUT_CETPH wire packet carries no CETPH header")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> rxTp =
            topo.receiver->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionReceiverTpn);

        Ptr<Packet> data = Create<Packet>(16);
        UbFlowTag flowTag(kUrmaWriteRegressionTaskId, 16);
        data->AddPacketTag(flowTag);

        UbMAExtTah maHeader;
        maHeader.SetLength(16);
        data->AddHeader(maHeader);

        UbTransactionHeader taHeader;
        taHeader.SetTaOpcode(TaOpcode::TA_OPCODE_WRITE);
        taHeader.SetIniTaSsn(7);
        taHeader.SetIniRcId(0);
        data->AddHeader(taHeader);

        UbTransportHeader tpHeader;
        tpHeader.SetTPOpcode(TpOpcode::TP_OPCODE_RELIABLE_TA);
        tpHeader.SetSrcTpn(kUrmaWriteRegressionSenderTpn);
        tpHeader.SetDestTpn(kUrmaWriteRegressionReceiverTpn);
        tpHeader.SetPsn(0);
        data->AddHeader(tpHeader);

        UdpHeader udpHeader;
        data->AddHeader(udpHeader);
        UbPort::AddIpv4Header(data, NodeIdToIp(topo.sender->GetId()), NodeIdToIp(topo.receiver->GetId()));

        UbIpBasedNetworkHeader networkHeader;
        data->AddHeader(networkHeader);
        UbDataLink::GenPacketHeader(data,
                                    false,
                                    false,
                                    kUrmaWriteRegressionPriority,
                                    kUrmaWriteRegressionPriority,
                                    false,
                                    true,
                                    UbDatalinkHeaderConfig::PACKET_IPV4);

        rxTp->RecvDataPacket(data);
        Ptr<Packet> ack = rxTp->GetNextPacketForTest();
        NS_TEST_ASSERT_MSG_NE(ack, nullptr, "Receiver should enqueue an ACK packet");

        UbDatalinkPacketHeader ackDlHeader;
        UbIpBasedNetworkHeader ackNetworkHeader;
        Ipv4Header ackIpv4Header;
        UdpHeader ackUdpHeader;
        UbTransportHeader ackTpHeader;
        UbAckTransactionHeader ackTaHeader;

        ack->RemoveHeader(ackDlHeader);
        ack->RemoveHeader(ackNetworkHeader);
        ack->RemoveHeader(ackIpv4Header);
        ack->RemoveHeader(ackUdpHeader);
        ack->RemoveHeader(ackTpHeader);
        NS_TEST_ASSERT_MSG_EQ(ackTpHeader.GetTPOpcode(),
                              static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH),
                              "Disabled congestion control should emit ACK without CETPH");

        ack->RemoveHeader(ackTaHeader);
        NS_TEST_ASSERT_MSG_EQ(ackTaHeader.GetTaOpcode(),
                              static_cast<uint8_t>(TaOpcode::TA_OPCODE_TRANSACTION_ACK),
                              "ACK_WITHOUT_CETPH should expose TA ACK immediately after TP header");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnSenderCutsRateOnCnpTest : public TestCase
{
public:
    UbDcqcnSenderCutsRateOnCnpTest()
        : TestCase("UnifiedBus - DCQCN sender cuts rate and blocks send after CNP")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::LineRate", DataRateValue(DataRate("100Gbps")));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(txTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Sender TP should bind a host DCQCN instance");

        UbCongestionExtTph cnp;
        cnp.SetAckSequence(0);
        cnp.SetRawBytes4to7(static_cast<uint32_t>(0x1U) << 30);

        NS_TEST_ASSERT_MSG_EQ(cc->IsCcLimited(256), false, "Fresh sender should not be blocked");
        cc->OnSenderCongestionNotification(TpOpcode::TP_OPCODE_CNP, 0, cnp);
        cc->OnSenderDataPacketSent(1, 4096);
        NS_TEST_ASSERT_MSG_EQ(cc->IsCcLimited(4096),
                              true,
                              "Sender should become pacing-limited immediately after a CNP cut");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnCnpDoesNotAdvanceAckStateTest : public TestCase
{
public:
    UbDcqcnCnpDoesNotAdvanceAckStateTest()
        : TestCase("UnifiedBus - DCQCN CNP does not advance reliable ACK state")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        uint32_t before = txTp->GetPsnSndUnaForTest();

        Ptr<Packet> cnpPacket = txTp->BuildDcqcnCnp(0x1, false);
        txTp->RecvTpAck(cnpPacket);

        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnSndUnaForTest(),
                              before,
                              "CNP must not advance ACK tracking");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnRecoveryTimerIncreasesRateTest : public TestCase
{
public:
    UbDcqcnRecoveryTimerIncreasesRateTest()
        : TestCase("UnifiedBus - DCQCN recovery timer raises sender rate after CNP cut")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::RateIncreaseTimer", TimeValue(MicroSeconds(55)));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(txTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Sender TP should bind a host DCQCN instance");

        const uint64_t before = cc->GetCurrentRateForTest().GetBitRate();
        cc->ApplySyntheticCnpForTest();

        Simulator::Schedule(MicroSeconds(60), [this, cc, before]() {
            NS_TEST_ASSERT_MSG_GT(cc->GetCurrentRateForTest().GetBitRate(),
                                  before / 2,
                                  "Recovery timer should begin increasing sender rate");
        });

        Simulator::Stop(MicroSeconds(61));
        Simulator::Run();
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnByteCounterIncreasesRateBeforeTimerTest : public TestCase
{
public:
    UbDcqcnByteCounterIncreasesRateBeforeTimerTest()
        : TestCase("UnifiedBus - DCQCN byte counter increases rate before timer expiry")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::RateIncreaseTimer", TimeValue(MilliSeconds(1)));
        Config::SetDefault("ns3::UbHostDcqcn::ByteCounterThreshold", UintegerValue(1024));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(txTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Sender TP should bind a host DCQCN instance");

        cc->ApplySyntheticCnpForTest();
        const uint64_t cutRate = cc->GetCurrentRateForTest().GetBitRate();
        cc->OnSenderDataPacketSent(1, 2048);

        Simulator::Schedule(MicroSeconds(5), [this, cc, cutRate]() {
            NS_TEST_ASSERT_MSG_GT(cc->GetCurrentRateForTest().GetBitRate(),
                                  cutRate,
                                  "Byte counter should increase rate before timer expiry");
        });

        Simulator::Stop(MicroSeconds(6));
        Simulator::Run();
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnHyperIncreaseUsesHaiRateTest : public TestCase
{
public:
    UbDcqcnHyperIncreaseUsesHaiRateTest()
        : TestCase("UnifiedBus - DCQCN hyper increase uses the configured HAI step")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::LineRate", DataRateValue(DataRate("1000Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::InitialRate", DataRateValue(DataRate("250Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::RateIncreaseTimer", TimeValue(MicroSeconds(1)));
        Config::SetDefault("ns3::UbHostDcqcn::ByteCounterThreshold", UintegerValue(1));
        Config::SetDefault("ns3::UbHostDcqcn::RateAi", DataRateValue(DataRate("1Mbps")));
        Config::SetDefault("ns3::UbHostDcqcn::HyperAiRate", DataRateValue(DataRate("100Mbps")));
        Config::SetDefault("ns3::UbHostDcqcn::FastRecoveryLimit", UintegerValue(2));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbController> receiverCtrl = topo.receiver->GetObject<UbController>();
        Ptr<UbTransportChannel> firstTp = senderCtrl->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> firstCc = DynamicCast<UbHostDcqcn>(firstTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(firstCc, nullptr, "First sender TP should bind host DCQCN");
        (void)firstCc->IsCcLimited(1);

        Ptr<UbCongestionControl> senderCc2 = UbCongestionControl::Create(UB_DEVICE);
        senderCtrl->CreateTp(topo.sender->GetId(),
                             topo.receiver->GetId(),
                             topo.senderPort->GetIfIndex(),
                             topo.receiverPort->GetIfIndex(),
                             kUrmaWriteRegressionPriority,
                             303,
                             404,
                             senderCc2);
        Ptr<UbCongestionControl> receiverCc2 = UbCongestionControl::Create(UB_DEVICE);
        receiverCtrl->CreateTp(topo.receiver->GetId(),
                               topo.sender->GetId(),
                               topo.receiverPort->GetIfIndex(),
                               topo.senderPort->GetIfIndex(),
                               kUrmaWriteRegressionPriority,
                               404,
                               303,
                               receiverCc2);

        Ptr<UbTransportChannel> txTp = senderCtrl->GetTpByTpn(303);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(txTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Sender TP should bind a host DCQCN instance");
        (void)cc->IsCcLimited(1);

        DataRate beforeHyper;
        cc->ApplySyntheticCnpForTest();
        cc->OnSenderDataPacketSent(1, 1);
        Simulator::Schedule(MicroSeconds(2), [cc]() { cc->OnSenderDataPacketSent(2, 1); });
        Simulator::Schedule(MicroSeconds(3), [&beforeHyper, cc]() {
            beforeHyper = cc->GetTargetRateForTest();
        });
        Simulator::Schedule(MicroSeconds(4), [cc]() { cc->OnSenderDataPacketSent(3, 1); });
        Simulator::Schedule(MicroSeconds(6), [this, cc, &beforeHyper]() {
            NS_TEST_ASSERT_MSG_GT(cc->GetTargetRateForTest().GetBitRate() - beforeHyper.GetBitRate(),
                                  DataRate("50Mbps").GetBitRate(),
                                  "Hyper increase should use a larger HAI step than additive increase");
        });

        Simulator::Stop(MicroSeconds(7));
        Simulator::Run();
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnRateNeverExceedsLineRateTest : public TestCase
{
public:
    UbDcqcnRateNeverExceedsLineRateTest()
        : TestCase("UnifiedBus - DCQCN sender rate never exceeds configured line rate")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::LineRate", DataRateValue(DataRate("100Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::RateIncreaseTimer", TimeValue(MicroSeconds(1)));
        Config::SetDefault("ns3::UbHostDcqcn::ByteCounterThreshold", UintegerValue(1));
        Config::SetDefault("ns3::UbHostDcqcn::RateAi", DataRateValue(DataRate("100Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::HyperAiRate", DataRateValue(DataRate("100Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::FastRecoveryLimit", UintegerValue(1));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(txTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Sender TP should bind a host DCQCN instance");

        cc->ApplySyntheticCnpForTest();
        for (uint32_t index = 0; index < 8; ++index)
        {
            Simulator::Schedule(MicroSeconds(index + 1),
                                [cc, index]() { cc->OnSenderDataPacketSent(index + 1, 1); });
        }

        Simulator::Schedule(MicroSeconds(12), [this, cc]() {
            NS_TEST_ASSERT_MSG_LT_OR_EQ(cc->GetCurrentRateForTest().GetBitRate(),
                                        DataRate("100Gbps").GetBitRate(),
                                        "Recovery logic must clamp sender rate to line rate");
        });

        Simulator::Stop(MicroSeconds(13));
        Simulator::Run();
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnBusyHostStartsSecondFlowAtInitialRateTest : public TestCase
{
public:
    UbDcqcnBusyHostStartsSecondFlowAtInitialRateTest()
        : TestCase("UnifiedBus - DCQCN busy host starts second flow below line rate")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::LineRate", DataRateValue(DataRate("100Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::InitialRate", DataRateValue(DataRate("25Gbps")));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbController> receiverCtrl = topo.receiver->GetObject<UbController>();
        Ptr<UbTransportChannel> firstTp = senderCtrl->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> firstCc = DynamicCast<UbHostDcqcn>(firstTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(firstCc, nullptr, "First sender TP should bind host DCQCN");

        (void)firstCc->IsCcLimited(1);

        Ptr<UbCongestionControl> senderCc2 = UbCongestionControl::Create(UB_DEVICE);
        senderCtrl->CreateTp(topo.sender->GetId(),
                             topo.receiver->GetId(),
                             topo.senderPort->GetIfIndex(),
                             topo.receiverPort->GetIfIndex(),
                             kUrmaWriteRegressionPriority,
                             303,
                             404,
                             senderCc2);

        Ptr<UbCongestionControl> receiverCc2 = UbCongestionControl::Create(UB_DEVICE);
        receiverCtrl->CreateTp(topo.receiver->GetId(),
                               topo.sender->GetId(),
                               topo.receiverPort->GetIfIndex(),
                               topo.senderPort->GetIfIndex(),
                               kUrmaWriteRegressionPriority,
                               404,
                               303,
                               receiverCc2);

        Ptr<UbTransportChannel> secondTp = senderCtrl->GetTpByTpn(303);
        Ptr<UbHostDcqcn> secondCc = DynamicCast<UbHostDcqcn>(secondTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(secondCc, nullptr, "Second sender TP should bind host DCQCN");

        (void)secondCc->IsCcLimited(1);

        NS_TEST_ASSERT_MSG_EQ(firstCc->GetCurrentRateForTest().GetBitRate(),
                              DataRate("100Gbps").GetBitRate(),
                              "First active flow should start at line rate");
        NS_TEST_ASSERT_MSG_EQ(secondCc->GetCurrentRateForTest().GetBitRate(),
                              DataRate("25Gbps").GetBitRate(),
                              "Busy-host second flow should start at configured initial rate");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnPacingWakeupResumesQueuedSendTest : public TestCase
{
public:
    UbDcqcnPacingWakeupResumesQueuedSendTest()
        : TestCase("UnifiedBus - DCQCN pacing wakeup resumes queued sends without external events")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::LineRate", DataRateValue(DataRate("100Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::InitialRate", DataRateValue(DataRate("100Gbps")));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbFunction> senderFunction = senderCtrl->GetUbFunction();
        Ptr<UbTransaction> senderTransaction = senderCtrl->GetUbTransaction();
        senderFunction->CreateJetty(topo.sender->GetId(),
                                    topo.receiver->GetId(),
                                    kUrmaWriteRegressionJettyNum);
        const std::vector<uint32_t> tpns = {kUrmaWriteRegressionSenderTpn};
        const bool bindOk = senderTransaction->JettyBindTp(topo.sender->GetId(),
                                                           topo.receiver->GetId(),
                                                           kUrmaWriteRegressionJettyNum,
                                                           false,
                                                           tpns);
        NS_TEST_ASSERT_MSG_EQ(bindOk, true, "Sender Jetty should bind to static TP pair");

        Ptr<UbTransportChannel> senderTp = senderCtrl->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(senderTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Sender TP should bind a host DCQCN instance");
        Ptr<UbJetty> jetty = senderFunction->GetJetty(kUrmaWriteRegressionJettyNum);
        NS_TEST_ASSERT_MSG_NE(jetty, nullptr, "Sender Jetty should exist");
        jetty->SetClientCallback(MakeCallback(&UbDcqcnPacingWakeupResumesQueuedSendTest::OnTaskCompleted,
                                              this));

        Ptr<UbWqe> wqe = senderFunction->CreateWqe(topo.sender->GetId(),
                                                   topo.receiver->GetId(),
                                                   64 * 1024,
                                                   kUrmaWriteRegressionTaskId,
                                                   TaOpcode::TA_OPCODE_WRITE);
        Simulator::ScheduleNow(&UbFunction::PushWqeToJetty,
                               senderFunction,
                               wqe,
                               kUrmaWriteRegressionJettyNum);

        Simulator::Schedule(MicroSeconds(2), [cc]() { cc->ApplySyntheticCnpForTest(); });

        uint64_t txBytesAtCut = 0;
        Simulator::Schedule(MicroSeconds(3), [&txBytesAtCut, &topo]() {
            txBytesAtCut = topo.senderPort->GetTxBytes();
        });

        Simulator::Schedule(MicroSeconds(80), [this, &topo, &txBytesAtCut]() {
            NS_TEST_ASSERT_MSG_GT(topo.senderPort->GetTxBytes(),
                                  txBytesAtCut,
                                  "Pacing wakeup should resume sending after a DCQCN cut even without ACK/CNP/PFC follow-up events");
        });

        Simulator::Stop(MicroSeconds(81));
        Simulator::Run();
        Simulator::Destroy();
        Config::Reset();
    }

private:
    void OnTaskCompleted(uint32_t, uint32_t) {}
};

class UbDcqcnCnpCutRescalesOutstandingPacingDebtTest : public TestCase
{
public:
    UbDcqcnCnpCutRescalesOutstandingPacingDebtTest()
        : TestCase("UnifiedBus - DCQCN CNP rescales outstanding pacing debt to the reduced rate")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::LineRate", DataRateValue(DataRate("100Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::InitialRate", DataRateValue(DataRate("100Gbps")));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(txTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Sender TP should bind a host DCQCN instance");

        (void)cc->IsCcLimited(1);
        cc->OnSenderDataPacketSent(1, 4096);

        Simulator::Schedule(NanoSeconds(100), [cc]() { cc->ApplySyntheticCnpForTest(); });
        Simulator::Schedule(NanoSeconds(400), [this, cc]() {
            NS_TEST_ASSERT_MSG_EQ(cc->IsCcLimited(4096),
                                  true,
                                  "CNP cut should stretch outstanding pacing debt to the reduced rate");
        });
        Simulator::Schedule(NanoSeconds(600), [this, cc]() {
            NS_TEST_ASSERT_MSG_EQ(cc->IsCcLimited(4096),
                                  false,
                                  "Sender should become sendable again after the rescaled pacing deadline");
        });

        Simulator::Stop(NanoSeconds(601));
        Simulator::Run();
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnCompletedFlowReleasesHostActiveSlotTest : public TestCase
{
public:
    UbDcqcnCompletedFlowReleasesHostActiveSlotTest()
        : TestCase("UnifiedBus - completed DCQCN flow releases host active-flow slot")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::LineRate", DataRateValue(DataRate("100Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::InitialRate", DataRateValue(DataRate("25Gbps")));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbController> receiverCtrl = topo.receiver->GetObject<UbController>();
        Ptr<UbFunction> senderFunction = senderCtrl->GetUbFunction();
        Ptr<UbTransaction> senderTransaction = senderCtrl->GetUbTransaction();
        senderFunction->CreateJetty(topo.sender->GetId(),
                                    topo.receiver->GetId(),
                                    kUrmaWriteRegressionJettyNum);
        const std::vector<uint32_t> tpns = {kUrmaWriteRegressionSenderTpn};
        const bool bindOk = senderTransaction->JettyBindTp(topo.sender->GetId(),
                                                           topo.receiver->GetId(),
                                                           kUrmaWriteRegressionJettyNum,
                                                           false,
                                                           tpns);
        NS_TEST_ASSERT_MSG_EQ(bindOk, true, "Sender Jetty should bind to static TP pair");

        Ptr<UbJetty> jetty = senderFunction->GetJetty(kUrmaWriteRegressionJettyNum);
        NS_TEST_ASSERT_MSG_NE(jetty, nullptr, "Sender Jetty should exist");
        jetty->SetClientCallback(MakeCallback(&UbDcqcnCompletedFlowReleasesHostActiveSlotTest::OnTaskCompleted,
                                              this));

        Ptr<UbWqe> wqe = senderFunction->CreateWqe(topo.sender->GetId(),
                                                   topo.receiver->GetId(),
                                                   4 * 1024,
                                                   kUrmaWriteRegressionTaskId,
                                                   TaOpcode::TA_OPCODE_WRITE);
        Simulator::ScheduleNow(&UbFunction::PushWqeToJetty,
                               senderFunction,
                               wqe,
                               kUrmaWriteRegressionJettyNum);

        Simulator::Stop(MicroSeconds(200));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(m_taskCompleted,
                              true,
                              "Baseline write should complete before checking host active-flow accounting");
        NS_TEST_ASSERT_MSG_EQ(senderCtrl->GetActiveSenderFlowCount(),
                              0u,
                              "Completed flow should release the host active-flow slot");

        Ptr<UbCongestionControl> senderCc2 = UbCongestionControl::Create(UB_DEVICE);
        senderCtrl->CreateTp(topo.sender->GetId(),
                             topo.receiver->GetId(),
                             topo.senderPort->GetIfIndex(),
                             topo.receiverPort->GetIfIndex(),
                             kUrmaWriteRegressionPriority,
                             303,
                             404,
                             senderCc2);
        Ptr<UbCongestionControl> receiverCc2 = UbCongestionControl::Create(UB_DEVICE);
        receiverCtrl->CreateTp(topo.receiver->GetId(),
                               topo.sender->GetId(),
                               topo.receiverPort->GetIfIndex(),
                               topo.senderPort->GetIfIndex(),
                               kUrmaWriteRegressionPriority,
                               404,
                               303,
                               receiverCc2);

        Ptr<UbTransportChannel> secondTp = senderCtrl->GetTpByTpn(303);
        Ptr<UbHostDcqcn> secondCc = DynamicCast<UbHostDcqcn>(secondTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(secondCc, nullptr, "Second sender TP should bind a host DCQCN instance");
        (void)secondCc->IsCcLimited(1);

        NS_TEST_ASSERT_MSG_EQ(secondCc->GetCurrentRateForTest().GetBitRate(),
                              DataRate("100Gbps").GetBitRate(),
                              "A host with no active flows should restart new flows at line rate");

        Simulator::Destroy();
        Config::Reset();
    }

private:
    void OnTaskCompleted(uint32_t, uint32_t)
    {
        m_taskCompleted = true;
    }

    bool m_taskCompleted{false};
};

class UbDcqcnIdleFlowCancelsRecoveryStateTest : public TestCase
{
public:
    UbDcqcnIdleFlowCancelsRecoveryStateTest()
        : TestCase("UnifiedBus - idle DCQCN sender cancels recovery timers and stays inactive")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::LineRate", DataRateValue(DataRate("100Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::InitialRate", DataRateValue(DataRate("25Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::RateIncreaseTimer", TimeValue(MicroSeconds(55)));
        Config::SetDefault("ns3::UbHostDcqcn::AlphaUpdateInterval", TimeValue(MicroSeconds(55)));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbTransportChannel> txTp = senderCtrl->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(txTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Sender TP should bind a host DCQCN instance");

        (void)cc->IsCcLimited(1);
        NS_TEST_ASSERT_MSG_EQ(senderCtrl->GetActiveSenderFlowCount(),
                              1u,
                              "Starting sender should register one active host flow");

        cc->ApplySyntheticCnpForTest();
        cc->OnSenderTransportIdle();

        NS_TEST_ASSERT_MSG_EQ(senderCtrl->GetActiveSenderFlowCount(),
                              0u,
                              "Idle sender should release host active-flow slot immediately");

        Simulator::Schedule(MicroSeconds(120), [this, senderCtrl, cc]() {
            NS_TEST_ASSERT_MSG_EQ(senderCtrl->GetActiveSenderFlowCount(),
                                  0u,
                                  "Idle sender must stay inactive after old DCQCN recovery timers would have fired");
            NS_TEST_ASSERT_MSG_EQ(cc->GetCurrentRateForTest().GetBitRate(),
                                  DataRate("25Gbps").GetBitRate(),
                                  "Idle sender should keep fresh baseline rate until a new flow starts");
        });

        Simulator::Stop(MicroSeconds(121));
        Simulator::Run();
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbUrmaReadWqeMetadataPropagationTest : public TestCase
{
  public:
    UbUrmaReadWqeMetadataPropagationTest()
        : TestCase("UnifiedBus - URMA_READ WQE metadata propagates through Jetty segmentation")
    {
    }

    void DoRun() override
    {
        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbController> controller = CreateObject<UbController>();
        node->AggregateObject(controller);
        controller->CreateUbFunction();
        controller->CreateUbTransaction();

        Ptr<UbFunction> function = controller->GetUbFunction();
        const uint32_t jettyNum = 7;
        const uint32_t taskId = 1234;
        const uint32_t payloadBytes = 4096;
        function->CreateJetty(node->GetId(), node->GetId() + 1, jettyNum);

        Ptr<UbWqe> wqe = function->CreateWqe(node->GetId(),
                                             node->GetId() + 1,
                                             payloadBytes,
                                             taskId,
                                             TaOpcode::TA_OPCODE_READ);
        NS_TEST_ASSERT_MSG_NE(wqe, nullptr, "CreateWqe should return a valid WQE");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(wqe->GetType()),
                              static_cast<uint8_t>(TaOpcode::TA_OPCODE_READ),
                              "URMA_READ must map to TA_OPCODE_READ");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(wqe->GetSegmentKind()),
                              static_cast<uint8_t>(UbTransactionSegmentKind::REQUEST),
                              "CreateWqe should initialize segment kind as request");
        NS_TEST_ASSERT_MSG_EQ(wqe->GetOriginJettyNum(),
                              UINT32_MAX,
                              "originJettyNum should be invalid before enqueue");
        NS_TEST_ASSERT_MSG_EQ(wqe->GetRequestTassn(),
                              UINT32_MAX,
                              "requestTassn should be invalid before enqueue");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(wqe->GetRequestOpcode()),
                              static_cast<uint8_t>(TaOpcode::TA_OPCODE_READ),
                              "requestOpcode should preserve read opcode");
        NS_TEST_ASSERT_MSG_EQ(wqe->GetResponseBytes(),
                              payloadBytes,
                              "CreateWqe should initialize read response bytes");
        NS_TEST_ASSERT_MSG_EQ(wqe->GetLogicalBytes(),
                              payloadBytes,
                              "READ WQE logicalBytes should equal request bytes");
        NS_TEST_ASSERT_MSG_EQ(wqe->GetPayloadBytes(),
                              0u,
                              "READ WQE payloadBytes should be zero");
        NS_TEST_ASSERT_MSG_EQ(wqe->GetCarrierBytes(),
                              1u,
                              "READ WQE carrierBytes should force one request packet");
        NS_TEST_ASSERT_MSG_EQ(wqe->NeedsTransactionResponse(),
                              true,
                              "URMA read must require transaction response");

        function->PushWqeToJetty(wqe, jettyNum);
        NS_TEST_ASSERT_MSG_EQ(wqe->GetOriginJettyNum(),
                              jettyNum,
                              "PushWqe must assign originJettyNum from bound Jetty");
        NS_TEST_ASSERT_MSG_EQ(wqe->GetRequestTassn(),
                              wqe->GetTaSsnStart(),
                              "PushWqe must assign requestTassn from WQE TA SSN start");

        Ptr<UbJetty> jetty = function->GetJetty(jettyNum);
        NS_TEST_ASSERT_MSG_NE(jetty, nullptr, "Jetty should exist after CreateJetty");
        Ptr<UbWqeSegment> segment = jetty->GetNextWqeSegment();
        NS_TEST_ASSERT_MSG_NE(segment, nullptr, "Jetty should generate a segment for queued WQE");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(segment->GetType()),
                              static_cast<uint8_t>(TaOpcode::TA_OPCODE_READ),
                              "Segment opcode should remain TA_OPCODE_READ");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(segment->GetSegmentKind()),
                              static_cast<uint8_t>(UbTransactionSegmentKind::REQUEST),
                              "Segment should preserve request kind");
        NS_TEST_ASSERT_MSG_EQ(segment->GetOriginJettyNum(),
                              jettyNum,
                              "Segment should inherit originJettyNum");
        NS_TEST_ASSERT_MSG_EQ(segment->GetRequestTassn(),
                              wqe->GetRequestTassn(),
                              "Segment should inherit requestTassn");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(segment->GetRequestOpcode()),
                              static_cast<uint8_t>(TaOpcode::TA_OPCODE_READ),
                              "Segment should preserve requestOpcode");
        NS_TEST_ASSERT_MSG_EQ(segment->GetResponseBytes(),
                              payloadBytes,
                              "Segment should carry read response byte count");
        NS_TEST_ASSERT_MSG_EQ(segment->GetLogicalBytes(),
                              payloadBytes,
                              "READ request slice logicalBytes should preserve request bytes");
        NS_TEST_ASSERT_MSG_EQ(segment->GetPayloadBytes(),
                              0u,
                              "READ request slice payloadBytes should be zero");
        NS_TEST_ASSERT_MSG_EQ(segment->GetCarrierBytes(),
                              1u,
                              "READ request slice carrierBytes should be one");
        NS_TEST_ASSERT_MSG_EQ(segment->NeedsTransactionResponse(),
                              true,
                              "Segment should preserve response-required flag");
    }
};

class UbUrmaWriteCompletionNeedsTransactionResponseTest : public TestCase
{
  public:
    UbUrmaWriteCompletionNeedsTransactionResponseTest()
        : TestCase("UnifiedBus - URMA_WRITE completes on TA response instead of request TP ACK")
    {
    }

    void DoRun() override
    {
        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbFunction> senderFunction = senderCtrl->GetUbFunction();
        Ptr<UbTransaction> senderTransaction = senderCtrl->GetUbTransaction();
        senderFunction->CreateJetty(topo.sender->GetId(),
                                    topo.receiver->GetId(),
                                    kUrmaWriteRegressionJettyNum);
        const std::vector<uint32_t> tpns = {kUrmaWriteRegressionSenderTpn};
        const bool bindOk = senderTransaction->JettyBindTp(topo.sender->GetId(),
                                                           topo.receiver->GetId(),
                                                           kUrmaWriteRegressionJettyNum,
                                                           false,
                                                           tpns);
        NS_TEST_ASSERT_MSG_EQ(bindOk, true, "Sender Jetty should bind to static TP pair");

        Ptr<UbJetty> jetty = senderFunction->GetJetty(kUrmaWriteRegressionJettyNum);
        NS_TEST_ASSERT_MSG_NE(jetty, nullptr, "Sender Jetty should exist");
        jetty->SetClientCallback(
            MakeCallback(&UbUrmaWriteCompletionNeedsTransactionResponseTest::OnTaskCompleted, this));

        Ptr<UbTransportChannel> senderTp = senderCtrl->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        NS_TEST_ASSERT_MSG_NE(senderTp, nullptr, "Sender TP should exist");
        senderTp->TraceConnectWithoutContext(
            "LastPacketACKsNotify",
            MakeCallback(&UbUrmaWriteCompletionNeedsTransactionResponseTest::ObserveSenderTpAck, this));
        senderTp->TraceConnectWithoutContext(
            "LastPacketReceivesNotify",
            MakeCallback(&UbUrmaWriteCompletionNeedsTransactionResponseTest::ObserveSenderResponsePacket,
                         this));

        Ptr<UbWqe> wqe = senderFunction->CreateWqe(topo.sender->GetId(),
                                                   topo.receiver->GetId(),
                                                   64 * 1024,
                                                   kUrmaWriteRegressionTaskId,
                                                   TaOpcode::TA_OPCODE_WRITE);
        Simulator::ScheduleNow(&UbFunction::PushWqeToJetty,
                               senderFunction,
                               wqe,
                               kUrmaWriteRegressionJettyNum);

        Simulator::Stop(MilliSeconds(1));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(m_requestTpAckObserved,
                              true,
                              "Sender should observe request TP ACK");
        NS_TEST_ASSERT_MSG_EQ(m_completedImmediatelyAfterRequestTpAck,
                              false,
                              "URMA write must not complete immediately after request TP ACK");
        NS_TEST_ASSERT_MSG_EQ(m_responsePacketObserved,
                              true,
                              "Sender should receive a transaction response packet");
        NS_TEST_ASSERT_MSG_EQ(m_taskCompleted,
                              true,
                              "URMA write should complete after transaction response");
        NS_TEST_ASSERT_MSG_EQ(m_completedTaskId,
                              kUrmaWriteRegressionTaskId,
                              "Completion callback should report the original task");
        const bool completionAfterResponse = m_taskCompleteTime >= m_responsePacketTime;
        NS_TEST_ASSERT_MSG_EQ(completionAfterResponse,
                              true,
                              "Task completion must not precede transaction response arrival");

        Simulator::Destroy();
    }

  private:
    void ObserveSenderTpAck(uint32_t,
                            uint32_t taskId,
                            uint32_t srcTpn,
                            uint32_t dstTpn,
                            uint32_t,
                            uint32_t,
                            uint32_t)
    {
        if (taskId != kUrmaWriteRegressionTaskId ||
            srcTpn != kUrmaWriteRegressionSenderTpn ||
            dstTpn != kUrmaWriteRegressionReceiverTpn)
        {
            return;
        }

        if (!m_requestTpAckObserved)
        {
            m_requestTpAckObserved = true;
            Simulator::ScheduleNow(
                &UbUrmaWriteCompletionNeedsTransactionResponseTest::CheckCompletionAfterRequestTpAck,
                this);
        }
    }

    void ObserveSenderResponsePacket(uint32_t,
                                     uint32_t srcTpn,
                                     uint32_t dstTpn,
                                     uint32_t,
                                     uint32_t,
                                     uint32_t)
    {
        if (srcTpn != kUrmaWriteRegressionReceiverTpn || dstTpn != kUrmaWriteRegressionSenderTpn)
        {
            return;
        }

        if (!m_responsePacketObserved)
        {
            m_responsePacketObserved = true;
            m_responsePacketTime = Simulator::Now();
        }
    }

    void CheckCompletionAfterRequestTpAck()
    {
        m_completedImmediatelyAfterRequestTpAck = m_taskCompleted;
    }

    void OnTaskCompleted(uint32_t taskId, uint32_t)
    {
        m_taskCompleted = true;
        m_completedTaskId = taskId;
        m_taskCompleteTime = Simulator::Now();
    }

    bool m_requestTpAckObserved{false};
    bool m_responsePacketObserved{false};
    bool m_taskCompleted{false};
    bool m_completedImmediatelyAfterRequestTpAck{false};
    uint32_t m_completedTaskId{UINT32_MAX};
    Time m_responsePacketTime{Seconds(0)};
    Time m_taskCompleteTime{Seconds(0)};
};

class UbUrmaReadCompletionNeedsReadResponseTest : public TestCase
{
  public:
    UbUrmaReadCompletionNeedsReadResponseTest()
        : TestCase("UnifiedBus - URMA_READ completes on READ_RESPONSE instead of request TP ACK")
    {
    }

    void DoRun() override
    {
        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbFunction> senderFunction = senderCtrl->GetUbFunction();
        Ptr<UbTransaction> senderTransaction = senderCtrl->GetUbTransaction();
        senderFunction->CreateJetty(topo.sender->GetId(),
                                    topo.receiver->GetId(),
                                    kUrmaReadRegressionJettyNum);
        const std::vector<uint32_t> tpns = {kUrmaWriteRegressionSenderTpn};
        const bool bindOk = senderTransaction->JettyBindTp(topo.sender->GetId(),
                                                           topo.receiver->GetId(),
                                                           kUrmaReadRegressionJettyNum,
                                                           false,
                                                           tpns);
        NS_TEST_ASSERT_MSG_EQ(bindOk, true, "Sender Jetty should bind to static TP pair");

        Ptr<UbJetty> jetty = senderFunction->GetJetty(kUrmaReadRegressionJettyNum);
        NS_TEST_ASSERT_MSG_NE(jetty, nullptr, "Sender Jetty should exist");
        jetty->SetClientCallback(
            MakeCallback(&UbUrmaReadCompletionNeedsReadResponseTest::OnTaskCompleted, this));

        Ptr<UbTransportChannel> senderTp = senderCtrl->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        NS_TEST_ASSERT_MSG_NE(senderTp, nullptr, "Sender TP should exist");
        senderTp->TraceConnectWithoutContext(
            "LastPacketACKsNotify",
            MakeCallback(&UbUrmaReadCompletionNeedsReadResponseTest::ObserveSenderTpAck, this));
        senderTp->TraceConnectWithoutContext(
            "LastPacketReceivesNotify",
            MakeCallback(&UbUrmaReadCompletionNeedsReadResponseTest::ObserveSenderReadResponse,
                         this));

        Ptr<UbWqe> wqe = senderFunction->CreateWqe(topo.sender->GetId(),
                                                   topo.receiver->GetId(),
                                                   4096,
                                                   kUrmaReadRegressionTaskId,
                                                   TaOpcode::TA_OPCODE_READ);
        Simulator::ScheduleNow(&UbFunction::PushWqeToJetty,
                               senderFunction,
                               wqe,
                               kUrmaReadRegressionJettyNum);

        Simulator::Stop(MilliSeconds(1));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(m_requestTpAckObserved,
                              true,
                              "Sender should observe read request TP ACK");
        NS_TEST_ASSERT_MSG_EQ(m_completedImmediatelyAfterRequestTpAck,
                              false,
                              "URMA read must not complete immediately after request TP ACK");
        NS_TEST_ASSERT_MSG_EQ(m_readResponseObserved,
                              true,
                              "Sender should receive a READ_RESPONSE packet");
        NS_TEST_ASSERT_MSG_EQ(m_taskCompleted,
                              true,
                              "URMA read should complete after READ_RESPONSE");
        NS_TEST_ASSERT_MSG_EQ(m_completedTaskId,
                              kUrmaReadRegressionTaskId,
                              "Completion callback should report the original read task");
        const bool completionAfterResponse = m_taskCompleteTime >= m_readResponseTime;
        NS_TEST_ASSERT_MSG_EQ(completionAfterResponse,
                              true,
                              "Task completion must not precede READ_RESPONSE arrival");

        Simulator::Destroy();
    }

  private:
    void ObserveSenderTpAck(uint32_t,
                            uint32_t taskId,
                            uint32_t srcTpn,
                            uint32_t dstTpn,
                            uint32_t,
                            uint32_t,
                            uint32_t)
    {
        if (taskId != kUrmaReadRegressionTaskId ||
            srcTpn != kUrmaWriteRegressionSenderTpn ||
            dstTpn != kUrmaWriteRegressionReceiverTpn)
        {
            return;
        }

        if (!m_requestTpAckObserved)
        {
            m_requestTpAckObserved = true;
            Simulator::ScheduleNow(
                &UbUrmaReadCompletionNeedsReadResponseTest::CheckCompletionAfterRequestTpAck,
                this);
        }
    }

    void ObserveSenderReadResponse(uint32_t,
                                   uint32_t srcTpn,
                                   uint32_t dstTpn,
                                   uint32_t,
                                   uint32_t,
                                   uint32_t)
    {
        if (srcTpn != kUrmaWriteRegressionReceiverTpn || dstTpn != kUrmaWriteRegressionSenderTpn)
        {
            return;
        }

        if (!m_readResponseObserved)
        {
            m_readResponseObserved = true;
            m_readResponseTime = Simulator::Now();
        }
    }

    void CheckCompletionAfterRequestTpAck()
    {
        m_completedImmediatelyAfterRequestTpAck = m_taskCompleted;
    }

    void OnTaskCompleted(uint32_t taskId, uint32_t)
    {
        m_taskCompleted = true;
        m_completedTaskId = taskId;
        m_taskCompleteTime = Simulator::Now();
    }

    bool m_requestTpAckObserved{false};
    bool m_readResponseObserved{false};
    bool m_taskCompleted{false};
    bool m_completedImmediatelyAfterRequestTpAck{false};
    uint32_t m_completedTaskId{UINT32_MAX};
    Time m_readResponseTime{Seconds(0)};
    Time m_taskCompleteTime{Seconds(0)};
};

class UbUrmaReadMultiPacketResponseCountTest : public TestCase
{
  public:
    UbUrmaReadMultiPacketResponseCountTest()
        : TestCase("UnifiedBus - multi-packet URMA_READ generates one READ_RESPONSE")
    {
    }

    void DoRun() override
    {
        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbFunction> senderFunction = senderCtrl->GetUbFunction();
        Ptr<UbTransaction> senderTransaction = senderCtrl->GetUbTransaction();
        senderFunction->CreateJetty(topo.sender->GetId(),
                                    topo.receiver->GetId(),
                                    kUrmaReadRegressionJettyNum);
        const std::vector<uint32_t> tpns = {kUrmaWriteRegressionSenderTpn};
        const bool bindOk = senderTransaction->JettyBindTp(topo.sender->GetId(),
                                                           topo.receiver->GetId(),
                                                           kUrmaReadRegressionJettyNum,
                                                           false,
                                                           tpns);
        NS_TEST_ASSERT_MSG_EQ(bindOk, true, "Sender Jetty should bind to static TP pair");

        Ptr<UbJetty> jetty = senderFunction->GetJetty(kUrmaReadRegressionJettyNum);
        NS_TEST_ASSERT_MSG_NE(jetty, nullptr, "Sender Jetty should exist");
        jetty->SetClientCallback(
            MakeCallback(&UbUrmaReadMultiPacketResponseCountTest::OnTaskCompleted, this));

        Ptr<UbTransportChannel> senderTp = senderCtrl->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        NS_TEST_ASSERT_MSG_NE(senderTp, nullptr, "Sender TP should exist");
        senderTp->TraceConnectWithoutContext(
            "LastPacketReceivesNotify",
            MakeCallback(&UbUrmaReadMultiPacketResponseCountTest::ObserveSenderReadResponse,
                         this));

        Ptr<UbWqe> wqe = senderFunction->CreateWqe(topo.sender->GetId(),
                                                   topo.receiver->GetId(),
                                                   64 * 1024,
                                                   kUrmaReadMultiPacketTaskId,
                                                   TaOpcode::TA_OPCODE_READ);
        Simulator::ScheduleNow(&UbFunction::PushWqeToJetty,
                               senderFunction,
                               wqe,
                               kUrmaReadRegressionJettyNum);

        Simulator::Stop(MilliSeconds(1));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(m_readResponseCount,
                              1u,
                              "A multi-packet read request should generate exactly one READ_RESPONSE");
        NS_TEST_ASSERT_MSG_EQ(m_taskCompleteCount,
                              1u,
                              "A multi-packet read request should complete exactly once");

        Simulator::Destroy();
    }

  private:
    void ObserveSenderReadResponse(uint32_t,
                                   uint32_t srcTpn,
                                   uint32_t dstTpn,
                                   uint32_t,
                                   uint32_t,
                                   uint32_t)
    {
        if (srcTpn != kUrmaWriteRegressionReceiverTpn || dstTpn != kUrmaWriteRegressionSenderTpn)
        {
            return;
        }

        ++m_readResponseCount;
    }

    void OnTaskCompleted(uint32_t taskId, uint32_t)
    {
        if (taskId == kUrmaReadMultiPacketTaskId)
        {
            ++m_taskCompleteCount;
        }
    }

    uint32_t m_readResponseCount{0};
    uint32_t m_taskCompleteCount{0};
};

class UbUrmaReadMultiSliceRequestPacketSemanticsTest : public TestCase
{
  public:
    UbUrmaReadMultiSliceRequestPacketSemanticsTest()
        : TestCase("UnifiedBus - multi-slice URMA_READ request packets carry zero payload")
    {
    }

    void DoRun() override
    {
        (void)UbFlowTag::GetTypeId();
        (void)UbPacketTraceTag::GetTypeId();
        (void)utils::UbUtils::Get();
        GlobalValue::Bind("UB_RECORD_PKT_TRACE", BooleanValue(true));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        m_expectedSrc = topo.sender->GetId();
        m_expectedDst = topo.receiver->GetId();

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbFunction> senderFunction = senderCtrl->GetUbFunction();
        Ptr<UbTransaction> senderTransaction = senderCtrl->GetUbTransaction();
        senderFunction->CreateJetty(topo.sender->GetId(),
                                    topo.receiver->GetId(),
                                    kUrmaReadRegressionJettyNum);
        const std::vector<uint32_t> tpns = {kUrmaWriteRegressionSenderTpn};
        const bool bindOk = senderTransaction->JettyBindTp(topo.sender->GetId(),
                                                           topo.receiver->GetId(),
                                                           kUrmaReadRegressionJettyNum,
                                                           false,
                                                           tpns);
        NS_TEST_ASSERT_MSG_EQ(bindOk, true, "Sender Jetty should bind to static TP pair");
        Ptr<UbJetty> senderJetty = senderFunction->GetJetty(kUrmaReadRegressionJettyNum);
        NS_TEST_ASSERT_MSG_NE(senderJetty, nullptr, "Sender Jetty should exist");
        senderJetty->SetClientCallback(
            MakeCallback(&UbUrmaReadMultiSliceRequestPacketSemanticsTest::OnTaskCompleted, this));

        Ptr<UbController> receiverCtrl = topo.receiver->GetObject<UbController>();
        Ptr<UbTransportChannel> receiverTp = receiverCtrl->GetTpByTpn(kUrmaWriteRegressionReceiverTpn);
        NS_TEST_ASSERT_MSG_NE(receiverTp, nullptr, "Receiver TP should exist");
        receiverTp->TraceConnectWithoutContext(
            "TpRecvNotify",
            MakeCallback(
                &UbUrmaReadMultiSliceRequestPacketSemanticsTest::ObserveTargetPacketReceive,
                this));
        receiverTp->TraceConnectWithoutContext(
            "WqeSegmentCompletesNotify",
            MakeCallback(
                &UbUrmaReadMultiSliceRequestPacketSemanticsTest::ObserveTargetReadRequestSlice,
                this));
        Ptr<UbTransportChannel> senderTp = senderCtrl->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        NS_TEST_ASSERT_MSG_NE(senderTp, nullptr, "Sender TP should exist");
        senderTp->TraceConnectWithoutContext(
            "LastPacketReceivesNotify",
            MakeCallback(
                &UbUrmaReadMultiSliceRequestPacketSemanticsTest::ObserveSenderReadResponse,
                this));

        Ptr<UbWqe> wqe = senderFunction->CreateWqe(topo.sender->GetId(),
                                                   topo.receiver->GetId(),
                                                   128 * 1024,
                                                   kUrmaReadMultiPacketTaskId,
                                                   TaOpcode::TA_OPCODE_READ);
        Simulator::ScheduleNow(&UbFunction::PushWqeToJetty,
                               senderFunction,
                               wqe,
                               kUrmaReadRegressionJettyNum);

        Simulator::Stop(MilliSeconds(1));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(m_readRequestPacketCount,
                              2u,
                              "128KiB read should produce 2 read request packets");
        NS_TEST_ASSERT_MSG_EQ(m_zeroPayloadReadRequestCount,
                              2u,
                              "Each read request packet should carry zero payload");
        NS_TEST_ASSERT_MSG_EQ(m_targetReadRequestSliceCount,
                              2u,
                              "128KiB read should complete 2 request slices at TA");
        NS_TEST_ASSERT_MSG_EQ(m_readResponseCount,
                              2u,
                              "128KiB read should generate 2 read responses");
        NS_TEST_ASSERT_MSG_EQ(m_taskCompleteCount,
                              1u,
                              "Multi-slice read should still complete the WQE exactly once");

        Simulator::Destroy();
        GlobalValue::Bind("UB_RECORD_PKT_TRACE", BooleanValue(false));
    }

  private:
    void ObserveTargetPacketReceive(uint32_t,
                                    uint32_t,
                                    uint32_t src,
                                    uint32_t dst,
                                    uint32_t srcTpn,
                                    uint32_t dstTpn,
                                    PacketType type,
                                    uint32_t payloadBytes,
                                    uint32_t taskId,
                                    UbPacketTraceTag)
    {
        if (type != PacketType::PACKET || taskId != kUrmaReadMultiPacketTaskId)
        {
            return;
        }
        if (src != m_expectedSrc || dst != m_expectedDst)
        {
            return;
        }
        if (srcTpn != kUrmaWriteRegressionSenderTpn || dstTpn != kUrmaWriteRegressionReceiverTpn)
        {
            return;
        }

        ++m_readRequestPacketCount;
        if (payloadBytes == 0)
        {
            ++m_zeroPayloadReadRequestCount;
        }
    }

    void ObserveTargetReadRequestSlice(uint32_t,
                                       uint32_t taskId,
                                       uint32_t)
    {
        if (taskId == kUrmaReadMultiPacketTaskId)
        {
            ++m_targetReadRequestSliceCount;
        }
    }

    void ObserveSenderReadResponse(uint32_t,
                                   uint32_t srcTpn,
                                   uint32_t dstTpn,
                                   uint32_t,
                                   uint32_t,
                                   uint32_t)
    {
        if (srcTpn != kUrmaWriteRegressionReceiverTpn || dstTpn != kUrmaWriteRegressionSenderTpn)
        {
            return;
        }

        ++m_readResponseCount;
    }

    void OnTaskCompleted(uint32_t taskId, uint32_t)
    {
        if (taskId == kUrmaReadMultiPacketTaskId)
        {
            ++m_taskCompleteCount;
        }
    }

    uint32_t m_readRequestPacketCount{0};
    uint32_t m_zeroPayloadReadRequestCount{0};
    uint32_t m_targetReadRequestSliceCount{0};
    uint32_t m_readResponseCount{0};
    uint32_t m_taskCompleteCount{0};
    uint32_t m_expectedSrc{UINT32_MAX};
    uint32_t m_expectedDst{UINT32_MAX};
};

class UbUrmaWriteOutOfOrderRequestSliceCompletionTest : public TestCase
{
  public:
    UbUrmaWriteOutOfOrderRequestSliceCompletionTest()
        : TestCase("UnifiedBus - out-of-order URMA_WRITE request slice still completes at TA")
    {
    }

    void DoRun() override
    {
        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbController> receiverCtrl = topo.receiver->GetObject<UbController>();
        Ptr<UbTransportChannel> senderTp = senderCtrl->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbTransportChannel> receiverTp = receiverCtrl->GetTpByTpn(kUrmaWriteRegressionReceiverTpn);
        NS_TEST_ASSERT_MSG_NE(senderTp, nullptr, "Sender TP should exist");
        NS_TEST_ASSERT_MSG_NE(receiverTp, nullptr, "Receiver TP should exist");

        receiverTp->TraceConnectWithoutContext(
            "WqeSegmentCompletesNotify",
            MakeCallback(
                &UbUrmaWriteOutOfOrderRequestSliceCompletionTest::ObserveTargetRequestSliceComplete,
                this));

        Ptr<UbWqeSegment> request = CreateOutOfOrderWriteRequestSegment(topo, senderTp);
        senderTp->UpdatePsnCnt(request->GetPsnSize());
        senderTp->UpDateMsnCnt(1);
        senderTp->PushWqeSegment(request);

        Ptr<Packet> firstPacket = senderTp->GetNextPacket();
        Ptr<Packet> lastPacket = senderTp->GetNextPacket();
        NS_TEST_ASSERT_MSG_NE(firstPacket, nullptr, "First request packet should exist");
        NS_TEST_ASSERT_MSG_NE(lastPacket, nullptr, "Last request packet should exist");

        receiverTp->RecvDataPacket(lastPacket->Copy());
        NS_TEST_ASSERT_MSG_EQ(m_targetRequestSliceCompleteCount,
                              0u,
                              "Out-of-order last packet alone must not complete the TA slice");

        receiverTp->RecvDataPacket(firstPacket->Copy());
        Simulator::Stop(MilliSeconds(1));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(m_targetRequestSliceCompleteCount,
                              1u,
                              "Completing the PSN gap should complete the write request slice");

        Simulator::Destroy();
    }

  private:
    Ptr<UbWqeSegment> CreateOutOfOrderWriteRequestSegment(const LocalTpTopology& topo,
                                                          const Ptr<UbTransportChannel>& senderTp)
    {
        constexpr uint32_t requestBytes = UB_MTU_BYTE + 512;
        Ptr<UbWqeSegment> segment = CreateObject<UbWqeSegment>();
        segment->SetSrc(topo.sender->GetId());
        segment->SetDest(topo.receiver->GetId());
        segment->SetSport(topo.senderPort->GetIfIndex());
        segment->SetDport(topo.receiverPort->GetIfIndex());
        segment->SetType(TaOpcode::TA_OPCODE_WRITE);
        segment->SetSize(requestBytes);
        segment->SetPriority(kUrmaWriteRegressionPriority);
        segment->SetTaskId(kUrmaWriteRegressionTaskId);
        segment->SetWqeSize(requestBytes);
        segment->SetJettyNum(kUrmaWriteRegressionJettyNum);
        segment->SetTaMsn(0);
        segment->SetTaSsn(0);
        segment->SetOrderType(OrderType::ORDER_NO);
        segment->SetTpn(kUrmaWriteRegressionSenderTpn);
        segment->SetTpMsn(senderTp->GetMsnCnt());
        segment->SetPsnStart(senderTp->GetPsnCnt());
        segment->SetSegmentKind(UbTransactionSegmentKind::REQUEST);
        segment->SetOriginJettyNum(kUrmaWriteRegressionJettyNum);
        segment->SetRequestTassn(0);
        segment->SetRequestOpcode(TaOpcode::TA_OPCODE_WRITE);
        segment->SetResponseBytes(0);
        segment->SetNeedsTransactionResponse(true);
        segment->SetLogicalBytes(requestBytes);
        segment->SetPayloadBytes(requestBytes);
        segment->SetCarrierBytes(requestBytes);
        return segment;
    }

    void ObserveTargetRequestSliceComplete(uint32_t, uint32_t taskId, uint32_t)
    {
        if (taskId == kUrmaWriteRegressionTaskId)
        {
            ++m_targetRequestSliceCompleteCount;
        }
    }

    uint32_t m_targetRequestSliceCompleteCount{0};
};

class UbCreateNodeSystemIdTest : public TestCase
{
  public:
    UbCreateNodeSystemIdTest()
        : TestCase("UnifiedBus - CreateNode honors systemId column")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        const uint32_t beforeNodes = NodeList::GetNNodes();
        auto uniqueSuffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        fs::path caseDir = fs::temp_directory_path() / ("ub-systemid-test-" + uniqueSuffix);
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(caseDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary case directory creation should succeed");

        fs::path nodePath = caseDir / "node.csv";
        std::ofstream nodeFile(nodePath.string());
        nodeFile << "nodeId,nodeType,portNum,forwardDelay,systemId\n";
        nodeFile << "0,DEVICE,1,1ns,0\n";
        nodeFile << "1,SWITCH,2,1ns,1\n";
        nodeFile.close();

        utils::UbUtils::Get()->CreateNode(nodePath.string());

        NS_TEST_ASSERT_MSG_EQ(NodeList::GetNNodes(), beforeNodes + 2, "CreateNode should create 2 nodes");
        NS_TEST_ASSERT_MSG_EQ(NodeList::GetNode(beforeNodes)->GetSystemId(), 0u,
                              "First created node should preserve systemId 0");
        NS_TEST_ASSERT_MSG_EQ(NodeList::GetNode(beforeNodes + 1)->GetSystemId(), 1u,
                              "Second created node should preserve systemId 1");

        fs::remove_all(caseDir, ec);
    }
};

class UbSwitchFlowControlModeAttributeTest : public TestCase
{
  public:
    UbSwitchFlowControlModeAttributeTest()
        : TestCase("UnifiedBus - UbSwitch flow-control mode attribute round-trips")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        sw->SetAttribute("FlowControl", EnumValue(FcType::PFC_DYNAMIC_PAPER));

        EnumValue<FcType> modeValue;
        sw->GetAttribute("FlowControl", modeValue);

        NS_TEST_ASSERT_MSG_EQ(static_cast<int>(modeValue.Get()),
                              static_cast<int>(FcType::PFC_DYNAMIC_PAPER),
                              "UbSwitch flow-control mode attribute should round-trip through the Config system");
        Config::Reset();
    }
};

class UbSwitchLocalRuntimeConfigTest : public TestCase
{
  public:
    UbSwitchLocalRuntimeConfigTest()
        : TestCase("UnifiedBus - UbSwitch local runtime config overrides global buffer and PFC defaults")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::PFC_FIXED));
        Config::SetDefault("ns3::UbQueueManager::ReservePerQueueBytes", UintegerValue(8192));
        Config::SetDefault("ns3::UbQueueManager::SharedPoolBytes", UintegerValue(16384));
        Config::SetDefault("ns3::UbQueueManager::HeadroomPerPortBytes", UintegerValue(2048));
        Config::SetDefault("ns3::UbPort::PfcUpThld", IntegerValue(1000));
        Config::SetDefault("ns3::UbPort::PfcLowThld", IntegerValue(900));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        sw->SetReservePerQueueBytes(100);
        sw->SetSharedPoolBytes(200);
        sw->SetHeadroomPerPortBytes(300);
        sw->SetPfcThresholds(120, 60);

        node->AggregateObject(sw);
        Ptr<UbPort> port = CreateObject<UbPort>();
        node->AddDevice(port);
        sw->Init();

        Ptr<UbQueueManager> queueManager = sw->GetQueueManager();
        auto profile = queueManager->GetBufferProfileView();
        NS_TEST_ASSERT_MSG_EQ(profile.reserve_per_queue_bytes,
                              100u,
                              "UbSwitch reserve config should override QueueManager global default");
        NS_TEST_ASSERT_MSG_EQ(profile.shared_pool_bytes,
                              200u,
                              "UbSwitch shared-pool config should override QueueManager global default");
        NS_TEST_ASSERT_MSG_EQ(profile.headroom_per_port_bytes,
                              300u,
                              "UbSwitch headroom config should override QueueManager global default");

        Ptr<UbPfc> pfc = DynamicCast<UbPfc>(port->GetFlowControl());
        constexpr uint32_t kPriority = 1;
        queueManager->PushToVoq(0, 0, kPriority, 130);

        Ptr<Packet> pfcPacket = pfc->CheckPfcThreshold(Create<Packet>(1), 0);
        NS_TEST_ASSERT_MSG_NE(pfcPacket,
                              nullptr,
                              "UbSwitch local PFC thresholds should override port global defaults");
        NS_TEST_ASSERT_MSG_EQ(port->GetCredits(kPriority),
                              0u,
                              "Switch-provided PFC xoff threshold should pause the queue");

        queueManager->PopFromVoq(0, 0, kPriority, 130);
        pfcPacket = pfc->CheckPfcThreshold(Create<Packet>(1), 0);
        NS_TEST_ASSERT_MSG_NE(pfcPacket,
                              nullptr,
                              "Queue draining below switch-provided xon should emit resume");
        NS_TEST_ASSERT_MSG_EQ(port->GetCredits(kPriority),
                              UB_CREDIT_MAX_VALUE,
                              "Switch-provided PFC xon threshold should resume the queue");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbPortSetDataRateWithoutCongestionControlTest : public TestCase
{
  public:
    UbPortSetDataRateWithoutCongestionControlTest()
        : TestCase("UnifiedBus - UbPort SetDataRate works without attached congestion control")
    {
    }

    void DoRun() override
    {
        Config::Reset();

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);

        Ptr<UbPort> port = CreateObject<UbPort>();
        node->AddDevice(port);
        sw->Init();

        const DataRate updatedRate("200Gbps");
        port->SetDataRate(updatedRate);

        NS_TEST_ASSERT_MSG_EQ(port->GetDataRate().GetBitRate(),
                              updatedRate.GetBitRate(),
                              "UbPort::SetDataRate should update the port even when no congestion control is attached");

        Simulator::Destroy();
        Config::Reset();
    }
};

#ifndef _WIN32
namespace
{

int RunInChildProcess(const std::function<void()>& fn);

} // namespace
#endif

class UbSwitchNotifySwitchDequeueWithoutCongestionControlTest : public TestCase
{
  public:
    UbSwitchNotifySwitchDequeueWithoutCongestionControlTest()
        : TestCase("UnifiedBus - UbSwitch NotifySwitchDequeue works without attached congestion control")
    {
    }

    void DoRun() override
    {
#ifndef _WIN32
        const int status = RunInChildProcess([]() {
            Config::Reset();

            Ptr<Node> node = CreateObject<Node>();
            Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
            node->AggregateObject(sw);

            Ptr<UbPort> port = CreateObject<UbPort>();
            node->AddDevice(port);
            sw->Init();

            const uint32_t kInPort = 0;
            const uint32_t kOutPort = 0;
            const uint32_t kPriority = 1;
            Ptr<Packet> packet = Create<Packet>(64);

            sw->GetQueueManager()->PushToVoq(kInPort, kOutPort, kPriority, packet->GetSize());
            sw->NotifySwitchDequeue(kInPort, kOutPort, kPriority, packet);

            Simulator::Destroy();
            Config::Reset();
        });

        NS_TEST_ASSERT_MSG_NE(status, -1, "fork/waitpid should succeed for dequeue null-cc guard test");
        NS_TEST_ASSERT_MSG_EQ(WIFEXITED(status), true, "Child process should exit normally");
        NS_TEST_ASSERT_MSG_EQ(WEXITSTATUS(status), 0, "NotifySwitchDequeue should not crash without congestion control");
#else
        NS_TEST_SKIP("fork-based crash guard test is not supported on Windows");
#endif
    }
};

namespace
{

struct UbSinglePortPfcFixture
{
    Ptr<Node> node;
    Ptr<UbSwitch> sw;
    Ptr<UbPort> port;
    Ptr<UbQueueManager> queueManager;
    Ptr<UbPfc> pfc;
};

struct UbMultiPortSwitchFixture
{
    Ptr<Node> node;
    Ptr<UbSwitch> sw;
    std::vector<Ptr<UbPort>> ports;
    Ptr<UbQueueManager> queueManager;
};

class UbObservingFlowControl : public UbFlowControl
{
  public:
    static TypeId GetTypeId(void)
    {
        static TypeId tid = TypeId("ns3::UbObservingFlowControl")
            .SetParent<UbFlowControl>()
            .AddConstructor<UbObservingFlowControl>();
        return tid;
    }

    void Configure(Ptr<UbQueueManager> queueManager, uint32_t observedInPort, uint32_t observedPriority)
    {
        m_queueManager = queueManager;
        m_observedInPort = observedInPort;
        m_observedPriority = observedPriority;
    }

    bool IsFcLimited(Ptr<UbIngressQueue> ingressQ) override
    {
        return false;
    }

    void OnIngressReleased(const UbFlowControlEventContext& context) override
    {
        m_called = true;
        m_observedIngressBytes =
            m_queueManager->GetQueueIngressTotalBytes(m_observedInPort, m_observedPriority);
        m_contextInPortId = context.inPortId;
        m_contextOutPortId = context.outPortId;
        m_contextPriority = context.priority;
    }

    bool m_called {false};
    uint64_t m_observedIngressBytes {0};
    uint32_t m_contextInPortId {std::numeric_limits<uint32_t>::max()};
    uint32_t m_contextOutPortId {std::numeric_limits<uint32_t>::max()};
    uint32_t m_contextPriority {std::numeric_limits<uint32_t>::max()};

  private:
    Ptr<UbQueueManager> m_queueManager;
    uint32_t m_observedInPort {0};
    uint32_t m_observedPriority {0};
};

class UbCbfcProbe : public UbCbfc
{
  public:
    static TypeId GetTypeId(void)
    {
        static TypeId tid = TypeId("ns3::UbCbfcProbe")
            .SetParent<UbCbfc>()
            .AddConstructor<UbCbfcProbe>();
        return tid;
    }

    int32_t GetPendingCells(uint32_t vl) const
    {
        return m_crdToReturn.at(vl);
    }

    int32_t GetTxFreeCells(uint32_t vl) const
    {
        return m_crdTxfree.at(vl);
    }

    bool ShouldUseCtrlCrdRtrForTest() const
    {
        return ShouldForceControlReturn();
    }

    Ptr<Packet> MaybeBuildControlReturnPacketForTest(uint32_t targetPortId)
    {
        return MaybeBuildControlReturnPacket(targetPortId);
    }

    void MaybeQueueControlReturnForTest(uint32_t targetPortId)
    {
        MaybeQueueControlReturn(targetPortId);
    }
};

UbSinglePortPfcFixture
CreateSinglePortPfcFixture(FcType mode,
                           uint32_t reserveBytes,
                           uint64_t sharedPoolBytes,
                           uint32_t headroomPerPortBytes,
                           uint32_t resumeGapBytes,
                           uint32_t alphaShift,
                           int32_t hiThresh,
                           int32_t loThresh)
{
    Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(mode));
    Config::SetDefault("ns3::UbPort::PfcUpThld", IntegerValue(hiThresh));
    Config::SetDefault("ns3::UbPort::PfcLowThld", IntegerValue(loThresh));
    Config::SetDefault("ns3::UbQueueManager::ReservePerQueueBytes", UintegerValue(reserveBytes));
    Config::SetDefault("ns3::UbQueueManager::SharedPoolBytes", UintegerValue(sharedPoolBytes));
    Config::SetDefault("ns3::UbQueueManager::HeadroomPerPortBytes", UintegerValue(headroomPerPortBytes));
    Config::SetDefault("ns3::UbQueueManager::DynamicPfcResumeGapBytes",
                       UintegerValue(resumeGapBytes));
    Config::SetDefault("ns3::UbQueueManager::AlphaShift", UintegerValue(alphaShift));

    UbSinglePortPfcFixture fixture;
    fixture.node = CreateObject<Node>();
    fixture.sw = CreateObject<UbSwitch>();
    fixture.node->AggregateObject(fixture.sw);
    fixture.port = CreateObject<UbPort>();
    fixture.node->AddDevice(fixture.port);
    fixture.sw->Init();
    fixture.queueManager = fixture.sw->GetQueueManager();
    fixture.pfc = DynamicCast<UbPfc>(fixture.port->GetFlowControl());
    return fixture;
}

int
ConsumeEgressPacketForTest(Ptr<Packet>, uint32_t, uint32_t, Ptr<UbPort>)
{
    return 0;
}

UbMultiPortSwitchFixture
CreateMultiPortSwitchFixture(FcType mode,
                             uint32_t portsNum,
                             uint32_t reserveBytes,
                             uint64_t sharedPoolBytes,
                             uint32_t headroomPerPortBytes,
                             uint32_t resumeGapBytes,
                             uint32_t alphaShift,
                             const std::vector<std::pair<int32_t, int32_t>>& pfcThresholds)
{
    Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(mode));
    Config::SetDefault("ns3::UbQueueManager::ReservePerQueueBytes", UintegerValue(reserveBytes));
    Config::SetDefault("ns3::UbQueueManager::SharedPoolBytes", UintegerValue(sharedPoolBytes));
    Config::SetDefault("ns3::UbQueueManager::HeadroomPerPortBytes", UintegerValue(headroomPerPortBytes));
    Config::SetDefault("ns3::UbQueueManager::DynamicPfcResumeGapBytes",
                       UintegerValue(resumeGapBytes));
    Config::SetDefault("ns3::UbQueueManager::AlphaShift", UintegerValue(alphaShift));

    UbMultiPortSwitchFixture fixture;
    fixture.node = CreateObject<Node>();
    fixture.sw = CreateObject<UbSwitch>();
    fixture.node->AggregateObject(fixture.sw);

    fixture.ports.reserve(portsNum);
    for (uint32_t portId = 0; portId < portsNum; ++portId)
    {
        Ptr<UbPort> port = CreateObject<UbPort>();
        if (portId < pfcThresholds.size())
        {
            port->SetAttribute("PfcUpThld", IntegerValue(pfcThresholds[portId].first));
            port->SetAttribute("PfcLowThld", IntegerValue(pfcThresholds[portId].second));
        }
        fixture.node->AddDevice(port);
        fixture.ports.push_back(port);
    }

    fixture.sw->Init();
    fixture.queueManager = fixture.sw->GetQueueManager();
    return fixture;
}

Ptr<UbQueueManager>
CreateQueueManagerFixture(uint32_t ports,
                          uint32_t vlNum,
                          uint32_t reserveBytes,
                          uint64_t sharedPoolBytes,
                          uint32_t headroomPerPortBytes,
                          uint32_t resumeGapBytes,
                          uint32_t alphaShift)
{
    Config::SetDefault("ns3::UbQueueManager::ReservePerQueueBytes", UintegerValue(reserveBytes));
    Config::SetDefault("ns3::UbQueueManager::SharedPoolBytes", UintegerValue(sharedPoolBytes));
    Config::SetDefault("ns3::UbQueueManager::HeadroomPerPortBytes", UintegerValue(headroomPerPortBytes));
    Config::SetDefault("ns3::UbQueueManager::DynamicPfcResumeGapBytes",
                       UintegerValue(resumeGapBytes));
    Config::SetDefault("ns3::UbQueueManager::AlphaShift", UintegerValue(alphaShift));

    Ptr<UbQueueManager> queueManager = CreateObject<UbQueueManager>();
    queueManager->SetPortsNum(ports);
    queueManager->SetVLNum(vlNum);
    queueManager->Init();
    return queueManager;
}

#ifndef _WIN32
int
RunInChildProcess(const std::function<void()>& fn)
{
    pid_t pid = fork();
    if (pid == -1)
    {
        return -1;
    }

    if (pid == 0)
    {
        fn();
        std::_Exit(0);
    }

    int status = 0;
    int waitRet = waitpid(pid, &status, 0);
    if (waitRet == -1)
    {
        return -1;
    }
    return status;
}
#endif

} // namespace

class UbPfcFixedModeCountsHeadroomTest : public TestCase
{
  public:
    UbPfcFixedModeCountsHeadroomTest()
        : TestCase("UnifiedBus - PFC_FIXED counts headroom occupancy")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kPriority = 1;
        auto fixture = CreateSinglePortPfcFixture(FcType::PFC_FIXED,
                                                  100,
                                                  0,
                                                  50,
                                                  10,
                                                  0,
                                                  100,
                                                  40);

        fixture.queueManager->PushToVoq(0, 0, kPriority, 120);

        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressNonHeadroomBytes(0, kPriority),
                              0u,
                              "Reserve/shared accounting should stay at zero when the packet enters headroom directly");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressHeadroomBytes(0, kPriority),
                              120u,
                              "Headroom bytes should record the packet when sharedPool is zero");

        Ptr<Packet> pfcPacket = fixture.pfc->CheckPfcThreshold(Create<Packet>(1), 0);
        NS_TEST_ASSERT_MSG_NE(pfcPacket,
                              nullptr,
                              "PFC_FIXED should send PAUSE when total ingress occupancy crosses the high watermark");
        NS_TEST_ASSERT_MSG_EQ(fixture.port->GetCredits(kPriority),
                              0u,
                              "PFC_FIXED PAUSE should clear credits for the congested priority");

        fixture.queueManager->PopFromVoq(0, 0, kPriority, 120);
        pfcPacket = fixture.pfc->CheckPfcThreshold(Create<Packet>(1), 0);
        NS_TEST_ASSERT_MSG_NE(pfcPacket,
                              nullptr,
                              "PFC_FIXED should send RESUME after the queue drains below the low watermark");
        NS_TEST_ASSERT_MSG_EQ(fixture.port->GetCredits(kPriority),
                              UB_CREDIT_MAX_VALUE,
                              "PFC_FIXED RESUME should restore credits for the drained priority");
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbPfcDynamicModePauseResumeTest : public TestCase
{
  public:
    UbPfcDynamicModePauseResumeTest()
        : TestCase("UnifiedBus - PFC_DYNAMIC pauses and resumes from shared occupancy")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kPriority = 1;
        auto fixture = CreateSinglePortPfcFixture(FcType::PFC_DYNAMIC,
                                                  100,
                                                  100,
                                                  50,
                                                  10,
                                                  0,
                                                  1000,
                                                  900);

        fixture.queueManager->PushToVoq(0, 0, kPriority, 120);
        fixture.queueManager->PushToVoq(0, 0, kPriority, 60);

        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressSharedBytes(0, kPriority),
                              80u,
                              "PFC_DYNAMIC test should build shared occupancy before pause");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressHeadroomBytes(0, kPriority),
                              0u,
                              "PFC_DYNAMIC pause case should be driven by shared bytes, not headroom");

        Ptr<Packet> pfcPacket = fixture.pfc->CheckPfcThreshold(Create<Packet>(1), 0);
        NS_TEST_ASSERT_MSG_NE(pfcPacket,
                              nullptr,
                              "PFC_DYNAMIC should send PAUSE when shared occupancy reaches the dynamic threshold");
        NS_TEST_ASSERT_MSG_EQ(fixture.port->GetCredits(kPriority),
                              0u,
                              "PFC_DYNAMIC PAUSE should clear credits for the congested priority");

        fixture.queueManager->PopFromVoq(0, 0, kPriority, 60);
        pfcPacket = fixture.pfc->CheckPfcThreshold(Create<Packet>(1), 0);
        NS_TEST_ASSERT_MSG_NE(pfcPacket,
                              nullptr,
                              "PFC_DYNAMIC should send RESUME after shared occupancy drains below xon");
        NS_TEST_ASSERT_MSG_EQ(fixture.port->GetCredits(kPriority),
                              UB_CREDIT_MAX_VALUE,
                              "PFC_DYNAMIC RESUME should restore credits for the drained priority");
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbQueueManagerDynamicPfcDecisionApiTest : public TestCase
{
  public:
    UbQueueManagerDynamicPfcDecisionApiTest()
        : TestCase("UnifiedBus - queue manager exposes unified dynamic PFC pause/resume decisions")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kPriority = 1;
        Ptr<UbQueueManager> queueManager = CreateQueueManagerFixture(/*ports*/ 1,
                                                                    /*vlNum*/ 4,
                                                                    /*reserveBytes*/ 100,
                                                                    /*sharedPoolBytes*/ 100,
                                                                    /*headroomPerPortBytes*/ 64,
                                                                    /*resumeGapBytes*/ 10,
                                                                    /*alphaShift*/ 0);

        queueManager->PushToVoq(0, 0, kPriority, 120);
        queueManager->PushToVoq(0, 0, kPriority, 60);

        auto queueOcc = queueManager->GetIngressQueueOccupancy(0, kPriority);
        Ptr<UbPfcDynamicDecisionHook> hook = CreateObject<UbPfcDynamicDecisionHook>();
        UbPfcDecision decision = hook->Evaluate(queueManager, 0, kPriority);

        NS_TEST_ASSERT_MSG_EQ(queueOcc.shared_bytes,
                              80u,
                              "test should build shared occupancy before asking for dynamic PFC decision");
        NS_TEST_ASSERT_MSG_EQ(decision.pause,
                              true,
                              "dynamic PFC pause decision should come from queue-manager state");
        NS_TEST_ASSERT_MSG_EQ(decision.resume,
                              false,
                              "queue above xoff should not be resumable");

        queueManager->PopFromVoq(0, 0, kPriority, 60);
        decision = hook->Evaluate(queueManager, 0, kPriority);
        NS_TEST_ASSERT_MSG_EQ(decision.pause,
                              false,
                              "after draining below xoff, pause decision should clear");
        NS_TEST_ASSERT_MSG_EQ(decision.resume,
                              true,
                              "after draining below xon, resume decision should come from queue-manager state");
        Config::Reset();
    }
};

class UbQueueManagerPaperPfcDecisionApiTest : public TestCase
{
  public:
    UbQueueManagerPaperPfcDecisionApiTest()
        : TestCase("UnifiedBus - queue manager exposes unified paper PFC pause/resume decisions")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kSharedPoolBytes = 10000;
        Config::SetDefault("ns3::UbQueueManager::PaperDynamicPfcBeta", UintegerValue(8));
        auto fixture = CreateSinglePortPfcFixture(FcType::PFC_DYNAMIC_PAPER,
                                                  /*reserveBytes*/ 0,
                                                  /*sharedPoolBytes*/ kSharedPoolBytes,
                                                  /*headroomPerPortBytes*/ 4096,
                                                  /*resumeGapBytes*/ 64,
                                                  /*alphaShift*/ 0,
                                                  /*hiThresh*/ 1000,
                                                  /*loThresh*/ 900);

        constexpr uint32_t kPriority = 1;
        Ptr<UbPfcPaperDynamicDecisionHook> hook = CreateObject<UbPfcPaperDynamicDecisionHook>();
        const uint64_t xoff = hook->Evaluate(fixture.queueManager, 0, kPriority).xoff_threshold_bytes;
        fixture.queueManager->PushToVoq(0, 0, kPriority, static_cast<uint32_t>(xoff + 100));

        UbPfcDecision decision = hook->Evaluate(fixture.queueManager, 0, kPriority);
        NS_TEST_ASSERT_MSG_EQ(decision.pause,
                              true,
                              "paper PFC pause decision should come from queue-manager state");
        NS_TEST_ASSERT_MSG_EQ(decision.resume,
                              false,
                              "paused paper-PFC queue should not immediately report resume");

        fixture.queueManager->PopFromVoq(0, 0, kPriority, static_cast<uint32_t>(xoff + 100));
        decision = hook->Evaluate(fixture.queueManager, 0, kPriority);
        NS_TEST_ASSERT_MSG_EQ(decision.pause,
                              false,
                              "after draining, paper PFC pause decision should clear");
        NS_TEST_ASSERT_MSG_EQ(decision.resume,
                              true,
                              "after draining, paper PFC resume decision should come from queue-manager state");
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbControlFrameUsesDedicatedAccountingTest : public TestCase
{
  public:
    UbControlFrameUsesDedicatedAccountingTest()
        : TestCase("UnifiedBus - control frames keep dedicated occupancy accounting")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateMultiPortSwitchFixture(FcType::NONE,
                                                    /*portsNum*/ 1,
                                                    /*reserveBytes*/ 0,
                                                    /*sharedPoolBytes*/ 0,
                                                    /*headroomPerPortBytes*/ 0,
                                                    /*resumeGapBytes*/ 16,
                                                    /*alphaShift*/ 1,
                                                    {});

        uint8_t credits[16] = {};
        credits[0] = 1;
        Ptr<Packet> controlPacket = UbDataLink::GenControlCreditPacket(credits);
        uint32_t controlBytes = controlPacket->GetSize();
        fixture.sw->SendControlFrame(controlPacket, 0);

        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressNonHeadroomBytes(0, 0),
                              0u,
                              "Control frames must not consume data ingress reserve/shared accounting");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressSharedBytes(0, 0),
                              0u,
                              "Control frames must not consume shared-pool accounting");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressHeadroomBytes(0, 0),
                              0u,
                              "Control frames must not consume headroom accounting");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetIngressControlBytes(0, 0),
                              controlBytes,
                              "Control frames should remain observable through dedicated ingress control accounting");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetOutPortControlBytes(0, 0),
                              controlBytes,
                              "Control frames should remain observable through dedicated out-port control accounting");

        fixture.queueManager->PopFromVoq(0, 0, 0, controlBytes);
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetIngressControlBytes(0, 0),
                              0u,
                              "Control accounting should drain when the queued control frame departs");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetOutPortControlBytes(0, 0),
                              0u,
                              "Out-port control accounting should drain when the queued control frame departs");
        Simulator::Destroy();
        Config::Reset();
    }
};

#ifndef _WIN32
class UbDataPacketHeaderRejectsPriorityZeroTest : public TestCase
{
  public:
    UbDataPacketHeaderRejectsPriorityZeroTest()
        : TestCase("UnifiedBus - data packet headers reject priority 0 in this simulator model")
    {
    }

    void DoRun() override
    {
        int status = RunInChildProcess([]() {
            Ptr<Packet> packet = Create<Packet>(0);
            UbDataLink::GenPacketHeader(packet,
                                        false,
                                        false,
                                        0,
                                        0,
                                        false,
                                        false,
                                        UbDatalinkHeaderConfig::PACKET_IPV4);
        });

        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status),
                              1,
                              "Generating a data packet on priority 0 should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status),
                              SIGABRT,
                              "Generating a data packet on priority 0 should fail with SIGABRT");
    }
};

class UbSendControlFrameRejectsDataPacketTest : public TestCase
{
  public:
    UbSendControlFrameRejectsDataPacketTest()
        : TestCase("UnifiedBus - SendControlFrame rejects non-control packets")
    {
    }

    void DoRun() override
    {
        int status = RunInChildProcess([]() {
            Config::Reset();
            auto fixture = CreateMultiPortSwitchFixture(FcType::NONE,
                                                        /*portsNum*/ 1,
                                                        /*reserveBytes*/ 0,
                                                        /*sharedPoolBytes*/ 0,
                                                        /*headroomPerPortBytes*/ 0,
                                                        /*resumeGapBytes*/ 16,
                                                        /*alphaShift*/ 1,
                                                        {});

            Ptr<Packet> dataPacket = Create<Packet>(0);
            UbDataLink::GenPacketHeader(dataPacket,
                                        false,
                                        false,
                                        1,
                                        1,
                                        false,
                                        false,
                                        UbDatalinkHeaderConfig::PACKET_IPV4);
            fixture.sw->SendControlFrame(dataPacket, 0);
        });

        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status),
                              1,
                              "SendControlFrame should abort when called with a data packet");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status),
                              SIGABRT,
                              "SendControlFrame misuse should fail with SIGABRT");
    }
};
#endif

class UbQueueManagerStickyHeadroomAccountingTest : public TestCase
{
  public:
    UbQueueManagerStickyHeadroomAccountingTest()
        : TestCase("UnifiedBus - sticky headroom accounts whole packets consistently")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Ptr<UbQueueManager> queueManager = CreateQueueManagerFixture(/*ports*/ 1,
                                                                    /*vlNum*/ 2,
                                                                    /*reserveBytes*/ 100,
                                                                    /*sharedPoolBytes*/ 50,
                                                                    /*headroomPerPortBytes*/ 256,
                                                                    /*resumeGapBytes*/ 16,
                                                                    /*alphaShift*/ 0);

        queueManager->PushToVoq(0, 0, 1, 180);
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressNonHeadroomBytes(0, 1),
                              0u,
                              "A crossing packet should enter headroom as a whole in the non-splitting model");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressHeadroomBytes(0, 1),
                              180u,
                              "A crossing packet should be fully charged to headroom");

        queueManager->PushToVoq(0, 0, 1, 20);
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressHeadroomBytes(0, 1),
                              200u,
                              "Once a queue enters headroom, subsequent packets should stay sticky in headroom");

        queueManager->PopFromVoq(0, 0, 1, 20);
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressHeadroomBytes(0, 1),
                              180u,
                              "Dequeues should release sticky headroom bytes first in the chosen model");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressNonHeadroomBytes(0, 1),
                              0u,
                              "Sticky headroom release should not fabricate non-headroom occupancy");

        queueManager->PopFromVoq(0, 0, 1, 180);
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressTotalBytes(0, 1),
                              0u,
                              "Draining the queue should release both headroom and non-headroom accounting");
        Config::Reset();
    }
};

class UbQueueManagerIngressPortOccupancyViewTest : public TestCase
{
  public:
    UbQueueManagerIngressPortOccupancyViewTest()
        : TestCase("UnifiedBus - queue manager exposes structured ingress port occupancy view")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kPriorityA = 1;
        constexpr uint32_t kPriorityB = 2;
        Ptr<UbQueueManager> queueManager = CreateQueueManagerFixture(/*ports*/ 1,
                                                                    /*vlNum*/ 4,
                                                                    /*reserveBytes*/ 100,
                                                                    /*sharedPoolBytes*/ 0,
                                                                    /*headroomPerPortBytes*/ 256,
                                                                    /*resumeGapBytes*/ 16,
                                                                    /*alphaShift*/ 0);

        queueManager->PushToVoq(0, 0, kPriorityA, 80);
        queueManager->PushToVoq(0, 0, kPriorityB, 150);

        auto portOcc = queueManager->GetIngressPortOccupancy(0);
        NS_TEST_ASSERT_MSG_EQ(portOcc.non_headroom_bytes,
                              80u,
                              "port occupancy view should sum reserve/shared bytes across priorities");
        NS_TEST_ASSERT_MSG_EQ(portOcc.headroom_bytes,
                              150u,
                              "port occupancy view should sum headroom bytes across priorities");
        NS_TEST_ASSERT_MSG_EQ(portOcc.total_bytes,
                              230u,
                              "port occupancy total should match non-headroom plus headroom");

        Config::Reset();
    }
};

class UbPfcDynamicModeXoffZeroEmptyQueueTest : public TestCase
{
  public:
    UbPfcDynamicModeXoffZeroEmptyQueueTest()
        : TestCase("UnifiedBus - PFC_DYNAMIC keeps empty queues resumed when xoff collapses to zero")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateSinglePortPfcFixture(FcType::PFC_DYNAMIC,
                                                  /*reserveBytes*/ 100,
                                                  /*sharedPoolBytes*/ 1,
                                                  /*headroomPerPortBytes*/ 32,
                                                  /*resumeGapBytes*/ 16,
                                                  /*alphaShift*/ 1,
                                                  /*hiThresh*/ 1000,
                                                  /*loThresh*/ 900);

        Ptr<UbPfcDynamicDecisionHook> hook = CreateObject<UbPfcDynamicDecisionHook>();
        UbPfcDecision emptyDecision = hook->Evaluate(fixture.queueManager, 0, 1);
        NS_TEST_ASSERT_MSG_EQ(emptyDecision.xoff_threshold_bytes,
                              0u,
                              "This test requires xoff to collapse to zero");
        Ptr<Packet> pfcPacket = fixture.pfc->CheckPfcThreshold(Create<Packet>(1), 0);
        NS_TEST_ASSERT_MSG_EQ(pfcPacket,
                              nullptr,
                              "An empty queue must not send PFC when xoff is zero");
        NS_TEST_ASSERT_MSG_EQ(fixture.port->GetCredits(1),
                              UB_CREDIT_MAX_VALUE,
                              "An empty queue must remain resumed when xoff is zero");
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbPfcPaperDynamicModePauseResumeTest : public TestCase
{
  public:
    UbPfcPaperDynamicModePauseResumeTest()
        : TestCase("UnifiedBus - PFC_DYNAMIC_PAPER follows paper threshold and 2-MTU resume gap")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kSharedPoolBytes = 10000;
        constexpr uint32_t kPaperDynamicPfcBeta = 8;
        Config::SetDefault("ns3::UbQueueManager::PaperDynamicPfcBeta",
                           UintegerValue(kPaperDynamicPfcBeta));
        auto fixture = CreateSinglePortPfcFixture(FcType::PFC_DYNAMIC_PAPER,
                                                  /*reserveBytes*/ 0,
                                                  /*sharedPoolBytes*/ kSharedPoolBytes,
                                                  /*headroomPerPortBytes*/ 4096,
                                                  /*resumeGapBytes*/ 64,
                                                  /*alphaShift*/ 0,
                                                  /*hiThresh*/ 1000,
                                                  /*loThresh*/ 900);

        constexpr uint32_t kPaperPriorityCount = DEFAULT_PAPER_DYNAMIC_PFC_PRIORITY_COUNT;
        constexpr uint32_t kGlobalOccupancyBytes = 1000;
        Ptr<UbPfcPaperDynamicDecisionHook> hook = CreateObject<UbPfcPaperDynamicDecisionHook>();
        fixture.queueManager->PushToVoq(0, 0, 1, kGlobalOccupancyBytes);
        UbPfcDecision decision = hook->Evaluate(fixture.queueManager, 0, 1);
        fixture.queueManager->PopFromVoq(0, 0, 1, kGlobalOccupancyBytes);
        uint64_t xoff = decision.xoff_threshold_bytes;
        uint64_t xon = decision.xon_threshold_bytes;

        NS_TEST_ASSERT_MSG_EQ(xoff,
                              static_cast<uint64_t>(kPaperDynamicPfcBeta) *
                                  (kSharedPoolBytes - kGlobalOccupancyBytes) / kPaperPriorityCount,
                              "PFC_DYNAMIC_PAPER xoff should follow beta * remaining / priorities");
        uint64_t expectedXon = xoff > 2u * UB_MTU_BYTE ? xoff - 2u * UB_MTU_BYTE : 0u;
        NS_TEST_ASSERT_MSG_EQ(xon,
                              expectedXon,
                              "PFC_DYNAMIC_PAPER xon should be xoff minus two MTUs");
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbPfcPaperDynamicModeIgnoresAlphaShiftForAdmissionTest : public TestCase
{
  public:
    UbPfcPaperDynamicModeIgnoresAlphaShiftForAdmissionTest()
        : TestCase("UnifiedBus - PFC_DYNAMIC_PAPER admission should not be limited by old dynamic alpha shift")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kSharedPoolBytes = 20971520;
        constexpr uint32_t kHeadroomPerPortBytes = 65536;
        constexpr uint32_t kPriority = 7;
        constexpr uint32_t kPaperDynamicPfcBeta = 8;
        constexpr uint32_t kAlphaShift = 7;
        constexpr uint32_t kInPort = 0;
        constexpr uint32_t kOutPort = 1;
        Config::SetDefault("ns3::UbQueueManager::PaperDynamicPfcBeta",
                           UintegerValue(kPaperDynamicPfcBeta));
        auto fixture = CreateMultiPortSwitchFixture(FcType::PFC_DYNAMIC_PAPER,
                                                    /*portsNum*/ 3,
                                                    /*reserveBytes*/ 0,
                                                    /*sharedPoolBytes*/ kSharedPoolBytes,
                                                    /*headroomPerPortBytes*/ kHeadroomPerPortBytes,
                                                    /*resumeGapBytes*/ 64,
                                                    /*alphaShift*/ kAlphaShift,
                                                    {{1000, 900}, {1000, 900}, {1000, 900}});

        Ptr<UbPfcDynamicDecisionHook> dynamicHook = CreateObject<UbPfcDynamicDecisionHook>();
        Ptr<UbPfcPaperDynamicDecisionHook> paperHook = CreateObject<UbPfcPaperDynamicDecisionHook>();
        const uint64_t oldDynamicXoff = dynamicHook->Evaluate(fixture.queueManager, kInPort, kPriority).xoff_threshold_bytes;
        const uint64_t paperXoff = paperHook->Evaluate(fixture.queueManager, kInPort, kPriority).xoff_threshold_bytes;
        const uint32_t packetBytes = static_cast<uint32_t>(oldDynamicXoff) + 100;

        NS_TEST_ASSERT_MSG_LT(packetBytes,
                              paperXoff,
                              "test packet should stay below the paper pause threshold");

        fixture.queueManager->PushToVoq(kInPort, kOutPort, kPriority, packetBytes);

        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressHeadroomBytes(kInPort, kPriority),
                              0u,
                              "PFC_DYNAMIC_PAPER admission should not enter headroom just because old dynamic xoff is small");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressTotalBytes(kInPort, kPriority),
                              static_cast<uint64_t>(packetBytes),
                              "PFC_DYNAMIC_PAPER admission should keep the packet in ingress occupancy");

        auto pfc = DynamicCast<UbPfc>(fixture.ports[kInPort]->GetFlowControl());
        Ptr<Packet> pfcPacket = pfc->CheckPfcThreshold(Create<Packet>(1), kInPort);
        NS_TEST_ASSERT_MSG_EQ(pfcPacket,
                              nullptr,
                              "PFC_DYNAMIC_PAPER should not pause before reaching the paper threshold");
        NS_TEST_ASSERT_MSG_EQ(fixture.ports[kInPort]->GetCredits(kPriority),
                              UB_CREDIT_MAX_VALUE,
                              "PFC_DYNAMIC_PAPER should keep credits when still below the paper threshold");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbPfcPaperDynamicModeUsesRealGlobalOccupancyTest : public TestCase
{
  public:
    UbPfcPaperDynamicModeUsesRealGlobalOccupancyTest()
        : TestCase("UnifiedBus - PFC_DYNAMIC_PAPER threshold tracks real switch global occupancy")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kSharedPoolBytes = 12000;
        constexpr uint32_t kHeadroomPerPortBytes = 4096;
        constexpr uint32_t kPaperDynamicPfcBeta = 8;
        constexpr uint32_t kPriority = 7;
        constexpr uint32_t kObservedPort = 0;
        constexpr uint32_t kOtherInPort = 1;
        constexpr uint32_t kOtherOutPort = 2;
        Config::SetDefault("ns3::UbQueueManager::PaperDynamicPfcBeta",
                           UintegerValue(kPaperDynamicPfcBeta));
        auto fixture = CreateMultiPortSwitchFixture(FcType::PFC_DYNAMIC_PAPER,
                                                    /*portsNum*/ 3,
                                                    /*reserveBytes*/ 0,
                                                    /*sharedPoolBytes*/ kSharedPoolBytes,
                                                    /*headroomPerPortBytes*/ kHeadroomPerPortBytes,
                                                    /*resumeGapBytes*/ 64,
                                                    /*alphaShift*/ 0,
                                                    {{1000, 900}, {1000, 900}, {1000, 900}});

        Ptr<UbPfcPaperDynamicDecisionHook> paperHook = CreateObject<UbPfcPaperDynamicDecisionHook>();
        const uint64_t thresholdWithoutLoad =
            paperHook->Evaluate(fixture.queueManager, kObservedPort, kPriority).xoff_threshold_bytes;
        fixture.queueManager->PushToVoq(kOtherInPort, kOtherOutPort, kPriority, 3000);
        const uint64_t measuredGlobalOccupancy =
            fixture.queueManager->GetSwitchBufferOccupancy().total_buffered_bytes;
        const uint64_t thresholdWithLoad =
            paperHook->Evaluate(fixture.queueManager, kObservedPort, kPriority).xoff_threshold_bytes;

        NS_TEST_ASSERT_MSG_EQ(measuredGlobalOccupancy,
                              3000u,
                              "paper dynamic occupancy should include bytes queued on other ports");
        NS_TEST_ASSERT_MSG_LT(thresholdWithLoad,
                              thresholdWithoutLoad,
                              "paper dynamic threshold should shrink when global occupancy increases");

        auto pfc = DynamicCast<UbPfc>(fixture.ports[kObservedPort]->GetFlowControl());
        constexpr uint32_t kObservedBytesBelowPause = 4000;
        fixture.queueManager->PushToVoq(kObservedPort,
                                        kObservedPort,
                                        kPriority,
                                        kObservedBytesBelowPause);
        Ptr<Packet> pfcPacket = pfc->CheckPfcThreshold(Create<Packet>(1), kObservedPort);
        NS_TEST_ASSERT_MSG_EQ(pfcPacket,
                              nullptr,
                              "queue below the self-consistent paper dynamic boundary should stay resumed");

        fixture.queueManager->PushToVoq(kObservedPort, kObservedPort, kPriority, 600);
        pfcPacket = pfc->CheckPfcThreshold(Create<Packet>(1), kObservedPort);
        NS_TEST_ASSERT_MSG_NE(pfcPacket,
                              nullptr,
                              "queue crossing the self-consistent paper dynamic boundary should emit PFC");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbQueueManagerTotalBufferedBytesTracksEgressTransferTest : public TestCase
{
  public:
    UbQueueManagerTotalBufferedBytesTracksEgressTransferTest()
        : TestCase("UnifiedBus - total buffered bytes keeps VOQ and egress occupancy consistent")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateMultiPortSwitchFixture(FcType::NONE,
                                                    /*portsNum*/ 2,
                                                    /*reserveBytes*/ 0,
                                                    /*sharedPoolBytes*/ 0,
                                                    /*headroomPerPortBytes*/ 0,
                                                    /*resumeGapBytes*/ 64,
                                                    /*alphaShift*/ 0,
                                                    {});

        constexpr uint32_t kIngressPort = 0;
        constexpr uint32_t kEgressPort = 1;
        constexpr uint32_t kPriority = 1;
        constexpr uint32_t kPacketBytes = 512;

        fixture.sw->SendPacket(Create<Packet>(kPacketBytes), kIngressPort, kEgressPort, kPriority);
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetSwitchBufferOccupancy().total_buffered_bytes,
                              static_cast<uint64_t>(kPacketBytes),
                              "VOQ enqueue should contribute to total buffered bytes");

        fixture.sw->GetAllocator()->AllocateNextPacket(fixture.ports[kEgressPort]);
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetSwitchBufferOccupancy().total_buffered_bytes,
                              static_cast<uint64_t>(kPacketBytes),
                              "moving a packet from VOQ to egress should preserve total buffered bytes");
        NS_TEST_ASSERT_MSG_EQ(fixture.ports[kEgressPort]->GetUbQueue()->GetCurrentBytes(),
                              static_cast<uint64_t>(kPacketBytes),
                              "packet should now reside in the egress queue");

        fixture.ports[kEgressPort]->SetFaultCallBack(MakeCallback(&ConsumeEgressPacketForTest));
        fixture.ports[kEgressPort]->NotifyLinkUp();
        fixture.ports[kEgressPort]->TriggerTransmit();
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetSwitchBufferOccupancy().total_buffered_bytes,
                              0u,
                              "egress dequeue should release total buffered bytes");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbPfcForwardingUsesIngressPortConfigTest : public TestCase
{
  public:
    UbPfcForwardingUsesIngressPortConfigTest()
        : TestCase("UnifiedBus - forwarding path uses the ingress port's PFC configuration")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateMultiPortSwitchFixture(FcType::PFC_FIXED,
                                                    /*portsNum*/ 2,
                                                    /*reserveBytes*/ 256,
                                                    /*sharedPoolBytes*/ 0,
                                                    /*headroomPerPortBytes*/ 256,
                                                    /*resumeGapBytes*/ 16,
                                                    /*alphaShift*/ 0,
                                                    {{100, 40}, {1000, 900}});

        constexpr uint32_t kIngressPort = 0;
        constexpr uint32_t kEgressPort = 1;
        constexpr uint32_t kPriority = 1;
        fixture.queueManager->PushToVoq(kIngressPort, kEgressPort, kPriority, 120);

        Ptr<UbPfc> ingressFlowControl = DynamicCast<UbPfc>(fixture.ports[kIngressPort]->GetFlowControl());
        ingressFlowControl->OnIngressReleased({
            .packet = Create<Packet>(120),
            .ingressQueue = nullptr,
            .inPortId = kIngressPort,
            .outPortId = kEgressPort,
            .priority = kPriority,
        });

        NS_TEST_ASSERT_MSG_EQ(fixture.ports[kIngressPort]->GetCredits(kPriority),
                              0u,
                              "Forwarding-triggered PFC checks must use the congested ingress port's thresholds");
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbFlowControlReleaseHookRunsAfterIngressDequeueTest : public TestCase
{
  public:
    UbFlowControlReleaseHookRunsAfterIngressDequeueTest()
        : TestCase("UnifiedBus - flow-control release hook observes post-dequeue ingress occupancy")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateMultiPortSwitchFixture(FcType::NONE,
                                                    /*portsNum*/ 2,
                                                    /*reserveBytes*/ 256,
                                                    /*sharedPoolBytes*/ 0,
                                                    /*headroomPerPortBytes*/ 0,
                                                    /*resumeGapBytes*/ 16,
                                                    /*alphaShift*/ 0,
                                                    {});

        constexpr uint32_t kIngressPort = 0;
        constexpr uint32_t kEgressPort = 1;
        constexpr uint32_t kPriority = 1;
        constexpr uint32_t kPacketBytes = 120;

        Ptr<UbObservingFlowControl> observingFc = CreateObject<UbObservingFlowControl>();
        observingFc->Configure(fixture.queueManager, kIngressPort, kPriority);
        fixture.ports[kIngressPort]->m_flowControl = observingFc;

        // Keep the port busy so SendPacket/AllocateNextPacket won't try to hit a null channel.
        fixture.ports[kEgressPort]->SetSendState(SendState::BUSY);

        fixture.sw->SendPacket(Create<Packet>(kPacketBytes), kIngressPort, kEgressPort, kPriority);
        fixture.sw->GetAllocator()->AllocateNextPacket(fixture.ports[kEgressPort]);

        NS_TEST_ASSERT_MSG_EQ(observingFc->m_called,
                              true,
                              "allocator path should trigger flow-control release hook for forwarded packets");
        NS_TEST_ASSERT_MSG_EQ(observingFc->m_observedIngressBytes,
                              0u,
                              "release hook should observe ingress occupancy after queue-manager dequeue");
        NS_TEST_ASSERT_MSG_EQ(observingFc->m_contextInPortId,
                              kIngressPort,
                              "release hook should receive ingress port through event context");
        NS_TEST_ASSERT_MSG_EQ(observingFc->m_contextOutPortId,
                              kEgressPort,
                              "release hook should receive egress port through event context");
        NS_TEST_ASSERT_MSG_EQ(observingFc->m_contextPriority,
                              kPriority,
                              "release hook should receive priority through event context");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbAllocatorKeepsIngressPacketWhenEgressQueueIsFullTest : public TestCase
{
  public:
    UbAllocatorKeepsIngressPacketWhenEgressQueueIsFullTest()
        : TestCase("UnifiedBus - allocator keeps ingress packet when egress queue is full")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateMultiPortSwitchFixture(FcType::NONE,
                                                    /*portsNum*/ 2,
                                                    /*reserveBytes*/ 256,
                                                    /*sharedPoolBytes*/ 0,
                                                    /*headroomPerPortBytes*/ 0,
                                                    /*resumeGapBytes*/ 16,
                                                    /*alphaShift*/ 0,
                                                    {});

        constexpr uint32_t kIngressPort = 0;
        constexpr uint32_t kEgressPort = 1;
        constexpr uint32_t kPriority = 1;
        constexpr uint32_t kPacketBytes = 120;

        Ptr<UbObservingFlowControl> observingFc = CreateObject<UbObservingFlowControl>();
        observingFc->Configure(fixture.queueManager, kIngressPort, kPriority);
        fixture.ports[kEgressPort]->m_flowControl = observingFc;
        fixture.ports[kEgressPort]->GetUbQueue()->SetAttribute("MaxEgressBytes", UintegerValue(0));

        fixture.sw->SendPacket(Create<Packet>(kPacketBytes), kIngressPort, kEgressPort, kPriority);
        fixture.sw->GetAllocator()->AllocateNextPacket(fixture.ports[kEgressPort]);

        NS_TEST_ASSERT_MSG_EQ(observingFc->m_called,
                              false,
                              "release hook must not run when the packet never entered the egress queue");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressTotalBytes(kIngressPort, kPriority),
                              static_cast<uint64_t>(kPacketBytes),
                              "allocator must keep ingress accounting when egress queue rejects the packet");
        NS_TEST_ASSERT_MSG_EQ(fixture.ports[kEgressPort]->GetUbQueue()->GetCurrentBytes(),
                              0u,
                              "egress queue occupancy should remain unchanged after a rejected enqueue");

        Simulator::Destroy();
        Config::Reset();
    }
};

#ifndef _WIN32
class UbCbfcRejectsZeroCellGeometryTest : public TestCase
{
  public:
    UbCbfcRejectsZeroCellGeometryTest()
        : TestCase("UnifiedBus - CBFC rejects zero cell geometry at init")
    {
    }

    void DoRun() override
    {
        int status = RunInChildProcess([]() {
            Config::Reset();
            Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::CBFC));
            Config::SetDefault("ns3::UbPort::CbfcFlitLenByte", UintegerValue(0));

            Ptr<Node> node = CreateObject<Node>();
            Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
            node->AggregateObject(sw);
            node->AddDevice(CreateObject<UbPort>());
            sw->Init();
        });

        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status),
                              1,
                              "Invalid CBFC cell geometry should abort during initialization");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status),
                              SIGABRT,
                              "Invalid CBFC cell geometry should fail with SIGABRT");
        Config::Reset();
    }
};

class UbPfcFixedRejectsNegativeThresholdTest : public TestCase
{
  public:
    UbPfcFixedRejectsNegativeThresholdTest()
        : TestCase("UnifiedBus - PFC_FIXED rejects negative thresholds at init")
    {
    }

    void DoRun() override
    {
        int status = RunInChildProcess([]() {
            Config::Reset();
            Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::PFC_FIXED));
            Config::SetDefault("ns3::UbPort::PfcUpThld", IntegerValue(-1));
            Config::SetDefault("ns3::UbPort::PfcLowThld", IntegerValue(-2));

            Ptr<Node> node = CreateObject<Node>();
            Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
            node->AggregateObject(sw);
            node->AddDevice(CreateObject<UbPort>());
            sw->Init();
        });

        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status),
                              1,
                              "Negative PFC_FIXED thresholds should abort during initialization");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status),
                              SIGABRT,
                              "Negative PFC_FIXED thresholds should fail with SIGABRT");
        Config::Reset();
    }
};

class UbSwitchLocalCbfcConfigTest : public TestCase
{
  public:
    UbSwitchLocalCbfcConfigTest()
        : TestCase("UnifiedBus - UbSwitch local CBFC config overrides global defaults")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::CBFC));
        Config::SetDefault("ns3::UbPort::CbfcFlitLenByte", UintegerValue(20));
        Config::SetDefault("ns3::UbPort::CbfcFlitsPerCell", UintegerValue(8));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainDataPacket", UintegerValue(4));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainControlPacket", UintegerValue(1));
        Config::SetDefault("ns3::UbPort::CbfcInitCreditCell", IntegerValue(1000));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        sw->SetCbfcCellGeometry(/*flitLenBytes*/ 10, /*flitsPerCell*/ 2);
        sw->SetCbfcReturnCellGrain(/*dataPacketCells*/ 4, /*controlPacketCells*/ 2);
        sw->SetCbfcCredits(/*initCreditCells*/ 5, /*sharedInitCreditCells*/ 0);
        node->AggregateObject(sw);

        Ptr<UbPort> port = CreateObject<UbPort>();
        node->AddDevice(port);
        sw->Init();

        Ptr<UbCbfc> cbfc = DynamicCast<UbCbfc>(port->GetFlowControl());
        NS_TEST_ASSERT_MSG_NE(cbfc, nullptr, "port should initialize CBFC flow control");

        constexpr uint32_t kPriority = 1;
        Ptr<UbPacketQueue> ingressQ = CreateObject<UbPacketQueue>();
        ingressQ->SetIngressPriority(kPriority);
        ingressQ->SetOutPortId(0);
        ingressQ->SetInPortId(0);
        ingressQ->Push(Create<Packet>(101));

        NS_TEST_ASSERT_MSG_EQ(cbfc->IsFcLimited(ingressQ),
                              true,
                              "Switch-local CBFC cell geometry and credits should override global defaults");

        Simulator::Destroy();
        Config::Reset();
    }
};
#endif

class UbCbfcPiggybackTargetVlCanDifferFromPacketVlTest : public TestCase
{
  public:
    UbCbfcPiggybackTargetVlCanDifferFromPacketVlTest()
        : TestCase("UnifiedBus - CBFC piggyback target VL can differ from packet VL")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::CBFC));
        Config::SetDefault("ns3::UbPort::CbfcFlitLenByte", UintegerValue(20));
        Config::SetDefault("ns3::UbPort::CbfcFlitsPerCell", UintegerValue(8));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainDataPacket", UintegerValue(4));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainControlPacket", UintegerValue(1));
        Config::SetDefault("ns3::UbPort::CbfcInitCreditCell", IntegerValue(100));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);
        Ptr<UbPort> port = CreateObject<UbPort>();
        node->AddDevice(port);
        sw->Init();

        Ptr<UbCbfcProbe> probe = CreateObject<UbCbfcProbe>();
        probe->Init(/*flitLen*/ 20, /*flitsPerCell*/ 8, /*retData*/ 4, /*retCtrl*/ 1,
                    /*initCredit*/ 100, /*forceThreshold*/ 64, node->GetId(), port->GetIfIndex());
        port->m_flowControl = probe;

        constexpr uint8_t kTargetVl = 5;
        for (int i = 0; i < 4; ++i)
        {
            probe->SetCrdToReturn(kTargetVl, 1, port);
        }

        Ptr<Packet> packet = Create<Packet>(256);
        UbDataLink::GenPacketHeader(packet,
                                    false,
                                    false,
                                    /*crdVl*/ 1,
                                    /*pktVl*/ 2,
                                    false,
                                    false,
                                    UbDatalinkHeaderConfig::PACKET_IPV4);
        Ptr<UbPacketQueue> ingressQueue = CreateObject<UbPacketQueue>();
        ingressQueue->SetIngressPriority(2);
        ingressQueue->SetInPortId(1);
        ingressQueue->SetOutPortId(0);

        probe->OnEgressEnqueued({
            .packet = packet,
            .ingressQueue = ingressQueue,
            .inPortId = 0,
            .outPortId = 0,
            .priority = 2,
        });

        UbDatalinkPacketHeader header;
        packet->PeekHeader(header);
        NS_TEST_ASSERT_MSG_EQ(header.GetCredit(),
                              true,
                              "eligible data packet should carry piggyback credit");
        NS_TEST_ASSERT_MSG_EQ(header.GetCreditTargetVL(),
                              kTargetVl,
                              "piggyback should target the selected pending-return VL, not packet VL");
        NS_TEST_ASSERT_MSG_EQ(header.GetPacketVL(),
                              2u,
                              "piggyback must not alter the packet's own VL");
        NS_TEST_ASSERT_MSG_EQ(probe->GetPendingCells(kTargetVl),
                              0,
                              "one data-grain worth of pending cells should be consumed by piggyback");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbCbfcPiggybackRestoreClearsLocalCreditBitTest : public TestCase
{
  public:
    UbCbfcPiggybackRestoreClearsLocalCreditBitTest()
        : TestCase("UnifiedBus - CBFC piggyback restore is consumed locally and clears header state")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateMultiPortSwitchFixture(FcType::CBFC,
                                                    /*portsNum*/ 1,
                                                    /*reserveBytes*/ 0,
                                                    /*sharedPoolBytes*/ 0,
                                                    /*headroomPerPortBytes*/ 0,
                                                    /*resumeGapBytes*/ 16,
                                                    /*alphaShift*/ 1,
                                                    {});

        Ptr<UbCbfcProbe> probe = CreateObject<UbCbfcProbe>();
        probe->Init(/*flitLen*/ 20, /*flitsPerCell*/ 8, /*retData*/ 4, /*retCtrl*/ 1,
                    /*initCredit*/ 10, /*forceThreshold*/ 64, fixture.node->GetId(), fixture.ports[0]->GetIfIndex());
        fixture.ports[0]->m_flowControl = probe;

        const int32_t before = probe->GetTxFreeCells(6);
        Ptr<Packet> packet = Create<Packet>(128);
        UbDataLink::GenPacketHeader(packet,
                                    true,
                                    false,
                                    /*crdVl*/ 6,
                                    /*pktVl*/ 3,
                                    false,
                                    false,
                                    UbDatalinkHeaderConfig::PACKET_IPV4);

        probe->OnDataPacketReceived(packet);

        UbDatalinkPacketHeader header;
        packet->PeekHeader(header);
        NS_TEST_ASSERT_MSG_EQ(header.GetCredit(),
                              false,
                              "piggyback credit flag should be cleared after the local hop consumes it");
        NS_TEST_ASSERT_MSG_EQ(header.GetCreditTargetVL(),
                              0u,
                              "piggyback target VL should be cleared after local consumption");
        NS_TEST_ASSERT_MSG_EQ(probe->GetTxFreeCells(6),
                              before + 4,
                              "piggyback restore should add one data-grain worth of cells to the target VL");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbCbfcControlFrameFallbackWithoutDataPacketTest : public TestCase
{
  public:
    UbCbfcControlFrameFallbackWithoutDataPacketTest()
        : TestCase("UnifiedBus - CBFC does not emit control frame below ctrl-credit-return threshold without data packet")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::CBFC));
        Config::SetDefault("ns3::UbPort::CbfcFlitLenByte", UintegerValue(20));
        Config::SetDefault("ns3::UbPort::CbfcFlitsPerCell", UintegerValue(8));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainDataPacket", UintegerValue(4));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainControlPacket", UintegerValue(1));
        Config::SetDefault("ns3::UbPort::CbfcInitCreditCell", IntegerValue(100));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);
        Ptr<UbPort> port = CreateObject<UbPort>();
        node->AddDevice(port);
        sw->Init();

        Ptr<UbCbfcProbe> probe = CreateObject<UbCbfcProbe>();
        probe->Init(/*flitLen*/ 20, /*flitsPerCell*/ 8, /*retData*/ 4, /*retCtrl*/ 1,
                    /*initCredit*/ 100, /*forceThreshold*/ 64, node->GetId(), port->GetIfIndex());
        port->m_flowControl = probe;

        probe->SetCrdToReturn(4, 1, port);
        Ptr<Packet> fallback = probe->MaybeBuildControlReturnPacketForTest(port->GetIfIndex());
        NS_TEST_ASSERT_MSG_EQ(fallback,
                              nullptr,
                              "below the ctrl-credit-return threshold, pending credit should stay queued instead of generating a control frame");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbCbfcCtrlCrdRtrThresholdTriggersControlFrameTest : public TestCase
{
  public:
    UbCbfcCtrlCrdRtrThresholdTriggersControlFrameTest()
        : TestCase("UnifiedBus - CBFC switches to control-frame credit return once pending threshold is reached")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::CBFC));
        Config::SetDefault("ns3::UbPort::CbfcFlitLenByte", UintegerValue(20));
        Config::SetDefault("ns3::UbPort::CbfcFlitsPerCell", UintegerValue(8));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainDataPacket", UintegerValue(4));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainControlPacket", UintegerValue(1));
        Config::SetDefault("ns3::UbPort::CbfcInitCreditCell", IntegerValue(100));
        Config::SetDefault("ns3::UbPort::CbfcCtrlCrdRtrThldCell", IntegerValue(64));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);
        Ptr<UbPort> port = CreateObject<UbPort>();
        node->AddDevice(port);
        sw->Init();

        Ptr<UbCbfcProbe> probe = CreateObject<UbCbfcProbe>();
        probe->Init(/*flitLen*/ 20, /*flitsPerCell*/ 8, /*retData*/ 4, /*retCtrl*/ 1,
                    /*initCredit*/ 100, /*forceThreshold*/ 64, node->GetId(), port->GetIfIndex());
        port->m_flowControl = probe;

        probe->SetCrdToReturn(6, 63, port);
        NS_TEST_ASSERT_MSG_EQ(probe->ShouldUseCtrlCrdRtrForTest(),
                              false,
                              "pending below threshold should not switch to control-frame credit return yet");

        probe->SetCrdToReturn(6, 1, port);
        NS_TEST_ASSERT_MSG_EQ(probe->ShouldUseCtrlCrdRtrForTest(),
                              true,
                              "pending reaching threshold should switch to control-frame credit return");

        Ptr<Packet> forced = probe->MaybeBuildControlReturnPacketForTest(port->GetIfIndex());
        NS_TEST_ASSERT_MSG_NE(forced,
                              nullptr,
                              "the ctrl-credit-return threshold should immediately generate a control frame");
        NS_TEST_ASSERT_MSG_EQ(port->GetCredits(6),
                              63u,
                              "control-frame credit return must still respect the 6-bit credit-field limit per VL");
        NS_TEST_ASSERT_MSG_EQ(probe->GetPendingCells(6),
                              1,
                              "one cell should remain pending after the first control-return frame when 64 cells are queued");

        UbDatalinkHeader dlHeader;
        forced->PeekHeader(dlHeader);
        NS_TEST_ASSERT_MSG_EQ(dlHeader.IsControlCreditHeader(),
                              true,
                              "ctrl-credit-return should use a real control credit frame");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbCbfcForcedControlReturnFlushesAllEligibleVlsTest : public TestCase
{
  public:
    UbCbfcForcedControlReturnFlushesAllEligibleVlsTest()
        : TestCase("UnifiedBus - CBFC ctrl-credit-return flushes every VL already eligible for control return")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::CBFC));
        Config::SetDefault("ns3::UbPort::CbfcFlitLenByte", UintegerValue(20));
        Config::SetDefault("ns3::UbPort::CbfcFlitsPerCell", UintegerValue(8));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainDataPacket", UintegerValue(4));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainControlPacket", UintegerValue(1));
        Config::SetDefault("ns3::UbPort::CbfcInitCreditCell", IntegerValue(100));
        Config::SetDefault("ns3::UbPort::CbfcCtrlCrdRtrThldCell", IntegerValue(64));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);
        Ptr<UbPort> port = CreateObject<UbPort>();
        node->AddDevice(port);
        sw->Init();

        Ptr<UbCbfcProbe> probe = CreateObject<UbCbfcProbe>();
        probe->Init(/*flitLen*/ 20, /*flitsPerCell*/ 8, /*retData*/ 4, /*retCtrl*/ 1,
                    /*initCredit*/ 100, /*forceThreshold*/ 64, node->GetId(), port->GetIfIndex());
        port->m_flowControl = probe;

        probe->SetCrdToReturn(6, 64, port);
        probe->SetCrdToReturn(3, 4, port);
        NS_TEST_ASSERT_MSG_EQ(probe->ShouldUseCtrlCrdRtrForTest(),
                              true,
                              "one VL reaching threshold should trigger the immediate ctrl-credit-return path");

        Ptr<Packet> forced = probe->MaybeBuildControlReturnPacketForTest(port->GetIfIndex());
        NS_TEST_ASSERT_MSG_NE(forced,
                              nullptr,
                              "ctrl-credit-return path should build a control frame when threshold is hit");

        NS_TEST_ASSERT_MSG_EQ(probe->GetPendingCells(6),
                              1,
                              "the triggering VL should retain only the residual cells left after one 63-grain control frame");
        NS_TEST_ASSERT_MSG_EQ(probe->GetPendingCells(3),
                              0,
                              "ctrl-credit-return should also flush other VLs already eligible for control return");
        NS_TEST_ASSERT_MSG_EQ(port->GetCredits(6),
                              63u,
                              "control frame should clamp the triggering VL to the 6-bit credit-field limit");
        NS_TEST_ASSERT_MSG_EQ(port->GetCredits(3),
                              4u,
                              "control frame should carry other eligible VL credits in the same packet");

        UbDatalinkHeader dlHeader;
        forced->PeekHeader(dlHeader);
        NS_TEST_ASSERT_MSG_EQ(dlHeader.IsControlCreditHeader(),
                              true,
                              "mixed-VL ctrl-credit-return should still use a real control credit frame");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbCbfcForcedControlReturnChunksControlFramesAtSixBitLimitTest : public TestCase
{
  public:
    UbCbfcForcedControlReturnChunksControlFramesAtSixBitLimitTest()
        : TestCase("UnifiedBus - CBFC ctrl-credit-return chunks large pending credit into multiple control frames")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::CBFC));
        Config::SetDefault("ns3::UbPort::CbfcFlitLenByte", UintegerValue(20));
        Config::SetDefault("ns3::UbPort::CbfcFlitsPerCell", UintegerValue(8));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainDataPacket", UintegerValue(4));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainControlPacket", UintegerValue(1));
        Config::SetDefault("ns3::UbPort::CbfcInitCreditCell", IntegerValue(100));
        Config::SetDefault("ns3::UbPort::CbfcCtrlCrdRtrThldCell", IntegerValue(64));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);
        Ptr<UbPort> port = CreateObject<UbPort>();
        node->AddDevice(port);
        sw->Init();

        Ptr<UbCbfcProbe> probe = CreateObject<UbCbfcProbe>();
        probe->Init(/*flitLen*/ 20, /*flitsPerCell*/ 8, /*retData*/ 4, /*retCtrl*/ 1,
                    /*initCredit*/ 100, /*forceThreshold*/ 64, node->GetId(), port->GetIfIndex());
        port->m_flowControl = probe;

        probe->SetCrdToReturn(6, 130, port);
        NS_TEST_ASSERT_MSG_EQ(probe->ShouldUseCtrlCrdRtrForTest(),
                              true,
                              "pending above threshold should enter ctrl-credit-return mode");

        Ptr<Packet> first = probe->MaybeBuildControlReturnPacketForTest(port->GetIfIndex());
        NS_TEST_ASSERT_MSG_NE(first,
                              nullptr,
                              "large pending credit should still build a first control frame");
        NS_TEST_ASSERT_MSG_EQ(port->GetCredits(6),
                              63u,
                              "a single control frame must clamp one VL to the 6-bit credit-field limit");
        NS_TEST_ASSERT_MSG_EQ(probe->GetPendingCells(6),
                              67,
                              "only the actually encoded 63 cells should be deducted from pending credit");

        probe->MaybeQueueControlReturnForTest(port->GetIfIndex());
        NS_TEST_ASSERT_MSG_EQ(probe->GetPendingCells(6),
                              0,
                              "batch control-return enqueue should drain the remaining pending credit");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbCbfcForwardedReleaseUsesIngressPortThresholdStateTest : public TestCase
{
  public:
    UbCbfcForwardedReleaseUsesIngressPortThresholdStateTest()
        : TestCase("UnifiedBus - CBFC forwarded release checks threshold state on the ingress port instance")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::CBFC));
        Config::SetDefault("ns3::UbPort::CbfcFlitLenByte", UintegerValue(20));
        Config::SetDefault("ns3::UbPort::CbfcFlitsPerCell", UintegerValue(8));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainDataPacket", UintegerValue(4));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainControlPacket", UintegerValue(1));
        Config::SetDefault("ns3::UbPort::CbfcInitCreditCell", IntegerValue(100));
        Config::SetDefault("ns3::UbPort::CbfcCtrlCrdRtrThldCell", IntegerValue(64));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);
        Ptr<UbPort> ingressPort = CreateObject<UbPort>();
        Ptr<UbPort> egressPort = CreateObject<UbPort>();
        node->AddDevice(ingressPort);
        node->AddDevice(egressPort);
        sw->Init();

        Ptr<UbCbfcProbe> ingressProbe = CreateObject<UbCbfcProbe>();
        ingressProbe->Init(/*flitLen*/ 20, /*flitsPerCell*/ 8, /*retData*/ 4, /*retCtrl*/ 1,
                           /*initCredit*/ 100, /*forceThreshold*/ 64, node->GetId(), ingressPort->GetIfIndex());
        ingressPort->m_flowControl = ingressProbe;

        Ptr<UbCbfcProbe> egressProbe = CreateObject<UbCbfcProbe>();
        egressProbe->Init(/*flitLen*/ 20, /*flitsPerCell*/ 8, /*retData*/ 4, /*retCtrl*/ 1,
                          /*initCredit*/ 100, /*forceThreshold*/ 64, node->GetId(), egressPort->GetIfIndex());
        egressPort->m_flowControl = egressProbe;

        ingressProbe->SetCrdToReturn(5, 64, ingressPort);
        NS_TEST_ASSERT_MSG_EQ(ingressProbe->ShouldUseCtrlCrdRtrForTest(),
                              true,
                              "ingress-side flow control should see the ctrl-credit-return threshold reached");
        NS_TEST_ASSERT_MSG_EQ(egressProbe->ShouldUseCtrlCrdRtrForTest(),
                              false,
                              "egress-side flow control should stay below the ctrl-credit-return threshold in this setup");

        Ptr<Packet> packet = Create<Packet>(128);
        UbDataLink::GenPacketHeader(packet,
                                    false,
                                    false,
                                    /*crdVl*/ 1,
                                    /*pktVl*/ 5,
                                    false,
                                    false,
                                    UbDatalinkHeaderConfig::PACKET_IPV4);
        Ptr<UbPacketQueue> ingressQueue = CreateObject<UbPacketQueue>();
        ingressQueue->SetIngressPriority(5);
        ingressQueue->SetInPortId(ingressPort->GetIfIndex());
        ingressQueue->SetOutPortId(egressPort->GetIfIndex());

        ingressProbe->OnIngressReleased({
            .packet = packet,
            .ingressQueue = ingressQueue,
            .inPortId = ingressPort->GetIfIndex(),
            .outPortId = egressPort->GetIfIndex(),
            .priority = 5,
        });

        NS_TEST_ASSERT_MSG_LT(ingressProbe->GetPendingCells(5),
                              64,
                              "forwarded release should drain pending credits on the ingress-port flow-control instance");

        Simulator::Destroy();
        Config::Reset();
    }
};

#ifdef NS3_MPI
class UbCreateTopoRemoteLinkTest : public TestCase
{
  public:
    UbCreateTopoRemoteLinkTest()
        : TestCase("UnifiedBus - CreateTopo builds remote link across systemId")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        const uint32_t beforeNodes = NodeList::GetNNodes();
        auto uniqueSuffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        fs::path caseDir = fs::temp_directory_path() / ("ub-remote-link-test-" + uniqueSuffix);
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(caseDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary case directory creation should succeed");

        fs::path nodePath = caseDir / "node.csv";
        const uint32_t node0Id = beforeNodes;
        const uint32_t node1Id = beforeNodes + 1;

        std::ofstream nodeFile(nodePath.string());
        nodeFile << "nodeId,nodeType,portNum,forwardDelay,systemId\n";
        nodeFile << node0Id << ",DEVICE,1,1ns,0\n";
        nodeFile << node1Id << ",DEVICE,1,1ns,1\n";
        nodeFile.close();

        fs::path topoPath = caseDir / "topology.csv";
        std::ofstream topoFile(topoPath.string());
        topoFile << "node1,port1,node2,port2,bandwidth,delay\n";
        topoFile << node0Id << ",0," << node1Id << ",0,400Gbps,10ns\n";
        topoFile.close();

        utils::UbUtils::Get()->CreateNode(nodePath.string());
        utils::UbUtils::Get()->CreateTopo(topoPath.string());

        NS_TEST_ASSERT_MSG_EQ(NodeList::GetNNodes(), beforeNodes + 2, "CreateNode should create 2 nodes");

        Ptr<Node> n0 = NodeList::GetNode(beforeNodes);
        Ptr<Node> n1 = NodeList::GetNode(beforeNodes + 1);
        Ptr<UbPort> p0 = DynamicCast<UbPort>(n0->GetDevice(0));
        Ptr<UbPort> p1 = DynamicCast<UbPort>(n1->GetDevice(0));
        Ptr<Channel> channel = p0->GetChannel();

        NS_TEST_ASSERT_MSG_NE(channel, nullptr, "Port channel should be created");
        NS_TEST_ASSERT_MSG_EQ(channel->GetInstanceTypeId().GetName(), std::string("ns3::UbRemoteLink"),
                              "Cross-systemId topology should use UbRemoteLink");
        NS_TEST_ASSERT_MSG_EQ(p0->HasMpiReceive(), true, "Remote link endpoint should enable MPI receive");
        NS_TEST_ASSERT_MSG_EQ(p1->HasMpiReceive(), true, "Remote link endpoint should enable MPI receive");
        NS_TEST_ASSERT_MSG_EQ(p1->GetChannel(), p0->GetChannel(), "Both ports should share the same link");

        fs::remove_all(caseDir, ec);
    }
};
#endif

#if defined(NS3_MPI) && defined(NS3_MTP)
class UbCreateTopoPackedSystemIdLocalLinkTest : public TestCase
{
  public:
    UbCreateTopoPackedSystemIdLocalLinkTest()
        : TestCase("UnifiedBus - CreateTopo keeps same-rank packed systemId on local link")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        const uint32_t beforeNodes = NodeList::GetNNodes();
        auto uniqueSuffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        fs::path caseDir = fs::temp_directory_path() / ("ub-packed-local-link-test-" + uniqueSuffix);
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(caseDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary case directory creation should succeed");

        const uint32_t node0Id = beforeNodes;
        const uint32_t node1Id = beforeNodes + 1;
        const uint32_t node0SystemId = (0x0001u << 16) | 0x0009u;
        const uint32_t node1SystemId = (0x0002u << 16) | 0x0009u;

        fs::path nodePath = caseDir / "node.csv";
        std::ofstream nodeFile(nodePath.string());
        nodeFile << "nodeId,nodeType,portNum,forwardDelay,systemId\n";
        nodeFile << node0Id << ",DEVICE,1,1ns," << node0SystemId << "\n";
        nodeFile << node1Id << ",DEVICE,1,1ns," << node1SystemId << "\n";
        nodeFile.close();

        fs::path topoPath = caseDir / "topology.csv";
        std::ofstream topoFile(topoPath.string());
        topoFile << "node1,port1,node2,port2,bandwidth,delay\n";
        topoFile << node0Id << ",0," << node1Id << ",0,400Gbps,10ns\n";
        topoFile.close();

        utils::UbUtils::Get()->CreateNode(nodePath.string());
        utils::UbUtils::Get()->CreateTopo(topoPath.string());

        Ptr<Node> n0 = NodeList::GetNode(beforeNodes);
        Ptr<Node> n1 = NodeList::GetNode(beforeNodes + 1);
        Ptr<UbPort> p0 = DynamicCast<UbPort>(n0->GetDevice(0));
        Ptr<UbPort> p1 = DynamicCast<UbPort>(n1->GetDevice(0));
        Ptr<Channel> channel = p0->GetChannel();

        NS_TEST_ASSERT_MSG_NE(channel, nullptr, "Port channel should be created");
        NS_TEST_ASSERT_MSG_EQ(channel->GetInstanceTypeId().GetName(), std::string("ns3::UbLink"),
                              "Same MPI rank packed systemId should keep a local UbLink");
        NS_TEST_ASSERT_MSG_EQ(p0->HasMpiReceive(), false,
                              "Local link should not enable MPI receive on the first endpoint");
        NS_TEST_ASSERT_MSG_EQ(p1->HasMpiReceive(), false,
                              "Local link should not enable MPI receive on the second endpoint");
        NS_TEST_ASSERT_MSG_EQ(p1->GetChannel(), p0->GetChannel(), "Both ports should share the same local link");

        fs::remove_all(caseDir, ec);
    }
};
#endif

class UbCreateTpPreloadInstancesTest : public TestCase
{
  public:
    UbCreateTpPreloadInstancesTest()
        : TestCase("UnifiedBus - CreateTp preloads TP instances from config")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        const uint32_t beforeNodes = NodeList::GetNNodes();
        auto uniqueSuffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        fs::path caseDir = fs::temp_directory_path() / ("ub-create-tp-test-" + uniqueSuffix);
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(caseDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary case directory creation should succeed");

        const uint32_t node0Id = beforeNodes;
        const uint32_t node1Id = beforeNodes + 1;

        fs::path nodePath = caseDir / "node.csv";
        std::ofstream nodeFile(nodePath.string());
        nodeFile << "nodeId,nodeType,portNum,forwardDelay,systemId\n";
        nodeFile << node0Id << ",DEVICE,1,1ns,0\n";
        nodeFile << node1Id << ",DEVICE,1,1ns,1\n";
        nodeFile.close();

        fs::path tpPath = caseDir / "transport_channel.csv";
        std::ofstream tpFile(tpPath.string());
        tpFile << "nodeId1,portId1,tpn1,nodeId2,portId2,tpn2,priority,metric\n";
        tpFile << node0Id << ",0,11," << node1Id << ",0,22,7,1\n";
        tpFile.close();

        utils::UbUtils::Get()->CreateNode(nodePath.string());
        utils::UbUtils::Get()->CreateTp(tpPath.string());

        Ptr<Node> n0 = NodeList::GetNode(beforeNodes);
        Ptr<Node> n1 = NodeList::GetNode(beforeNodes + 1);
        Ptr<UbController> c0 = n0->GetObject<UbController>();
        Ptr<UbController> c1 = n1->GetObject<UbController>();

        NS_TEST_ASSERT_MSG_EQ(c0->IsTPExists(11), true, "Source-side TP should be preloaded from config");
        NS_TEST_ASSERT_MSG_EQ(c1->IsTPExists(22), true, "Destination-side TP should be preloaded from config");

        fs::remove_all(caseDir, ec);
    }
};

class UbTraceDirSetupTest : public TestCase
{
  public:
    UbTraceDirSetupTest()
        : TestCase("UnifiedBus - Trace directory setup tolerates missing runlog")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        auto uniqueSuffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        fs::path caseDir = fs::temp_directory_path() / ("ub-trace-dir-test-" + uniqueSuffix);
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(caseDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary case directory creation should succeed");

        fs::path configPath = caseDir / "network_attribute.txt";
        std::string tracePath = utils::UbUtils::PrepareTraceDir(configPath.string());

        NS_TEST_ASSERT_MSG_EQ(fs::exists(caseDir / "runlog"), true, "runlog directory should be created");
        NS_TEST_ASSERT_MSG_EQ(tracePath.empty(), false, "Returned trace path should not be empty");

        std::ofstream staleFile((caseDir / "runlog" / "stale.tr").string());
        staleFile << "stale";
        staleFile.close();

        tracePath = utils::UbUtils::PrepareTraceDir(configPath.string());
        NS_TEST_ASSERT_MSG_EQ(fs::exists(caseDir / "runlog" / "stale.tr"), false, "Existing runlog contents should be removed");
        NS_TEST_ASSERT_MSG_EQ(fs::exists(caseDir / "runlog"), true, "runlog directory should be recreated");

        fs::remove_all(caseDir, ec);
    }
};

class UbAlgorithmTraceGateDefaultOffTest : public TestCase
{
  public:
    UbAlgorithmTraceGateDefaultOffTest()
        : TestCase("UnifiedBus - algorithm trace files stay off by default even when trace path exists")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        const auto uniqueSuffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const fs::path caseDir =
            fs::temp_directory_path() / ("ub-algo-trace-default-off-" + uniqueSuffix);
        const fs::path runlogDir = caseDir / "runlog";
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(runlogDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary runlog directory creation should succeed");

        utils::UbUtils::Get()->Destroy();
        GlobalValue::Bind("UB_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_FLOW_CONTROL_TRACE_ENABLE", BooleanValue(false));
        GlobalValue::Bind("UB_CONGESTION_CONTROL_TRACE_ENABLE", BooleanValue(false));

        utils::UbUtils::SetTracePathForTest(caseDir.string());

        utils::UbUtils::PfcStateNotify(3, 1, "PAUSE", 7, 12345);
        utils::UbUtils::DcqcnMarkNotify(3, 2, 67890, 0.5);
        utils::UbUtils::Get()->Destroy();

        NS_TEST_ASSERT_MSG_EQ(fs::exists(runlogDir / "PfcTrace_node_3_port_1.tr"),
                              false,
                              "Flow-control trace should stay off by default");
        NS_TEST_ASSERT_MSG_EQ(fs::exists(runlogDir / "DcqcnMarkTrace_node_3_port_2.tr"),
                              false,
                              "Congestion-control trace should stay off by default");

        fs::remove_all(caseDir, ec);
    }
};

class UbAlgorithmTraceCategoryGateTest : public TestCase
{
  public:
    UbAlgorithmTraceCategoryGateTest()
        : TestCase("UnifiedBus - algorithm trace category gates independently control flow and congestion traces")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        const auto uniqueSuffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const fs::path caseDir =
            fs::temp_directory_path() / ("ub-algo-trace-category-gate-" + uniqueSuffix);
        const fs::path runlogDir = caseDir / "runlog";
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(runlogDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary runlog directory creation should succeed");

        utils::UbUtils::Get()->Destroy();
        GlobalValue::Bind("UB_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_FLOW_CONTROL_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_CONGESTION_CONTROL_TRACE_ENABLE", BooleanValue(false));
        utils::UbUtils::SetTracePathForTest(caseDir.string());

        utils::UbUtils::PfcStateNotify(5, 0, "PAUSE", 7, 222);
        utils::UbUtils::DcqcnMarkNotify(5, 0, 333, 0.25);
        utils::UbUtils::Get()->Destroy();

        NS_TEST_ASSERT_MSG_EQ(fs::exists(runlogDir / "PfcTrace_node_5_port_0.tr"),
                              true,
                              "Flow-control trace file should be emitted when flow-control gate is on");
        NS_TEST_ASSERT_MSG_EQ(fs::exists(runlogDir / "DcqcnMarkTrace_node_5_port_0.tr"),
                              false,
                              "Congestion-control trace file should stay off when its gate is off");

        ec.clear();
        fs::remove_all(runlogDir, ec);
        fs::create_directories(runlogDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary runlog directory reset should succeed");

        utils::UbUtils::Get()->Destroy();
        GlobalValue::Bind("UB_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_FLOW_CONTROL_TRACE_ENABLE", BooleanValue(false));
        GlobalValue::Bind("UB_CONGESTION_CONTROL_TRACE_ENABLE", BooleanValue(true));
        utils::UbUtils::SetTracePathForTest(caseDir.string());

        utils::UbUtils::PfcStateNotify(6, 1, "PAUSE", 7, 444);
        utils::UbUtils::DcqcnMarkNotify(6, 1, 555, 0.75);
        utils::UbUtils::Get()->Destroy();

        NS_TEST_ASSERT_MSG_EQ(fs::exists(runlogDir / "PfcTrace_node_6_port_1.tr"),
                              false,
                              "Flow-control trace file should stay off when flow-control gate is off");
        NS_TEST_ASSERT_MSG_EQ(fs::exists(runlogDir / "DcqcnMarkTrace_node_6_port_1.tr"),
                              true,
                              "Congestion-control trace file should be emitted when its gate is on");
        fs::remove_all(caseDir, ec);
    }
};

class UbQueueTraceCategoryGateTest : public TestCase
{
  public:
    UbQueueTraceCategoryGateTest()
        : TestCase("UnifiedBus - queue trace gate independently controls queue trace files")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        const auto uniqueSuffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const fs::path caseDir =
            fs::temp_directory_path() / ("ub-queue-trace-category-gate-" + uniqueSuffix);
        const fs::path runlogDir = caseDir / "runlog";
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(runlogDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary runlog directory creation should succeed");

        utils::UbUtils::Get()->Destroy();
        GlobalValue::Bind("UB_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_QUEUE_TRACE_ENABLE", BooleanValue(false));
        GlobalValue::Bind("UB_QUEUE_SAMPLE_INTERVAL_NS", UintegerValue(1000));
        utils::UbUtils::SetTracePathForTest(caseDir.string());

        BuildLocalTpTopology();
        utils::UbUtils::Get()->TopoTraceConnect();
        Simulator::Stop(NanoSeconds(1500));
        Simulator::Run();
        Simulator::Destroy();
        utils::UbUtils::Get()->Destroy();

        NS_TEST_ASSERT_MSG_EQ(fs::exists(runlogDir / "QueueTrace_node_0_port_0.tr"),
                              false,
                              "Queue trace file should stay off when queue-trace gate is off");

        ec.clear();
        fs::remove_all(runlogDir, ec);
        fs::create_directories(runlogDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary runlog directory reset should succeed");

        utils::UbUtils::Get()->Destroy();
        GlobalValue::Bind("UB_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_QUEUE_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_QUEUE_SAMPLE_INTERVAL_NS", UintegerValue(1000));
        utils::UbUtils::SetTracePathForTest(caseDir.string());

        BuildLocalTpTopology();
        utils::UbUtils::Get()->TopoTraceConnect();
        Simulator::Stop(NanoSeconds(1500));
        Simulator::Run();
        Simulator::Destroy();
        utils::UbUtils::Get()->Destroy();

        const fs::path queueTraceFile = runlogDir / "QueueTrace_node_0_port_0.tr";
        NS_TEST_ASSERT_MSG_EQ(fs::exists(queueTraceFile),
                              true,
                              "Queue trace file should be emitted when queue-trace gate is on");
        const std::string text = std::ifstream(queueTraceFile).good()
                                     ? [&queueTraceFile]() {
                                           std::ifstream input(queueTraceFile);
                                           std::ostringstream oss;
                                           oss << input.rdbuf();
                                           return oss.str();
                                       }()
                                     : "";
        NS_TEST_ASSERT_MSG_NE(text.find("source: SAMPLE"),
                              std::string::npos,
                              "Queue trace file should contain sampled queue state when gate is on");

        fs::remove_all(caseDir, ec);
    }
};

namespace utils
{

class UbQueueSamplerEventRetentionTest : public TestCase
{
  public:
    UbQueueSamplerEventRetentionTest()
        : TestCase("UnifiedBus - queue sampler keeps only one pending event per port")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        const auto uniqueSuffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const fs::path caseDir =
            fs::temp_directory_path() / ("ub-queue-sampler-retention-" + uniqueSuffix);
        const fs::path runlogDir = caseDir / "runlog";
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(runlogDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary runlog directory creation should succeed");

        UbUtils::Get()->Destroy();
        GlobalValue::Bind("UB_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_QUEUE_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_QUEUE_SAMPLE_INTERVAL_NS", UintegerValue(10));
        UbUtils::SetTracePathForTest(caseDir.string());

        BuildLocalTpTopology();
        UbUtils::Get()->TopoTraceConnect();
        const std::size_t initialSamplerEvents = UbUtils::queue_sampler_events.size();
        NS_TEST_ASSERT_MSG_NE(initialSamplerEvents,
                              0u,
                              "Queue sampler should track at least one switch port");

        Simulator::Stop(NanoSeconds(95));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(UbUtils::queue_sampler_events.size(),
                              initialSamplerEvents,
                              "Queue sampler should not retain historical tick EventIds");

        Simulator::Destroy();
        UbUtils::Get()->Destroy();
        fs::remove_all(caseDir, ec);
    }
};

class UbTraceFileConcurrencyTest : public TestCase
{
  public:
    UbTraceFileConcurrencyTest()
        : TestCase("UnifiedBus - trace batching keeps file contents intact under concurrent writers")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        constexpr uint32_t kThreadCount = 8;
        constexpr uint32_t kLinesPerThread = 4000;
        const auto uniqueSuffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const fs::path caseDir =
            fs::temp_directory_path() / ("ub-trace-concurrency-test-" + uniqueSuffix);
        const fs::path runlogDir = caseDir / "runlog";
        const fs::path traceFile = runlogDir / "concurrent.tr";

        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(runlogDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary runlog directory creation should succeed");

        UbUtils::Get()->Destroy();
        UbUtils::trace_path = caseDir.string();
        if (!UbUtils::trace_path.empty() &&
            UbUtils::trace_path.back() != fs::path::preferred_separator)
        {
            UbUtils::trace_path.push_back(fs::path::preferred_separator);
        }

        std::atomic<bool> start{false};
        std::vector<std::thread> writers;
        writers.reserve(kThreadCount);

        for (uint32_t tid = 0; tid < kThreadCount; ++tid)
        {
            writers.emplace_back([&, tid]() {
                while (!start.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }

                const std::string payload(96, static_cast<char>('A' + (tid % 26)));
                for (uint32_t line = 0; line < kLinesPerThread; ++line)
                {
                    std::ostringstream oss;
                    oss << "thread=" << tid << " line=" << line << " payload=" << payload;
                    UbUtils::PrintTraceInfoNoTs(traceFile.string(), oss.str());
                    if ((line & 0x3f) == 0)
                    {
                        std::this_thread::yield();
                    }
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& writer : writers)
        {
            writer.join();
        }

        UbUtils::Get()->Destroy();

        std::ifstream input(traceFile);
        NS_TEST_ASSERT_MSG_EQ(input.is_open(), true, "Concurrent trace file should be created");

        uint64_t lineCount = 0;
        std::string line;
        while (std::getline(input, line))
        {
            ++lineCount;
        }

        NS_TEST_ASSERT_MSG_EQ(lineCount,
                              static_cast<uint64_t>(kThreadCount) * kLinesPerThread,
                              "Concurrent trace writes should preserve every line");

        fs::remove_all(caseDir, ec);
    }
};

} // namespace utils

class UbMpiRankExtractionHelperTest : public TestCase
{
  public:
    UbMpiRankExtractionHelperTest()
        : TestCase("UnifiedBus - ExtractMpiRank follows MPI rank encoding rules")
    {
    }

    void DoRun() override
    {
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::ExtractMpiRank(7u),
                              7u,
                              "Plain systemId should preserve rank value");

        const uint32_t packedSystemId = (0x1234u << 16) | 0x002au;
#ifdef NS3_MTP
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::ExtractMpiRank(packedSystemId),
                              0x002au,
                              "MTP packed systemId should use low 16 bits as MPI rank");
#else
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::ExtractMpiRank(packedSystemId),
                              packedSystemId,
                              "Non-MTP build should use the full systemId as MPI rank");
#endif
    }
};

class UbSameMpiRankHelperTest : public TestCase
{
  public:
    UbSameMpiRankHelperTest()
        : TestCase("UnifiedBus - IsSameMpiRank compares MPI rank instead of raw packed systemId")
    {
    }

    void DoRun() override
    {
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSameMpiRank(5u, 5u),
                              true,
                              "Identical plain systemId values should be on the same MPI rank");
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSameMpiRank(5u, 6u),
                              false,
                              "Different plain systemId values should be on different MPI ranks");

        const uint32_t lhsPacked = (0x0001u << 16) | 0x0009u;
        const uint32_t rhsSameRankPacked = (0x0002u << 16) | 0x0009u;
        const uint32_t rhsDifferentRankPacked = (0x0002u << 16) | 0x000au;

#ifdef NS3_MTP
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSameMpiRank(lhsPacked, rhsSameRankPacked),
                              true,
                              "MTP packed systemId values with the same low 16 bits should match");
#else
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSameMpiRank(lhsPacked, rhsSameRankPacked),
                              false,
                              "Non-MTP build should compare full systemId values");
#endif
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSameMpiRank(lhsPacked, rhsDifferentRankPacked),
                              false,
                              "Different MPI rank encodings should not match");
    }
};

class UbSystemOwnedByRankHelperTest : public TestCase
{
  public:
    UbSystemOwnedByRankHelperTest()
        : TestCase("UnifiedBus - IsSystemOwnedByRank follows packed MPI ownership rules")
    {
    }

    void DoRun() override
    {
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSystemOwnedByRank(7u, 7u),
                              true,
                              "Plain systemId should be owned by the same MPI rank");
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSystemOwnedByRank(7u, 6u),
                              false,
                              "Plain systemId should not be owned by a different MPI rank");

        const uint32_t packedSystemId = (0x1234u << 16) | 0x0009u;

#ifdef NS3_MTP
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSystemOwnedByRank(packedSystemId, 0x0009u),
                              true,
                              "Packed systemId should be owned by the matching low-16-bit MPI rank");
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSystemOwnedByRank(packedSystemId, 0x000au),
                              false,
                              "Packed systemId should not be owned by a different low-16-bit MPI rank");
#else
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSystemOwnedByRank(packedSystemId, packedSystemId),
                              true,
                              "Non-MTP build should treat the full systemId as the owner key");
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSystemOwnedByRank(packedSystemId, 0x0009u),
                              false,
                              "Non-MTP build should not mask packed systemId values");
#endif
    }
};

class UbQueueManagerReserveOnlyAdmissionTest : public TestCase
{
  public:
    UbQueueManagerReserveOnlyAdmissionTest()
        : TestCase("UnifiedBus - reserve-only admission allows exact-fit enqueue and keeps shared state at zero")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kReserveBytes = 100;
        constexpr uint32_t kPriority = 1;
        Ptr<UbQueueManager> queueManager = CreateQueueManagerFixture(/*ports*/ 1,
                                                                    /*vlNum*/ 2,
                                                                    kReserveBytes,
                                                                    /*sharedPoolBytes*/ 0,
                                                                    /*headroomPerPortBytes*/ 0,
                                                                    /*resumeGapBytes*/ 16,
                                                                    /*alphaShift*/ 1);

        NS_TEST_ASSERT_MSG_EQ(queueManager->CheckInPortSpace(0, kPriority, kReserveBytes),
                              true,
                              "Reserve-only admission should accept a packet that exactly fills the reserved bytes");

        queueManager->PushToVoq(0, 0, kPriority, kReserveBytes);

        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressNonHeadroomBytes(0, kPriority),
                              kReserveBytes,
                              "Exact-fit enqueue should be reflected in ingress usage");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressSharedBytes(0, kPriority),
                              0u,
                              "Reserve-only admission should not accumulate shared usage");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressHeadroomBytes(0, kPriority),
                              0u,
                              "Reserve-only admission should not accumulate headroom usage");
        NS_TEST_ASSERT_MSG_EQ(queueManager->CheckInPortSpace(0, kPriority, 1),
                              false,
                              "Reserve-only admission should reject packets once the reserved bytes are fully consumed");

        queueManager->PopFromVoq(0, 0, kPriority, kReserveBytes);

        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressNonHeadroomBytes(0, kPriority),
                              0u,
                              "PopFromVoq should release reserve-only ingress accounting");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetSwitchBufferOccupancy().shared_pool_used_bytes,
                              0u,
                              "Reserve-only admission should keep shared-pool accounting at zero after drain");
        Config::Reset();
    }
};

class UbSlidingBitmapWindowAdvancesWithoutLosingOutOfOrderMarksTest : public TestCase
{
  public:
    UbSlidingBitmapWindowAdvancesWithoutLosingOutOfOrderMarksTest()
        : TestCase("UnifiedBus - sliding bitmap window preserves out-of-order marks while advancing")
    {
    }

    void DoRun() override
    {
        UbSlidingBitmapWindow window(8);
        window.Reset(100);

        NS_TEST_ASSERT_MSG_EQ(window.Mark(102),
                              true,
                              "Marking an in-window out-of-order sequence should succeed");
        NS_TEST_ASSERT_MSG_EQ(window.AdvanceContiguous(),
                              0u,
                              "Window must not advance before the gap closes");
        NS_TEST_ASSERT_MSG_EQ(window.GetBase(), 100u, "Base should stay at the first missing sequence");

        NS_TEST_ASSERT_MSG_EQ(window.Mark(100),
                              true,
                              "Marking the base sequence should succeed");
        NS_TEST_ASSERT_MSG_EQ(window.AdvanceContiguous(),
                              1u,
                              "Closing the base gap should advance exactly one slot");
        NS_TEST_ASSERT_MSG_EQ(window.GetBase(), 101u, "Base should move to the next missing sequence");

        NS_TEST_ASSERT_MSG_EQ(window.Mark(101),
                              true,
                              "Marking the next gap should succeed");
        NS_TEST_ASSERT_MSG_EQ(window.AdvanceContiguous(),
                              2u,
                              "Advance should consume both the newly filled gap and the preserved out-of-order mark");
        NS_TEST_ASSERT_MSG_EQ(window.GetBase(), 103u, "Base should now point past the contiguous run");
    }
};

class UbSlidingBitmapWindowReusesSlotsWithoutGhostMarksTest : public TestCase
{
  public:
    UbSlidingBitmapWindowReusesSlotsWithoutGhostMarksTest()
        : TestCase("UnifiedBus - sliding bitmap window reuses slots without reviving stale marks")
    {
    }

    void DoRun() override
    {
        UbSlidingBitmapWindow window(4);
        window.Reset(10);

        NS_TEST_ASSERT_MSG_EQ(window.Mark(10), true, "Base mark should succeed");
        NS_TEST_ASSERT_MSG_EQ(window.Mark(11), true, "Second mark should succeed");
        NS_TEST_ASSERT_MSG_EQ(window.AdvanceContiguous(), 2u, "Two contiguous marks should advance by two");
        NS_TEST_ASSERT_MSG_EQ(window.GetBase(), 12u, "Base should move forward after consuming two marks");

        NS_TEST_ASSERT_MSG_EQ(window.Contains(10),
                              false,
                              "Consumed sequences must not remain marked after their slots are reused");
        NS_TEST_ASSERT_MSG_EQ(window.Mark(14), true, "Reused slot should accept a new in-window sequence");
        NS_TEST_ASSERT_MSG_EQ(window.Mark(12), true, "New base should still be markable after slot reuse");
        NS_TEST_ASSERT_MSG_EQ(window.AdvanceContiguous(), 1u, "Only the rebuilt contiguous prefix should advance");
        NS_TEST_ASSERT_MSG_EQ(window.GetBase(), 13u, "Base should stop at the next missing sequence");
        NS_TEST_ASSERT_MSG_EQ(window.Contains(14),
                              true,
                              "Future out-of-order marks should remain visible after partial advance");
    }
};

class UbBusyPortArrivalPrefetchesNextPacketTest : public TestCase
{
  public:
    UbBusyPortArrivalPrefetchesNextPacketTest()
        : TestCase("UnifiedBus - busy port arrival still prefetches next packet into egress queue")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateSinglePortPfcFixture(FcType::NONE,
                                                  1024,
                                                  0,
                                                  0,
                                                  0,
                                                  0,
                                                  0,
                                                  0);
        fixture.sw->SetCongestionCtrl(CreateObject<UbCongestionControl>());

        fixture.port->NotifyLinkUp();
        fixture.port->SetSendState(SendState::BUSY);
        NS_TEST_ASSERT_MSG_EQ(fixture.port->GetUbQueue()->IsEmpty(),
                              true,
                              "Fixture should start with an empty egress queue");

        fixture.sw->SendPacket(Create<Packet>(128), 0, 0, 1);
        NS_TEST_ASSERT_MSG_EQ(fixture.port->GetUbQueue()->IsEmpty(),
                              true,
                              "Busy ports should not dequeue immediately before allocator latency elapses");

        Simulator::Stop(NanoSeconds(11));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(fixture.port->GetUbQueue()->IsEmpty(),
                              false,
                              "A packet arriving while the out port is busy should still be prefetched "
                              "into the egress queue after AllocationTime elapses");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbPacketSprayUsesEvenRoundRobinAcrossEqualPortsTest : public TestCase
{
  public:
    UbPacketSprayUsesEvenRoundRobinAcrossEqualPortsTest()
        : TestCase("UnifiedBus - packet spray evenly round-robins across equal-cost ports")
    {
    }

    void DoRun() override
    {
        Ptr<UbRoutingProcess> routing = CreateObject<UbRoutingProcess>();
        const std::vector<uint16_t> equalPorts = {10, 11, 12};
        routing->AddShortestRoute(NodeIdToIp(42).Get(), equalPorts);

        RoutingKey rtKey{};
        rtKey.sip = NodeIdToIp(1).Get();
        rtKey.dip = NodeIdToIp(42).Get();
        rtKey.dport = 7;
        rtKey.priority = 2;
        rtKey.useShortestPath = true;
        rtKey.usePacketSpray = true;

        std::map<uint16_t, uint32_t> counts;
        for (uint16_t port : equalPorts)
        {
            counts[port] = 0;
        }

        for (uint32_t spraySalt = 1; spraySalt <= 977; ++spraySalt)
        {
            rtKey.sport = static_cast<uint16_t>(spraySalt);
            bool selectedShortestPath = false;
            const int outPort = routing->GetOutPort(rtKey, selectedShortestPath);

            NS_TEST_ASSERT_MSG_EQ(selectedShortestPath,
                                  true,
                                  "Packet spray should stay within the equal shortest-path set");
            NS_TEST_ASSERT_MSG_EQ((counts.count(static_cast<uint16_t>(outPort)) == 1),
                                  true,
                                  "Packet spray must select one of the configured equal-cost ports");
            counts[static_cast<uint16_t>(outPort)]++;
        }

        uint32_t minCount = std::numeric_limits<uint32_t>::max();
        uint32_t maxCount = 0;
        for (uint16_t port : equalPorts)
        {
            minCount = std::min(minCount, counts[port]);
            maxCount = std::max(maxCount, counts[port]);
        }

        NS_TEST_ASSERT_MSG_EQ(((maxCount - minCount) <= 1),
                              true,
                              "Packet spray should differ by at most one packet across equal-cost ports");
    }
};

class UbRoundRobinAllocatorSeedsDifferentInitialPhasesPerOutPortTest : public TestCase
{
  public:
    UbRoundRobinAllocatorSeedsDifferentInitialPhasesPerOutPortTest()
        : TestCase("UnifiedBus - round robin allocator seeds different initial queue phases per out port")
    {
    }

    void DoRun() override
    {
        Config::Reset();

        Ptr<Node> node = CreateObject<Node>(0);
        InitNode(node, UB_SWITCH, 4);

        Ptr<UbSwitch> sw = node->GetObject<UbSwitch>();
        Ptr<UbRoundRobinAllocator> allocator = DynamicCast<UbRoundRobinAllocator>(sw->GetAllocator());
        NS_TEST_ASSERT_MSG_NE(allocator, nullptr, "Default switch allocator should be round robin");

        constexpr uint32_t kPriority = 1;
        for (uint32_t outPort = 0; outPort < 4; ++outPort)
        {
            for (uint32_t inPort = 0; inPort < 4; ++inPort)
            {
                sw->PushPacketToVoq(Create<Packet>(64), outPort, kPriority, inPort);
            }
        }

        std::vector<uint32_t> firstInPorts;
        for (uint32_t outPort = 0; outPort < 4; ++outPort)
        {
            Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(outPort));
            Ptr<UbIngressQueue> selected = allocator->SelectNextIngressQueue(port);
            NS_TEST_ASSERT_MSG_NE(selected, nullptr, "Symmetric non-empty VOQs should yield a selected queue");
            firstInPorts.push_back(selected->GetInPortId());
        }

        NS_TEST_ASSERT_MSG_EQ(firstInPorts[0], 0u, "Port 0 should keep the baseline initial phase");
        NS_TEST_ASSERT_MSG_EQ(firstInPorts[1], 1u, "Port 1 should not start from the same ingress queue as port 0");
        NS_TEST_ASSERT_MSG_EQ(firstInPorts[2], 2u, "Port 2 should get its own initial queue phase");
        NS_TEST_ASSERT_MSG_EQ(firstInPorts[3], 3u, "Port 3 should get its own initial queue phase");

        Simulator::Destroy();
        Config::Reset();
    }
};

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
    AddTestCase(new UbDcqcnFactoryCreatesHostAndSwitchTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnHostAckCeTphDefaultTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCongestionControlDisabledFactoryStillReturnsObjectTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbCaqmHostRttUsesSmoothedEstimateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchAttachDisabledCongestionControlTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchAttachEnabledCongestionControlTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnCnpHeaderRoundTripTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnSwitchMarksFecnTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnSwitchNoMarkPreservesPacketHeadersTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnReceiverSuppressesBurstCnpTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnControlPriorityPrefersCnpTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbAckWithoutCetphCarriesNoCetphHeaderTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnCnpOpcodeIsValidTransportOpcodeTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnSwitchDoesNotRemarkMarkedFecnTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnSenderCutsRateOnCnpTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnCnpDoesNotAdvanceAckStateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnRecoveryTimerIncreasesRateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnByteCounterIncreasesRateBeforeTimerTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnHyperIncreaseUsesHaiRateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnRateNeverExceedsLineRateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnBusyHostStartsSecondFlowAtInitialRateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnPacingWakeupResumesQueuedSendTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnCnpCutRescalesOutstandingPacingDebtTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnCompletedFlowReleasesHostActiveSlotTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnIdleFlowCancelsRecoveryStateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbUrmaReadWqeMetadataPropagationTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbUrmaWriteCompletionNeedsTransactionResponseTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbUrmaReadCompletionNeedsReadResponseTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbUrmaReadMultiPacketResponseCountTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbUrmaReadMultiSliceRequestPacketSemanticsTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTraceDirSetupTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbAlgorithmTraceGateDefaultOffTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbAlgorithmTraceCategoryGateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueTraceCategoryGateTest(), TestCase::Duration::QUICK);
    AddTestCase(new utils::UbQueueSamplerEventRetentionTest(), TestCase::Duration::QUICK);
    AddTestCase(new utils::UbTraceFileConcurrencyTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbMpiRankExtractionHelperTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSameMpiRankHelperTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSystemOwnedByRankHelperTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCreateNodeSystemIdTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchFlowControlModeAttributeTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchLocalRuntimeConfigTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPortSetDataRateWithoutCongestionControlTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchNotifySwitchDequeueWithoutCongestionControlTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbControlFrameUsesDedicatedAccountingTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcFixedModeCountsHeadroomTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcDynamicModePauseResumeTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerDynamicPfcDecisionApiTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcDynamicModeXoffZeroEmptyQueueTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcPaperDynamicModePauseResumeTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerPaperPfcDecisionApiTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcPaperDynamicModeIgnoresAlphaShiftForAdmissionTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcPaperDynamicModeUsesRealGlobalOccupancyTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerTotalBufferedBytesTracksEgressTransferTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbPfcForwardingUsesIngressPortConfigTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbFlowControlReleaseHookRunsAfterIngressDequeueTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbAllocatorKeepsIngressPacketWhenEgressQueueIsFullTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerReserveOnlyAdmissionTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerStickyHeadroomAccountingTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerIngressPortOccupancyViewTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSlidingBitmapWindowAdvancesWithoutLosingOutOfOrderMarksTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSlidingBitmapWindowReusesSlotsWithoutGhostMarksTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbBusyPortArrivalPrefetchesNextPacketTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbRoundRobinAllocatorSeedsDifferentInitialPhasesPerOutPortTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbPacketSprayUsesEvenRoundRobinAcrossEqualPortsTest(),
                TestCase::Duration::QUICK);
#ifndef _WIN32
    AddTestCase(new UbDataPacketHeaderRejectsPriorityZeroTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSendControlFrameRejectsDataPacketTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcRejectsZeroCellGeometryTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcFixedRejectsNegativeThresholdTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchLocalCbfcConfigTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcPiggybackTargetVlCanDifferFromPacketVlTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcPiggybackRestoreClearsLocalCreditBitTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcControlFrameFallbackWithoutDataPacketTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcCtrlCrdRtrThresholdTriggersControlFrameTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcForcedControlReturnFlushesAllEligibleVlsTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcForcedControlReturnChunksControlFramesAtSixBitLimitTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcForwardedReleaseUsesIngressPortThresholdStateTest(), TestCase::Duration::QUICK);
#endif
#ifdef NS3_MPI
    AddTestCase(new UbCreateTopoRemoteLinkTest(), TestCase::Duration::QUICK);
#endif
#if defined(NS3_MPI) && defined(NS3_MTP)
    AddTestCase(new UbCreateTopoPackedSystemIdLocalLinkTest(), TestCase::Duration::QUICK);
#endif
    AddTestCase(new UbCreateTpPreloadInstancesTest(), TestCase::Duration::QUICK);
}

// Register the test suite
static UbTestSuite g_ubTestSuite;

class UbOutOfOrderRegressionTestSuite : public TestSuite
{
  public:
    UbOutOfOrderRegressionTestSuite()
        : TestSuite("unified-bus-transport-ooo-regression", Type::UNIT)
    {
        AddTestCase(new UbUrmaWriteOutOfOrderRequestSliceCompletionTest(),
                    TestCase::Duration::QUICK);
    }
};

static UbOutOfOrderRegressionTestSuite g_ubOutOfOrderRegressionTestSuite;

namespace
{

std::filesystem::path
LocateRepoRoot();

std::pair<int, std::string>
RunQuickExampleCommand(const std::string& testFile,
                       const std::string& extraArgs,
                       const std::string& commandPrefix,
                       const std::string& casePathRelative = "");

std::pair<int, std::string>
RunNs3RunCommand(const std::string& testFile,
                 const std::string& programAndArgs,
                 const std::string& commandPrefix = "",
                 bool noBuild = true);

} // namespace

#ifdef NS3_MTP
class UbQuickExampleLocalMtpSystemTest : public TestCase
{
  public:
    UbQuickExampleLocalMtpSystemTest()
        : TestCase("UnifiedBus - ub-quick-example local MTP mode runs without MPI init failure")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename("ub-quick-example-local-mtp.log"),
                                   "--mtp-threads=2",
                                   "",
                                   "scratch/2nodes_single-tp");

        NS_TEST_ASSERT_MSG_EQ(status,
                              0,
                              "ub-quick-example local MTP mode should exit successfully");
        NS_TEST_ASSERT_MSG_EQ(output.find("MPI_Testany() ... before MPI_INIT"), std::string::npos,
                              "ub-quick-example local MTP mode should not touch MPI before MPI_Init");
    }
};
#endif

#ifdef NS3_MPI
class UbQuickExampleSpoofedMpiEnvSystemTest : public TestCase
{
  public:
    UbQuickExampleSpoofedMpiEnvSystemTest()
        : TestCase("UnifiedBus - ub-quick-example ignores spoofed MPI env without launcher")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename("ub-quick-example-spoofed-mpi-env.log"),
                                   "--test",
                                   "env OMPI_COMM_WORLD_SIZE=2",
                                   "scratch/2nodes_single-tp");

        NS_TEST_ASSERT_MSG_EQ(status,
                              0,
                              "spoofed MPI environment without launcher should stay on local runtime");
        NS_TEST_ASSERT_MSG_EQ(output.find(UbTrafficGen::GetMultiProcessUnsupportedMessage()),
                              std::string::npos,
                              "spoofed MPI environment should not trigger the multi-process rejection");
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "spoofed MPI environment case should still complete locally");
    }
};
#endif

namespace
{

bool
HasQuickExampleBinary(const std::filesystem::path& repoRoot)
{
    return std::filesystem::exists(
               repoRoot / "build/src/unified-bus/examples/ns3.44-ub-quick-example-default") ||
           std::filesystem::exists(repoRoot / "build/src/unified-bus/examples/ns3.44-ub-quick-example");
}

std::filesystem::path
LocateRepoRoot()
{
    std::filesystem::path repoRoot = PROJECT_SOURCE_PATH;
    if (HasQuickExampleBinary(repoRoot))
    {
        return repoRoot;
    }

    repoRoot = NS_TEST_SOURCEDIR;
    if (repoRoot.is_relative())
    {
        repoRoot = std::filesystem::path(PROJECT_SOURCE_PATH) / repoRoot;
    }
    for (uint32_t i = 0; i < 4 && !HasQuickExampleBinary(repoRoot); ++i)
    {
        repoRoot = repoRoot.parent_path();
    }
    return repoRoot;
}

std::filesystem::path
LocateQuickExampleBinary(const std::filesystem::path& repoRoot)
{
    const std::vector<std::filesystem::path> candidates = {
        "build/src/unified-bus/examples/ns3.44-ub-quick-example",
        "build/src/unified-bus/examples/ns3.44-ub-quick-example-default",
    };

    for (const auto& candidate : candidates)
    {
        const std::filesystem::path binaryPath = repoRoot / candidate;
        if (std::filesystem::exists(binaryPath))
        {
            return binaryPath;
        }
    }

    const std::filesystem::path examplesDir = repoRoot / "build/src/unified-bus/examples";
    if (std::filesystem::exists(examplesDir))
    {
        for (const auto& entry : std::filesystem::directory_iterator(examplesDir))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            const std::string filename = entry.path().filename().string();
            if (filename.find("ub-quick-example") != std::string::npos)
            {
                return entry.path();
            }
        }
    }

    return repoRoot / candidates.front();
}

std::pair<int, std::string>
RunQuickExampleCommand(const std::string& testFile,
                       const std::string& extraArgs,
                       const std::string& commandPrefix,
                       const std::string& casePathRelative)
{
    const std::filesystem::path repoRoot = LocateRepoRoot();
    const std::filesystem::path binaryPath = LocateQuickExampleBinary(repoRoot);

    std::string command;
    if (!commandPrefix.empty())
    {
        command += commandPrefix + " ";
    }
    command += "\"" + binaryPath.string() + "\"";
    if (!casePathRelative.empty())
    {
        const std::filesystem::path casePath = repoRoot / casePathRelative;
        command += " --case-path=\"" + casePath.string() + "\"";
    }
    if (!extraArgs.empty())
    {
        command += " " + extraArgs;
    }
    command += " > \"" + testFile + "\" 2>&1";

    const int status = std::system(command.c_str());

    std::ifstream input(testFile);
    std::stringstream buffer;
    buffer << input.rdbuf();
    return {status, buffer.str()};
}

std::pair<int, std::string>
RunNs3RunCommand(const std::string& testFile,
                 const std::string& programAndArgs,
                 const std::string& commandPrefix,
                 bool noBuild)
{
    const std::filesystem::path repoRoot = LocateRepoRoot();
    const std::string pythonCommand =
        std::system("command -v python3.12 >/dev/null 2>&1") == 0 ? "python3.12" : "python3";

    std::string command;
    if (!commandPrefix.empty())
    {
        command += commandPrefix + " ";
    }
    command += pythonCommand + " \"" + (repoRoot / "ns3").string() + "\" run \"" + programAndArgs +
               "\"";
    if (noBuild)
    {
        command += " --no-build";
    }
    command += " > \"" + testFile + "\" 2>&1";

    const int status = std::system(command.c_str());

    std::ifstream input(testFile);
    std::stringstream buffer;
    buffer << input.rdbuf();
    return {status, buffer.str()};
}

std::string
NormalizeTestPath(const std::filesystem::path& path)
{
    return std::filesystem::absolute(path).lexically_normal().string();
}

std::filesystem::path
CopyCaseDirWithoutFile(const std::string& sourceCasePathRelative, const std::string& omittedFilename)
{
    namespace fs = std::filesystem;

    const fs::path repoRoot = LocateRepoRoot();
    const fs::path sourceCaseDir = repoRoot / sourceCasePathRelative;
    const auto uniqueSuffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path tempCaseDir =
        (fs::temp_directory_path() / ("ub-quick-example-case-copy-" + uniqueSuffix)).lexically_normal();

    fs::create_directories(tempCaseDir);
    for (const auto& entry : fs::directory_iterator(sourceCaseDir))
    {
        const fs::path destination = tempCaseDir / entry.path().filename();
        if (entry.path().filename() == omittedFilename)
        {
            continue;
        }
        fs::copy(entry.path(), destination, fs::copy_options::recursive);
    }

    return tempCaseDir;
}

std::filesystem::path
CopyCaseDirWithTrafficFile(const std::string& sourceCasePathRelative, const std::string& trafficCsvContent)
{
    namespace fs = std::filesystem;

    const fs::path repoRoot = LocateRepoRoot();
    const fs::path sourceCaseDir = repoRoot / sourceCasePathRelative;
    const auto uniqueSuffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path tempCaseDir =
        (fs::temp_directory_path() / ("ub-quick-example-traffic-copy-" + uniqueSuffix))
            .lexically_normal();

    fs::create_directories(tempCaseDir);
    for (const auto& entry : fs::directory_iterator(sourceCaseDir))
    {
        const fs::path destination = tempCaseDir / entry.path().filename();
        fs::copy(entry.path(), destination, fs::copy_options::recursive);
    }

    std::ofstream trafficFile(tempCaseDir / "traffic.csv");
    trafficFile << trafficCsvContent;
    trafficFile.close();

    return tempCaseDir;
}

void
ReplaceInFile(const std::filesystem::path& filePath,
              const std::string& needle,
              const std::string& replacement)
{
    std::ifstream input(filePath);
    if (!input.is_open())
    {
        throw std::runtime_error("failed to open file for rewrite helper: " + filePath.string());
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    std::string content = buffer.str();
    const auto pos = content.find(needle);
    if (pos == std::string::npos)
    {
        throw std::runtime_error("expected text not found in file rewrite helper: " + needle);
    }
    content.replace(pos, needle.size(), replacement);
    std::ofstream output(filePath, std::ios::trunc);
    if (!output.is_open())
    {
        throw std::runtime_error("failed to open file for write helper: " + filePath.string());
    }
    output << content;
}

// The old ub-local-hybrid-minimal fixture was removed as a non-core sample, so
// local single-thread quick-example tests reuse the maintained 2-node case.
constexpr const char* kLocalSingleThreadQuickExampleCase = "scratch/2nodes_single-tp";

std::pair<int, std::string>
RunQuickExampleAbsoluteCaseCommand(const std::string& testFile,
                                   const std::string& extraArgs,
                                   const std::string& commandPrefix,
                                   const std::filesystem::path& casePath)
{
    const std::filesystem::path repoRoot = LocateRepoRoot();
    const std::filesystem::path binaryPath = LocateQuickExampleBinary(repoRoot);

    std::string command;
    if (!commandPrefix.empty())
    {
        command += commandPrefix + " ";
    }
    command += "\"" + binaryPath.string() + "\"";
    command += " --case-path=\"" + casePath.string() + "\"";
    if (!extraArgs.empty())
    {
        command += " " + extraArgs;
    }
    command += " > \"" + testFile + "\" 2>&1";

    const int status = std::system(command.c_str());

    std::ifstream input(testFile);
    std::stringstream buffer;
    buffer << input.rdbuf();
    return {status, buffer.str()};
}

} // namespace

class UbQuickExampleMissingCasePathSystemTest : public TestCase
{
  public:
    UbQuickExampleMissingCasePathSystemTest()
        : TestCase("UnifiedBus - ub-quick-example rejects missing case-path")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        auto [status, output] = RunQuickExampleCommand(CreateTempDirFilename(GetName() + ".log"),
                                                       "",
                                                       "");

        NS_TEST_ASSERT_MSG_NE(status,
                              0,
                              "ub-quick-example without case-path should exit with failure");
        NS_TEST_ASSERT_MSG_NE(output.find("missing required case path (--case-path or casePath)"),
                              std::string::npos,
                              "ub-quick-example should print a clear missing case-path error");
    }
};

class UbQuickExampleMissingCaseDirSystemTest : public TestCase
{
  public:
    UbQuickExampleMissingCaseDirSystemTest()
        : TestCase("UnifiedBus - ub-quick-example rejects missing case directory")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::filesystem::path repoRoot = LocateRepoRoot();
        const std::filesystem::path missingCaseDir = repoRoot / "scratch/ub-case-does-not-exist";
        const std::string expectedError =
            "case path does not exist: " + NormalizeTestPath(missingCaseDir);
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + ".log"),
                                   "--case-path=\"" + missingCaseDir.string() + "\"",
                                   "",
                                   "");

        NS_TEST_ASSERT_MSG_NE(status,
                              0,
                              "ub-quick-example should fail when case-path directory is missing");
        NS_TEST_ASSERT_MSG_NE(output.find(expectedError),
                              std::string::npos,
                              "ub-quick-example should print a clear missing case directory error");
    }
};

class UbQuickExampleMissingCaseFileSystemTest : public TestCase
{
  public:
    UbQuickExampleMissingCaseFileSystemTest()
        : TestCase("UnifiedBus - ub-quick-example rejects case directory with missing required files")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::filesystem::path caseDir =
            std::filesystem::path(CreateTempDirFilename("ub-quick-example-missing-files-case"));
        std::filesystem::create_directories(caseDir);
        const std::filesystem::path expectedMissingFile = caseDir / "network_attribute.txt";
        const std::string expectedError =
            "missing required case file: " + NormalizeTestPath(expectedMissingFile);

        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + ".log"),
                                   "--case-path=\"" + caseDir.string() + "\"",
                                   "",
                                   "");

        NS_TEST_ASSERT_MSG_NE(status,
                              0,
                              "ub-quick-example should fail when required case files are missing");
        NS_TEST_ASSERT_MSG_NE(output.find(expectedError),
                              std::string::npos,
                              "ub-quick-example should identify the first missing required case file");
    }
};

class UbQuickExampleHelpTextSystemTest : public TestCase
{
  public:
    UbQuickExampleHelpTextSystemTest()
        : TestCase("UnifiedBus - ub-quick-example help marks case-path as required")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + ".log"), "--help", "");

        NS_TEST_ASSERT_MSG_EQ(status, 0, "ub-quick-example --help should exit successfully");
        NS_TEST_ASSERT_MSG_NE(output.find("Required path to the unified-bus case directory"),
                              std::string::npos,
                              "help text should describe case-path as required");
        NS_TEST_ASSERT_MSG_NE(output.find("Typical usage:"),
                              std::string::npos,
                              "help text should include quick-example usage guidance");
        NS_TEST_ASSERT_MSG_NE(output.find("traffic.csv / UbTrafficGen is single-process only"),
                              std::string::npos,
                              "help text should explain the MPI TrafficGen boundary");
    }
};

class UbQuickExampleLocalSingleThreadSystemTest : public TestCase
{
  public:
    UbQuickExampleLocalSingleThreadSystemTest()
        : TestCase("UnifiedBus - ub-quick-example local mtp-threads=1 runs as single-thread")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + ".log"),
                                   "--mtp-threads=1",
                                   "",
                                   "scratch/2nodes_single-tp");

        NS_TEST_ASSERT_MSG_EQ(status,
                              0,
                              "ub-quick-example local mtp-threads=1 should exit successfully");
        NS_TEST_ASSERT_MSG_EQ(output.find("MPI_Testany() ... before MPI_INIT"), std::string::npos,
                              "ub-quick-example local mtp-threads=1 should not touch MPI before MPI_Init");
    }
};

class UbQuickScratchLegacyAliasSystemTest : public TestCase
{
  public:
    UbQuickScratchLegacyAliasSystemTest()
        : TestCase("UnifiedBus - legacy scratch ub-quick-example remains usable")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::filesystem::path repoRoot = LocateRepoRoot();
        const std::filesystem::path casePath = repoRoot / "scratch/2nodes_single-tp";
        auto [status, output] =
            RunNs3RunCommand(CreateTempDirFilename(GetName() + ".log"),
                             "scratch/ub-quick-example --case-path=" + casePath.string() +
                                 " --test",
                             "",
                             false);

        NS_TEST_ASSERT_MSG_EQ(status, 0, "legacy scratch quick-example should exit successfully");
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "legacy scratch quick-example should complete the local case");
    }
};

class UbQuickExampleSameCasePathSystemTest : public TestCase
{
  public:
    UbQuickExampleSameCasePathSystemTest()
        : TestCase("UnifiedBus - ub-quick-example accepts equivalent duplicated case-path inputs")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::filesystem::path repoRoot = LocateRepoRoot();
        const std::filesystem::path sameCasePath =
            (repoRoot / "scratch/2nodes_single-tp/../2nodes_single-tp").lexically_normal();
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + ".log"),
                                   "\"" + sameCasePath.string() + "\" --stop-ms=1",
                                   "",
                                   "scratch/2nodes_single-tp");

        NS_TEST_ASSERT_MSG_EQ(status,
                              0,
                              "ub-quick-example should accept equivalent duplicated case-path inputs");
        NS_TEST_ASSERT_MSG_EQ(output.find("conflicting case paths provided via --case-path and casePath"),
                              std::string::npos,
                              "equivalent duplicated case-path inputs should not trigger a conflict");
    }
};

class UbQuickExampleConflictingCasePathSystemTest : public TestCase
{
  public:
    UbQuickExampleConflictingCasePathSystemTest()
        : TestCase("UnifiedBus - ub-quick-example rejects conflicting case-path inputs")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::filesystem::path repoRoot = LocateRepoRoot();
        const std::filesystem::path positionalCasePath = repoRoot / "scratch/ub-mpi-minimal";
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + ".log"),
                                   "\"" + positionalCasePath.string() + "\"",
                                   "",
                                   "scratch/2nodes_single-tp");

        NS_TEST_ASSERT_MSG_NE(status,
                              0,
                              "ub-quick-example with conflicting case paths should exit with failure");
        NS_TEST_ASSERT_MSG_NE(output.find("conflicting case paths provided via --case-path and casePath"),
                              std::string::npos,
                              "ub-quick-example should print a clear conflicting case-path error");
    }
};

class UbQuickExampleOptionalTransportChannelSystemTest : public TestCase
{
  public:
    UbQuickExampleOptionalTransportChannelSystemTest()
        : TestCase("UnifiedBus - ub-quick-example succeeds without transport_channel.csv")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::filesystem::path caseDir =
            CopyCaseDirWithoutFile("scratch/2nodes_single-tp", "transport_channel.csv");
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + ".log"),
                                   "--case-path=\"" + caseDir.string() + "\"",
                                   "",
                                   "");

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_EQ(status,
                              0,
                              "ub-quick-example should accept case directories without transport_channel.csv");
    }
};

class UbQuickExampleLegacyNetworkAttributeHintSystemTest : public TestCase
{
  public:
    UbQuickExampleLegacyNetworkAttributeHintSystemTest()
        : TestCase("UnifiedBus - ub-quick-example reports migration hint for legacy network attribute keys")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::filesystem::path caseDir =
            CopyCaseDirWithoutFile("scratch/2nodes_single-tp", "transport_channel.csv");
        std::ofstream config(caseDir / "network_attribute.txt", std::ios::app);
        config << "\ndefault ns3::UbQueueManager::ResumeOffset \"4096\"\n";
        config.close();

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--stop-ms=1",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_NE(status,
                              0,
                              "legacy network attribute key should reject the case before ConfigStore");
        NS_TEST_ASSERT_MSG_NE(
            output.find("Legacy network_attribute.txt key: ns3::UbQueueManager::ResumeOffset"),
            std::string::npos,
            "legacy key diagnostic should name the old key");
        NS_TEST_ASSERT_MSG_NE(
            output.find("Use ns3::UbQueueManager::DynamicPfcResumeGapBytes instead"),
            std::string::npos,
            "legacy key diagnostic should name the replacement key");
        NS_TEST_ASSERT_MSG_EQ(
            output.find("Could not set default value for ns3::UbQueueManager::ResumeOffset"),
            std::string::npos,
            "legacy key should be caught before the generic ConfigStore failure");
    }
};

class UbQuickExampleDropWithoutRetransFailsFastSystemTest : public TestCase
{
  public:
    UbQuickExampleDropWithoutRetransFailsFastSystemTest()
        : TestCase("UnifiedBus - ub-quick-example stops when packet drops with retransmission disabled")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,131072,URMA_WRITE,7,10ns,10,\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile(kLocalSingleThreadQuickExampleCase, trafficCsv);

        ReplaceInFile(caseDir / "network_attribute.txt",
                      "default ns3::UbSwitch::FlowControl \"CBFC\"",
                      "default ns3::UbSwitch::FlowControl \"NONE\"");
        ReplaceInFile(caseDir / "network_attribute.txt",
                      "default ns3::UbQueueManager::ReservePerQueueBytes \"1048576\"",
                      "default ns3::UbQueueManager::ReservePerQueueBytes \"512\"");

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=1 --stop-ms=5",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_NE(status, 0, "run should fail-fast after packet drop without retransmission");
        NS_TEST_ASSERT_MSG_NE(output.find("Packet dropped while retransmission is disabled"),
                              std::string::npos,
                              "run should explain why it stopped early");
    }
};

class UbQuickExampleDropWithRetransCanContinueSystemTest : public TestCase
{
  public:
    UbQuickExampleDropWithRetransCanContinueSystemTest()
        : TestCase("UnifiedBus - ub-quick-example does not fail-fast on packet drop when retransmission is enabled")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,131072,URMA_WRITE,7,10ns,10,\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile(kLocalSingleThreadQuickExampleCase, trafficCsv);

        ReplaceInFile(caseDir / "network_attribute.txt",
                      "default ns3::UbSwitch::FlowControl \"CBFC\"",
                      "default ns3::UbSwitch::FlowControl \"NONE\"");
        ReplaceInFile(caseDir / "network_attribute.txt",
                      "default ns3::UbTransportChannel::EnableRetrans \"false\"",
                      "default ns3::UbTransportChannel::EnableRetrans \"true\"");
        ReplaceInFile(caseDir / "network_attribute.txt",
                      "default ns3::UbTransportChannel::MaxRetransAttempts \"7\"",
                      "default ns3::UbTransportChannel::MaxRetransAttempts \"64\"");
        ReplaceInFile(caseDir / "network_attribute.txt",
                      "default ns3::UbQueueManager::ReservePerQueueBytes \"1048576\"",
                      "default ns3::UbQueueManager::ReservePerQueueBytes \"512\"");

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=1 --stop-ms=5",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_EQ(output.find("Packet dropped while retransmission is disabled"),
                              std::string::npos,
                              "retrans-enabled run should not print the fail-fast diagnostic");
        NS_TEST_ASSERT_MSG_EQ(status, 0, "retrans-enabled run should not fail-fast");
    }
};

class UbQuickExampleLocalDependentDagSingleThreadSystemTest : public TestCase
{
  public:
    UbQuickExampleLocalDependentDagSingleThreadSystemTest()
        : TestCase("UnifiedBus - ub-quick-example local dependent DAG runs in single-thread")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,4096,URMA_WRITE,7,10ns,10,\n"
            "1,1,0,4096,URMA_WRITE,7,10ns,20,\n"
            "2,0,1,4096,URMA_WRITE,7,10ns,30,10 20\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile("scratch/2nodes_single-tp", trafficCsv);

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=1 --test",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_EQ(status, 0, "single-thread dependent DAG case should exit successfully");
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "single-thread dependent DAG case should report PASSED");
    }
};

class UbQuickExampleLocalSingleUrmaReadSystemTest : public TestCase
{
  public:
    UbQuickExampleLocalSingleUrmaReadSystemTest()
        : TestCase("UnifiedBus - ub-quick-example local single URMA_READ runs in single-thread")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,4096,URMA_READ,7,10ns,10,\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile(kLocalSingleThreadQuickExampleCase, trafficCsv);

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=1 --test",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_EQ(status, 0, "single URMA_READ case should exit successfully");
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "single URMA_READ case should report PASSED");
    }
};

class UbQuickExampleLocalSingleUrmaWriteSystemTest : public TestCase
{
  public:
    UbQuickExampleLocalSingleUrmaWriteSystemTest()
        : TestCase("UnifiedBus - ub-quick-example local single URMA_WRITE runs in single-thread")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,4096,URMA_WRITE,7,10ns,10,\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile(kLocalSingleThreadQuickExampleCase, trafficCsv);

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=1 --test",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_EQ(status, 0, "single URMA_WRITE case should exit successfully");
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "single URMA_WRITE case should report PASSED");
    }
};

class UbQuickExampleLocalWriteThenReadSystemTest : public TestCase
{
  public:
    UbQuickExampleLocalWriteThenReadSystemTest()
        : TestCase("UnifiedBus - ub-quick-example local URMA_WRITE then URMA_READ runs in single-thread")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,4096,URMA_WRITE,7,10ns,10,\n"
            "1,0,1,4096,URMA_READ,7,10ns,20,10\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile(kLocalSingleThreadQuickExampleCase, trafficCsv);

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=1 --test",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_EQ(status, 0, "dependent URMA write/read case should exit successfully");
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "dependent URMA write/read case should report PASSED");
    }
};

class UbQuickExampleLocalMixedUrmaReadWriteSystemTest : public TestCase
{
  public:
    UbQuickExampleLocalMixedUrmaReadWriteSystemTest()
        : TestCase("UnifiedBus - ub-quick-example local mixed URMA read-write workload runs in single-thread")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,4096,URMA_WRITE,7,10ns,10,\n"
            "1,0,1,8192,URMA_READ,7,10ns,20,\n"
            "2,1,0,4096,URMA_WRITE,7,10ns,30,\n"
            "3,1,0,8192,URMA_READ,7,10ns,40,\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile(kLocalSingleThreadQuickExampleCase, trafficCsv);

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(
                CreateTempDirFilename("ub-quick-example-local-mixed-urma-read-write.log"),
                                               "--mtp-threads=1 --test",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_EQ(status, 0, "mixed URMA read/write case should exit successfully");
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "mixed URMA read/write case should report PASSED");
    }
};

class UbQuickExampleLocalDependentDagMtpRedSystemTest : public TestCase
{
  public:
    UbQuickExampleLocalDependentDagMtpRedSystemTest()
        : TestCase("UnifiedBus - ub-quick-example local dependent DAG fanout runs in MTP")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        std::ostringstream traffic;
        traffic << "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,"
                   "dependOnPhases\n";
        traffic << "0,0,1,4096,URMA_WRITE,7,10ns,10,\n";
        traffic << "1,1,0,4096,URMA_WRITE,7,10ns,20,\n";
        for (uint32_t taskId = 2; taskId <= 40; ++taskId)
        {
            traffic << taskId << ",0,1,4096,URMA_WRITE,7,10ns," << (100 + taskId) << ",10 20\n";
        }
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile("scratch/2nodes_single-tp", traffic.str());

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=2 --test",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_EQ(status, 0, "MTP dependent DAG fanout case should exit successfully");
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "MTP dependent DAG fanout case should report PASSED");
    }
};

#ifdef NS3_MPI
class UbQuickExampleMpiCrossRankPhaseDependencySystemTest : public TestCase
{
  public:
    UbQuickExampleMpiCrossRankPhaseDependencySystemTest()
        : TestCase("UnifiedBus - ub-quick-example rejects cross-rank phase dependency under MPI")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,3,4096,URMA_WRITE,7,10ns,10,\n"
            "1,3,0,4096,URMA_WRITE,7,10ns,20,10\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile("scratch/ub-mpi-minimal", trafficCsv);

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=2 --stop-ms=1 --test",
                                               "mpirun -np 2",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_NE(status, 0, "MPI cross-rank dependent DAG command should be rejected");
        NS_TEST_ASSERT_MSG_NE(output.find(UbTrafficGen::GetMultiProcessUnsupportedMessage()),
                              std::string::npos,
                              "cross-rank phase dependency should print the unsupported-runtime message");
    }
};

class UbQuickExampleMpiSystemTest : public TestCase
{
  public:
    UbQuickExampleMpiSystemTest(const std::string& name,
                                const std::string& casePathRelative,
                                const std::string& extraArgs)
        : TestCase(name),
          m_casePathRelative(casePathRelative),
          m_extraArgs(extraArgs)
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + ".log"),
                                   m_extraArgs,
                                   "mpirun -np 2",
                                   m_casePathRelative);

        NS_TEST_ASSERT_MSG_NE(status, 0, "ub-quick-example MPI invocation should be rejected");
        NS_TEST_ASSERT_MSG_NE(output.find(UbTrafficGen::GetMultiProcessUnsupportedMessage()),
                              std::string::npos,
                              "MPI quick-example should explain the unsupported UbTrafficGen runtime");
    }

  private:
    std::string m_casePathRelative;
    std::string m_extraArgs;
};
#endif

class UbQuickExampleSystemTestSuite : public TestSuite
{
  public:
    UbQuickExampleSystemTestSuite()
        : TestSuite("unified-bus-examples", Type::SYSTEM)
    {
        AddTestCase(new UbQuickExampleMissingCasePathSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMissingCaseDirSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMissingCaseFileSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleHelpTextSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLocalSingleThreadSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickScratchLegacyAliasSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleSameCasePathSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleConflictingCasePathSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleOptionalTransportChannelSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLegacyNetworkAttributeHintSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleDropWithoutRetransFailsFastSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleDropWithRetransCanContinueSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLocalSingleUrmaWriteSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLocalSingleUrmaReadSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLocalWriteThenReadSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLocalMixedUrmaReadWriteSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLocalDependentDagSingleThreadSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLocalDependentDagMtpRedSystemTest(),
                    TestCase::Duration::QUICK);
#ifdef NS3_MTP
        AddTestCase(new UbQuickExampleLocalMtpSystemTest(), TestCase::Duration::QUICK);
#endif
#ifdef NS3_MPI
        AddTestCase(new UbQuickExampleSpoofedMpiEnvSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMpiSystemTest("UnifiedBus - ub-quick-example rejects MPI minimal case",
                                                    "scratch/ub-mpi-minimal",
                                                    ""),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMpiSystemTest("UnifiedBus - ub-quick-example rejects MPI mtp-threads=1 case",
                                                    "scratch/ub-mpi-minimal",
                                                    "--mtp-threads=1"),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMpiSystemTest("UnifiedBus - ub-quick-example rejects hybrid minimal case",
                                                    "scratch/ub-mpi-minimal",
                                                    "--mtp-threads=2"),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMpiSystemTest("UnifiedBus - ub-quick-example rejects hybrid ldst case",
                                                    "scratch/ub-mpi-minimal",
                                                    "--mtp-threads=2"),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMpiSystemTest("UnifiedBus - ub-quick-example rejects hybrid multi-remote case",
                                                    "scratch/ub-mpi-minimal",
                                                    "--mtp-threads=2"),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMpiCrossRankPhaseDependencySystemTest(),
                    TestCase::Duration::QUICK);
#endif
    }
};

static UbQuickExampleSystemTestSuite g_ubQuickExampleSystemTestSuite;
