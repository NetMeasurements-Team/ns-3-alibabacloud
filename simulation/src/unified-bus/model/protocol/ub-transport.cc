// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-caqm.h"
#include "ns3/ub-dcqcn.h"
#include "../ub-network-address.h"
#include "ns3/node.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-queue-manager.h"
#include "ns3/ub-transport.h"
#include "ns3/ub-utils.h"

using namespace utils;
namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbTransportChannel");

NS_OBJECT_ENSURE_REGISTERED(UbTransportChannel);

namespace
{

bool
IsZeroPayloadReadRequest(const Ptr<UbWqeSegment>& segment)
{
    return segment != nullptr && segment->GetType() == TaOpcode::TA_OPCODE_READ &&
           segment->GetSegmentKind() == UbTransactionSegmentKind::REQUEST;
}

uint32_t
GetProgressBytesThisPacket(const Ptr<UbWqeSegment>& segment)
{
    return std::min<uint64_t>(segment->GetBytesLeft(), UB_MTU_BYTE);
}

uint32_t
GetPayloadBytesThisPacket(const Ptr<UbWqeSegment>& segment, uint32_t progressBytes)
{
    return IsZeroPayloadReadRequest(segment) ? 0 : progressBytes;
}

uint32_t
GetWireLengthBytes(const Ptr<UbWqeSegment>& segment, uint32_t payloadBytes)
{
    return IsZeroPayloadReadRequest(segment) ? segment->GetLogicalBytes() : payloadBytes;
}

uint32_t
GetTotalProgressBytes(const Ptr<UbWqeSegment>& segment)
{
    return segment->GetCarrierBytes() == 0 ? segment->GetSize() : segment->GetCarrierBytes();
}

void
TraceTpDebugState(uint32_t nodeId,
                  uint32_t tpn,
                  const std::string& reason,
                  uint64_t psnSndNxt,
                  uint64_t psnSndUna,
                  uint64_t maxInflightPackets,
                  bool ccLimited,
                  bool sendWindowLimited,
                  uint32_t activeSegments,
                  uint32_t totalSegments,
                  uint32_t ackQueueLen,
                  uint32_t cnpQueueLen)
{
    const uint64_t inflightPackets = psnSndNxt - psnSndUna;
    utils::UbUtils::TpDebugStateNotify(nodeId,
                                       tpn,
                                       reason,
                                       psnSndNxt,
                                       psnSndUna,
                                       inflightPackets,
                                       maxInflightPackets,
                                       inflightPackets >= maxInflightPackets,
                                       ccLimited,
                                       sendWindowLimited,
                                       activeSegments,
                                       totalSegments,
                                       ackQueueLen,
                                       cnpQueueLen);
}

} // namespace

TypeId UbTransportChannel::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbTransportChannel")
        .SetParent<UbIngressQueue>()
        .SetGroupName("UnifiedBus")
        .AddAttribute("EnableRetrans",
                      "Enable transport-layer retransmission.",
                      BooleanValue(false),
                      MakeBooleanAccessor(&UbTransportChannel::m_isRetransEnable),
                      MakeBooleanChecker())
        .AddAttribute("InitialRTO",
                      "Initial retransmission timeout in nanoseconds (RTO0).",
                      TimeValue(NanoSeconds(25600)),
                      MakeTimeAccessor(&UbTransportChannel::m_initialRto),
                      MakeTimeChecker())
        .AddAttribute("MaxRetransAttempts",
                      "Maximum retransmission attempts before aborting.",
                      UintegerValue(7),
                      MakeUintegerAccessor(&UbTransportChannel::m_maxRetransAttempts),
                      MakeUintegerChecker<uint16_t>())
        .AddAttribute("RetransExponentFactor",
                      "Exponential backoff multiplier applied to RTO on each retransmission attempt.",
                      UintegerValue(1),
                      MakeUintegerAccessor(&UbTransportChannel::m_retransExponentFactor),
                      MakeUintegerChecker<uint16_t>())
        .AddAttribute("DefaultMaxWqeSegNum",
                      "Default limit on outstanding WQE segments per TP.",
                      UintegerValue(1000),
                      MakeUintegerAccessor(&UbTransportChannel::m_defaultMaxWqeSegNum),
                      MakeUintegerChecker<uint32_t>())
        .AddAttribute("DefaultMaxInflightPacketSize",
                      "Maximum number of in-flight packets allowed per transport channel.",
                      UintegerValue(1000),
                      MakeUintegerAccessor(&UbTransportChannel::m_defaultMaxInflightPacketSize),
                      MakeUintegerChecker<uint32_t>())
        .AddAttribute("TpOooThreshold",
                      "Receiver out-of-order PSN window size tracked in bitmap.",
                      UintegerValue(2048),
                      MakeUintegerAccessor(&UbTransportChannel::m_psnOooThreshold),
                      MakeUintegerChecker<uint64_t>())
        .AddAttribute("UsePacketSpray",
                      "Enable per-packet ECMP/packet spray across multiple paths.",
                      BooleanValue(false),
                      MakeBooleanAccessor(&UbTransportChannel::m_usePacketSpray),
                      MakeBooleanChecker())
        .AddAttribute("UseShortestPaths",
                      "Sets a packet header flag that instructs switches to restrict forwarding to shortest paths (true) or allow non-shortest paths (false).",
                      BooleanValue(true),
                      MakeBooleanAccessor(&UbTransportChannel::m_useShortestPaths),
                      MakeBooleanChecker())
        .AddTraceSource("FirstPacketSendsNotify",
                        "Fires when the first packet of a WQE segment is sent.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceFirstPacketSendsNotify),
                        "ns3::UbTransportChannel::FirstPacketSendsNotify")
        .AddTraceSource("LastPacketSendsNotify",
                        "Fires when the last packet of a WQE segment is sent.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceLastPacketSendsNotify),
                        "ns3::UbTransportChannel::LastPacketSendsNotify")
        .AddTraceSource("LastPacketACKsNotify",
                        "Fires when the last packet of a WQE segment is ACKed.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceLastPacketACKsNotify),
                        "ns3::UbTransportChannel::LastPacketACKsNotify")
        .AddTraceSource("LastPacketReceivesNotify",
                        "Fires when the last packet of a WQE segment is received.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceLastPacketReceivesNotify),
                        "ns3::UbTransportChannel::LastPacketReceivesNotify")
        .AddTraceSource("WqeSegmentSendsNotify",
                        "Fires when a WQE segment is scheduled for transmission.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceWqeSegmentSendsNotify),
                        "ns3::UbTransportChannel::WqeSegmentSendsNotify")
        .AddTraceSource("WqeSegmentCompletesNotify",
                        "Fires when a WQE segment completes at the receiver.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceWqeSegmentCompletesNotify),
                        "ns3::UbTransportChannel::WqeSegmentCompletesNotify")
        .AddTraceSource("TpRecvNotify",
                        "Fires on TP data or ACK reception (provides info and trace tags).",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_tpRecvNotify),
                        "ns3::UbTransportChannel::TpRecvNotify");
    return tid;
}

/**
 * @brief Constructor for UbTransportChannel
 */
UbTransportChannel::UbTransportChannel()
{
    BooleanValue val;
    if (GlobalValue::GetValueByNameFailSafe("UB_RECORD_PKT_TRACE", val)) {
        GlobalValue::GetValueByName("UB_RECORD_PKT_TRACE", val);
        m_pktTraceEnabled = val.Get();
    } else {
        m_pktTraceEnabled = false;
    }
    NS_LOG_FUNCTION(this);
}

UbTransportChannel::~UbTransportChannel()
{
    // Clear WQE queues and release resources
    NS_LOG_INFO("tp release, node:" << m_src << " tpn:" << m_tpn);
    NS_LOG_FUNCTION(this);
}


void UbTransportChannel::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_ackQ = queue<Ptr<Packet>>();
    m_cnpQ = queue<Ptr<Packet>>();
    m_wqeSegmentVector.clear();
    m_inboundTaUnits.clear();
    m_bufferedInboundPackets.clear();
    m_congestionCtrl = nullptr;
    m_recvPsnWindow.Resize(0);
}

/**
 * @brief Get next packet from transport channel queue
 * Called by Switch Allocator during scheduling to retrieve the next packet for transmission
 */
Ptr<Packet> UbTransportChannel::GetNextPacket()
{
    if (!m_cnpQ.empty()) {
        Ptr<Packet> p = m_cnpQ.front();
        m_cnpQ.pop();
        if (!IsEmpty()) {
            m_headArrivalTime = Simulator::Now();
        }
        return p;
    }

    // 如果有ack，先发ack
    if (!m_ackQ.empty()) {
        Ptr<Packet> p = m_ackQ.front();
        m_ackQ.pop();
        if (!IsEmpty()) {
            m_headArrivalTime = Simulator::Now();
        }
        return p;
    }

    if (m_wqeSegmentVector.empty()) {
        NS_LOG_DEBUG("No WQE segments available to send");
        TraceTpDebugState(m_nodeId,
                          m_tpn,
                          "GET_NEXT_EMPTY_SEGMENTS",
                          m_psnSndNxt,
                          m_psnSndUna,
                          m_maxInflightPacketSize,
                          false,
                          m_sendWindowLimited,
                          GetActiveSendSegmentCount(),
                          static_cast<uint32_t>(m_wqeSegmentVector.size()),
                          static_cast<uint32_t>(m_ackQ.size()),
                          static_cast<uint32_t>(m_cnpQ.size()));
        return nullptr;
    }

    if (IsInflightLimited()) {
        m_sendWindowLimited = true;
        NS_LOG_DEBUG("Full Send Window");
        TraceTpDebugState(m_nodeId,
                          m_tpn,
                          "GET_NEXT_INFLIGHT_LIMITED",
                          m_psnSndNxt,
                          m_psnSndUna,
                          m_maxInflightPacketSize,
                          false,
                          m_sendWindowLimited,
                          GetActiveSendSegmentCount(),
                          static_cast<uint32_t>(m_wqeSegmentVector.size()),
                          static_cast<uint32_t>(m_ackQ.size()),
                          static_cast<uint32_t>(m_cnpQ.size()));
        return nullptr;
    }
    for (size_t i = 0; i < m_wqeSegmentVector.size(); ++i) {
        Ptr<UbWqeSegment> currentSegment = m_wqeSegmentVector[i];

        if (currentSegment == nullptr || currentSegment->IsSentCompleted()) {
            continue;
        }
        const uint32_t progressBytes = GetProgressBytesThisPacket(currentSegment);
        const uint32_t payloadSize = GetPayloadBytesThisPacket(currentSegment, progressBytes);
        const uint32_t wireLengthBytes = GetWireLengthBytes(currentSegment, payloadSize);
        const uint32_t totalProgressBytes = GetTotalProgressBytes(currentSegment);

        if (m_congestionCtrl->IsCcLimited(progressBytes)) {
            m_sendWindowLimited = true;
            TraceTpDebugState(m_nodeId,
                              m_tpn,
                              "GET_NEXT_CC_LIMITED",
                              m_psnSndNxt,
                              m_psnSndUna,
                              m_maxInflightPacketSize,
                              true,
                              m_sendWindowLimited,
                              GetActiveSendSegmentCount(),
                              static_cast<uint32_t>(m_wqeSegmentVector.size()),
                              static_cast<uint32_t>(m_ackQ.size()),
                              static_cast<uint32_t>(m_cnpQ.size()));
            return nullptr;
        }

        Ptr<Packet> p = GenDataPacket(currentSegment, payloadSize, wireLengthBytes, progressBytes);

        m_congestionCtrl->OnSenderDataPacketSent(m_psnSndNxt, progressBytes);

        if (currentSegment->GetBytesLeft() == totalProgressBytes) {
            // wqe segment first packet
            FirstPacketSendsNotify(m_nodeId, currentSegment->GetTaskId(), m_tpn, m_dstTpn,
                currentSegment->GetTpMsn(), m_psnSndNxt, m_sport);
        }
        if (currentSegment->GetBytesLeft() == progressBytes) {
            // wqe segment last packet
            LastPacketSendsNotify(m_nodeId, currentSegment->GetTaskId(), m_tpn, m_dstTpn,
                currentSegment->GetTpMsn(), m_psnSndNxt, m_sport);
        }
        // PacketUid: TaskId: Tpn: Psn: PacketType: Src: Dst: PacketSize:
        NS_LOG_DEBUG("[Transport channel] Send packet."
                  << " PacketUid: " << p->GetUid()
                  << " Tpn: " << m_tpn
                  << " DstTpn: " << m_dstTpn
                  << " Psn: " << m_psnSndNxt
                  << " PacketType: Packet"
                  << " Src: " << m_src
                  << " Dst: " << m_dest
                  << " PacketSize: " << p->GetSize()
                  << " TaskId: " << currentSegment->GetTaskId());
        currentSegment->UpdateSentBytes(progressBytes);
        m_psnSndNxt++;
        TraceTpDebugState(m_nodeId,
                          m_tpn,
                          "SEND_PACKET",
                          m_psnSndNxt,
                          m_psnSndUna,
                          m_maxInflightPacketSize,
                          false,
                          m_sendWindowLimited,
                          GetActiveSendSegmentCount(),
                          static_cast<uint32_t>(m_wqeSegmentVector.size()),
                          static_cast<uint32_t>(m_ackQ.size()),
                          static_cast<uint32_t>(m_cnpQ.size()));
        // 发送时，更新定时器时间
        if (m_isRetransEnable) {
            if (m_retransEvent.IsExpired()) {
                // Schedules retransmit timeout. m_rto should be already doubled.
                m_rto = m_initialRto;
                NS_LOG_LOGIC(this << " SendDataPacket Schedule ReTxTimeout at time "
                                << Simulator::Now().GetNanoSeconds() << " to expire at time "
                                << (Simulator::Now().GetNanoSeconds() + m_rto.GetNanoSeconds()));
                m_retransEvent = Simulator::Schedule(m_rto, &UbTransportChannel::ReTxTimeout, this);
            }
        }
        // 浅流水只限制仍可继续发送的活跃 segment。
        // 当前 segment 最后一个 data packet 发出后，就尝试补一个新的 segment，
        // 但已发完未 ACK 的 segment 仍保留在账本中供 ACK/重传使用。
        if (currentSegment->IsSentCompleted() && GetActiveSendSegmentCount() < 2) {
            ApplyNextWqeSegment();
        }
        if (!IsEmpty()) {
            m_headArrivalTime = Simulator::Now();
        }
        return p;
    }
    return nullptr;
}

void
UbTransportChannel::EnqueueDcqcnCnp(uint8_t ecn, bool location)
{
    Ptr<Packet> cnp = BuildDcqcnCnp(ecn, location);
    const uint8_t packetVl = std::max<uint8_t>(m_priority, 1);

    UbPort::AddUdpHeader(cnp, this);
    UbPort::AddIpv4Header(cnp, this);

    UbIpBasedNetworkHeader networkHeader;
    cnp->AddHeader(networkHeader);

    UbDataLink::GenPacketHeader(cnp,
                                false,
                                false,
                                packetVl,
                                packetVl,
                                m_usePacketSpray,
                                m_useShortestPaths,
                                UbDatalinkHeaderConfig::PACKET_IPV4);

    if (m_cnpQ.empty() && m_ackQ.empty() && m_wqeSegmentVector.empty()) {
        m_headArrivalTime = Simulator::Now();
    }
    m_cnpQ.push(cnp);

    Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
    if (port != nullptr) {
        port->TriggerTransmit();
    }
}

Ptr<Packet>
UbTransportChannel::BuildDcqcnCnp(uint8_t ecn, bool location) const
{
    Ptr<Packet> cnp = Create<Packet>(0);

    UbCnpExtTph cnpHeader;
    cnpHeader.SetEcn(ecn);
    cnpHeader.SetLocation(location);
    cnp->AddHeader(cnpHeader);

    UbTransportHeader tpHeader;
    tpHeader.SetLastPacket(false);
    tpHeader.SetTPOpcode(TpOpcode::TP_OPCODE_CNP);
    tpHeader.SetNLP(0x0);
    tpHeader.SetSrcTpn(m_tpn);
    tpHeader.SetDestTpn(m_dstTpn);
    tpHeader.SetAckRequest(0);
    tpHeader.SetErrorFlag(0);
    tpHeader.SetPsn(0);
    tpHeader.SetTpMsn(0);
    cnp->AddHeader(tpHeader);

    return cnp;
}

uint32_t UbTransportChannel::GetNextPacketSize()
{
    uint32_t pktSize = 0;
    UbMAExtTah MAExtTaHeader;
    UbTransactionHeader TransactionHeader;
    UbTransportHeader  TransportHeader;
    UdpHeader   UHeader;
    Ipv4Header  I4Header;
    UbDatalinkPacketHeader  DataLinkPacketHeader;

    uint32_t MAExtTaHeaderSize = MAExtTaHeader.GetSerializedSize();
    uint32_t UbTransactionHeaderSize = TransactionHeader.GetSerializedSize();
    uint32_t UbTransportHeaderSize = TransportHeader.GetSerializedSize();
    uint32_t UdpHeaderSize = UHeader.GetSerializedSize();
    uint32_t Ipv4HeaderSize = I4Header.GetSerializedSize();
    uint32_t UbDataLinkPktSize = DataLinkPacketHeader.GetSerializedSize();

    uint32_t headerSize = MAExtTaHeaderSize + UbTransactionHeaderSize + UbTransportHeaderSize
                          + UdpHeaderSize + Ipv4HeaderSize + UbDataLinkPktSize;

    if (!m_cnpQ.empty()) {
        return m_cnpQ.front()->GetSize();
    }
    if (!m_ackQ.empty()) {
        return m_ackQ.front()->GetSize();
    }
    for (size_t i = 0; i < m_wqeSegmentVector.size(); ++i) {
        Ptr<UbWqeSegment> currentSegment = m_wqeSegmentVector[i];
        if (currentSegment == nullptr || currentSegment->IsSentCompleted()) {
            continue;
        }
        const uint32_t progressBytes = GetProgressBytesThisPacket(currentSegment);
        const uint32_t payloadSize = GetPayloadBytesThisPacket(currentSegment, progressBytes);
        pktSize = payloadSize + headerSize;
        return pktSize;
    }
    return pktSize;
}
Ptr<Packet> UbTransportChannel::GenDataPacket(Ptr<UbWqeSegment> wqeSegment,
                                              uint32_t payloadSize,
                                              uint32_t wireLengthBytes,
                                              uint32_t progressBytes)
{
    Ptr<Packet> p = Create<Packet>(payloadSize);
    UbFlowTag flowTag(wqeSegment->GetTaskId(), wqeSegment->GetWqeSize());
    p->AddPacketTag(flowTag);
    // add UbMAExtTah
    UbMAExtTah MAExtTaHeader;
    MAExtTaHeader.SetLength(wireLengthBytes);
    p->AddHeader(MAExtTaHeader);
    // add TaHeader
    UbTransactionHeader TaHeader;
    TaHeader.SetTaOpcode(wqeSegment->GetType());
    const uint16_t wireIniTaSsn =
        wqeSegment->GetSegmentKind() == UbTransactionSegmentKind::RESPONSE
            ? static_cast<uint16_t>(wqeSegment->GetRequestTassn())
            : wqeSegment->GetTaSsn();
    TaHeader.SetIniTaSsn(wireIniTaSsn);
    TaHeader.SetOrder(wqeSegment->GetOrderType());
    TaHeader.SetIniRcType(0x01);
    TaHeader.SetIniRcId(wqeSegment->GetOriginJettyNum());
    p->AddHeader(TaHeader);
    // add TpHeader
    UbTransportHeader TpHeader;
    if (wqeSegment->GetBytesLeft() == progressBytes) {
        TpHeader.SetLastPacket(true); // last packet
    } else {
        TpHeader.SetLastPacket(false); // not last packet
    }
    TpHeader.SetTPOpcode(0x1);
    TpHeader.SetNLP(0x0);
    if (wqeSegment->GetSegmentKind() == UbTransactionSegmentKind::RESPONSE) {
        TpHeader.SetSrcTpn(m_dstTpn);
        TpHeader.SetDestTpn(m_tpn);
    } else {
        TpHeader.SetSrcTpn(m_tpn);
        TpHeader.SetDestTpn(m_dstTpn);
    }
    TpHeader.SetAckRequest(1);
    TpHeader.SetErrorFlag(0);
    TpHeader.SetPsn(m_psnSndNxt);
    TpHeader.SetTpMsn(wqeSegment->GetTpMsn());
    p->AddHeader(TpHeader);
    // add udp header
    if (m_usePacketSpray) {
        if (m_lbHashSalt == UINT16_MAX) {
            m_lbHashSalt = 0;
        } else {
            m_lbHashSalt++;
        }
    }
    if (wqeSegment->GetSegmentKind() == UbTransactionSegmentKind::RESPONSE) {
        UbPort::AddUdpHeader(p, GetUdpSport(), GetSport());
    } else {
        UbPort::AddUdpHeader(p, this);
    }
    // add ipv4 header
    if (wqeSegment->GetSegmentKind() == UbTransactionSegmentKind::RESPONSE) {
        UbPort::AddIpv4Header(p, GetDip(), GetSip());
    } else {
        UbPort::AddIpv4Header(p, this);
    }
    // add network header
    UbIpBasedNetworkHeader networkHeader;
    m_congestionCtrl->OnSenderPrepareIpBasedNetworkHeader(networkHeader);
    p->AddHeader(networkHeader);
    // add dl header
    UbDataLink::GenPacketHeader(p, false, false, m_priority, m_priority, m_usePacketSpray,
                                m_useShortestPaths, UbDatalinkHeaderConfig::PACKET_IPV4);
    return p;
}

Ptr<UbWqeSegment> UbTransportChannel::TrackInboundTaPacket(const UbTransportHeader& tpHeader,
                                                           const UbTransactionHeader& taHeader,
                                                           uint32_t logicalBytes,
                                                           uint32_t payloadBytes,
                                                           uint32_t taskId)
{
    const auto key = std::make_pair(tpHeader.GetSrcTpn(), tpHeader.GetTpMsn());
    auto& state = m_inboundTaUnits[key];
    if (state.segment == nullptr)
    {
        state.segment = CreateObject<UbWqeSegment>();
        state.segment->SetSrc(m_dest);
        state.segment->SetDest(m_src);
        state.segment->SetSport(m_dport);
        state.segment->SetDport(m_sport);
        state.segment->SetPriority(m_priority);
        state.segment->SetTaskId(taskId);
        state.segment->SetWqeSize(0);
        state.segment->SetJettyNum(taHeader.GetIniRcId());
        state.segment->SetTaMsn(tpHeader.GetTpMsn());
        state.segment->SetTaSsn(taHeader.GetIniTaSsn());
        state.segment->SetOriginJettyNum(taHeader.GetIniRcId());
        state.segment->SetRequestTassn(taHeader.GetIniTaSsn());
        state.segment->SetTpMsn(tpHeader.GetTpMsn());
        state.segment->SetTpn(m_tpn);
    }

    const auto taOpcode = static_cast<TaOpcode>(taHeader.GetTaOpcode());
    const bool isReadRequest = taOpcode == TaOpcode::TA_OPCODE_READ;
    const bool isReadResponse = taOpcode == TaOpcode::TA_OPCODE_READ_RESPONSE;
    const bool isTransactionAck = taOpcode == TaOpcode::TA_OPCODE_TRANSACTION_ACK;
    state.segment->SetType(taOpcode);
    state.segment->SetOrderType(static_cast<OrderType>(taHeader.GetOrder()));
    state.segment->SetSegmentKind(
        isTransactionAck || isReadResponse
            ? UbTransactionSegmentKind::RESPONSE
            : UbTransactionSegmentKind::REQUEST);
    if (isTransactionAck)
    {
        state.segment->SetRequestOpcode(TaOpcode::TA_OPCODE_WRITE);
    }
    else if (isReadResponse)
    {
        state.segment->SetRequestOpcode(TaOpcode::TA_OPCODE_READ);
    }
    else
    {
        state.segment->SetRequestOpcode(taOpcode);
    }
    state.segment->SetResponseBytes(isReadRequest ? logicalBytes : (isReadResponse ? payloadBytes : 0));
    state.segment->SetNeedsTransactionResponse(
        taOpcode == TaOpcode::TA_OPCODE_WRITE || isReadRequest);

    state.bytesReceived += payloadBytes;
    if (isReadRequest)
    {
        state.segment->SetSize(logicalBytes);
        state.segment->SetWqeSize(logicalBytes);
        state.segment->SetLogicalBytes(logicalBytes);
        state.segment->SetPayloadBytes(0);
        state.segment->SetCarrierBytes(1);
    }
    else
    {
        state.segment->SetSize(state.bytesReceived);
        state.segment->SetWqeSize(state.bytesReceived);
        state.segment->SetLogicalBytes(state.bytesReceived);
        state.segment->SetPayloadBytes(state.bytesReceived);
        state.segment->SetCarrierBytes(state.bytesReceived);
    }

    if (!tpHeader.GetLastPacket())
    {
        return nullptr;
    }

    Ptr<UbWqeSegment> completed = state.segment;
    m_inboundTaUnits.erase(key);
    return completed;
}

bool UbTransportChannel::ShouldCompleteOnTpAck(const Ptr<UbWqeSegment>& segment) const
{
    if (segment == nullptr)
    {
        return false;
    }
    if (segment->GetSegmentKind() == UbTransactionSegmentKind::RESPONSE)
    {
        return false;
    }
    return !segment->NeedsTransactionResponse();
}

/**
 * @brief Receive Transport Acknowledgment message
 * @param tpack Transport acknowledgment message to process
 * TP完成一个WQE后，产生TA ACK. 调用此函数将TA ACK传到TA
 */
void UbTransportChannel::RecvTpAck(Ptr<Packet> p)
{
    if (p == nullptr) {
        NS_LOG_ERROR("Null ack packet received");
        return;
    }
    UbAckTransactionHeader AckTaHeader;
    UbTransportHeader TpHeader;
    p->RemoveHeader(TpHeader); // 处理接收包信息
    if (TpHeader.GetTPOpcode() == static_cast<uint8_t>(TpOpcode::TP_OPCODE_CNP)) {
        UbCnpExtTph cnpHeader;
        p->RemoveHeader(cnpHeader);
        UbCongestionExtTph notification;
        notification.SetAckSequence(0);
        notification.SetRawBytes4to7(
            (static_cast<uint32_t>(cnpHeader.GetEcn() & 0x3U) << 30) |
            (static_cast<uint32_t>(cnpHeader.GetLocation() ? 1U : 0U) << 29));
        m_congestionCtrl->OnSenderCongestionNotification(TpOpcode::TP_OPCODE_CNP,
                                                         TpHeader.GetPsn(),
                                                         notification);
        NS_LOG_DEBUG("Recv TP CNP");
        return;
    }
    if (TpHeader.GetTPOpcode() == static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITH_CETPH)) {
        UbCongestionExtTph CETPH;
        p->RemoveHeader(CETPH);
        m_congestionCtrl->OnSenderCongestionNotification(TpOpcode::TP_OPCODE_ACK_WITH_CETPH,
                                                         TpHeader.GetPsn(),
                                                         CETPH);
    }
    p->RemoveHeader(AckTaHeader); // 处理接收包信息

    // 拿到多个packet后组成taack发送
    if ((TpHeader.GetPsn() + 1) > m_psnSndUna) {
        m_psnSndUna = TpHeader.GetPsn() + 1;
        if (m_sendWindowLimited && IsInflightLimited() == false) {
            if (!m_congestionCtrl->IsCcLimited(UB_MTU_BYTE)) {
                m_sendWindowLimited = false;
                Ptr<UbPort> port =
                    DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
                port->TriggerTransmit(); // 触发发送
            }
        }
        NS_LOG_DEBUG("[Transport channel] Recv ack."
                  << " PacketUid: " << p->GetUid()
                  << " Tpn: " << m_tpn
                  << " Psn: " << m_psnSndUna - 1
                  << " PacketType: Ack"
                  << " Src: " << m_src
                  << " Dst: " << m_dest
                  << " PacketSize: " << p->GetSize());
        if (m_pktTraceEnabled) {
            UbFlowTag flowTag;
            p->PeekPacketTag(flowTag);
            UbPacketTraceTag traceTag;
            p->PeekPacketTag(traceTag);
            TpRecvNotify({p->GetUid(), m_psnSndUna - 1, m_dest, m_src, m_dstTpn, m_tpn,
                         PacketType::ACK, p->GetSize(), flowTag.GetFlowId(), traceTag});
        }
        // 收到有效ack后更新rto和超时重传次数为初始值，关闭超时事件并重新设定超时事件
        if (m_isRetransEnable) {
            m_rto = m_initialRto;
            m_retransAttemptsLeft = m_maxRetransAttempts;
            m_retransEvent.Cancel();
            NS_LOG_LOGIC(this << " Recv ack time " << Simulator::Now().GetNanoSeconds()
                            << " reset m_retransEvent at time "
                            << (Simulator::Now().GetNanoSeconds() + m_rto.GetNanoSeconds()));
            m_retransEvent = Simulator::Schedule(m_rto, &UbTransportChannel::ReTxTimeout, this);
        }
    }
    for (size_t i = 0; i < m_wqeSegmentVector.size();) {
        if (m_psnSndUna >= (m_wqeSegmentVector[i]->GetPsnStart() + m_wqeSegmentVector[i]->GetPsnSize())) {
            // 对应ack的所有wqeSeg完成
            if (TpHeader.GetLastPacket()) {
                // 尾包ack被确认
                LastPacketACKsNotify(m_nodeId, m_wqeSegmentVector[i]->GetTaskId(), m_tpn, m_dstTpn,
                    TpHeader.GetTpMsn(), TpHeader.GetPsn(), m_sport);
            }
            if (ShouldCompleteOnTpAck(m_wqeSegmentVector[i])) {
                auto ubTa = GetTransaction();
                if (!ubTa->ProcessWqeSegmentComplete(m_wqeSegmentVector[i])) {
                    ++i;
                    continue;
                }
                WqeSegmentCompletesNotify(m_nodeId, m_wqeSegmentVector[i]->GetTaskId(),
                    m_wqeSegmentVector[i]->GetTaSsn());
            }
            m_wqeSegmentVector.erase(m_wqeSegmentVector.begin() + i);
            // 浅流水只按仍可继续发送的活跃 segment 计数。
            if (GetActiveSendSegmentCount() < 2) {
                ApplyNextWqeSegment();
            }
        } else {
            ++i;
        }
    }
    TraceTpDebugState(m_nodeId,
                      m_tpn,
                      "RECV_ACK",
                      m_psnSndNxt,
                      m_psnSndUna,
                      m_maxInflightPacketSize,
                      m_congestionCtrl->IsCcLimited(UB_MTU_BYTE),
                      m_sendWindowLimited,
                      GetActiveSendSegmentCount(),
                      static_cast<uint32_t>(m_wqeSegmentVector.size()),
                      static_cast<uint32_t>(m_ackQ.size()),
                      static_cast<uint32_t>(m_cnpQ.size()));
    // tp从超过缓存限制的状态中恢复
    if (m_tpFullFlag && IsWqeSegmentLimited() == false) {
        m_tpFullFlag = false;
        ApplyNextWqeSegment();
    }
    if (m_isRetransEnable) {
        if (m_wqeSegmentVector.size() == 0) {
            m_retransEvent.Cancel(); // 如果确认流都完成，取消定时器
        }
    }
    const bool transportIdle = IsEmpty();
    if (transportIdle) {
        m_congestionCtrl->OnSenderTransportIdle();
    }
    if (!transportIdle && !m_congestionCtrl->IsCcLimited(UB_MTU_BYTE)) {
        Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
        port->TriggerTransmit(); // 触发发送
    }
    NS_LOG_DEBUG("Recv TP(data packet) acknowledgment");
}


void UbTransportChannel::SetUbTransport(uint32_t nodeId,
                                        uint32_t src,
                                        uint32_t dest,
                                        uint32_t srcTpn,        // TP Number
                                        uint32_t dstTpn,
                                        uint64_t size,          // Size parameter
                                        uint16_t priority,      // Process group identifier
                                        uint16_t sport,
                                        uint16_t dport,
                                        Ipv4Address sip,         // Source IP address
                                        Ipv4Address dip,         // Dest IP address
                                        Ptr<UbCongestionControl> congestionCtrl)
{
    m_nodeId = nodeId;
    m_src = src;
    m_dest = dest;
    m_tpn = srcTpn;
    m_dstTpn = dstTpn;
    m_size = size;
    m_priority = priority;
    m_sport = sport;
    m_dport = dport;
    m_sip = sip;
    m_dip = dip;
    m_congestionCtrl = congestionCtrl;
    m_congestionCtrl->OnTpAttached(this);
    m_retransAttemptsLeft = m_maxRetransAttempts;
    m_maxQueueSize = m_defaultMaxWqeSegNum;
    m_maxInflightPacketSize = m_defaultMaxInflightPacketSize;
    m_recvPsnWindow.Resize(m_psnOooThreshold);
    m_recvPsnWindow.Reset(m_psnRecvNxt);
}

/**
 * @brief Receive Data Packets
 * @param tpack Transport acknowledgment message to process
 * TP接收到一个数据包的时候，调用此函数处理，产生tpack
 */
void UbTransportChannel::RecvDataPacket(Ptr<Packet> p)
{
    if (p == nullptr) {
        NS_LOG_ERROR("Null packet received");
        return;
    }

    UbDatalinkPacketHeader pktHeader;
    UbTransactionHeader TaHeader;
    UbAckTransactionHeader AckTaHeader;
    UbTransportHeader TpHeader;
    UbCongestionExtTph CETPH;
    UbIpBasedNetworkHeader NetworkHeader;
    UdpHeader udpHeader;
    Ipv4Header ipv4Header;
    UbMAExtTah MAExtTaHeader;
    Ptr<Packet> ackp = Create<Packet>(0);
    p->RemoveHeader(pktHeader);
    p->RemoveHeader(NetworkHeader);
    p->RemoveHeader(ipv4Header);
    p->RemoveHeader(udpHeader);
    p->RemoveHeader(TpHeader);
    p->RemoveHeader(TaHeader); // 处理接收包信息
    p->RemoveHeader(MAExtTaHeader);
    const uint32_t payloadBytes = p->GetSize();
    const uint32_t logicalBytes = MAExtTaHeader.GetLength();
    uint64_t psn = TpHeader.GetPsn();
    NS_LOG_DEBUG("[Transport channel] Recv packet."
                  << " PacketUid: "  << p->GetUid()
                  << " Tpn: " << m_tpn
                  << " Psn: " << psn
                  << " PacketType: Packet"
                  << " Src: " << m_src
                  << " Dst: " << m_dest
                  << " PacketSize: " << p->GetSize());
    UbFlowTag flowTag;
    p->PeekPacketTag(flowTag);
    std::vector<Ptr<UbWqeSegment>> completedTaUnits;
    if (m_pktTraceEnabled) {
        UbPacketTraceTag traceTag;
        p->PeekPacketTag(traceTag);
        TpRecvNotify({p->GetUid(), psn, m_dest, m_src, m_dstTpn, m_tpn,
                     PacketType::PACKET, p->GetSize(), flowTag.GetFlowId(), traceTag});
    }
    ackp->AddPacketTag(flowTag);
    if (TpHeader.GetLastPacket()) {
        // 尾包被接收
        LastPacketReceivesNotify(m_nodeId, TpHeader.GetSrcTpn(), TpHeader.GetDestTpn(), TpHeader.GetTpMsn(),
            TpHeader.GetPsn(), m_dport);
    }
    if (IsRepeatPacket(psn)) {
        TpHeader.SetTPOpcode(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH); // 包类型变为ack
        TpHeader.SetPsn(m_psnRecvNxt - 1);
        TpHeader.SetSrcTpn(m_tpn);
        TpHeader.SetDestTpn(m_dstTpn);
        CETPH.SetAckSequence(m_psnRecvNxt - 1);
        CETPH.SetLocation(NetworkHeader.GetLocation());
        CETPH.SetI(NetworkHeader.GetI());
        CETPH.SetC(NetworkHeader.GetC());
        CETPH.SetHint(NetworkHeader.GetHint());
        AckTaHeader.SetTaOpcode(TaOpcode::TA_OPCODE_TRANSACTION_ACK);
        AckTaHeader.SetIniTaSsn(TaHeader.GetIniTaSsn());
        AckTaHeader.SetIniRcId(TaHeader.GetIniRcId());
        ackp->AddHeader(AckTaHeader);
        if (TpHeader.GetTPOpcode() == static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITH_CETPH)) {
            ackp->AddHeader(CETPH);
        }
        ackp->AddHeader(TpHeader);
        ackp->AddHeader(udpHeader);
        UbPort::AddIpv4Header(ackp, ipv4Header.GetDestination(), ipv4Header.GetSource());
        ackp->AddHeader(NetworkHeader);
        UbDataLink::GenPacketHeader(ackp, false, true, pktHeader.GetCreditTargetVL(), pktHeader.GetPacketVL(),
            0, 1, UbDatalinkHeaderConfig::PACKET_IPV4);
        if (m_ackQ.empty()) {
            m_headArrivalTime = Simulator::Now();
        }
        m_ackQ.push(ackp); // 将ack放入队列
        NS_LOG_DEBUG("[Transport channel] Send ack. "
                  << " PacketUid: "  << ackp->GetUid()
                  << " Tpn: " << m_tpn
                  << " Psn: " << m_psnRecvNxt - 1
                  << " PacketType: Ack"
                  << " Src: " << m_src
                  << " Dst: " << m_dest
                  << " PacketSize: " << ackp->GetSize());
        Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
        port->TriggerTransmit(); // 触发发送
        return;
    }
    uint32_t psnStart = 0;
    uint32_t psnEnd = 0;
    if (psn >= m_psnRecvNxt) {
        // psn=m_psnRecvNxt代表顺序收到包，psn>m_psnRecvNxt代表乱序
        if (!SetBitmap(psn)) {
            // 超出bitmap允许的乱序规格了,先空着
            NS_LOG_WARN("Over Out-of-Order! Max Out-of-Order :" << m_psnOooThreshold);
            return;
        }
        // 记录包号和size
        m_congestionCtrl->OnReceiverDataPacketReceived(psn, payloadBytes, NetworkHeader);
        m_bufferedInboundPackets[psn] = {TpHeader,
                                         TaHeader,
                                         logicalBytes,
                                         payloadBytes,
                                         flowTag.GetFlowId()};
        if (psn > m_psnRecvNxt) {
            NS_LOG_DEBUG("Out-of-Order Packet,tpn:{" << m_tpn << "} psn:{" << psn
                        << "} expectedPsn:{" << m_psnRecvNxt << "}");
            return; // 未开启sack的情况下乱序包不用回复ack，只用记录了bitmap
        }
        uint32_t oldRecvNxt = m_psnRecvNxt;
        while (m_psnRecvNxt < oldRecvNxt + m_psnOooThreshold) {
            uint32_t currentBitIndex = m_psnRecvNxt - oldRecvNxt;
            if (currentBitIndex < m_recvPsnWindow.GetWindowSize() &&
                m_recvPsnWindow.Contains(m_psnRecvNxt)) {
                auto bufferedIt = m_bufferedInboundPackets.find(m_psnRecvNxt);
                if (bufferedIt == m_bufferedInboundPackets.end()) {
                    NS_LOG_WARN("Missing buffered inbound packet for contiguous psn " << m_psnRecvNxt
                                << " on tpn " << m_tpn);
                    break;
                }
                Ptr<UbWqeSegment> completedTaUnit =
                    TrackInboundTaPacket(bufferedIt->second.tpHeader,
                                         bufferedIt->second.taHeader,
                                         bufferedIt->second.logicalBytes,
                                         bufferedIt->second.payloadBytes,
                                         bufferedIt->second.taskId);
                if (completedTaUnit != nullptr) {
                    completedTaUnits.push_back(completedTaUnit);
                }
                m_bufferedInboundPackets.erase(bufferedIt);
                m_psnRecvNxt++;
            } else if (currentBitIndex) {
                break; // 遇到未确认的分段，停止
            }
        }
        // 如果 m_psnRecvNxt 有更新，需要清理 bitset
        if (m_psnRecvNxt > oldRecvNxt) {
            NS_LOG_DEBUG("Updated m_psnRecvNxt from " << oldRecvNxt
                        << " to " << m_psnRecvNxt);
            // 手动右移 bitset
            uint32_t shiftCount = m_psnRecvNxt - oldRecvNxt;
            RightShiftBitset(shiftCount);
            psnStart = oldRecvNxt;
            psnEnd = m_psnRecvNxt;
        }
    }
    NS_LOG_DEBUG("RecvDataPacket ready to send ack psn: " << (m_psnRecvNxt - 1) << " node: " << m_src);
    TpHeader.SetTPOpcode(m_congestionCtrl->GetAckOpcode());
    CETPH = m_congestionCtrl->OnReceiverPrepareAckCongestionHeader(psnStart, psnEnd);
    TpHeader.SetPsn(m_psnRecvNxt - 1);
    TpHeader.SetSrcTpn(m_tpn);
    TpHeader.SetDestTpn(m_dstTpn);
    AckTaHeader.SetTaOpcode(TaOpcode::TA_OPCODE_TRANSACTION_ACK);
    AckTaHeader.SetIniTaSsn(TaHeader.GetIniTaSsn());
    AckTaHeader.SetIniRcId(TaHeader.GetIniRcId());
    ackp->AddHeader(AckTaHeader);
    if (TpHeader.GetTPOpcode() == static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITH_CETPH)) {
        ackp->AddHeader(CETPH);
    }
    ackp->AddHeader(TpHeader);
    ackp->AddHeader(udpHeader);
    UbPort::AddIpv4Header(ackp, ipv4Header.GetDestination(), ipv4Header.GetSource());
    ackp->AddHeader(NetworkHeader);
    UbDataLink::GenPacketHeader(ackp, false, true, pktHeader.GetCreditTargetVL(), pktHeader.GetPacketVL(),
        0, 1, UbDatalinkHeaderConfig::PACKET_IPV4);
    if (m_ackQ.empty()) {
        m_headArrivalTime = Simulator::Now();
    }
    m_ackQ.push(ackp); // 将ack放入队列
    NS_LOG_DEBUG("[Transport channel] Send ack. "
                  << " PacketUid: "  << ackp->GetUid()
                  << " Tpn: " << m_tpn
                  << " Psn: " << m_psnRecvNxt - 1
                  << " PacketType: Ack"
                  << " Src: " << m_src
                  << " Dst: " << m_dest
                  << " PacketSize: " << ackp->GetSize());
    Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
    port->TriggerTransmit(); // 触发发送
    for (const Ptr<UbWqeSegment>& completedTaUnit : completedTaUnits)
    {
        if (completedTaUnit == nullptr) {
            continue;
        }
        GetTransaction()->HandleInboundTaUnit(m_tpn, completedTaUnit);
        WqeSegmentCompletesNotify(m_nodeId, completedTaUnit->GetTaskId(), completedTaUnit->GetTaSsn());
    }
}

void UbTransportChannel::ReTxTimeout()
{
    m_retransAttemptsLeft--;
    uint64_t rto = m_rto.GetNanoSeconds();
    rto = rto << m_retransExponentFactor; // 下一次超时重传变成Base_time * 2^(N*Times)
    m_rto = ns3::NanoSeconds(rto);
    NS_ASSERT_MSG (m_retransAttemptsLeft > 0, "Avaliable retransmission attempts exhausted.");
    // 重传逻辑
    m_psnSndNxt = m_psnSndUna; // 将发送指针回退到未确认的包
    // 重置已发送字节数
    for (size_t i = 0; i < m_wqeSegmentVector.size(); ++i) {
        Ptr<UbWqeSegment> currentSegment = m_wqeSegmentVector[i];
        if (currentSegment->GetPsnStart() <= m_psnSndUna) {
            if (currentSegment->GetPsnStart() + currentSegment->GetPsnSize() > m_psnSndUna) {
                uint32_t  resetSentBytes =  (m_psnSndUna - currentSegment->GetPsnStart()) * UB_MTU_BYTE;
                currentSegment->ResetSentBytes(resetSentBytes); // 重置已发送字节数到未被确认的地方
                NS_LOG_INFO("Packet Retransmits,taskId: " << currentSegment->GetTaskId() << " psn: " << m_psnSndNxt);
            }
        } else {
            currentSegment->ResetSentBytes(); // 整个wqeSegment都未被确认，全重置已发送字节数
            NS_LOG_INFO("Packet Retransmits,taskId: " << currentSegment->GetTaskId() << " psn: " << m_psnSndNxt);
        }
    }

    // 重新发送
    m_retransEvent = Simulator::Schedule(m_rto, &UbTransportChannel::ReTxTimeout, this);
    Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
    port->TriggerTransmit(); // 触发发送
}

/**
 * @brief Get current queue size
 * @return Current number of WQEs in queue
 */
uint32_t UbTransportChannel::GetCurrentSqSize() const
{
    return m_wqeSegmentVector.size();
}

uint32_t UbTransportChannel::GetActiveSendSegmentCount() const
{
    uint32_t activeCount = 0;
    for (const Ptr<UbWqeSegment>& segment : m_wqeSegmentVector) {
        if (segment != nullptr && !segment->IsSentCompleted()) {
            ++activeCount;
        }
    }
    return activeCount;
}

bool
UbTransportChannel::IsWqeSegmentLimited() const
{
    if (GetCurrentSqSize() >= m_maxQueueSize) {
        return true;
    }
    return false;
}

// 相当于发送窗口，应该与拥塞窗口取小值。目前尚未使用。
bool UbTransportChannel::IsInflightLimited() const
{
    if (m_psnSndNxt - m_psnSndUna >= m_maxInflightPacketSize) {
        return true;
    }
    return false;
}

/**
 * @brief Move right Bitset
 * @return
*/
void UbTransportChannel::RightShiftBitset(uint32_t shiftCount)
{
    (void)shiftCount;
    m_recvPsnWindow.AdvanceContiguous();
}

/**
  * @brief Set bitmap
  * @return Set the PSN position to 1
*/
bool UbTransportChannel::SetBitmap(uint64_t psn)
{
    return m_recvPsnWindow.Mark(psn);
}

/**
  * @brief IsRepeatPacket
  * @return
*/
bool UbTransportChannel::IsRepeatPacket(uint64_t psn)
{
    if (psn < m_psnRecvNxt) {
        return true;
    }
    if (psn >= m_recvPsnWindow.GetBase() + m_recvPsnWindow.GetWindowSize()) {
        return false;
    }
    return m_recvPsnWindow.Contains(psn);
}

void UbTransportChannel::WqeSegmentTriggerPortTransmit(Ptr<UbWqeSegment> segment)
{
    WqeSegmentSendsNotify(m_nodeId, segment->GetTaskId(), segment->GetTaSsn());
    Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
    port->TriggerTransmit(); // 触发发送
}

Ptr<UbTransaction> UbTransportChannel::GetTransaction()
{
    return NodeList::GetNode(m_nodeId)->GetObject<UbController>()->GetUbTransaction();
}

void UbTransportChannel::ApplyNextWqeSegment()
{
    GetTransaction()->ApplyScheduleWqeSegment(this);
}

bool UbTransportChannel::IsEmpty()
{
    if (!m_cnpQ.empty()) {
        return false;
    }
    if (!m_ackQ.empty()) {
        return false;
    }
    if (m_wqeSegmentVector.empty()) {
        return true;
    }
    return m_psnSndNxt >= m_tpPsnCnt;
}

bool UbTransportChannel::IsLimited()
{
    if (!m_cnpQ.empty()) {
        return false;
    }
    if (!m_ackQ.empty()) {
        return false;
    }
    if (IsInflightLimited()) {
        m_sendWindowLimited = true;
        NS_LOG_DEBUG("Full Send Window");
        return true;
    }
    if (m_congestionCtrl->IsCcLimited(UB_MTU_BYTE)) {
        m_sendWindowLimited = true;
        return true;
    }
    return false;
}

IngressQueueType UbTransportChannel::GetIngressQueueType()
{
    return m_ingressQueueType;
}

void UbTransportChannel::FirstPacketSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t mTpn,
                                                uint32_t mDstTpn, uint32_t tpMsn, uint32_t mPsnSndNxt, uint32_t mSport)
{
    m_traceFirstPacketSendsNotify(nodeId, taskId, mTpn, mDstTpn, tpMsn, mPsnSndNxt, mSport);
}

void UbTransportChannel::LastPacketSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t mTpn,
                                               uint32_t mDstTpn, uint32_t tpMsn, uint32_t mPsnSndNxt, uint32_t mSport)
{
    m_traceLastPacketSendsNotify(nodeId, taskId, mTpn, mDstTpn, tpMsn, mPsnSndNxt, mSport);
}

void UbTransportChannel::LastPacketACKsNotify(uint32_t nodeId, uint32_t taskId, uint32_t mTpn,
                                              uint32_t mDstTpn, uint32_t tpMsn, uint32_t psn, uint32_t mSport)
{
    m_traceLastPacketACKsNotify(nodeId, taskId, mTpn, mDstTpn, tpMsn, psn, mSport);
}

void UbTransportChannel::LastPacketReceivesNotify(uint32_t nodeId, uint32_t srcTpn, uint32_t dstTpn,
                                                  uint32_t tpMsn, uint32_t psn, uint32_t mDport)
{
    m_traceLastPacketReceivesNotify(nodeId, srcTpn, dstTpn, tpMsn, psn, mDport);
}

void UbTransportChannel::WqeSegmentSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t taSsn)
{
    m_traceWqeSegmentSendsNotify(nodeId, taskId, taSsn);
}

void UbTransportChannel::WqeSegmentCompletesNotify(uint32_t nodeId, uint32_t taskId, uint32_t taSsn)
{
    m_traceWqeSegmentCompletesNotify(nodeId, taskId, taSsn);
}

void UbTransportChannel::TpRecvNotify(UbTpRecvTraceData data)
{
    m_tpRecvNotify(data);
}

// ==========================================================================
// UbTransportGroup Implementation
// ==========================================================================

TypeId UbTransportGroup::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbTransportGroup")
        .SetParent<Object>()
        .SetGroupName("UnifiedBus")
        .AddConstructor<UbTransportGroup>();
    return tid;
}

UbTransportGroup::UbTransportGroup()
{
}

UbTransportGroup::~UbTransportGroup()
{
}

} // namespace ns3
