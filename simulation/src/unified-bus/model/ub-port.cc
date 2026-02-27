// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-port.h"
#include "ns3/ub-link.h"
#include "ns3/log.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-caqm.h"
#include "ns3/ub-tag.h"
using namespace utils;

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(UbPort);
NS_LOG_COMPONENT_DEFINE("UbPort");

constexpr long DEFAULT_PFC_UP_THLD = 1677721;
constexpr long DEFAULT_PFC_LOW_THLD = 1342176;

/*********************
 * UbEgressQueue
 ********************/

// UbEgressQueue
TypeId UbEgressQueue::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbEgressQueue")
            .SetParent<Object>()
            .AddTraceSource(
            "UbEnqueue",
            "Enqueue a packet in the UbEgressQueue.",
            MakeTraceSourceAccessor(&UbEgressQueue::m_traceUbEnqueue),
            "ns3::UbEgressQueue::UbEnqueue")
            .AddTraceSource(
            "UbDequeue",
            "Dequeue a packet in the UbEgressQueue.",
            MakeTraceSourceAccessor(&UbEgressQueue::m_traceUbDequeue),
            "ns3::UbEgressQueue::UbDequeue")
            .AddAttribute(
            "MaxEgressBytes",
            "The maximum number of bytes accepted by this egress queue.",
            UintegerValue(512 * 1024),  // 512KB default (about 128 max-size packets of 4096 bytes)
            MakeUintegerAccessor(&UbEgressQueue::m_maxEgressBytes),
            MakeUintegerChecker<uint64_t>());
    return tid;
}

UbEgressQueue::UbEgressQueue()
{
}

bool UbEgressQueue::DoEnqueue(PacketEntry packetEntry)
{
    NS_LOG_FUNCTION (this);
    
    auto [inPortId, priority, pkt] = packetEntry;
    uint32_t pktSize = pkt->GetSize();

    // Check byte limit
    if (m_currentBytes + pktSize > m_maxEgressBytes) {
        NS_LOG_WARN ("Buffer full, packet dropped: " 
                     << (m_currentBytes + pktSize) << " bytes > limit " << m_maxEgressBytes 
                     << " bytes. Packet dropped (inPort=" << inPortId 
                     << " priority=" << (uint32_t)priority << ")");
        return false;
    }
    
    m_egressQ.push(packetEntry);
    m_currentBytes += pktSize;  // 更新字节统计

    NS_LOG_LOGIC ("[UbEgressQueue DoEnqueue] Egress Queue size: " << m_egressQ.size () 
                  << " bytes: " << m_currentBytes << " (+" << pktSize << ")");

    return true;
}

PacketEntry UbEgressQueue::Peekqueue()
{
    NS_LOG_FUNCTION (this);
    if (m_egressQ.empty()) {
        NS_LOG_LOGIC ("Queue empty");
        return std::make_tuple(0, 0, nullptr);
    }
    auto [inPortId, priority, pkt] = m_egressQ.front();
    return std::make_tuple(inPortId, priority, pkt);
}

PacketEntry UbEgressQueue::DoDequeue()
{
    NS_LOG_FUNCTION (this);

    if (m_egressQ.empty()) {
        NS_LOG_LOGIC ("Queue empty");
        return std::make_tuple(0, 0, nullptr);
    }

    auto packetEntry = m_egressQ.front ();
    m_egressQ.pop();
    
    auto [inPortId, priority, pkt] = packetEntry;
    uint32_t pktSize = pkt->GetSize();
    m_currentBytes -= pktSize;  // 更新字节统计

    NS_LOG_LOGIC ("[UbEgressQueue DoDequeue] Egress Queue size: " << m_egressQ.size ()
                  << " bytes: " << m_currentBytes << " (-" << pktSize << ")");

    return packetEntry;
}

bool UbEgressQueue::IsEmpty()
{
    return m_egressQ.empty();
}

/******************
 * UbPort
 *****************/
std::unordered_map<UbNodeType_t, std::string>
UbPort::g_node_type_map = {{UB_SWITCH, "SWITCH"},
                           {UB_DEVICE, "HOST"}};

TypeId UbPort::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbPort")
            .SetParent<PointToPointNetDevice>()
            .AddConstructor<UbPort>()
            .AddAttribute("UbDataRate",
                          "The default data rate for ub link",
                          DataRateValue(DataRate ("400Gbps")),
                          MakeDataRateAccessor(&UbPort::m_bps),
                          MakeDataRateChecker())
            .AddAttribute("UbInterframeGap",
                          "The time to wait between packet transmissions",
                          TimeValue(Seconds (0.0)),
                          MakeTimeAccessor(&UbPort::m_tInterframeGap),
                          MakeTimeChecker())
            .AddAttribute("CbfcFlitLenByte",
                          "Cbfc flit len in Byte",
                          UintegerValue(20),
                          MakeUintegerAccessor(&UbPort::m_cbfcFlitLen),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("CbfcFlitsPerCell",
                          "Cbfc flits per cell",
                          UintegerValue(4),
                          MakeUintegerAccessor(&UbPort::m_cbfcFlitsPerCell),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("CbfcRetCellGrainDataPacket",
                          "Cbfc return cell grain data packet",
                          UintegerValue(2),
                          MakeUintegerAccessor(&UbPort::m_cbfcRetCellGrainDataPacket),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("CbfcRetCellGrainControlPacket",
                          "Cbfc return cell grain control packet",
                          UintegerValue(2),
                          MakeUintegerAccessor(&UbPort::m_cbfcRetCellGrainControlPacket),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("CbfcInitCreditCell",
                          "According to the configuration of the receive buffer at the connected node port, "
                          "the unit is cell",
                          UintegerValue(1024),
                          MakeIntegerAccessor(&UbPort::m_cbfcPortTxfree),
                          MakeIntegerChecker<int32_t>())
            .AddAttribute("CbfcSharedInitCreditCell",
                          "Cbfc shared credit mode",
                          UintegerValue(8192),
                          MakeIntegerAccessor(&UbPort::m_cbfcSharedInitCells),
                          MakeIntegerChecker<int32_t>())
            .AddAttribute("PfcUpThld",
                          "Pfc up thld",
                          IntegerValue(DEFAULT_PFC_UP_THLD),
                          MakeIntegerAccessor(&UbPort::m_pfcUpThld),
                          MakeIntegerChecker<int32_t>())
            .AddAttribute("PfcLowThld",
                          "Pfc low thld",
                          IntegerValue(DEFAULT_PFC_LOW_THLD),
                          MakeIntegerAccessor(&UbPort::m_pfcLowThld),
                          MakeIntegerChecker<int32_t>())
            .AddTraceSource("PortTxNotify",
                            "Port Tx",
                            MakeTraceSourceAccessor(&UbPort::m_tracePortTxNotify),
                            "ns3::UbPort::PortTxNotify")
            .AddTraceSource("PortRxNotify",
                            "Port Rx",
                            MakeTraceSourceAccessor(&UbPort::m_tracePortRxNotify),
                            "ns3::UbPort::PortRxNotify")
            .AddTraceSource("PktRcvNotify",
                            "Notify after receiving the data packet.",
                            MakeTraceSourceAccessor(&UbPort::m_tracePktRcvNotify),
                            "ns3::UbPort::PktRcvNotify")
            .AddTraceSource("TraComEventNotify",
                            "Transmit complete event.",
                            MakeTraceSourceAccessor(&UbPort::m_traceTraComEventNotify),
                            "ns3::UbPort::TraComEventNotify");
    return tid;
}

UbPort::UbPort()
{
    NS_LOG_FUNCTION(this);
    m_ubEQ = CreateObject<UbEgressQueue>();
    m_sendState = SendState::READY;
    m_txBytes = 0;
    BooleanValue val;
    if (GlobalValue::GetValueByNameFailSafe("UB_RECORD_PKT_TRACE", val)) {
        GlobalValue::GetValueByName("UB_RECORD_PKT_TRACE", val);
        m_pktTraceEnabled = val.Get();
    } else {
        m_pktTraceEnabled = false;
    }
}

UbPort::~UbPort()
{
    NS_LOG_FUNCTION(this);
}

void UbPort::SetIfIndex(const uint32_t portId)
{
    m_portId = portId;
}

uint32_t UbPort::GetIfIndex() const
{
    return m_portId;
}

void UbPort::SetSendState(SendState state)
{
    m_sendState = state;
}

void UbPort::CreateAndInitFc(FcType type)
{
    switch (type) {
        case FcType::CBFC:
            m_flowControl = CreateObject<UbCbfc>();
            if (m_flowControl == nullptr) {
                NS_FATAL_ERROR("Failed to create UbCbfc object for port " << m_portId);
            } else {
                auto flowControl = DynamicCast<UbCbfc>(m_flowControl);
                flowControl->Init(m_cbfcFlitLen, m_cbfcFlitsPerCell, m_cbfcRetCellGrainDataPacket,
                    m_cbfcRetCellGrainControlPacket, m_cbfcPortTxfree, GetNode()->GetId(), m_portId);
                NS_LOG_DEBUG("[UbPort CreateAndInitFc] flowControl Cbfc Init");
            }
            break;
        case FcType::CBFC_SHARED_CRD:
            m_flowControl = CreateObject<UbCbfcSharedCredit>();
            if (m_flowControl == nullptr) {
                NS_FATAL_ERROR("Failed to create UbCbfcSharedCredit object for port " << m_portId);
            } else {
                auto flowControl = DynamicCast<UbCbfcSharedCredit>(m_flowControl);
                const int32_t reservedPerVlCells = m_cbfcPortTxfree;
                const int32_t sharedInitCells = m_cbfcSharedInitCells;

                flowControl->Init(m_cbfcFlitLen, m_cbfcFlitsPerCell,
                                m_cbfcRetCellGrainDataPacket, m_cbfcRetCellGrainControlPacket,
                                reservedPerVlCells, sharedInitCells,
                                GetNode()->GetId(), m_portId);

                NS_LOG_DEBUG("[UbPort CreateAndInitFc] flowControl CbfcSharedMode Init");
            }
            break;
        case FcType::PFC:
            m_flowControl = CreateObject<UbPfc>();
            if (m_flowControl == nullptr) {
                NS_FATAL_ERROR("Failed to create UbPfc object for port " << m_portId);
            } else {
                auto flowControl = DynamicCast<UbPfc>(m_flowControl);
                flowControl->Init(m_pfcUpThld, m_pfcLowThld, GetNode()->GetId(), m_portId);
                IntegerValue val;
                g_ub_vl_num.GetValue(val);
                int ubVlNum = val.Get();
                m_revQueueSize.resize(ubVlNum, 0);
                NS_LOG_DEBUG("[UbPort CreateAndInitFc] flowControl Pfc Init");
            }
            break;
        case FcType::NONE:
            // No flow control: base class with no-op implementation
            m_flowControl = CreateObject<UbFlowControl>();
            if (m_flowControl == nullptr) {
                NS_FATAL_ERROR("Failed to create UbFlowControl object for port " << m_portId);
            }
            NS_LOG_DEBUG("[UbPort CreateAndInitFc] Default flow control (no-op) Init");
            break;
        default:
            NS_FATAL_ERROR("Unknown flow control type for port " << m_portId);
    }
}

void UbPort::TransmitComplete()
{
    NS_LOG_FUNCTION(this);
    
    NS_ASSERT_MSG(m_currentPkt, "UbPort::TransmitComplete(): m_currentPkt zero");

    NS_LOG_DEBUG("[UbPort TransmitComplete] complete at: "
        << " NodeId: " << GetNode()->GetId()
        << " PortId: " << GetIfIndex()
        << " PacketUid: " << m_currentPkt->GetUid());

    m_sendState = SendState::READY;
    m_currentPkt = nullptr;
    m_currentInPortId = 0;
    m_currentPriority = 0;
    Simulator::ScheduleNow(&UbPort::TriggerTransmit, this);
}

void UbPort::DequeuePacket(void)
{
    NS_ASSERT_MSG(!m_ubEQ->IsEmpty(), "No packets can be sent! NodeId: "<< GetNode()->GetId()
        << " PortId: " << m_portId);
    m_sendState = SendState::BUSY;

    auto [inPortId, priority, packet] = m_ubEQ->DoDequeue();

    m_currentPkt = packet;
    m_currentInPortId = inPortId;
    m_currentPriority = priority;
    if (m_ubEQ->IsEmpty()) {
        // Switch allocation when port sendding packet.
        auto allocator = GetNode()->GetObject<UbSwitch>()->GetAllocator();
        Simulator::ScheduleNow(&UbSwitchAllocator::TriggerAllocator, allocator, this);
    }

    if (!m_faultCallBack.IsNull()) {
        m_faultCallBack(packet, GetNode()->GetId(), m_portId, this);
        return;
    }
    TransmitPacket(packet, Time(0));
    return;
}

void UbPort::TransmitPacket(Ptr<Packet> packet, Time delay)
{
    PortTxNotify(GetNode()->GetId(), m_portId, packet->GetSize());
    NS_LOG_DEBUG("[UbPort send] nodetype: " << g_node_type_map[GetNode()->GetObject<UbSwitch>()->GetNodeType()]
        << " NodeId: " << GetNode()->GetId() << " PortId: " << m_portId << " send to:"
        << " NodeId: " << m_channel->GetDestination(this)->GetNode()->GetId()
        << " PortId: " << m_channel->GetDestination(this)->GetIfIndex()
        << " PacketUid: " << packet->GetUid());
    if (m_pktTraceEnabled) {
        UbPacketTraceTag tag;
        packet->RemovePacketTag(tag);
        tag.AddPortSendTrace(GetNode()->GetId(), m_portId, Simulator::Now().GetNanoSeconds());
        packet->AddPacketTag(tag);
    }

    Time txTime = m_bps.CalculateBytesTxTime(packet->GetSize()) + delay;
    Time txCompleteTime = txTime + m_tInterframeGap;

    // Try to start transmission on the channel first. Only if it succeeds
    // should we schedule the TransmitComplete event and notify traces.
    bool result = m_channel->TransmitStart(packet, this, txTime);
    if (result == false) {
        NS_LOG_WARN("Channel transmission failed! Packet dropped at Node " << GetNode()->GetId() << " Port " << m_portId);
        // Reset port state so it won't remain BUSY and try next packet
        m_sendState = SendState::READY;
        m_currentPkt = nullptr;
        m_currentInPortId = 0;
        m_currentPriority = 0;
        Simulator::ScheduleNow(&UbPort::TriggerTransmit, this);
        return;
    }

    // Transmission started successfully: notify and schedule completion
    TraComEventNotify(packet, txCompleteTime);
    Simulator::Schedule(txCompleteTime, &UbPort::TransmitComplete, this);
    NS_LOG_DEBUG("[UbFc DequeueAndTransmit] will send pkt size: " << packet->GetSize());
    UpdateTxBytes(packet->GetSize());

    return;
}

void UbPort::Receive(Ptr<Packet> packet)
{
    NS_LOG_DEBUG("[UbPort recv] nodetype: " << g_node_type_map[GetNode()->GetObject<UbSwitch>()->GetNodeType()]
        << " NodeId: " << GetNode()->GetId()
        << " PortId: " << GetIfIndex()
        << " recv from:"
        << " NodeId: " << m_channel->GetDestination(this)->GetNode()->GetId()
        << " PortId: " << m_channel->GetDestination(this)->GetIfIndex()
        << " PacketUid: "<< packet->GetUid());
    if (m_pktTraceEnabled) {
        UbPacketTraceTag tag;
        packet->RemovePacketTag(tag);
        tag.AddPortRecvTrace(GetNode()->GetId(), m_portId, Simulator::Now().GetNanoSeconds());
        packet->AddPacketTag(tag);
    }
    PortRxNotify(GetNode()->GetId(), m_portId, packet->GetSize());
    GetNode()->GetObject<UbSwitch>()->SwitchHandlePacket(this, packet);

    return;
}

bool UbPort::Attach(Ptr<UbLink> ch)
{
    NS_LOG_FUNCTION(this << &ch);
    m_channel = ch;
    m_channel->Attach(this);
    NotifyLinkUp();
    return true;
}

void UbPort::NotifyLinkUp (void)
{
    NS_LOG_FUNCTION(this);
    m_linkUp = true;
}


uint32_t UbPort::ParseHeader(Ptr<Packet> p, Header& h)
{
    return p->RemoveHeader(h);
}

void UbPort::AddUdpHeader(Ptr<Packet> p, Ptr<UbTransportChannel> tp)
{
    UdpHeader udpHeader;
    udpHeader.SetDestinationPort(tp->GetDport());
    udpHeader.SetSourcePort(tp->GetUdpSport());
    p->AddHeader(udpHeader);
}

void UbPort::AddUdpHeader(Ptr<Packet> p, uint16_t sPort, uint16_t dPort)
{
    UdpHeader udpHeader;
    udpHeader.SetDestinationPort(dPort);
    udpHeader.SetSourcePort(sPort);
    p->AddHeader(udpHeader);
}

void UbPort::AddIpv4Header(Ptr<Packet> p, Ptr<UbTransportChannel> tp)
{
    Ipv4Header ipHeader;
    ipHeader.SetSource(tp->GetSip());
    ipHeader.SetDestination(tp->GetDip());
    ipHeader.SetProtocol(0x11);
    ipHeader.SetPayloadSize(p->GetSize());
    ipHeader.SetTtl(TIME_TO_LIVE);
    ipHeader.SetTos(0);
    p->AddHeader(ipHeader);
}

void UbPort::AddIpv4Header(Ptr<Packet> p, Ipv4Address sIp, Ipv4Address dIp)
{
    Ipv4Header ipHeader;
    ipHeader.SetSource(sIp);
    ipHeader.SetDestination(dIp);
    ipHeader.SetProtocol(0x11);
    ipHeader.SetPayloadSize(p->GetSize());
    ipHeader.SetTtl(TIME_TO_LIVE);
    ipHeader.SetTos(0);
    p->AddHeader(ipHeader);
}

void UbPort::AddNetHeader(Ptr<Packet> p)
{
    UbNetworkHeader netHeader;
    p->AddHeader(netHeader);
}

Ptr<Channel> UbPort::GetChannel(void) const
{
    return m_channel;
}

bool UbPort::IsUb(void) const
{
    return true;
}

void UbPort::TriggerTransmit()
{
    NS_LOG_DEBUG("[UbPort TriggerTransmit] nodeId: " << GetNode()->GetId()
        << " portId: " << GetIfIndex() <<" TriggerTransmit...");
    if (!m_linkUp) {
        NS_LOG_DEBUG("[UbPort TriggerTransmit] m_linkUp");
        return;
    } // if link is down, return
    if (IsBusy()) {
        NS_LOG_DEBUG("[UbPort TriggerTransmit] SendState::BUSY");
        return; // Quit if channel busy
    }
    if (m_ubEQ->IsEmpty()) {
        NS_LOG_DEBUG("[UbPort TriggerTransmit] trigger Allocator");
        auto allocator = GetNode()->GetObject<UbSwitch>()->GetAllocator();
        Simulator::ScheduleNow(&UbSwitchAllocator::TriggerAllocator, allocator, this);
        return;
    }
    DequeuePacket();
}

void UbPort::NotifyAllocationFinish()
{
    if (IsBusy()) {
        return;
    }
    if (m_ubEQ->IsEmpty()) {
        // No Packet to send
        return;
    }
    DequeuePacket();
}

Ptr<UbEgressQueue> UbPort::GetUbQueue()
{
    return m_ubEQ;
}

void UbPort::PktRcvNotify(Ptr<Packet> p)
{
    m_tracePktRcvNotify(p);
}

void UbPort::TraComEventNotify(Ptr<Packet> p, Time t)
{
    m_traceTraComEventNotify(p, t);
}

DataRate UbPort::GetDataRate()
{
    return m_bps;
}

Time UbPort::GetInterframeGap()
{
    return m_tInterframeGap;
}

void UbPort::SetCredits(int index, uint8_t value)
{ // 设置用于恢复的信用证值
    m_credits[index] = value;
}

void UbPort::ResetCredits()
{ // 设置用于恢复的信用证值
    for (uint8_t index = 0; index < qCnt; index++) {
        m_credits[index] = 0;
    }
}

uint8_t UbPort::GetCredits(int index)
{ // 获取用于恢复的信用证值
    return m_credits[index];
}

void UbPort::UpdateTxBytes(uint64_t bytes)
{
    m_txBytes += bytes;
}

uint64_t UbPort::GetTxBytes()
{
    return m_txBytes;
}

bool UbPort::IsReady()
{
    return m_sendState == SendState::READY;
}

bool UbPort::IsBusy()
{
    return m_sendState == SendState::BUSY;
}

void UbPort::SetDataRate(DataRate bps)
{
    NS_LOG_DEBUG("port set data rate");
    m_bps = bps;
    Ptr<UbCongestionControl> congestionCtrl = GetNode()->GetObject<UbSwitch>()->GetCongestionCtrl();
    if (congestionCtrl->GetCongestionAlgo() == CAQM) {
        Ptr<UbSwitchCaqm> caqmSw = DynamicCast<UbSwitchCaqm>(congestionCtrl);
        caqmSw->SetDataRate(m_portId, bps);
    }
}

void UbPort::IncreaseRcvQueueSize(Ptr<Packet> p, Ptr<UbPort> port)
{
    uint32_t pktSize = p->GetSize();
    NS_LOG_DEBUG("[UbFc IncreaseRcvQueueSize] pktSize: " << pktSize << " PortId: " << port->GetIfIndex());
    UbDatalinkPacketHeader pktHeader;
    p->PeekHeader(pktHeader);
    uint8_t vlId = pktHeader.GetPacketVL();
    NS_LOG_DEBUG("[UbFc IncreaseRcvQueueSize] before m_revQueueSize[ "
        << (uint32_t)vlId << " ]: " << (uint32_t)m_revQueueSize[vlId]);
    m_revQueueSize[vlId] += pktSize;
    NS_LOG_DEBUG("[UbFc IncreaseRcvQueueSize] after m_revQueueSize[ "
        << (uint32_t)vlId << " ]: " << (uint32_t)m_revQueueSize[vlId]);
}

void UbPort::DecreaseRcvQueueSize(Ptr<Packet> p, uint32_t portId)
{
    Ptr<UbPort> port = DynamicCast<UbPort>(GetNode()->GetDevice(portId));
    uint32_t pktSize = p->GetSize();
    NS_LOG_DEBUG("[UbFc DecreaseRcvQueueSize] pktSize: " << pktSize << " PortId: " << port->GetIfIndex());
    UbDatalinkPacketHeader pktHeader;
    p->PeekHeader(pktHeader);
    uint8_t vlId = pktHeader.GetPacketVL();
    NS_LOG_DEBUG("[UbFc DecreaseRcvQueueSize] before m_revQueueSize[ " << (uint32_t)vlId << " ]: "
        << (uint32_t)m_revQueueSize[vlId]);
    port->m_revQueueSize[vlId] -= pktSize;
    NS_LOG_DEBUG("[UbFc DecreaseRcvQueueSize] after m_revQueueSize[ " << (uint32_t)vlId << " ]: "
        << (uint32_t)m_revQueueSize[vlId]);
}

uint32_t UbPort::GetRcvVlQueueSize(uint8_t vlId)
{
    return m_revQueueSize[vlId];
}

std::vector<uint32_t> UbPort::GetRcvQueueSize()
{
    return m_revQueueSize;
}

void UbPort::PortTxNotify(uint32_t nodeId, uint32_t mPortId, uint32_t size)
{
    m_tracePortTxNotify(nodeId, mPortId, size);
}

void UbPort::PortRxNotify(uint32_t nodeId, uint32_t mPortId, uint32_t size)
{
    m_tracePortRxNotify(nodeId, mPortId, size);
}

void UbPort::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_ubEQ = nullptr;
    m_channel = nullptr;
    m_currentPkt = nullptr;
    m_datalink = nullptr;
    m_flowControl = nullptr;
    m_revQueueSize.clear();
    NetDevice::DoDispose();
}

} // namespace ns3
