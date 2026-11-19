// SPDX-License-Identifier: GPL-2.0-only
#include "ub-reliable-rerouting.h"

#include "ns3/log.h"
#include "ns3/boolean.h"
#include "ns3/ub-header.h"
#include "ns3/ub-network-address.h"
#include "ns3/simulator.h"
#include "ns3/ub-link.h"
#include "ns3/ub-port.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-utils.h"

#include <algorithm>
#include <string>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbReliableRerouting");
NS_OBJECT_ENSURE_REGISTERED(UbReliableRerouting);
NS_OBJECT_ENSURE_REGISTERED(UbLastAckTag);

Ptr<Packet>
BuildReliableReroutingLastAck(uint32_t sourceNode,
                              uint32_t destinationNode,
                              uint32_t destinationPort,
                              uint16_t receiveNext)
{
    NS_ABORT_MSG_IF(sourceNode > 65535 || destinationNode > 65535 || destinationPort > 65535,
                    "LastAck identity exceeds the supported address mapping");
    const uint8_t body[] = {static_cast<uint8_t>(receiveNext >> 8),
                            static_cast<uint8_t>(receiveNext),
                            static_cast<uint8_t>(destinationPort >> 8),
                            static_cast<uint8_t>(destinationPort)};
    auto packet = Create<Packet>(body, sizeof(body));
    if (sourceNode <= 4095 && destinationNode <= 4095)
    {
        UbCna16NetworkHeader header;
        header.SetScna(utils::NodeIdToCna16(sourceNode));
        header.SetDcna(utils::NodeIdToCna16(destinationNode));
        header.SetServiceLevel(1);
        header.SetNlp(6);
        packet->AddHeader(header);
    }
    else
    {
        UbCna24NetworkHeader header;
        header.SetScna(utils::NodeIdToCna24(sourceNode));
        header.SetDcna(utils::NodeIdToCna24(destinationNode));
        header.SetServiceLevel(1);
        header.SetNlp(6);
        packet->AddHeader(header);
    }
    UbDatalinkPacketHeader header;
    header.SetConfig(5);
    header.SetPacketVL(1);
    packet->AddHeader(header);
    return packet;
}

bool
ParseReliableReroutingLastAck(Ptr<const Packet> packet, UbLastAckMessage& message)
{
    if (packet == nullptr || (packet->GetSize() != 16 && packet->GetSize() != 18))
    {
        return false;
    }
    auto copy = packet->Copy();
    UbDatalinkPacketHeader dl;
    copy->RemoveHeader(dl);
    if (dl.GetConfig() != 5 || dl.GetPacketVL() != 1)
    {
        return false;
    }
    UbLastAckMessage decoded;
    if (packet->GetSize() == 16)
    {
        UbCna16NetworkHeader header;
        copy->RemoveHeader(header);
        if (header.GetNlp() != 6 || header.GetServiceLevel() != 1 ||
            (header.GetScna() & 15) != 0 || (header.GetDcna() & 15) != 0)
        {
            return false;
        }
        decoded.sourceNode = utils::Cna16ToNodeId(header.GetScna());
        decoded.destinationNode = utils::Cna16ToNodeId(header.GetDcna());
    }
    else
    {
        UbCna24NetworkHeader header;
        copy->RemoveHeader(header);
        if (header.GetNlp() != 6 || header.GetServiceLevel() != 1 ||
            (header.GetScna() & 255) != 0 || (header.GetDcna() & 255) != 0)
        {
            return false;
        }
        decoded.sourceNode = utils::Cna24ToNodeId(header.GetScna());
        decoded.destinationNode = utils::Cna24ToNodeId(header.GetDcna());
    }
    uint8_t body[4];
    copy->CopyData(body, sizeof(body));
    decoded.receiveNext = (static_cast<uint16_t>(body[0]) << 8) | body[1];
    decoded.destinationPort = (static_cast<uint16_t>(body[2]) << 8) | body[3];
    message = decoded;
    return true;
}

TypeId
UbReliableRerouting::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::UbReliableRerouting")
            .SetParent<Object>()
            .SetGroupName("UnifiedBus")
            .AddConstructor<UbReliableRerouting>()
            .AddAttribute("TraceOccupancyEvents",
                          "Write optional recovery occupancy event CSV; summary is always available.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&UbReliableRerouting::m_traceOccupancyEvents),
                          MakeBooleanChecker())
            .AddTraceSource("Rerouting",
                            "Reliable rerouting state transitions and retained-work events.",
                            MakeTraceSourceAccessor(&UbReliableRerouting::m_traceRerouting),
                            "ns3::UbReliableRerouting::Rerouting");
    return tid;
}

UbReliableRerouting::UbReliableRerouting() = default;

UbReliableRerouting::~UbReliableRerouting() = default;

const char*
UbReliableRerouting::GetStateName(State state)
{
    switch (state)
    {
    case State::DISABLED:
        return "DISABLED";
    case State::HEALTHY:
        return "HEALTHY";
    case State::DETECTION_PENDING:
        return "DETECTION_PENDING";
    case State::WAIT_LAST_ACK:
        return "WAIT_LAST_ACK";
    case State::RECOVERING:
        return "RECOVERING";
    case State::DRAINED:
        return "DRAINED";
    }
    return "UNKNOWN";
}

void
UbReliableRerouting::Arm(Ptr<UbPort> port, Ptr<UbLink> link, Time detectionDelay, Time lastAckTimeout)
{
    NS_ABORT_MSG_IF(port == nullptr || link == nullptr,
                    "Reliable rerouting requires an attached port and link");
    NS_ABORT_MSG_IF(detectionDelay.IsStrictlyNegative() || lastAckTimeout <= Time(0),
                    "Reliable rerouting delays must be non-negative");
    NS_ABORT_MSG_IF(m_state != State::DISABLED,
                    "Reliable rerouting is armed once, before traffic starts");
    m_port = port;
    m_link = link;
    m_detectionDelay = detectionDelay;
    m_lastAckTimeout = lastAckTimeout;
    const auto peer = link->GetDestination(port);
    m_peerNode = peer->GetNode()->GetId();
    m_peerPort = peer->GetIfIndex();
    NS_ABORT_MSG_IF(m_peerNode > 65535 || port->GetNode()->GetId() > 65535 ||
                        m_peerPort > 65535 || port->GetIfIndex() > 65535,
                    "LastAck endpoint exceeds supported identity range");
    TransitionTo(State::HEALTHY);
}

void
UbReliableRerouting::TransitionTo(State state)
{
    m_state = state;
    NS_LOG_DEBUG("[UbReliableRerouting] node " << (m_port ? m_port->GetNode()->GetId() : 0)
                                               << " port " << (m_port ? m_port->GetIfIndex() : 0)
                                               << " -> " << GetStateName(state));
    Publish(std::string("state:") + GetStateName(state));
    PublishOccupancy("state");
}

uint64_t
UbReliableRerouting::CreateAttempt(Ptr<Packet> packet, uint32_t inPort, uint32_t priority)
{
    if (m_state == State::DISABLED || packet == nullptr)
    {
        return 0;
    }

    NS_ABORT_MSG_IF(IsOutputBlocked(), "Cannot transmit after local failure detection");
    // Delivery on the failed link is invalidated before this original can be recovered.
    Attempt attempt;
    attempt.token = m_nextAttemptToken++;
    attempt.packet = packet;
    attempt.inPort = inPort;
    attempt.priority = priority;
    if (m_state == State::HEALTHY)
    {
        m_inFlight.push_back(attempt);
        AddOccupancy(UbRecoveryStage::IN_FLIGHT, 1, packet->GetSize());
        PublishOccupancy("send");
    }
    else
    {
        m_attempts.push_back(attempt);
        AddOccupancy(UbRecoveryStage::RETRY, 1, packet->GetSize());
        PublishOccupancy("failed_send");
        Publish("attempt_start", attempt.token, m_port->GetIfIndex());
    }
    return attempt.token;
}

void
UbReliableRerouting::ConfirmAttempt(uint64_t token)
{
    if (token == 0 || m_state == State::DISABLED)
    {
        return;
    }
    // Complete receipts on one physical direction are ordered. Failure removes the entire
    // in-flight set, and the link suppresses its old delivery callbacks before they get here.
    if (m_inFlight.empty())
    {
        return;
    }
    NS_ABORT_MSG_IF(m_inFlight.front().token != token || token != m_confirmedNext + 1,
                    "Reliable rerouting requires ordered complete link confirmations");
    m_confirmedNext = token;
    RemoveOccupancy(UbRecoveryStage::IN_FLIGHT, 1, m_inFlight.front().packet->GetSize());
    m_inFlight.pop_front();
    PublishOccupancy("complete_receipt");
}

void
UbReliableRerouting::ReceiveAttempt(uint64_t token)
{
    if (token == 0 || m_state == State::DISABLED)
    {
        return;
    }
    NS_ABORT_MSG_IF(token != m_receiveNext + 1,
                    "Reliable rerouting requires ordered complete link delivery");
    m_receiveNext = token;
}

const UbRecoveryOccupancy&
UbReliableRerouting::GetOccupancy(UbRecoveryStage stage) const
{
    return m_occupancy.at(static_cast<size_t>(stage));
}

const char*
UbReliableRerouting::GetStageName(UbRecoveryStage stage)
{
    switch (stage)
    {
    case UbRecoveryStage::IN_FLIGHT: return "in_flight";
    case UbRecoveryStage::RETRY: return "retry";
    case UbRecoveryStage::FAILED_EGRESS: return "failed_egress";
    case UbRecoveryStage::RECOVERY_VOQ: return "recovery_voq";
    case UbRecoveryStage::COUNT: break;
    }
    NS_ABORT_MSG("Invalid recovery occupancy stage");
    return "invalid";
}

void
UbReliableRerouting::AddOccupancy(UbRecoveryStage stage, uint64_t packets, uint64_t bytes)
{
    NS_ABORT_MSG_IF(!IsArmed(), "Recovery occupancy requires an armed origin");
    auto& count = m_occupancy.at(static_cast<size_t>(stage));
    count.packets += packets;
    count.bytes += bytes;
    count.peakPackets = std::max(count.peakPackets, count.packets);
    count.peakBytes = std::max(count.peakBytes, count.bytes);
}

void
UbReliableRerouting::RemoveOccupancy(UbRecoveryStage stage, uint64_t packets, uint64_t bytes)
{
    auto& count = m_occupancy.at(static_cast<size_t>(stage));
    NS_ABORT_MSG_IF(count.packets < packets || count.bytes < bytes, "Recovery occupancy underflow");
    count.packets -= packets;
    count.bytes -= bytes;
}

void
UbReliableRerouting::EnqueueRecoveryVoq(uint32_t bytes, UbRecoveryStage source)
{
    RemoveOccupancy(source, 1, bytes);
    AddOccupancy(UbRecoveryStage::RECOVERY_VOQ, 1, bytes);
    PublishOccupancy("voq_handoff", false, source);
}

void
UbReliableRerouting::DequeueRecoveryVoq(uint32_t bytes)
{
    RemoveOccupancy(UbRecoveryStage::RECOVERY_VOQ, 1, bytes);
    PublishOccupancy("voq_service");
}

void
UbReliableRerouting::FailedEgressEnqueued(uint32_t bytes)
{
    if (m_state == State::DETECTION_PENDING)
    {
        AddOccupancy(UbRecoveryStage::FAILED_EGRESS, 1, bytes);
        PublishOccupancy("failed_egress_enqueue");
    }
}

void
UbReliableRerouting::FailedEgressDequeued(uint32_t bytes)
{
    if (m_state == State::DETECTION_PENDING)
    {
        RemoveOccupancy(UbRecoveryStage::FAILED_EGRESS, 1, bytes);
    }
}

void
UbReliableRerouting::DiscardFailedEgress(uint32_t bytes)
{
    RemoveOccupancy(UbRecoveryStage::FAILED_EGRESS, 1, bytes);
    PublishOccupancy("failed_egress_discard");
}

void
UbReliableRerouting::WriteOccupancySummary()
{
    if (IsArmed())
    {
        PublishOccupancy("summary", true);
    }
}

void
UbReliableRerouting::PublishOccupancy(const std::string& event, bool summary, UbRecoveryStage source)
{
    if (!IsArmed() || (!summary && !m_traceOccupancyEvents))
    {
        return;
    }
    for (size_t index = 0; index < m_occupancy.size(); ++index)
    {
        UbRecoveryOccupancyEvent record;
        record.nodeId = m_port->GetNode()->GetId();
        record.portId = m_port->GetIfIndex();
        record.stage = GetStageName(static_cast<UbRecoveryStage>(index));
        record.state = GetStateName(m_state);
        record.event = event;
        record.occupancy = m_occupancy[index];
        record.source = source == UbRecoveryStage::COUNT ? "none" : GetStageName(source);
        utils::UbUtils::ReliableReroutingOccupancyNotify(record, summary);
    }
}

void
UbReliableRerouting::OnPhysicalFailure()
{
    if (m_state != State::HEALTHY)
    {
        NS_LOG_DEBUG("[UbReliableRerouting] ignoring repeated physical failure notification");
        return;
    }
    m_attempts.swap(m_inFlight);
    const auto inFlight = GetOccupancy(UbRecoveryStage::IN_FLIGHT);
    RemoveOccupancy(UbRecoveryStage::IN_FLIGHT, inFlight.packets, inFlight.bytes);
    AddOccupancy(UbRecoveryStage::RETRY, inFlight.packets, inFlight.bytes);
    AddOccupancy(UbRecoveryStage::FAILED_EGRESS, m_port->GetUbQueue()->m_egressQ.size(),
                 m_port->GetUbQueue()->GetCurrentBytes());
    TransitionTo(State::DETECTION_PENDING);
    m_detectionEvent = Simulator::Schedule(m_detectionDelay,
                                           &UbReliableRerouting::OnDetection,
                                           this);
}

void
UbReliableRerouting::OnDetection()
{
    if (m_state != State::DETECTION_PENDING)
    {
        return;
    }
    m_detectionTime = Simulator::Now();
    TransitionTo(State::WAIT_LAST_ACK);
    Ptr<UbSwitch> sw = m_port->GetNode()->GetObject<UbSwitch>();
    NS_ABORT_MSG_IF(sw == nullptr, "Reliable rerouting requires a switch on the port node");
    sw->NotifyOutputDetectedFailed(m_port->GetIfIndex());
    m_lastAckEvent = Simulator::Schedule(m_lastAckTimeout,
                                         &UbReliableRerouting::OnLastAckTimeout, this);
    m_lastAckSendEvent = Simulator::Schedule(MilliSeconds(500),
                                             &UbReliableRerouting::OnLastAckSendTimeout, this);
    for (uint8_t copy = 1; copy <= 3; ++copy)
    {
        auto packet = BuildReliableReroutingLastAck(m_port->GetNode()->GetId(), m_peerNode,
                                                    m_peerPort, static_cast<uint16_t>(m_receiveNext));
        UbLastAckTag tag;
        tag.sourceNode = m_port->GetNode()->GetId();
        tag.sourcePort = m_port->GetIfIndex();
        tag.copy = copy;
        packet->AddPacketTag(tag);
        sw->SendLastAck(packet);
    }
}

namespace {

void FillLastAckIdentity(UbReliableReroutingEvent& record, Ptr<const Packet> packet)
{
    UbLastAckMessage message;
    UbLastAckTag tag;
    if (ParseReliableReroutingLastAck(packet, message))
    {
        record.sourceNode = message.sourceNode;
        record.destinationNode = message.destinationNode;
        record.destinationPort = message.destinationPort;
        record.receiveNext = message.receiveNext;
    }
    if (packet != nullptr && packet->PeekPacketTag(tag))
    {
        record.sourcePort = tag.sourcePort;
        record.copy = tag.copy;
    }
}

} // namespace

void
TraceReliableReroutingLastAck(Ptr<const Packet> packet, uint32_t node, uint32_t port,
                              const std::string& event)
{
    UbReliableReroutingEvent record;
    record.nodeId = node;
    record.portId = port;
    record.event = event;
    FillLastAckIdentity(record, packet);
    utils::UbUtils::ReliableReroutingNotify(record);
}

void
UbReliableRerouting::ReceiveLastAck(Ptr<const Packet> packet, const UbLastAckMessage& message)
{
    const bool valid = message.sourceNode == m_peerNode &&
                       message.destinationNode == m_port->GetNode()->GetId() &&
                       message.destinationPort == m_port->GetIfIndex() &&
                       message.receiveNext == static_cast<uint16_t>(m_confirmedNext) &&
                       m_confirmedNext < m_nextAttemptToken &&
                       (m_state != State::WAIT_LAST_ACK || m_attempts.empty() ||
                        m_attempts.front().token == m_confirmedNext + 1);
    if (!valid || (m_state != State::WAIT_LAST_ACK && m_state != State::RECOVERING &&
                   m_state != State::DRAINED))
    {
        Publish("lastack_rejected", 0, UB_REROUTING_NO_OUTPUT, UB_REROUTING_NO_OUTPUT, packet);
        return;
    }
    if (m_state != State::WAIT_LAST_ACK)
    {
        Publish("lastack_duplicate", 0, UB_REROUTING_NO_OUTPUT, UB_REROUTING_NO_OUTPUT, packet);
        return;
    }
    Publish("lastack_valid", 0, UB_REROUTING_NO_OUTPUT, UB_REROUTING_NO_OUTPUT, packet);
    m_lastAckEvent.Cancel();
    OnLastAck();
}

void
UbReliableRerouting::NotifyLastAckSent(uint8_t copy)
{
    NS_ABORT_MSG_IF(copy < 1 || copy > 3, "Invalid LastAck source copy identity");
    m_sentCopies |= 1u << (copy - 1);
    if (m_sentCopies == 7)
    {
        m_lastAckSendEvent.Cancel();
    }
}

void
UbReliableRerouting::OnLastAckTimeout()
{
    NS_ABORT_MSG("ReliableRerouting_TO: node=" << m_port->GetNode()->GetId()
                 << " failed_port=" << m_port->GetIfIndex() << " peer=" << m_peerNode << ':' << m_peerPort
                 << " detection_ps=" << m_detectionTime.GetPicoSeconds()
                 << " timeout_ps=" << m_lastAckTimeout.GetPicoSeconds());
}

void
UbReliableRerouting::OnLastAckSendTimeout()
{
    NS_ABORT_MSG("ReliableRerouting_SEND_TO: node=" << m_port->GetNode()->GetId()
                 << " failed_port=" << m_port->GetIfIndex() << " peer=" << m_peerNode << ':' << m_peerPort
                 << " detection_ps=" << m_detectionTime.GetPicoSeconds()
                 << " timeout_ps=" << MilliSeconds(500).GetPicoSeconds()
                 << " sent_mask=" << +m_sentCopies);
}

void
UbReliableRerouting::OnLastAck()
{
    if (m_state != State::WAIT_LAST_ACK)
    {
        return;
    }
    TransitionTo(State::RECOVERING);
    Ptr<UbSwitch> sw = m_port->GetNode()->GetObject<UbSwitch>();
    NS_ABORT_MSG_IF(sw == nullptr, "Reliable rerouting requires a switch on the port node");
    // All categories share this LastAck gate. Existing target contents stay ahead of the
    // appended retry, failed-egress, then old-VOQ work.
    TransferRetryBacklog();
    sw->StartOutputRecovery(m_port->GetIfIndex());
}

void
UbReliableRerouting::TransferRetryBacklog()
{
    if (m_state != State::RECOVERING)
    {
        return;
    }
    Ptr<UbSwitch> sw = m_port->GetNode()->GetObject<UbSwitch>();
    NS_ABORT_MSG_IF(sw == nullptr, "Reliable rerouting requires a switch on the port node");
    const uint32_t outPort = m_port->GetIfIndex();

    // The shared VOQ accepts recovery storage independently of original ingress capacity.
    // The endpoint releases its reference only after the switch has taken ownership.
    while (!m_attempts.empty())
    {
        Attempt& attempt = m_attempts.front();
        const uint64_t token = attempt.token;
        const uint32_t inPort = attempt.inPort;
        const uint32_t priority = attempt.priority;
        Ptr<Packet> packet = attempt.packet;
        const UbRecoverySubmitResult result =
            sw->SubmitRecoveryPacket(packet, inPort, priority, outPort, token);
        if (result == UbRecoverySubmitResult::DISCARDED)
        {
            // Link-local credit/PFC frames are not rerouted; their disposal is not data loss.
            RemoveOccupancy(UbRecoveryStage::RETRY, 1, packet->GetSize());
            m_attempts.pop_front();
            PublishOccupancy("retry_discard");
            Publish("control_frame_discarded", token, outPort);
            continue;
        }
        m_attempts.pop_front();
        Publish("transfer", token, outPort, sw->GetLastRecoveryOutput());
    }
    UpdateDrainState();
}

void
UbReliableRerouting::NotifyBacklogFinished()
{
    m_backlogFinished = true;
    UpdateDrainState();
}

void
UbReliableRerouting::PublishBacklogEvent(const std::string& event,
                                         uint64_t entryId,
                                         uint32_t oldOutput,
                                         uint32_t newOutput)
{
    Publish(event, entryId, oldOutput, newOutput, nullptr, true);
}

void
UbReliableRerouting::SetPendingRecoveryCounters(uint64_t packets, uint64_t bytes, uint64_t processing)
{
    m_pendingPackets = packets;
    m_pendingBytes = bytes;
    m_processingPackets = processing;
    if (packets != 0 || processing != 0)
    {
        m_backlogFinished = false;
    }
    UpdateDrainState();
}

void
UbReliableRerouting::UpdateDrainState()
{
    if (m_state != State::RECOVERING)
    {
        return;
    }
    // Local drain means: no retained attempt here, no retained packet in the switch's recovery
    // storage, and no egress backlog still owned by the failed output.
    const bool egressBacklog = m_port != nullptr && m_port->GetUbQueue() != nullptr &&
                               !m_port->GetUbQueue()->IsEmpty();
    if (!m_backlogFinished || !m_attempts.empty() || m_pendingPackets != 0 ||
        m_processingPackets != 0 || egressBacklog)
    {
        return;
    }
    m_backlogFinished = true;
    TransitionTo(State::DRAINED);
}

void
UbReliableRerouting::Publish(const std::string& event,
                             uint64_t attemptId,
                             uint32_t oldOutput,
                             uint32_t newOutput,
                             Ptr<const Packet> message,
                             bool switchOwned)
{
    UbReliableReroutingEvent record;
    record.nodeId = m_port != nullptr ? m_port->GetNode()->GetId() : 0;
    record.portId = m_port != nullptr ? m_port->GetIfIndex() : 0;
    record.event = event;
    record.attemptId = attemptId;
    record.owner = attemptId == 0 ? "none" : (switchOwned ? "switch" : "retry");
    record.oldOutput = oldOutput;
    record.newOutput = newOutput;
    record.retryPackets = GetRetryPacketCount();
    record.retryBytes = GetRetryByteCount();
    record.pendingPackets = m_pendingPackets;
    record.pendingBytes = m_pendingBytes;
    record.processingPackets = m_processingPackets;
    record.inFlightPackets = GetInFlightPacketCount();
    record.inFlightBytes = GetInFlightByteCount();
    record.failedEgressPackets = GetOccupancy(UbRecoveryStage::FAILED_EGRESS).packets;
    record.failedEgressBytes = GetOccupancy(UbRecoveryStage::FAILED_EGRESS).bytes;
    record.recoveryVoqPackets = GetRecoveryVoqPacketCount();
    record.recoveryVoqBytes = GetRecoveryVoqByteCount();
    FillLastAckIdentity(record, message);
    m_traceRerouting(record);
}

void
UbReliableRerouting::DoDispose()
{
    m_detectionEvent.Cancel();
    m_lastAckEvent.Cancel();
    m_lastAckSendEvent.Cancel();
    m_inFlight.clear();
    m_attempts.clear();
    m_occupancy = {};
    m_pendingPackets = 0;
    m_pendingBytes = 0;
    m_processingPackets = 0;
    m_link = nullptr;
    m_port = nullptr;
    m_state = State::DISABLED;
    Object::DoDispose();
}

} // namespace ns3
