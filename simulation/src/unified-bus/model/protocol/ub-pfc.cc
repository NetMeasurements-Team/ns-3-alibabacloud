// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-pfc.h"
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

TypeId UbPfcDecisionHook::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbPfcDecisionHook").SetParent<Object>();
    return tid;
}

TypeId UbPfcFixedDecisionHook::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbPfcFixedDecisionHook")
        .SetParent<UbPfcDecisionHook>()
        .AddConstructor<UbPfcFixedDecisionHook>();
    return tid;
}

void UbPfcFixedDecisionHook::Init(uint64_t xoffThresholdBytes, uint64_t xonThresholdBytes)
{
    m_xoffThresholdBytes = xoffThresholdBytes;
    m_xonThresholdBytes = xonThresholdBytes;
}

UbPfcDecision UbPfcFixedDecisionHook::Evaluate(Ptr<UbQueueManager> queueManager,
                                               uint32_t inPort,
                                               uint32_t priority) const
{
    auto queueOcc = queueManager->GetIngressQueueOccupancy(inPort, priority);
    auto switchOcc = queueManager->GetSwitchBufferOccupancy();
    return {
        .pause = queueOcc.total_bytes >= m_xoffThresholdBytes,
        .resume = queueOcc.total_bytes < m_xonThresholdBytes,
        .ingress_total_bytes = queueOcc.total_bytes,
        .ingress_shared_bytes = queueOcc.shared_bytes,
        .ingress_headroom_bytes = queueOcc.headroom_bytes,
        .switch_shared_pool_used_bytes = switchOcc.shared_pool_used_bytes,
        .switch_total_buffered_bytes = switchOcc.total_buffered_bytes,
        .xoff_threshold_bytes = m_xoffThresholdBytes,
        .xon_threshold_bytes = m_xonThresholdBytes,
    };
}

TypeId UbPfcDynamicDecisionHook::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbPfcDynamicDecisionHook")
        .SetParent<UbPfcDecisionHook>()
        .AddConstructor<UbPfcDynamicDecisionHook>();
    return tid;
}

UbPfcDecision UbPfcDynamicDecisionHook::Evaluate(Ptr<UbQueueManager> queueManager,
                                                 uint32_t inPort,
                                                 uint32_t priority) const
{
    auto queueOcc = queueManager->GetIngressQueueOccupancy(inPort, priority);
    auto switchOcc = queueManager->GetSwitchBufferOccupancy();
    const uint64_t xoff = queueManager->GetDynamicPauseThresholdBytes();
    const uint64_t xon = queueManager->GetDynamicResumeThresholdBytes();
    const bool pause = queueOcc.headroom_bytes > 0 || (queueOcc.shared_bytes >= xoff && queueOcc.shared_bytes > 0);
    return {
        .pause = pause,
        .resume = !pause && queueOcc.shared_bytes <= xon,
        .ingress_total_bytes = queueOcc.total_bytes,
        .ingress_shared_bytes = queueOcc.shared_bytes,
        .ingress_headroom_bytes = queueOcc.headroom_bytes,
        .switch_shared_pool_used_bytes = switchOcc.shared_pool_used_bytes,
        .switch_total_buffered_bytes = switchOcc.total_buffered_bytes,
        .xoff_threshold_bytes = xoff,
        .xon_threshold_bytes = xon,
    };
}

TypeId UbPfcPaperDynamicDecisionHook::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbPfcPaperDynamicDecisionHook")
        .SetParent<UbPfcDecisionHook>()
        .AddConstructor<UbPfcPaperDynamicDecisionHook>();
    return tid;
}

UbPfcDecision UbPfcPaperDynamicDecisionHook::Evaluate(Ptr<UbQueueManager> queueManager,
                                                      uint32_t inPort,
                                                      uint32_t priority) const
{
    auto queueOcc = queueManager->GetIngressQueueOccupancy(inPort, priority);
    auto switchOcc = queueManager->GetSwitchBufferOccupancy();
    const uint64_t paperBytes = queueManager->GetPaperBufferedBytes();
    const uint64_t xoff = queueManager->GetPaperPauseThresholdBytes(paperBytes);
    const uint64_t xon = queueManager->GetPaperResumeThresholdBytes(paperBytes);
    const bool pause = queueOcc.headroom_bytes > 0 || (queueOcc.total_bytes >= xoff && queueOcc.total_bytes > 0);
    return {
        .pause = pause,
        .resume = !pause && queueOcc.total_bytes <= xon,
        .ingress_total_bytes = queueOcc.total_bytes,
        .ingress_shared_bytes = queueOcc.shared_bytes,
        .ingress_headroom_bytes = queueOcc.headroom_bytes,
        .switch_shared_pool_used_bytes = switchOcc.shared_pool_used_bytes,
        .switch_total_buffered_bytes = switchOcc.total_buffered_bytes,
        .xoff_threshold_bytes = xoff,
        .xon_threshold_bytes = xon,
    };
}

TypeId UbPfc::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbPfc")
        .SetParent<UbFlowControl>()
        .AddConstructor<UbPfc>();
    return tid;
}

FcType UbPfc::GetFcType()
{
    return m_fcType;
}

void UbPfc::InitRuntimeState(FcType mode,
                             int32_t portpfcUpThld,
                             int32_t portpfcLowThld,
                             uint32_t nodeId,
                             uint32_t portId,
                             uint32_t vlNum)
{
    m_pfcStatus = pfcStatus_t(vlNum);
    m_pfcCfg.m_portpfcUpThld = portpfcUpThld;
    m_pfcCfg.m_portpfcLowThld = portpfcLowThld;
    m_nodeId = nodeId;
    m_portId = portId;
    m_fcType = mode;
}

void UbPfc::BindFixedDecisionHook()
{
    NS_ABORT_MSG_IF(m_pfcCfg.m_portpfcUpThld < 0 || m_pfcCfg.m_portpfcLowThld < 0,
                    "PFC_FIXED requires non-negative hi/lo thresholds");
    NS_ABORT_MSG_IF(m_pfcCfg.m_portpfcLowThld >= m_pfcCfg.m_portpfcUpThld,
                    "PFC_FIXED requires PfcLowThld < PfcUpThld");
    auto hook = CreateObject<UbPfcFixedDecisionHook>();
    hook->Init(m_pfcCfg.m_portpfcUpThld, m_pfcCfg.m_portpfcLowThld);
    m_decisionHook = hook;
}

void UbPfc::BindDynamicDecisionHook(Ptr<UbQueueManager> queueManager)
{
    const auto profile = queueManager->GetBufferProfileView();
    NS_ABORT_MSG_IF(profile.shared_pool_bytes == 0,
                    "PFC_DYNAMIC requires SharedPoolBytes > 0");
    NS_ABORT_MSG_IF(profile.headroom_per_port_bytes == 0,
                    "PFC_DYNAMIC requires HeadroomPerPortBytes > 0");
    m_decisionHook = CreateObject<UbPfcDynamicDecisionHook>();
}

void UbPfc::BindPaperDynamicDecisionHook(Ptr<UbQueueManager> queueManager)
{
    const auto profile = queueManager->GetBufferProfileView();
    NS_ABORT_MSG_IF(profile.shared_pool_bytes == 0,
                    "PFC_DYNAMIC_PAPER requires SharedPoolBytes > 0");
    m_decisionHook = CreateObject<UbPfcPaperDynamicDecisionHook>();
}

void UbPfc::BindDecisionHook(Ptr<UbQueueManager> queueManager)
{
    NS_ABORT_MSG_IF(m_fcType != FcType::PFC_FIXED && m_fcType != FcType::PFC_DYNAMIC &&
                        m_fcType != FcType::PFC_DYNAMIC_PAPER,
                    "UbPfc must be initialized with PFC_FIXED, PFC_DYNAMIC, or PFC_DYNAMIC_PAPER mode");

    switch (m_fcType) {
        case FcType::PFC_FIXED:
            BindFixedDecisionHook();
            break;
        case FcType::PFC_DYNAMIC:
            BindDynamicDecisionHook(queueManager);
            break;
        case FcType::PFC_DYNAMIC_PAPER:
            BindPaperDynamicDecisionHook(queueManager);
            break;
        default:
            NS_ABORT_MSG("Unsupported PFC mode in BindDecisionHook");
    }
}

void UbPfc::Init(FcType mode,
                 int32_t portpfcUpThld,
                 int32_t portpfcLowThld,
                 uint32_t nodeId,
                 uint32_t portId)
{
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    const uint32_t ubVlNum = static_cast<uint32_t>(val.Get());
    InitRuntimeState(mode, portpfcUpThld, portpfcLowThld, nodeId, portId, ubVlNum);

    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(m_portId));
    auto queueManager = port->GetNode()->GetObject<UbSwitch>()->GetQueueManager();

    NS_LOG_DEBUG("NodeId: " << m_nodeId
                 << " PortId: " << m_portId
                 << " Init Pfc mode="
                 << (m_fcType == FcType::PFC_FIXED ? "PFC_FIXED"
                     : (m_fcType == FcType::PFC_DYNAMIC ? "PFC_DYNAMIC" : "PFC_DYNAMIC_PAPER"))
                 << " hi=" << m_pfcCfg.m_portpfcUpThld
                 << " lo=" << m_pfcCfg.m_portpfcLowThld);

    BindDecisionHook(queueManager);
}

void UbPfc::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_decisionHook = nullptr;
    Object::DoDispose();
}

bool UbPfc::IsFcLimited(Ptr<UbIngressQueue> ingressQ)
{
    if (ingressQ->GetIngressQueueType() == IngressQueueType::VOQ && ingressQ->IsControlFrame()) {
        NS_LOG_DEBUG("is Pfc pkt");
        return false;
    }
    if (m_pfcStatus.m_portCredits[ingressQ->GetIngressPriority()] == 0) {
        NS_LOG_INFO("Flow Control Pfc Limited! NodeId: " << m_nodeId << ",outPort:{" << ingressQ->GetOutPortId() << "} VL:{"
                    << ingressQ->GetIngressPriority() << "}");
        return true;  // 不允许发送
    }

    return false;
}

void UbPfc::OnIngressReleased(const UbFlowControlEventContext& context)
{
    if (context.inPortId != context.outPortId) { // 转发的报文
        Ptr<Packet> pfcPkt = CheckPfcThreshold(context.packet, context.inPortId);
        if (pfcPkt != nullptr) {
            SendPfc(pfcPkt, context.inPortId);
        }
    }
}

void UbPfc::OnEgressEnqueued(const UbFlowControlEventContext& context)
{
}

void UbPfc::OnControlFrameReceived(Ptr<Packet> p)
{
    UpdatePfcStatus(p);
}

void UbPfc::OnIngressEnqueued(const UbFlowControlEventContext& context)
{
    Ptr<Packet> pfcPkt = CheckPfcThreshold(context.packet, m_portId);
    if (pfcPkt != nullptr) {
        SendPfc(pfcPkt, m_portId);
    }
}

bool UbPfc::UpdatePfcStatus(Ptr<Packet> p)
{
    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(m_portId));

    UbDatalinkControlCreditHeader pfcHeader =
        UbDataLink::ConsumeCreditHeaderAndRestoreCredits(p, port);
    bool ret = false;
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();
    for (int index = 0; index < ubVlNum; index++) {
        if (m_pfcStatus.m_portCredits[index] != port->m_credits[index]) {
            m_pfcStatus.m_portCredits[index] = port->m_credits[index];
            ret = true;
        }
    }

    NS_LOG_DEBUG("Recv Pfc uid: " << p->GetUid() << " NodeId: " << port->GetNode()->GetId() << " PortId: "
                << port->GetIfIndex() << " m_pfcStatus->m_portCredits:{"
                << (uint32_t)m_pfcStatus.m_portCredits[0] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[1] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[2] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[3] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[4] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[5] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[6] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[7] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[8] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[9] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[10] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[11] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[12] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[13] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[14] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[15] << "}");

    Simulator::ScheduleNow(&UbPort::TriggerTransmit, port);

    return ret;
}

void UbPfc::SendPfc(Ptr<Packet> pfcPacket, uint32_t targetPortId)
{
    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(targetPortId));

    node->GetObject<UbSwitch>()->SendControlFrame(pfcPacket, targetPortId);

    auto flowControl = DynamicCast<UbPfc>(port->GetFlowControl());
    flowControl->m_pfcStatus.m_pfcSndCnt++;
}

Ptr<Packet> UbPfc::CheckPfcThreshold(Ptr<Packet> p, uint32_t portId)
{
    Ptr<Packet> pfcPkt = nullptr;
    Ptr<Node> node = NodeList::GetNode(m_nodeId);

    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(portId));
    NS_LOG_DEBUG("NodeId: " << node->GetId() << " PortId: " << portId);
    auto flowControl = DynamicCast<UbPfc>(port->GetFlowControl());

    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();

    NS_ASSERT_MSG(flowControl->m_decisionHook != nullptr,
                  "UbPfc must have a decision hook before threshold evaluation");

    auto queueManager = node->GetObject<UbSwitch>()->GetQueueManager();
    for (int pri = 0; pri < ubVlNum; pri++) {
        UbPfcDecision decision = flowControl->m_decisionHook->Evaluate(queueManager, portId, pri);
        const uint8_t currentPauseState = decision.pause ? 1 : 0;

        if (flowControl->m_fcType == FcType::PFC_DYNAMIC &&
            flowControl->m_pfcStatus.m_pfcDynamicLastTracePause[pri] != currentPauseState) {
            utils::UbUtils::PfcDynamicStateNotify(port->GetNode()->GetId(),
                                                  portId,
                                                  static_cast<uint32_t>(pri),
                                                  decision.ingress_total_bytes,
                                                  decision.ingress_shared_bytes,
                                                  decision.ingress_headroom_bytes,
                                                  decision.xoff_threshold_bytes,
                                                  decision.xon_threshold_bytes,
                                                  decision.pause);
            flowControl->m_pfcStatus.m_pfcDynamicLastTracePause[pri] = currentPauseState;
        }

        if (decision.pause) {
            flowControl->m_pfcStatus.m_pfcSndCredits[pri] = 0;
        } else if (decision.resume) {
            flowControl->m_pfcStatus.m_pfcSndCredits[pri] = UB_CREDIT_MAX_VALUE;
        }
    }

    if (flowControl->m_pfcStatus.m_pfcSndCredits == flowControl->m_pfcStatus.m_pfcLastSndCredits) {
        NS_LOG_DEBUG("State Preservation");
        return pfcPkt;
    }
    for (int pri = 0; pri < ubVlNum; pri++) {
        if (flowControl->m_pfcStatus.m_pfcSndCredits[pri] ==
            flowControl->m_pfcStatus.m_pfcLastSndCredits[pri]) {
            continue;
        }
        const bool pause = flowControl->m_pfcStatus.m_pfcSndCredits[pri] == 0;
        const uint64_t ingressBytes = queueManager != nullptr
                                          ? queueManager->GetQueueIngressTotalBytes(portId, pri)
                                          : 0;
        UbUtils::PfcStateNotify(node->GetId(),
                                portId,
                                pause ? "PAUSE" : "RESUME",
                                static_cast<uint32_t>(pri),
                                ingressBytes);
    }

    port->ResetCredits();
    for (int pri = 0; pri < ubVlNum; pri++) {
        if (flowControl->m_pfcStatus.m_pfcSndCredits[pri]) {
            port->SetCredits(pri, flowControl->m_pfcStatus.m_pfcSndCredits[pri]);
        }
    }

    NS_LOG_DEBUG("m_pfcStatus->m_pfcSndCredits: ");
    for (int index = 0; index < ubVlNum; index++) {
        NS_LOG_DEBUG((uint32_t)flowControl->m_pfcStatus.m_pfcSndCredits[index] << " ");
    }

    flowControl->m_pfcStatus.m_pfcLastSndCredits = flowControl->m_pfcStatus.m_pfcSndCredits;

    NS_LOG_DEBUG("Port credits changed. NodeId: " << node->GetId() << " inPort:{" << portId
            << "} VL:{" << (uint32_t)port->m_credits[0] << " " << (uint32_t)port->m_credits[1] << " "
            << (uint32_t)port->m_credits[2] << " " << (uint32_t)port->m_credits[3] << " "
            << (uint32_t)port->m_credits[4] << " " << (uint32_t)port->m_credits[5] << " "
            << (uint32_t)port->m_credits[6] << " " << (uint32_t)port->m_credits[7] << " "
            << (uint32_t)port->m_credits[8] << " " << (uint32_t)port->m_credits[9] << " "
            << (uint32_t)port->m_credits[10] << " " << (uint32_t)port->m_credits[11] << " "
            << (uint32_t)port->m_credits[12] << " " << (uint32_t)port->m_credits[13] << " "
            << (uint32_t)port->m_credits[14] << " " << (uint32_t)port->m_credits[15] << "}");
    pfcPkt = UbDataLink::BuildControlCreditPacket(port->m_credits);
    NS_LOG_DEBUG("Create pfcpkt uid: " << pfcPkt->GetUid());

    return pfcPkt;
}
} // namespace ns3
