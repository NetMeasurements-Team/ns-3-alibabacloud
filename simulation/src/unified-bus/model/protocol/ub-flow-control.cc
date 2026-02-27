// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-port.h"
#include "ns3/ub-link.h"
#include "ns3/log.h"
#include "ns3/ub-network-address.h"
using namespace utils;

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(UbFlowControl);
NS_LOG_COMPONENT_DEFINE("UbFlowControl");

TypeId UbFlowControl::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbFlowControl").SetParent<Object>().AddConstructor<UbFlowControl>();
    return tid;
}

TypeId UbCbfc::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbCbfc").SetParent<UbFlowControl>().AddConstructor<UbCbfc>();
    return tid;
}

void UbCbfc::Init(uint8_t flitLen, uint8_t nFlitPerCell, uint8_t retCellGrainDataPacket,
                  uint8_t retCellGrainControlPacket, int32_t portTxfree,
                  uint32_t nodeId, uint32_t portId)
{
    // 基础参数配置
    m_cbfcCfg = new cbfcCfg_t;
    m_cbfcCfg->m_flitLen = flitLen;
    m_cbfcCfg->m_nFlitPerCell = nFlitPerCell;
    m_cbfcCfg->m_retCellGrainDataPacket = retCellGrainDataPacket;
    m_cbfcCfg->m_retCellGrainControlPacket = retCellGrainControlPacket;
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();
    m_crdTxfree.resize(ubVlNum, portTxfree);
    m_crdToReturn.resize(ubVlNum, 0);
    m_nodeId = nodeId;
    m_portId = portId;
    NS_LOG_DEBUG("NodeId: " << m_nodeId << "PortId: " << m_portId << "Init Cbfc");

    NS_LOG_DEBUG("m_crdTxfree[*]: " << m_crdTxfree[0]);
}

void UbCbfc::DoDispose()
{
    NS_LOG_FUNCTION(this);
    delete m_cbfcCfg;

    Object::DoDispose();
}

bool UbCbfc::IsFcLimited(Ptr<UbIngressQueue> ingressQ)
{
    uint32_t nextPktSize = 0;
    if (ingressQ->GetIngressQueueType() == IngressQueueType::VOQ && ingressQ->IsControlFrame()) {
        NS_LOG_DEBUG("is crd pkt");
        return false;
    }

    nextPktSize = ingressQ->GetNextPacketSize();
    NS_LOG_DEBUG("nextPktSize:" << nextPktSize);

    int32_t consumeCellNum = ceil((float)nextPktSize / (m_cbfcCfg->m_flitLen * m_cbfcCfg->m_nFlitPerCell));
    if (m_crdTxfree[ingressQ->GetIngressPriority()] < consumeCellNum) {
        NS_LOG_INFO("Flow Control Credit Limited,outPort:{" << ingressQ->GetOutPortId() << "} VL:{"
                                                            << ingressQ->GetIngressPriority() << "}");
        NS_LOG_DEBUG("m_crdTxfree[ " << ingressQ->GetIngressPriority() << " ]: " << m_crdTxfree[ingressQ->GetIngressPriority()]
                                     << "is insufficient");
        return true;
    }
    NS_LOG_DEBUG("m_crdTxfree[ " << ingressQ->GetIngressPriority() << " ]: " << m_crdTxfree[ingressQ->GetIngressPriority()]
                                 << "is enough");

    return false;
}

void UbCbfc::HandleReleaseOccupiedFlowControl(Ptr<Packet> p, uint32_t inPortId, uint32_t outPortId)
{
    if (inPortId != outPortId) { // 转发的报文
        Ptr<Packet> cbfcPkt = ReleaseOccupiedCrd(p, inPortId);
        if (cbfcPkt != nullptr) {
            SendCrdAck(cbfcPkt, inPortId);
        }
    }
}

void UbCbfc::HandleSentPacket(Ptr<Packet> p, Ptr<UbIngressQueue> ingressQ)
{
    if (ingressQ->IsControlFrame()) {
        NS_LOG_DEBUG("is crd pkt");
        return;
    }
    CbfcConsumeCrd(p); // 计算消耗的信用证
}

void UbCbfc::HandleReceivedControlPacket(Ptr<Packet> p)
{
    CbfcRestoreCrd(p);
}

void UbCbfc::HandleReceivedPacket(Ptr<Packet> p)
{
    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(m_portId));

    Ptr<Packet> cbfcPkt = ReleaseOccupiedCrd(p, m_portId);
    if (cbfcPkt != nullptr) {
        SendCrdAck(cbfcPkt, m_portId);
    }
}

int32_t UbCbfc::GetCrdToReturn(uint8_t vlId)
{
    int32_t crdToReturnCell = m_crdToReturn[vlId];

    return crdToReturnCell;
}

void UbCbfc::SetCrdToReturn(uint8_t vlId, int32_t consumeCell, Ptr<UbPort> targetPort)
{
    NS_LOG_DEBUG("NodeId: " << targetPort->GetNode()->GetId() << " PortId: " << targetPort->GetIfIndex());
    int32_t &vlRxbuf = m_crdToReturn[vlId];
    NS_LOG_DEBUG("before set m_crdToReturn[ " << (uint32_t)vlId << " ]: " << m_crdToReturn[vlId]
                                              << " consumeCell: " << consumeCell);

    vlRxbuf += consumeCell;
    NS_LOG_DEBUG("after set m_crdToReturn[ " << (uint32_t)vlId << " ]: " << m_crdToReturn[vlId]);
}

void UbCbfc::UpdateCrdToReturn(uint8_t vlId, int32_t consumeCell, Ptr<UbPort> targetPort)
{
    NS_LOG_DEBUG("NodeId: " << targetPort->GetNode()->GetId() << " PortId: " << targetPort->GetIfIndex()
                 << " vlId: " << (uint32_t)vlId);

    int32_t &vlRxbuf = m_crdToReturn[vlId];
    NS_LOG_DEBUG("before set: "
                 << "m_crdToReturn[ " << (uint32_t)vlId << " ]: " << m_crdToReturn[vlId]);
    if (vlRxbuf >= consumeCell) {
        vlRxbuf -= consumeCell;
        NS_LOG_DEBUG("after set: "
                     << "m_crdToReturn[ " << (uint32_t)vlId << " ]: " << m_crdToReturn[vlId]);
    }
}

bool UbCbfc::CbfcConsumeCrd(Ptr<Packet> p)
{
    uint32_t pktSize = p->GetSize();
    NS_LOG_DEBUG("NodeId: " << m_nodeId << " PortId: " << m_portId << " pktSize: " << pktSize);
    UbDatalinkPacketHeader pktHeader;
    p->PeekHeader(pktHeader);
    uint8_t vlId = pktHeader.GetPacketVL();
    int32_t consumeCellNum = ceil((float)(pktSize) / (m_cbfcCfg->m_flitLen * m_cbfcCfg->m_nFlitPerCell));
    NS_LOG_DEBUG("befor consume, m_crdTxfree[ " << (uint32_t)vlId << " ]: " << m_crdTxfree[vlId]);
    if (m_crdTxfree[vlId] >= consumeCellNum) {
        m_crdTxfree[vlId] -= consumeCellNum;
        NS_LOG_DEBUG("left m_crdTxfree[ " << (uint32_t)vlId << " ]: " << m_crdTxfree[vlId]);
        return true;
    }

    return false;
}

bool UbCbfc::CbfcRestoreCrd(Ptr<Packet> p)
{
    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(m_portId));

    NS_LOG_DEBUG("NodeId: " << m_nodeId << " PortId: " << m_portId);
    port->ResetCredits();
    UbDatalinkControlCreditHeader crdHeader = UbDataLink::ParseCreditHeader(p, port);

    uint32_t resumeCellGrainNum = 0;
    bool ret = false;
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();

    for (int index = 0; index < ubVlNum; index++) {
        NS_LOG_DEBUG("port m_credits[ " << (uint32_t)index << " ]: " << (uint32_t)port->m_credits[index]);
    }

    for (int index = 0; index < ubVlNum; index++) {
        if (port->m_credits[index] > 0) {
            resumeCellGrainNum = port->m_credits[index];
            NS_LOG_DEBUG("before resume m_crdTxfree[ " << (uint32_t)index << " ]: " << m_crdTxfree[index]);
            m_crdTxfree[index] += resumeCellGrainNum * m_cbfcCfg->m_retCellGrainControlPacket;  // 粒度数量 * 粒度大小
            NS_LOG_DEBUG("left m_crdTxfree[ " << (uint32_t)index << " ]: " << m_crdTxfree[index]);
            ret = true;
        }
    }

    Simulator::ScheduleNow(&UbPort::TriggerTransmit, port);
    return ret;
}

void UbCbfc::SendCrdAck(Ptr<Packet> cbfcPkt, uint32_t targetPortId)
{
    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    
    node->GetObject<UbSwitch>()->SendControlFrame(cbfcPkt, targetPortId);
    NS_LOG_DEBUG("send crd pkt");
}

Ptr<Packet> UbCbfc::ReleaseOccupiedCrd(Ptr<Packet> p, uint32_t targetPortId)
{
    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    
    Ptr<Packet> cbfcPkt = nullptr;
    bool shouldReturnCredit = false;
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(targetPortId));
    uint32_t pktSize = p->GetSize();
    UbDatalinkPacketHeader pktHeader;
    p->PeekHeader(pktHeader);
    uint8_t vlId = pktHeader.GetPacketVL();
    NS_LOG_DEBUG("NodeId: " << node->GetId() << " PortId: " << port->GetIfIndex()
                 << " vlId: " << (uint32_t)vlId << " pktSize: " << pktSize);

    int32_t consumeCellNum = ceil((float)(pktSize) / (m_cbfcCfg->m_flitLen * m_cbfcCfg->m_nFlitPerCell));

    auto flowControl = DynamicCast<UbCbfc>(port->m_flowControl);
    flowControl->SetCrdToReturn(vlId, consumeCellNum, port);
    int32_t leftCrdToReturn = 0;
    int32_t crdSndGrains = 0;
    port->ResetCredits();

    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();

    for (int index = 0; index < ubVlNum; index++) {
        leftCrdToReturn = flowControl->GetCrdToReturn(index);
        if (leftCrdToReturn >= m_cbfcCfg->m_retCellGrainControlPacket) {
            crdSndGrains = leftCrdToReturn / m_cbfcCfg->m_retCellGrainControlPacket;
            NS_LOG_DEBUG("index: " << (uint32_t)index << " m_cbfcCfg->m_retCellGrainControlPacket: "
                         << (uint32_t)m_cbfcCfg->m_retCellGrainControlPacket
                         << " crdSndGrains: " << crdSndGrains);
            port->SetCredits(index, crdSndGrains);
            flowControl->UpdateCrdToReturn(index, crdSndGrains * m_cbfcCfg->m_retCellGrainControlPacket, port);
            shouldReturnCredit = true;
        }
    }

    for (int index = 0; index < ubVlNum; index++) {
        NS_LOG_DEBUG("SndCredits[ " << (uint32_t)index << " ]: " << (uint32_t)port->m_credits[index]);
    }

    if (shouldReturnCredit) {
        cbfcPkt = UbDataLink::GenControlCreditPacket(port->m_credits);
    }

    return cbfcPkt;
}

FcType UbCbfc::GetFcType()
{
    return m_fcType;
}


TypeId UbCbfcSharedCredit::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbCbfcSharedCredit")
        .SetParent<UbCbfc>()
        .AddConstructor<UbCbfcSharedCredit>();
    return tid;
}

void UbCbfcSharedCredit::Init(uint8_t flitLen, uint8_t nFlitPerCell, uint8_t retCellGrainDataPacket,
                            uint8_t retCellGrainControlPacket, int32_t reservedPerVlCells,
                            int32_t sharedInitCells, uint32_t nodeId, uint32_t portId)
{
    UbCbfc::Init(flitLen, nFlitPerCell, retCellGrainDataPacket, retCellGrainControlPacket,
                 reservedPerVlCells, nodeId, portId);

    m_shareCrd = sharedInitCells;
    m_reservedPerVlCells = reservedPerVlCells;

    NS_LOG_DEBUG("NodeId: " << m_nodeId << "PortId: " << m_portId << "Init CbfcSharedMode");
    NS_LOG_DEBUG("m_shareCrd: " << m_shareCrd << " reservedPerVlCells: " << m_reservedPerVlCells);
}

FcType UbCbfcSharedCredit::GetFcType()
{
    return FcType::CBFC_SHARED_CRD;
}

bool UbCbfcSharedCredit::IsFcLimited(Ptr<UbIngressQueue> ingressQ)
{
    uint32_t nextPktSize = 0;

    if (ingressQ->GetIngressQueueType() == IngressQueueType::VOQ && ingressQ->IsControlFrame()) {
        NS_LOG_DEBUG("is crd pkt");
        return false;
    }
    nextPktSize = ingressQ->GetNextPacketSize();
    NS_LOG_DEBUG("nextPktSize: " << nextPktSize);

    const int32_t consumeCellNum =
        ceil((float)nextPktSize / (m_cbfcCfg->m_flitLen * m_cbfcCfg->m_nFlitPerCell));

    const uint8_t vlId = ingressQ->GetIngressPriority();
    const int32_t totalAvail = m_shareCrd + m_crdTxfree[vlId];

    if (totalAvail < consumeCellNum) {
        NS_LOG_INFO("Flow Control Credit Limited,outPort:{" << ingressQ->GetOutPortId()
                                                            << "} VL:{" << (uint32_t)vlId << "}");
        NS_LOG_DEBUG("TotalAvailable[ " << (uint32_t)vlId << " ]: " << totalAvail
                                     << " is insufficient. Need: " << consumeCellNum);
        return true;
    }
    NS_LOG_DEBUG("TotalAvailable[ " << (uint32_t)vlId << " ]: " << totalAvail
                                 << " is enough. Need: " << consumeCellNum);

    return false;
}

void UbCbfcSharedCredit::HandleSentPacket(Ptr<Packet> p, Ptr<UbIngressQueue> ingressQ)
{
    if (ingressQ->IsControlFrame()) {
        NS_LOG_DEBUG("is crd pkt");
        return;
    }
    CbfcSharedConsumeCrd(p);
}

void UbCbfcSharedCredit::HandleReceivedControlPacket(Ptr<Packet> p)
{
    CbfcSharedRestoreCrd(p);
}

// CBFC 信用共享模式：信用消耗（Consume）逻辑
// 1. 计算当前报文需要消耗的 Cell 数量
// 2. 优先尝试从共享信用池 (m_shareCrd) 中扣除
// 3. 如果共享池信用充足，完成消耗并返回
// 4. 如果共享池信用不足，将共享池清零，并从该 VL 独占的信用证 (m_crdTxfree[vlId]) 中补充扣除剩余部分
// 5. 如果独占信用证依然不足，记录警告并归零
bool UbCbfcSharedCredit::CbfcSharedConsumeCrd(Ptr<Packet> p)
{
    const uint32_t pktSize = p->GetSize();
    NS_LOG_DEBUG("NodeId: " << m_nodeId << " PortId: " << m_portId << " pktSize: " << pktSize);
    UbDatalinkPacketHeader pktHeader;
    p->PeekHeader(pktHeader);
    const uint8_t vlId = pktHeader.GetPacketVL();

    const int32_t consumeCellNum =
        ceil((float)pktSize / (m_cbfcCfg->m_flitLen * m_cbfcCfg->m_nFlitPerCell));
    
    NS_LOG_DEBUG("befor consume, m_shareCrd: " << m_shareCrd << " m_crdTxfree[ " << (uint32_t)vlId << " ]: " << m_crdTxfree[vlId]);

    if (m_shareCrd >= consumeCellNum) {
        m_shareCrd -= consumeCellNum;
        NS_LOG_DEBUG("left m_shareCrd: " << m_shareCrd << " left m_crdTxfree[ " << (uint32_t)vlId << " ]: " << m_crdTxfree[vlId]);
        return true;
    }

    const int32_t remainder = consumeCellNum - m_shareCrd;
    m_shareCrd = 0;

    if (m_crdTxfree[vlId] >= remainder) {
        m_crdTxfree[vlId] -= remainder;
        NS_LOG_DEBUG("left m_shareCrd: " << m_shareCrd << " left m_crdTxfree[ " << (uint32_t)vlId << " ]: " << m_crdTxfree[vlId]);
        return true;
    }

    NS_LOG_WARN("CbfcSharedConsumeCrd underflow, vlId: " << (uint32_t)vlId);
    m_crdTxfree[vlId] = 0;
    return false;
}

// CBFC 信用共享模式：信用归还（Restore）逻辑
// 1. 解析控制报文，统计当前端口收到的所有 VL 归还的信用证总数
// 2. 将所有归还的信用证统一填充到共享信用池 (m_shareCrd) 中
// 3. 遍历所有优先级队列 (VL)，检查各 VL 的独占信用证是否达到预留阈值 (m_reservedPerVlCells)
// 4. 若某个 VL 的独占信用不足，则从共享池中拨付信用进行补充，直到达到阈值或共享池耗尽
// 4.1 补充顺序：可根据实际场景自定义。目前实现为按照 VL 优先级索引从小到大依次进行补充
// 5. 触发端口的发送流程 (TriggerTransmit) 以尝试发送因信用不足而积压的报文
bool UbCbfcSharedCredit::CbfcSharedRestoreCrd(Ptr<Packet> p)
{
    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(m_portId));

    NS_LOG_DEBUG("NodeId: " << m_nodeId << " PortId: " << m_portId);
    port->ResetCredits();
    UbDatalinkControlCreditHeader crdHeader = UbDataLink::ParseCreditHeader(p, port);

    uint32_t resumeCellGrainNum = 0;
    bool ret = false;
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();

    for (int index = 0; index < ubVlNum; index++) {
        NS_LOG_DEBUG("port m_credits[ " << (uint32_t)index << " ]: " << (uint32_t)port->m_credits[index]);
    }

    int32_t totalReturned = 0;

    for (int index = 0; index < ubVlNum; index++) {
        if (port->m_credits[index] > 0) {
            resumeCellGrainNum = port->m_credits[index];
            int32_t cells = resumeCellGrainNum * m_cbfcCfg->m_retCellGrainControlPacket;
            totalReturned += cells;
        }
    }

    if (totalReturned > 0) {
        NS_LOG_DEBUG("before resume Share: " << m_shareCrd << " TotalReturned: " << totalReturned);
        m_shareCrd += totalReturned;
        
        for (int vl = 0; vl < ubVlNum; vl++) {
            if (m_shareCrd <= 0) break;

            if (m_crdTxfree[vl] < m_reservedPerVlCells) {
                int32_t needed = m_reservedPerVlCells - m_crdTxfree[vl];
                int32_t canGive = std::min(needed, m_shareCrd);
                
                NS_LOG_DEBUG("Refill VL " << vl << " before: " << m_crdTxfree[vl] << " add: " << canGive);
                m_crdTxfree[vl] += canGive;
                m_shareCrd -= canGive;
                NS_LOG_DEBUG("Refill VL " << vl << " after: " << m_crdTxfree[vl]);
            }
        }
        NS_LOG_DEBUG("left Share: " << m_shareCrd);
        ret = true;
    }

    Simulator::ScheduleNow(&UbPort::TriggerTransmit, port);
    return ret;
}


TypeId UbPfc::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbPfc").SetParent<UbFlowControl>().AddConstructor<UbPfc>();
    return tid;
}

FcType UbPfc::GetFcType()
{
    return m_fcType;
}

void UbPfc::Init(int32_t portpfcUpThld, int32_t portpfcLowThld, uint32_t nodeId, uint32_t portId)
{
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();

    m_pfcCfg = new pfcCfg_t;
    m_pfcStatus = new pfcStatus_t(ubVlNum);
    m_pfcCfg->m_portpfcUpThld = portpfcUpThld;    // 0.3 * m_pfcPortTotThld
    m_pfcCfg->m_portpfcLowThld = portpfcLowThld;  // 0.8 * m_portpfcUpThld

    m_nodeId = nodeId;
    m_portId = portId;
    NS_LOG_DEBUG("NodeId: " << m_nodeId << "PortId: " << m_portId << "Init Pfc");
}

void UbPfc::DoDispose()
{
    NS_LOG_FUNCTION(this);
    delete m_pfcCfg;
    delete m_pfcStatus;
    Object::DoDispose();
}

bool UbPfc::IsFcLimited(Ptr<UbIngressQueue> ingressQ)
{
    if (ingressQ->GetIngressQueueType() == IngressQueueType::VOQ && ingressQ->IsControlFrame()) {
        NS_LOG_DEBUG("is Pfc pkt");
        return false;
    }
    if (m_pfcStatus->m_portCredits[ingressQ->GetIngressPriority()] == 0) {
        NS_LOG_INFO("Flow Control Pfc Limited! NodeId: " << m_nodeId << ",outPort:{" << ingressQ->GetOutPortId() << "} VL:{"
                    << ingressQ->GetIngressPriority() << "}");
        return true;  // 不允许发送
    }

    return false;
}

void UbPfc::HandleReleaseOccupiedFlowControl(Ptr<Packet> p, uint32_t inPortId, uint32_t outPortId)
{
    if (inPortId != outPortId) { // 转发的报文
        Ptr<Packet> pfcPkt = CheckPfcThreshold(p, inPortId);
        if (pfcPkt != nullptr) {
            SendPfc(pfcPkt, inPortId);
        }
    }
}

void UbPfc::HandleSentPacket(Ptr<Packet> p, Ptr<UbIngressQueue> ingressQ)
{
    // do nothing
}

void UbPfc::HandleReceivedControlPacket(Ptr<Packet> p)
{
    UpdatePfcStatus(p);
}

void UbPfc::HandleReceivedPacket(Ptr<Packet> p)
{
    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(m_portId));

    Ptr<Packet> pfcPkt = CheckPfcThreshold(p, m_portId);
    if (pfcPkt != nullptr) {
        SendPfc(pfcPkt, m_portId);
    }
    return;
}

bool UbPfc::UpdatePfcStatus(Ptr<Packet> p)
{
    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(m_portId));

    UbDatalinkControlCreditHeader pfcHeader = UbDataLink::ParseCreditHeader(p, port);
    bool ret = false;
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();
    for (int index = 0; index < ubVlNum; index++) {
        if (m_pfcStatus->m_portCredits[index] != port->m_credits[index]) {
            m_pfcStatus->m_portCredits[index] = port->m_credits[index];
            ret = true;
        }
    }

    NS_LOG_DEBUG("Recv Pfc uid: " << p->GetUid() << " NodeId: " << port->GetNode()->GetId() << " PortId: "
                << port->GetIfIndex() << " m_pfcStatus->m_portCredits:{"
                << (uint32_t)m_pfcStatus->m_portCredits[0] << " "
                << (uint32_t)m_pfcStatus->m_portCredits[1] << " "
                << (uint32_t)m_pfcStatus->m_portCredits[2] << " "
                << (uint32_t)m_pfcStatus->m_portCredits[3] << " "
                << (uint32_t)m_pfcStatus->m_portCredits[4] << " "
                << (uint32_t)m_pfcStatus->m_portCredits[5] << " "
                << (uint32_t)m_pfcStatus->m_portCredits[6] << " "
                << (uint32_t)m_pfcStatus->m_portCredits[7] << " "
                << (uint32_t)m_pfcStatus->m_portCredits[8] << " "
                << (uint32_t)m_pfcStatus->m_portCredits[9] << " "
                << (uint32_t)m_pfcStatus->m_portCredits[10] << " "
                << (uint32_t)m_pfcStatus->m_portCredits[11] << " "
                << (uint32_t)m_pfcStatus->m_portCredits[12] << " "
                << (uint32_t)m_pfcStatus->m_portCredits[13] << " "
                << (uint32_t)m_pfcStatus->m_portCredits[14] << " "
                << (uint32_t)m_pfcStatus->m_portCredits[15] << "}");

    Simulator::ScheduleNow(&UbPort::TriggerTransmit, port);

    return ret;
}

void UbPfc::SendPfc(Ptr<Packet> pfcPacket, uint32_t targetPortId)
{
    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(targetPortId));
    
    node->GetObject<UbSwitch>()->SendControlFrame(pfcPacket, targetPortId);
    
    auto flowControl = DynamicCast<UbPfc>(port->m_flowControl);
    flowControl->m_pfcStatus->m_pfcSndCnt++;
}

Ptr<Packet> UbPfc::CheckPfcThreshold(Ptr<Packet> p, uint32_t portId)
{
    Ptr<Packet> pfcPkt = nullptr;
    Ptr<Node> node = NodeList::GetNode(m_nodeId);

    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(portId));
    NS_LOG_DEBUG("NodeId: " << node->GetId() << " PortId: " << portId);

    uint32_t hi_thresh = m_pfcCfg->m_portpfcUpThld;
    uint32_t lo_thresh = m_pfcCfg->m_portpfcLowThld;
    auto flowControl = DynamicCast<UbPfc>(port->m_flowControl);

    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();
    for (int pri = 0; pri < ubVlNum; pri++) {
        auto queueManager = node->GetObject<UbSwitch>()->GetQueueManager();
        // 使用InPort视图检查入端口拥塞状态
        uint64_t inPortBufUsed = queueManager->GetInPortBufferUsed(portId, pri);
        
        if (inPortBufUsed < lo_thresh) {
            NS_LOG_DEBUG("InPortBuf[" << pri << "]: " << inPortBufUsed
                         << " < lo_thresh: " << lo_thresh << " m_pfcSndCredits: "
                         << (uint32_t)flowControl->m_pfcStatus->m_pfcSndCredits[pri]);
            flowControl->m_pfcStatus->m_pfcSndCredits[pri] = UB_CREDIT_MAX_VALUE;
        }
        if (inPortBufUsed >= hi_thresh) {
            NS_LOG_DEBUG("InPortBuf[" << pri << "]: " << inPortBufUsed
                         << " >= hi_thresh: " << hi_thresh << " m_pfcSndCredits = 0");
            flowControl->m_pfcStatus->m_pfcSndCredits[pri] = 0;
        }
    }

    if (flowControl->m_pfcStatus->m_pfcSndCredits == flowControl->m_pfcStatus->m_pfcLastSndCredits) {
        NS_LOG_DEBUG("State Preservation");
        return pfcPkt;
    }

    port->ResetCredits();
    for (int pri = 0; pri < ubVlNum; pri++) {
        if (flowControl->m_pfcStatus->m_pfcSndCredits[pri]) {
            port->SetCredits(pri, flowControl->m_pfcStatus->m_pfcSndCredits[pri]);
        }
    }

    NS_LOG_DEBUG("m_pfcStatus->m_pfcSndCredits: ");
    for (int index = 0; index < ubVlNum; index++) {
        NS_LOG_DEBUG((uint32_t)flowControl->m_pfcStatus->m_pfcSndCredits[index] << " ");
    }

    flowControl->m_pfcStatus->m_pfcLastSndCredits = flowControl->m_pfcStatus->m_pfcSndCredits;

    NS_LOG_DEBUG("Port credits changed. NodeId: " << node->GetId() << " inPort:{" << portId
            << "} VL:{" << (uint32_t)port->m_credits[0] << " " << (uint32_t)port->m_credits[1] << " "
            << (uint32_t)port->m_credits[2] << " " << (uint32_t)port->m_credits[3] << " "
            << (uint32_t)port->m_credits[4] << " " << (uint32_t)port->m_credits[5] << " "
            << (uint32_t)port->m_credits[6] << " " << (uint32_t)port->m_credits[7] << " "
            << (uint32_t)port->m_credits[8] << " " << (uint32_t)port->m_credits[9] << " "
            << (uint32_t)port->m_credits[10] << " " << (uint32_t)port->m_credits[11] << " "
            << (uint32_t)port->m_credits[12] << " " << (uint32_t)port->m_credits[13] << " "
            << (uint32_t)port->m_credits[14] << " " << (uint32_t)port->m_credits[15] << "}");
    pfcPkt = UbDataLink::GenControlCreditPacket(port->m_credits);
    NS_LOG_DEBUG("Create pfcpkt uid: " << pfcPkt->GetUid());

    return pfcPkt;
}

}  // namespace ns3
