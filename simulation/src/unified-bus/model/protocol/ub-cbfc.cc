// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-cbfc.h"
#include "ns3/ub-port.h"
#include "ns3/ub-link.h"
#include "ns3/log.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-utils.h"
#include "ns3/ub-datatype.h"
using namespace utils;

namespace ns3 {
LogComponent& GetUbFlowControlLogComponent();
static LogComponent& g_log = GetUbFlowControlLogComponent();

namespace {

bool
IsValidCbfcCellGrain(uint8_t grain)
{
    switch (grain)
    {
    case 1:
    case 2:
    case 4:
    case 8:
    case 16:
    case 32:
    case 64:
    case 128:
        return true;
    default:
        return false;
    }
}

} // namespace

TypeId UbCbfc::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbCbfc")
            .SetParent<UbFlowControl>()
            .AddConstructor<UbCbfc>()
            .AddTraceSource("ControlCreditRestoreNotify",
                            "Observed restored control credits at the real flow-control receive path.",
                            MakeTraceSourceAccessor(&UbCbfc::m_traceControlCreditRestoreNotify),
                            "ns3::UbCbfc::ControlCreditRestoreNotify");
    return tid;
}

void UbCbfc::Init(uint8_t flitLen, uint8_t nFlitPerCell, uint8_t retCellGrainDataPacket,
                  uint8_t retCellGrainControlPacket, int32_t portTxfree, int32_t ctrlCrdRtrThldCells,
                  uint32_t nodeId, uint32_t portId)
{
    // 基础参数配置
    m_cbfcCfg.m_flitLen = flitLen;
    m_cbfcCfg.m_nFlitPerCell = nFlitPerCell;
    m_cbfcCfg.m_retCellGrainDataPacket = retCellGrainDataPacket;
    m_cbfcCfg.m_retCellGrainControlPacket = retCellGrainControlPacket;
    m_cbfcCfg.m_ctrlCrdRtrThldCells = ctrlCrdRtrThldCells;
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();
    m_crdTxfree.resize(ubVlNum, portTxfree);
    m_crdToReturn.resize(ubVlNum, 0);
    m_creditBlockedLast.resize(ubVlNum, false);
    m_nodeId = nodeId;
    m_portId = portId;
    NS_LOG_DEBUG("NodeId: " << m_nodeId
                 << " PortId: " << m_portId
                 << " Init Cbfc"
                 << " flitLen=" << static_cast<uint32_t>(m_cbfcCfg.m_flitLen)
                 << " flitsPerCell=" << static_cast<uint32_t>(m_cbfcCfg.m_nFlitPerCell)
                 << " retCellGrainData=" << static_cast<uint32_t>(m_cbfcCfg.m_retCellGrainDataPacket)
                 << " retCellGrainCtrl=" << static_cast<uint32_t>(m_cbfcCfg.m_retCellGrainControlPacket)
                 << " ctrlCrdRtrThld=" << m_cbfcCfg.m_ctrlCrdRtrThldCells
                 << " initCreditPerVl=" << portTxfree);

    NS_ABORT_MSG_IF(m_cbfcCfg.m_flitLen == 0 || m_cbfcCfg.m_nFlitPerCell == 0,
                    "CBFC requires non-zero cell geometry (flitLen and nFlitPerCell)");
    NS_ABORT_MSG_IF(m_cbfcCfg.m_retCellGrainDataPacket == 0 ||
                        m_cbfcCfg.m_retCellGrainControlPacket == 0,
                    "CBFC requires non-zero return grain for both data and control packets");
    NS_ABORT_MSG_IF(!IsValidCbfcCellGrain(m_cbfcCfg.m_nFlitPerCell),
                    "CBFC flits-per-cell must be one of {1,2,4,8,16,32,64,128}");
    NS_ABORT_MSG_IF(!IsValidCbfcCellGrain(m_cbfcCfg.m_retCellGrainDataPacket),
                    "CBFC data-packet credit grain must be one of {1,2,4,8,16,32,64,128}");
    NS_ABORT_MSG_IF(!IsValidCbfcCellGrain(m_cbfcCfg.m_retCellGrainControlPacket),
                    "CBFC control-packet credit grain must be one of {1,2,4,8,16,32,64,128}");
    NS_ABORT_MSG_IF(m_cbfcCfg.m_ctrlCrdRtrThldCells <= 0,
                    "CBFC requires a positive control-frame force threshold");
    NS_ABORT_MSG_IF(portTxfree < 0,
                    "CBFC requires non-negative initial transmit credit per VL");

    if (portTxfree == 0)
    {
        NS_LOG_WARN("CBFC initial credit is zero on NodeId=" << m_nodeId
                    << " PortId=" << m_portId
                    << " initCreditPerVl=" << portTxfree);
    }
}

void UbCbfc::DoDispose()
{
    NS_LOG_FUNCTION(this);
    Object::DoDispose();
}

bool UbCbfc::IsFcLimited(Ptr<UbIngressQueue> ingressQ)
{
    uint32_t nextPktSize = 0;
    const uint32_t vl = ingressQ->GetIngressPriority();
    if (ingressQ->GetIngressQueueType() == IngressQueueType::VOQ && ingressQ->IsControlFrame()) {
        NS_LOG_DEBUG("is crd pkt");
        return false;
    }

    nextPktSize = ingressQ->GetNextPacketSize();
    NS_LOG_DEBUG("nextPktSize:" << nextPktSize);

    int32_t consumeCellNum = ceil((float)nextPktSize / (m_cbfcCfg.m_flitLen * m_cbfcCfg.m_nFlitPerCell));
    const bool blocked = m_crdTxfree[vl] < consumeCellNum;
    if (m_creditBlockedLast[vl] != blocked) {
        utils::UbUtils::CbfcStateNotify(m_nodeId,
                                        m_portId,
                                        blocked ? "BLOCK" : "RESUME",
                                        vl,
                                        m_crdTxfree[vl],
                                        nextPktSize,
                                        static_cast<uint32_t>(m_cbfcCfg.m_flitLen) *
                                            m_cbfcCfg.m_nFlitPerCell);
        m_creditBlockedLast[vl] = blocked;
    }
    if (blocked) {
        NS_LOG_INFO("Flow Control Credit Limited,outPort:{" << ingressQ->GetOutPortId() << "} VL:{"
                                                            << vl << "}");
        NS_LOG_DEBUG("m_crdTxfree[ " << vl << " ]: " << m_crdTxfree[vl]
                                     << "is insufficient");
        return true;
    }
    NS_LOG_DEBUG("m_crdTxfree[ " << vl << " ]: " << m_crdTxfree[vl]
                                 << "is enough");

    return false;
}

void UbCbfc::OnIngressReleased(const UbFlowControlEventContext& context)
{
    if (context.inPortId != context.outPortId) { // 转发的报文
        // A recovered packet whose upstream credit was already returned when it left the original
        // ingress releases recovery storage without returning that credit twice.
        if (!context.upstreamCreditAlreadyReturned) {
            AccumulateReturnedCredit(context.packet, m_portId);
        }
        MaybeQueueControlReturn(m_portId);
    }
}

void UbCbfc::OnEgressEnqueued(const UbFlowControlEventContext& context)
{
    if (context.ingressQueue->IsControlFrame()) {
        NS_LOG_DEBUG("is crd pkt");
        return;
    }
    CbfcConsumeCrd(context.packet); // 计算消耗的信用证
    TryAttachPiggybackCredit(context.packet);
}

void UbCbfc::OnControlFrameReceived(Ptr<Packet> p)
{
    CbfcRestoreCrd(p);
}

void UbCbfc::OnDataPacketReceived(Ptr<Packet> p)
{
    RestoreDataPacketCredit(p);
}

void
UbCbfc::ControlCreditRestoreNotify(uint32_t nodeId,
                                   uint32_t portId,
                                   const std::vector<uint8_t>& credits)
{
    m_traceControlCreditRestoreNotify(nodeId, portId, credits);
}

void UbCbfc::OnIngressEnqueued(const UbFlowControlEventContext& context)
{
    AccumulateReturnedCredit(context.packet, m_portId);
    MaybeQueueControlReturn(m_portId);
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
    int32_t consumeCellNum = ceil((float)(pktSize) / (m_cbfcCfg.m_flitLen * m_cbfcCfg.m_nFlitPerCell));
    NS_LOG_DEBUG("befor consume, m_crdTxfree[ " << (uint32_t)vlId << " ]: " << m_crdTxfree[vlId]);
    if (m_crdTxfree[vlId] >= consumeCellNum) {
        m_crdTxfree[vlId] -= consumeCellNum;
        utils::UbUtils::CbfcCreditLevelNotify(m_nodeId,
                                              m_portId,
                                              "CONSUME",
                                              vlId,
                                              m_crdTxfree[vlId],
                                              -consumeCellNum,
                                              static_cast<uint32_t>(m_cbfcCfg.m_flitLen) *
                                                  m_cbfcCfg.m_nFlitPerCell);
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
    UbDatalinkControlCreditHeader crdHeader =
        UbDataLink::ConsumeCreditHeaderAndRestoreCredits(p, port);

    uint32_t resumeCellGrainNum = 0;
    bool ret = false;
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();

    std::vector<uint8_t> restoredCredits;
    restoredCredits.reserve(ubVlNum);
    std::vector<int32_t> restoredCreditCells;
    restoredCreditCells.reserve(ubVlNum);
    for (int index = 0; index < ubVlNum; index++) {
        NS_LOG_DEBUG("port m_credits[ " << (uint32_t)index << " ]: " << (uint32_t)port->m_credits[index]);
        restoredCredits.push_back(port->m_credits[index]);
        restoredCreditCells.push_back(static_cast<int32_t>(port->m_credits[index]) *
                                      m_cbfcCfg.m_retCellGrainControlPacket);
    }

    for (int index = 0; index < ubVlNum; index++) {
        if (port->m_credits[index] > 0) {
            resumeCellGrainNum = port->m_credits[index];
            NS_LOG_DEBUG("before resume m_crdTxfree[ " << (uint32_t)index << " ]: " << m_crdTxfree[index]);
            m_crdTxfree[index] += resumeCellGrainNum * m_cbfcCfg.m_retCellGrainControlPacket;  // 粒度数量 * 粒度大小
            utils::UbUtils::CbfcCreditLevelNotify(m_nodeId,
                                                  m_portId,
                                                  "RESTORE",
                                                  index,
                                                  m_crdTxfree[index],
                                                  static_cast<int32_t>(resumeCellGrainNum *
                                                                       m_cbfcCfg.m_retCellGrainControlPacket),
                                                  static_cast<uint32_t>(m_cbfcCfg.m_flitLen) *
                                                      m_cbfcCfg.m_nFlitPerCell);
            NS_LOG_DEBUG("left m_crdTxfree[ " << (uint32_t)index << " ]: " << m_crdTxfree[index]);
            ret = true;
        }
    }

    if (ret)
    {
        ControlCreditRestoreNotify(m_nodeId, m_portId, restoredCredits);
        utils::UbUtils::CbfcCreditRestoreTraceNotify(
            m_nodeId,
            m_portId,
            restoredCreditCells,
            static_cast<uint32_t>(m_cbfcCfg.m_flitLen) * m_cbfcCfg.m_nFlitPerCell);
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

void UbCbfc::AccumulateReturnedCredit(Ptr<Packet> p, uint32_t targetPortId)
{
    if (p != nullptr) {
        uint32_t pktSize = p->GetSize();
        Ptr<Node> node = NodeList::GetNode(m_nodeId);
        Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(targetPortId));
        UbDatalinkPacketHeader pktHeader;
        p->PeekHeader(pktHeader);
        uint8_t vlId = pktHeader.GetPacketVL();
        NS_LOG_DEBUG("NodeId: " << node->GetId() << " PortId: " << port->GetIfIndex()
                     << " vlId: " << (uint32_t)vlId << " pktSize: " << pktSize);

        int32_t consumeCellNum = ceil((float)(pktSize) / (m_cbfcCfg.m_flitLen * m_cbfcCfg.m_nFlitPerCell));
        SetCrdToReturn(vlId, consumeCellNum, port);
    }
}

Ptr<Packet> UbCbfc::BuildControlReturnPacket(uint32_t targetPortId,
                                             int32_t minPendingCells,
                                             const std::string& reason)
{
    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(targetPortId));
    if (port == nullptr) {
        return nullptr;
    }

    int32_t leftCrdToReturn = 0;
    int32_t crdSndGrains = 0;
    bool shouldReturnCredit = false;
    port->ResetCredits();
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();
    std::vector<int32_t> pendingSnapshot;
    pendingSnapshot.reserve(ubVlNum);

    for (int index = 0; index < ubVlNum; index++) {
        leftCrdToReturn = GetCrdToReturn(index);
        pendingSnapshot.push_back(leftCrdToReturn);
        if (leftCrdToReturn >= minPendingCells) {
            const int32_t pendingGrains = leftCrdToReturn / m_cbfcCfg.m_retCellGrainControlPacket;
            crdSndGrains = std::min(pendingGrains, static_cast<int32_t>(kMaxCtrlCreditGrainsPerFrame));
            NS_LOG_DEBUG("index: " << (uint32_t)index << " m_cbfcCfg->m_retCellGrainControlPacket: "
                         << (uint32_t)m_cbfcCfg.m_retCellGrainControlPacket
                         << " crdSndGrains: " << crdSndGrains);
            if (crdSndGrains > 0) {
                port->SetCredits(index, static_cast<uint8_t>(crdSndGrains));
                UpdateCrdToReturn(index, crdSndGrains * m_cbfcCfg.m_retCellGrainControlPacket, port);
                shouldReturnCredit = true;
            }
        }
    }

    for (int index = 0; index < ubVlNum; index++) {
        NS_LOG_DEBUG("SndCredits[ " << (uint32_t)index << " ]: " << (uint32_t)port->m_credits[index]);
    }

    if (!shouldReturnCredit) {
        return nullptr;
    }
    std::vector<int32_t> sendCreditCells;
    sendCreditCells.reserve(ubVlNum);
    for (int index = 0; index < ubVlNum; ++index) {
        sendCreditCells.push_back(static_cast<int32_t>(port->m_credits[index]) *
                                  m_cbfcCfg.m_retCellGrainControlPacket);
    }
    utils::UbUtils::CbfcControlSendNotify(
        m_nodeId,
        targetPortId,
        reason,
        m_cbfcCfg.m_ctrlCrdRtrThldCells,
        minPendingCells,
        pendingSnapshot,
        sendCreditCells,
        static_cast<uint32_t>(m_cbfcCfg.m_flitLen) * m_cbfcCfg.m_nFlitPerCell);
    return UbDataLink::BuildControlCreditPacket(port->m_credits);
}

Ptr<Packet>
UbCbfc::MaybeBuildControlReturnPacket(uint32_t targetPortId)
{
    if (ShouldForceControlReturn()) {
        return BuildControlReturnPacket(targetPortId,
                                        m_cbfcCfg.m_retCellGrainControlPacket,
                                        "force_threshold");
    }
    return nullptr;
}

uint8_t
UbCbfc::SelectPiggybackCreditVl() const
{
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    const uint8_t ubVlNum = static_cast<uint8_t>(val.Get());

    for (uint8_t offset = 1; offset <= ubVlNum; ++offset) {
        uint8_t candidate = static_cast<uint8_t>((m_lastPiggybackVl + offset) % ubVlNum);
        if (m_crdToReturn[candidate] >= m_cbfcCfg.m_retCellGrainDataPacket) {
            return candidate;
        }
    }
    return UB_PRIORITY_NUM_DEFAULT;
}

bool
UbCbfc::TryAttachPiggybackCredit(Ptr<Packet> p)
{
    if (p == nullptr) {
        return false;
    }

    UbDatalinkPacketHeader header;
    p->RemoveHeader(header);
    if (header.GetCredit()) {
        p->AddHeader(header);
        return false;
    }

    const uint8_t targetVl = SelectPiggybackCreditVl();
    if (targetVl >= UB_PRIORITY_NUM_DEFAULT) {
        p->AddHeader(header);
        return false;
    }

    header.SetCredit(true);
    header.SetCreditTargetVL(targetVl);
    p->AddHeader(header);
    m_crdToReturn[targetVl] -= m_cbfcCfg.m_retCellGrainDataPacket;
    m_lastPiggybackVl = targetVl;
    return true;
}

bool
UbCbfc::RestoreDataPacketCredit(Ptr<Packet> p)
{
    if (p == nullptr) {
        return false;
    }

    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(m_portId));

    UbDatalinkPacketHeader header;
    p->RemoveHeader(header);
    if (!header.GetCredit()) {
        p->AddHeader(header);
        return false;
    }

    const uint8_t targetVl = header.GetCreditTargetVL();
    const int32_t restoreCells = m_cbfcCfg.m_retCellGrainDataPacket;
    m_crdTxfree[targetVl] += restoreCells;
    utils::UbUtils::CbfcCreditLevelNotify(m_nodeId,
                                          m_portId,
                                          "RESTORE",
                                          targetVl,
                                          m_crdTxfree[targetVl],
                                          restoreCells,
                                          static_cast<uint32_t>(m_cbfcCfg.m_flitLen) *
                                              m_cbfcCfg.m_nFlitPerCell);

    header.SetCredit(false);
    header.SetCreditTargetVL(0);
    p->AddHeader(header);
    Simulator::ScheduleNow(&UbPort::TriggerTransmit, port);
    return true;
}

bool
UbCbfc::ShouldForceControlReturn() const
{
    for (int32_t creditsToReturn : m_crdToReturn)
    {
        if (creditsToReturn >= m_cbfcCfg.m_ctrlCrdRtrThldCells)
        {
            return true;
        }
    }
    return false;
}

void
UbCbfc::MaybeQueueControlReturn(uint32_t targetPortId)
{
    if (!ShouldForceControlReturn()) {
        return;
    }
    while (Ptr<Packet> cbfcPkt =
               BuildControlReturnPacket(targetPortId,
                                        m_cbfcCfg.m_retCellGrainControlPacket,
                                        "force_threshold")) {
        SendCrdAck(cbfcPkt, targetPortId);
    }
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
                            int32_t ctrlCrdRtrThldCells,
                            int32_t sharedInitCells, uint32_t nodeId, uint32_t portId)
{
    UbCbfc::Init(flitLen, nFlitPerCell, retCellGrainDataPacket, retCellGrainControlPacket,
                 reservedPerVlCells, ctrlCrdRtrThldCells, nodeId, portId);

    m_shareCrd = sharedInitCells;
    m_reservedPerVlCells = reservedPerVlCells;

    NS_LOG_DEBUG("NodeId: " << m_nodeId
                 << " PortId: " << m_portId
                 << " Init CbfcSharedMode"
                 << " reservedPerVlCells=" << m_reservedPerVlCells
                 << " sharedInitCells=" << m_shareCrd);

    NS_ABORT_MSG_IF(m_reservedPerVlCells < 0 || m_shareCrd < 0,
                    "CBFC_SHARED requires non-negative reserved and shared initial credits");
    if (m_reservedPerVlCells == 0 && m_shareCrd == 0)
    {
        NS_LOG_WARN("CBFC shared-credit configuration has zero total transmit credit on NodeId="
                    << m_nodeId
                    << " PortId=" << m_portId);
    }
}

FcType UbCbfcSharedCredit::GetFcType()
{
    return FcType::CBFC_SHARED;
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
        ceil((float)nextPktSize / (m_cbfcCfg.m_flitLen * m_cbfcCfg.m_nFlitPerCell));

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

void UbCbfcSharedCredit::OnEgressEnqueued(const UbFlowControlEventContext& context)
{
    if (context.ingressQueue->IsControlFrame()) {
        NS_LOG_DEBUG("is crd pkt");
        return;
    }
    CbfcSharedConsumeCrd(context.packet);
    TryAttachPiggybackCredit(context.packet);
}

void UbCbfcSharedCredit::OnControlFrameReceived(Ptr<Packet> p)
{
    CbfcSharedRestoreCrd(p);
}

void UbCbfcSharedCredit::OnDataPacketReceived(Ptr<Packet> p)
{
    RestoreSharedDataPacketCredit(p);
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
        ceil((float)pktSize / (m_cbfcCfg.m_flitLen * m_cbfcCfg.m_nFlitPerCell));

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
    UbDatalinkControlCreditHeader crdHeader =
        UbDataLink::ConsumeCreditHeaderAndRestoreCredits(p, port);

    uint32_t resumeCellGrainNum = 0;
    bool ret = false;
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();

    std::vector<uint8_t> restoredCredits;
    restoredCredits.reserve(ubVlNum);
    for (int index = 0; index < ubVlNum; index++) {
        NS_LOG_DEBUG("port m_credits[ " << (uint32_t)index << " ]: " << (uint32_t)port->m_credits[index]);
        restoredCredits.push_back(port->m_credits[index]);
    }

    int32_t totalReturned = 0;

    for (int index = 0; index < ubVlNum; index++) {
        if (port->m_credits[index] > 0) {
            resumeCellGrainNum = port->m_credits[index];
            int32_t cells = resumeCellGrainNum * m_cbfcCfg.m_retCellGrainControlPacket;
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

    if (ret)
    {
        ControlCreditRestoreNotify(m_nodeId, m_portId, restoredCredits);
    }

    Simulator::ScheduleNow(&UbPort::TriggerTransmit, port);
    return ret;
}

bool
UbCbfcSharedCredit::RestoreSharedDataPacketCredit(Ptr<Packet> p)
{
    if (p == nullptr) {
        return false;
    }

    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(m_portId));

    UbDatalinkPacketHeader header;
    p->RemoveHeader(header);
    if (!header.GetCredit()) {
        p->AddHeader(header);
        return false;
    }

    m_shareCrd += m_cbfcCfg.m_retCellGrainDataPacket;
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    const int ubVlNum = val.Get();
    for (int vl = 0; vl < ubVlNum && m_shareCrd > 0; ++vl) {
        if (m_crdTxfree[vl] < m_reservedPerVlCells) {
            const int32_t need = m_reservedPerVlCells - m_crdTxfree[vl];
            const int32_t grant = std::min<int32_t>(need, m_shareCrd);
            m_crdTxfree[vl] += grant;
            m_shareCrd -= grant;
        }
    }

    header.SetCredit(false);
    header.SetCreditTargetVL(0);
    p->AddHeader(header);
    Simulator::ScheduleNow(&UbPort::TriggerTransmit, port);
    return true;
}
} // namespace ns3
