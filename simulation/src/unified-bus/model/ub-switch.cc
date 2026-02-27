// SPDX-License-Identifier: GPL-2.0-only
#include <iostream>
#include "ns3/ipv4.h"
#include "ns3/packet.h"
#include "ns3/flow-id-tag.h"
#include "ns3/ub-switch-allocator.h"
#include "ns3/ub-caqm.h"
#include "ns3/ub-port.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-controller.h"

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(UbSwitch);
NS_LOG_COMPONENT_DEFINE("UbSwitch");


/*-----------------------------------------UbSwitchNode----------------------------------------------*/

TypeId UbSwitch::GetTypeId (void)
{
    static TypeId tid = TypeId ("ns3::UbSwitch")
        .SetParent<Object> ()
        .SetGroupName("UnifiedBus")
        .AddConstructor<UbSwitch> ()
        .AddAttribute("FlowControl",
                      "The flow control mechanism to use (NONE, CBFC, CBFC_SHARED, or PFC).",
                      EnumValue(FcType::NONE),
                      MakeEnumAccessor(&UbSwitch::m_flowControlType),
                      MakeEnumChecker(FcType::NONE, "NONE",
                                      FcType::CBFC, "CBFC",
                                      FcType::CBFC_SHARED_CRD, "CBFC_SHARED",
                                      FcType::PFC, "PFC"))
        .AddAttribute("VlScheduler",
                      "VL inter-scheduling algorithm (SP or DWRR).",
                      EnumValue(SP),
                      MakeEnumAccessor(&UbSwitch::m_vlScheduler),
                      MakeEnumChecker(SP, "SP",
                                      DWRR, "DWRR"))
        .AddTraceSource("LastPacketTraversesNotify",
                        "Last Packet Traverses, NodeId",
                        MakeTraceSourceAccessor(&UbSwitch::m_traceLastPacketTraversesNotify),
                        "ns3::UbSwitch::LastPacketTraversesNotify");
    return tid;
}
/**
 * @brief Init UbNode, create algorithm, queueManager, fc and so on
 */
void UbSwitch::Init()
{
    auto node = GetObject<Node>();
    m_portsNum = node->GetNDevices();
    // alg init
    switch (m_vlScheduler) {
        case DWRR:
            m_allocator = CreateObject<UbDwrrAllocator>();
            break;
        case SP:
        default:
            m_allocator = CreateObject<UbRoundRobinAllocator>();
            break;
    }
    m_allocator->SetNodeId(node->GetId());
    m_allocator->Init();
    VoqInit();
    RegisterVoqsWithAllocator();

    // queueManager init
    m_queueManager = CreateObject<UbQueueManager>();
    m_queueManager->SetVLNum(m_vlNum);
    m_queueManager->SetPortsNum(m_portsNum);
    m_queueManager->Init();

    InitNodePortsFlowControl();
    m_routingProcess = CreateObject<UbRoutingProcess>();
    m_routingProcess->SetNodeId(node->GetId());
    m_Ipv4Addr = utils::NodeIdToIp(node->GetId());
}

void UbSwitch::DoDispose()
{
    m_queueManager = nullptr;
    m_congestionCtrl = nullptr;
    m_allocator = nullptr;
    m_voq.clear();
    m_routingProcess = nullptr;
}

/**
 * @brief Init flow control for each port
 */
void UbSwitch::InitNodePortsFlowControl()
{
    NS_LOG_DEBUG("[UbSwitch InitNodePortsFlowControl] m_portsNum: " << m_portsNum
                << " m_flowControlType: " << static_cast<int>(m_flowControlType));

    for (uint32_t pidx = 0; pidx < m_portsNum; pidx++) {
        Ptr<UbPort> port = DynamicCast<ns3::UbPort>(GetObject<Node>()->GetDevice(pidx));
        port->CreateAndInitFc(m_flowControlType);
    }
}

/**
 * @brief 将初始化后的vop放入调度算法中
 */
void UbSwitch::RegisterVoqsWithAllocator()
{
    for (uint32_t i = 0; i < m_portsNum; i++) {
        for (uint32_t j = 0; j < m_vlNum; j++) {
            for (uint32_t k = 0 ; k < m_portsNum; k++) { // voq
                auto ingressQ = m_voq[i][j][k];
                m_allocator->RegisterUbIngressQueue(ingressQ, i, j);
            }
        }
    }
}

/**
 * @brief 将tp放入调度算法中
 */
void UbSwitch::RegisterTpWithAllocator(Ptr<UbIngressQueue> tp, uint32_t outPort, uint32_t priority)
{
    if ((outPort >= m_portsNum) || (priority >= m_vlNum)) {
        NS_ASSERT_MSG(0, "Invalid indices (outPort, priority)!");
    }
    NS_LOG_DEBUG("[UbSwitch RegisterTpWithAllocator] TP: outPortIdx: " << outPort
                 << "priorityIdx: " << priority << "outPort: " << outPort);
    tp->SetOutPortId(outPort);
    tp->SetInPortId(outPort); // tp uses outPort as inPort, since tp has no inPort
    tp->SetIngressPriority(priority);
    m_allocator->RegisterUbIngressQueue(tp, outPort, priority);
}

/**
 * @brief 将tp从调度算法中删除
 */
void UbSwitch::RemoveTpFromAllocator(Ptr<UbIngressQueue> tp)
{
    uint32_t outPort = tp->GetOutPortId();
    uint32_t priority = tp->GetIngressPriority();
    m_allocator->UnregisterUbIngressQueue(tp, outPort, priority);
}

UbSwitch::UbSwitch()
{
}
UbSwitch::~UbSwitch()
{
}

Ptr<UbSwitchAllocator> UbSwitch::GetAllocator()
{
    return m_allocator;
}

/**
 * @brief init voq
 */
void UbSwitch::VoqInit()
{
    uint32_t outPortIdx = 0;
    uint32_t priorityIdx = 0;
    uint32_t inPortIdx = 0;
    m_voq.resize(m_portsNum);
    for (auto &i : m_voq) {
        priorityIdx = 0;
        i.resize(m_vlNum);
        for (auto &j : i) {
            inPortIdx = 0;
            for (uint32_t k = 0; k < m_portsNum; k++) {
                auto q = CreateObject<UbPacketQueue>();
                q->SetOutPortId(outPortIdx);
                q->SetIngressPriority(priorityIdx);
                q->SetInPortId(inPortIdx); // tp不使用inport
                q->SetInPortId(k);
                j.push_back(q);
                inPortIdx++;
            }
            priorityIdx++;
        }
        outPortIdx++;
    }
}

/**
 * @brief push packet into voq
 */
void UbSwitch::PushPacketToVoq(Ptr<Packet> p, uint32_t outPort, uint32_t priority, uint32_t inPort)
{
    if ((outPort > m_portsNum) || (priority > m_vlNum) || (inPort > m_portsNum)) { // 不合理请求
        NS_ASSERT_MSG(0, "Invalid VOQ indices (outPort, priority, inPort)!");
    }
    m_voq[outPort][priority][inPort]->Push(p);
}

UbPacketType_t UbSwitch::GetPacketType(Ptr<Packet> packet)
{
    UbDatalinkHeader dlHeader;
    packet->PeekHeader(dlHeader);
    if (dlHeader.IsControlCreditHeader())
        return UB_CONTROL_FRAME;
    if (dlHeader.IsPacketIpv4Header())
        return UB_URMA_DATA_PACKET;
    if (dlHeader.IsPacketUbMemHeader())
        return UB_LDST_DATA_PACKET;
    return UNKOWN_TYPE;
}

/**
 * @brief Receive packet from port. Node handle packet
 */
void UbSwitch::SwitchHandlePacket(Ptr<UbPort> port, Ptr<Packet> packet)
{
    // 帧类型判断
    auto packetType = GetPacketType(packet);
    switch (packetType) {
        case UB_CONTROL_FRAME:
            port->m_flowControl->HandleReceivedControlPacket(packet);
            break;
        case UB_URMA_DATA_PACKET:
            HandleURMADataPacket(port, packet);
            break;
        case UB_LDST_DATA_PACKET:
            HandleLdstDataPacket(port, packet);
            break;
        default:
            NS_ASSERT_MSG(0, "Invalid Packet Type!");
    }
    return;
}

/**
 * @brief Sink control frame
 */
void UbSwitch::SinkControlFrame(Ptr<UbPort> port, Ptr<Packet> packet)
{
    if (IsCBFCSharedEnable()) {
        auto flowControl = DynamicCast<UbCbfcSharedCredit>(port->GetFlowControl());
        flowControl->CbfcSharedRestoreCrd(packet);
    } else if (IsCBFCEnable()) {
        auto flowControl = DynamicCast<UbCbfc>(port->GetFlowControl());
        flowControl->CbfcRestoreCrd(packet);
    } else if (IsPFCEnable()) {
        auto flowControl = DynamicCast<UbPfc>(port->GetFlowControl());
        flowControl->UpdatePfcStatus(packet);
    }

    return;
}

/**
 * @brief Handle URMA type data packet
 */
void UbSwitch::HandleURMADataPacket(Ptr<UbPort> port, Ptr<Packet> packet)
{
    // Parse headers once for efficient reuse
    ParsedURMAHeaders headers;
    ParseURMAPacketHeader(packet, headers);

    switch (GetNodeType()) {
        case UB_DEVICE:
            if (!SinkTpDataPacket(port, packet, headers)) {
                ForwardDataPacket(port, packet, headers);
            }
            break;
        case UB_SWITCH:
            ForwardDataPacket(port, packet, headers);
            break;
        default:
            NS_ASSERT_MSG(0, "Invalid Node! ");
    }
}

/**
 * @brief Handle Ldst type data packet
 */
void UbSwitch::HandleLdstDataPacket(Ptr<UbPort> port, Ptr<Packet> packet)
{
    // Parse headers once for efficient reuse
    ParsedLdstHeaders headers;
    ParseLdstPacketHeader(packet, headers);

    switch (GetNodeType()) {
        case UB_DEVICE:
            if (!SinkLdstDataPacket(port, packet, headers)) {
                ForwardDataPacket(port, packet, headers);
            }
            break;
        case UB_SWITCH:
            ForwardDataPacket(port, packet, headers);
            break;
        default:
            NS_ASSERT_MSG(0, "Invalid Node! ");
    }
}

/**
 * @brief Sink URMA type data packet
 */
bool UbSwitch::SinkTpDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedURMAHeaders &headers)
{
    NS_LOG_DEBUG("[UbPort recv] Psn: " << headers.transportHeader.GetPsn());
    Ipv4Mask mask("255.255.255.0");

    // Forward
    if (!utils::IsInSameSubnet(headers.ipv4Header.GetDestination(), GetNodeIpv4Addr(), mask)) {
        return false;
    }
    // Sink
    NS_LOG_DEBUG("[UbPort recv] Pkt tb is local");
    if (IsCBFCEnable() || IsCBFCSharedEnable()) {
        port->m_flowControl->HandleReceivedPacket(packet);
    }

    uint32_t dstTpn = headers.transportHeader.GetDestTpn();
    auto targetTp = GetObject<UbController>()->GetTpByTpn(dstTpn);
    if (targetTp == nullptr) {
        if (GetObject<UbController>()->GetTpConnManager()->IsTpRemoveMode()) {
            NS_LOG_WARN("Auto remove tp mode, drop this packet.");
            return true;
        } else {
            NS_ASSERT_MSG(0, "Port Cannot Get Tp By Tpn!");
        }
    }
    if (headers.transportHeader.GetTPOpcode() == static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITH_CETPH)
        || headers.transportHeader.GetTPOpcode() == static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH)) {
        NS_LOG_DEBUG("[UbPort recv] is ACK");
        UbDatalinkPacketHeader tempDlHeader;
        UbNetworkHeader tempNetHeader;
        Ipv4Header tempIpv4Header;
        UdpHeader tempUdpHeader;
        packet->RemoveHeader(tempDlHeader);
        packet->RemoveHeader(tempNetHeader);
        packet->RemoveHeader(tempIpv4Header);
        packet->RemoveHeader(tempUdpHeader);
        targetTp->RecvTpAck(packet);
    } else {
        targetTp->RecvDataPacket(packet);
    }
    return true;
}

/**
 * @brief Sink Ldst type data packet
 */
bool UbSwitch::SinkLdstDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedLdstHeaders &headers)
{
    // Store/load request: DLH cNTH cTAH(0x03/0x06) [cMAETAH] Payload
    // Store/load response: DLH cNTH cATAH(0x11/0x12) Payload
    NS_LOG_DEBUG("[UbPort recv] ub ldst frame");
    uint16_t dCna = headers.cna16NetworkHeader.GetDcna();
    uint32_t dnode = utils::Cna16ToNodeId(dCna);
    // Forward
    if (dnode != GetObject<Node>()->GetId()) {
        return false;
    }
    // Sink Packet
    if (IsCBFCEnable() || IsCBFCSharedEnable()) {
        port->m_flowControl->HandleReceivedPacket(packet);
    }

    auto ldstApi = GetObject<Node>()->GetObject<UbController>()->GetUbFunction()->GetUbLdstApi();
    NS_ASSERT_MSG(ldstApi != nullptr, "UbLdstApi can not be nullptr!");

    uint8_t type = headers.dummyTransactionHeader.GetTaOpcode();
    // 数据包
    if (type == (uint8_t)TaOpcode::TA_OPCODE_WRITE || type == (uint8_t)TaOpcode::TA_OPCODE_READ) {
        ldstApi->RecvDataPacket(packet);
    } else if (type == (uint8_t)TaOpcode::TA_OPCODE_TRANSACTION_ACK ||
               type == (uint8_t)TaOpcode::TA_OPCODE_READ_RESPONSE) {
        ldstApi->RecvResponse(packet);
        NS_LOG_DEBUG("ldst packet is ack!");
    } else {
        NS_ASSERT_MSG(0, "packet Ta Op code is wrong!");
    }
    return true;
}

/**
 * @brief Forward URMA data packet (headers already parsed)
 */
void UbSwitch::ForwardDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedURMAHeaders &headers)
{
    // Log packet traversal
    LastPacketTraversesNotify(GetObject<Node>()->GetId(), headers.transportHeader);

    // Get routing key from parsed headers
    RoutingKey rtKey;
    GetURMARoutingKey(headers, rtKey);

    // Route
    bool selectedShortestPath = false;
    int outPort = m_routingProcess->GetOutPort(rtKey, selectedShortestPath, port->GetIfIndex());
    if (outPort < 0) {
        NS_LOG_WARN("The route cannot be found. Packet Dropped!");
        return;
    }

    // If packet routed via non-shortest path, force subsequent hops to use shortest path
    if (!selectedShortestPath) {
        ForceShortestPathRouting(packet, headers.datalinkPacketHeader);
    }

    // Buffer management: check input port buffer space
    uint32_t inPort = port->GetIfIndex();
    uint8_t priority = headers.datalinkPacketHeader.GetPacketVL();
    uint32_t pSize = packet->GetSize();

    if (!m_queueManager->CheckInPortSpace(inPort, priority, pSize)) {
        NS_LOG_WARN("NodeId " << GetObject<Node>()->GetId() << " InPort " << inPort << " pri=" << (uint32_t)priority
                    << " buffer full. Packet Dropped!");
        return;
    }

    SendPacket(packet, inPort, outPort, priority);
}

/**
 * @brief Forward LDST data packet (headers already parsed)
 */
void UbSwitch::ForwardDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedLdstHeaders &headers)
{
    // Get routing key from parsed headers
    RoutingKey rtKey;
    GetLdstRoutingKey(headers, rtKey);

    // Route
    bool selectedShortestPath = false;
    int outPort = m_routingProcess->GetOutPort(rtKey, selectedShortestPath, port->GetIfIndex());
    if (outPort < 0) {
        NS_LOG_WARN("The route cannot be found. Packet Dropped!");
        return;
    }

    // If packet routed via non-shortest path, force subsequent hops to use shortest path
    if (!selectedShortestPath) {
        ForceShortestPathRouting(packet, headers.datalinkPacketHeader);
    }

    // Buffer management: check input port buffer space
    uint32_t inPort = port->GetIfIndex();
    uint8_t priority = headers.datalinkPacketHeader.GetPacketVL();
    uint32_t pSize = packet->GetSize();

    if (!m_queueManager->CheckInPortSpace(inPort, priority, pSize)) {
        NS_LOG_WARN("NodeId " << GetObject<Node>()->GetId() << " InPort " << inPort << " pri=" << (uint32_t)priority
                    << " buffer full. Packet Dropped!");
        return;
    }

    SendPacket(packet, inPort, outPort, priority);
}

void UbSwitch::ForceShortestPathRouting(Ptr<Packet> packet, const UbDatalinkPacketHeader &parsedHeader)
{
    UbDatalinkPacketHeader modifiedHeader = parsedHeader;
    modifiedHeader.SetRoutingPolicy(true);  // Force shortest path

    UbDatalinkPacketHeader tempHeader;
    packet->RemoveHeader(tempHeader);
    packet->AddHeader(modifiedHeader);
}

void UbSwitch::ParseURMAPacketHeader(Ptr<Packet> packet, ParsedURMAHeaders &headers)
{
    // Parse headers needed by switch (store all that must be removed anyway)
    // Order: DLH -> NH -> IPv4 -> UDP -> TP -> ...
    packet->RemoveHeader(headers.datalinkPacketHeader);
    packet->RemoveHeader(headers.networkHeader);
    packet->RemoveHeader(headers.ipv4Header);
    packet->RemoveHeader(headers.udpHeader);
    packet->PeekHeader(headers.transportHeader);
    packet->AddHeader(headers.udpHeader);
    packet->AddHeader(headers.ipv4Header);
    packet->AddHeader(headers.networkHeader);
    packet->AddHeader(headers.datalinkPacketHeader);
}

void UbSwitch::ParseLdstPacketHeader(Ptr<Packet> packet, ParsedLdstHeaders &headers)
{
    // Parse only headers needed by switch for routing and forwarding
    // Order: DLH -> CNA16NH -> dummyTA -> ...
    // Note: dummyTA can be either UbCompactTransactionHeader or UbCompactAckTransactionHeader
    packet->RemoveHeader(headers.datalinkPacketHeader);
    packet->RemoveHeader(headers.cna16NetworkHeader);
    packet->PeekHeader(headers.dummyTransactionHeader);
    packet->AddHeader(headers.cna16NetworkHeader);
    packet->AddHeader(headers.datalinkPacketHeader);
}

void UbSwitch::GetURMARoutingKey(const ParsedURMAHeaders &headers, RoutingKey &rtKey)
{
    rtKey.sip = headers.ipv4Header.GetSource().Get();
    rtKey.dip = headers.ipv4Header.GetDestination().Get();
    rtKey.sport = headers.udpHeader.GetSourcePort();
    rtKey.dport = headers.udpHeader.GetDestinationPort();
    rtKey.priority = headers.datalinkPacketHeader.GetPacketVL();
    rtKey.useShortestPath = headers.datalinkPacketHeader.GetRoutingPolicy();
    rtKey.usePacketSpray = headers.datalinkPacketHeader.GetLoadBalanceMode();
}

void UbSwitch::GetLdstRoutingKey(const ParsedLdstHeaders &headers, RoutingKey &rtKey)
{
    uint16_t dCna = headers.cna16NetworkHeader.GetDcna();
    uint16_t sCna = headers.cna16NetworkHeader.GetScna();
    uint32_t snode = utils::Cna16ToNodeId(sCna);
    uint32_t dnode = utils::Cna16ToNodeId(dCna);
    uint16_t sport = utils::Cna16ToPortId(sCna);
    uint16_t dport = 0;
    uint16_t lb = headers.cna16NetworkHeader.GetLb();
    rtKey.sip = utils::NodeIdToIp(snode, sport).Get();
    rtKey.dip = utils::NodeIdToIp(dnode, dport).Get();
    rtKey.sport = lb;
    rtKey.dport = dport;
    rtKey.priority = headers.datalinkPacketHeader.GetPacketVL();
    rtKey.useShortestPath = headers.datalinkPacketHeader.GetRoutingPolicy();
    rtKey.usePacketSpray = headers.datalinkPacketHeader.GetLoadBalanceMode();
}

/**
 * @brief Packet enters VOQ from input port
 * Place packet into VOQ[outPort][priority][inPort] and update buffer statistics
 */
void UbSwitch::SendPacket(Ptr<Packet> packet, uint32_t inPort, uint32_t outPort, uint32_t priority)
{
    auto node = GetObject<Node>();
    Ptr<UbPort> recvPort = DynamicCast<ns3::UbPort>(node->GetDevice(inPort));

    m_voq[outPort][priority][inPort]->Push(packet);

    // Update both InPort and OutPort view buffer statistics
    m_queueManager->PushToVoq(inPort, outPort, priority, packet->GetSize());

    if (IsPFCEnable()) {
        recvPort->m_flowControl->HandleReceivedPacket(packet);
    }

    Ptr<UbPort> port = DynamicCast<ns3::UbPort>(node->GetDevice(outPort));
    port->TriggerTransmit();
}

/**
 * @brief Send control frame (PFC/CBFC) on specified port
 * Control frames use highest priority (0) and are locally generated (inPort = outPort)
 */
void UbSwitch::SendControlFrame(Ptr<Packet> packet, uint32_t portId)
{
    // Control frames: inPort = outPort, priority = 0
    uint32_t priority = 0;

    // Check if high priority buffer has space (should rarely be full)
    if (!m_queueManager->CheckInPortSpace(portId, priority, packet->GetSize())) {
        NS_LOG_WARN("High priority buffer full! Port=" << portId
                    << " This should rarely happen for control frames.");
    }

    SendPacket(packet, portId, portId, priority);
}

/**
 * @brief Packet dequeued from VOQ and moved to EgressQueue
 * Called by allocator when packet is selected from VOQ and placed into EgressQueue.
 * Updates buffer statistics to reflect packet leaving VOQ.
 */
void UbSwitch::NotifySwitchDequeue(uint16_t inPortId, uint32_t outPort, uint32_t priority, Ptr<Packet> packet)
{
    // Update buffer statistics for all packets (including control frames)
    m_queueManager->PopFromVoq(inPortId, outPort, priority, packet->GetSize());

    // Only data packets trigger congestion control
    UbPacketType_t packetType = GetPacketType(packet);
    if (packetType != UB_CONTROL_FRAME) {
        NS_LOG_DEBUG("[QMU] Node:" << GetObject<Node>()->GetId()
              << " port:" << outPort
              << " VOQ outPort buffer:" << m_queueManager->GetTotalOutPortBufferUsed(outPort));
        m_congestionCtrl->SwitchForwardPacket(inPortId, outPort, packet);
    }
}

bool UbSwitch::IsCBFCEnable()
{
    return m_flowControlType == FcType::CBFC;
}

bool UbSwitch::IsCBFCSharedEnable()
{
    return m_flowControlType == FcType::CBFC_SHARED_CRD;
}

bool UbSwitch::IsPFCEnable()
{
    return m_flowControlType == FcType::PFC;
}

Ptr<UbQueueManager> UbSwitch::GetQueueManager()
{
    return m_queueManager;
}

void UbSwitch::SetCongestionCtrl(Ptr<UbCongestionControl> congestionCtrl)
{
    m_congestionCtrl = congestionCtrl;
}

Ptr<UbCongestionControl> UbSwitch::GetCongestionCtrl()
{
    return m_congestionCtrl;
}

void UbSwitch::LastPacketTraversesNotify(uint32_t nodeId, UbTransportHeader ubTpHeader)
{
    m_traceLastPacketTraversesNotify(nodeId, ubTpHeader);
}

}  // namespace ns3
