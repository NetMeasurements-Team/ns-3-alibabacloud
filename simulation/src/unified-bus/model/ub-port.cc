// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-port.h"
#include "ns3/ub-enum-accessor.h"
#include "ns3/ub-cbfc.h"
#include "ns3/ub-link.h"
#include "ns3/ub-pfc.h"
#include "ns3/log.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-caqm.h"
#include "ns3/ub-reliable-rerouting.h"
#include "ns3/ub-tag.h"
#include "ns3/ub-utils.h"
#include "ns3/ub-header.h"
#include "ns3/ub-datatype.h"
#ifdef NS3_MPI
#include "ns3/mpi-receiver.h"
#endif
using namespace utils;

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(UbPort);
NS_LOG_COMPONENT_DEFINE("UbPort");

constexpr long DEFAULT_PFC_UP_THLD = 819200;   // 800KB ≈ 78% of 1MB per-queue reserve
constexpr long DEFAULT_PFC_LOW_THLD = 655360;  // 640KB ≈ 80% of XOFF threshold

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
        UbUtils::RecordRuntimePacketDrop("egress queue buffer full");
        NS_LOG_WARN ("Buffer full, packet dropped: " 
                     << (m_currentBytes + pktSize) << " bytes > limit " << m_maxEgressBytes 
                     << " bytes. Packet dropped (inPort=" << inPortId 
                     << " priority=" << (uint32_t)priority << ")");
        return false;
    }
    
    m_egressQ.push(packetEntry);
    m_currentBytes += pktSize;  // 更新字节统计
    m_traceUbEnqueue(pkt, static_cast<uint32_t>(m_currentBytes));

    NS_LOG_LOGIC ("[UbEgressQueue DoEnqueue] Egress Queue size: " << m_egressQ.size () 
                  << " bytes: " << m_currentBytes << " (+" << pktSize << ")");

    return true;
}

bool UbEgressQueue::CanEnqueue(uint32_t packetBytes) const
{
    return m_currentBytes + packetBytes <= m_maxEgressBytes;
}

bool
UbPort::EnqueueToEgress(PacketEntry packetEntry)
{
    auto [inPortId, priority, packet] = packetEntry;
    (void)inPortId;
    (void)priority;
    const uint32_t pktSize = packet->GetSize();

    const bool enqueueOk = m_ubEQ->DoEnqueue(packetEntry);
    if (!enqueueOk)
    {
        return false;
    }

    Ptr<UbSwitch> ubSwitch = GetNode()->GetObject<UbSwitch>();
    if (ubSwitch != nullptr)
    {
        ubSwitch->GetQueueManager()->AddEgressBufferedBytes(pktSize);
    }
    if (m_rerouting != nullptr && m_rerouting->IsArmed())
    {
        m_rerouting->FailedEgressEnqueued(pktSize);
    }
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
    m_traceUbDequeue(pkt, static_cast<uint32_t>(m_currentBytes));

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
                          "Port data rate for Unified Bus link transmission.",
                          DataRateValue(DataRate ("400Gbps")),
                          MakeDataRateAccessor(&UbPort::m_bps),
                          MakeDataRateChecker())
            .AddAttribute("UbInterframeGap",
                          "Minimum idle time inserted between consecutive packet transmissions.",
                          TimeValue(Seconds (0.0)),
                          MakeTimeAccessor(&UbPort::m_tInterframeGap),
                          MakeTimeChecker())
            .AddAttribute("PacketRateControl",
                          "Data link layer packet rate control mode (NONE, STATIC, DYNAMIC), "
                          "UB Base Spec 2.1 §4.7. The sender fills No_Operation Blocks after small "
                          "DLLDPs so the SOP-to-SOP interval between consecutive DLLDPs meets "
                          "PACKET_MIN_INTERVAL, lowering the packet reception rate required of the "
                          "peer receiver. DYNAMIC implements the §4.7.3.2 deficit counters "
                          "directly. STATIC implements §4.7.3.1 per DLLDP instead of tracking the "
                          "previous SOP: every DLLDP is padded against its own "
                          "PACKET_MIN_INTERVAL, which agrees with the spec for back-to-back "
                          "DLLDPs and is the more conservative reading after an idle period. "
                          "DYNAMIC additionally needs the PacketRateAverage*/PacketRateDevice*/"
                          "PacketRateBurst* parameters, which carry no defaults: §4.7.1 takes them "
                          "from the peer receiver's Init Block, and the Table 4-9 note says \"It is "
                          "not allowed to send only part of the initialization parameters\", so a "
                          "partly configured DYNAMIC port aborts instead of assuming values. The "
                          "parameters describe the peer "
                          "receiver's capability and constrain this port's egress direction only; "
                          "the two link directions are configured independently. Per §4.7.1 the "
                          "mode is what the peer receiver advertised through the Init Block, so "
                          "setting it here describes the peer, not this port. NONE is a "
                          "simulator default rather than a specification-defined state: Table 4-9 "
                          "encodes PACKET_RATE_CONTROL_MODE as a single bit with no disabled value.",
                          MakeUbEnumValue(PacketRateControlMode::NONE),
                          MakeUbEnumAccessor(&UbPort::m_packetRateControl),
                          MakeUbEnumChecker(PacketRateControlMode::NONE, "NONE",
                                          PacketRateControlMode::STATIC, "STATIC",
                                          PacketRateControlMode::DYNAMIC, "DYNAMIC"))
            .AddAttribute("PacketRateMinIntervalFlits",
                          "Spec PACKET_MIN_INTERVAL (Table 4-9, Flit 3 bits 55:48, 8 bits): "
                          "\"Minimum interval allowed between two consecutive DLLDPs received by "
                          "the receiver. The value of this field indicates the minimum interval "
                          "between the current DLLDP (header flit included) and the next DLLDP "
                          "(header flit excluded). Unit: flit. This field does not require "
                          "negotiation and is directly set to the received value.\" Required when "
                          "PacketRateControl is STATIC or DYNAMIC; flits are sized by "
                          "CbfcFlitLenByte. 32 is a simulator default, not a specification "
                          "value: Table 4-9 carries this as a mandatory Init Block field the "
                          "local end takes from the peer, and specifies no fallback. 0 is still "
                          "rejected rather than read as \"no minimum\".",
                          UintegerValue(32),
                          MakeUintegerAccessor(&UbPort::m_packetRateMinIntervalFlits),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("PacketRateAveragePacketLengthFlits",
                          "Spec AVERAGE_PACKET_LENGTH (Table 4-9, Flit 4 bits 55:48, 8 bits): "
                          "\"Average packet length in flits. It is obtained by dividing the maximum "
                          "bandwidth of the data link layer by the maximum number of packets that "
                          "can be processed by the receiver in a unit time. The calculation result "
                          "is rounded up to the nearest flit.\" §4.7.2 works the example as "
                          "X * 120/128 * 6656/6696 divided by the device packet processing "
                          "capacity, for a physical layer rate X under RS(128,120) FEC. Used by "
                          "DYNAMIC only. No default: §4.7.1 takes it from the peer receiver's Init "
                          "Block, and the Table 4-9 note forbids sending only part of the "
                          "initialization parameters. DYNAMIC aborts until it is set.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&UbPort::m_packetRateAveragePacketLengthFlits),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("PacketRateAverageDuLengthFlits",
                          "Spec AVERAGE_DU_LENGTH (Table 4-9, Flit 4 bits 71:64, 8 bits): "
                          "\"Average data unit length, in flits. It is obtained by dividing the "
                          "maximum bandwidth of the data link layer at the receiver by the maximum "
                          "number of data units that can be processed by the receiver in a unit "
                          "time. The calculation result is rounded up to the nearest flit.\" §4.7.2 "
                          "notes that the device data unit processing capacity generally differs "
                          "from its packet processing capacity. §4.7.2's worked example divides by "
                          "the packet processing capacity in this case too, which reads as a typo "
                          "for the data unit processing capacity. Used by DYNAMIC only. No default: "
                          "§4.7.1 takes it from the peer receiver's Init Block, and the Table 4-9 "
                          "note forbids sending only part of the initialization parameters. DYNAMIC "
                          "aborts until it is set.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&UbPort::m_packetRateAverageDuLengthFlits),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("PacketRateDeviceDuLengthFlits",
                          "Spec DEVICE_DU_LENGTH (Table 4-9, Flit 4 bits 79:72, 8 bits): \"Length "
                          "of the internal data unit of the device (advanced devices generally "
                          "divide a packet into several data units of equal length for "
                          "processing), in flits.\" §4.7.2 notes it may be identical with the "
                          "number of flits contained in the cell negotiated by both ends of the "
                          "link. Used by DYNAMIC only. No default: §4.7.1 takes it from the peer "
                          "receiver's Init Block, and the Table 4-9 note forbids sending only part "
                          "of the initialization parameters. DYNAMIC aborts until it is set.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&UbPort::m_packetRateDeviceDuLengthFlits),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("PacketRateBurstSizeFlits",
                          "Spec PACKET_BURST_SIZE (Table 4-9, Flit 4 bits 31:16, 16 bits): "
                          "\"Maximum number of burst flits supported by the peer receive buffer, in "
                          "flits.\" Used by DYNAMIC only, and unlike the other dynamic parameters "
                          "it is a plain field value rather than an invented default: the "
                          "specification does not say whether 0 is legal, and this port reads 0 as "
                          "\"no burst allowance\", so any positive deficit triggers padding. A peer "
                          "whose buffer does absorb a burst has to be described explicitly.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&UbPort::m_packetRateBurstSizeFlits),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("CbfcFlitLenByte",
                          "Flit size in bytes; the fixed-size data link layer transfer unit (Spec §1.6: 20 bytes).",
                          UintegerValue(20),
                          MakeUintegerAccessor(&UbPort::m_cbfcFlitLen),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("CbfcFlitsPerCell",
                          "Number of flits per credit cell (Spec FLOW_CTRL_SIZE). "
                          "Valid: {1,2,4,8,16,32,64,128}; spec default 8 = 160 bytes/cell.",
                          UintegerValue(8),
                          MakeUintegerAccessor(&UbPort::m_cbfcFlitsPerCell),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("CbfcRetCellGrainDataPacket",
                          "Credit return granularity in DLLDP for data packets (Spec DATA_CREDIT_GRAIN_SIZE). "
                          "Valid: {1,2,4,8,16,32,64,128} cells; spec default 4.",
                          UintegerValue(4),
                          MakeUintegerAccessor(&UbPort::m_cbfcRetCellGrainDataPacket),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("CbfcRetCellGrainControlPacket",
                          "Credit return granularity in Crd_Ack Block for control packets "
                          "(Spec CTRL_CREDIT_GRAIN_SIZE). Valid: {1,2,4,8,16,32,64,128} cells; "
                          "repo default 32, spec default 1.",
                          UintegerValue(32),
                          MakeUintegerAccessor(&UbPort::m_cbfcRetCellGrainControlPacket),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("CbfcInitCreditCell",
                          "Initial CBFC transmit credit in cells. In exclusive mode this equals "
                          "floor(receive_buffer / cell_size); spec example: 1MB / 160B = 6553. "
                          "In shared mode this is the per-VL reserved minimum and should be reduced.",
                          IntegerValue(6553),
                          MakeIntegerAccessor(&UbPort::m_cbfcPortTxfree),
                          MakeIntegerChecker<int32_t>())
            .AddAttribute("CbfcSharedInitCreditCell",
                          "Shared credit pool size in cells for CBFC shared-credit mode. "
                          "reserved + shared must not exceed per-VL buffer capacity in cells.",
                          IntegerValue(4096),
                          MakeIntegerAccessor(&UbPort::m_cbfcSharedInitCells),
                          MakeIntegerChecker<int32_t>())
            .AddAttribute("CbfcCtrlCrdRtrThldCell",
                          "Per-VL pending returned-credit threshold in cells. Once a VL reaches this threshold, "
                          "CBFC switches to control-frame credit return instead of waiting for piggyback opportunities. "
                          "Repo default 1024 cells.",
                          IntegerValue(1024),
                          MakeIntegerAccessor(&UbPort::m_cbfcCtrlCrdRtrThldCells),
                          MakeIntegerChecker<int32_t>(1))
            .AddAttribute("PfcUpThld",
                          "PFC XOFF threshold in bytes; a PAUSE frame is sent when the receive queue exceeds this level.",
                          IntegerValue(DEFAULT_PFC_UP_THLD),
                          MakeIntegerAccessor(&UbPort::m_pfcUpThld),
                          MakeIntegerChecker<int32_t>())
            .AddAttribute("PfcLowThld",
                          "PFC XON threshold in bytes; the PAUSE is released when the receive queue drains below this level.",
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
    m_rerouting = CreateObject<UbReliableRerouting>();
    m_sendState = SendState::READY;
    // An unattached port is not link-up. Leaving this indeterminate let queue-only fixtures reach
    // the transmit path with a null channel.
    m_linkUp = false;
    m_txBytes = 0;
    m_mpiReceiveEnabled = false;
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

void
UbPort::SetReceiveHandler(ReceiveHandler handler)
{
    NS_LOG_FUNCTION(this);
    if (handler.IsNull())
    {
        ClearReceiveHandler();
        return;
    }
    m_receiveHandler = handler;
}

void
UbPort::ClearReceiveHandler()
{
    NS_LOG_FUNCTION(this);
    m_receiveHandler = MakeNullCallback<void, Ptr<UbPort>, Ptr<Packet>>();
}

bool
UbPort::HasReceiveHandler() const
{
    return !m_receiveHandler.IsNull();
}

void
UbPort::EnableMpiReceive()
{
#ifdef NS3_MPI
    if (m_mpiReceiveEnabled)
    {
        return;
    }

    Ptr<MpiReceiver> mpiReceiver = GetObject<MpiReceiver>();
    if (mpiReceiver == nullptr)
    {
        mpiReceiver = CreateObject<MpiReceiver>();
        AggregateObject(mpiReceiver);
    }

    mpiReceiver->SetReceiveCallback(MakeCallback(&UbPort::Receive, this));
    m_mpiReceiveEnabled = true;
#else
    NS_LOG_WARN("EnableMpiReceive called without MPI support");
#endif
}

bool
UbPort::HasMpiReceive() const
{
    return m_mpiReceiveEnabled;
}

void UbPort::SetIfIndex(const uint32_t portId)
{
    m_portId = portId;
    NS_ABORT_MSG_IF(GetNode() == nullptr, "Unified Bus port index requires an attached node");
    m_ubEQ->SetOwnerNodeId(GetNode()->GetId());
}

uint32_t UbPort::GetIfIndex() const
{
    return m_portId;
}

void UbPort::SetSendState(SendState state)
{
    m_sendState = state;
}

void
UbPort::ConfirmTransmitAttempt(uint64_t token)
{
    if (m_rerouting != nullptr)
    {
        m_rerouting->ConfirmAttempt(token);
    }
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
                    m_cbfcRetCellGrainControlPacket, m_cbfcPortTxfree, m_cbfcCtrlCrdRtrThldCells,
                    GetNode()->GetId(), m_portId);
                NS_LOG_DEBUG("[UbPort CreateAndInitFc] flowControl Cbfc Init");
            }
            break;
        case FcType::CBFC_SHARED:
            m_flowControl = CreateObject<UbCbfcSharedCredit>();
            if (m_flowControl == nullptr) {
                NS_FATAL_ERROR("Failed to create UbCbfcSharedCredit object for port " << m_portId);
            } else {
                auto flowControl = DynamicCast<UbCbfcSharedCredit>(m_flowControl);
                const int32_t reservedPerVlCells = m_cbfcPortTxfree;
                const int32_t sharedInitCells = m_cbfcSharedInitCells;

                flowControl->Init(m_cbfcFlitLen, m_cbfcFlitsPerCell,
                                m_cbfcRetCellGrainDataPacket, m_cbfcRetCellGrainControlPacket,
                                reservedPerVlCells, m_cbfcCtrlCrdRtrThldCells, sharedInitCells,
                                GetNode()->GetId(), m_portId);

                NS_LOG_DEBUG("[UbPort CreateAndInitFc] flowControl CbfcSharedMode Init");
            }
            break;
        case FcType::PFC_FIXED:
        case FcType::PFC_DYNAMIC:
        case FcType::PFC_DYNAMIC_PAPER:
            m_flowControl = CreateObject<UbPfc>();
            if (m_flowControl == nullptr) {
                NS_FATAL_ERROR("Failed to create UbPfc object for port " << m_portId);
            } else {
                auto flowControl = DynamicCast<UbPfc>(m_flowControl);
                flowControl->Init(type, m_pfcUpThld, m_pfcLowThld, GetNode()->GetId(), m_portId);
                IntegerValue val;
                g_ub_vl_num.GetValue(val);
                int ubVlNum = val.Get();
                m_revQueueSize.resize(ubVlNum, 0);
                for (int pri = 0; pri < ubVlNum; ++pri) {
                    // PFC starts from the link-up/resumed state. Keeping the local port-side
                    // credit image explicit avoids empty-queue/xoff==0 cases looking paused.
                    m_credits[pri] = UB_CREDIT_MAX_VALUE;
                }
                NS_LOG_DEBUG("[UbPort CreateAndInitFc] flowControl Pfc Init mode=" << static_cast<int>(type));
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

void
UbPort::LastAckSerialized(Ptr<Packet> packet)
{
    UbLastAckTag lastAckTag;
    if (packet != nullptr && packet->PeekPacketTag(lastAckTag))
    {
        TraceReliableReroutingLastAck(packet, GetNode()->GetId(), GetIfIndex(), "lastack_tx_complete");
        if (lastAckTag.sourceNode == GetNode()->GetId())
        {
            DynamicCast<UbPort>(GetNode()->GetDevice(lastAckTag.sourcePort))
                ->GetReliableRerouting()->NotifyLastAckSent(lastAckTag.copy);
        }
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
    if (m_outputBlocked)
    {
        // A detected failed output keeps its egress backlog for recovery; it must not dequeue.
        NS_LOG_DEBUG("[UbPort DequeuePacket] output blocked; retaining egress backlog");
        return;
    }
    m_sendState = SendState::BUSY;

    auto [inPortId, priority, packet] = m_ubEQ->DoDequeue();
    Ptr<UbSwitch> ubSwitch = GetNode()->GetObject<UbSwitch>();
    if (ubSwitch != nullptr)
    {
        ubSwitch->GetQueueManager()->RemoveEgressBufferedBytes(packet->GetSize());
    }

    if (m_rerouting != nullptr && m_rerouting->IsArmed())
    {
        m_rerouting->FailedEgressDequeued(packet->GetSize());
    }
    m_currentPkt = packet;
    m_currentInPortId = inPortId;
    m_currentPriority = priority;
    if (m_ubEQ->IsEmpty()) {
        // Switch allocation when port sendding packet.
        if (ubSwitch != nullptr && ubSwitch->GetAllocator() != nullptr) {
            ubSwitch->GetAllocator()->ScheduleTriggerAllocator(this);
        }
    }

    if (!m_faultCallBack.IsNull()) {
        m_faultCallBack(packet, GetNode()->GetId(), m_portId, this);
        return;
    }
    TransmitPacket(packet, Time(0));
    return;
}

bool
UbPort::IsDllDataPacket(Ptr<Packet> packet) const
{
    // Two bytes are enough for the config field of UbDatalinkHeader, which both
    // UbDatalinkPacketHeader and UbDatalinkControlCreditHeader start with.
    if (packet->GetSize() < 2) {
        return false;
    }
    UbDatalinkHeader dlHeader;
    packet->PeekHeader(dlHeader);
    switch (static_cast<UbDatalinkHeaderConfig>(dlHeader.GetConfig())) {
    case UbDatalinkHeaderConfig::PACKET_IPV4:
    case UbDatalinkHeaderConfig::PACKET_IPV6:
    case UbDatalinkHeaderConfig::PACKET_CNA16:
    case UbDatalinkHeaderConfig::PACKET_CNA24:
    case UbDatalinkHeaderConfig::PACKET_UB_MEM:
        return true;
    case UbDatalinkHeaderConfig::CONTROL:
    default:
        // Control frames travel through the same egress queue as data, and an
        // unrecognised config must keep its current behaviour rather than be
        // padded by guesswork.
        return false;
    }
}

void
UbPort::ValidatePacketRateControlConfig() const
{
    // Built lazily: looking up the node costs a Ptr copy and this runs once per
    // transmitted DLLDP, while a failed check aborts the process anyway.
    const auto where = [this]() {
        return " NodeId: " +
               std::to_string(GetNode() != nullptr ? GetNode()->GetId() : Simulator::NO_CONTEXT) +
               " PortId: " + std::to_string(m_portId) + ".";
    };

    NS_ABORT_MSG_IF(m_cbfcFlitLen == 0,
                    "Packet rate control is enabled but CbfcFlitLenByte is 0, so a flit "
                    "length cannot be derived."
                        << where());
    NS_ABORT_MSG_IF(m_packetRateMinIntervalFlits == 0,
                    "Packet rate control is enabled but PacketRateMinIntervalFlits "
                    "(PACKET_MIN_INTERVAL) is 0."
                        << where());
    if (m_packetRateControl == PacketRateControlMode::DYNAMIC) {
        NS_ABORT_MSG_IF(
            m_packetRateAveragePacketLengthFlits == 0,
            "Dynamic packet rate control has no default for "
            "PacketRateAveragePacketLengthFlits (AVERAGE_PACKET_LENGTH); it is the peer "
            "receiver's advertised capability and has to be configured explicitly."
                << where());
        NS_ABORT_MSG_IF(m_packetRateAverageDuLengthFlits == 0,
                        "Dynamic packet rate control has no default for "
                        "PacketRateAverageDuLengthFlits (AVERAGE_DU_LENGTH); it is the peer "
                        "receiver's advertised capability and has to be configured explicitly."
                            << where());
        NS_ABORT_MSG_IF(m_packetRateDeviceDuLengthFlits == 0,
                        "Dynamic packet rate control has no default for "
                        "PacketRateDeviceDuLengthFlits (DEVICE_DU_LENGTH); it is the peer "
                        "receiver's advertised capability and has to be configured explicitly."
                            << where());
        // PacketRateBurstSizeFlits is deliberately not checked: 0 is the field's own
        // value meaning the peer absorbs no burst, not an unset marker.
    }
}

Time
UbPort::ComputePacketRateNobTime(Ptr<Packet> packet) const
{
    if (m_packetRateControl == PacketRateControlMode::NONE) {
        return Time(0);
    }
    if (!IsDllDataPacket(packet)) {
        return Time(0);
    }
    ValidatePacketRateControlConfig();

    const uint32_t flitBytes = m_cbfcFlitLen;
    const uint32_t pktFlits = (packet->GetSize() + flitBytes - 1) / flitBytes;
    const Time txTime = m_bps.CalculateBytesTxTime(packet->GetSize());
    // Bytes to time once per call, so a flit shorter than the clock resolution
    // can never round to zero and take a later division down with it.
    const auto wireTime = [this, flitBytes](uint64_t flits) {
        return m_bps.CalculateBytesTxTime(flits * flitBytes);
    };

    if (m_packetRateControl == PacketRateControlMode::STATIC) {
        // §4.7.3.1, "Static Packet Rate Control Rules", quoted:
        //
        //   1. "After the previous DLLDP is sent, determine whether it is
        //      necessary to insert NOBs after transmitting the next DLLDP, based
        //      on PACKET_INTERVAL."
        //   2. "PACKET_INTERVAL is the interval between the SOPs of the previous
        //      DLLDP and the next DLLDP to be sent, in flits."
        //   3. "If PACKET_INTERVAL >= PACKET_MIN_INTERVAL, no NOBs SHALL be
        //      inserted."
        //   4. "If PACKET_INTERVAL < PACKET_MIN_INTERVAL, NOBs SHALL be
        //      inserted. The number of inserted NOBs is calculated as follows:
        //      NOB_NUM = PACKET_MIN_INTERVAL - PACKET_INTERVAL."
        //
        // Modelled per DLLDP. The spec compares the SOPs of two
        // consecutive DLLDPs and pads the next one to PACKET_MIN_INTERVAL; this
        // port does not keep the previous SOP, and instead requires every DLLDP
        // to leave the next SOP PACKET_MIN_INTERVAL away. Back to back the two
        // readings agree and rules 3 and 4 become a fixed point of each other,
        // so no DLLDP can inherit an interval a predecessor broke. They diverge
        // after an idle period, where the spec would measure a long interval and
        // pad nothing while this model still pads the first small DLLDP to its
        // own target -- the more conservative of the two.
        //
        // The interframe gap is part of the interval, so a gap long enough on
        // its own suppresses the padding. Subtracting the real txTime also
        // absorbs the tail of a DLLDP that does not end on a flit boundary.
        const Time target = wireTime(m_packetRateMinIntervalFlits);
        const Time interval = txTime + m_tInterframeGap;
        return interval < target ? target - interval : Time(0);
    }

    // §4.7.3.2, "Dynamic Packet Rate Control Rules", quoted:
    //
    //   1. "Calculate the transmission length (PKT_FLIT_NUM) of the DLLDP to be
    //      transmitted, satisfying the minimum packet interval requirement:
    //      (1) If the DLLDP length is greater than or equal to
    //      PACKET_MIN_INTERVAL, PKT_FLIT_NUM equals the DLLDP length (i.e., the
    //      value of LPH.PLENGTH[13:5] plus 1). (2) Otherwise, PKT_FLIT_NUM
    //      equals PACKET_MIN_INTERVAL."
    //   2. "Calculate the total data unit length (PKT_DU_LENGTH) occupied by the
    //      DLLDP to be transmitted, meeting the average data unit length
    //      requirement: PKT_DU_LENGTH = AVERAGE_DU_LENGTH x [Floor((LPH.
    //      PLENGTH[13:5])/(DEVICE_DU_LENGTH)) + 1]"
    //   3. "Maintain a counter, CNT_1, to track the accumulated deficit of data
    //      length, in flits, caused by transmitted DLLDPs falling short of
    //      AVERAGE_PACKET_LENGTH. The initial value of CNT_1 is 0."
    //   4. "Maintain a counter, CNT_2, to track the accumulated deficit of data
    //      length, in flits, caused by transmitted DLLDPs falling short of
    //      AVERAGE_DU_LENGTH. The initial value of CNT_2 is 0."
    //   5. "Based on the values of CNT_1 and CNT_2, perform the following
    //      calculations for each transmitted DLLDP" -- the four branches below,
    //      selected by whether each deficit exceeds PACKET_BURST_SIZE.
    //
    // PKT_FLIT_NUM is the DLLDP length rounded up to
    // PACKET_MIN_INTERVAL, and PKT_DU_LENGTH scales AVERAGE_DU_LENGTH by the
    // number of DEVICE_DU_LENGTH blocks the DLLDP occupies. Note that the
    // specification's LPH.PLENGTH[13:5] is the DLLDP length minus one, which is
    // the (pktFlits - 1) below.
    const uint32_t pkFlitNum = std::max<uint32_t>(pktFlits, m_packetRateMinIntervalFlits);
    const uint32_t pktDuLen = m_packetRateAverageDuLengthFlits *
                              ((pktFlits - 1) / m_packetRateDeviceDuLengthFlits + 1);

    // Signed arithmetic on purpose: once PKT_FLIT_NUM grows past the accumulated
    // deficit plus the target, both expressions go negative, and an unsigned
    // subtraction would wrap to a huge positive value and select the wrong
    // branch. A DLLDP longer than both targets is the ordinary way to get there.
    const int64_t deficit1 =
        static_cast<int64_t>(m_packetRateCnt1) + m_packetRateAveragePacketLengthFlits - pkFlitNum;
    const int64_t deficit2 = static_cast<int64_t>(m_packetRateCnt2) + pktDuLen - pkFlitNum;
    const int64_t burst = m_packetRateBurstSizeFlits;
    const bool overBurst1 = deficit1 > burst;
    const bool overBurst2 = deficit2 > burst;

    int64_t nobFlits = 0;
    if (overBurst1 && overBurst2) {
        nobFlits = std::max<int64_t>(m_packetRateAveragePacketLengthFlits, pktDuLen) - pktFlits;
    } else if (overBurst2) {
        nobFlits = static_cast<int64_t>(pktDuLen) - pktFlits;
    } else if (overBurst1) {
        nobFlits = static_cast<int64_t>(m_packetRateAveragePacketLengthFlits) - pktFlits;
    } else {
        nobFlits = static_cast<int64_t>(pkFlitNum) - pktFlits;
    }
    nobFlits = std::max<int64_t>(nobFlits, 0);

    // NOB_NUM counts whole flits, so the padded DLLDP occupies pktFlits +
    // nobFlits flits. The interframe gap is not subtracted here: it is a fixed
    // idle time this model adds after every send, and keeping it on top of the
    // specified NOB count only makes the interval longer than required.
    const Time target = wireTime(static_cast<uint64_t>(pktFlits) + nobFlits);
    return target > txTime ? target - txTime : Time(0);
}

void
UbPort::CommitPacketRateControlCounters(Ptr<Packet> packet)
{
    if (m_packetRateControl != PacketRateControlMode::DYNAMIC || !IsDllDataPacket(packet)) {
        return;
    }

    const uint32_t flitBytes = m_cbfcFlitLen;
    const uint32_t pktFlits = (packet->GetSize() + flitBytes - 1) / flitBytes;
    const uint32_t pkFlitNum = std::max<uint32_t>(pktFlits, m_packetRateMinIntervalFlits);
    const uint32_t pktDuLen = m_packetRateAverageDuLengthFlits *
                              ((pktFlits - 1) / m_packetRateDeviceDuLengthFlits + 1);
    const int64_t burst = m_packetRateBurstSizeFlits;

    // §4.7.3.2 rules 6 and 7, evaluated after this DLLDP has actually been
    // sent. Rule 6 quoted (rule 7 is the same with CNT_2 and PKT_DU_LENGTH):
    //
    //   "(1) When (CNT_1 + AVERAGE_PACKET_LENGTH - PKT_FLIT_NUM) >
    //    PACKET_BURST_SIZE, CNT_1 remains unchanged.
    //    (2) When (CNT_1 + AVERAGE_PACKET_LENGTH) < PKT_FLIT_NUM, CNT_1 is
    //    reset to 0.
    //    (3) Otherwise, CNT_1 = CNT_1 + AVERAGE_PACKET_LENGTH - PKT_FLIT_NUM."
    //
    // A counter that is over budget stays put, and otherwise carries the
    // deficit forward, floored at 0 by the reset condition. Both expressions
    // subtract the same PKT_FLIT_NUM and differ only in the target they measure
    // against -- AVERAGE_PACKET_LENGTH for CNT_1, this DLLDP's own
    // PKT_DU_LENGTH for CNT_2.
    const int64_t deficit1 =
        static_cast<int64_t>(m_packetRateCnt1) + m_packetRateAveragePacketLengthFlits - pkFlitNum;
    const int64_t deficit2 = static_cast<int64_t>(m_packetRateCnt2) + pktDuLen - pkFlitNum;

    if (deficit1 <= burst) {
        m_packetRateCnt1 = static_cast<uint64_t>(std::max<int64_t>(deficit1, 0));
    }
    if (deficit2 <= burst) {
        m_packetRateCnt2 = static_cast<uint64_t>(std::max<int64_t>(deficit2, 0));
    }
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
    // NOB insertion extends only how long this port stays occupied; it never
    // changes this packet's own serialization time.
    Time txCompleteTime = txTime + m_tInterframeGap + ComputePacketRateNobTime(packet);

    // Retain the original whole packet at the actual send boundary so a failed link can
    // recover an attempt that the peer never completely received. The token is local simulator
    // metadata; it is never serialized and is unrelated to TCP-style PSNs or Packet::GetUid().
    const uint64_t attemptToken =
        m_rerouting != nullptr && m_rerouting->IsArmed()
            ? m_rerouting->CreateAttempt(packet, m_currentInPortId, m_currentPriority)
            : 0;

    // Try to start transmission on the channel first. Only if it succeeds
    // should we schedule the TransmitComplete event and notify traces.
    bool result = attemptToken == 0
                      ? m_channel->TransmitStart(packet, this, txTime)
                      : m_channel->TransmitStart(packet, this, txTime, attemptToken);
    if (result == false) {
        NS_LOG_WARN("Channel transmission failed! Packet dropped at Node " << GetNode()->GetId() << " Port " << m_portId);
        NS_ABORT_MSG_IF(attemptToken != 0,
                        "Reliable rerouting carrier rejected an accepted transmit attempt");
        // Reset port state so it won't remain BUSY and try next packet
        m_sendState = SendState::READY;
        m_currentPkt = nullptr;
        m_currentInPortId = 0;
        m_currentPriority = 0;
        Simulator::ScheduleNow(&UbPort::TriggerTransmit, this);
        return;
    }

    // Transmission started successfully: notify and schedule completion. The
    // counters advance here and not in ComputePacketRateNobTime, so a rejected
    // attempt cannot consume burst budget.
    UbLastAckTag lastAckTag;
    if (packet->PeekPacketTag(lastAckTag))
    {
        TraceReliableReroutingLastAck(packet, GetNode()->GetId(), GetIfIndex(), "lastack_tx_start");
        m_lastAckSerialization = Simulator::Schedule(txTime, &UbPort::LastAckSerialized, this, packet);
    }
    CommitPacketRateControlCounters(packet);
    TraComEventNotify(packet, txCompleteTime);
    Simulator::Schedule(txCompleteTime, &UbPort::TransmitComplete, this);
    NS_LOG_DEBUG("[UbFc DequeueAndTransmit] will send pkt size: " << packet->GetSize());
    UpdateTxBytes(packet->GetSize());

    return;
}

void
UbPort::TransmitPacketDetached(Ptr<Packet> packet)
{
    PortTxNotify(GetNode()->GetId(), m_portId, packet->GetSize());
    NS_LOG_DEBUG("[UbPort detached send] nodetype: " << g_node_type_map[GetNode()->GetObject<UbSwitch>()->GetNodeType()]
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

    Time txTime = m_bps.CalculateBytesTxTime(packet->GetSize());
    bool result = m_channel->TransmitStart(packet, this, txTime);
    if (result == false) {
        NS_LOG_WARN("Detached channel transmission failed! Packet dropped at Node "
                    << GetNode()->GetId() << " Port " << m_portId);
        return;
    }

    TraComEventNotify(packet, txTime);
    UpdateTxBytes(packet->GetSize());
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
    PktRcvNotify(packet);
    DispatchReceive(packet);

    return;
}

void
UbPort::DispatchReceive(Ptr<Packet> packet)
{
    if (!m_receiveHandler.IsNull())
    {
        m_receiveHandler(this, packet);
        return;
    }
    HandleWithUbSwitch(packet);
}

void
UbPort::HandleWithUbSwitch(Ptr<Packet> packet)
{
    GetNode()->GetObject<UbSwitch>()->SwitchHandlePacket(this, packet);
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
    UbIpBasedNetworkHeader netHeader;
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
    if (m_outputBlocked) {
        // Detection stops new sends; the retained backlog waits for the modeled LastAck.
        NS_LOG_DEBUG("[UbPort TriggerTransmit] output blocked");
        return;
    }
    if (IsBusy()) {
        if (m_ubEQ->IsEmpty()) {
            Ptr<UbSwitch> ubSwitch = GetNode()->GetObject<UbSwitch>();
            if (ubSwitch != nullptr && ubSwitch->GetAllocator() != nullptr) {
                // Keep allocator prefetch alive while the current packet is still sending.
                // This lets a late-arriving next packet hide AllocationTime behind the
                // current serialization delay instead of always bubbling after tx-complete.
                ubSwitch->GetAllocator()->ScheduleTriggerAllocator(this);
            }
        }
        NS_LOG_DEBUG("[UbPort TriggerTransmit] SendState::BUSY");
        return; // Quit if channel busy
    }
    if (m_ubEQ->IsEmpty()) {
        NS_LOG_DEBUG("[UbPort TriggerTransmit] trigger Allocator");
        Ptr<UbSwitch> ubSwitch = GetNode()->GetObject<UbSwitch>();
        if (ubSwitch != nullptr && ubSwitch->GetAllocator() != nullptr) {
            ubSwitch->GetAllocator()->ScheduleTriggerAllocator(this);
        }
        return;
    }
    DequeuePacket();
}

void UbPort::NotifyAllocationFinish()
{
    if (m_outputBlocked) {
        return;
    }
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
    m_lastAckSerialization.Cancel();
    m_ubEQ = nullptr;
    m_channel = nullptr;
    m_currentPkt = nullptr;
    if (m_rerouting != nullptr)
    {
        m_rerouting->Dispose();
        m_rerouting = nullptr;
    }
    m_outputBlocked = false;
    m_datalink = nullptr;
    m_flowControl = nullptr;
    m_mpiReceiveEnabled = false;
    m_receiveHandler = MakeNullCallback<void, Ptr<UbPort>, Ptr<Packet>>();
    m_revQueueSize.clear();
    NetDevice::DoDispose();
}

} // namespace ns3
