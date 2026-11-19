// SPDX-License-Identifier: GPL-2.0-only
#include "ub-ctp.h"

#include "ub-ta-codec.h"
#include "ub-transaction-rules.h"
#include "ub-datalink.h"
#include "ub-function.h"
#include "ub-header.h"
#include "ub-routing-process.h"
#include "ub-transaction.h"

#include "ns3/boolean.h"
#include "ns3/global-value.h"
#include "ns3/hash.h"
#include "ns3/log.h"
#include "ns3/node-list.h"
#include "ns3/node.h"
#include "ns3/simulator.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-lifecycle.h"
#include "ns3/ub-modulo-sequence.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-port.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-utils.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>
#include <optional>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("UbCtpTransportService");
NS_OBJECT_ENSURE_REGISTERED(UbCtpTxQueue);
NS_OBJECT_ENSURE_REGISTERED(UbCtpTransportService);

namespace
{

using UbTaSsnSequence = UbModuloSequence<16, uint32_t>;

uint16_t
EntityLoadBalanceSalt(uint32_t srcNodeId, const UbCtpEntityKey& key)
{
    uint8_t buf[17];
    buf[0] = static_cast<uint8_t>((srcNodeId >> 24) & 0xff);
    buf[1] = static_cast<uint8_t>((srcNodeId >> 16) & 0xff);
    buf[2] = static_cast<uint8_t>((srcNodeId >> 8) & 0xff);
    buf[3] = static_cast<uint8_t>(srcNodeId & 0xff);
    buf[4] = static_cast<uint8_t>((key.dstNodeId >> 24) & 0xff);
    buf[5] = static_cast<uint8_t>((key.dstNodeId >> 16) & 0xff);
    buf[6] = static_cast<uint8_t>((key.dstNodeId >> 8) & 0xff);
    buf[7] = static_cast<uint8_t>(key.dstNodeId & 0xff);
    buf[8] = static_cast<uint8_t>((key.srcEntityId >> 24) & 0xff);
    buf[9] = static_cast<uint8_t>((key.srcEntityId >> 16) & 0xff);
    buf[10] = static_cast<uint8_t>((key.srcEntityId >> 8) & 0xff);
    buf[11] = static_cast<uint8_t>(key.srcEntityId & 0xff);
    buf[12] = static_cast<uint8_t>((key.dstEntityId >> 24) & 0xff);
    buf[13] = static_cast<uint8_t>((key.dstEntityId >> 16) & 0xff);
    buf[14] = static_cast<uint8_t>((key.dstEntityId >> 8) & 0xff);
    buf[15] = static_cast<uint8_t>(key.dstEntityId & 0xff);
    buf[16] = key.vl;
    return static_cast<uint16_t>(
        Hash64(std::string(reinterpret_cast<const char*>(buf), sizeof(buf))) & 0xffff);
}

std::string
FormatPortList(const std::vector<uint16_t>& ports)
{
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < ports.size(); ++index)
    {
        if (index != 0)
        {
            out << ",";
        }
        out << ports[index];
    }
    out << "]";
    return out.str();
}

Ptr<Packet>
BuildCtpPacket(Ptr<UbWqeSegment> segment,
               uint32_t srcNodeId,
               const UbCtpEntityKey& key,
               uint32_t srcPortId,
               uint32_t dstPortId,
               uint32_t taSsn,
               uint32_t progressBytes,
               const UbCtpRoutingPolicy& routing,
               uint8_t loadBalanceSalt)
{
    NS_ABORT_MSG_IF(segment == nullptr, "CTP packet requires WQE segment");
    NS_ABORT_MSG_IF(srcPortId != UINT32_MAX && srcPortId > CTP_MAX_CNA16_PORT_ID,
                    "CTP source port cannot be encoded as CNA16: " << srcPortId);
    NS_ABORT_MSG_IF(dstPortId != UINT32_MAX && dstPortId > CTP_MAX_CNA16_PORT_ID,
                    "CTP destination port cannot be encoded as CNA16: " << dstPortId);

    const bool isAckResponse = segment->GetSegmentKind() == UbTransactionSegmentKind::RESPONSE;
    const bool isReadRequest = segment->GetType() == UbTransactionOpcode::READ &&
                               segment->GetSegmentKind() == UbTransactionSegmentKind::REQUEST;
    const uint32_t payloadBytes = isAckResponse
                                      ? segment->GetPayloadBytes()
                                      : (isReadRequest ? 0 : progressBytes);
    const uint32_t logicalBytes =
        segment->GetLogicalBytes() == 0 ? payloadBytes : segment->GetLogicalBytes();
    NS_ABORT_MSG_IF(payloadBytes > UB_MTU_BYTE,
                    "CTP packet payload must not exceed one MTU-sized transaction segment");
    NS_ABORT_MSG_IF(isReadRequest && logicalBytes > UB_MTU_BYTE,
                    "CTP read request segment must not describe more than one MTU response");

    Ptr<Packet> packet;
    if (segment->HasExplicitPayload() && payloadBytes > 0)
    {
        Ptr<const Packet> payload = segment->GetExplicitPayload();
        const uint32_t carrierBytes =
            segment->GetCarrierBytes() == 0 ? segment->GetSize() : segment->GetCarrierBytes();
        const uint32_t bytesLeft = static_cast<uint32_t>(segment->GetBytesLeft());
        NS_ABORT_MSG_IF(bytesLeft > carrierBytes,
                        "CTP explicit payload progress exceeds its wire carrier size");
        NS_ABORT_MSG_IF(payload->GetSize() != carrierBytes,
                        "CTP explicit WQE segment payload must match the segment transfer size");
        packet = payload->CreateFragment(carrierBytes - bytesLeft, payloadBytes);
        packet->AddPacketTag(UbExplicitPayloadTag{});
    }
    else
    {
        packet = Create<Packet>(payloadBytes);
    }
    UbFlowTag flowTag(segment->GetTaskId(), segment->GetWqeSize());
    packet->AddPacketTag(flowTag);

    const auto* rule = FindTransactionRule(isAckResponse ? segment->GetRequestOpcode()
                                                       : segment->GetType());
    NS_ABORT_MSG_IF(rule == nullptr, "CTP packet requires a supported transaction rule");
    NS_ABORT_MSG_IF(!segment->GetCtpRcid(), "CTP packet requires explicit requester wire identity");
    const auto plan = isAckResponse
                          ? UbTaHeaderPlan::ForResponse(*rule)
                          : UbTaHeaderPlan::ForRequest(
                                *rule,
                                DeclareRequestHeaderPresence(*rule, segment->GetTransactionFields()));
    UbFullTaHeader base;
    base.isResponse = isAckResponse;
    base.request.SetUbTransactionOpcode(segment->GetType());
    base.request.SetIniTaSsn(static_cast<uint16_t>(taSsn));
    base.request.SetIniRcId(*segment->GetCtpRcid());
    base.request.SetIniRcType(segment->GetCtpRcType());
    base.response.SetUbTransactionOpcode(segment->GetType());
    base.response.SetIniTaSsn(static_cast<uint16_t>(taSsn));
    base.response.SetIniRcId(*segment->GetCtpRcid());
    base.response.SetIniRcType(segment->GetCtpRcType());
    UbMAExtTah memory;
    memory.SetVirtualAddress(segment->GetRemoteAddress());
    memory.SetTokenId(segment->GetRemoteTokenId());
    memory.SetLength(logicalBytes);
    PrependFullTaHeaders(packet, base, plan, memory, segment->GetTransactionFields());

    UbCompactEidHeader eidHeader;
    eidHeader.SetSourceEid(key.srcEntityId);
    eidHeader.SetDestinationEid(key.dstEntityId);
    packet->AddHeader(eidHeader);

    UbCompactUpiHeader upiHeader;
    upiHeader.SetUpi(0);
    packet->AddHeader(upiHeader);

    UbCtpHeader ctpHeader;
    ctpHeader.SetTPOpcode(CtpOpcode::CTP_DATA);
    const uint32_t nthBytes = packet->GetSize() + ctpHeader.GetSerializedSize() +
                              UbCna16NetworkHeader().GetSerializedSize();
    const uint8_t padding = static_cast<uint8_t>((4 - nthBytes % 4) % 4);
    const uint8_t zeroPadding[3]{};
    packet->AddAtEnd(Create<Packet>(zeroPadding, padding));
    ctpHeader.SetPadding(padding);
    ctpHeader.SetNlp(UB_CTPH_NLP_UPI16_EID40_TAH);
    packet->AddHeader(ctpHeader);

    UbCna16NetworkHeader cnaHeader;
    const uint32_t srcCna = srcPortId == UINT32_MAX
                                ? utils::NodeIdToCna16(srcNodeId)
                                : utils::NodeIdToCna16(srcNodeId, srcPortId);
    cnaHeader.SetScna(static_cast<uint16_t>(srcCna));
    const uint32_t dstCna = dstPortId == UINT32_MAX
                                ? utils::NodeIdToCna16(key.dstNodeId)
                                : utils::NodeIdToCna16(key.dstNodeId, dstPortId);
    cnaHeader.SetDcna(static_cast<uint16_t>(dstCna));
    cnaHeader.SetLb(loadBalanceSalt);
    cnaHeader.SetServiceLevel(key.vl);
    cnaHeader.SetNlp(UB_CNA_NLP_CTPH);
    packet->AddHeader(cnaHeader);

    UbDataLink::AddPacketHeader(packet,
                                false,
                                false,
                                key.vl,
                                key.vl,
                                routing.routingType,
                                UbDatalinkHeaderConfig::PACKET_CNA16);
    return packet;
}

} // namespace

bool
UbCtpCongestionKey::operator<(const UbCtpCongestionKey& other) const
{
    return std::tie(dstNodeId, dstEntityId, vl) <
           std::tie(other.dstNodeId, other.dstEntityId, other.vl);
}

UbCtpEntityState::UbCtpEntityState(UbCtpEntityKey key)
    : m_key(key)
{
}

UbCtpTxState&
UbCtpEntityState::Tx()
{
    return m_tx;
}

const UbCtpTxState&
UbCtpEntityState::Tx() const
{
    return m_tx;
}

UbCtpRxState&
UbCtpEntityState::Rx()
{
    return m_rx;
}

const UbCtpRxState&
UbCtpEntityState::Rx() const
{
    return m_rx;
}

const UbCtpEntityKey&
UbCtpEntityState::GetKey() const
{
    return m_key;
}

void
UbCtpEntityState::AddOutPort(uint32_t outPort)
{
    if (std::find(m_tx.outPorts.begin(), m_tx.outPorts.end(), outPort) == m_tx.outPorts.end())
    {
        m_tx.outPorts.push_back(outPort);
    }
}

const std::vector<uint32_t>&
UbCtpEntityState::GetOutPorts() const
{
    return m_tx.outPorts;
}

TypeId
UbCtpTxQueue::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbCtpTxQueue")
                            .SetParent<UbIngressQueue>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbCtpTxQueue>();
    return tid;
}

UbCtpTxQueue::UbCtpTxQueue() = default;

UbCtpTxQueue::~UbCtpTxQueue() = default;

IngressQueueType
UbCtpTxQueue::GetIngressQueueType()
{
    return IngressQueueType::CTP;
}

bool
UbCtpTxQueue::IsEmpty()
{
    return m_cnpQueue.empty() && m_ackQueue.empty() && m_dataQueue.empty();
}

Ptr<Packet>
UbCtpTxQueue::GetNextPacket()
{
    UbCtpTxQueue::ClassQueue* queue = GetPriorityQueue();
    if (queue == nullptr)
    {
        return nullptr;
    }

    Ptr<Packet> packet = queue->front().packet;
    queue->pop();
    if (!IsEmpty())
    {
        m_headArrivalTime = Simulator::Now();
    }
    RefreshAllocatorSummary();
    return packet;
}

uint32_t
UbCtpTxQueue::GetNextPacketSize()
{
    const UbCtpTxQueue::ClassQueue* queue = GetPriorityQueue();
    return queue == nullptr ? 0 : queue->front().packet->GetSize();
}

void
UbCtpTxQueue::EnqueueData(Ptr<Packet> packet)
{
    EnqueueToClass(UbCtpQueueClass::DATA, packet);
}

void
UbCtpTxQueue::EnqueueAck(Ptr<Packet> packet)
{
    EnqueueToClass(UbCtpQueueClass::ACK, packet);
}

void
UbCtpTxQueue::EnqueueCnp(Ptr<Packet> packet)
{
    EnqueueToClass(UbCtpQueueClass::CNP, packet);
}

void
UbCtpTxQueue::Enqueue(Ptr<Packet> packet)
{
    EnqueueData(packet);
}

Ptr<Packet>
UbCtpTxQueue::PeekNextPacket() const
{
    const UbCtpTxQueue::ClassQueue* queue = GetPriorityQueue();
    return queue == nullptr ? nullptr : queue->front().packet;
}

uint32_t
UbCtpTxQueue::GetQueuedPacketCountForTest() const
{
    return static_cast<uint32_t>(m_dataQueue.size() + m_ackQueue.size() + m_cnpQueue.size());
}

UbCtpTxQueue::ClassQueue&
UbCtpTxQueue::QueueForClass(UbCtpQueueClass queueClass)
{
    switch (queueClass)
    {
    case UbCtpQueueClass::CNP:
        return m_cnpQueue;
    case UbCtpQueueClass::ACK:
        return m_ackQueue;
    case UbCtpQueueClass::DATA:
    default:
        return m_dataQueue;
    }
}

const UbCtpTxQueue::ClassQueue&
UbCtpTxQueue::QueueForClass(UbCtpQueueClass queueClass) const
{
    switch (queueClass)
    {
    case UbCtpQueueClass::CNP:
        return m_cnpQueue;
    case UbCtpQueueClass::ACK:
        return m_ackQueue;
    case UbCtpQueueClass::DATA:
    default:
        return m_dataQueue;
    }
}

UbCtpTxQueue::ClassQueue*
UbCtpTxQueue::GetPriorityQueue()
{
    return const_cast<UbCtpTxQueue::ClassQueue*>(
        static_cast<const UbCtpTxQueue*>(this)->GetPriorityQueue());
}

const UbCtpTxQueue::ClassQueue*
UbCtpTxQueue::GetPriorityQueue() const
{
    if (!m_cnpQueue.empty())
    {
        return &m_cnpQueue;
    }
    if (!m_ackQueue.empty())
    {
        return &m_ackQueue;
    }
    if (!m_dataQueue.empty())
    {
        return &m_dataQueue;
    }
    return nullptr;
}

void
UbCtpTxQueue::EnqueueToClass(UbCtpQueueClass queueClass, Ptr<Packet> packet,
                             std::optional<UbLifecycleResourceIdentity> requester)
{
    NS_ABORT_MSG_IF(packet == nullptr, "Cannot enqueue null CTP packet");
    const bool wasEmpty = IsEmpty();
    if (wasEmpty)
    {
        m_headArrivalTime = Simulator::Now();
    }
    QueueForClass(queueClass).push({packet, requester});
    if (wasEmpty)
    {
        RefreshAllocatorSummary();
    }
}

void
UbCtpTxQueue::EnqueueRequest(Ptr<Packet> packet, const UbLifecycleResourceIdentity& owner)
{
    NS_ABORT_MSG_IF(owner.kind != UbLifecycleResourceKind::JETTY || owner.generation == 0,
                    "CTP source request requires an exact local Jetty owner");
    EnqueueToClass(UbCtpQueueClass::DATA, packet, owner);
}

void
UbCtpTxQueue::RemoveRequesterPackets(const UbLifecycleResourceIdentity& owner)
{
    const auto oldHead = PeekNextPacket();
    // Rotate the existing DATA queue once, preserving FIFO for every surviving entry.
    const auto count = m_dataQueue.size();
    for (size_t i = 0; i < count; ++i)
    {
        auto entry = std::move(m_dataQueue.front());
        m_dataQueue.pop();
        if (entry.requester != owner) m_dataQueue.push(std::move(entry));
    }
    if (!IsEmpty() && PeekNextPacket() != oldHead) m_headArrivalTime = Simulator::Now();
    RefreshAllocatorSummary();
}

bool
UbCtpTxQueue::HasRequesterPackets(const UbLifecycleResourceIdentity& owner) const
{
    auto entries = m_dataQueue;
    while (!entries.empty())
    {
        if (entries.front().requester == owner) return true;
        entries.pop();
    }
    return false;
}

TypeId
UbCtpTransportService::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbCtpTransportService")
            .SetParent<Object>()
            .SetGroupName("UnifiedBus")
            .AddConstructor<UbCtpTransportService>()
            .AddAttribute("BoundOutPortCount",
                          "Maximum number of shortest-path outports that a CTP "
                          "Entity/VL may bind. Zero means no explicit limit.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&UbCtpTransportService::m_boundOutPortCount),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("WindowTraceEnabled",
                          "Enable CTP transaction admission and TAACK window trace.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&UbCtpTransportService::m_windowTraceEnabled),
                          MakeBooleanChecker())
            .AddAttribute("WindowTraceSrcNode",
                          "Trace only this CTP source node. UINT32_MAX traces any source.",
                          UintegerValue(std::numeric_limits<uint32_t>::max()),
                          MakeUintegerAccessor(&UbCtpTransportService::m_windowTraceSrcNode),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute(
                "WindowTraceDstNode",
                "Trace only this CTP destination node. UINT32_MAX traces any destination.",
                UintegerValue(std::numeric_limits<uint32_t>::max()),
                MakeUintegerAccessor(&UbCtpTransportService::m_windowTraceDstNode),
                MakeUintegerChecker<uint32_t>())
            .AddAttribute("DelayTaAckSrcNode",
                          "Experiment hook: delay TAACK completion only for this CTP source node.",
                          UintegerValue(std::numeric_limits<uint32_t>::max()),
                          MakeUintegerAccessor(&UbCtpTransportService::m_delayTaAckSrcNode),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute(
                "DelayTaAckDstNode",
                "Experiment hook: delay TAACK completion only for this CTP destination node.",
                UintegerValue(std::numeric_limits<uint32_t>::max()),
                MakeUintegerAccessor(&UbCtpTransportService::m_delayTaAckDstNode),
                MakeUintegerChecker<uint32_t>())
            .AddAttribute("DelayTaAckSequence",
                          "Experiment hook: delay this TAACK sequence. UINT32_MAX disables it.",
                          UintegerValue(std::numeric_limits<uint32_t>::max()),
                          MakeUintegerAccessor(&UbCtpTransportService::m_delayTaAckSequence),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("DelayTaAckModulo",
                          "Experiment hook: delay TAACKs matching sequence modulo.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&UbCtpTransportService::m_delayTaAckModulo),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("DelayTaAckRemainder",
                          "Experiment hook: modulo remainder for DelayTaAckModulo.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&UbCtpTransportService::m_delayTaAckRemainder),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("DelayTaAckTime",
                          "Experiment hook: delay duration for matching TAACK.",
                          TimeValue(NanoSeconds(0)),
                          MakeTimeAccessor(&UbCtpTransportService::m_delayTaAckTime),
                          MakeTimeChecker())
            .AddTraceSource(
                "FirstPacketSendsNotify",
                "Fires when the first packet of a CTP TA unit is enqueued.",
                MakeTraceSourceAccessor(&UbCtpTransportService::m_traceFirstPacketSendsNotify),
                "ns3::UbCtpTransportService::FirstPacketSendsNotify")
            .AddTraceSource(
                "LastPacketACKsNotify",
                "Fires when the last TAACK for a CTP TA unit is processed.",
                MakeTraceSourceAccessor(&UbCtpTransportService::m_traceLastPacketACKsNotify),
                "ns3::UbCtpTransportService::LastPacketACKsNotify")
            .AddTraceSource("CtpRecvNotify",
                            "Fires when a CTP data or ACK packet is received with path trace.",
                            MakeTraceSourceAccessor(&UbCtpTransportService::m_ctpRecvNotify),
                            "ns3::UbCtpTransportService::CtpRecvNotify");
    return tid;
}

UbCtpTransportService::UbCtpTransportService()
{
    BooleanValue traceEnable(false);
    BooleanValue packetTraceEnable(false);
    const bool traceValueExists =
        GlobalValue::GetValueByNameFailSafe("UB_TRACE_ENABLE", traceEnable);
    const bool packetTraceValueExists =
        GlobalValue::GetValueByNameFailSafe("UB_PACKET_TRACE_ENABLE", packetTraceEnable);
    m_pktTraceEnabled =
        traceValueExists && packetTraceValueExists && traceEnable.Get() && packetTraceEnable.Get();
}

UbCtpTransportService::~UbCtpTransportService() = default;

void
UbCtpTransportService::SetNode(Ptr<Node> node)
{
    NS_ABORT_MSG_IF(node == nullptr, "CTP service requires an owning node");
    NS_ABORT_MSG_IF(m_node != nullptr && m_node != node,
                    "CTP service cannot move between owning nodes");
    m_node = node;
    Ptr<UbLifecycleCoordinator> coordinator = UbLifecycleCoordinator::GetOrCreate(node);
    m_lifecycleOwner = coordinator->OpenResource(UbLifecycleResourceKind::CTP_SERVICE, 0);
    m_delayedTaAckEvents.Init(coordinator);
}

Ptr<Node>
UbCtpTransportService::GetNode() const
{
    return m_node;
}

void
UbCtpTransportService::SetRoutingType(RoutingType routingType)
{
    m_defaultRoutingPolicy.routingType = routingType;
}

void
UbCtpTransportService::ValidateRoutingType(RoutingType routingType, const std::string& owner) const
{
    if (m_node == nullptr)
    {
        return;
    }
    Ptr<UbSwitch> sw = m_node->GetObject<UbSwitch>();
    if (sw == nullptr || sw->GetRoutingProcess() == nullptr)
    {
        return;
    }
    sw->GetRoutingProcess()->ValidateRoutingType(routingType, owner);
}

void
UbCtpTransportService::SetBoundOutPortCountForTest(uint32_t boundOutPortCount)
{
    m_boundOutPortCount = boundOutPortCount;
}

UbCtpEntityState&
UbCtpTransportService::GetOrCreateEntityState(const UbCtpEntityKey& key)
{
    auto [it, inserted] = m_entityStates.try_emplace(key, key);
    if (inserted)
    {
        it->second.Tx().routing = m_defaultRoutingPolicy;
    }
    return it->second;
}

Ptr<UbTransaction>
UbCtpTransportService::GetTransactionOwner() const
{
    Ptr<UbController> controller = m_node == nullptr ? nullptr : m_node->GetObject<UbController>();
    Ptr<UbTransaction> transaction =
        controller == nullptr ? nullptr : controller->GetUbTransaction();
    NS_ABORT_MSG_IF(transaction == nullptr, "CTP transaction state requires the Transaction owner");
    return transaction;
}

void
UbCtpTransportService::ConfigureRoutingPolicy(const UbCtpEntityKey& key, RoutingType routingType)
{
    ValidateRoutingType(routingType, "CTP entity routing policy");
    UbCtpEntityState& state = GetOrCreateEntityState(key);
    state.Tx().routing.routingType = routingType;
}

uint32_t
UbCtpTransportService::GetEntityStateCount() const
{
    return static_cast<uint32_t>(m_entityStates.size());
}

Ptr<UbCtpTxQueue>
UbCtpTransportService::GetOrCreateQueue(const UbCtpEntityKey& key,
                                        uint32_t outPort,
                                        uint32_t priority)
{
    QueueKey queueKey = std::make_tuple(key, outPort, priority);
    auto it = m_queues.find(queueKey);
    if (it != m_queues.end())
    {
        return it->second;
    }

    Ptr<UbCtpTxQueue> queue = CreateObject<UbCtpTxQueue>();
    UbCtpEntityState& state = GetOrCreateEntityState(key);
    queue->SetOutPortId(outPort);
    queue->SetInPortId(outPort);
    queue->SetIngressPriority(priority);
    m_queues.emplace(queueKey, queue);
    state.AddOutPort(outPort);
    return queue;
}

Ptr<Packet>
UbCtpTransportService::BuildDataPacket(Ptr<UbWqeSegment> segment, const UbCtpEntityKey& key)
{
    NS_ABORT_MSG_IF(segment == nullptr, "CTP packet requires WQE segment");

    const uint32_t taSsn = segment->GetSegmentKind() == UbTransactionSegmentKind::RESPONSE
                               ? segment->GetRequestTassn()
                               : segment->GetTaSsn();
    const uint32_t progressBytes =
        segment->GetSegmentKind() == UbTransactionSegmentKind::RESPONSE &&
                segment->GetType() == UbTransactionOpcode::TAACK
            ? 0
            : static_cast<uint32_t>(segment->GetBytesLeft());
    const UbCtpEntityKey packetKey = MakeTxKeyForSegment(key, segment);
    UbCtpEntityState& state = GetOrCreateEntityState(packetKey);
    const uint16_t lbSalt = NextLoadBalanceSalt(state, packetKey);
    return BuildDataPacketWithTaSsn(segment,
                                    state,
                                    packetKey,
                                    taSsn,
                                    progressBytes,
                                    static_cast<uint8_t>(lbSalt & 0xff));
}

Ptr<Packet>
UbCtpTransportService::BuildDataPacketWithTaSsn(Ptr<UbWqeSegment> segment,
                                                UbCtpEntityState& state,
                                                const UbCtpEntityKey& packetKey,
                                                uint32_t taSsn,
                                                uint32_t progressBytes,
                                                uint8_t loadBalanceSalt)
{
    Ptr<Packet> packet = BuildCtpPacket(segment,
                                        m_node->GetId(),
                                        packetKey,
                                        segment->GetSport() == CTP_WILDCARD_PORT
                                            ? UINT32_MAX
                                            : segment->GetSport(),
                                        segment->GetDport() == CTP_WILDCARD_PORT
                                            ? UINT32_MAX
                                            : segment->GetDport(),
                                        taSsn,
                                        progressBytes,
                                        state.Tx().routing,
                                        loadBalanceSalt);
    return packet;
}

Ptr<Packet>
UbCtpTransportService::BuildResponsePacketForTest(Ptr<UbWqeSegment> response,
                                                  const UbCtpEntityKey& key)
{
    return BuildDataPacket(response, key);
}

void
UbCtpTransportService::AddCtpNetworkHeaders(Ptr<Packet> packet,
                                            const UbCtpEntityKey& key,
                                            CtpOpcode opcode,
                                            uint8_t loadBalanceSalt)
{
    AddCtpNetworkHeaders(packet, key, opcode, loadBalanceSalt, m_defaultRoutingPolicy.routingType);
}

void
UbCtpTransportService::AddCtpNetworkHeaders(Ptr<Packet> packet,
                                            const UbCtpEntityKey& key,
                                            CtpOpcode opcode,
                                            uint8_t loadBalanceSalt,
                                            RoutingType routingType)
{
    UbCtpRoutingPolicy routing{routingType, 0};
    Ptr<UbWqeSegment> empty = CreateObject<UbWqeSegment>();
    empty->SetSegmentKind(UbTransactionSegmentKind::REQUEST);
    empty->SetType(UbTransactionOpcode::WRITE);
    empty->SetPayloadBytes(packet->GetSize());
    empty->SetCarrierBytes(packet->GetSize());
    Ptr<Packet> headers = BuildCtpPacket(empty,
                                         m_node->GetId(),
                                         key,
                                         UINT32_MAX,
                                         UINT32_MAX,
                                         0,
                                         packet->GetSize(),
                                         routing,
                                         loadBalanceSalt);
    (void)headers;
    NS_ABORT_MSG_IF(opcode != CtpOpcode::CTP_DATA,
                    "manual CTP header helper only supports CTP_DATA in the entity path");
}

bool
UbCtpTransportService::SendSegment(Ptr<UbWqeSegment> segment, const UbCtpEntityKey& key)
{
    if (segment == nullptr || segment->IsSentCompleted() ||
        segment->GetBytesLeft() > GetTransactionConstraints().maxSegmentBytes ||
        !segment->GetCtpRcid() || !segment->HasTransactionCorrelation()) return false;
    UbCtpEntityKey ownerKey;
    std::string reason;
    auto current = GetTransactionOwner()->FindCtpOutstanding(
        *segment->GetCtpRcid(), static_cast<uint16_t>(segment->GetTaSsn()), ownerKey, reason);
    if (current != segment || !(ownerKey == key)) return false;
    auto& state = GetOrCreateEntityState(key);
    return SendAdmittedFragment(segment, state, MakeTxKeyForSegment(key, segment),
                                 segment->GetTaSsn(), segment->GetBytesLeft());
}

UbCtpTransportConstraints
UbCtpTransportService::GetTransactionConstraints() const
{
    return {};
}

bool
UbCtpTransportService::PrepareJetty(Ptr<UbJetty> jetty, const UbCtpEntityKey& key)
{
    return PrepareJettyWithResult(jetty, key).IsSuccess();
}

UbCtpPreparationResult
UbCtpTransportService::PrepareJettyWithResult(Ptr<UbJetty> jetty, const UbCtpEntityKey& key)
{
    if (jetty == nullptr || m_node == nullptr || jetty->GetSrc() != m_node->GetId())
    {
        return {UbCtpPreparationStatus::BINDING_MISMATCH, {}, "CTP Jetty/source mismatch"};
    }
    if ((jetty->HasBoundDest() && jetty->GetDest() != key.dstNodeId) ||
        (!jetty->HasBoundDest() && !(MakeSourceGroupKeyForRequest(key) == key)))
    {
        return {UbCtpPreparationStatus::BINDING_MISMATCH, {}, "CTP destination binding mismatch"};
    }
    auto controller = m_node->GetObject<UbController>();
    auto transaction = controller == nullptr ? nullptr : controller->GetUbTransaction();
    auto function = controller == nullptr ? nullptr : controller->GetUbFunction();
    if (transaction == nullptr || function == nullptr ||
        function->GetJetty(jetty->GetJettyNum()) != jetty ||
        !function->IsJettyAcceptingWqes(jetty->GetJettyNum()))
    {
        return {UbCtpPreparationStatus::JETTY_NOT_ACCEPTING, {}, "CTP Jetty is not active"};
    }
    if (transaction->HasJettyTpBinding(jetty->GetJettyNum()))
    {
        return {UbCtpPreparationStatus::BINDING_MISMATCH, {}, "Jetty already has an RTP binding"};
    }
    Ptr<UbJetty> preparedJetty;
    UbCtpEntityKey preparedKey;
    const bool prepared = TryGetJettyPreparation(jetty->GetJettyNum(), preparedJetty, preparedKey);
    if (prepared && (preparedJetty != jetty || !(preparedKey == key)))
    {
        return {UbCtpPreparationStatus::BINDING_MISMATCH, {}, "CTP preparation key mismatch"};
    }
    // All recoverable checks precede RCID allocation; publication below invokes no callbacks.
    auto result = transaction->PrepareCtpRequester(jetty);
    if (!result.IsSuccess() || prepared)
    {
        return result;
    }
    function->RegisterJettyCtpBinding(jetty->GetJettyNum(), Ptr<UbCtpTransportService>(this));
    m_keyJettys[key].push_back(jetty);
    UbCtpEntityState& state = GetOrCreateEntityState(key);
    state.Tx().routing.routingType = m_defaultRoutingPolicy.routingType;
    return result;
}

bool
UbCtpTransportService::HasJettyPreparation(uint32_t jettyNum) const
{
    Ptr<UbJetty> jetty;
    UbCtpEntityKey key;
    return TryGetJettyPreparation(jettyNum, jetty, key);
}

bool
UbCtpTransportService::CanSubmitWqe(uint32_t jettyNum, Ptr<UbWqe> wqe) const
{
    Ptr<UbJetty> jetty;
    UbCtpEntityKey key;
    if (!TryGetJettyPreparation(jettyNum, jetty, key) || jetty == nullptr || wqe == nullptr ||
        !jetty->CanAcceptWqe(wqe))
    {
        return false;
    }

    if (m_node == nullptr || !wqe->HasSrcEntityId() || !wqe->HasDstEntityId() ||
        wqe->GetSrc() != m_node->GetId() || wqe->GetSrcEntityId() != key.srcEntityId ||
        static_cast<uint32_t>(wqe->GetPriority()) != key.vl)
    {
        return false;
    }
    if (jetty->HasBoundDest())
    {
        return wqe->GetDest() == key.dstNodeId && wqe->GetDstEntityId() == key.dstEntityId;
    }
    return MakeSourceGroupKeyForRequest(key) == key;
}

bool
UbCtpTransportService::CanSubmitWqe(uint32_t jettyNum,
                                    Ptr<UbWqe> wqe,
                                    const UbCtpEntityKey& key) const
{
    Ptr<UbJetty> jetty;
    UbCtpEntityKey preparedKey;
    return TryGetJettyPreparation(jettyNum, jetty, preparedKey) && preparedKey == key &&
           GetTransactionOwner()->GetCtpRequesterRcid(jettyNum).has_value() &&
           CanSubmitWqe(jettyNum, wqe);
}

bool
UbCtpTransportService::SubmitPreparedWqe(uint32_t jettyNum, Ptr<UbWqe> wqe)
{
    Ptr<UbJetty> jetty;
    UbCtpEntityKey key;
    if (!CanSubmitWqe(jettyNum, wqe) || !TryGetJettyPreparation(jettyNum, jetty, key))
    {
        return false;
    }

    Ptr<UbController> controller = m_node == nullptr ? nullptr : m_node->GetObject<UbController>();
    Ptr<UbFunction> function = controller == nullptr ? nullptr : controller->GetUbFunction();
    if (function == nullptr ||
        function->SubmitCtpWqe(wqe, jettyNum, key).status != UbWqeSubmissionStatus::ACCEPTED)
    {
        return false;
    }
    return true;
}

bool
UbCtpTransportService::NotifyJettyWork(uint32_t jettyNum)
{
    Ptr<UbJetty> jetty;
    UbCtpEntityKey key;
    if (!TryGetJettyPreparation(jettyNum, jetty, key))
    {
        return false;
    }
    TryContinueJettyTransmission(key);
    return true;
}

bool
UbCtpTransportService::UnprepareJetty(uint32_t jettyNum)
{
    Ptr<UbJetty> jetty;
    UbCtpEntityKey key;
    if (!TryGetJettyPreparation(jettyNum, jetty, key) || jetty == nullptr || !jetty->IsIdle())
    {
        return false;
    }

    Ptr<UbController> controller = m_node == nullptr ? nullptr : m_node->GetObject<UbController>();
    Ptr<UbFunction> function = controller == nullptr ? nullptr : controller->GetUbFunction();
    if (function == nullptr || !function->IsJettyDrainReady(jettyNum) ||
        !function->UnregisterJettyCtpBinding(jettyNum, Ptr<UbCtpTransportService>(this)))
    {
        return false;
    }

    auto jettysIt = m_keyJettys.find(key);
    NS_ABORT_MSG_IF(jettysIt == m_keyJettys.end(),
                    "Prepared CTP Jetty lost its canonical key binding");
    auto& jettys = jettysIt->second;
    jettys.erase(std::remove(jettys.begin(), jettys.end(), jetty), jettys.end());
    if (jettys.empty())
    {
        m_keyJettys.erase(jettysIt);
        m_keyJettyRrIndex.erase(key);
    }
    return true;
}

void
UbCtpTransportService::StartJetty(Ptr<UbJetty> jetty, const UbCtpEntityKey& key)
{
    Ptr<UbJetty> preparedJetty;
    UbCtpEntityKey preparedKey;
    if (jetty != nullptr &&
        TryGetJettyPreparation(jetty->GetJettyNum(), preparedJetty, preparedKey) &&
        preparedJetty == jetty && preparedKey == key)
    {
        NotifyJettyWork(jetty->GetJettyNum());
    }
}

void
UbCtpTransportService::ReleaseJetty(uint32_t jettyNum)
{
    for (auto it = m_keyJettys.begin(); it != m_keyJettys.end();)
    {
        auto& jettys = it->second;
        jettys.erase(std::remove_if(jettys.begin(),
                                    jettys.end(),
                                    [jettyNum](const Ptr<UbJetty>& jetty) {
                                        return jetty == nullptr || jetty->GetJettyNum() == jettyNum;
                                    }),
                     jettys.end());
        if (jettys.empty())
        {
            m_keyJettyRrIndex.erase(it->first);
            it = m_keyJettys.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool
UbCtpTransportService::HasJettyBindingForTest(uint32_t jettyNum) const
{
    return HasJettyPreparation(jettyNum);
}

bool
UbCtpTransportService::TryGetJettyPreparation(uint32_t jettyNum,
                                              Ptr<UbJetty>& jetty,
                                              UbCtpEntityKey& key) const
{
    for (const auto& [candidateKey, jettys] : m_keyJettys)
    {
        auto it =
            std::find_if(jettys.begin(), jettys.end(), [jettyNum](const Ptr<UbJetty>& candidate) {
                return candidate != nullptr && candidate->GetJettyNum() == jettyNum;
            });
        if (it != jettys.end())
        {
            jetty = *it;
            key = candidateKey;
            return true;
        }
    }
    return false;
}

UbCtpEntityKey
UbCtpTransportService::MakeSourceGroupKeyForRequest(const UbCtpEntityKey& key) const
{
    UbCtpEntityKey sourceKey = key;
    sourceKey.dstNodeId = 0;
    sourceKey.dstEntityId = 0;
    return sourceKey;
}

UbCtpEntityKey
UbCtpTransportService::MakeTxKeyForSegment(const UbCtpEntityKey& jettyKey,
                                           Ptr<UbWqeSegment> segment) const
{
    NS_ABORT_MSG_IF(segment == nullptr, "CTP segment key requires segment");
    UbCtpEntityKey txKey = jettyKey;
    txKey.dstNodeId = segment->GetDest();
    if (segment->HasSrcEntityId())
    {
        txKey.srcEntityId = segment->GetSrcEntityId();
    }
    if (segment->HasDstEntityId())
    {
        txKey.dstEntityId = segment->GetDstEntityId();
    }
    txKey.vl = static_cast<uint8_t>(segment->GetPriority());
    return txKey;
}

bool
UbCtpTransportService::SendAdmittedFragment(Ptr<UbWqeSegment> segment,
                                            UbCtpEntityState& state,
                                            const UbCtpEntityKey& packetKey,
                                            uint32_t taSsn,
                                            uint32_t progressBytes)
{
    const uint16_t lbSalt = NextLoadBalanceSalt(state, packetKey);
    const uint32_t outPort = SelectOutPort(segment, state, packetKey, lbSalt);
    Ptr<Packet> packet = BuildDataPacketWithTaSsn(segment,
                                                  state,
                                                  packetKey,
                                                  taSsn,
                                                  progressBytes,
                                                  static_cast<uint8_t>(lbSalt & 0xff));
    Ptr<UbCtpTxQueue> queue = GetOrCreateQueue(packetKey, outPort, packetKey.vl);
    RegisterQueueIfNeeded(queue, outPort, packetKey.vl);
    Ptr<UbPort> port = DynamicCast<UbPort>(m_node->GetDevice(outPort));
    NS_ABORT_MSG_IF(port == nullptr, "CTP send selected a non-UB source port");
    const auto* owner = GetTransactionOwner()->FindCtpRequester(*segment->GetCtpRcid());
    NS_ABORT_MSG_IF(owner == nullptr, "Admitted CTP source packet lost its requester owner");
    queue->EnqueueRequest(packet, *owner);
    segment->UpdateSentBytes(progressBytes);
    TraceWindowEvent(packetKey, "data-enqueued", taSsn,
                     *GetTransactionOwner()->GetJetty(segment->GetJettyNum()), outPort);
    FirstPacketSendsNotify(segment->GetTaskId(), packetKey, taSsn, outPort,
                           progressBytes, segment->GetType());

    port->TriggerTransmit();
    return true;
}

bool
UbCtpTransportService::SendResponseSegmentForTest(Ptr<UbWqeSegment> response,
                                                  const UbCtpEntityKey& key)
{
    UbCtpEntityState& state = GetOrCreateEntityState(key);
    return SendResponseSegment(response, state);
}

void
UbCtpTransportService::TryContinueJettyTransmission(const UbCtpEntityKey& entityKey)
{
    Ptr<UbCtpTransportService> keepAlive(this);
    const UbCtpEntityKey key = entityKey;
    if (m_node == nullptr || m_keyJettys.find(key) == m_keyJettys.end()) return;
    // Revisit each requester after one success; a full pass without a candidate waits for
    // dispatch/completion wakeups. Reacquire bindings after sending, since trace callbacks may
    // retire a requester and erase its scheduling group.
    auto transaction = GetTransactionOwner();
    while (true)
    {
        auto jettysIt = m_keyJettys.find(key);
        if (jettysIt == m_keyJettys.end() || jettysIt->second.empty()) return;
        auto& jettys = jettysIt->second;
        uint32_t& rrIndex = m_keyJettyRrIndex[key];
        Ptr<UbWqeSegment> segment;
        const uint32_t count = jettys.size();
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t idx = (rrIndex + i) % count;
            if (jettys[idx] == nullptr ||
                !transaction->GetFunction()->ShouldContinueAcceptedWqes(jettys[idx]->GetJettyNum()))
                continue;
            segment = transaction->PrepareNextCtpWqeSegment(
                jettys[idx], GetTransactionConstraints().maxSegmentBytes);
            if (segment != nullptr && transaction->IsCtpWqeDispatchReady(
                                          jettys[idx]->GetJettyNum(), segment->GetTaskId()))
            {
                rrIndex = (idx + 1) % count;
                break;
            }
            segment = nullptr;
        }
        if (segment == nullptr) return;
        transaction->CommitCtpWqeSegment(segment);
        TraceWindowEvent(MakeTxKeyForSegment(key, segment), "admit", segment->GetTaSsn(),
                         *transaction->GetJetty(segment->GetJettyNum()));
        NS_ABORT_MSG_IF(!SendSegment(segment, key), "Admitted CTP segment lost its exact owner");
    }
}

bool
UbCtpTransportService::HandleReceivedPacket(Ptr<Packet> packet)
{
    return HandleReceivedPacket(packet, std::numeric_limits<uint32_t>::max());
}

bool
UbCtpTransportService::HandleReceivedPacket(Ptr<Packet> packet, uint32_t ingressPortId)
{
    if (packet == nullptr)
    {
        return false;
    }

    const auto reject = [](const std::string& reason) {
        std::cerr << "Rejected CTP packet: " << reason << std::endl;
        return false;
    };
    Ptr<Packet> copy = CopyPacketView(packet);
    const auto remove = [&copy](Header& header) {
        return copy->GetSize() >= header.GetSerializedSize() && copy->RemoveHeader(header) != 0;
    };

    UbDatalinkPacketHeader linkHeader;
    if (!remove(linkHeader) ||
        linkHeader.GetConfig() != static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_CNA16))
    {
        return false;
    }

    const uint32_t nthBytes = copy->GetSize();
    UbCna16NetworkHeader cnaHeader;
    if (!remove(cnaHeader) || cnaHeader.GetNlp() != UB_CNA_NLP_CTPH)
    {
        return false;
    }

    UbCtpHeader ctpHeader;
    if (!remove(ctpHeader))
    {
        return false;
    }

    uint32_t srcEntityId = 0;
    uint32_t dstEntityId = 0;
    if (ctpHeader.GetNlp() == UB_CTPH_NLP_UPI16_EID40_TAH)
    {
        UbCompactUpiHeader upiHeader;
        UbCompactEidHeader eidHeader;
        if (!remove(upiHeader) || !remove(eidHeader))
        {
            return false;
        }
        srcEntityId = eidHeader.GetSourceEid();
        dstEntityId = eidHeader.GetDestinationEid();
    }
    else
    {
        return false;
    }

    const uint32_t wireSrcNodeId = utils::Cna16ToNodeId(cnaHeader.GetScna());
    const uint32_t wireDstNodeId = utils::Cna16ToNodeId(cnaHeader.GetDcna());
    Ptr<UbController> controller = m_node == nullptr ? nullptr : m_node->GetObject<UbController>();
    NS_ABORT_MSG_IF(controller == nullptr || !controller->AreCtpEntitiesFrozen(),
                    "CTP receive requires a frozen local Entity registry");
    NS_ABORT_MSG_IF(wireDstNodeId != m_node->GetId(),
                    "CTP packet destination node does not match the receiving host");
    NS_ABORT_MSG_IF(!controller->HasCtpEntity(dstEntityId),
                    "CTP destination Entity " << dstEntityId
                                                << " is not configured on node "
                                                << m_node->GetId());

    const bool destinationPortExplicit = (cnaHeader.GetDcna() & 0xF) != 0;
    uint32_t receivePort = ingressPortId;
    if (receivePort == std::numeric_limits<uint32_t>::max() && destinationPortExplicit)
    {
        receivePort = utils::Cna16ToPortId(cnaHeader.GetDcna());
    }
    NS_ABORT_MSG_IF(receivePort == std::numeric_limits<uint32_t>::max(),
                    "CTP receive requires the actual ingress port for a Primary CNA");
    NS_ABORT_MSG_IF(receivePort > CTP_MAX_CNA16_PORT_ID,
                    "CTP ingress port cannot be encoded as CNA16: " << receivePort);

    Ptr<UbPort> ingressPort = nullptr;
    for (uint32_t deviceIndex = 0; deviceIndex < m_node->GetNDevices(); ++deviceIndex)
    {
        Ptr<UbPort> candidate = DynamicCast<UbPort>(m_node->GetDevice(deviceIndex));
        if (candidate != nullptr && candidate->GetIfIndex() == receivePort)
        {
            ingressPort = candidate;
            break;
        }
    }
    NS_ABORT_MSG_IF(ingressPort == nullptr,
                    "CTP ingress port is not a UB port on node " << m_node->GetId() << ": "
                                                                  << receivePort);
    NS_ABORT_MSG_IF(!controller->IsCtpEntityPortMember(dstEntityId, receivePort),
                    "CTP ingress port " << receivePort << " is not a member of Entity "
                                          << dstEntityId);
    if (destinationPortExplicit)
    {
        NS_ABORT_MSG_IF(utils::Cna16ToPortId(cnaHeader.GetDcna()) != receivePort,
                        "Explicit CTP destination CNA does not match the ingress port");
    }

    UbCtpEntityKey localKey{dstEntityId,
                            wireSrcNodeId,
                            srcEntityId,
                            cnaHeader.GetServiceLevel()};
    if (ctpHeader.GetTPOpcode() == static_cast<uint8_t>(CtpOpcode::CTP_CNP))
    {
        ++GetOrCreateEntityState(localKey).Rx().receivedPackets;
        RecordCnpForTest(localKey);
        return true;
    }
    if (ctpHeader.GetTPOpcode() != static_cast<uint8_t>(CtpOpcode::CTP_DATA))
        return false;
    const uint32_t padding = ctpHeader.GetPadding();
    if (nthBytes % 4 != 0 || padding > 3 || copy->GetSize() < padding)
        return reject("invalid carrier padding length");
    if (padding != 0)
    {
        uint8_t tail[3]{};
        copy->CreateFragment(copy->GetSize() - padding, padding)->CopyData(tail, padding);
        if (std::any_of(tail, tail + padding, [](uint8_t byte) { return byte != 0; }))
            return reject("nonzero carrier padding");
        copy->RemoveAtEnd(padding);
    }
    UbFullTaHeader base;
    if (!DecodeFullTaBase(copy, base).IsSuccess())
        return reject("truncated full TA base");
    if (base.isResponse)
    {
        const auto& ack = base.response;
        if (ack.GetIniRcType() > 1)
            return reject("unsupported response RC type");
        std::string reason;
        UbCtpEntityKey ownerKey;
        auto request = GetTransactionOwner()->FindCtpOutstanding(
            ack.GetIniRcId(), ack.GetIniTaSsn(), ownerKey, reason);
        if (request == nullptr) return reject(reason);
        if (ack.GetIniRcType() != request->GetCtpRcType() || ack.GetTaVersion() != 0 ||
            ack.GetPoison() || ack.GetRspStatus() != 0 || ack.GetRspInfo() != 0 || ack.GetSV() != 0)
            return reject("unsupported response profile or mismatched RC type");
        if (wireSrcNodeId != request->GetDest() || wireDstNodeId != request->GetSrc() ||
            srcEntityId != request->GetDstEntityId() || dstEntityId != request->GetSrcEntityId() ||
            localKey.vl != request->GetPriority())
            return reject("response peer, Entity or VL mismatch");
        const bool sourcePortExplicit = (cnaHeader.GetScna() & 0xf) != 0;
        if ((request->GetSport() != CTP_WILDCARD_PORT &&
             (!destinationPortExplicit ||
              utils::Cna16ToPortId(cnaHeader.GetDcna()) != request->GetSport())) ||
            (request->GetDport() != CTP_WILDCARD_PORT &&
             (!sourcePortExplicit ||
              utils::Cna16ToPortId(cnaHeader.GetScna()) != request->GetDport())))
            return reject("response endpoint mismatch");
        const auto* rule = FindTransactionRule(request->GetType());
        NS_ABORT_MSG_IF(rule == nullptr, "outstanding CTP request has no rule");
        const auto expected = request->HasTransactionResolved()
                                  ? request->GetTransactionResolved().responseOpcode
                                  : rule->responseOpcode;
        if (ack.GetUbTransactionOpcode() != static_cast<uint8_t>(expected))
            return reject("response opcode mismatch");
        UbTransactionFields fields;
        UbMAExtTah memory;
        if (!DecodeFullTaExtensions(copy, UbTaHeaderPlan::ForResponse(*rule), memory, fields)
                 .IsSuccess())
            return reject("malformed response extension layout");
        const uint32_t expectedBytes = request->HasTransactionResolved()
                                         ? request->GetTransactionResolved().responsePayloadBytes
                                         : request->GetResponseBytes();
        if (!IsAcceptedResponsePayloadBytes(request->GetType(), expectedBytes, copy->GetSize()) ||
            (request->GetType() == UbTransactionOpcode::ATOMIC_SYNC &&
             fields.taskId != request->GetTransactionFields().taskId))
            return reject("response payload or TAID mismatch");
        auto response = CreateObject<UbWqeSegment>();
        response->SetSrc(wireSrcNodeId);
        response->SetDest(wireDstNodeId);
        response->SetPriority(localKey.vl);
        response->SetTaskId(request->GetTaskId());
        response->SetWqeSize(request->GetWqeSize());
        response->SetJettyNum(request->GetJettyNum());
        response->SetOriginJettyNum(request->GetOriginJettyNum());
        response->SetTaSsn(request->GetTaSsn());
        response->SetRequestTassn(request->GetTaSsn());
        response->SetSegmentKind(UbTransactionSegmentKind::RESPONSE);
        response->SetType(expected);
        response->SetRequestOpcode(request->GetType());
        response->SetCtpRequesterIdentity(ack.GetIniRcId(), ack.GetIniRcType());
        response->SetTransactionFields(fields);
        response->SetTransactionCorrelation(request->GetTransactionCorrelation());
        response->SetPayloadBytes(copy->GetSize());
        response->SetLogicalBytes(copy->GetSize());
        response->SetResponseBytes(copy->GetSize());
        response->SetSize(std::max(1u, copy->GetSize()));
        response->SetCarrierBytes(std::max(1u, copy->GetSize()));
        response->SetNeedsTransactionResponse(false);
        UbPacketTraceTag traceTag;
        packet->PeekPacketTag(traceTag);
        CtpRecvNotify(packet->GetUid(), localKey, request->GetTaSsn(), PacketType::ACK,
                      copy->GetSize(), request->GetTaskId(), traceTag);
        ++GetOrCreateEntityState(localKey).Rx().receivedPackets;
        CompleteFromTaAck(ownerKey, request->GetTaSsn(), response);
        return true;
    }
    const auto& taHeader = base.request;
    if (taHeader.GetIniRcType() > 1 || taHeader.GetTaVersion() != 0 || taHeader.GetPoison() ||
        taHeader.GetNoTaAck() || taHeader.HasUnsupportedCtpControl())
        return reject("unsupported request profile");
    const auto taOpcode = static_cast<UbTransactionOpcode>(taHeader.GetUbTransactionOpcode());
    const auto* rule = FindTransactionRule(taOpcode);
    if (rule == nullptr || rule->role != UbTransactionSegmentKind::REQUEST)
        return reject("unsupported request opcode");
    UbTaHeaderPresence presence;
    presence.tokenValue = taHeader.GetTvEn();
    presence.messageTarget = taHeader.GetMtEn();
    presence.userData = taHeader.GetUdFlg();
    const auto plan = UbTaHeaderPlan::ForRequest(*rule, presence);
    UbMAExtTah maHeader;
    UbTransactionFields transactionFields;
    if (!DecodeFullTaExtensions(copy, plan, maHeader, transactionFields).IsSuccess())
        return reject("malformed request extension layout");
    const bool maetahPlanned = plan.Contains(UbTransactionHeaderKind::MAETAH);
    if (maetahPlanned)
    {
        if (maHeader.GetLength() > UINT64_MAX - maHeader.GetVirtualAddress())
            return reject("memory access address overflows");
        transactionFields.memoryAccess = UbMemoryAccessFields{
            maHeader.GetVirtualAddress(), maHeader.GetTokenId(), maHeader.GetLength()};
    }
    UbFlowTag flowTag;
    packet->PeekPacketTag(flowTag);
    UbPacketTraceTag traceTag;
    packet->PeekPacketTag(traceTag);
    CtpRecvNotify(packet->GetUid(), localKey, taHeader.GetIniTaSsn(), PacketType::PACKET,
                  copy->GetSize(), flowTag.GetFlowId(), traceTag);

    Ptr<const Packet> explicitPayload;
    UbExplicitPayloadTag explicitPayloadTag;
    if (copy->PeekPacketTag(explicitPayloadTag))
    {
        Ptr<Packet> headerFreePayload = copy->Copy();
        headerFreePayload->RemovePacketTag(explicitPayloadTag);
        explicitPayload = headerFreePayload;
    }

    Ptr<UbWqeSegment> request = BuildInboundTaOperation(taHeader,
                                                        maHeader,
                                                        taOpcode,
                                                        transactionFields,
                                                        maetahPlanned,
                                                        wireSrcNodeId,
                                                        wireDstNodeId,
                                                        localKey.vl,
                                                        copy->GetSize(),
                                                        flowTag.GetFlowId(),
                                                        flowTag.GetFlowSize(),
                                                        explicitPayload);
    if (request == nullptr)
    {
        return false;
    }

    request->SetDport(CTP_WILDCARD_PORT);
    if (receivePort != std::numeric_limits<uint32_t>::max() &&
        (!RoutingTypeIsPerPacket(linkHeader.GetRoutingType()) || destinationPortExplicit))
    {
        request->SetDport(static_cast<uint8_t>(receivePort));
    }
    const bool requestSourcePortExplicit = (cnaHeader.GetScna() & 0xF) != 0;
    if (requestSourcePortExplicit)
    {
        const uint32_t sourcePort = utils::Cna16ToPortId(cnaHeader.GetScna());
        NS_ABORT_MSG_IF(sourcePort > CTP_MAX_CNA16_PORT_ID,
                        "CTP source CNA cannot be encoded as a CNA16 port");
        request->SetSport(static_cast<uint8_t>(sourcePort));
    }
    else
    {
        request->SetSport(CTP_WILDCARD_PORT);
    }
    auto& localState = GetOrCreateEntityState(localKey);
    ++localState.Rx().receivedPackets;
    localState.Tx().routing.routingType = linkHeader.GetRoutingType();

    // The target execution delay belongs to the common transaction entry, which both carriers use.
    return DispatchInboundTransaction(request, localKey);
}

bool
UbCtpTransportService::DispatchInboundTransaction(Ptr<UbWqeSegment> request,
                                                   const UbCtpEntityKey& key)
{
    Ptr<UbController> controller = m_node == nullptr ? nullptr : m_node->GetObject<UbController>();
    Ptr<UbTransaction> transaction =
        controller == nullptr ? nullptr : controller->GetUbTransaction();
    if (transaction == nullptr)
        return false;
    struct ResponseSink
    {
        UbCtpTransportService* self;
        UbCtpEntityKey key;

        void operator()(Ptr<UbWqeSegment> response) const
        {
            if (response == nullptr)
                return;
            auto& state = self->GetOrCreateEntityState(key);
            ++state.Rx().generatedResponses;
            self->SendResponseSegment(response, state);
        }

        // ns-3.36 FunctorCallbackImpl compares functors; distinct sinks are
        // never equal.
        bool operator!=(const ResponseSink&) const { return true; }
    };
    UbTransactionResponseSink sink(ResponseSink{this, key}, false, false);
    transaction->ExecuteInboundTransaction(UINT32_MAX, request, sink);
    return true;
}

Ptr<UbWqeSegment>
UbCtpTransportService::BuildInboundTaOperation(const UbTransactionHeader& taHeader,
                                               const UbMAExtTah& maHeader,
                                               UbTransactionOpcode taOpcode,
                                               const UbTransactionFields& fields,
                                               bool maetahPresent,
                                               uint32_t srcNodeId,
                                               uint32_t dstNodeId,
                                               uint32_t priority,
                                               uint32_t payloadBytes,
                                               uint32_t flowId,
                                               uint32_t flowSize,
                                               Ptr<const Packet> explicitPayload)
{
    // UB Base Specification 2.0, 6.7.2: one CTP TP Packet carries one complete TA operation.
    const bool isReadRequest = taOpcode == UbTransactionOpcode::READ;
    const uint32_t logicalBytes = maetahPresent ? maHeader.GetLength() : payloadBytes;
    // The complete inbound TA operation is normalized here, once; the common target entry consumes
    // this cached result instead of validating the request again.
    UbTransactionRequestInput request;
    request.opcode = taOpcode;
    request.logicalBytes = logicalBytes;
    request.payloadBytes = payloadBytes;
    request.fields = fields;
    request.maxPayloadBytes = GetTransactionConstraints().maxSegmentBytes;
    request.origin = UbTransactionRequestOrigin::INBOUND_DECODE;
    const auto validation = ValidateTransactionRequest(request);
    if (!validation.IsValid())
    {
        std::cerr << "Rejected CTP transaction taskId=" << flowId << ": " << validation.message
                  << std::endl;
        return nullptr;
    }
    const UbTransactionResolved& resolved = validation.resolved;
    if (explicitPayload != nullptr && (isReadRequest || explicitPayload->GetSize() != payloadBytes))
    {
        return nullptr;
    }

    Ptr<UbWqeSegment> operation = CreateObject<UbWqeSegment>();
    operation->SetSrc(srcNodeId);
    operation->SetDest(dstNodeId);
    operation->SetPriority(priority);
    operation->SetTaskId(flowId);
    operation->SetWqeSize(flowSize);
    operation->SetJettyNum(taHeader.GetIniTaSsn());
    operation->SetTaMsn(0);
    operation->SetTaSsn(taHeader.GetIniTaSsn());
    operation->SetSegmentKind(UbTransactionSegmentKind::REQUEST);
    operation->SetOriginJettyNum(taHeader.GetIniTaSsn());
    operation->SetRequestTassn(taHeader.GetIniTaSsn());
    operation->SetCtpRequesterIdentity(taHeader.GetIniRcId(), taHeader.GetIniRcType());
    operation->SetType(taOpcode);
    operation->SetRequestOpcode(taOpcode);
    operation->SetRemoteAddress(maHeader.GetVirtualAddress());
    operation->SetRemoteTokenId(maHeader.GetTokenId());
    operation->SetResponseBytes(resolved.responsePayloadBytes);
    operation->SetNeedsTransactionResponse(resolved.requiresResponse);
    operation->SetTransactionFields(fields);
    operation->SetTransactionResolved(resolved);

    operation->SetSize(logicalBytes);
    operation->SetLogicalBytes(logicalBytes);
    operation->SetPayloadBytes(payloadBytes);
    operation->SetCarrierBytes(isReadRequest ? 1 : payloadBytes);
    if (explicitPayload != nullptr)
    {
        operation->SetExplicitPayload(explicitPayload);
    }
    return operation;
}

void
UbCtpTransportService::RegisterQueueForTest(Ptr<UbCtpTxQueue> queue,
                                            uint32_t outPort,
                                            uint32_t priority)
{
    RegisterQueueIfNeeded(queue, outPort, priority);
}

void
UbCtpTransportService::CompleteFromTaAckForTest(uint32_t jettyNum, uint32_t taSsn)
{
    const auto rcid = GetTransactionOwner()->GetCtpRequesterRcid(jettyNum);
    if (!rcid) return;
    UbCtpEntityKey key;
    std::string reason;
    auto request = GetTransactionOwner()->FindCtpOutstanding(*rcid, taSsn, key, reason);
    if (request == nullptr || request->GetTaSsn() != taSsn) return;
    auto response = GetTransactionOwner()->BuildTransactionResponse(
        UINT32_MAX, request, *FindTransactionRule(request->GetType()), UbWorkExecutionResult{});
    response->SetSrc(request->GetDest());
    response->SetDest(request->GetSrc());
    response->SetTaskId(request->GetTaskId());
    response->SetOriginJettyNum(jettyNum);
    response->SetRequestTassn(taSsn);
    response->SetCtpRequesterIdentity(*rcid, request->GetCtpRcType());
    response->SetTransactionCorrelation(request->GetTransactionCorrelation());
    CompleteFromTaAck(key, taSsn, response);
}

void
UbCtpTransportService::RecordCnpForTest(const UbCtpEntityKey& key)
{
    UbCtpCongestionKey congestionKey{key.dstNodeId, key.dstEntityId, key.vl};
    ++m_congestionSignals[congestionKey];
}

uint32_t
UbCtpTransportService::GetQueueCount() const
{
    return static_cast<uint32_t>(m_queues.size());
}

uint32_t
UbCtpTransportService::GetCongestionStateCountForTest() const
{
    return static_cast<uint32_t>(m_congestionSignals.size());
}

const UbLifecycleResourceIdentity&
UbCtpTransportService::GetLifecycleOwnerForTest() const
{
    NS_ABORT_MSG_IF(m_lifecycleOwner.generation == 0, "CTP lifecycle owner is not initialized");
    return m_lifecycleOwner;
}

uint32_t
UbCtpTransportService::SelectOutPort(Ptr<UbWqeSegment> segment,
                                     UbCtpEntityState& state,
                                     const UbCtpEntityKey& routeKey,
                                     uint16_t loadBalanceSalt)
{
    NS_ABORT_MSG_IF(m_node == nullptr, "CTP send requires a source node");
    NS_ABORT_MSG_IF(segment == nullptr, "CTP send requires a WQE segment");

    const uint32_t sourcePortId =
        segment->GetSport() == CTP_WILDCARD_PORT ? UINT32_MAX : segment->GetSport();
    Ptr<UbController> controller = m_node->GetObject<UbController>();
    NS_ABORT_MSG_IF(controller == nullptr || !controller->AreCtpEntitiesFrozen(),
                    "CTP source Entity registry must be frozen before sending");
    NS_ABORT_MSG_IF(!controller->HasCtpEntity(routeKey.srcEntityId),
                    "CTP source Entity " << routeKey.srcEntityId << " is not configured on node "
                                          << m_node->GetId());
    const std::vector<uint16_t>& entityPorts =
        controller->GetCtpEntityPorts(routeKey.srcEntityId);

    const uint32_t dstPort =
        segment->GetDport() == CTP_WILDCARD_PORT ? UINT32_MAX : segment->GetDport();
    NS_ABORT_MSG_IF(routeKey.dstNodeId >= NodeList::GetNNodes(),
                    "CTP destination node does not exist: " << routeKey.dstNodeId);
    Ptr<UbController> peerController =
        NodeList::GetNode(routeKey.dstNodeId)->GetObject<UbController>();
    NS_ABORT_MSG_IF(peerController == nullptr || !peerController->AreCtpEntitiesFrozen(),
                    "CTP destination Entity registry must be frozen before sending from node "
                        << m_node->GetId() << " to node " << routeKey.dstNodeId << " Entity "
                        << routeKey.dstEntityId << " at TASSN " << segment->GetTaSsn());
    NS_ABORT_MSG_IF(!peerController->HasCtpEntity(routeKey.dstEntityId),
                    "CTP destination Entity " << routeKey.dstEntityId
                                               << " is not configured on node "
                                               << routeKey.dstNodeId);
    NS_ABORT_MSG_IF(dstPort != UINT32_MAX &&
                        !peerController->IsCtpEntityPortMember(routeKey.dstEntityId, dstPort),
                    "CTP destination port " << dstPort << " is not a member of Entity "
                                             << routeKey.dstEntityId << " on node "
                                             << routeKey.dstNodeId);

    if (sourcePortId != UINT32_MAX)
    {
        const uint32_t outPort = sourcePortId;
        NS_ABORT_MSG_IF(!controller->IsCtpEntityPortMember(routeKey.srcEntityId, outPort),
                        "CTP source port " << outPort << " is not a member of Entity "
                                            << routeKey.srcEntityId);
        NS_ABORT_MSG_IF(outPort > CTP_MAX_CNA16_PORT_ID,
                        "CTP source port " << outPort << " cannot be encoded as CNA16");
        NS_ABORT_MSG_IF(outPort >= m_node->GetNDevices(),
                        "CTP source port " << outPort << " is outside the node device range");
        NS_ABORT_MSG_IF(DynamicCast<UbPort>(m_node->GetDevice(outPort)) == nullptr,
                        "CTP source port " << outPort << " is not a UB port");
        return outPort;
    }

    Ptr<UbSwitch> sw = m_node->GetObject<UbSwitch>();
    NS_ABORT_MSG_IF(sw == nullptr,
                    "CTP send requires an explicit source port or a route to destination node");
    Ptr<UbRoutingProcess> routing = sw->GetRoutingProcess();
    NS_ABORT_MSG_IF(routing == nullptr,
                    "CTP send requires an explicit source port or a route to destination node");

    const uint32_t routingDstPort =
        dstPort == UINT32_MAX
            ? peerController->SelectCtpEntityMemberPort(routeKey.dstEntityId,
                                                        static_cast<uint8_t>(loadBalanceSalt))
            : dstPort;
    RoutingKey rtKey;
    rtKey.sip = utils::NodeIdToIp(m_node->GetId()).Get();
    rtKey.dip = utils::NodeIdToIp(routeKey.dstNodeId, routingDstPort).Get();
    rtKey.sport = loadBalanceSalt;
    rtKey.dport = static_cast<uint16_t>(routingDstPort);
    rtKey.priority = routeKey.vl;
    rtKey.routingType = state.Tx().routing.routingType;

    bool selectedShortestPath = false;
    const int outPort = routing->GetOutPortForEndpointRestricted(rtKey,
                                                                 entityPorts,
                                                                 selectedShortestPath);
    // TODO: Source routing currently ignores selectedShortestPath. Propagate a non-shortest
    // selection into the transmit routing policy before building the packet header so a
    // downstream switch cannot consume the UB one-detour allowance again.
    NS_ABORT_MSG_IF(
        outPort < 0,
        "CTP source node " << m_node->GetId() << " Entity " << routeKey.srcEntityId
                           << " members " << FormatPortList(entityPorts)
                           << " has no route to destination node " << routeKey.dstNodeId
                           << " Entity " << routeKey.dstEntityId << " member port "
                           << routingDstPort);
    NS_ABORT_MSG_IF(static_cast<uint32_t>(outPort) >= m_node->GetNDevices(),
                    "CTP route selected an outPort outside the node device range");
    NS_ABORT_MSG_IF(DynamicCast<UbPort>(m_node->GetDevice(static_cast<uint32_t>(outPort))) ==
                        nullptr,
                    "CTP route selected a non-UB source port");
    return static_cast<uint32_t>(outPort);
}

uint16_t
UbCtpTransportService::NextLoadBalanceSalt(UbCtpEntityState& state, const UbCtpEntityKey& routeKey)
{
    if (!RoutingTypeIsPerPacket(state.Tx().routing.routingType))
    {
        return EntityLoadBalanceSalt(m_node->GetId(), routeKey);
    }
    return static_cast<uint16_t>(state.Tx().routing.nextSprayIndex++);
}

void
UbCtpTransportService::RegisterQueueIfNeeded(Ptr<UbCtpTxQueue> queue,
                                             uint32_t outPort,
                                             uint32_t priority)
{
    NS_ABORT_MSG_IF(queue == nullptr, "CTP queue registration requires a queue");
    NS_ABORT_MSG_IF(m_node == nullptr, "CTP queue registration requires a source node");

    if (m_registeredQueues.find(queue) != m_registeredQueues.end())
    {
        return;
    }

    Ptr<UbSwitch> sw = m_node->GetObject<UbSwitch>();
    NS_ABORT_MSG_IF(sw == nullptr, "CTP queue registration requires UbSwitch");
    sw->RegisterTpWithAllocator(queue, outPort, priority);
    m_registeredQueues.insert(queue);
}

bool
UbCtpTransportService::SendResponseSegment(Ptr<UbWqeSegment> response, UbCtpEntityState& state)
{
    NS_ABORT_MSG_IF(response == nullptr, "CTP response send requires WQE segment");
    if (response->GetType() != UbTransactionOpcode::TAACK &&
        response->GetBytesLeft() > GetTransactionConstraints().maxSegmentBytes)
    {
        return false;
    }

    const UbCtpEntityKey& key = state.GetKey();
    uint32_t taSsn = response->GetRequestTassn();
    if (response->GetType() == UbTransactionOpcode::TAACK)
    {
        const uint16_t lbSalt = NextLoadBalanceSalt(state, key);
        const uint32_t outPort = SelectOutPort(response, state, key, lbSalt);
        Ptr<Packet> packet = BuildDataPacketWithTaSsn(response,
                                                      state,
                                                      key,
                                                      taSsn,
                                                      0,
                                                      static_cast<uint8_t>(lbSalt & 0xff));
        Ptr<UbCtpTxQueue> queue = GetOrCreateQueue(key, outPort, key.vl);
        RegisterQueueIfNeeded(queue, outPort, key.vl);
        queue->EnqueueAck(packet);

        Ptr<UbPort> port = DynamicCast<UbPort>(m_node->GetDevice(outPort));
        NS_ABORT_MSG_IF(port == nullptr, "CTP response send requires valid out port");
        port->TriggerTransmit();
        return true;
    }

    if (!response->IsSentCompleted())
    {
        const uint32_t progressBytes = static_cast<uint32_t>(response->GetBytesLeft());
        const uint16_t lbSalt = NextLoadBalanceSalt(state, key);
        const uint32_t outPort = SelectOutPort(response, state, key, lbSalt);
        Ptr<Packet> packet = BuildDataPacketWithTaSsn(response,
                                                      state,
                                                      key,
                                                      taSsn,
                                                      progressBytes,
                                                      static_cast<uint8_t>(lbSalt & 0xff));
        Ptr<UbCtpTxQueue> queue = GetOrCreateQueue(key, outPort, key.vl);
        RegisterQueueIfNeeded(queue, outPort, key.vl);
        queue->EnqueueData(packet);
        response->UpdateSentBytes(progressBytes);

        Ptr<UbPort> port = DynamicCast<UbPort>(m_node->GetDevice(outPort));
        NS_ABORT_MSG_IF(port == nullptr, "CTP response send requires valid out port");
        port->TriggerTransmit();
        ++taSsn;
    }
    return true;
}

void
UbCtpTransportService::CompleteFromTaAck(const UbCtpEntityKey& key,
                                         uint32_t taSsn,
                                         Ptr<UbWqeSegment> response)
{
    if (ShouldDelayTaAck(key, taSsn))
    {
        TraceWindowEvent(key, "taack-delay", taSsn,
                         *GetTransactionOwner()->GetJetty(response->GetOriginJettyNum()));
        const auto* identity = GetTransactionOwner()->FindCtpRequester(*response->GetCtpRcid());
        NS_ABORT_MSG_IF(identity == nullptr, "Validated delayed CTP response lost its requester");
        const auto owner = *identity;
        const uint64_t token = m_delayedTaAckEvents.Track(owner);
        EventId event = Simulator::Schedule(m_delayTaAckTime,
                                            &UbCtpTransportService::RunTrackedCompleteFromTaAck,
                                            this, owner, key, taSsn, response, token);
        m_delayedTaAckEvents.Attach(owner, token, event, Callback<void>());
        return;
    }
    CompleteFromTaAckNow(key, taSsn, response);
}

void
UbCtpTransportService::RunTrackedCompleteFromTaAck(UbLifecycleResourceIdentity owner,
                                                   UbCtpEntityKey key,
                                                   uint32_t taSsn,
                                                   Ptr<UbWqeSegment> response,
                                                   uint64_t token)
{
    Ptr<UbCtpTransportService> keepAlive(this);
    if (!m_delayedTaAckEvents.Begin(owner, token)) return;
    // Keep the manager alive across a completion callback that may dispose this service.
    auto transaction = GetTransactionOwner();
    Ptr<UbFunction> function = transaction->GetFunction();
    const auto* currentOwner = transaction->FindCtpRequester(*response->GetCtpRcid());
    NS_ABORT_MSG_IF(currentOwner != nullptr && *currentOwner != owner,
                    "Delayed CTP completion changed its exact requester generation");
    if (currentOwner != nullptr) CompleteFromTaAckNow(key, taSsn, response);
    m_delayedTaAckEvents.Resolve(owner, token);
    function->NotifyJettyProgress(static_cast<uint32_t>(owner.localId));
}

void
UbCtpTransportService::CompleteFromTaAckNow(const UbCtpEntityKey& key,
                                            uint32_t taSsn,
                                            Ptr<UbWqeSegment> response)
{
    if (response == nullptr || !response->GetCtpRcid() ||
        !response->HasTransactionCorrelation()) return;
    UbCtpEntityKey currentKey;
    std::string reason;
    auto transaction = GetTransactionOwner();
    auto request = transaction->FindCtpOutstanding(
        *response->GetCtpRcid(), static_cast<uint16_t>(taSsn), currentKey, reason);
    // A resolved duplicate can outlive an entire wire sequence cycle. Its captured absolute
    // sequence must never be reinterpreted as a later operation using the same wire16 value.
    if (request == nullptr || request->GetTaSsn() != taSsn) return;
    NS_ABORT_MSG_IF(!(currentKey == key) ||
                        request->GetTransactionOperationIdentity() !=
                            response->GetTransactionOperationIdentity() ||
                        request->GetTransactionSegmentIdentity() !=
                            response->GetTransactionSegmentIdentity(),
                    "Delayed CTP completion contradicts live operation/segment identity");
    // The common completion seam validates the response before retiring the sole SegmentRecord
    // and marking the sole Jetty ACK bitmap. No Entity progress is maintained here.
    // Both wire ingress and the test response builder already validated this response. Preserve
    // the existing last-ACK trace before the completion callback can submit dependent work.
    LastPacketACKsNotify(request->GetTaskId(), key, taSsn, request->GetSize(), request->GetType());
    // Trace observers may synchronously retire this requester. That is a stale completion,
    // while a conflicting still-live record is an internal ownership error.
    auto afterTrace = transaction->FindCtpOutstanding(
        *response->GetCtpRcid(), static_cast<uint16_t>(taSsn), currentKey, reason);
    if (afterTrace == nullptr) return;
    NS_ABORT_MSG_IF(afterTrace != request, "CTP ACK trace changed a live segment identity");
    NS_ABORT_MSG_IF(!transaction->ProcessInboundTaResponse(response),
                    "Validated CTP response contradicts its live operation/segment backing");
}

void
UbCtpTransportService::RemoveRequesterPackets(const UbLifecycleResourceIdentity& owner)
{
    for (const auto& [key, queue] : m_queues)
    {
        (void)key;
        queue->RemoveRequesterPackets(owner);
    }
}

bool
UbCtpTransportService::HasRequesterPackets(const UbLifecycleResourceIdentity& owner) const
{
    for (const auto& [key, queue] : m_queues)
    {
        (void)key;
        if (queue->HasRequesterPackets(owner)) return true;
    }
    return false;
}

void
UbCtpTransportService::CancelRequesterCompletionEvents(const UbLifecycleResourceIdentity& owner)
{
    // Reuse the service's existing event set, now leasing the exact requester generation.
    // Running callbacks remain leased until their normal unwind/Resolve.
    const auto cleanups = m_delayedTaAckEvents.CancelPending(owner);
    for (const auto& cleanup : cleanups) cleanup();
}

void
UbCtpTransportService::DoDispose()
{
    if (m_node != nullptr)
    {
        std::vector<UbLifecycleResourceIdentity> owners;
        for (const auto& [key, jettys] : m_keyJettys)
        {
            (void)key;
            for (const auto& jetty : jettys)
            {
                UbLifecycleResourceIdentity identity;
                if (GetTransactionOwner()->GetFunction()->TryGetJettyLifecycleIdentity(
                        jetty->GetJettyNum(), identity)) owners.push_back(identity);
            }
        }
        if (!owners.empty())
        {
            Ptr<UbFunction> function = GetTransactionOwner()->GetFunction();
            for (const auto& owner : owners)
            {
                CancelRequesterCompletionEvents(owner);
                function->NotifyJettyProgress(static_cast<uint32_t>(owner.localId));
            }
        }
    }
    if (m_delayedTaAckEvents.IsEmpty())
    {
        m_delayedTaAckEvents.Clear();
    }

    Ptr<UbSwitch> sw = m_node == nullptr ? nullptr : m_node->GetObject<UbSwitch>();
    for (const auto& [key, queue] : m_queues)
    {
        (void)key;
        if (queue == nullptr)
        {
            continue;
        }
        if (m_registeredQueues.find(queue) != m_registeredQueues.end() && sw != nullptr &&
            sw->GetAllocator() != nullptr)
        {
            sw->RemoveTpFromAllocator(queue);
        }
    }
    m_registeredQueues.clear();
    m_queues.clear();
    m_keyJettys.clear();
    m_keyJettyRrIndex.clear();
    m_node = nullptr;
    Object::DoDispose();
}

uint32_t
UbCtpTransportService::GetQueuedPacketCountForTest(const UbCtpEntityKey& key) const
{
    uint32_t count = 0;
    for (const auto& [queueKey, queue] : m_queues)
    {
        if (std::get<0>(queueKey) == key && queue != nullptr)
        {
            count += queue->GetQueuedPacketCountForTest();
        }
    }
    return count;
}

uint32_t
UbCtpTransportService::GetQueuedPacketCountForTest(const UbCtpEntityKey& key,
                                                   uint32_t outPort,
                                                   uint32_t priority) const
{
    QueueKey queueKey = std::make_tuple(key, outPort, priority);
    auto it = m_queues.find(queueKey);
    if (it == m_queues.end() || it->second == nullptr)
    {
        return 0;
    }
    return it->second->GetQueuedPacketCountForTest();
}

uint32_t
UbCtpTransportService::GetRegisteredQueueCountForTest() const
{
    return static_cast<uint32_t>(m_registeredQueues.size());
}

bool
UbCtpTransportService::IsPacketTraceEnabledForTest() const
{
    return m_pktTraceEnabled;
}

void
UbCtpTransportService::FirstPacketSendsNotify(uint32_t taskId,
                                              const UbCtpEntityKey& key,
                                              uint32_t taSsn,
                                              uint32_t outPort,
                                              uint32_t payloadBytes,
                                              UbTransactionOpcode opcode)
{
    m_traceFirstPacketSendsNotify({m_node == nullptr ? UINT32_MAX : m_node->GetId(),
                                   taskId,
                                   m_node == nullptr ? UINT32_MAX : m_node->GetId(),
                                   key.dstNodeId,
                                   key.srcEntityId,
                                   key.dstEntityId,
                                   static_cast<uint32_t>(key.vl),
                                   taSsn,
                                   outPort,
                                   payloadBytes,
                                   static_cast<uint32_t>(opcode)});
}

void
UbCtpTransportService::LastPacketACKsNotify(uint32_t taskId,
                                            const UbCtpEntityKey& key,
                                            uint32_t taSsn,
                                            uint32_t payloadBytes,
                                            UbTransactionOpcode opcode)
{
    m_traceLastPacketACKsNotify({m_node == nullptr ? UINT32_MAX : m_node->GetId(),
                                 taskId,
                                 m_node == nullptr ? UINT32_MAX : m_node->GetId(),
                                 key.dstNodeId,
                                 key.srcEntityId,
                                 key.dstEntityId,
                                 static_cast<uint32_t>(key.vl),
                                 taSsn,
                                 UINT32_MAX,
                                 payloadBytes,
                                 static_cast<uint32_t>(opcode)});
}

void
UbCtpTransportService::CtpRecvNotify(uint32_t packetUid,
                                     const UbCtpEntityKey& key,
                                     uint32_t taSsn,
                                     PacketType type,
                                     uint32_t size,
                                     uint32_t taskId,
                                     UbPacketTraceTag traceTag)
{
    m_ctpRecvNotify({packetUid,
                     key.dstNodeId,
                     m_node == nullptr ? UINT32_MAX : m_node->GetId(),
                     key.dstEntityId,
                     key.srcEntityId,
                     static_cast<uint32_t>(key.vl),
                     taSsn,
                     type,
                     size,
                     taskId,
                     traceTag});
}

bool
UbCtpTransportService::ShouldTraceWindow(const UbCtpEntityKey& key) const
{
    if (!m_windowTraceEnabled)
    {
        return false;
    }
    if (m_windowTraceSrcNode != std::numeric_limits<uint32_t>::max() &&
        (m_node == nullptr || m_node->GetId() != m_windowTraceSrcNode))
    {
        return false;
    }
    if (m_windowTraceDstNode != std::numeric_limits<uint32_t>::max() &&
        key.dstNodeId != m_windowTraceDstNode)
    {
        return false;
    }
    return m_node != nullptr;
}

bool
UbCtpTransportService::ShouldDelayTaAck(const UbCtpEntityKey& key, uint32_t taSsn) const
{
    if (m_delayTaAckSrcNode == std::numeric_limits<uint32_t>::max() ||
        m_delayTaAckTime <= NanoSeconds(0))
    {
        return false;
    }
    const bool exactMatch = m_delayTaAckSequence != std::numeric_limits<uint32_t>::max() &&
                            taSsn == m_delayTaAckSequence;
    const bool moduloMatch =
        m_delayTaAckModulo != 0 &&
        taSsn % m_delayTaAckModulo == m_delayTaAckRemainder % m_delayTaAckModulo;
    if (!exactMatch && !moduloMatch)
    {
        return false;
    }
    if (m_node == nullptr || m_node->GetId() != m_delayTaAckSrcNode)
    {
        return false;
    }
    if (m_delayTaAckDstNode != std::numeric_limits<uint32_t>::max() &&
        key.dstNodeId != m_delayTaAckDstNode)
    {
        return false;
    }
    return true;
}

void
UbCtpTransportService::TraceWindowEvent(const UbCtpEntityKey& key,
                                        const std::string& event,
                                        uint32_t taSsn,
                                        const UbJetty& jetty,
                                        uint32_t outPort) const
{
    if (!ShouldTraceWindow(key))
    {
        return;
    }

    std::ostringstream oss;
    oss << "CTP WINDOW"
        << " event: " << event << " src: " << m_node->GetId() << " srcEntity: " << key.srcEntityId
        << " dst: " << key.dstNodeId << " dstEntity: " << key.dstEntityId
        << " vl: " << static_cast<uint32_t>(key.vl) << " taSsn: " << taSsn
        << " jetty: " << jetty.GetJettyNum() << " sendNext: " << jetty.GetTaSsnSndNxt()
        << " completeUna: " << jetty.GetTaSsnSndUna()
        << " outstanding: " << (jetty.GetTaSsnSndNxt() - jetty.GetTaSsnSndUna());
    if (outPort != std::numeric_limits<uint32_t>::max())
    {
        oss << " outPort: " << outPort;
    }
    NS_LOG_INFO(oss.str());
}

void
UbCtpTransportService::TraceDetailEvent(const UbCtpEntityKey& key,
                                        const std::string& event,
                                        uint32_t taSsn,
                                        uint32_t bytes,
                                        uint32_t expectedBytes,
                                        uint32_t flowBytes,
                                        uint32_t flowSize,
                                        uint32_t outPort) const
{
    if (!ShouldTraceWindow(key))
    {
        return;
    }

    std::ostringstream oss;
    oss << "CTP DETAIL"
        << " event: " << event << " src: " << m_node->GetId() << " srcEntity: " << key.srcEntityId
        << " dst: " << key.dstNodeId << " dstEntity: " << key.dstEntityId
        << " vl: " << static_cast<uint32_t>(key.vl) << " taSsn: " << taSsn;
    if (bytes != UINT32_MAX)
    {
        oss << " bytes: " << bytes;
    }
    if (expectedBytes != UINT32_MAX)
    {
        oss << " expectedBytes: " << expectedBytes;
    }
    if (flowBytes != UINT32_MAX)
    {
        oss << " flowBytes: " << flowBytes;
    }
    if (flowSize != UINT32_MAX)
    {
        oss << " flowSize: " << flowSize;
    }
    if (outPort != UINT32_MAX)
    {
        oss << " outPort: " << outPort;
    }
    NS_LOG_INFO(oss.str());
}

} // namespace ns3
