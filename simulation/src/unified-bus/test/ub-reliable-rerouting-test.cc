// SPDX-License-Identifier: GPL-2.0-only
//
// Focused suite for UB 2.1 packet-level reliable rerouting
// (.scratch/ub21-reliable-rerouting/design.md). Link-only checks stay at the opaque-byte
// boundary; forwarding checks go through the production switch parsers, routing keys, and the
// recovery admission path.
#include "ns3/test.h"
#include "ns3/ub-app.h"
#include "ns3/ub-congestion-control.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-datalink.h"
#include "ns3/ub-header.h"
#include "ns3/ub-link.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-next-hop-index.h"
#include "ns3/ub-rule-routing.h"
#include "ns3/ub-port.h"
#include "ns3/ub-pfc.h"
#include "ns3/ub-reliable-rerouting.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-transport.h"
#include "ns3/ub-utils.h"

#include <algorithm>
#include <csignal>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <set>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace ns3;

namespace {

constexpr uint8_t kPriority = 7;
constexpr uint32_t kTpSource = 100;
constexpr uint32_t kTpDestination = 200;

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------

Ptr<UbPort>
CreateTestPort(Ptr<Node> node)
{
    Ptr<UbPort> port = CreateObject<UbPort>();
    port->SetAddress(Mac48Address::Allocate());
    node->AddDevice(port);
    return port;
}

/**
 * @brief Node builder that keeps reliable rerouting on NONE flow control.
 *
 * Mirrors the repository node fixture with an explicit flow-control attribute set before
 * `UbSwitch::Init()`, so this slice cannot depend on credit behaviour.
 */
void
InitReroutingNode(Ptr<Node> node,
                  UbNodeType_t nodeType,
                  uint32_t portCount,
                  FcType flowControl = FcType::NONE)
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
        CreateTestPort(node);
    }

    sw->SetAttribute("FlowControl", EnumValue(flowControl));
    sw->Init();
    Ptr<UbCongestionControl> congestionCtrl = UbCongestionControl::Create(UB_SWITCH);
    congestionCtrl->OnSwitchAttached(sw);
}

Ptr<UbLink>
ConnectPorts(Ptr<UbPort> left,
             Ptr<UbPort> right,
             bool enableRerouting,
             Time delay,
             Time detectionDelay = NanoSeconds(0))
{
    Ptr<UbLink> link = CreateObject<UbLink>();
    link->SetAttribute("Delay", TimeValue(delay));
    link->SetAttribute("EnableReliableRerouting", BooleanValue(enableRerouting));
    // Arming captures the immutable configuration when the link completes, so every recovery
    // attribute must be in place before the second endpoint attaches.
    link->SetAttribute("FailureDetectionDelay", TimeValue(detectionDelay));
    link->SetAttribute("LastAckTimeout", TimeValue(Seconds(1)));
    left->Attach(link);
    right->Attach(link);
    return link;
}

/** Adds the plain destination-node route used by the production IPv4 path. */
void
AddRoute(Ptr<Node> node, uint32_t destNodeId, const std::vector<uint16_t>& outPorts)
{
    node->GetObject<UbSwitch>()->GetRoutingProcess()->GetRoutingTable().AddShortestRoute(
        utils::NodeIdToIp(destNodeId).Get(),
        outPorts);
}

std::vector<uint8_t>
PacketBytes(Ptr<const Packet> packet)
{
    std::vector<uint8_t> bytes(packet->GetSize());
    if (!bytes.empty())
    {
        packet->CopyData(bytes.data(), bytes.size());
    }
    return bytes;
}

std::string
HexOf(const std::vector<uint8_t>& bytes)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint8_t byte : bytes)
    {
        out << std::setw(2) << static_cast<unsigned>(byte);
    }
    return out.str();
}

/** Ordered-independent view of a packet set (recovery may legitimately reorder traffic). */
std::vector<std::string>
SortedHex(const std::vector<std::vector<uint8_t>>& packets)
{
    std::vector<std::string> hex;
    hex.reserve(packets.size());
    for (const auto& packet : packets)
    {
        hex.push_back(HexOf(packet));
    }
    std::sort(hex.begin(), hex.end());
    return hex;
}

/** @brief Production URMA packet builder: the header stack the switch decoder consumes. */
Ptr<Packet>
BuildUrmaPacket(uint32_t payloadBytes,
                uint32_t sourceNodeId,
                uint32_t destinationNodeId,
                uint8_t priority,
                RoutingType routingType,
                uint32_t psn = 0)
{
    Ptr<Packet> packet = Create<Packet>(payloadBytes);
    packet->AddPacketTag(UbFlowTag(1, payloadBytes));

    UbMAExtTah maHeader;
    maHeader.SetLength(payloadBytes);
    packet->AddHeader(maHeader);

    UbTransactionHeader transactionHeader;
    transactionHeader.SetUbTransactionOpcode(UbTransactionOpcode::WRITE);
    transactionHeader.SetIniTaSsn(0);
    transactionHeader.SetIniRcId(0);
    packet->AddHeader(transactionHeader);

    UbTransportHeader transportHeader;
    transportHeader.SetTPOpcode(TpOpcode::TP_OPCODE_RELIABLE_TA);
    transportHeader.SetSrcTpn(kTpSource);
    transportHeader.SetDestTpn(kTpDestination);
    transportHeader.SetPsn(psn);
    transportHeader.SetTpMsn(psn);
    transportHeader.SetLastPacket(true);
    packet->AddHeader(transportHeader);

    UbPort::AddUdpHeader(packet, 0, 1);
    UbPort::AddIpv4Header(packet,
                          utils::NodeIdToIp(sourceNodeId),
                          utils::NodeIdToIp(destinationNodeId));

    UbIpBasedNetworkHeader networkHeader;
    packet->AddHeader(networkHeader);
    UbDataLink::AddPacketHeader(packet,
                                false,
                                false,
                                priority,
                                priority,
                                routingType,
                                UbDatalinkHeaderConfig::PACKET_IPV4);
    return packet;
}

/**
 * @brief Production routed RTP control packet (ACK/NAK/SACK/CNP family).
 *
 * Uses the transport/transaction header stack the switch and transport decoder consume, so a routed
 * acknowledgement travels the ordinary forwarding path instead of a test-only shortcut.
 */
Ptr<Packet>
BuildRoutedTpAckPacket(uint32_t payloadBytes,
                       uint32_t sourceNodeId,
                       uint32_t destinationNodeId,
                       TpOpcode opcode,
                       uint8_t priority,
                       RoutingType routingType)
{
    Ptr<Packet> packet = Create<Packet>(payloadBytes);

    UbAckTransactionHeader ackHeader;
    ackHeader.SetUbTransactionOpcode(UbTransactionOpcode::TAACK);
    packet->AddHeader(ackHeader);

    if (opcode == TpOpcode::TP_OPCODE_SACK_WITHOUT_CETPH ||
        opcode == TpOpcode::TP_OPCODE_SACK_WITH_CETPH)
    {
        if (opcode == TpOpcode::TP_OPCODE_SACK_WITH_CETPH)
        {
            UbCongestionExtTph cetph;
            cetph.SetAckSequence(0);
            packet->AddHeader(cetph);
        }
        UbSelectiveAckExtTph saetph;
        saetph.SetBitmapBitCount(64);
        saetph.SetMaxRcvPsn(0);
        saetph.SetBitmapBit(0, true);
        packet->AddHeader(saetph);
    }
    else if (opcode == TpOpcode::TP_OPCODE_ACK_WITH_CETPH)
    {
        UbCongestionExtTph cetph;
        cetph.SetAckSequence(0);
        packet->AddHeader(cetph);
    }

    UbTransportHeader transportHeader;
    transportHeader.SetTPOpcode(opcode);
    transportHeader.SetSrcTpn(kTpDestination);
    transportHeader.SetDestTpn(kTpSource);
    transportHeader.SetPsn(0);
    transportHeader.SetTpMsn(0);
    transportHeader.SetLastPacket(true);
    packet->AddHeader(transportHeader);

    UbPort::AddUdpHeader(packet, 1, 0);
    UbPort::AddIpv4Header(packet,
                          utils::NodeIdToIp(sourceNodeId),
                          utils::NodeIdToIp(destinationNodeId));
    UbIpBasedNetworkHeader networkHeader;
    packet->AddHeader(networkHeader);
    UbDataLink::AddPacketHeader(packet,
                                false,
                                false,
                                priority,
                                priority,
                                routingType,
                                UbDatalinkHeaderConfig::PACKET_IPV4);
    return packet;
}

/** @brief Production compact-transport packet (request or response role by TA opcode). */
Ptr<Packet>
BuildCompactTransportPacket(uint32_t payloadBytes,
                            uint32_t sourceNodeId,
                            uint32_t destinationNodeId,
                            UbTransactionOpcode opcode,
                            uint8_t priority,
                            RoutingType routingType)
{
    Ptr<Packet> packet = Create<Packet>(payloadBytes);

    UbDummyTransactionHeader taHeader;
    taHeader.SetUbTransactionOpcode(opcode);
    packet->AddHeader(taHeader);

    UbCtpHeader ctpHeader;
    ctpHeader.SetNlp(UB_CTPH_NLP_COMPACT_TAH);
    packet->AddHeader(ctpHeader);

    UbCna16NetworkHeader cnaHeader;
    cnaHeader.SetScna(utils::NodeIdToCna16(sourceNodeId));
    cnaHeader.SetDcna(utils::NodeIdToCna16(destinationNodeId));
    cnaHeader.SetNlp(UB_CNA_NLP_CTPH);
    packet->AddHeader(cnaHeader);
    UbDataLink::AddPacketHeader(packet,
                                false,
                                false,
                                priority,
                                priority,
                                routingType,
                                UbDatalinkHeaderConfig::PACKET_CNA16);
    return packet;
}

/** @brief Production LDST memory packet (request or response role by TA opcode). */
Ptr<Packet>
BuildLdstMemoryPacket(uint32_t payloadBytes,
                      uint32_t sourceNodeId,
                      uint32_t destinationNodeId,
                      UbTransactionOpcode opcode,
                      uint8_t priority,
                      RoutingType routingType)
{
    Ptr<Packet> packet = Create<Packet>(payloadBytes);

    UbDummyTransactionHeader taHeader;
    taHeader.SetUbTransactionOpcode(opcode);
    packet->AddHeader(taHeader);

    UbLdstNetworkFields fields;
    fields.source.nodeId = sourceNodeId;
    fields.destination.nodeId = destinationNodeId;
    AddLdstNetworkHeader(packet,
                                            fields,
                                            UbDatalinkHeaderConfig::PACKET_UB_MEM);
    UbDataLink::AddPacketHeader(packet,
                                false,
                                false,
                                priority,
                                priority,
                                routingType,
                                UbDatalinkHeaderConfig::PACKET_UB_MEM);
    return packet;
}

// ---------------------------------------------------------------------------------------------
// Topology fixture
// ---------------------------------------------------------------------------------------------

/**
 * @brief Two hosts and two parallel switch-to-switch links.
 *
 *   hostA(0) --0-- swB(1) --1==1-- swC(2) --0-- hostD(3)
 *                       \--2==2--/
 *
 * Both switches route to the remote host over equal-cost candidates {1, 2} with
 * PER_FLOW_SHORTEST_PATHS + INGRESS_PORT_STRIPE, so the host-facing ingress port 0 always picks
 * candidate 1 until the detected failed output is excluded.
 */
struct ReroutingTopology
{
    Ptr<Node> hostA;
    Ptr<Node> swB;
    Ptr<Node> swC;
    Ptr<Node> hostD;
    Ptr<UbPort> a0;
    Ptr<UbPort> b0;
    Ptr<UbPort> b1;
    Ptr<UbPort> b2;
    Ptr<UbPort> c0;
    Ptr<UbPort> c1;
    Ptr<UbPort> c2;
    Ptr<UbPort> d0;
    Ptr<UbLink> failedLink;
    Ptr<UbLink> alternateLink;

    ReroutingTopology(bool enableRerouting = true,
                      Time failedLinkDelay = NanoSeconds(20),
                      Time detectionDelay = NanoSeconds(0),
                      Time lastAckPathDelay = NanoSeconds(0),
                      FcType flowControl = FcType::NONE,
                      uint32_t switchBPorts = 3)
    {
        hostA = CreateObject<Node>(0);
        swB = CreateObject<Node>(1);
        swC = CreateObject<Node>(2);
        hostD = CreateObject<Node>(3);
        InitReroutingNode(hostA, UB_DEVICE, 1, flowControl);
        InitReroutingNode(swB, UB_SWITCH, switchBPorts, flowControl);
        InitReroutingNode(swC, UB_SWITCH, 3, flowControl);
        InitReroutingNode(hostD, UB_DEVICE, 1, flowControl);

        a0 = DynamicCast<UbPort>(hostA->GetDevice(0));
        b0 = DynamicCast<UbPort>(swB->GetDevice(0));
        b1 = DynamicCast<UbPort>(swB->GetDevice(1));
        b2 = DynamicCast<UbPort>(swB->GetDevice(2));
        c0 = DynamicCast<UbPort>(swC->GetDevice(0));
        c1 = DynamicCast<UbPort>(swC->GetDevice(1));
        c2 = DynamicCast<UbPort>(swC->GetDevice(2));
        d0 = DynamicCast<UbPort>(hostD->GetDevice(0));

        ConnectPorts(a0, b0, enableRerouting, NanoSeconds(20));
        failedLink = ConnectPorts(b1,
                                  c1,
                                  enableRerouting,
                                  failedLinkDelay,
                                  detectionDelay);
        alternateLink = ConnectPorts(b2, c2, enableRerouting, NanoSeconds(20) + lastAckPathDelay);
        ConnectPorts(c0, d0, enableRerouting, NanoSeconds(20));

        AddRoute(swB, swC->GetId(), {1, 2});
        AddRoute(swC, swB->GetId(), {1, 2});
        AddRoute(swB, hostD->GetId(), {1, 2});
        AddRoute(swC, hostA->GetId(), {1, 2});
        AddRoute(swB, hostA->GetId(), {0});
        AddRoute(swC, hostD->GetId(), {0});
        AddRoute(hostA, hostD->GetId(), {0});
        AddRoute(hostD, hostA->GetId(), {0});

        swB->GetObject<UbSwitch>()->GetRoutingProcess()->SetAttribute(
            "MultipathSelector",
            EnumValue(MultipathSelector::INGRESS_PORT_STRIPE));
        swC->GetObject<UbSwitch>()->GetRoutingProcess()->SetAttribute(
            "MultipathSelector",
            EnumValue(MultipathSelector::INGRESS_PORT_STRIPE));
    }

    Ptr<UbSwitch> Switch(Ptr<Node> node) const { return node->GetObject<UbSwitch>(); }

    UbReliableRerouting::State State(uint32_t portId) const
    {
        Ptr<UbPort> port = DynamicCast<UbPort>(swB->GetDevice(portId));
        return port->GetReliableRerouting()->GetState();
    }

    UbReliableRerouting* Rerouting(uint32_t portId) const
    {
        return PeekPointer(DynamicCast<UbPort>(swB->GetDevice(portId))->GetReliableRerouting());
    }
};

// ---------------------------------------------------------------------------------------------
// Trace/callback adapters (ns-3 MakeCallback accepts function pointers, not lambdas)
// ---------------------------------------------------------------------------------------------

struct PacketSink
{
    std::vector<std::vector<uint8_t>> packets;
    std::vector<const Packet*> identities;
    /** Packets that leaked a switch-local recovery marker into a later hop. */
    uint32_t taggedPackets{0};
};

struct ControlFrameCounter
{
    uint32_t count{0};
};

void
CountControlFrame(ControlFrameCounter* counter, Ptr<UbPort>, Ptr<Packet> packet)
{
    UbDatalinkPacketHeader header;
    packet->PeekHeader(header);
    if (header.IsUbDatalinkControlCreditHeader())
    {
        ++counter->count;
    }
}

void
CaptureDeliveredPacket(PacketSink* sink, Ptr<UbPort>, Ptr<Packet> packet)
{
    UbRecoveredCreditTag leakedTag;
    if (packet->PeekPacketTag(leakedTag))
    {
        ++sink->taggedPackets;
    }
    sink->identities.push_back(PeekPointer(packet));
    sink->packets.push_back(PacketBytes(packet));
}

/** @brief Coordinates a single failure action that must cover both link directions. */
struct BilateralArm
{
    bool afterInjection{false};
    bool forwardSeen{false};
    bool reverseSeen{false};
    bool armed{false};
    Time offset{NanoSeconds(40)};
    Ptr<UbLink> link;
};

struct SendWatcher
{
    uint32_t nodeId{0};
    uint32_t portId{0};
    Ptr<UbLink> link;
    Ptr<UbPort> port;
    Time failOffset;
    bool armed{false};
    bool failOnSend{false};
    Time observedStart;
    bool* startedFlag{nullptr};
    // Optional post-delivery probe: records what the sender still retained one nanosecond after
    // the delivery instant.
    PacketSink* probeSink{nullptr};
    UbReliableRerouting* probeRerouting{nullptr};
    uint64_t retryAtDelivery{0};
    BilateralArm* bilateral{nullptr};
    bool bilateralIsForward{false};
};

void
WatchSendStart(SendWatcher* watcher, uint32_t nodeId, uint32_t portId, uint32_t packetBytes)
{
    if (nodeId != watcher->nodeId || portId != watcher->portId)
    {
        return;
    }
    if (watcher->observedStart.IsZero())
    {
        watcher->observedStart = Simulator::Now();
    }
    if (watcher->startedFlag != nullptr)
    {
        *watcher->startedFlag = true;
    }
    if (watcher->bilateral != nullptr && watcher->bilateral->afterInjection)
    {
        BilateralArm* arm = watcher->bilateral;
        if (watcher->bilateralIsForward)
        {
            arm->forwardSeen = true;
        }
        else
        {
            arm->reverseSeen = true;
        }
        if (arm->forwardSeen && arm->reverseSeen && !arm->armed)
        {
            arm->armed = true;
            Simulator::Schedule(arm->offset, &UbLink::Fail, PeekPointer(arm->link));
        }
    }
    if (watcher->link == nullptr)
    {
        // Start-only watcher used to coordinate a failure across both directions.
        return;
    }
    const Time txTime = watcher->port != nullptr
                            ? watcher->port->GetDataRate().CalculateBytesTxTime(packetBytes)
                            : Time(0);
    // Simulator::Schedule takes a delay, so the delivery instant is expressed relative to now.
    const Time deliveryDelay = txTime + watcher->link->GetDelay();
    const Time deliveryTime = watcher->observedStart + deliveryDelay;
    if (watcher->probeSink != nullptr)
    {
        SendWatcher* watcherPtr = watcher;
        const Time probeTime = deliveryDelay + NanoSeconds(1);
        Simulator::Schedule(probeTime, [watcherPtr]() {
            if (watcherPtr->probeRerouting != nullptr)
            {
                watcherPtr->retryAtDelivery = watcherPtr->probeRerouting->GetRetryPacketCount();
            }
        });
    }
    if (watcher->failOnSend && !watcher->armed)
    {
        watcher->armed = true;
        const Time failTime = watcher->failOffset.IsZero()
                                  ? deliveryDelay
                                  : watcher->failOffset;
        Simulator::Schedule(failTime, &UbLink::Fail, PeekPointer(watcher->link));
    }
}

/** Same-timestamp ordering helper: schedule the failure once a packet is fully serialized. */
void
WatchPacketSerialized(SendWatcher* watcher, Ptr<Packet>, Time txCompleteTime)
{
    if (watcher->failOnSend || watcher->armed)
    {
        return;
    }
    watcher->armed = true;
    Simulator::Schedule(txCompleteTime + watcher->link->GetDelay(),
                        &UbLink::Fail,
                        PeekPointer(watcher->link));
}

struct TransferWatcher
{
    Time transferTime;
    Time validLastAckTime;
    Time firstDrainTime;
    uint32_t admissionBlocks{0};
    uint32_t validCopies{0};
    uint32_t duplicateCopies{0};
    bool sawDrain{false};
    bool sawLastAckWait{false};
    bool sawControlFrameDiscard{false};
    Ptr<UbSwitch> newcomerSwitch;
    Ptr<UbPort> newcomerIngress;
    Ptr<Packet> newcomer;
};

void
WatchReroutingEvent(TransferWatcher* watcher, const UbReliableReroutingEvent& record)
{
    if (record.event == "state:WAIT_LAST_ACK" && !watcher->sawLastAckWait)
    {
        watcher->sawLastAckWait = true;
        if (watcher->newcomer != nullptr)
        {
            // New traffic offered after detection must use the healthy alternate immediately.
            Simulator::ScheduleNow(&UbSwitch::SwitchHandlePacket,
                                   watcher->newcomerSwitch,
                                   watcher->newcomerIngress,
                                   watcher->newcomer);
        }
    }
    if (record.event == "lastack_valid")
    {
        ++watcher->validCopies;
        watcher->validLastAckTime = Simulator::Now();
    }
    if (record.event == "lastack_duplicate")
    {
        ++watcher->duplicateCopies;
    }
    if (record.event == "transfer" && watcher->transferTime.IsZero())
    {
        watcher->transferTime = Simulator::Now();
    }
    if (record.event == "control_frame_discarded")
    {
        watcher->sawControlFrameDiscard = true;
    }
    if (record.event == "admission_block")
    {
        ++watcher->admissionBlocks;
    }
    if (record.event == "state:DRAINED")
    {
        if (!watcher->sawDrain) watcher->firstDrainTime = Simulator::Now();
        watcher->sawDrain = true;
    }
}

struct ReceiveWatcher
{
    std::vector<uint8_t> expected;
    Time arrivalTime;
};

void WatchSendPacket(ReceiveWatcher* watcher, Ptr<Packet> packet, Time)
{
    if (PacketBytes(packet) == watcher->expected)
    {
        watcher->arrivalTime = Simulator::Now();
    }
}

/** Records the local transmit-attempt identity of every recovery transfer. */
void
WatchAttemptTokens(std::vector<uint64_t>* tokens, const UbReliableReroutingEvent& record)
{
    if (record.event == "transfer")
    {
        tokens->push_back(record.attemptId);
    }
}

void
WatchReceive(ReceiveWatcher* watcher, Ptr<Packet> packet)
{
    if (watcher->arrivalTime.IsZero() && PacketBytes(packet) == watcher->expected)
    {
        watcher->arrivalTime = Simulator::Now();
    }
}

// ---------------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------------

#ifndef _WIN32
void
ExitOnAbortSignal(int)
{
    std::_Exit(42);
}

struct ChildOutcome
{
    bool collected{false};
    bool exited{false};
    int exitCode{0};
    bool signalled{false};
    int signalNumber{0};
    std::string diagnostic;
};

/**
 * @brief Runs `body` in a child process and reports how that child terminated.
 *
 * The child turns SIGABRT into exit code 42, so a fatal-error abort is observable without
 * killing the test runner.
 */
ChildOutcome
RunInChild(const std::function<void()>& body)
{
    ChildOutcome outcome;
    int diagnosticPipe[2];
    if (pipe(diagnosticPipe) != 0)
    {
        return outcome;
    }
    pid_t child = fork();
    if (child == -1)
    {
        close(diagnosticPipe[0]);
        close(diagnosticPipe[1]);
        return outcome;
    }
    if (child == 0)
    {
        close(diagnosticPipe[0]);
        dup2(diagnosticPipe[1], STDERR_FILENO);
        close(diagnosticPipe[1]);
        std::signal(SIGABRT, ExitOnAbortSignal);
        body();
        std::_Exit(1);
    }
    close(diagnosticPipe[1]);
    char diagnosticBuffer[1024];
    ssize_t diagnosticBytes;
    while ((diagnosticBytes = read(diagnosticPipe[0], diagnosticBuffer, sizeof(diagnosticBuffer))) > 0)
    {
        outcome.diagnostic.append(diagnosticBuffer, diagnosticBytes);
    }
    close(diagnosticPipe[0]);
    int status = 0;
    outcome.collected = (waitpid(child, &status, 0) == child);
    outcome.exited = WIFEXITED(status);
    outcome.exitCode = outcome.exited ? WEXITSTATUS(status) : 0;
    outcome.signalled = WIFSIGNALED(status);
    outcome.signalNumber = outcome.signalled ? WTERMSIG(status) : 0;
    return outcome;
}
#endif

/** @brief Scope and configuration rules for `UbLink::Fail()` and the arming attributes. */
class UbReroutingScopeTest : public TestCase
{
  public:
    UbReroutingScopeTest()
        : TestCase("Reliable rerouting - arming scope, idempotence, and rejected failures")
    {
    }

  private:
    void DoRun() override
    {
        {
            ReroutingTopology topo;
            NS_TEST_ASSERT_MSG_EQ(topo.failedLink->IsReliableReroutingArmed(), true, "armed");
            NS_TEST_ASSERT_MSG_EQ(topo.alternateLink->IsReliableReroutingArmed(), true, "armed");
            NS_TEST_ASSERT_MSG_EQ(topo.State(1), UbReliableRerouting::State::HEALTHY,
                                  "healthy before failure");

            // A host access link shares the same global enable value but must stay ordinary.
            Ptr<UbLink> hostLink = DynamicCast<UbLink>(topo.a0->GetChannel());
            NS_TEST_ASSERT_MSG_NE(hostLink, nullptr, "host link exists");
            NS_TEST_ASSERT_MSG_EQ(hostLink->IsReliableReroutingArmed(),
                                  false,
                                  "host access link is not armed");
            NS_TEST_ASSERT_MSG_EQ(
                topo.hostA->GetDevice(0)->GetObject<UbPort>()->GetReliableRerouting()->GetState(),
                UbReliableRerouting::State::DISABLED,
                "host port keeps its recovery component disarmed");

            topo.failedLink->Fail();
            NS_TEST_ASSERT_MSG_EQ(topo.failedLink->IsFailed(), true, "failed");
            const auto stateAfterFail = topo.State(1);
            topo.failedLink->Fail();
            NS_TEST_ASSERT_MSG_EQ(topo.State(1), stateAfterFail,
                                  "repeat failure restarts no timer");
            Simulator::Destroy();
        }

#ifndef _WIN32
        {
            // A second distinct link is rejected before mutating that link.
            ReroutingTopology topo;
            topo.failedLink->Fail();
            Ptr<UbLink> alternate = topo.alternateLink;
            const auto outcome = RunInChild([alternate]() { alternate->Fail(); });
            NS_TEST_ASSERT_MSG_EQ(outcome.collected, true, "child collected");
            NS_TEST_ASSERT_MSG_EQ(outcome.exited, true, "second distinct link failure aborts");
            NS_TEST_ASSERT_MSG_EQ(outcome.exitCode, 42, "second distinct link failure aborts");
            NS_TEST_ASSERT_MSG_EQ(topo.alternateLink->IsFailed(), false,
                                  "rejected link stays unchanged");
            Simulator::Destroy();
        }
        {
            // A link that never armed (feature disabled) rejects Fail() before mutating.
            ReroutingTopology topo(false);
            NS_TEST_ASSERT_MSG_EQ(topo.failedLink->IsReliableReroutingArmed(),
                                  false,
                                  "disabled link is not armed");
            Ptr<UbLink> link = topo.failedLink;
            const auto outcome = RunInChild([link]() { link->Fail(); });
            NS_TEST_ASSERT_MSG_EQ(outcome.collected, true, "child collected");
            NS_TEST_ASSERT_MSG_EQ(outcome.exited, true, "failure of a disabled link aborts");
            NS_TEST_ASSERT_MSG_EQ(outcome.exitCode, 42, "failure of a disabled link aborts");
            NS_TEST_ASSERT_MSG_EQ(topo.failedLink->IsFailed(), false, "disabled link unchanged");
            Simulator::Destroy();
        }
        {
            // A host access link rejects Fail() before mutating.
            ReroutingTopology topo;
            Ptr<UbLink> hostLink = DynamicCast<UbLink>(topo.a0->GetChannel());
            const auto outcome = RunInChild([hostLink]() { hostLink->Fail(); });
            NS_TEST_ASSERT_MSG_EQ(outcome.collected, true, "child collected");
            NS_TEST_ASSERT_MSG_EQ(outcome.exited, true, "failure of a host link aborts");
            NS_TEST_ASSERT_MSG_EQ(outcome.exitCode, 42, "failure of a host link aborts");
            NS_TEST_ASSERT_MSG_EQ(hostLink->IsFailed(), false, "host link unchanged");
            Simulator::Destroy();
        }
        {
            // Recovery configuration is armed before traffic and cannot change later.
            ReroutingTopology topo;
            topo.failedLink->SetAttribute("LastAckTimeout", TimeValue(MicroSeconds(7)));
            Ptr<UbLink> link = topo.failedLink;
            const auto outcome = RunInChild([link]() { link->Fail(); });
            NS_TEST_ASSERT_MSG_EQ(outcome.collected, true, "child collected");
            NS_TEST_ASSERT_MSG_EQ(outcome.exited, true, "changed configuration aborts");
            NS_TEST_ASSERT_MSG_EQ(outcome.exitCode, 42, "changed configuration aborts");
            NS_TEST_ASSERT_MSG_EQ(topo.failedLink->IsFailed(), false,
                                  "rejected configuration leaves the link unchanged");
            Simulator::Destroy();
        }
#endif

        {
            // The one-link guard is scoped to a simulation and resets at Destroy().
            ReroutingTopology first;
            first.failedLink->Fail();
            Simulator::Destroy();

            ReroutingTopology second;
            second.failedLink->Fail();
            NS_TEST_ASSERT_MSG_EQ(second.failedLink->IsFailed(), true,
                                  "a fresh simulation may fail its own link");
            Simulator::Destroy();
        }
    }
};

/** @brief Same-timestamp ordering: the event that runs first decides, per ns-3 event order. */
class UbReroutingSameTimestampOrderTest : public TestCase
{
  public:
    explicit UbReroutingSameTimestampOrderTest(bool receiveFirst)
        : TestCase(std::string("Reliable rerouting - ") +
                   (receiveFirst ? "complete receipt before failure delivers"
                                 : "failure before complete receipt suppresses delivery")),
          m_receiveFirst(receiveFirst)
    {
    }

  private:
    void DoRun() override
    {
        // Long recovery delays keep the modeled LastAck out of the observation window, so the probe
        // one nanosecond after the delivery instant sees only the receipt outcome.
        ReroutingTopology topo(true,
                               NanoSeconds(20),
                               MilliSeconds(1),
                               MilliSeconds(1));
        PacketSink sink;
        topo.d0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sink));

        Ptr<Packet> packet = BuildUrmaPacket(4096,
                                             topo.hostA->GetId(),
                                             topo.hostD->GetId(),
                                             kPriority,
                                             RoutingType::PER_FLOW_SHORTEST_PATHS);

        SendWatcher watcher;
        watcher.nodeId = topo.swB->GetId();
        watcher.portId = 1;
        watcher.link = topo.failedLink;
        watcher.port = topo.b1;
        watcher.probeSink = &sink;
        watcher.probeRerouting = topo.Rerouting(1);
        watcher.failOnSend = !m_receiveFirst;

        // "PortTxNotify" fires before the link schedules the delivery event, so a failure scheduled
        // from it sorts ahead of the delivery. "TraComEventNotify" fires after the delivery event
        // exists, so the same timestamp runs the receipt first. Both orderings come from ns-3's
        // ordinary same-timestamp event order.
        topo.b1->TraceConnectWithoutContext("PortTxNotify",
                                            MakeBoundCallback(&WatchSendStart, &watcher));
        topo.b1->TraceConnectWithoutContext("TraComEventNotify",
                                            MakeBoundCallback(&WatchPacketSerialized, &watcher));

        Simulator::Schedule(NanoSeconds(10),
                            &UbSwitch::SwitchHandlePacket,
                            topo.Switch(topo.swB),
                            topo.b0,
                            packet);
        // Long enough for the modeled detection + LastAck + recovery transfer to complete.
        Simulator::Stop(MilliSeconds(5));
        Simulator::Run();
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_GT(watcher.observedStart.GetNanoSeconds(), 0, "send instant observed");
        NS_TEST_ASSERT_MSG_EQ(topo.failedLink->IsFailed(), true, "failure action ran");
        if (m_receiveFirst)
        {
            NS_TEST_ASSERT_MSG_EQ(watcher.retryAtDelivery, 0,
                                  "peer receipt completed before the failure releases the backup");
        }
        else
        {
            NS_TEST_ASSERT_MSG_EQ(watcher.retryAtDelivery, 1,
                                  "receipt not complete when the link failed stays retained");
        }
        // Either way the local recovery path eventually delivers the packet to its destination.
        NS_TEST_ASSERT_MSG_EQ(sink.packets.size(), 1, "packet reaches the destination once");
    }

    bool m_receiveFirst;
};

/** @brief Bidirectional in-flight recovery through the healthy alternate. */
class UbReroutingBidirectionalRecoveryTest : public TestCase
{
  public:
    explicit UbReroutingBidirectionalRecoveryTest(bool positiveDelays)
        : TestCase(positiveDelays
                       ? "Reliable rerouting - in-flight recovery with positive delays"
                       : "Reliable rerouting - bilateral in-flight recovery after LastAck"),
          m_positiveDelays(positiveDelays)
    {
    }

  private:
    void DoRun() override
    {
        const Time detectionDelay = m_positiveDelays ? MicroSeconds(3) : NanoSeconds(0);
        const Time lastAckPathDelay = m_positiveDelays ? MicroSeconds(5) : NanoSeconds(0);
        ReroutingTopology topo(true, NanoSeconds(20), detectionDelay, lastAckPathDelay);

        PacketSink sinkD;
        PacketSink sinkA;
        topo.d0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sinkD));
        topo.a0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sinkA));

        // Packet 0 completes before the failure and must never replay.
        Ptr<Packet> early = BuildUrmaPacket(512,
                                            topo.hostA->GetId(),
                                            topo.hostD->GetId(),
                                            kPriority,
                                            RoutingType::PER_FLOW_SHORTEST_PATHS,
                                            0);
        const auto earlyBytes = PacketBytes(early);

        // The two later packets stay in flight when the link fails.
        Ptr<Packet> forwardPacket = BuildUrmaPacket(4096,
                                                    topo.hostA->GetId(),
                                                    topo.hostD->GetId(),
                                                    kPriority,
                                                    RoutingType::PER_FLOW_SHORTEST_PATHS,
                                                    1);
        Ptr<Packet> reversePacket = BuildUrmaPacket(4096,
                                                    topo.hostD->GetId(),
                                                    topo.hostA->GetId(),
                                                    kPriority,
                                                    RoutingType::PER_FLOW_SHORTEST_PATHS,
                                                    2);
        const auto forwardBytes = PacketBytes(forwardPacket);
        const auto reverseBytes = PacketBytes(reversePacket);

        BilateralArm armState;
        armState.link = topo.failedLink;
        SendWatcher forwardWatcher;
        forwardWatcher.nodeId = topo.swB->GetId();
        forwardWatcher.portId = 1;
        forwardWatcher.bilateral = &armState;
        forwardWatcher.bilateralIsForward = true;
        SendWatcher reverseWatcher;
        reverseWatcher.nodeId = topo.swC->GetId();
        reverseWatcher.portId = 1;
        reverseWatcher.bilateral = &armState;
        reverseWatcher.bilateralIsForward = false;

        topo.b1->TraceConnectWithoutContext("PortTxNotify",
                                            MakeBoundCallback(&WatchSendStart, &forwardWatcher));
        topo.c1->TraceConnectWithoutContext("PortTxNotify",
                                            MakeBoundCallback(&WatchSendStart, &reverseWatcher));

        Simulator::Schedule(NanoSeconds(10),
                            &UbSwitch::SwitchHandlePacket,
                            topo.Switch(topo.swB),
                            topo.b0,
                            early);
        Simulator::Schedule(MicroSeconds(1), [&]() {
            NS_TEST_ASSERT_MSG_EQ(sinkD.packets.size(), 1,
                                  "early packet completed before the failure");
            // 4096 bytes at 400 Gbps serializes in 81.92 ns, so a 40 ns offset keeps both attempts
            // strictly in flight while still inside their serialization window.
            armState.afterInjection = true;
            Simulator::ScheduleNow(&UbSwitch::SwitchHandlePacket,
                                   topo.Switch(topo.swB),
                                   topo.b0,
                                   forwardPacket);
            Simulator::ScheduleNow(&UbSwitch::SwitchHandlePacket,
                                   topo.Switch(topo.swC),
                                   topo.c0,
                                   reversePacket);
        });

        Simulator::Stop(MicroSeconds(200));
        Simulator::Run();

        const auto retryB = topo.Rerouting(1)->GetRetryPacketCount();
        const auto stateB = topo.State(1);
        const auto runtimeDrops = UbUtils::Get()->GetRuntimePacketDropCount();
        const auto dropReason = UbUtils::Get()->GetRuntimePacketDropReason();
        const auto retryC =
            PeekPointer(topo.c1->GetReliableRerouting())->GetRetryPacketCount();
        const auto stateC =
            DynamicCast<UbPort>(topo.swC->GetDevice(1))->GetReliableRerouting()->GetState();
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_EQ(topo.failedLink->IsFailed(), true, "link failed during the run");
        NS_TEST_ASSERT_MSG_EQ(runtimeDrops, 0u,
                              "suppressed delivery is retained work, not a runtime packet drop: " +
                                  dropReason);
        NS_TEST_ASSERT_MSG_EQ(sinkD.packets.size(), 2, "forward direction receives two packets");
        NS_TEST_ASSERT_MSG_EQ(HexOf(sinkD.packets[0]), HexOf(earlyBytes),
                                  "pre-failure packet arrives once");
        NS_TEST_ASSERT_MSG_EQ(HexOf(sinkD.packets[1]), HexOf(forwardBytes),
                                  "in-flight packet recovered byte-identically");
        NS_TEST_ASSERT_MSG_EQ(sinkA.packets.size(), 1, "reverse direction receives its packet");
        NS_TEST_ASSERT_MSG_EQ(HexOf(sinkA.packets[0]), HexOf(reverseBytes),
                                  "reverse in-flight packet recovered byte-identically");

        NS_TEST_ASSERT_MSG_EQ(retryB, 0, "forward retry storage released after recovery");
        NS_TEST_ASSERT_MSG_EQ(retryC, 0, "reverse retry storage released after recovery");
        NS_TEST_ASSERT_MSG_EQ(stateB, UbReliableRerouting::State::DRAINED, "forward local drain");
        NS_TEST_ASSERT_MSG_EQ(stateC, UbReliableRerouting::State::DRAINED, "reverse local drain");
    }

    bool m_positiveDelays;
};

/** @brief Healthy delivery and real failure handshake at zero, wrapped and retained boundaries. */
class UbReroutingHealthyBackupTest : public TestCase
{
  public:
    explicit UbReroutingHealthyBackupTest(bool enabled, bool wrap = false, bool retained = false)
        : TestCase(std::string("Reliable rerouting - healthy traffic with feature ") +
                   (enabled ? "enabled" : "disabled") + (wrap ? " wrap" : "") +
                   (retained ? " unconfirmed" : "")),
          m_enabled(enabled), m_wrap(wrap), m_retained(retained)
    {
    }

  private:
    void DoRun() override
    {
        ReroutingTopology topo(m_enabled, m_retained ? MilliSeconds(10) : NanoSeconds(20));
        PacketSink sink;
        topo.d0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sink));

        NS_TEST_ASSERT_MSG_EQ(topo.Rerouting(1)->GetConfirmedNext(), 0u, "zero TX boundary");
        NS_TEST_ASSERT_MSG_EQ(topo.c1->GetReliableRerouting()->GetReceiveNext(), 0u, "zero RX boundary");
        const uint32_t count = m_wrap ? 65537 : 4;
        std::vector<std::vector<uint8_t>> sent;
        for (uint32_t index = 0; index < count; ++index)
        {
            Ptr<Packet> packet = BuildUrmaPacket(m_wrap ? 1 : 1024,
                                                 topo.hostA->GetId(),
                                                 topo.hostD->GetId(),
                                                 kPriority,
                                                 RoutingType::PER_FLOW_SHORTEST_PATHS,
                                                 index);
            sent.push_back(PacketBytes(packet));
            Simulator::Schedule(NanoSeconds(10) + (m_wrap ? NanoSeconds(index * 100) : MicroSeconds(index * 20)),
                                &UbSwitch::SwitchHandlePacket,
                                topo.Switch(topo.swB),
                                topo.b0,
                                packet);
        }

        TransferWatcher events;
        if (m_enabled)
        {
            topo.Rerouting(1)->TraceConnectWithoutContext("Rerouting",
                MakeBoundCallback(&WatchReroutingEvent, &events));
            // Healthy complete delivery anchors a nonzero wrapped boundary; retained mode
            // fails before any delayed delivery, leaving more than a wire period outstanding.
            Simulator::Schedule(MilliSeconds(7), &UbLink::Fail, topo.failedLink);
        }
        Simulator::Stop(MilliSeconds(9));
        Simulator::Run();
        if (m_enabled)
        {
            NS_TEST_ASSERT_MSG_EQ(events.validCopies, 1u, "real LastAck accepts anchored boundary");
        }
        const auto peakRetryB = topo.Rerouting(1)->GetPeakRetryPacketCount();
        if (m_retained)
        {
            NS_TEST_ASSERT_MSG_EQ(peakRetryB, count, "more than one field period was unconfirmed");
        }
        const auto retryB = topo.Rerouting(1)->GetRetryPacketCount();
        NS_TEST_ASSERT_MSG_EQ(topo.Rerouting(1)->GetConfirmedNext(), (m_enabled && !m_retained ? count : 0),
                              "wide TX boundary survives wire wraparound");
        NS_TEST_ASSERT_MSG_EQ(topo.c1->GetReliableRerouting()->GetReceiveNext(), (m_enabled && !m_retained ? count : 0),
                              "RX complete delivery agrees with confirmed boundary");
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_EQ(sink.packets.size(), sent.size(), "all packets delivered");
        for (size_t index = 0; index < std::min(sink.packets.size(), sent.size()); ++index)
        {
            NS_TEST_ASSERT_MSG_EQ(HexOf(sink.packets[index]), HexOf(sent[index]),
                                  "packet bytes and receive order preserved");
        }
        NS_TEST_ASSERT_MSG_EQ(retryB, 0, "healthy backups return to zero");
        if (m_enabled && !m_retained)
        {
            NS_TEST_ASSERT_MSG_EQ(peakRetryB, 0, "confirmed healthy packets never occupy retry");
        }
    }

    bool m_enabled;
    bool m_wrap;
    bool m_retained;
};

/**
 * @brief Detected failure freezes the retained attempt until LastAck while new traffic proceeds.
 */

/**
 * @brief A detected failed output cannot silently fall back to the ordinary drop branch.
 *
 * The scenario keeps a single legal candidate, so after detection no legal output remains and the
 * switch must abort with the failed-output diagnostic instead of dropping the packet.
 */
/** Original objects survive propagation, early recovery and late invalid delivery events. */
class UbReroutingOriginalPacketTest : public TestCase
{
  public:
    UbReroutingOriginalPacketTest(bool enabled, bool fail, bool longIfg = false)
        : TestCase("Original packet retention enabled=" + std::to_string(enabled) +
                   " failure=" + std::to_string(fail) + " long-ifg=" + std::to_string(longIfg)),
          m_enabled(enabled), m_fail(fail), m_longIfg(longIfg)
    {
    }

  private:
    void DoRun() override
    {
        ReroutingTopology topo(m_enabled, m_longIfg ? NanoSeconds(20) : MicroSeconds(10));
        if (m_longIfg)
        {
            topo.b1->SetAttribute("UbInterframeGap", TimeValue(MicroSeconds(20)));
        }
        PacketSink sink;
        topo.d0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sink));
        std::vector<Ptr<Packet>> originals;
        const uint32_t count = m_longIfg ? 1 : 4;
        for (uint32_t i = 0; i < count; ++i)
        {
            auto packet = BuildUrmaPacket(64, topo.hostA->GetId(), topo.hostD->GetId(),
                                          kPriority, RoutingType::PER_FLOW_SHORTEST_PATHS, i);
            originals.push_back(packet);
            Simulator::Schedule(NanoSeconds(10 + i * 100), &UbSwitch::SwitchHandlePacket,
                                topo.Switch(topo.swB), topo.b0, packet);
        }
        Simulator::Schedule(MicroSeconds(1), [&]() {
            NS_TEST_ASSERT_MSG_EQ(topo.Rerouting(1)->GetRetryPacketCount(), 0u,
                                  "healthy propagation is not retry storage");
            NS_TEST_ASSERT_MSG_EQ(topo.Rerouting(1)->GetInFlightPacketCount(),
                                  (m_enabled && !m_longIfg ? count : 0u),
                                  "only armed endpoints retain unreceived originals");
            NS_TEST_ASSERT_MSG_EQ(topo.b0->GetReliableRerouting()->GetInFlightPacketCount(), 0u,
                                  "ineligible access links have no in-flight records");
            if (m_fail)
            {
                topo.failedLink->Fail();
                NS_TEST_ASSERT_MSG_EQ(topo.Rerouting(1)->GetRetryPacketCount(),
                                      (m_longIfg ? 0u : count),
                                      "failure retains all unreceived packets, not the IFG pointer");
            }
        });
        Simulator::Stop(MicroSeconds(30));
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(sink.identities.size(), count, "each original delivered once");
        for (uint32_t i = 0; i < std::min<size_t>(count, sink.identities.size()); ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(sink.identities[i], PeekPointer(originals[i]),
                                  "recovery hands off the original object, not a same-UID copy");
        }
        NS_TEST_ASSERT_MSG_EQ(topo.Rerouting(1)->GetRetryPacketCount(), 0u,
                              "late old-link delivery events cannot resurrect retry entries");
        Simulator::Destroy();
        for (const auto& packet : originals)
        {
            NS_TEST_ASSERT_MSG_EQ(packet->GetReferenceCount(), 1u,
                                  "completed events and owners released their references");
        }
    }
    bool m_enabled;
    bool m_fail;
    bool m_longIfg;
};

class UbReroutingDetectionCreditTest : public TestCase
{
  public:
    UbReroutingDetectionCreditTest()
        : TestCase("Reliable rerouting - detection window remains credit limited")
    {
    }
    void DoRun() override
    {
        ReroutingTopology topo(true, MicroSeconds(10), MicroSeconds(5), MicroSeconds(20),
                              FcType::CBFC);
        // Exactly two one-cell packets may leave this output before credit is exhausted.
        topo.b1->SetAttribute("CbfcFlitsPerCell", UintegerValue(16));
        topo.b1->SetAttribute("CbfcInitCreditCell", IntegerValue(2));
        topo.b1->CreateAndInitFc(FcType::CBFC);
        PacketSink sink;
        topo.d0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sink));
        for (uint32_t i = 0; i < 4; ++i)
        {
            auto packet = BuildUrmaPacket(64, topo.hostA->GetId(), topo.hostD->GetId(),
                                          kPriority, RoutingType::PER_FLOW_SHORTEST_PATHS, i);
            Simulator::Schedule(NanoSeconds(10 + i * 100), &UbSwitch::SwitchHandlePacket,
                                topo.Switch(topo.swB), topo.b0, packet);
        }
        Simulator::Schedule(NanoSeconds(80), [&]() {
            topo.failedLink->Fail();
            NS_TEST_ASSERT_MSG_EQ(topo.Rerouting(1)->GetRetryPacketCount(), 1u,
                                  "physical failure captures first propagating original");
        });
        for (auto when : {MicroSeconds(1), MicroSeconds(6), MicroSeconds(15)})
        {
            Simulator::Schedule(when, [&]() {
                NS_TEST_ASSERT_MSG_EQ(topo.Rerouting(1)->GetRetryPacketCount(), 2u,
                                      "one later attempt consumes last credit, then no growth");
                NS_TEST_ASSERT_MSG_EQ(topo.Switch(topo.swB)->GetVoqPacketCountForTest(
                                          1, kPriority, 0), 2u,
                                      "uncredited packets stay in failed VOQ until real LastAck");
            });
        }
        Simulator::Stop(MicroSeconds(80));
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(sink.packets.size(), 4u, "real LastAck recovers all four packets");
        NS_TEST_ASSERT_MSG_EQ(topo.Rerouting(1)->GetRetryPacketCount(), 0u, "retry handed off");
        Simulator::Destroy();
    }
};

class UbReroutingBacklogOrderTest : public TestCase
{
  public:
    UbReroutingBacklogOrderTest()
        : TestCase("Reliable rerouting - append retry then egress then old VOQ after LastAck")
    {
    }
    void DoRun() override
    {
        ReroutingTopology topo(true, MicroSeconds(10), NanoSeconds(0), MicroSeconds(5),
                              FcType::CBFC);
        topo.b1->SetAttribute("UbInterframeGap", TimeValue(MicroSeconds(20)));
        topo.b2->GetUbQueue()->SetAttribute("MaxEgressBytes", UintegerValue(16));
        PacketSink sink;
        topo.d0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sink));
        std::vector<Ptr<Packet>> packets;
        for (uint32_t i = 0; i < 5; ++i)
        {
            packets.push_back(BuildUrmaPacket(256, topo.hostA->GetId(), topo.hostD->GetId(),
                                              kPriority, RoutingType::PER_FLOW_SHORTEST_PATHS, i));
        }
        const uint32_t bytes = packets[0]->GetSize();
        topo.b1->GetUbQueue()->SetAttribute("MaxEgressBytes", UintegerValue(bytes));
        for (uint32_t i = 0; i < 4; ++i)
        {
            Simulator::Schedule(NanoSeconds(10 + i * 100), &UbSwitch::SwitchHandlePacket,
                                topo.Switch(topo.swB), topo.b0, packets[i]);
        }
        Simulator::Schedule(MicroSeconds(1), &UbLink::Fail, topo.failedLink);
        Simulator::Schedule(MicroSeconds(2), &UbSwitch::SwitchHandlePacket,
                            topo.Switch(topo.swB), topo.b0, packets[4]);
        Simulator::Schedule(MicroSeconds(3), [&]() {
            NS_TEST_ASSERT_MSG_EQ(topo.Rerouting(1)->GetRetryPacketCount(), 1u, "retry waits");
            NS_TEST_ASSERT_MSG_EQ(topo.b1->GetUbQueue()->GetCurrentBytes(), bytes, "egress waits");
            NS_TEST_ASSERT_MSG_EQ(topo.Switch(topo.swB)->GetVoqPacketCountForTest(1, kPriority, 0),
                                  2u, "both old VOQ entries wait");
            NS_TEST_ASSERT_MSG_EQ(topo.Switch(topo.swB)->GetVoqPacketCountForTest(2, kPriority, 0),
                                  1u, "only post-detection new traffic selected the healthy VOQ");
        });
        Simulator::Schedule(MicroSeconds(15), [&]() {
            NS_TEST_ASSERT_MSG_EQ(topo.Switch(topo.swB)->GetVoqPacketCountForTest(2, kPriority, 0),
                                  5u, "all categories appended after actual LastAck");
            NS_TEST_ASSERT_MSG_EQ(topo.Switch(topo.swB)->GetQueueManager()->
                                      GetQueueIngressTotalBytes(0, kPriority),
                                  3u * bytes, "new and old VOQ keep ordinary reservations");
        });
        Simulator::Schedule(MicroSeconds(20), [&]() {
            topo.b2->GetUbQueue()->SetAttribute("MaxEgressBytes", UintegerValue(65536));
            topo.b2->TriggerTransmit();
        });
        Simulator::Stop(MicroSeconds(80));
        Simulator::Run();
        const uint32_t expected[] = {4, 0, 1, 2, 3};
        NS_TEST_ASSERT_MSG_EQ(sink.identities.size(), 5u, "all originals reach the destination once");
        for (uint32_t i = 0; i < std::min<size_t>(5, sink.identities.size()); ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(sink.identities[i], PeekPointer(packets[expected[i]]),
                                  "existing target precedes retry, egress and FIFO old VOQ");
        }
        NS_TEST_ASSERT_MSG_EQ(topo.State(1), UbReliableRerouting::State::DRAINED, "handoff closed");
        Simulator::Destroy();
    }
};

class UbReroutingOccupancyOutputTest : public TestCase
{
  public:
    UbReroutingOccupancyOutputTest(bool detail, bool early, bool enabled = true)
        : TestCase("Reliable rerouting - occupancy output detail=" + std::to_string(detail) +
                   " early=" + std::to_string(early) + " enabled=" + std::to_string(enabled)),
          m_detail(detail), m_early(early), m_enabled(enabled)
    {
    }
    void DoRun() override
    {
        namespace fs = std::filesystem;
        const auto dir = fs::temp_directory_path() /
            ("ub-recovery-occupancy-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir / "output");
        UbUtils::Get()->Destroy();
        UbUtils::SetTracePathForTest(dir.string());
        ReroutingTopology topo(m_enabled, MicroSeconds(10), NanoSeconds(0), MicroSeconds(5));
        topo.b1->SetAttribute("UbInterframeGap", TimeValue(MicroSeconds(20)));
        topo.b2->GetUbQueue()->SetAttribute("MaxEgressBytes", UintegerValue(16));
        topo.Rerouting(1)->SetAttribute("TraceOccupancyEvents", BooleanValue(m_detail));
        PacketSink sink;
        topo.d0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sink));
        uint32_t bytes = 0;
        for (uint32_t i = 0; i < 2; ++i)
        {
            auto packet = BuildUrmaPacket(256, topo.hostA->GetId(), topo.hostD->GetId(),
                                          kPriority, RoutingType::PER_FLOW_SHORTEST_PATHS, i);
            bytes = packet->GetSize();
            Simulator::Schedule(NanoSeconds(10 + i * 100), &UbSwitch::SwitchHandlePacket,
                                topo.Switch(topo.swB), topo.b0, packet);
        }
        if (m_enabled)
        {
            Simulator::Schedule(MicroSeconds(1), &UbLink::Fail, topo.failedLink);
        }
        Simulator::Schedule(MicroSeconds(20), [&]() {
            topo.b2->GetUbQueue()->SetAttribute("MaxEgressBytes", UintegerValue(65536));
            topo.b2->TriggerTransmit();
        });
        Simulator::Stop(MicroSeconds(m_early ? 15 : 80));
        Simulator::Run();
        auto owner = topo.b1->GetReliableRerouting();
        if (m_enabled)
        {
            NS_TEST_ASSERT_MSG_EQ(owner->GetOccupancy(UbRecoveryStage::RETRY).peakPackets, 1u,
                                  "one attempted original retained");
            NS_TEST_ASSERT_MSG_EQ(owner->GetOccupancy(UbRecoveryStage::FAILED_EGRESS).peakBytes,
                                  bytes, "one unsent egress original retained");
            NS_TEST_ASSERT_MSG_EQ(owner->GetOccupancy(UbRecoveryStage::RECOVERY_VOQ).peakBytes,
                                  2u * bytes, "both origins coexist in healthy VOQ");
            NS_TEST_ASSERT_MSG_EQ(owner->GetRetryPacketCount(), 0u, "retry handed off");
            NS_TEST_ASSERT_MSG_EQ(owner->GetRecoveryVoqPacketCount(), (m_early ? 2u : 0u),
                                  "early stop reports queued recovery even with retry zero");
        }
        UbUtils::Get()->Destroy();
        const auto summary = dir / "output/reliable_rerouting_occupancy_summary.csv";
        const auto events = dir / "output/reliable_rerouting_occupancy_events.csv";
        NS_TEST_ASSERT_MSG_EQ(fs::exists(summary), m_enabled, "summary follows actual arming");
        NS_TEST_ASSERT_MSG_EQ(fs::exists(events), (m_enabled && m_detail), "detail is opt-in");
        if (m_enabled)
        {
            std::ifstream input(summary);
            std::string line;
            std::getline(input, line);
            NS_TEST_ASSERT_MSG_EQ(line,
                "time_ps,node_id,failed_port,stage,packets,bytes,peak_packets,peak_bytes,state,event,source",
                "fixed occupancy CSV schema");
            uint32_t matched = 0;
            while (std::getline(input, line))
            {
                for (uint32_t i = 0; i < static_cast<uint32_t>(UbRecoveryStage::COUNT); ++i)
                {
                    auto stage = static_cast<UbRecoveryStage>(i);
                    const auto& count = owner->GetOccupancy(stage);
                    std::ostringstream expected;
                    expected << Simulator::Now().GetPicoSeconds() << ',' << topo.swB->GetId()
                             << ",1," << owner->GetStageName(stage) << ',' << count.packets << ','
                             << count.bytes << ',' << count.peakPackets << ',' << count.peakBytes
                             << ',' << owner->GetStateName(owner->GetState()) << ",summary,none";
                    if (line == expected.str())
                    {
                        ++matched;
                    }
                }
            }
            NS_TEST_ASSERT_MSG_EQ(matched, 4u, "summary exactly matches all four query stages");
            if (m_detail)
            {
                std::ifstream detail(events);
                std::array<std::vector<std::string>, 4> last;
                std::set<std::string> sources;
                std::getline(detail, line);
                while (std::getline(detail, line))
                {
                    std::istringstream row(line);
                    std::vector<std::string> fields;
                    std::string field;
                    while (std::getline(row, field, ',')) fields.push_back(field);
                    NS_TEST_ASSERT_MSG_EQ(fields.size(), 11u, "fixed-width event row");
                    if (fields.size() != 11) continue;
                    if (fields[9] == "voq_handoff") sources.insert(fields[10]);
                    for (uint32_t i = 0; i < 4; ++i)
                    {
                        if (fields[1] == std::to_string(topo.swB->GetId()) && fields[2] == "1" &&
                            fields[3] == owner->GetStageName(static_cast<UbRecoveryStage>(i)))
                        {
                            last[i] = fields;
                        }
                    }
                }
                NS_TEST_ASSERT_MSG_EQ(sources.count("retry"), 1u, "explicit retry source");
                NS_TEST_ASSERT_MSG_EQ(sources.count("failed_egress"), 1u, "explicit egress source");
                for (uint32_t i = 0; i < 4; ++i)
                {
                    NS_TEST_ASSERT_MSG_EQ(last[i].size(), 11u, "each stage has event evidence");
                    if (last[i].size() != 11) continue;
                    const auto& count = owner->GetOccupancy(static_cast<UbRecoveryStage>(i));
                    NS_TEST_ASSERT_MSG_EQ(last[i][4], std::to_string(count.packets), "event packets match");
                    NS_TEST_ASSERT_MSG_EQ(last[i][5], std::to_string(count.bytes), "event bytes match");
                    NS_TEST_ASSERT_MSG_EQ(last[i][6], std::to_string(count.peakPackets), "event peak packets match");
                    NS_TEST_ASSERT_MSG_EQ(last[i][7], std::to_string(count.peakBytes), "event peak bytes match");
                }
            }
        }
        if (!m_early)
        {
            NS_TEST_ASSERT_MSG_EQ(sink.packets.size(), 2u, "reporting preserves complete delivery");
        }
        Simulator::Destroy();
        UbUtils::SetTracePathForTest("");
        fs::remove_all(dir);
    }
  private:
    bool m_detail;
    bool m_early;
    bool m_enabled;
};

class UbReroutingNoLegalOutputTest : public TestCase
{
  public:
    UbReroutingNoLegalOutputTest()
        : TestCase("Reliable rerouting - no legal candidate after detection is fatal")
    {
    }

  private:
    void DoRun() override
    {
#ifndef _WIN32
        const auto outcome = RunInChild([]() {
            ReroutingTopology topo(true, NanoSeconds(20), NanoSeconds(0), MicroSeconds(50));
            // Only the failed output is legal towards the destination host.
            Ptr<UbRoutingProcess> routing =
                topo.swB->GetObject<UbSwitch>()->GetRoutingProcess();
            routing->GetRoutingTable().RemoveShortestRoute(utils::NodeIdToIp(topo.hostD->GetId()).Get());
            routing->GetRoutingTable().AddShortestRoute(
                utils::NodeIdToIp(topo.hostD->GetId()).Get(),
                std::vector<uint16_t>{1});

            Ptr<Packet> packet = BuildUrmaPacket(512,
                                                 topo.hostA->GetId(),
                                                 topo.hostD->GetId(),
                                                 kPriority,
                                                 RoutingType::PER_FLOW_SHORTEST_PATHS);
            topo.failedLink->Fail();
            Simulator::ScheduleNow(&UbSwitch::SwitchHandlePacket,
                                   topo.Switch(topo.swB),
                                   topo.b0,
                                   packet);
            Simulator::Stop(MicroSeconds(10));
            Simulator::Run();
            Simulator::Destroy();
        });
        NS_TEST_ASSERT_MSG_EQ(outcome.collected, true, "child collected");
        NS_TEST_ASSERT_MSG_EQ(outcome.exited, true,
                              "no legal candidate after detection aborts the simulation");
        NS_TEST_ASSERT_MSG_EQ(outcome.exitCode, 42,
                              "no legal candidate after detection aborts the simulation");
#endif
    }
};

class UbReroutingLastAckGateTest : public TestCase
{
  public:
    UbReroutingLastAckGateTest()
        : TestCase("Reliable rerouting - backlog waits for LastAck, new traffic uses the alternate")
    {
    }

  private:
    void DoRun() override
    {
        const Time detectionDelay = MicroSeconds(4);
        const Time lastAckPathDelay = MicroSeconds(4);
        ReroutingTopology topo(true, NanoSeconds(20), detectionDelay, lastAckPathDelay);

        PacketSink sink;
        topo.d0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sink));

        Ptr<Packet> retained = BuildUrmaPacket(4096,
                                              topo.hostA->GetId(),
                                              topo.hostD->GetId(),
                                              kPriority,
                                              RoutingType::PER_FLOW_SHORTEST_PATHS,
                                              0);
        Ptr<Packet> newcomer = BuildUrmaPacket(1024,
                                              topo.hostA->GetId(),
                                              topo.hostD->GetId(),
                                              kPriority,
                                              RoutingType::PER_FLOW_SHORTEST_PATHS,
                                              1);
        const auto retainedBytes = PacketBytes(retained);
        const auto newcomerBytes = PacketBytes(newcomer);

        TransferWatcher transfers;
        transfers.newcomerSwitch = topo.Switch(topo.swB);
        transfers.newcomerIngress = topo.b0;
        transfers.newcomer = newcomer;
        topo.Rerouting(1)->TraceConnectWithoutContext(
            "Rerouting",
            MakeBoundCallback(&WatchReroutingEvent, &transfers));
        ReceiveWatcher newcomerSent;
        newcomerSent.expected = newcomerBytes;
        topo.b2->TraceConnectWithoutContext("TraComEventNotify",
            MakeBoundCallback(&WatchSendPacket, &newcomerSent));
        ReceiveWatcher newcomerArrival;
        newcomerArrival.expected = newcomerBytes;
        topo.d0->TraceConnectWithoutContext("PktRcvNotify",
                                            MakeBoundCallback(&WatchReceive, &newcomerArrival));

        SendWatcher watcher;
        watcher.nodeId = topo.swB->GetId();
        watcher.portId = 1;
        watcher.link = topo.failedLink;
        watcher.failOffset = NanoSeconds(40);
        watcher.failOnSend = true;
        topo.b1->TraceConnectWithoutContext("PortTxNotify",
                                            MakeBoundCallback(&WatchSendStart, &watcher));

        Simulator::Schedule(NanoSeconds(10),
                            &UbSwitch::SwitchHandlePacket,
                            topo.Switch(topo.swB),
                            topo.b0,
                            retained);

        Simulator::Stop(MicroSeconds(200));
        Simulator::Run();
        const auto stateB = topo.State(1);
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_EQ(transfers.sawLastAckWait, true,
                              "detection happened before the new packet is offered");
        const Time expectedTransfer =
            watcher.observedStart + watcher.failOffset + detectionDelay + lastAckPathDelay;
        NS_TEST_ASSERT_MSG_GT(transfers.transferTime.GetNanoSeconds(), 0,
                              "retained attempt transferred");
        NS_TEST_ASSERT_MSG_GT(transfers.validLastAckTime, expectedTransfer,
                              "LastAck includes actual queue and wire delay");
        NS_TEST_ASSERT_MSG_EQ(transfers.transferTime, transfers.validLastAckTime,
                              "retained transfer is gated by actual LastAck receipt");
        NS_TEST_ASSERT_MSG_GT(newcomerArrival.arrivalTime.GetNanoSeconds(), 0,
                              "new traffic delivered");
        NS_TEST_ASSERT_MSG_LT(newcomerSent.arrivalTime, transfers.validLastAckTime,
                              "new traffic starts over the healthy output during LastAck wait");
        NS_TEST_ASSERT_MSG_EQ(sink.packets.size(), 2, "both packets delivered exactly once");
        NS_TEST_ASSERT_MSG_EQ(HexOf(sink.packets[0]), HexOf(newcomerBytes),
                              "new traffic arrives first");
        NS_TEST_ASSERT_MSG_EQ(HexOf(sink.packets[1]), HexOf(retainedBytes),
                              "retained packet arrives after recovery");
        NS_TEST_ASSERT_MSG_EQ(stateB, UbReliableRerouting::State::DRAINED, "local drain reached");
    }
};

/** @brief Disposal releases owned timers and storage, and never reports a successful drain. */
class UbReroutingDisposalTest : public TestCase
{
  public:
    UbReroutingDisposalTest()
        : TestCase("Reliable rerouting - disposal cancels recovery without a false drain")
    {
    }

  private:
    void DoRun() override
    {
        ReroutingTopology topo(true, NanoSeconds(20), MicroSeconds(5), MicroSeconds(5));
        Ptr<Packet> retained = BuildUrmaPacket(4096,
                                              topo.hostA->GetId(),
                                              topo.hostD->GetId(),
                                              kPriority,
                                              RoutingType::PER_FLOW_SHORTEST_PATHS);

        TransferWatcher transfers;
        topo.Rerouting(1)->TraceConnectWithoutContext(
            "Rerouting",
            MakeBoundCallback(&WatchReroutingEvent, &transfers));

        SendWatcher watcher;
        watcher.nodeId = topo.swB->GetId();
        watcher.portId = 1;
        watcher.link = topo.failedLink;
        watcher.failOffset = NanoSeconds(40);
        watcher.failOnSend = true;
        topo.b1->TraceConnectWithoutContext("PortTxNotify",
                                            MakeBoundCallback(&WatchSendStart, &watcher));

        Simulator::Schedule(NanoSeconds(10),
                            &UbSwitch::SwitchHandlePacket,
                            topo.Switch(topo.swB),
                            topo.b0,
                            retained);

        // Stop inside the LastAck wait: retained work must never look drained.
        Simulator::Stop(MicroSeconds(7));
        Simulator::Run();
        const auto stateBefore = topo.State(1);
        const auto retryBefore = topo.Rerouting(1)->GetRetryPacketCount();
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_EQ(transfers.sawDrain, false, "unfinished recovery is not drained");
        NS_TEST_ASSERT_MSG_EQ(stateBefore,
                              UbReliableRerouting::State::WAIT_LAST_ACK,
                              "stopped during the LastAck wait");
        NS_TEST_ASSERT_MSG_EQ(retryBefore, 1, "retained attempt still owned at the early stop");
        NS_TEST_ASSERT_MSG_EQ(retained->GetReferenceCount(), 1u,
                              "destroy releases retained originals without claiming delivery");
    }
};

/**
 * @brief The suite registers ticket-01 reliable-rerouting behaviour.
 */

/**
 * @brief VOQ backlog with an outstanding allocator grant at detection.
 *
 * A long allocation time keeps a forwarded packet in the failed output's VOQ when the link fails.
 * Detection cancels its pending grant; LastAck migrates the packet to the healthy alternate while
 * its original ingress ownership and VL are preserved.
 */
class UbReroutingVoqGrantCancellationTest : public TestCase
{
  public:
    UbReroutingVoqGrantCancellationTest()
        : TestCase("Reliable rerouting - VOQ backlog recovers after a cancelled grant")
    {
    }

  private:
    void DoRun() override
    {
        const Time detectionDelay = MicroSeconds(1);
        const Time lastAckPathDelay = MicroSeconds(2);
        ReroutingTopology topo(true, NanoSeconds(20), detectionDelay, lastAckPathDelay);
        // Keep the packet invisible to the allocator long enough to fail before it is granted.
        topo.Switch(topo.swB)->GetAllocator()->SetAttribute("AllocationTime",
                                                             TimeValue(MicroSeconds(50)));

        PacketSink sink;
        topo.d0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sink));
        Ptr<Packet> packet = BuildUrmaPacket(4096,
                                             topo.hostA->GetId(),
                                             topo.hostD->GetId(),
                                             kPriority,
                                             RoutingType::PER_FLOW_SHORTEST_PATHS);
        const auto bytes = PacketBytes(packet);

        TransferWatcher transfers;
        topo.Rerouting(1)->TraceConnectWithoutContext(
            "Rerouting",
            MakeBoundCallback(&WatchReroutingEvent, &transfers));

        Simulator::Schedule(NanoSeconds(10),
                            &UbSwitch::SwitchHandlePacket,
                            topo.Switch(topo.swB),
                            topo.b0,
                            packet);
        Simulator::Schedule(MicroSeconds(1),
                            &UbLink::Fail,
                            PeekPointer(topo.failedLink));

        Simulator::Stop(MicroSeconds(300));
        Simulator::Run();

        const auto failedVoq = topo.Switch(topo.swB)->GetVoqPacketCountForTest(1, kPriority, 0);
        const auto egressBytes = topo.b1->GetUbQueue()->GetCurrentBytes();
        const auto pendingBytes = topo.Switch(topo.swB)->GetPendingRecoveryByteCount(1);
        const auto stateB = topo.State(1);
        const auto retry = topo.Rerouting(1)->GetRetryPacketCount();
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_EQ(sink.packets.size(), 1, "VOQ backlog reaches the destination once");
        if (sink.packets.size() != 1)
        {
            return;
        }
        NS_TEST_ASSERT_MSG_EQ(HexOf(sink.packets[0]), HexOf(bytes),
                              "migrated packet keeps its bytes");
        NS_TEST_ASSERT_MSG_EQ(transfers.transferTime,
                              transfers.validLastAckTime,
                              "migration waits for actual LastAck");
        NS_TEST_ASSERT_MSG_EQ(failedVoq, 0u, "failed output VOQ drained by migration");
        NS_TEST_ASSERT_MSG_EQ(egressBytes, 0u, "failed output egress never took the packet");
        NS_TEST_ASSERT_MSG_EQ(pendingBytes, 0u, "recovery storage released after transfer");
        NS_TEST_ASSERT_MSG_EQ(retry, 0u, "no retry attempt was retained for a queued packet");
        NS_TEST_ASSERT_MSG_EQ(stateB, UbReliableRerouting::State::DRAINED, "local drain reached");
    }
};

/**
 * @brief Egress backlog of the failed output migrates after LastAck without duplication.
 */
class UbReroutingEgressBacklogTest : public TestCase
{
  public:
    UbReroutingEgressBacklogTest()
        : TestCase("Reliable rerouting - egress backlog migrates after LastAck")
    {
    }

  private:
    void DoRun() override
    {
        const Time detectionDelay = MicroSeconds(1);
        const Time lastAckPathDelay = MicroSeconds(2);
        ReroutingTopology topo(true, NanoSeconds(20), detectionDelay, lastAckPathDelay);
        // A long interframe gap keeps later packets in the egress queue while the first is in flight.
        for (Ptr<UbPort> port : {topo.b1, topo.c1})
        {
            port->SetAttribute("UbInterframeGap", TimeValue(MicroSeconds(5)));
        }

        PacketSink sinkD;
        PacketSink sinkA;
        topo.d0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sinkD));
        topo.a0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sinkA));

        std::vector<std::vector<uint8_t>> sentForward;
        SendWatcher watcher;
        watcher.nodeId = topo.swB->GetId();
        watcher.portId = 1;
        watcher.link = topo.failedLink;
        watcher.port = topo.b1;
        watcher.failOffset = NanoSeconds(40);
        watcher.failOnSend = true;
        topo.b1->TraceConnectWithoutContext("PortTxNotify",
                                            MakeBoundCallback(&WatchSendStart, &watcher));

        for (uint32_t index = 0; index < 3; ++index)
        {
            Ptr<Packet> packet = BuildUrmaPacket(4096,
                                                 topo.hostA->GetId(),
                                                 topo.hostD->GetId(),
                                                 kPriority,
                                                 RoutingType::PER_FLOW_SHORTEST_PATHS,
                                                 index);
            sentForward.push_back(PacketBytes(packet));
            Simulator::Schedule(NanoSeconds(10),
                                &UbSwitch::SwitchHandlePacket,
                                topo.Switch(topo.swB),
                                topo.b0,
                                packet);
        }

        Simulator::Stop(MicroSeconds(400));
        Simulator::Run();
        const auto stateB = topo.State(1);
        const auto egressBytes = topo.b1->GetUbQueue()->GetCurrentBytes();
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_EQ(sinkD.packets.size(), sentForward.size(),
                              "every egress-queued packet reaches the destination exactly once");
        // Recovered packets may overtake each other; exactly-once delivery is the invariant.
        const auto receivedSet = SortedHex(sinkD.packets);
        const auto sentSet = SortedHex(sentForward);
        for (size_t index = 0; index < sentSet.size(); ++index)
        {
            NS_TEST_ASSERT_MSG_EQ(receivedSet[index], sentSet[index],
                                  "egress-migrated packet keeps its bytes");
        }
        NS_TEST_ASSERT_MSG_EQ(sinkA.packets.size(), 0, "no packet leaks into the reverse host");
        NS_TEST_ASSERT_MSG_EQ(sinkD.taggedPackets, 0u,
                              "recovery-local accounting marker never reaches a later hop");
        NS_TEST_ASSERT_MSG_EQ(egressBytes, 0u, "failed output egress backlog drained");
        NS_TEST_ASSERT_MSG_EQ(stateB, UbReliableRerouting::State::DRAINED, "local drain reached");
    }
};

/**
 * @brief Delayed in-port processing keeps its ready time and reservation across recovery.
 */
class UbReroutingDelayedProcessingTest : public TestCase
{
  public:
    explicit UbReroutingDelayedProcessingTest(bool readyBeforeLastAck)
        : TestCase(std::string("Reliable rerouting - delayed processing ready ") +
                   (readyBeforeLastAck ? "before LastAck" : "after LastAck")),
          m_readyBeforeLastAck(readyBeforeLastAck)
    {
    }

  private:
    void DoRun() override
    {
        const Time detectionDelay = MicroSeconds(1);
        const Time lastAckPathDelay = MicroSeconds(2);
        const Time processingDelay = m_readyBeforeLastAck ? MicroSeconds(3) : MicroSeconds(10);
        ReroutingTopology topo(true, NanoSeconds(20), detectionDelay, lastAckPathDelay);
        topo.Switch(topo.swB)->SetAttribute("InPortProcessingDelay", TimeValue(processingDelay));

        PacketSink sink;
        topo.d0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sink));
        Ptr<Packet> packet = BuildUrmaPacket(4096,
                                             topo.hostA->GetId(),
                                             topo.hostD->GetId(),
                                             kPriority,
                                             RoutingType::PER_FLOW_SHORTEST_PATHS);

        TransferWatcher transfers;
        topo.Rerouting(1)->TraceConnectWithoutContext(
            "Rerouting",
            MakeBoundCallback(&WatchReroutingEvent, &transfers));

        Simulator::Schedule(NanoSeconds(10),
                            &UbSwitch::SwitchHandlePacket,
                            topo.Switch(topo.swB),
                            topo.b0,
                            packet);
        Simulator::Schedule(MicroSeconds(1), &UbLink::Fail, PeekPointer(topo.failedLink));

        // A normal healthy-output packet releases the same ingress/VL after the failed
        // packet is ready but before the peer LastAck returns.
        if (m_readyBeforeLastAck)
        {
            Simulator::Schedule(NanoSeconds(3100), [&, packet]() {
                topo.Switch(topo.swB)->SetAttribute("InPortProcessingDelay", TimeValue(NanoSeconds(0)));
                topo.Switch(topo.swB)->SwitchHandlePacket(topo.b0, packet->Copy());
            });
        }
        Simulator::Stop(MicroSeconds(80));
        Simulator::Run();
        const auto stateB = topo.State(1);
        Simulator::Destroy();

        const Time failure = MicroSeconds(1);
        const Time lastAck = transfers.validLastAckTime;
        const Time ready = NanoSeconds(10) + processingDelay;
        const Time expected = std::max(lastAck, ready);
        NS_TEST_ASSERT_MSG_EQ(transfers.transferTime, expected,
                              "admission waits for both readiness and LastAck");
        NS_TEST_ASSERT_MSG_EQ(sink.packets.size(), (m_readyBeforeLastAck ? 2 : 1), "each packet arrives once");
        NS_TEST_ASSERT_MSG_EQ(transfers.firstDrainTime, expected, "first drain waits for processing");
        NS_TEST_ASSERT_MSG_EQ(stateB, UbReliableRerouting::State::DRAINED, "local drain reached");
    }

    bool m_readyBeforeLastAck;
};

/**
 * @brief Blocked admission keeps the retained packet and resumes on a coalesced capacity wakeup.
 */
class UbReroutingBlockedAdmissionTest : public TestCase
{
  public:
    explicit UbReroutingBlockedAdmissionTest(bool shared = false)
        : TestCase(std::string("Reliable rerouting - recovery bypasses occupied ingress ") +
                   (shared ? "cross ingress shared pool" : "retry only")), m_shared(shared)
    {
    }

  private:
    void DoRun() override
    {
        const Time detectionDelay = NanoSeconds(0);
        const Time lastAckPathDelay = MicroSeconds(2);
        ReroutingTopology topo(true, NanoSeconds(20), detectionDelay, lastAckPathDelay);

        PacketSink sink;
        topo.d0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sink));
        Ptr<Packet> packet = BuildUrmaPacket(4096,
                                             topo.hostA->GetId(),
                                             topo.hostD->GetId(),
                                             kPriority,
                                             RoutingType::PER_FLOW_SHORTEST_PATHS);
        const auto bytes = PacketBytes(packet);

        UbQueueManager* queueManager = PeekPointer(topo.Switch(topo.swB)->GetQueueManager());

        TransferWatcher transfers;
        topo.Rerouting(1)->TraceConnectWithoutContext(
            "Rerouting",
            MakeBoundCallback(&WatchReroutingEvent, &transfers));

        SendWatcher watcher;
        watcher.nodeId = topo.swB->GetId();
        watcher.portId = 1;
        watcher.link = topo.failedLink;
        watcher.port = topo.b1;
        // Small acknowledgement packets serialize in a few nanoseconds, so fail just after the send
        // boundary while the attempt is still in flight on the link.
        watcher.failOffset = NanoSeconds(1);
        watcher.failOnSend = true;
        topo.b1->TraceConnectWithoutContext("PortTxNotify",
                                            MakeBoundCallback(&WatchSendStart, &watcher));

        Simulator::Schedule(NanoSeconds(10),
                            &UbSwitch::SwitchHandlePacket,
                            topo.Switch(topo.swB),
                            topo.b0,
                            packet);

        // Occupy the only ingress admission slot for the whole blocked window, then release it well
        // after LastAck. Real occupancy release is the single wakeup source.
        queueManager->SetReservePerQueueBytes(m_shared ? 0 : packet->GetSize());
        queueManager->SetAttribute("SharedPoolBytes", UintegerValue(m_shared ? packet->GetSize() : 0));
        queueManager->SetAttribute("HeadroomPerPortBytes", UintegerValue(0));
        queueManager->SetAttribute("AlphaShift", UintegerValue(0));
        const uint32_t blockerIngress = m_shared ? 2 : 0;
        const uint32_t blockerPriority = m_shared ? 6 : kPriority;
        const uint32_t blockerBytes = packet->GetSize();
        const Time blockStart = NanoSeconds(100);
        const Time release = MicroSeconds(20);
        Simulator::Schedule(blockStart, [=]() {
            queueManager->PushToVoq(blockerIngress, 2, blockerPriority, blockerBytes);
        });
        Time blockedPendingProbe;
        Simulator::Schedule(release - MicroSeconds(1), [&]() {
            blockedPendingProbe = Simulator::Now();
        });
        Simulator::Schedule(release, [=]() {
            queueManager->PopFromVoq(blockerIngress, 2, blockerPriority, blockerBytes);
        });

        const uint64_t eventsBeforeRun = Simulator::GetEventCount();
        Simulator::Stop(MicroSeconds(60));
        Simulator::Run();
        const uint64_t eventsAfterRun = Simulator::GetEventCount();

        const auto stateB = topo.State(1);
        const auto dropCount = UbUtils::Get()->GetRuntimePacketDropCount();
        const auto resolutions = topo.Switch(topo.swB)->GetRecoveryResolutionCountForTest();
        const auto transferCount = topo.Switch(topo.swB)->GetRecoveryTransferCountForTest();
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_GT(blockedPendingProbe.GetNanoSeconds(), 0,
                              "blocked window observed");
        NS_TEST_ASSERT_MSG_EQ(sink.packets.size(), 1,
                              "retained packet arrives despite occupied ingress");
        if (!sink.packets.empty())
        {
            NS_TEST_ASSERT_MSG_EQ(HexOf(sink.packets[0]), HexOf(bytes), "blocked bytes preserved");
        }
        NS_TEST_ASSERT_MSG_EQ(transfers.admissionBlocks, 0u, "recovery does not reacquire ingress");
        NS_TEST_ASSERT_MSG_EQ(transfers.transferTime, transfers.validLastAckTime,
                              "LastAck transfers despite the still-occupied ingress");
        NS_TEST_ASSERT_MSG_EQ(dropCount, 0u, "blocked admission never drops the packet");
        NS_TEST_ASSERT_MSG_EQ(stateB, UbReliableRerouting::State::DRAINED, "local drain reached");
        NS_TEST_ASSERT_MSG_EQ(resolutions, 1u,
                              "a blocked transfer resolves its alternate output only once");
        NS_TEST_ASSERT_MSG_EQ(transferCount, 1u, "exactly one recovery transfer happened");
        // No polling: the whole run stays far below the event count a busy loop would produce.
        NS_TEST_ASSERT_MSG_LT(eventsAfterRun - eventsBeforeRun, 200u,
                              "blocked admission does not schedule a zero-time polling loop");
    }
    bool m_shared;
};

/**
 * @brief CBFC must not return the original upstream credit twice for a recovered packet.
 */
/** Full ingress and small healthy egress exercise mixed ordinary/recovery VOQ ownership. */
class UbReroutingMixedVoqTest : public TestCase
{
  public:
    explicit UbReroutingMixedVoqTest(bool shared, bool dwrr = false, bool creditLimited = false)
        : TestCase("Reliable rerouting - mixed VOQ isolates ingress shared=" +
                   std::to_string(shared) + " dwrr=" + std::to_string(dwrr) +
                   " credit=" + std::to_string(creditLimited)),
          m_shared(shared), m_dwrr(dwrr), m_creditLimited(creditLimited)
    {
    }
    void DoRun() override
    {
        Config::SetDefault("ns3::UbSwitch::VlScheduler", StringValue(m_dwrr ? "DWRR" : "SP"));
        ReroutingTopology topo(true, MicroSeconds(10), NanoSeconds(0), MicroSeconds(5),
                              m_shared ? FcType::CBFC_SHARED : FcType::CBFC);
        Config::SetDefault("ns3::UbSwitch::VlScheduler", StringValue("SP"));
        topo.b1->SetAttribute("UbInterframeGap", TimeValue(MicroSeconds(20)));
        // LastAck fits; data cannot enter the egress yet. Existing allocator must retain it.
        if (!m_creditLimited)
        {
            topo.b2->GetUbQueue()->SetAttribute("MaxEgressBytes", UintegerValue(16));
        }
        PacketSink sink;
        topo.d0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sink));
        auto qm = topo.Switch(topo.swB)->GetQueueManager();
        std::vector<Ptr<Packet>> packets;
        for (uint32_t i = 0; i < 4; ++i)
        {
            packets.push_back(BuildUrmaPacket(256, topo.hostA->GetId(), topo.hostD->GetId(),
                                              kPriority, RoutingType::PER_FLOW_SHORTEST_PATHS, i));
        }
        const uint32_t bytes = packets[0]->GetSize();
        qm->SetReservePerQueueBytes(2 * bytes);
        if (m_creditLimited)
        {
            // A real warmup transfer spends all data-VL credit. Three VL1 credits still allow
            // the source LastAck copies; no data credit returns until the peer's control frame.
            topo.b2->SetAttribute("CbfcInitCreditCell", IntegerValue((bytes + 159) / 160));
            topo.b2->SetAttribute("CbfcSharedInitCreditCell", IntegerValue(0));
            topo.b2->CreateAndInitFc(m_shared ? FcType::CBFC_SHARED : FcType::CBFC);
            auto warmup = BuildUrmaPacket(256, topo.hostA->GetId(), topo.hostD->GetId(),
                                          kPriority, RoutingType::PER_FLOW_SHORTEST_PATHS, 99);
            topo.Switch(topo.swB)->SendPacket(warmup, 0, 2, kPriority);
        }
        const Time arrivals[] = {NanoSeconds(10), NanoSeconds(110), MicroSeconds(2), MicroSeconds(10)};
        for (uint32_t i = 0; i < 4; ++i)
        {
            Simulator::Schedule(arrivals[i], &UbSwitch::SwitchHandlePacket,
                                topo.Switch(topo.swB), topo.b0, packets[i]);
        }
        Simulator::Schedule(MicroSeconds(1), &UbLink::Fail, topo.failedLink);
        Simulator::Schedule(MicroSeconds(15), [&]() {
            NS_TEST_ASSERT_MSG_EQ(topo.Rerouting(1)->GetRetryPacketCount(), 0u,
                                  "retry hands off even though original ingress is full");
            NS_TEST_ASSERT_MSG_EQ(topo.Rerouting(1)->GetRecoveryVoqPacketCount(), 2u,
                                  "retry and failed egress remain visible in the shared VOQ");
            NS_TEST_ASSERT_MSG_EQ(topo.Rerouting(1)->GetRecoveryVoqByteCount(), 2u * bytes,
                                  "both recovery origins use independent storage");
            NS_TEST_ASSERT_MSG_EQ(qm->GetQueueIngressTotalBytes(0, kPriority), 2u * bytes,
                                  "only the two ordinary packets consume ingress capacity");
            NS_TEST_ASSERT_MSG_EQ(qm->GetOutPortQueueBacklogBytes(2), 4u * bytes,
                                  "actual VOQ contains ordinary and recovery packets");
            NS_TEST_ASSERT_MSG_EQ(qm->GetSwitchBufferOccupancy().recovery_voq_bytes, 2u * bytes,
                                  "recovery is a subset of actual queued bytes");
        });
        Simulator::Schedule(MicroSeconds(20), [&]() {
            if (m_creditLimited)
            {
                uint8_t credits[16] = {};
                credits[kPriority] = 1;
                topo.Switch(topo.swC)->SendControlFrame(
                    UbDataLink::BuildControlCreditPacket(credits), 2);
            }
            else
            {
                topo.b2->GetUbQueue()->SetAttribute("MaxEgressBytes", UintegerValue(65536));
                topo.b2->TriggerTransmit();
            }
        });
        Simulator::Stop(MicroSeconds(80));
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(sink.packets.size(), (m_creditLimited ? 5u : 4u), "all accepted packets delivered once");
        NS_TEST_ASSERT_MSG_EQ(sink.taggedPackets, 0u, "local recovery marker never leaves owner");
        NS_TEST_ASSERT_MSG_EQ(qm->GetQueueIngressTotalBytes(0, kPriority), 0u,
                              "ordinary packets each release their reservation once");
        NS_TEST_ASSERT_MSG_EQ(topo.Rerouting(1)->GetRecoveryVoqPacketCount(), 0u,
                              "successful service releases recovery occupancy");
        NS_TEST_ASSERT_MSG_EQ(UbUtils::Get()->GetRuntimePacketDropCount(), 0u,
                              "legally arriving ordinary packets keep their promised capacity");
        auto upstream = DynamicCast<UbCbfc>(topo.b0->GetFlowControl());
        NS_TEST_ASSERT_MSG_EQ(upstream->GetCrdToReturn(kPriority),
                              (m_creditLimited ? 5 : 4) * static_cast<int32_t>((bytes + 159) / 160),
                              "ordinary admissions return credit once, recovery returns none");
        Simulator::Destroy();
    }
  private:
    bool m_shared;
    bool m_dwrr;
    bool m_creditLimited;
};

class UbReroutingCbfcCreditTest : public TestCase
{
  public:
    UbReroutingCbfcCreditTest()
        : TestCase("Reliable rerouting - upstream credit is returned at most once")
    {
    }

  private:
    uint32_t CreditToReturn(bool failLink)
    {
        ReroutingTopology topo(true,
                               NanoSeconds(20),
                               NanoSeconds(0),
                               MicroSeconds(2),
                               FcType::CBFC);
        Ptr<Packet> packet = BuildUrmaPacket(4096,
                                             topo.hostA->GetId(),
                                             topo.hostD->GetId(),
                                             kPriority,
                                             RoutingType::PER_FLOW_SHORTEST_PATHS);
        // Sink the packet at the destination port so the test observes the credit state only.
        PacketSink sink;
        topo.d0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sink));
        SendWatcher watcher;
        watcher.nodeId = topo.swB->GetId();
        watcher.portId = 1;
        watcher.link = topo.failedLink;
        watcher.port = topo.b1;
        watcher.failOffset = NanoSeconds(40);
        watcher.failOnSend = failLink;
        topo.b1->TraceConnectWithoutContext("PortTxNotify",
                                            MakeBoundCallback(&WatchSendStart, &watcher));

        Simulator::Schedule(NanoSeconds(10),
                            &UbSwitch::SwitchHandlePacket,
                            topo.Switch(topo.swB),
                            topo.b0,
                            packet);
        Simulator::Stop(MicroSeconds(60));
        Simulator::Run();

        Ptr<UbCbfc> ingressCbfc = DynamicCast<UbCbfc>(topo.b0->GetFlowControl());
        const uint32_t credits = ingressCbfc == nullptr
                                     ? 0u
                                     : static_cast<uint32_t>(ingressCbfc->GetCrdToReturn(kPriority));
        Simulator::Destroy();
        return credits;
    }

    void DoRun() override
    {
        const uint32_t healthy = CreditToReturn(false);
        const uint32_t recovered = CreditToReturn(true);
        NS_TEST_ASSERT_MSG_GT(healthy, 0u, "a forwarded packet returns upstream credit once");
        NS_TEST_ASSERT_MSG_EQ(recovered, healthy,
                              "recovery does not return the original upstream credit twice");
    }
};

/**
 * @brief Link-local control frames are disposed instead of being rerouted to a healthy neighbour.
 */
class UbReroutingControlFrameDisposalTest : public TestCase
{
  public:
    UbReroutingControlFrameDisposalTest()
        : TestCase("Reliable rerouting - retained control frame is disposed, not rerouted")
    {
    }

  private:
    void DoRun() override
    {
        ReroutingTopology topo(true, NanoSeconds(20), NanoSeconds(0), MicroSeconds(2));

        PacketSink sinkC;
        topo.c1->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sinkC));

        TransferWatcher transfers;
        topo.Rerouting(1)->TraceConnectWithoutContext(
            "Rerouting",
            MakeBoundCallback(&WatchReroutingEvent, &transfers));

        SendWatcher watcher;
        watcher.nodeId = topo.swB->GetId();
        watcher.portId = 1;
        watcher.link = topo.failedLink;
        watcher.port = topo.b1;
        // A control frame serializes in under a nanosecond, so fail right after the send boundary.
        watcher.failOffset = NanoSeconds(1);
        watcher.failOnSend = true;
        topo.b1->TraceConnectWithoutContext("PortTxNotify",
                                            MakeBoundCallback(&WatchSendStart, &watcher));

        const uint8_t credits[16] = {0};
        Ptr<Packet> controlFrame = UbDataLink::BuildControlCreditPacket(credits);
        Simulator::Schedule(NanoSeconds(10),
                            &UbSwitch::SendControlFrame,
                            topo.Switch(topo.swB),
                            controlFrame,
                            static_cast<uint32_t>(1));

        Simulator::Stop(MicroSeconds(20));
        Simulator::Run();
        const auto stateB = topo.State(1);
        const auto retry = topo.Rerouting(1)->GetRetryPacketCount();
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_EQ(sinkC.packets.size(), 0,
                              "control frame never reaches the alternate neighbour");
        NS_TEST_ASSERT_MSG_EQ(retry, 0u, "disposed control frame releases its retry storage");
        NS_TEST_ASSERT_MSG_EQ(transfers.sawControlFrameDiscard, true,
                              "control-frame disposal is visible in the evidence stream");
        NS_TEST_ASSERT_MSG_EQ(stateB, UbReliableRerouting::State::DRAINED,
                              "control-frame disposal completes local recovery");
    }
};

/**
 * @brief The suite registers ticket-01 reliable-rerouting behaviour.
 */
/**
 * @brief PFC must still observe real occupancy release across a recovery transfer.
 */
class UbReroutingPfcReleaseTest : public TestCase
{
  public:
    UbReroutingPfcReleaseTest()
        : TestCase("Reliable rerouting - PFC observes the recovered packet's occupancy release")
    {
    }

  private:
    uint32_t ControlFramesSeenByUpstream(bool failLink)
    {
        ReroutingTopology topo(true,
                               NanoSeconds(20),
                               NanoSeconds(0),
                               MicroSeconds(2),
                               FcType::PFC_FIXED);
        // Make the host-facing ingress port pause on any occupancy and resume when it drains.
        topo.b0->SetAttribute("PfcUpThld", IntegerValue(1));
        topo.b0->SetAttribute("PfcLowThld", IntegerValue(0));
        topo.b0->CreateAndInitFc(FcType::PFC_FIXED);

        ControlFrameCounter upstreamFrames;
        topo.a0->SetReceiveHandler(MakeBoundCallback(&CountControlFrame, &upstreamFrames));
        PacketSink sink;
        topo.d0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sink));

        Ptr<Packet> packet = BuildUrmaPacket(4096,
                                             topo.hostA->GetId(),
                                             topo.hostD->GetId(),
                                             kPriority,
                                             RoutingType::PER_FLOW_SHORTEST_PATHS);
        SendWatcher watcher;
        watcher.nodeId = topo.swB->GetId();
        watcher.portId = 1;
        watcher.link = topo.failedLink;
        watcher.port = topo.b1;
        watcher.failOffset = NanoSeconds(40);
        watcher.failOnSend = failLink;
        topo.b1->TraceConnectWithoutContext("PortTxNotify",
                                            MakeBoundCallback(&WatchSendStart, &watcher));

        Simulator::Schedule(NanoSeconds(10),
                            &UbSwitch::SwitchHandlePacket,
                            topo.Switch(topo.swB),
                            topo.b0,
                            packet);
        Simulator::Stop(MicroSeconds(60));
        Simulator::Run();
        const uint32_t count = upstreamFrames.count;
        Simulator::Destroy();
        return count;
    }

    void DoRun() override
    {
        const uint32_t healthy = ControlFramesSeenByUpstream(false);
        const uint32_t recovered = ControlFramesSeenByUpstream(true);
        NS_TEST_ASSERT_MSG_GT(healthy, 0u,
                              "PFC threshold crossing produces upstream flow-control frames");
        NS_TEST_ASSERT_MSG_EQ(recovered, healthy,
                              "recovery releases real occupancy without changing PFC observation");
    }
};

/** PAPER keeps a healthy upstream paused until retained ingress occupancy really drains. */
class UbReroutingPaperPendingIngressTest : public TestCase
{
  public:
    UbReroutingPaperPendingIngressTest()
        : TestCase("Reliable rerouting - PAPER pending holds PAUSE until real release and upstream progress")
    {
    }

  private:
    void ReceiveUpstream(Ptr<UbPort> port, Ptr<Packet> packet)
    {
        const auto before = port->GetCredits(kPriority);
        // Keep production control-frame decoding and flow-control wakeups intact.
        port->GetNode()->GetObject<UbSwitch>()->SwitchHandlePacket(port, packet);
        const auto after = port->GetCredits(kPriority);
        if (before != 0 && after == 0)
        {
            ++m_pauses;
        }
        if (before == 0 && after != 0)
        {
            ++m_resumes;
            m_resumeTime = Simulator::Now();
        }
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbQueueManager::ReservePerQueueBytes", UintegerValue(65536));
        Config::SetDefault("ns3::UbQueueManager::SharedPoolBytes", UintegerValue(102400));
        Config::SetDefault("ns3::UbQueueManager::PaperDynamicPfcBeta", UintegerValue(8));
        ReroutingTopology topo(true, NanoSeconds(20), NanoSeconds(0), MicroSeconds(10),
                               FcType::PFC_DYNAMIC_PAPER, 5);
        auto source = CreateObject<Node>(4);
        InitReroutingNode(source, UB_DEVICE, 1, FcType::PFC_DYNAMIC_PAPER);
        auto retainedIngress = DynamicCast<UbPort>(topo.swB->GetDevice(4));
        ConnectPorts(DynamicCast<UbPort>(source->GetDevice(0)), retainedIngress, false,
                     NanoSeconds(20));
        auto sw = topo.Switch(topo.swB);
        auto qm = sw->GetQueueManager();
        topo.a0->SetReceiveHandler(
            MakeCallback(&UbReroutingPaperPendingIngressTest::ReceiveUpstream, this));
        PacketSink sink;
        topo.d0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sink));
        TransferWatcher transfers;
        topo.Rerouting(1)->TraceConnectWithoutContext(
            "Rerouting", MakeBoundCallback(&WatchReroutingEvent, &transfers));
        std::vector<std::vector<uint8_t>> expected;
        auto makePacket = [&](uint32_t bytes, uint32_t psn) {
            const auto overhead = BuildUrmaPacket(0, topo.hostA->GetId(), topo.hostD->GetId(),
                                                  kPriority,
                                                  RoutingType::PER_FLOW_SHORTEST_PATHS)->GetSize();
            auto packet = BuildUrmaPacket(bytes - overhead, topo.hostA->GetId(),
                                           topo.hostD->GetId(), kPriority,
                                           RoutingType::PER_FLOW_SHORTEST_PATHS, psn);
            expected.push_back(PacketBytes(packet));
            return packet;
        };
        // Two real ingress processing populations; ingress stripe 4 also selects failed output 1.
        sw->SetAttribute("InPortProcessingDelay", TimeValue(MicroSeconds(3)));
        for (uint32_t i = 0; i < 10; ++i)
        {
            sw->SwitchHandlePacket(retainedIngress, makePacket(4096, i));
        }
        sw->SetAttribute("InPortProcessingDelay", TimeValue(MicroSeconds(40)));
        for (uint32_t i = 0; i < 7; ++i)
        {
            sw->SwitchHandlePacket(topo.b0, makePacket(5120, 10 + i));
        }
        Simulator::Schedule(NanoSeconds(500), [&]() {
            // PFC rechecks on enqueue completion, not processing admission. Complete a small
            // ordinary packet before the held population moves to pending to establish PAUSE.
            sw->SetAttribute("InPortProcessingDelay", TimeValue(NanoSeconds(0)));
            sw->SwitchHandlePacket(topo.b0, makePacket(100, 19));
        });
        Simulator::Schedule(MicroSeconds(1), &UbLink::Fail, PeekPointer(topo.failedLink));
        Simulator::Schedule(MicroSeconds(4), [&]() {
            NS_TEST_ASSERT_MSG_EQ(topo.State(1), UbReliableRerouting::State::WAIT_LAST_ACK,
                                  "real network LastAck is still in flight");
            NS_TEST_ASSERT_MSG_EQ(qm->GetQueuePendingRecoveryBytes(4, kPriority), 40960u,
                                  "40 KiB processing moved to pending");
            NS_TEST_ASSERT_MSG_EQ(qm->GetIngressQueueOccupancy(4, kPriority).total_bytes, 40960u,
                                  "pending retains original ingress");
            sw->SetAttribute("InPortProcessingDelay", TimeValue(NanoSeconds(0)));
            // A normal arrival/release rechecks the observed ingress using production PFC hooks.
            sw->SwitchHandlePacket(topo.b0, makePacket(100, 17));
        });
        auto followup = makePacket(512, 18);
        // Reuse the existing generated-source fixture: a production packet queue registered
        // with the allocator, without acquiring a forwarded packet's ingress reservation.
        auto upstreamQueue = CreateObject<UbPacketQueue>();
        upstreamQueue->SetInPortId(0);
        upstreamQueue->SetOutPortId(0);
        upstreamQueue->SetIngressPriority(kPriority);
        Simulator::Schedule(MicroSeconds(6), [&]() {
            NS_TEST_ASSERT_MSG_GT(m_pauses, 0u, "PAUSE reached healthy upstream");
            NS_TEST_ASSERT_MSG_EQ(m_resumes, 0u, "no early RESUME while LastAck is pending");
            NS_TEST_ASSERT_MSG_EQ(static_cast<uint32_t>(topo.a0->GetCredits(kPriority)), 0u,
                                  "production receiver keeps upstream paused");
            upstreamQueue->Push(followup);
            topo.Switch(topo.hostA)->RegisterTpWithAllocator(upstreamQueue, 0, kPriority);
            topo.a0->TriggerTransmit();
        });
        Simulator::Schedule(MicroSeconds(8), [&]() {
            NS_TEST_ASSERT_MSG_EQ(upstreamQueue->GetPacketCount(), 1u,
                                  "later data cannot advance before genuine release");
        });
        Simulator::Stop(MicroSeconds(100));
        Simulator::Run();
        NS_TEST_ASSERT_MSG_GT(transfers.validCopies, 0u, "real network LastAck arrived");
        NS_TEST_ASSERT_MSG_EQ(m_resumes, 1u, "one genuine RESUME reaches healthy upstream");
        NS_TEST_ASSERT_MSG_GT(m_resumeTime, transfers.validLastAckTime,
                              "RESUME follows LastAck and real occupancy release");
        NS_TEST_ASSERT_MSG_EQ(upstreamQueue->GetPacketCount(), 0u, "upstream queue drains");
        NS_TEST_ASSERT_MSG_EQ((SortedHex(sink.packets) == SortedHex(expected)), true,
                              "all recovered and later data arrive byte-identically exactly once");
        NS_TEST_ASSERT_MSG_EQ(qm->GetPendingRecoveryBytes(), 0u, "pending fully drains");
        NS_TEST_ASSERT_MSG_EQ(qm->GetSwitchBufferOccupancy().total_buffered_bytes, 0u,
                              "switch fully drains");
        auto hook = CreateObject<UbPfcPaperDynamicDecisionHook>();
        NS_TEST_ASSERT_MSG_EQ(hook->Evaluate(qm, 0, kPriority).xoff_threshold_bytes, 102400u,
                              "no retained PAPER correction after drain");
        NS_TEST_ASSERT_MSG_EQ(UbUtils::Get()->GetRuntimePacketDropCount(), 0u, "no data drops");
        topo.Switch(topo.hostA)->RemoveTpFromAllocator(upstreamQueue);
        Simulator::Destroy();
        Config::Reset();
    }

    uint32_t m_pauses{0};
    uint32_t m_resumes{0};
    Time m_resumeTime;
};

/** @brief One protocol family member for the recovery matrix. */
struct ReroutingProtocolCase
{
    std::string name;
    std::function<Ptr<Packet>()> build;
};

/**
 * @brief Every supported routable packet family recovers through the same failed-link path.
 *
 * The matrix uses production builders/parsers: routed RTP data plus routed ACK/SACK/CNP
 * acknowledgements, compact-transport requests, and LDST memory requests/responses. Each packet must
 * arrive byte-identically exactly once after the modeled LastAck, with exactly one recovery transfer
 * and no terminal drop report.
 */
class UbReroutingProtocolMatrixTest : public TestCase
{
  public:
    UbReroutingProtocolMatrixTest()
        : TestCase("Reliable rerouting - routed protocol families recover byte-identically")
    {
    }

  private:
    void RunCase(const ReroutingProtocolCase& protocolCase)
    {
        ReroutingTopology topo(true, NanoSeconds(20), NanoSeconds(0), MicroSeconds(2));
        PacketSink sink;
        topo.d0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sink));

        Ptr<Packet> packet = protocolCase.build();
        const auto bytes = PacketBytes(packet);
        const int classified =
            static_cast<int>(topo.Switch(topo.swB)->GetPacketTypeForTest(packet));
        NS_TEST_ASSERT_MSG_NE(classified,
                              static_cast<int>(UNKOWN_TYPE),
                              protocolCase.name + ": production classification succeeds");

        SendWatcher watcher;
        watcher.nodeId = topo.swB->GetId();
        watcher.portId = 1;
        watcher.link = topo.failedLink;
        watcher.port = topo.b1;
        // Small acknowledgement packets serialize in a few nanoseconds, so fail just after the send
        // boundary while the attempt is still in flight on the link.
        watcher.failOffset = NanoSeconds(1);
        watcher.failOnSend = true;
        topo.b1->TraceConnectWithoutContext("PortTxNotify",
                                            MakeBoundCallback(&WatchSendStart, &watcher));

        Simulator::Schedule(NanoSeconds(10),
                            &UbSwitch::SwitchHandlePacket,
                            topo.Switch(topo.swB),
                            topo.b0,
                            packet);
        Simulator::Stop(MicroSeconds(40));
        Simulator::Run();

        const auto stateB = topo.State(1);
        const auto retry = topo.Rerouting(1)->GetRetryPacketCount();
        const auto transfers = topo.Switch(topo.swB)->GetRecoveryTransferCountForTest();
        const auto drops = UbUtils::Get()->GetRuntimePacketDropCount();
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_EQ(sink.packets.size(), static_cast<size_t>(1),
                              protocolCase.name + ": delivered exactly once");
        NS_TEST_ASSERT_MSG_EQ(HexOf(sink.packets.front()), HexOf(bytes),
                              protocolCase.name + ": header/payload preserved");
        NS_TEST_ASSERT_MSG_EQ(retry, 0u, protocolCase.name + ": retry storage released");
        NS_TEST_ASSERT_MSG_EQ(transfers, 1u, protocolCase.name + ": one recovery transfer only");
        NS_TEST_ASSERT_MSG_EQ(drops, 0u, protocolCase.name + ": no terminal packet drop");
        NS_TEST_ASSERT_MSG_EQ(stateB, UbReliableRerouting::State::DRAINED,
                              protocolCase.name + ": local drain reached");
    }

    void DoRun() override
    {
        const uint32_t src = 1;
        const uint32_t dst = 3;
        const std::vector<ReroutingProtocolCase> matrix = {
            {"RTP data",
             [src, dst]() {
                 return BuildUrmaPacket(2048,
                                        src,
                                        dst,
                                        kPriority,
                                        RoutingType::PER_FLOW_SHORTEST_PATHS);
             }},
            {"RTP ACK without CETPH",
             [src, dst]() {
                 return BuildRoutedTpAckPacket(0,
                                               src,
                                               dst,
                                               TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH,
                                               kPriority,
                                               RoutingType::PER_FLOW_SHORTEST_PATHS);
             }},
            {"RTP ACK with CETPH",
             [src, dst]() {
                 return BuildRoutedTpAckPacket(0,
                                               src,
                                               dst,
                                               TpOpcode::TP_OPCODE_ACK_WITH_CETPH,
                                               kPriority,
                                               RoutingType::PER_FLOW_SHORTEST_PATHS);
             }},
            {"RTP SACK without CETPH",
             [src, dst]() {
                 return BuildRoutedTpAckPacket(0,
                                               src,
                                               dst,
                                               TpOpcode::TP_OPCODE_SACK_WITHOUT_CETPH,
                                               kPriority,
                                               RoutingType::PER_FLOW_SHORTEST_PATHS);
             }},
            {"RTP SACK with CETPH",
             [src, dst]() {
                 return BuildRoutedTpAckPacket(0,
                                               src,
                                               dst,
                                               TpOpcode::TP_OPCODE_SACK_WITH_CETPH,
                                               kPriority,
                                               RoutingType::PER_FLOW_SHORTEST_PATHS);
             }},
            {"CTP request",
             [src, dst]() {
                 return BuildCompactTransportPacket(512,
                                                    src,
                                                    dst,
                                                    UbTransactionOpcode::WRITE,
                                                    kPriority,
                                                    RoutingType::PER_FLOW_SHORTEST_PATHS);
             }},
            {"LDST request",
             [src, dst]() {
                 return BuildLdstMemoryPacket(256,
                                              src,
                                              dst,
                                              UbTransactionOpcode::WRITE,
                                              kPriority,
                                              RoutingType::PER_FLOW_SHORTEST_PATHS);
             }},
            {"LDST response",
             [src, dst]() {
                 return BuildLdstMemoryPacket(256,
                                              src,
                                              dst,
                                              UbTransactionOpcode::TAACK,
                                              kPriority,
                                              RoutingType::PER_FLOW_SHORTEST_PATHS);
             }},
        };

        for (const auto& protocolCase : matrix)
        {
            RunCase(protocolCase);
        }
    }
};

/**
 * @brief Every routing representation and selection entry point excludes the failed output.
 *
 * Tested representations: materialized routing table, compact next-hop index, explicit candidate
 * lists (the seam structured/index candidates flow through), restricted candidate sets, and the
 * endpoint lookup. Only removed candidates change; everything else keeps working.
 */
class UbReroutingRoutingRepresentationTest : public TestCase
{
  public:
    UbReroutingRoutingRepresentationTest()
        : TestCase("Reliable rerouting - table, index, restricted and selector candidates filter")
    {
    }

  private:
    RoutingKey MakeKey()
    {
        RoutingKey key;
        key.sip = utils::NodeIdToIp(1).Get();
        key.dip = utils::NodeIdToIp(3).Get();
        key.sport = 1;
        key.dport = 1;
        key.priority = kPriority;
        key.routingType = RoutingType::PER_FLOW_SHORTEST_PATHS;
        return key;
    }

  public:
    static MultipathSelector StringToSelector(const std::string& name)
    {
        if (name == "HASH64") return MultipathSelector::HASH64;
        if (name == "CRC32") return MultipathSelector::CRC32;
        if (name == "TOEPLITZ") return MultipathSelector::TOEPLITZ;
        if (name == "ROUND_ROBIN") return MultipathSelector::ROUND_ROBIN;
        if (name == "ADAPTIVE") return MultipathSelector::ADAPTIVE;
        return MultipathSelector::INGRESS_PORT_STRIPE;
    }

  private:
    void DoRun() override
    {
        // Materialized table representation.
        {
            Ptr<UbRoutingProcess> routing = CreateObject<UbRoutingProcess>();
            routing->SetNodeId(1);
            routing->GetRoutingTable().AddShortestRoute(utils::NodeIdToIp(3).Get(), {1, 2});
            for (const std::string selector : {"HASH64", "CRC32", "TOEPLITZ", "INGRESS_PORT_STRIPE"})
            {
                routing->SetAttribute("MultipathSelector", EnumValue(StringToSelector(selector)));
                bool selectedShortestPath = false;
                RoutingKey key = MakeKey();
                const int healthy = routing->GetOutPort(key, selectedShortestPath, 0);
                const bool healthyInsideCandidates = (healthy == 1 || healthy == 2);
                NS_TEST_ASSERT_MSG_EQ(
                    healthyInsideCandidates,
                    true,
                    selector + ": healthy selection stays inside the candidates");
                if (selector == "INGRESS_PORT_STRIPE")
                {
                    NS_TEST_ASSERT_MSG_EQ(healthy,
                                          1,
                                          selector + ": ingress stripe picks the first candidate");
                }
                routing->SetOutputUnavailable(1);
                RoutingKey filteredKey = MakeKey();
                const int filtered = routing->GetOutPort(filteredKey, selectedShortestPath, 0);
                NS_TEST_ASSERT_MSG_EQ(filtered, 2, selector + ": failed output excluded");
                NS_TEST_ASSERT_MSG_EQ(routing->LastFailureExcludedAllCandidates(), false,
                                      selector + ": a legal candidate remained");
                routing->ClearOutputUnavailable(1);
            }
        }

        // Compact next-hop index representation.
        {
            UbNextHopIndexBuilder builder(4);
            builder.AddRoute({1, 3, 0, {1, 2}, {}});
            builder.AddPrimaryRoute(1, 3, {1, 2}, {});
            std::shared_ptr<const UbNextHopIndex> index = builder.Build();
            Ptr<UbRoutingProcess> routing = CreateObject<UbRoutingProcess>();
            routing->SetNodeId(1);
            routing->SetNextHopIndex(index);
            routing->SetAttribute("MultipathSelector",
                                  EnumValue(MultipathSelector::INGRESS_PORT_STRIPE));
            bool selectedShortestPath = false;
            RoutingKey key = MakeKey();
            const int healthy = routing->GetOutPort(key, selectedShortestPath, 0);
            NS_TEST_ASSERT_MSG_EQ(healthy, 1, "index: healthy candidate selected");
            routing->SetOutputUnavailable(1);
            RoutingKey filteredKey = MakeKey();
            const int filtered = routing->GetOutPort(filteredKey, selectedShortestPath, 0);
            NS_TEST_ASSERT_MSG_EQ(filtered, 2, "index: failed output excluded");
        }

        // Explicit candidate lists (the structured-rule / precomputed candidate seam).
        {
            Ptr<UbRoutingProcess> routing = CreateObject<UbRoutingProcess>();
            routing->SetNodeId(1);
            routing->SetAttribute("MultipathSelector",
                                  EnumValue(MultipathSelector::INGRESS_PORT_STRIPE));
            bool selectedShortestPath = false;
            RoutingKey key = MakeKey();
            NS_TEST_ASSERT_MSG_EQ(routing->SelectOutPort(key,
                                                         {1, 2},
                                                         {},
                                                         selectedShortestPath,
                                                         0),
                                  1,
                                  "explicit candidates: healthy selection");
            routing->SetOutputUnavailable(1);
            RoutingKey filteredKey = MakeKey();
            NS_TEST_ASSERT_MSG_EQ(routing->SelectOutPort(filteredKey,
                                                         {1, 2},
                                                         {},
                                                         selectedShortestPath,
                                                         0),
                                  2,
                                  "explicit candidates: failed output excluded");
            // A candidate set that contains only the failed output is a failure-aware no-route.
            RoutingKey onlyFailed = MakeKey();
            NS_TEST_ASSERT_MSG_EQ(routing->SelectOutPort(onlyFailed,
                                                         {1},
                                                         {},
                                                         selectedShortestPath,
                                                         0),
                                  -1,
                                  "explicit candidates: no legal output");
            NS_TEST_ASSERT_MSG_EQ(routing->LastFailureExcludedAllCandidates(), true,
                                  "failure-aware diagnostic is armed");
        }

        // Restricted candidates and endpoint lookups keep their own constraints.
        {
            Ptr<UbRoutingProcess> routing = CreateObject<UbRoutingProcess>();
            routing->SetNodeId(1);
            routing->GetRoutingTable().AddShortestRoute(utils::NodeIdToIp(3).Get(), {1, 2});
            routing->GetRoutingTable().AddShortestRoute(utils::NodeIdToIp(3, 0).Get(), {1, 2});
            routing->SetAttribute("MultipathSelector",
                                  EnumValue(MultipathSelector::INGRESS_PORT_STRIPE));
            bool selectedShortestPath = false;
            RoutingKey restricted = MakeKey();
            NS_TEST_ASSERT_MSG_EQ(routing->GetOutPortRestricted(restricted,
                                                                {1},
                                                                selectedShortestPath,
                                                                0),
                                  1,
                                  "restricted candidates: allowed healthy output");
            routing->SetOutputUnavailable(1);
            RoutingKey restrictedFiltered = MakeKey();
            NS_TEST_ASSERT_MSG_EQ(routing->GetOutPortRestricted(restrictedFiltered,
                                                                {1},
                                                                selectedShortestPath,
                                                                0),
                                  -1,
                                  "restricted candidates: failed output stays excluded");
            NS_TEST_ASSERT_MSG_EQ(routing->LastFailureExcludedAllCandidates(), true,
                                  "restricted candidates: failure-aware diagnostic");
            RoutingKey endpoint = MakeKey();
            endpoint.dip = utils::NodeIdToIp(3, 0).Get();
            NS_TEST_ASSERT_MSG_EQ(routing->GetOutPortForEndpointRestricted(endpoint,
                                                                           {1, 2},
                                                                           selectedShortestPath,
                                                                           0),
                                  2,
                                  "endpoint candidates: failed output excluded");
        }

        // A physically connected detour outside the allowed candidate set is not a fallback.
        {
            Ptr<UbRoutingProcess> routing = CreateObject<UbRoutingProcess>();
            routing->SetNodeId(1);
            routing->GetRoutingTable().AddShortestRoute(utils::NodeIdToIp(3).Get(), {1});
            routing->GetRoutingTable().AddOtherRoute(utils::NodeIdToIp(3).Get(), {2});
            routing->SetAttribute("MultipathSelector", EnumValue(MultipathSelector::HASH64));
            routing->SetOutputUnavailable(1);
            bool selectedShortestPath = false;
            RoutingKey key = MakeKey();
            // PER_FLOW_SHORTEST_PATHS forbids the detour, so no legal candidate remains.
            NS_TEST_ASSERT_MSG_EQ(routing->GetOutPort(key, selectedShortestPath, 0),
                                  -1,
                                  "a disallowed detour is not used as a fallback");
            NS_TEST_ASSERT_MSG_EQ(routing->LastFailureExcludedAllCandidates(), true,
                                  "disallowed-detour case is failure-aware");
            RoutingKey recoveryKey = MakeKey();
            NS_TEST_ASSERT_MSG_EQ(routing->GetOutPortAfterFailure(recoveryKey, selectedShortestPath, 0),
                                  2, "explicit failure entry permits configured detour");
            NS_TEST_ASSERT_MSG_EQ(selectedShortestPath, false, "detour is recorded");
            RoutingKey ingressBlocked = MakeKey();
            NS_TEST_ASSERT_MSG_EQ(routing->GetOutPortAfterFailure(ingressBlocked, selectedShortestPath, 1),
                                  -1, "ingress exclusion cannot authorize a detour");
            const std::vector<uint16_t> onlyDetour{2};
            RoutingKey denied = MakeKey();
            NS_TEST_ASSERT_MSG_EQ(routing->GetOutPortAfterFailure(denied, selectedShortestPath, 0,
                                                                  false, &onlyDetour),
                                  -1, "allow-list excluded shortest before failure");
            RoutingKey missingEndpoint = MakeKey();
            missingEndpoint.dip = utils::NodeIdToIp(3, 9).Get();
            NS_TEST_ASSERT_MSG_EQ(routing->GetOutPortAfterFailure(missingEndpoint, selectedShortestPath,
                                                                  0, true),
                                  -1, "exact missing endpoint cannot fall back to device route");
            // With an all-paths routing type the same detour is legal again.
            RoutingKey allPathsKey = MakeKey();
            allPathsKey.routingType = RoutingType::PER_FLOW_ALL_PATHS;
            NS_TEST_ASSERT_MSG_EQ(routing->GetOutPort(allPathsKey, selectedShortestPath, 0),
                                  2,
                                  "the detour stays legal for an all-paths routing type");
        }

        // Original-ingress exclusion still applies on top of the failure filter.
        {
            Ptr<UbRoutingProcess> routing = CreateObject<UbRoutingProcess>();
            routing->SetNodeId(1);
            routing->GetRoutingTable().AddShortestRoute(utils::NodeIdToIp(3).Get(), {1, 2});
            routing->SetAttribute("MultipathSelector", EnumValue(MultipathSelector::HASH64));
            routing->SetOutputUnavailable(1);
            bool selectedShortestPath = false;
            // Exclusive original-ingress routing plus the detected failed output leaves no legal
            // candidate, which must be reported as a failure-aware no-route.
            RoutingKey ingressOnly = MakeKey();
            NS_TEST_ASSERT_MSG_EQ(routing->GetOutPort(ingressOnly, selectedShortestPath, 2),
                                  -1,
                                  "original-ingress-only candidates cannot use the failed output");
            NS_TEST_ASSERT_MSG_EQ(routing->LastFailureExcludedAllCandidates(), true,
                                  "original-ingress-only case is failure-aware");
            // A different ingress port can still use the healthy alternate.
            RoutingKey alternateIngress = MakeKey();
            NS_TEST_ASSERT_MSG_EQ(routing->GetOutPort(alternateIngress, selectedShortestPath, 0),
                                  2,
                                  "healthy candidate remains reachable from another ingress");
        }
    }
};

/**
 * @brief Local attempt identities stay distinct when a transport resend repeats the same PSN.
 *
 * Two transmit attempts that carry identical transport bytes (the original and its transport
 * resend) each get their own local token, each transfer exactly once, and are both delivered. Local
 * recovery must not collapse them, because transport retransmission remains an independent
 * mechanism.
 */
class UbReroutingAttemptIdentityTest : public TestCase
{
  public:
    UbReroutingAttemptIdentityTest()
        : TestCase("Reliable rerouting - original and transport resend are distinct attempts")
    {
    }

  private:
    void DoRun() override
    {
        // A positive detection interval lets both transmit attempts happen before the output is
        // blocked, exactly like an original packet and its transport resend during detection.
        ReroutingTopology topo(true, NanoSeconds(20), MicroSeconds(1), MicroSeconds(2));
        PacketSink sink;
        topo.d0->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sink));

        // Same PSN and identical bytes on both attempts, exactly like a transport resend.
        Ptr<Packet> original = BuildUrmaPacket(1024,
                                               topo.hostA->GetId(),
                                               topo.hostD->GetId(),
                                               kPriority,
                                               RoutingType::PER_FLOW_SHORTEST_PATHS,
                                               7);
        Ptr<Packet> resend = original->Copy();
        const auto bytes = PacketBytes(original);

        std::vector<uint64_t> attemptTokens;
        topo.Rerouting(1)->TraceConnectWithoutContext(
            "Rerouting",
            MakeBoundCallback(&WatchAttemptTokens, &attemptTokens));

        // Both attempts are sent while the link is failing, so both stay in retry storage.
        SendWatcher watcher;
        watcher.nodeId = topo.swB->GetId();
        watcher.portId = 1;
        watcher.link = topo.failedLink;
        watcher.port = topo.b1;
        watcher.failOffset = NanoSeconds(1);
        watcher.failOnSend = true;
        topo.b1->TraceConnectWithoutContext("PortTxNotify",
                                            MakeBoundCallback(&WatchSendStart, &watcher));

        Simulator::Schedule(NanoSeconds(10),
                            &UbSwitch::SwitchHandlePacket,
                            topo.Switch(topo.swB),
                            topo.b0,
                            original);
        // The resend follows the original inside the detection window.
        Simulator::Schedule(NanoSeconds(300),
                            &UbSwitch::SwitchHandlePacket,
                            topo.Switch(topo.swB),
                            topo.b0,
                            resend);

        Simulator::Stop(MicroSeconds(20));
        Simulator::Run();
        const auto transfers = topo.Switch(topo.swB)->GetRecoveryTransferCountForTest();
        const auto retry = topo.Rerouting(1)->GetRetryPacketCount();
        const auto stateB = topo.State(1);
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_EQ(transfers, 2u, "each local attempt transfers exactly once");
        NS_TEST_ASSERT_MSG_EQ(attemptTokens.size(), 2u, "two local attempts were transferred");
        if (attemptTokens.size() == 2)
        {
            NS_TEST_ASSERT_MSG_NE(attemptTokens[0], 0u, "original has a local attempt token");
            NS_TEST_ASSERT_MSG_NE(attemptTokens[1], 0u, "resend has a local attempt token");
            NS_TEST_ASSERT_MSG_NE(attemptTokens[0],
                                  attemptTokens[1],
                                  "original and resend keep distinct local identities");
        }
        NS_TEST_ASSERT_MSG_EQ(retry, 0u, "retry storage released for both attempts");
        NS_TEST_ASSERT_MSG_EQ(sink.packets.size(), 2u, "both attempts reach the destination");
        NS_TEST_ASSERT_MSG_EQ(HexOf(sink.packets[0]), HexOf(bytes), "attempt bytes preserved");
        NS_TEST_ASSERT_MSG_EQ(HexOf(sink.packets[1]), HexOf(bytes), "resend bytes preserved");
        NS_TEST_ASSERT_MSG_EQ(stateB, UbReliableRerouting::State::DRAINED, "local drain reached");
    }
};

void CountLastAckSerialization(uint32_t* count, Ptr<Packet> packet, Time)
{
    UbLastAckMessage message;
    if (ParseReliableReroutingLastAck(packet, message))
    {
        ++*count;
    }
}

struct LastAckLoss
{
    uint32_t drop{0};
    uint32_t seen{0};
    uint32_t corruption{0};
};

void ReceiveWithLastAckLoss(LastAckLoss* loss, Ptr<UbPort> port, Ptr<Packet> packet)
{
    UbLastAckMessage message;
    if (ParseReliableReroutingLastAck(packet, message))
    {
        if (++loss->seen <= loss->drop)
        {
            return;
        }
        if (loss->corruption != 0)
        {
            packet = BuildReliableReroutingLastAck(
                message.sourceNode + (loss->corruption == 2 ? 1 : 0), message.destinationNode,
                message.destinationPort + (loss->corruption == 3 ? 100 : 0),
                message.receiveNext + (loss->corruption == 1 ? 1 : 0));
        }
    }
    port->GetNode()->GetObject<UbSwitch>()->SwitchHandlePacket(port, packet);
}

class UbReroutingWireHandshakeTest : public TestCase
{
  public:
    UbReroutingWireHandshakeTest(uint32_t drop = 0, bool cna24 = false)
        : TestCase("LastAck real three-copy handshake drop=" + std::to_string(drop) +
                   (cna24 ? " CNA24" : " CNA16")), m_drop(drop), m_cna24(cna24) {}
    void DoRun() override
    {
        if (m_cna24)
        {
            for (uint32_t i = 0; i < 4096; ++i)
            {
                CreateObject<Node>();
            }
        }
        ReroutingTopology topo;
        uint32_t left = 0, right = 0;
        LastAckLoss leftLoss{m_drop}, rightLoss{m_drop};
        topo.b2->SetReceiveHandler(MakeBoundCallback(&ReceiveWithLastAckLoss, &leftLoss));
        topo.c2->SetReceiveHandler(MakeBoundCallback(&ReceiveWithLastAckLoss, &rightLoss));
        TransferWatcher leftEvents, rightEvents;
        topo.Rerouting(1)->TraceConnectWithoutContext("Rerouting",
            MakeBoundCallback(&WatchReroutingEvent, &leftEvents));
        topo.c1->GetReliableRerouting()->TraceConnectWithoutContext("Rerouting",
            MakeBoundCallback(&WatchReroutingEvent, &rightEvents));
        topo.b2->TraceConnectWithoutContext("TraComEventNotify",
            MakeBoundCallback(&CountLastAckSerialization, &left));
        topo.c2->TraceConnectWithoutContext("TraComEventNotify",
            MakeBoundCallback(&CountLastAckSerialization, &right));
        topo.failedLink->Fail();
        Simulator::Stop(MicroSeconds(20));
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(left, 3u, "three source sends left");
        NS_TEST_ASSERT_MSG_EQ(right, 3u, "three source sends right");
        NS_TEST_ASSERT_MSG_EQ(leftEvents.validCopies, 1u, "one recovery start left");
        NS_TEST_ASSERT_MSG_EQ(rightEvents.validCopies, 1u, "one recovery start right");
        NS_TEST_ASSERT_MSG_EQ(leftEvents.duplicateCopies, (2 - m_drop), "duplicate suppression left");
        NS_TEST_ASSERT_MSG_EQ(rightEvents.duplicateCopies, (2 - m_drop), "duplicate suppression right");
        NS_TEST_ASSERT_MSG_EQ(topo.State(1), UbReliableRerouting::State::DRAINED, "left empty drain");
        NS_TEST_ASSERT_MSG_EQ(topo.c1->GetReliableRerouting()->GetState(),
                              UbReliableRerouting::State::DRAINED, "right empty drain");
        Simulator::Destroy();
    }
  private:
    uint32_t m_drop;
    bool m_cna24;
};

class UbReroutingHandshakeTimeoutTest : public TestCase
{
  public:
    explicit UbReroutingHandshakeTimeoutTest(uint32_t mode)
        : TestCase("LastAck explicit failure mode " + std::to_string(mode)), m_mode(mode) {}
    void DoRun() override
    {
#ifndef _WIN32
        const auto outcome = RunInChild([this]() {
            ReroutingTopology topo(true, NanoSeconds(20), NanoSeconds(0),
                                   m_mode == 3 ? Seconds(2) : NanoSeconds(0));
            LastAckLoss loss{m_mode == 0 ? 3u : 0u, 0, m_mode == 1 ? 1u : (m_mode == 5 ? 2u : (m_mode == 6 ? 3u : 0u))};
            topo.b2->SetReceiveHandler(MakeBoundCallback(&ReceiveWithLastAckLoss, &loss));
            if (m_mode == 2)
            {
                topo.Switch(topo.swB)->GetAllocator()->SetAttribute("AllocationTime",
                                                                   TimeValue(MilliSeconds(600)));
            }
            if (m_mode == 4)
            {
                topo.b2->SetAttribute("UbInterframeGap", TimeValue(MilliSeconds(200)));
                topo.c2->SetAttribute("UbInterframeGap", TimeValue(MilliSeconds(200)));
            }
            topo.failedLink->Fail();
            Simulator::Stop(Seconds(2));
            Simulator::Run();
            Simulator::Destroy();
        });
        if (m_mode == 4)
        {
            NS_TEST_ASSERT_MSG_EQ(outcome.exitCode, 1, "third serialization at 400ms beats deadline despite IFG");
            return;
        }
        NS_TEST_ASSERT_MSG_EQ(outcome.exitCode, 42, "explicit recovery failure");
        const std::string expected = m_mode == 2 ? "ReliableRerouting_SEND_TO" : "ReliableRerouting_TO";
        NS_TEST_ASSERT_MSG_NE(outcome.diagnostic.find(expected), std::string::npos,
                              "expected timeout diagnostic: " + outcome.diagnostic);
#endif
    }
  private:
    uint32_t m_mode;
};

class UbReroutingLastAckCodecTest : public TestCase
{
  public:
    UbReroutingLastAckCodecTest() : TestCase("LastAck codec validates wire layout and device identity") {}
    void DoRun() override
    {
        for (uint32_t source : {12u, 4096u})
        {
            auto packet = BuildReliableReroutingLastAck(source, 23, 65535, 0x1234);
            NS_TEST_ASSERT_MSG_EQ(packet->GetSize(), (source == 12 ? 16 : 18), "wire length");
            UbLastAckMessage message;
            NS_TEST_ASSERT_MSG_EQ(ParseReliableReroutingLastAck(packet, message), true, "valid message");
            NS_TEST_ASSERT_MSG_EQ(message.sourceNode, source, "source device");
            NS_TEST_ASSERT_MSG_EQ(message.destinationNode, 23, "destination device");
            NS_TEST_ASSERT_MSG_EQ(message.destinationPort, 65535, "full port range");
            NS_TEST_ASSERT_MSG_EQ(message.receiveNext, 0x1234, "packet boundary");
            const auto bytes = PacketBytes(packet);
            NS_TEST_ASSERT_MSG_EQ(bytes[bytes.size()-4], 0x12, "boundary network byte order");
            NS_TEST_ASSERT_MSG_EQ(bytes[bytes.size()-3], 0x34, "boundary network byte order");
            packet->RemoveAtEnd(1);
            NS_TEST_ASSERT_MSG_EQ(ParseReliableReroutingLastAck(packet, message), false, "truncated");
        }
    }
};

class UbReroutingFailureRepresentationsTest : public TestCase
{
  public:
    UbReroutingFailureRepresentationsTest() : TestCase("Failure detours preserve table index rule endpoint constraints") {}
    void DoRun() override
    {
        for (uint32_t representation = 0; representation < 3; ++representation)
        {
            auto routing = CreateObject<UbRoutingProcess>();
            routing->SetNodeId(1);
            const uint32_t destination = utils::NodeIdToIp(3, 0).Get();
            if (representation == 0)
            {
                routing->GetRoutingTable().AddShortestRoute(destination, {1});
                routing->GetRoutingTable().AddOtherRoute(destination, {2});
            }
            else if (representation == 1)
            {
                UbNextHopIndexBuilder builder(4, 1);
                builder.AddRoute({1, 3, 0, {1}, {2}});
                routing->SetNextHopIndex(builder.Build());
            }
            else
            {
                auto rules = std::make_shared<UbRuleRoutingTable>();
                rules->AddPortGroup({1, 10, {1}});
                rules->AddPortGroup({1, 11, {2}});
                rules->AddRule({1, {3, 0}, {10}, {11}});
                rules->PrepareForLookup();
                routing->SetRuleRoutingTable(rules);
            }
            RoutingKey key{};
            key.dip = destination;
            key.routingType = RoutingType::PER_FLOW_SHORTEST_PATHS;
            bool shortest = false;
            NS_TEST_ASSERT_MSG_EQ(routing->GetOutPortAfterFailure(key, shortest, 0, true), 1,
                                  "healthy shortest preserved");
            routing->SetOutputUnavailable(1);
            NS_TEST_ASSERT_MSG_EQ(routing->GetOutPortForEndpoint(key, shortest, 0), -1,
                                  "ordinary API still rejects detour");
            NS_TEST_ASSERT_MSG_EQ(routing->GetOutPortAfterFailure(key, shortest, 0, true), 2,
                                  "same failure detour for each representation");
            NS_TEST_ASSERT_MSG_EQ(shortest, false, "selected nonshortest");
            NS_TEST_ASSERT_MSG_EQ(routing->GetOutPortAfterFailure(key, shortest, 2, true), -1,
                                  "final selector cannot use ingress");
            const std::vector<uint16_t> onlyShortest{1};
            const std::vector<uint16_t> onlyDetour{2};
            NS_TEST_ASSERT_MSG_EQ(routing->GetOutPortAfterFailure(key, shortest, 0, true, &onlyShortest),
                                  -1, "allow-list forbids detour");
            NS_TEST_ASSERT_MSG_EQ(routing->GetOutPortAfterFailure(key, shortest, 0, true, &onlyDetour),
                                  -1, "allow-list forbids original shortest");
            key.dip = utils::NodeIdToIp(3, 1).Get();
            NS_TEST_ASSERT_MSG_EQ(routing->GetOutPortAfterFailure(key, shortest, 0, true), -1,
                                  "exact endpoint has no primary fallback");
        }
        Simulator::Destroy();
    }
};

/** Three leaves and two spines, with hosts on two leaves and no parallel backup links. */
class UbReroutingClosHandshakeTest : public TestCase
{
  public:
    UbReroutingClosHandshakeTest() : TestCase("Reliable rerouting - real Clos three-hop handshake") {}
    void DoRun() override
    {
        auto h0 = CreateObject<Node>();
        auto h1 = CreateObject<Node>();
        auto l0 = CreateObject<Node>();
        auto l1 = CreateObject<Node>();
        auto l2 = CreateObject<Node>();
        auto s0 = CreateObject<Node>();
        auto s1 = CreateObject<Node>();
        InitReroutingNode(h0, UB_DEVICE, 1);
        InitReroutingNode(h1, UB_DEVICE, 1);
        for (auto leaf : {l0, l1, l2}) InitReroutingNode(leaf, UB_SWITCH, 3);
        for (auto spine : {s0, s1}) InitReroutingNode(spine, UB_SWITCH, 3);
        const auto port = [](Ptr<Node> node, uint32_t id) {
            return DynamicCast<UbPort>(node->GetDevice(id));
        };
        ConnectPorts(port(h0, 0), port(l0, 0), true, NanoSeconds(20));
        ConnectPorts(port(h1, 0), port(l1, 0), true, NanoSeconds(20));
        auto failed = ConnectPorts(port(l0, 1), port(s0, 0), true, NanoSeconds(200), MicroSeconds(1));
        ConnectPorts(port(l0, 2), port(s1, 0), true, NanoSeconds(20));
        ConnectPorts(port(l1, 1), port(s0, 1), true, NanoSeconds(20));
        ConnectPorts(port(l1, 2), port(s1, 1), true, NanoSeconds(20));
        ConnectPorts(port(l2, 1), port(s0, 2), true, NanoSeconds(20));
        ConnectPorts(port(l2, 2), port(s1, 2), true, NanoSeconds(20));
        for (auto leaf : {l0, l1, l2})
        {
            AddRoute(leaf, s0->GetId(), {1});
            AddRoute(leaf, s1->GetId(), {2});
            auto routing = leaf->GetObject<UbSwitch>()->GetRoutingProcess();
            routing->GetRoutingTable().AddOtherRoute(utils::NodeIdToIp(s0->GetId()).Get(), {2});
            routing->GetRoutingTable().AddOtherRoute(utils::NodeIdToIp(s1->GetId()).Get(), {1});
            for (auto destination : {l0, l1, l2})
            {
                if (leaf != destination) AddRoute(leaf, destination->GetId(), {1, 2});
            }
            AddRoute(leaf, h0->GetId(), leaf == l0 ? std::vector<uint16_t>{0} : std::vector<uint16_t>{1, 2});
            AddRoute(leaf, h1->GetId(), leaf == l1 ? std::vector<uint16_t>{0} : std::vector<uint16_t>{1, 2});
        }
        for (auto spine : {s0, s1})
        {
            auto routing = spine->GetObject<UbSwitch>()->GetRoutingProcess();
            for (auto destination : {l0, h0})
            {
                AddRoute(spine, destination->GetId(), {0});
                routing->GetRoutingTable().AddOtherRoute(utils::NodeIdToIp(destination->GetId()).Get(), {1, 2});
            }
            for (auto destination : {l1, h1})
            {
                AddRoute(spine, destination->GetId(), {1});
                routing->GetRoutingTable().AddOtherRoute(utils::NodeIdToIp(destination->GetId()).Get(), {0, 2});
            }
            AddRoute(spine, l2->GetId(), {2});
            AddRoute(spine, (spine == s0 ? s1 : s0)->GetId(), {0, 1, 2});
        }
        uint32_t serializations = 0;
        for (auto sw : {l0, l1, l2, s0, s1})
        {
            sw->GetObject<UbSwitch>()->GetRoutingProcess()->SetAttribute(
                "MultipathSelector", EnumValue(MultipathSelector::INGRESS_PORT_STRIPE));
            for (uint32_t id = 0; id < sw->GetNDevices(); ++id)
            {
                port(sw, id)->TraceConnectWithoutContext("TraComEventNotify",
                    MakeBoundCallback(&CountLastAckSerialization, &serializations));
            }
        }
        PacketSink sink0, sink1;
        port(h0, 0)->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sink0));
        port(h1, 0)->SetReceiveHandler(MakeBoundCallback(&CaptureDeliveredPacket, &sink1));
        TransferWatcher left, right;
        port(l0, 1)->GetReliableRerouting()->TraceConnectWithoutContext("Rerouting",
            MakeBoundCallback(&WatchReroutingEvent, &left));
        port(s0, 0)->GetReliableRerouting()->TraceConnectWithoutContext("Rerouting",
            MakeBoundCallback(&WatchReroutingEvent, &right));
        for (uint32_t copy = 0; copy < 2; ++copy)
        {
            const Time when = copy == 0 ? NanoSeconds(10) : NanoSeconds(1150);
            auto forward = BuildUrmaPacket(1024, h0->GetId(), h1->GetId(), kPriority,
                                            RoutingType::PER_FLOW_SHORTEST_PATHS, copy);
            auto reverse = BuildUrmaPacket(1024, h1->GetId(), h0->GetId(), kPriority,
                                            RoutingType::PER_FLOW_SHORTEST_PATHS, copy);
            Simulator::Schedule(when, &UbSwitch::SwitchHandlePacket, l0->GetObject<UbSwitch>(), port(l0, 0), forward);
            Simulator::Schedule(when, &UbSwitch::SwitchHandlePacket, l1->GetObject<UbSwitch>(), port(l1, 0), reverse);
        }
        Simulator::Schedule(NanoSeconds(100), &UbLink::Fail, failed);
        Simulator::Stop(MicroSeconds(30));
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(serializations, 18u, "six messages each serialize on three real hops");
        NS_TEST_ASSERT_MSG_EQ(left.validCopies, 1u, "leaf receives exactly one first valid copy");
        NS_TEST_ASSERT_MSG_EQ(right.validCopies, 1u, "spine receives exactly one first valid copy");
        NS_TEST_ASSERT_MSG_GT(left.transferTime, Time(0), "leaf retains failure traffic");
        NS_TEST_ASSERT_MSG_GT(right.transferTime, Time(0), "spine retains failure traffic");
        NS_TEST_ASSERT_MSG_EQ(sink0.packets.size(), 2u, "reverse retained and new traffic delivered once");
        NS_TEST_ASSERT_MSG_EQ(sink1.packets.size(), 2u, "forward retained and new traffic delivered once");
        NS_TEST_ASSERT_MSG_EQ(port(l0, 1)->GetReliableRerouting()->GetState(),
                              UbReliableRerouting::State::DRAINED, "leaf drained");
        NS_TEST_ASSERT_MSG_EQ(port(s0, 0)->GetReliableRerouting()->GetState(),
                              UbReliableRerouting::State::DRAINED, "spine drained");
        Simulator::Destroy();
    }
};

class UbReliableReroutingSuite : public TestSuite
{
  public:
    UbReliableReroutingSuite()
        : TestSuite("unified-bus-reliable-rerouting", Type::UNIT)
    {
        AddTestCase(new UbReroutingFailureRepresentationsTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingClosHandshakeTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingWireHandshakeTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingWireHandshakeTest(2), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingWireHandshakeTest(0, true), TestCase::Duration::QUICK);
        for (uint32_t mode = 0; mode < 7; ++mode)
        {
            AddTestCase(new UbReroutingHandshakeTimeoutTest(mode), TestCase::Duration::QUICK);
        }
        AddTestCase(new UbReroutingLastAckCodecTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingScopeTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingSameTimestampOrderTest(true), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingSameTimestampOrderTest(false), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingBidirectionalRecoveryTest(false), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingBidirectionalRecoveryTest(true), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingHealthyBackupTest(false), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingHealthyBackupTest(true), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingHealthyBackupTest(true, true), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingHealthyBackupTest(true, true, true), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingOriginalPacketTest(false, false), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingOriginalPacketTest(true, false), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingOriginalPacketTest(true, true), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingOriginalPacketTest(true, true, true), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingDetectionCreditTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingBacklogOrderTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingOccupancyOutputTest(false, false), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingOccupancyOutputTest(true, false), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingOccupancyOutputTest(false, true), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingOccupancyOutputTest(true, true), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingOccupancyOutputTest(true, false, false), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingNoLegalOutputTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingLastAckGateTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingDisposalTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingVoqGrantCancellationTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingEgressBacklogTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingDelayedProcessingTest(true), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingDelayedProcessingTest(false), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingBlockedAdmissionTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingBlockedAdmissionTest(true), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingMixedVoqTest(false), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingMixedVoqTest(true), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingMixedVoqTest(false, true), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingMixedVoqTest(true, true), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingMixedVoqTest(false, false, true), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingMixedVoqTest(true, false, true), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingMixedVoqTest(false, true, true), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingMixedVoqTest(true, true, true), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingCbfcCreditTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingControlFrameDisposalTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingPaperPendingIngressTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingPfcReleaseTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingProtocolMatrixTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingRoutingRepresentationTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReroutingAttemptIdentityTest(), TestCase::Duration::QUICK);
    }
};

UbReliableReroutingSuite g_reliableReroutingSuite;

} // namespace
