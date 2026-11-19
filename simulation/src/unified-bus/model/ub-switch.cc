// SPDX-License-Identifier: GPL-2.0-only

#include <algorithm>
#include "ns3/ub-enum-accessor.h"
#include <limits>
#include "ns3/ipv4.h"
#include "ns3/node-list.h"
#include "ns3/packet.h"
#include "ns3/flow-id-tag.h"
#include "ns3/ub-switch-allocator.h"
#include "ns3/ub-caqm.h"
#include "ns3/ub-port.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-function.h"
#include "ns3/ub-ctp.h"
#include "ns3/ub-reliable-rerouting.h"
#include "ns3/ub-utils.h"

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(UbSwitch);
NS_LOG_COMPONENT_DEFINE("UbSwitch");

namespace {

UbFlowControlEventContext
MakeIngressFlowControlEventContext(Ptr<Packet> packet,
                                   Ptr<UbIngressQueue> ingressQueue,
                                   uint32_t inPortId,
                                   uint32_t outPortId,
                                   uint32_t priority)
{
    return {
        .packet = packet,
        .ingressQueue = ingressQueue,
        .inPortId = inPortId,
        .outPortId = outPortId,
        .priority = priority,
    };
}

uint16_t
Cna24ToRoutingPortId(uint32_t cna)
{
    const uint32_t encodedPort = cna & 0xFF;
    return encodedPort == 0 ? 0 : static_cast<uint16_t>(encodedPort - 1);
}

} // namespace


/*-----------------------------------------UbSwitchNode----------------------------------------------*/

TypeId UbSwitch::GetTypeId (void)
{
    static TypeId tid = TypeId ("ns3::UbSwitch")
        .SetParent<Object> ()
        .SetGroupName("UnifiedBus")
        .AddConstructor<UbSwitch> ()
        .AddAttribute("FlowControl",
                      "Flow control mechanism (NONE, CBFC, CBFC_SHARED, PFC_FIXED, PFC_DYNAMIC, "
                      "or PFC_DYNAMIC_PAPER). PFC_DYNAMIC_PAPER is the paper-style dynamic PFC "
                      "mode used for DCQCN reproduction from \"Congestion Control for Large-Scale "
                      "RDMA Deployments\" (SIGCOMM 2015). "
                      "CBFC and PFC modes are peer policies over the same ingress accounting model.",
                      MakeUbEnumValue(FcType::CBFC),
                      MakeUbEnumAccessor(&UbSwitch::m_flowControlType),
                      MakeUbEnumChecker(FcType::NONE, "NONE",
                                      FcType::CBFC, "CBFC",
                                      FcType::CBFC_SHARED, "CBFC_SHARED",
                                      FcType::PFC_FIXED, "PFC_FIXED",
                                      FcType::PFC_DYNAMIC, "PFC_DYNAMIC",
                                      FcType::PFC_DYNAMIC_PAPER, "PFC_DYNAMIC_PAPER"))
        .AddAttribute("VlScheduler",
                      "VL inter-scheduling algorithm (SP or DWRR).",
                      MakeUbEnumValue(SP),
                      MakeUbEnumAccessor(&UbSwitch::m_vlScheduler),
                      MakeUbEnumChecker(SP, "SP",
                                      DWRR, "DWRR"))
        .AddAttribute("InPortProcessingDelay",
                      "Fixed non-blocking input-port processing latency after route/output are "
                      "known and before the packet becomes visible in VOQ to the allocator.",
                      TimeValue(NanoSeconds(0)),
                      MakeTimeAccessor(&UbSwitch::m_inPortProcessingDelay),
                      MakeTimeChecker())
        .AddTraceSource("LastPacketTraversesNotify",
                        "Last Packet Traverses, NodeId",
                        MakeTraceSourceAccessor(&UbSwitch::m_traceLastPacketTraversesNotify),
                        "ns3::UbSwitch::LastPacketTraversesNotify");
    return tid;
}

void UbSwitch::SetReservePerQueueBytes(uint32_t bytes)
{
    m_bufferOverrides.reservePerQueueBytes = bytes;
}

void UbSwitch::SetSharedPoolBytes(uint64_t bytes)
{
    m_bufferOverrides.sharedPoolBytes = bytes;
}

void UbSwitch::SetHeadroomPerPortBytes(uint32_t bytes)
{
    m_bufferOverrides.headroomPerPortBytes = bytes;
}

void UbSwitch::SetDynamicPfcResumeGapBytes(uint32_t bytes)
{
    m_bufferOverrides.dynamicPfcResumeGapBytes = bytes;
}

void UbSwitch::SetDynamicThresholdAlphaShift(uint32_t shift)
{
    m_bufferOverrides.dynamicThresholdAlphaShift = shift;
}

void UbSwitch::SetPaperDynamicPfcBeta(uint32_t beta)
{
    m_bufferOverrides.paperDynamicPfcBeta = beta;
}

void UbSwitch::SetPfcThresholds(int32_t xoffBytes, int32_t xonBytes)
{
    m_flowControlOverrides.pfcXoffBytes = xoffBytes;
    m_flowControlOverrides.pfcXonBytes = xonBytes;
}

void UbSwitch::SetCbfcCellGeometry(uint8_t flitLenBytes, uint8_t flitsPerCell)
{
    m_flowControlOverrides.cbfcFlitLenBytes = flitLenBytes;
    m_flowControlOverrides.cbfcFlitsPerCell = flitsPerCell;
}

void UbSwitch::SetCbfcReturnCellGrain(uint8_t dataPacketCells, uint8_t controlPacketCells)
{
    m_flowControlOverrides.cbfcReturnGrainDataCells = dataPacketCells;
    m_flowControlOverrides.cbfcReturnGrainControlCells = controlPacketCells;
}

void UbSwitch::SetCbfcCredits(int32_t initCreditCells, int32_t sharedInitCreditCells)
{
    m_flowControlOverrides.cbfcInitCreditCells = initCreditCells;
    m_flowControlOverrides.cbfcSharedInitCreditCells = sharedInitCreditCells;
}

void UbSwitch::ApplyLocalQueueManagerConfig()
{
    NS_ASSERT_MSG(m_queueManager != nullptr, "QueueManager must exist before applying local config");

    if (m_bufferOverrides.reservePerQueueBytes.has_value()) {
        m_queueManager->SetAttribute("ReservePerQueueBytes",
                                     UintegerValue(m_bufferOverrides.reservePerQueueBytes.value()));
    }
    if (m_bufferOverrides.sharedPoolBytes.has_value()) {
        m_queueManager->SetAttribute("SharedPoolBytes",
                                     UintegerValue(m_bufferOverrides.sharedPoolBytes.value()));
    }
    if (m_bufferOverrides.headroomPerPortBytes.has_value()) {
        m_queueManager->SetAttribute("HeadroomPerPortBytes",
                                     UintegerValue(m_bufferOverrides.headroomPerPortBytes.value()));
    }
    if (m_bufferOverrides.dynamicPfcResumeGapBytes.has_value()) {
        m_queueManager->SetAttribute("DynamicPfcResumeGapBytes",
                                     UintegerValue(m_bufferOverrides.dynamicPfcResumeGapBytes.value()));
    }
    if (m_bufferOverrides.dynamicThresholdAlphaShift.has_value()) {
        m_queueManager->SetAttribute("AlphaShift",
                                     UintegerValue(m_bufferOverrides.dynamicThresholdAlphaShift.value()));
    }
    if (m_bufferOverrides.paperDynamicPfcBeta.has_value()) {
        m_queueManager->SetAttribute("PaperDynamicPfcBeta",
                                     UintegerValue(m_bufferOverrides.paperDynamicPfcBeta.value()));
    }
}

void UbSwitch::ApplyLocalPortFlowControlConfig(Ptr<UbPort> port)
{
    NS_ASSERT_MSG(port != nullptr, "Port must exist before applying local flow-control config");

    if (m_flowControlOverrides.pfcXoffBytes.has_value()) {
        port->SetAttribute("PfcUpThld", IntegerValue(m_flowControlOverrides.pfcXoffBytes.value()));
    }
    if (m_flowControlOverrides.pfcXonBytes.has_value()) {
        port->SetAttribute("PfcLowThld", IntegerValue(m_flowControlOverrides.pfcXonBytes.value()));
    }
    if (m_flowControlOverrides.cbfcFlitLenBytes.has_value()) {
        port->SetAttribute("CbfcFlitLenByte", UintegerValue(m_flowControlOverrides.cbfcFlitLenBytes.value()));
    }
    if (m_flowControlOverrides.cbfcFlitsPerCell.has_value()) {
        port->SetAttribute("CbfcFlitsPerCell", UintegerValue(m_flowControlOverrides.cbfcFlitsPerCell.value()));
    }
    if (m_flowControlOverrides.cbfcReturnGrainDataCells.has_value()) {
        port->SetAttribute("CbfcRetCellGrainDataPacket",
                           UintegerValue(m_flowControlOverrides.cbfcReturnGrainDataCells.value()));
    }
    if (m_flowControlOverrides.cbfcReturnGrainControlCells.has_value()) {
        port->SetAttribute("CbfcRetCellGrainControlPacket",
                           UintegerValue(m_flowControlOverrides.cbfcReturnGrainControlCells.value()));
    }
    if (m_flowControlOverrides.cbfcInitCreditCells.has_value()) {
        port->SetAttribute("CbfcInitCreditCell", IntegerValue(m_flowControlOverrides.cbfcInitCreditCells.value()));
    }
    if (m_flowControlOverrides.cbfcSharedInitCreditCells.has_value()) {
        port->SetAttribute("CbfcSharedInitCreditCell",
                           IntegerValue(m_flowControlOverrides.cbfcSharedInitCreditCells.value()));
    }
}

void UbSwitch::InitAllocator(Ptr<Node> node)
{
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
}

void UbSwitch::InitQueueManager(Ptr<Node> node)
{
    m_queueManager = CreateObject<UbQueueManager>();
    m_queueManager->SetOwnerNode(node);
    m_queueManager->SetVLNum(m_vlNum);
    m_queueManager->SetPortsNum(m_portsNum);
    m_queueManager->SetPaperDynamicAdmissionEnabled(m_flowControlType == FcType::PFC_DYNAMIC_PAPER);
    ApplyLocalQueueManagerConfig();
    m_queueManager->Init();
}

void UbSwitch::InitRoutingProcess(Ptr<Node> node)
{
    m_routingProcess = CreateObject<UbRoutingProcess>();
    m_routingProcess->SetNodeId(node->GetId());
    m_Ipv4Addr = utils::NodeIdToIp(node->GetId());

    Ptr<UbController> controller = node->GetObject<UbController>();
    if (controller != nullptr)
    {
        Ptr<UbFunction> function = controller->GetUbFunction();
        if (function != nullptr && function->GetUbLdstApi() != nullptr)
        {
            function->GetUbLdstApi()->ValidateRoutingConfiguration();
        }
    }
}
/**
 * @brief Init UbNode, create algorithm, queueManager, fc and so on
 */
void UbSwitch::Init()
{
    auto node = GetObject<Node>();
    m_portsNum = node->GetNDevices();
    NS_LOG_DEBUG("UbSwitch Init: nodeId=" << node->GetId()
                 << " flowControlMode=" << static_cast<int>(m_flowControlType)
                 << " ports=" << m_portsNum
                 << " vlNum=" << m_vlNum);

    InitAllocator(node);
    InitQueueManager(node);
    InitNodePortsFlowControl();
    InitRoutingProcess(node);
}

void UbSwitch::DoDispose()
{
    m_recoveryDisposed = true;
    if (m_detectedFailedOutput != UINT32_MAX)
    {
        ClearRecoveryBacklogForOutput(m_detectedFailedOutput);
    }
    PruneResolvedInPortEvents();
    m_forcedInPortEventResidue = 0;
    m_forcedInPortByteResidue = 0;
    for (auto& pending : m_inPortProcessingEvents) {
        if (!pending.event.IsExpired()) {
            ++m_forcedInPortEventResidue;
            m_forcedInPortByteResidue += pending.packetBytes;
            pending.event.Cancel();
        }
    }
    m_inPortProcessingEvents.clear();
    m_forcedVoqPacketResidue = 0;
    for (const auto& group : m_voq.groups) {
        for (const auto& entry : group) {
            if (entry.queue != nullptr) {
                m_forcedVoqPacketResidue += entry.queue->GetPacketCount();
            }
        }
    }
    m_forcedBufferedByteResidue = m_queueManager == nullptr
                                      ? 0
                                      : m_queueManager->GetSwitchBufferOccupancy().total_buffered_bytes;
    if (m_allocator != nullptr) {
        m_allocator->Dispose();
    }
    m_queueManager = nullptr;
    m_congestionCtrl = nullptr;
    m_allocator = nullptr;
    m_voq.groups.clear();
    m_pendingRecovery.clear();
    m_routingProcess = nullptr;
    Object::DoDispose();
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
        ApplyLocalPortFlowControlConfig(port);
        port->CreateAndInitFc(m_flowControlType);
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
    const auto groupCount =
        static_cast<VirtualOutputQueueGroupKey>(m_portsNum) * m_vlNum;
    NS_ASSERT_MSG(groupCount <= std::numeric_limits<size_t>::max(), "VOQ group count too large");
    m_voq.groups.clear();
    m_voq.groups.resize(static_cast<size_t>(groupCount));
}

/**
 * @brief push packet into voq
 */
void UbSwitch::PushPacketToVoq(Ptr<Packet> p, uint32_t outPort, uint32_t priority, uint32_t inPort)
{
    GetOrCreateVoq(outPort, priority, inPort)->Push(p);
}

Ptr<UbPacketQueue> UbSwitch::GetOrCreateVoq(uint32_t outPort, uint32_t priority, uint32_t inPort)
{
    if (!IsValidVoqIndices(outPort, priority, inPort, m_portsNum, m_vlNum)) {
        NS_ASSERT_MSG(0, "Invalid VOQ indices (outPort, priority, inPort)!");
    }
    const auto groupKey = BuildVoqGroupKey(outPort, priority);
    Ptr<UbPacketQueue>* slot = GetOrCreateVoqSlot(groupKey, inPort);
    if (*slot != nullptr) {
        return *slot;
    }

    Ptr<UbPacketQueue> voq = CreateObject<UbPacketQueue>();
    voq->SetOutPortId(outPort);
    voq->SetIngressPriority(priority);
    voq->SetInPortId(inPort);
    m_allocator->RegisterUbIngressQueue(voq, outPort, priority);
    *slot = voq;
    return voq;
}

UbSwitch::VirtualOutputQueueGroupKey
UbSwitch::BuildVoqGroupKey(uint32_t outPort, uint32_t priority) const
{
    return static_cast<VirtualOutputQueueGroupKey>(outPort) * m_vlNum + priority;
}

const Ptr<UbPacketQueue>*
UbSwitch::FindVoqSlot(UbSwitch::VirtualOutputQueueGroupKey groupKey, uint32_t inPort) const
{
    if (groupKey >= m_voq.groups.size()) {
        return nullptr;
    }

    const auto& group = m_voq.groups[static_cast<size_t>(groupKey)];
    for (const auto& entry : group) {
        if (entry.inPort == inPort) {
            return &entry.queue;
        }
    }
    return nullptr;
}

Ptr<UbPacketQueue>*
UbSwitch::GetOrCreateVoqSlot(UbSwitch::VirtualOutputQueueGroupKey groupKey, uint32_t inPort)
{
    NS_ASSERT_MSG(groupKey < m_voq.groups.size(), "VOQ group key is out of range");

    auto& group = m_voq.groups[static_cast<size_t>(groupKey)];
    for (auto& entry : group) {
        if (entry.inPort == inPort) {
            return &entry.queue;
        }
    }

    group.push_back({inPort, nullptr});
    return &group.back().queue;
}

void
UbSwitch::ForwardDataPacketAfterAdmission(Ptr<Packet> packet,
                                          uint32_t inPort,
                                          uint32_t outPort,
                                          uint32_t priority)
{
    NS_ASSERT_MSG(packet != nullptr, "ForwardDataPacketAfterAdmission requires a packet");
    NS_ASSERT_MSG(m_queueManager != nullptr, "ForwardDataPacketAfterAdmission requires QueueManager");
    NS_ASSERT_MSG(IsValidVoqIndices(outPort, priority, inPort, m_portsNum, m_vlNum),
                  "ForwardDataPacketAfterAdmission invalid VOQ indices");

    m_queueManager->PushToInPortProcessing(inPort, outPort, priority, packet->GetSize());

    if (m_inPortProcessingDelay <= NanoSeconds(0)) {
        MoveInPortProcessingToVoq(packet, inPort, outPort, priority);
        FinalizeForwardedPacketEnqueue(packet, inPort, outPort, priority);
        return;
    }

    m_inPortProcessingEvents.push_back(
        {Simulator::Schedule(m_inPortProcessingDelay,
                             &UbSwitch::CompleteInPortProcessing,
                             this,
                             packet,
                             inPort,
                             outPort,
                             priority),
         inPort,
         outPort,
         priority,
         packet->GetSize()});
}

void
UbSwitch::MoveInPortProcessingToVoq(Ptr<Packet> packet,
                                    uint32_t inPort,
                                    uint32_t outPort,
                                    uint32_t priority)
{
    if (packet == nullptr || m_queueManager == nullptr) {
        return;
    }

    NS_ASSERT_MSG(IsValidVoqIndices(outPort, priority, inPort, m_portsNum, m_vlNum),
                  "MoveInPortProcessingToVoq invalid VOQ indices");
    m_queueManager->MoveInPortProcessingToVoq(inPort, outPort, priority, packet->GetSize());
    GetOrCreateVoq(outPort, priority, inPort)->Push(packet);
}

void
UbSwitch::FinalizeForwardedPacketEnqueue(Ptr<Packet> packet,
                                         uint32_t inPort,
                                         uint32_t outPort,
                                         uint32_t priority,
                                         bool recoveryTransfer)
{
    if (packet == nullptr) {
        return;
    }

    auto node = GetObject<Node>();
    NS_ASSERT_MSG(node != nullptr, "FinalizeForwardedPacketEnqueue requires an owning node");
    Ptr<UbPort> recvPort = DynamicCast<UbPort>(node->GetDevice(inPort));
    Ptr<UbPort> outDevice = DynamicCast<UbPort>(node->GetDevice(outPort));
    NS_ASSERT_MSG(recvPort != nullptr, "FinalizeForwardedPacketEnqueue requires a valid ingress port");
    NS_ASSERT_MSG(outDevice != nullptr, "FinalizeForwardedPacketEnqueue requires a valid out port");

    // A recovery transfer re-enters an existing packet that already reported its arrival-time
    // congestion effect; re-running the post-enqueue hook would double count it.
    if (m_congestionCtrl != nullptr && !recoveryTransfer && GetPacketType(packet) != UB_LAST_ACK_PACKET)
    {
        m_congestionCtrl->OnSwitchPostEnqueue(inPort, outPort, packet);
    }

    if (IsPFCEnable()) {
        recvPort->GetFlowControl()->OnIngressEnqueued(
            MakeIngressFlowControlEventContext(packet,
                                               GetOrCreateVoq(outPort, priority, inPort),
                                               inPort,
                                               outPort,
                                               priority));
    }

    outDevice->TriggerTransmit();
}

void
UbSwitch::CompleteInPortProcessing(Ptr<Packet> packet,
                                   uint32_t inPort,
                                   uint32_t outPort,
                                   uint32_t priority)
{
    PruneResolvedInPortEvents();

    if (outPort == m_detectedFailedOutput)
    {
        NS_ABORT_MSG_IF(m_pendingFailedProcessing == 0, "Failed processing count underflow");
        --m_pendingFailedProcessing;
        // The packet keeps its original ready time and ingress reservation, but it may enter a
        // healthy VOQ only after the modeled LastAck.
        m_queueManager->MoveProcessingToPendingRecovery(inPort, outPort, priority, packet->GetSize());
        ParkRecoveryPacket(packet, inPort, priority, /*ingressHeld=*/true);
        PublishRecoveryBacklogCounters(outPort);
        if (m_recoveryBacklogMigrated)
        {
            TryTransferPendingRecovery();
        }
        return;
    }

    MoveInPortProcessingToVoq(packet, inPort, outPort, priority);
    FinalizeForwardedPacketEnqueue(packet, inPort, outPort, priority);
}

void
UbSwitch::PruneResolvedInPortEvents()
{
    m_inPortProcessingEvents.erase(
        std::remove_if(m_inPortProcessingEvents.begin(),
                       m_inPortProcessingEvents.end(),
                       [](const InPortProcessingEvent& pending) {
                           return pending.event.IsExpired();
                       }),
        m_inPortProcessingEvents.end());
}

bool
UbSwitch::HasPendingInPortWork(uint32_t outPort, uint32_t priority, uint32_t inPort) const
{
    return std::any_of(m_inPortProcessingEvents.begin(),
                       m_inPortProcessingEvents.end(),
                       [outPort, priority, inPort](const InPortProcessingEvent& pending) {
                           return !pending.event.IsExpired() && pending.outPort == outPort &&
                                  pending.priority == priority && pending.inPort == inPort;
                       });
}

bool
UbSwitch::TryDestroyEmptyVoq(uint32_t outPort, uint32_t priority, uint32_t inPort)
{
    if (!IsValidVoqIndices(outPort, priority, inPort, m_portsNum, m_vlNum) ||
        m_allocator == nullptr || m_queueManager == nullptr) {
        return false;
    }

    const auto groupKey = BuildVoqGroupKey(outPort, priority);
    const Ptr<UbPacketQueue>* slot = FindVoqSlot(groupKey, inPort);
    if (slot == nullptr || *slot == nullptr) {
        return false;
    }
    Ptr<UbPacketQueue> queue = *slot;
    if (queue->GetPacketCount() != 0 || HasPendingInPortWork(outPort, priority, inPort) ||
        m_allocator->HasPendingOrActiveWork(queue)) {
        return false;
    }

    const bool hasAccounting =
        m_queueManager->GetQueueIngressTotalBytes(inPort, priority) != 0 ||
        m_queueManager->GetOutPortBufferUsed(outPort, priority) != 0 ||
        m_queueManager->GetIngressControlBytes(inPort, priority) != 0 ||
        m_queueManager->GetOutPortControlBytes(outPort, priority) != 0;
    if (hasAccounting) {
        return false;
    }

    m_allocator->UnregisterUbIngressQueue(queue, outPort, priority);
    if (queue->GetPacketCount() != 0 || HasPendingInPortWork(outPort, priority, inPort) ||
        m_allocator->HasPendingOrActiveWork(queue) ||
        m_queueManager->GetQueueIngressTotalBytes(inPort, priority) != 0 ||
        m_queueManager->GetOutPortBufferUsed(outPort, priority) != 0 ||
        m_queueManager->GetIngressControlBytes(inPort, priority) != 0 ||
        m_queueManager->GetOutPortControlBytes(outPort, priority) != 0) {
        m_allocator->RegisterUbIngressQueue(queue, outPort, priority);
        return false;
    }
    auto& group = m_voq.groups[static_cast<size_t>(groupKey)];
    const auto oldSize = group.size();
    group.erase(std::remove_if(group.begin(),
                               group.end(),
                               [inPort, queue](const VirtualOutputQueueEntry& entry) {
                                   return entry.inPort == inPort && entry.queue == queue;
                               }),
                group.end());
    return group.size() + 1 == oldSize;
}

bool UbSwitch::IsValidVoqIndices(uint32_t outPort, uint32_t priority, uint32_t inPort, uint32_t portsNum, uint32_t vlNum)
{
    return outPort < portsNum && priority < vlNum && inPort < portsNum;
}

uint64_t UbSwitch::GetAllocatedVoqCountForTest() const
{
    uint64_t count = 0;
    for (const auto& group : m_voq.groups) {
        for (const auto& entry : group) {
            if (entry.queue != nullptr) {
                ++count;
            }
        }
    }
    return count;
}

bool UbSwitch::HasVoqForTest(uint32_t outPort, uint32_t priority, uint32_t inPort) const
{
    if (!IsValidVoqIndices(outPort, priority, inPort, m_portsNum, m_vlNum)) {
        return false;
    }
    const Ptr<UbPacketQueue>* slot = FindVoqSlot(BuildVoqGroupKey(outPort, priority), inPort);
    return slot != nullptr && *slot != nullptr;
}

bool
UbSwitch::TryDestroyVoqForTest(uint32_t outPort, uint32_t priority, uint32_t inPort)
{
    PruneResolvedInPortEvents();
    return TryDestroyEmptyVoq(outPort, priority, inPort);
}

uint32_t
UbSwitch::GetPendingInPortProcessingEventCountForTest() const
{
    return static_cast<uint32_t>(
        std::count_if(m_inPortProcessingEvents.begin(),
                      m_inPortProcessingEvents.end(),
                      [](const InPortProcessingEvent& pending) {
                          return !pending.event.IsExpired();
                      }));
}

UbPacketType_t UbSwitch::GetPacketType(Ptr<Packet> packet)
{
    UbDatalinkHeader dlHeader;
    packet->PeekHeader(dlHeader);
    if (dlHeader.GetConfig() == 5)
        return UB_LAST_ACK_PACKET;
    if (dlHeader.IsControlCreditHeader())
        return UB_CONTROL_FRAME;
    if (dlHeader.IsPacketIpv4Header())
        return UB_URMA_DATA_PACKET;
    if (dlHeader.GetConfig() == static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_CNA16)) {
        Ptr<Packet> copy = CopyPacketView(packet);
        UbDatalinkPacketHeader packetHeader;
        UbCna16NetworkHeader cna16Header;
        copy->RemoveHeader(packetHeader);
        copy->RemoveHeader(cna16Header);
        if (cna16Header.GetNlp() == UB_CNA_NLP_CTPH) {
            return UB_CTP_DATA_PACKET;
        }
        return UB_LDST_DATA_PACKET;
    }
    if (dlHeader.GetConfig() == static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_CNA24)) {
        Ptr<Packet> copy = CopyPacketView(packet);
        UbDatalinkPacketHeader packetHeader;
        UbCna24NetworkHeader cna24Header;
        copy->RemoveHeader(packetHeader);
        copy->RemoveHeader(cna24Header);
        if (cna24Header.GetNlp() == UB_CNA_NLP_CTPH) {
            return UB_CTP_DATA_PACKET;
        }
        if (cna24Header.GetNlp() == UB_CNA_NLP_LDST_BYPASS) {
            return UB_LDST_DATA_PACKET;
        }
    }
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
        case UB_LAST_ACK_PACKET:
            HandleLastAck(port, packet);
            break;
        case UB_CONTROL_FRAME:
            port->GetFlowControl()->OnControlFrameReceived(packet);
            break;
        case UB_URMA_DATA_PACKET:
            if (IsCBFCEnable() || IsCBFCSharedEnable()) {
                port->GetFlowControl()->OnDataPacketReceived(packet);
            }
            HandleURMADataPacket(port, packet);
            break;
        case UB_LDST_DATA_PACKET:
            if (IsCBFCEnable() || IsCBFCSharedEnable()) {
                port->GetFlowControl()->OnDataPacketReceived(packet);
            }
            HandleLdstDataPacket(port, packet);
            break;
        case UB_CTP_DATA_PACKET:
            if (IsCBFCEnable() || IsCBFCSharedEnable()) {
                port->GetFlowControl()->OnDataPacketReceived(packet);
            }
            HandleCtpDataPacket(port, packet);
            break;
        default:
            NS_ASSERT_MSG(0, "Invalid Packet Type!");
    }
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
 * @brief Handle CTP type data packet
 */
void UbSwitch::HandleCtpDataPacket(Ptr<UbPort> port, Ptr<Packet> packet)
{
    ParsedCtpHeaders headers;
    ParseCtpPacketHeader(packet, headers);

    switch (GetNodeType()) {
        case UB_DEVICE:
            if (!SinkCtpDataPacket(port, packet, headers)) {
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
        port->GetFlowControl()->OnIngressEnqueued(
            MakeIngressFlowControlEventContext(packet,
                                               nullptr,
                                               port->GetIfIndex(),
                                               port->GetIfIndex(),
                                               headers.datalinkPacketHeader.GetPacketVL()));
    }

    const uint32_t srcTpn = headers.transportHeader.GetSrcTpn();
    uint32_t dstTpn = headers.transportHeader.GetDestTpn();
    Ptr<UbController> controller = GetObject<UbController>();
    const UbInboundTpResolution resolution =
        controller->GetTransportChannelManager()->ResolveInboundTp(
            srcTpn,
            dstTpn,
            headers.ipv4Header.GetSource(),
            headers.ipv4Header.GetDestination(),
            headers.datalinkPacketHeader.GetPacketVL());
    if (resolution.state == UbInboundTpState::RETIRED_EXACT)
    {
        NS_LOG_DEBUG("Ignoring stale packet for retired TP channel key");
        return true;
    }
    Ptr<UbTransportChannel> targetTp = resolution.endpoint;
    if (resolution.state != UbInboundTpState::LIVE || targetTp == nullptr) {
        NS_ABORT_MSG("Received unreserved or mismatched TP channel key. node="
                     << GetObject<Node>()->GetId() << " srcTpn=" << srcTpn << " dstTpn=" << dstTpn
                     << " sourceIp=" << headers.ipv4Header.GetSource()
                     << " destinationIp=" << headers.ipv4Header.GetDestination() << " priority="
                     << static_cast<uint32_t>(headers.datalinkPacketHeader.GetPacketVL())
                     << " packetUid=" << packet->GetUid());
    }
    const uint8_t tpOpcode = headers.transportHeader.GetTPOpcode();
    if (UbTransportChannel::IsTransportResponseOpcode(tpOpcode)) {
        NS_LOG_DEBUG("[UbPort recv] is ACK");
        UbDatalinkPacketHeader tempDlHeader;
        UbIpBasedNetworkHeader tempNetHeader;
        Ipv4Header tempIpv4Header;
        UdpHeader tempUdpHeader;
        packet->RemoveHeader(tempDlHeader);
        packet->RemoveHeader(tempNetHeader);
        packet->RemoveHeader(tempIpv4Header);
        packet->RemoveHeader(tempUdpHeader);
        targetTp->RecvTpAck(packet);
    } else if (tpOpcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_UNRELIABLE_TA) ||
               tpOpcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_RELIABLE_TA)) {
        targetTp->RecvDataPacket(packet);
    } else {
        NS_LOG_WARN("Dropping reserved transport opcode " << static_cast<uint32_t>(tpOpcode));
        utils::UbUtils::RecordRuntimePacketDrop("reserved transport opcode");
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
    uint32_t dnode = headers.network.destination.nodeId;
    // Forward
    if (dnode != GetObject<Node>()->GetId()) {
        return false;
    }
    if (headers.network.destination.hasPort)
    {
        NS_ABORT_MSG_IF(headers.network.destination.portId != port->GetIfIndex(),
                        "LDST destination endpoint port does not match the receiving UB port");
    }
    // Sink Packet
    if (IsCBFCEnable() || IsCBFCSharedEnable()) {
        port->GetFlowControl()->OnIngressEnqueued(
            MakeIngressFlowControlEventContext(packet,
                                               nullptr,
                                               port->GetIfIndex(),
                                               port->GetIfIndex(),
                                               headers.datalinkPacketHeader.GetPacketVL()));
    }

    auto ldstApi = GetObject<Node>()->GetObject<UbController>()->GetUbFunction()->GetUbLdstApi();
    NS_ASSERT_MSG(ldstApi != nullptr, "UbLdstApi can not be nullptr!");

    uint8_t type = headers.dummyTransactionHeader.GetUbTransactionOpcode();
    // 数据包
    if (type == (uint8_t)UbTransactionOpcode::WRITE || type == (uint8_t)UbTransactionOpcode::READ) {
        ldstApi->RecvDataPacket(packet, port->GetIfIndex());
    } else if (type == (uint8_t)UbTransactionOpcode::TAACK ||
               type == (uint8_t)UbTransactionOpcode::READ_RESPONSE) {
        ldstApi->RecvResponse(packet);
        NS_LOG_DEBUG("ldst packet is ack!");
    } else {
        NS_ASSERT_MSG(0, "packet Ta Op code is wrong!");
    }
    return true;
}

/**
 * @brief Sink CTP type data packet. Task 6 wires real receive semantics.
 */
bool UbSwitch::SinkCtpDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedCtpHeaders &headers)
{
    uint32_t dnode = headers.isCna24
                         ? utils::Cna24ToNodeId(headers.cna24NetworkHeader.GetDcna())
                         : utils::Cna16ToNodeId(headers.cna16NetworkHeader.GetDcna());
    if (dnode != GetObject<Node>()->GetId()) {
        return false;
    }
    if (!headers.isCna24 && (headers.cna16NetworkHeader.GetDcna() & 0xF) != 0)
    {
        NS_ABORT_MSG_IF(utils::Cna16ToPortId(headers.cna16NetworkHeader.GetDcna()) !=
                            port->GetIfIndex(),
                        "CTP destination endpoint port does not match the receiving UB "
                        "port");
    }

    if (IsCBFCEnable() || IsCBFCSharedEnable()) {
        port->GetFlowControl()->OnIngressEnqueued(
            MakeIngressFlowControlEventContext(packet,
                                               nullptr,
                                               port->GetIfIndex(),
                                               port->GetIfIndex(),
                                               headers.datalinkPacketHeader.GetPacketVL()));
    }

    Ptr<UbController> controller = GetObject<Node>()->GetObject<UbController>();
    if (controller != nullptr) {
        Ptr<UbCtpTransportService> ctpService = controller->GetCtpTransportService();
        if (ctpService != nullptr) {
            ctpService->HandleReceivedPacket(CopyPacketTransfer(packet), port->GetIfIndex());
        }
    }

    NS_LOG_DEBUG("[UbPort recv] local CTP packet consumed");
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

    // Route
    bool selectedShortestPath = false;
    int outPort =
        ResolveURMAOutput(packet, headers, port->GetIfIndex(), rtKey, selectedShortestPath);
    if (outPort < 0) {
        ReportForwardingRouteFailure(rtKey, port->GetIfIndex());
        return;
    }

    // Buffer management: check input port buffer space
    uint32_t inPort = port->GetIfIndex();
    uint8_t priority = headers.datalinkPacketHeader.GetPacketVL();
    uint32_t pSize = packet->GetSize();
    NS_ABORT_MSG_IF(priority == 0,
                    "Unified-bus reserves priority 0 for locally generated control frames in the "
                    "simulator model. Data packets must use priority 1..15.");

    if (!m_queueManager->CheckInPortSpace(inPort, priority, pSize)) {
        utils::UbUtils::RecordRuntimePacketDrop("ingress buffer full");
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

    // Route
    bool selectedShortestPath = false;
    int outPort =
        ResolveLdstOutput(packet, headers, port->GetIfIndex(), rtKey, selectedShortestPath);
    if (outPort < 0) {
        ReportForwardingRouteFailure(rtKey, port->GetIfIndex());
        return;
    }

    // Buffer management: check input port buffer space
    uint32_t inPort = port->GetIfIndex();
    uint8_t priority = headers.datalinkPacketHeader.GetPacketVL();
    uint32_t pSize = packet->GetSize();
    NS_ABORT_MSG_IF(priority == 0,
                    "Unified-bus reserves priority 0 for locally generated control frames in the "
                    "simulator model. Data packets must use priority 1..15.");

    if (!m_queueManager->CheckInPortSpace(inPort, priority, pSize)) {
        utils::UbUtils::RecordRuntimePacketDrop("ingress buffer full");
        NS_LOG_WARN("NodeId " << GetObject<Node>()->GetId() << " InPort " << inPort << " pri=" << (uint32_t)priority
                    << " buffer full. Packet Dropped!");
        return;
    }

    SendPacket(packet, inPort, outPort, priority);
}

/**
 * @brief Forward CTP data packet (headers already parsed)
 */
void UbSwitch::ForwardDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedCtpHeaders &headers)
{
    RoutingKey rtKey;

    bool selectedShortestPath = false;
    int outPort =
        ResolveCtpOutput(packet, headers, port->GetIfIndex(), rtKey, selectedShortestPath);
    if (outPort < 0) {
        ReportForwardingRouteFailure(rtKey, port->GetIfIndex());
        return;
    }

    uint32_t inPort = port->GetIfIndex();
    uint8_t priority = headers.datalinkPacketHeader.GetPacketVL();
    uint32_t pSize = packet->GetSize();
    NS_ABORT_MSG_IF(priority == 0,
                    "Unified-bus reserves priority 0 for locally generated control frames in the "
                    "simulator model. Data packets must use priority 1..15.");

    if (!m_queueManager->CheckInPortSpace(inPort, priority, pSize)) {
        utils::UbUtils::RecordRuntimePacketDrop("ingress buffer full");
        NS_LOG_WARN("NodeId " << GetObject<Node>()->GetId() << " InPort " << inPort << " pri=" << (uint32_t)priority
                    << " buffer full. Packet Dropped!");
        return;
    }

    SendPacket(packet, inPort, outPort, priority);
}

void UbSwitch::ForceShortestPathRouting(Ptr<Packet> packet, const UbDatalinkPacketHeader &parsedHeader)
{
    UbDatalinkPacketHeader modifiedHeader = parsedHeader;
    modifiedHeader.SetRoutingType(
        MakeRoutingType(RoutingTypeIsPerPacket(parsedHeader.GetRoutingType()), true));

    UbDatalinkPacketHeader tempHeader;
    packet->RemoveHeader(tempHeader);
    packet->AddHeader(modifiedHeader);
}

/**
 * @brief Shared URMA output resolution used by arrival forwarding and recovery transfer.
 *
 * Extracted unchanged from the arrival path so recovered packets use the same routing key,
 * endpoint/ingress constraints, one-detour behavior, and failed-output filter.
 */
int
UbSwitch::ResolveURMAOutput(Ptr<Packet> packet,
                            const ParsedURMAHeaders& headers,
                            uint32_t inPort,
                            RoutingKey& rtKey,
                            bool& selectedShortestPath)
{
    GetURMARoutingKey(headers, rtKey);
    const bool hasExplicitDestinationPort = (rtKey.dip & 0xff) != 0;
    const int outPort = m_routingProcess->GetOutPortAfterFailure(
        rtKey, selectedShortestPath, inPort, hasExplicitDestinationPort);

    // If packet routed via non-shortest path, force subsequent hops to use shortest path
    if (outPort >= 0 && !selectedShortestPath) {
        ForceShortestPathRouting(packet, headers.datalinkPacketHeader);
    }
    return outPort;
}

/**
 * @brief Shared LDST output resolution used by arrival forwarding and recovery transfer.
 */
int
UbSwitch::ResolveLdstOutput(Ptr<Packet> packet,
                            const ParsedLdstHeaders& headers,
                            uint32_t inPort,
                            RoutingKey& rtKey,
                            bool& selectedShortestPath)
{
    GetLdstRoutingKey(headers, rtKey);
    const bool hasExplicitDestinationPort = headers.network.destination.hasPort;
    const int outPort = m_routingProcess->GetOutPortAfterFailure(
        rtKey, selectedShortestPath, inPort, hasExplicitDestinationPort);

    if (outPort >= 0 && !selectedShortestPath) {
        ForceShortestPathRouting(packet, headers.datalinkPacketHeader);
    }
    return outPort;
}

/**
 * @brief Shared CTP output resolution used by arrival forwarding and recovery transfer.
 */
int
UbSwitch::ResolveCtpOutput(Ptr<Packet> packet,
                           const ParsedCtpHeaders& headers,
                           uint32_t inPort,
                           RoutingKey& rtKey,
                           bool& selectedShortestPath)
{
    GetCtpRoutingKey(headers, rtKey);

    const bool hasExplicitDestinationPort = headers.isCna24
                                                ? (headers.cna24NetworkHeader.GetDcna() & 0xff) != 0
                                                : (headers.cna16NetworkHeader.GetDcna() & 0xf) != 0;
    const bool hasCompactEntityId =
        headers.ctpHeader.GetNlp() == UB_CTPH_NLP_UPI16_EID40_TAH;
    if (!hasExplicitDestinationPort && hasCompactEntityId)
    {
        const uint32_t destinationNodeId =
            headers.isCna24
                ? utils::Cna24ToNodeId(headers.cna24NetworkHeader.GetDcna())
                : utils::Cna16ToNodeId(headers.cna16NetworkHeader.GetDcna());
        NS_ABORT_MSG_IF(destinationNodeId >= NodeList::GetNNodes(),
                        "CTP destination node does not exist: " << destinationNodeId);
        Ptr<UbController> destinationController =
            NodeList::GetNode(destinationNodeId)->GetObject<UbController>();
        NS_ABORT_MSG_IF(destinationController == nullptr ||
                            !destinationController->AreCtpEntitiesFrozen(),
                        "CTP destination Entity registry must be frozen before forwarding to node "
                            << destinationNodeId);
        const uint32_t destinationEntityId =
            headers.compactEidHeader.GetDestinationEid();
        NS_ABORT_MSG_IF(!destinationController->HasCtpEntity(destinationEntityId),
                        "CTP destination Entity " << destinationEntityId
                                                   << " is not configured on node "
                                                   << destinationNodeId);
        const uint8_t loadBalanceField =
            headers.isCna24 ? headers.cna24NetworkHeader.GetLb()
                            : headers.cna16NetworkHeader.GetLb();
        const uint16_t destinationPort = destinationController->SelectCtpEntityMemberPort(
            destinationEntityId,
            loadBalanceField);
        rtKey.dip = utils::NodeIdToIp(destinationNodeId, destinationPort).Get();
        rtKey.dport = destinationPort;
    }
    const int outPort = m_routingProcess->GetOutPortAfterFailure(
        rtKey, selectedShortestPath, inPort, hasExplicitDestinationPort || hasCompactEntityId);

    if (outPort >= 0 && !selectedShortestPath) {
        ForceShortestPathRouting(packet, headers.datalinkPacketHeader);
    }
    return outPort;
}

void
UbSwitch::ReportForwardingRouteFailure(const RoutingKey& key, uint32_t inPort)
{
    if (m_routingProcess != nullptr && m_routingProcess->LastFailureExcludedAllCandidates())
    {
        NS_FATAL_ERROR("No legal output remains after a detected link failure: node "
                       << GetObject<Node>()->GetId() << " inPort " << inPort << " failedOutput "
                       << (m_detectedFailedOutput == UINT32_MAX
                               ? std::string("none")
                               : std::to_string(m_detectedFailedOutput))
                       << " destination " << Ipv4Address(key.dip) << " RoutingType "
                       << static_cast<uint32_t>(key.routingType) << " priority "
                       << static_cast<uint32_t>(key.priority)
                       << " (original-ingress exclusion and endpoint restrictions stay in effect)");
    }
    utils::UbUtils::RecordRuntimePacketDrop("route cannot be found");
    NS_LOG_WARN("The route cannot be found. Packet Dropped!");
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
    // Use a read-only view: reconstructing from decoded fields would lose CCI bits.
    Ptr<Packet> view = CopyPacketView(packet);
    view->RemoveHeader(headers.datalinkPacketHeader);
    headers.network = RemoveLdstNetworkHeader(view, headers.datalinkPacketHeader.GetConfig());
    view->PeekHeader(headers.dummyTransactionHeader);
}

void UbSwitch::ParseCtpPacketHeader(Ptr<Packet> packet, ParsedCtpHeaders &headers)
{
    packet->RemoveHeader(headers.datalinkPacketHeader);
    headers.isCna24 = headers.datalinkPacketHeader.GetConfig() ==
                      static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_CNA24);
    if (headers.isCna24) {
        packet->RemoveHeader(headers.cna24NetworkHeader);
    } else {
        packet->RemoveHeader(headers.cna16NetworkHeader);
    }
    packet->RemoveHeader(headers.ctpHeader);
    if (headers.ctpHeader.GetNlp() == UB_CTPH_NLP_UPI16_EID40_TAH)
    {
        UbCompactUpiHeader upiHeader;
        packet->RemoveHeader(upiHeader);
        packet->PeekHeader(headers.compactEidHeader);
        packet->AddHeader(upiHeader);
    }
    packet->AddHeader(headers.ctpHeader);
    if (headers.isCna24) {
        packet->AddHeader(headers.cna24NetworkHeader);
    } else {
        packet->AddHeader(headers.cna16NetworkHeader);
    }
    packet->AddHeader(headers.datalinkPacketHeader);
}

void UbSwitch::GetURMARoutingKey(const ParsedURMAHeaders &headers, RoutingKey &rtKey)
{
    rtKey.sip = headers.ipv4Header.GetSource().Get();
    rtKey.dip = headers.ipv4Header.GetDestination().Get();
    rtKey.sport = headers.udpHeader.GetSourcePort();
    rtKey.dport = headers.udpHeader.GetDestinationPort();
    rtKey.priority = headers.datalinkPacketHeader.GetPacketVL();
    rtKey.routingType = headers.datalinkPacketHeader.GetRoutingType();
}

void UbSwitch::GetLdstRoutingKey(const ParsedLdstHeaders &headers, RoutingKey &rtKey)
{
    const auto& network = headers.network;
    uint32_t snode = network.source.nodeId;
    uint32_t dnode = network.destination.nodeId;
    const bool sourcePortExplicit = network.source.hasPort;
    uint16_t sport = network.source.portId;
    uint16_t dport = network.destination.portId;
    uint16_t lb = network.lb;
    rtKey.sip = sourcePortExplicit ? utils::NodeIdToIp(snode, sport).Get()
                                   : utils::NodeIdToIp(snode).Get();
    // Keep the legacy LDST destination lookup key for wildcard CNA16 addresses. The
    // original CNA low nibble is used by forwarding to distinguish wildcard from exact
    // endpoint routing, so this key must not be used to infer port presence.
    rtKey.dip = utils::NodeIdToIp(dnode, dport).Get();
    rtKey.sport = lb;
    rtKey.dport = dport;
    rtKey.priority = headers.datalinkPacketHeader.GetPacketVL();
    rtKey.routingType = headers.datalinkPacketHeader.GetRoutingType();
}

void UbSwitch::GetCtpRoutingKey(const ParsedCtpHeaders &headers, RoutingKey &rtKey)
{
    uint32_t snode = 0;
    uint32_t dnode = 0;
    uint16_t sport = 0;
    uint16_t dport = 0;
    uint16_t lb = 0;
    if (headers.isCna24) {
        const uint32_t dCna = headers.cna24NetworkHeader.GetDcna();
        const uint32_t sCna = headers.cna24NetworkHeader.GetScna();
        snode = utils::Cna24ToNodeId(sCna);
        dnode = utils::Cna24ToNodeId(dCna);
        sport = Cna24ToRoutingPortId(sCna);
        dport = Cna24ToRoutingPortId(dCna);
        lb = headers.cna24NetworkHeader.GetLb();
    } else {
        const uint16_t dCna = headers.cna16NetworkHeader.GetDcna();
        const uint16_t sCna = headers.cna16NetworkHeader.GetScna();
        snode = utils::Cna16ToNodeId(sCna);
        dnode = utils::Cna16ToNodeId(dCna);
        const bool sourcePortExplicit = (sCna & 0xF) != 0;
        const bool destinationPortExplicit = (dCna & 0xF) != 0;
        sport = sourcePortExplicit ? utils::Cna16ToPortId(sCna) : 0;
        dport = destinationPortExplicit ? utils::Cna16ToPortId(dCna) : 0;
        lb = headers.cna16NetworkHeader.GetLb();
        rtKey.sip = sourcePortExplicit ? utils::NodeIdToIp(snode, sport).Get()
                                       : utils::NodeIdToIp(snode).Get();
        // Preserve the legacy CTP wildcard destination lookup key. CNA16 wildcard
        // destination routing historically used endpoint port 0, while an explicit
        // port still selects its own endpoint route.
        rtKey.dip = utils::NodeIdToIp(dnode, dport).Get();
    }
    if (headers.isCna24)
    {
        rtKey.sip = utils::NodeIdToIp(snode, sport).Get();
        rtKey.dip = utils::NodeIdToIp(dnode, dport).Get();
    }
    rtKey.sport = lb;
    rtKey.dport = dport;
    rtKey.priority = headers.datalinkPacketHeader.GetPacketVL();
    rtKey.routingType = headers.datalinkPacketHeader.GetRoutingType();
}

/**
 * @brief Packet enters VOQ from input port
 * Place packet into VOQ[outPort][priority][inPort] and update buffer statistics
 */
void UbSwitch::SendPacket(Ptr<Packet> packet, uint32_t inPort, uint32_t outPort, uint32_t priority)
{
    ForwardDataPacketAfterAdmission(packet, inPort, outPort, priority);
}

void
UbSwitch::SendLastAck(Ptr<Packet> packet)
{
    UbLastAckMessage message;
    NS_ABORT_MSG_IF(!ParseReliableReroutingLastAck(packet, message), "Invalid local LastAck");
    RoutingKey key{};
    key.sip = utils::NodeIdToIp(message.sourceNode).Get();
    key.dip = utils::NodeIdToIp(message.destinationNode).Get();
    key.priority = 1;
    key.routingType = RoutingType::PER_FLOW_SHORTEST_PATHS;
    bool shortest = false;
    const int output = m_routingProcess->GetOutPortAfterFailure(key, shortest);
    NS_ABORT_MSG_IF(output < 0, "Reliable rerouting has no legal LastAck output");
    GetOrCreateVoq(output, 1, output)->Push(packet);
    TraceReliableReroutingLastAck(packet, GetObject<Node>()->GetId(), output, "lastack_submit");
    DynamicCast<UbPort>(GetObject<Node>()->GetDevice(output))->TriggerTransmit();
}

void
UbSwitch::HandleLastAck(Ptr<UbPort> port, Ptr<Packet> packet)
{
    const auto node = GetObject<Node>();
    TraceReliableReroutingLastAck(packet, node->GetId(), port->GetIfIndex(), "lastack_hop_receive");
    UbLastAckMessage message;
    if (!ParseReliableReroutingLastAck(packet, message))
    {
        TraceReliableReroutingLastAck(packet, node->GetId(), port->GetIfIndex(), "lastack_format_rejected");
        return;
    }
    if (IsCBFCEnable() || IsCBFCSharedEnable())
    {
        port->GetFlowControl()->OnDataPacketReceived(packet);
    }
    if (message.destinationNode == node->GetId())
    {
        if (IsCBFCEnable() || IsCBFCSharedEnable())
        {
            port->GetFlowControl()->OnIngressEnqueued(MakeIngressFlowControlEventContext(
                packet, nullptr, port->GetIfIndex(), port->GetIfIndex(), 1));
        }
        if (message.destinationPort >= m_portsNum)
        {
            TraceReliableReroutingLastAck(packet, node->GetId(), port->GetIfIndex(), "lastack_port_rejected");
            return;
        }
        auto target = DynamicCast<UbPort>(node->GetDevice(message.destinationPort))->GetReliableRerouting();
        if (target->IsArmed())
        {
            target->ReceiveLastAck(packet, message);
        }
        return;
    }
    RoutingKey key{};
    key.sip = utils::NodeIdToIp(message.sourceNode).Get();
    key.dip = utils::NodeIdToIp(message.destinationNode).Get();
    key.priority = 1;
    key.routingType = RoutingType::PER_FLOW_SHORTEST_PATHS;
    bool shortest = false;
    const int output = m_routingProcess->GetOutPortAfterFailure(key, shortest, port->GetIfIndex());
    NS_ABORT_MSG_IF(output < 0, "Reliable rerouting has no legal LastAck forwarding output");
    if (!m_queueManager->CheckInPortSpace(port->GetIfIndex(), 1, packet->GetSize()))
    {
        utils::UbUtils::RecordRuntimePacketDrop("LastAck ingress buffer full");
        return;
    }
    TraceReliableReroutingLastAck(packet, node->GetId(), output, "lastack_forward");
    SendPacket(packet, port->GetIfIndex(), output, 1);
}

// Control frames use priority 0 and inPort==outPort in this simulator model.
// This is a performance-oriented modeling tradeoff, not a UB protocol requirement.
// The switch peeks once here, then the VOQ path relies on queue identity instead of re-parsing
// packet headers at every scheduling/accounting step.
void UbSwitch::SendControlFrame(Ptr<Packet> packet, uint32_t portId)
{
    NS_ABORT_MSG_IF(GetPacketType(packet) != UB_CONTROL_FRAME,
                    "SendControlFrame expects a real UB control-credit packet");
    uint32_t priority = 0;
    Ptr<Node> node = GetObject<Node>();
    Ptr<UbPort> port = DynamicCast<ns3::UbPort>(node->GetDevice(portId));
    GetOrCreateVoq(portId, priority, portId)->Push(packet);
    m_queueManager->PushToVoq(portId, portId, priority, packet->GetSize());

    port->TriggerTransmit();
}

void
UbSwitch::NotifyOutputDetectedFailed(uint32_t outPort)
{
    NS_ABORT_MSG_IF(outPort >= m_portsNum, "Detected failed output is out of range");
    if (m_detectedFailedOutput == outPort)
    {
        return;
    }
    NS_ABORT_MSG_IF(m_detectedFailedOutput != UINT32_MAX,
                    "Version one supports one detected failed output per switch");

    m_detectedFailedOutput = outPort;
    // Snapshot once at detection; subsequent completions update the count directly. New arrivals
    // already avoid this output, so recovery handoffs never need to scan processing events.
    m_pendingFailedProcessing = std::count_if(
        m_inPortProcessingEvents.begin(),
        m_inPortProcessingEvents.end(),
        [outPort](const InPortProcessingEvent& pending) {
            return pending.outPort == outPort && !pending.event.IsExpired();
        });
    m_routingProcess->SetOutputUnavailable(static_cast<uint16_t>(outPort));

    // Detection stops new allocations and invalidates pending grants/notifications for the output
    // without dequeuing, consuming credit, or advancing round-robin/DWRR service state.
    m_allocator->SuspendOutput(outPort);

    Ptr<Node> node = GetObject<Node>();
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(outPort));
    NS_ABORT_MSG_IF(port == nullptr, "Detected failed output has no port");
    port->SetOutputBlocked(true);

    NS_LOG_DEBUG("[UbSwitch] detected failed output " << outPort << " on node " << node->GetId());
}

void
UbSwitch::StartOutputRecovery(uint32_t outPort)
{
    NS_ABORT_MSG_IF(outPort != m_detectedFailedOutput,
                    "Recovery start requires the detected failed output");
    if (m_recoveryBacklogMigrated)
    {
        TryTransferPendingRecovery();
        return;
    }
    m_recoveryBacklogMigrated = true;

    Ptr<Node> node = GetObject<Node>();

    // Already-ready processing packets still hold their ingress reservations. Keep them after
    // packets that had reached the old VOQ earlier, without waiting for future ready callbacks.
    auto readyProcessing = std::move(m_pendingRecovery);
    m_pendingRecovery.clear();

    // Failed-egress backlog moves to recovery storage, then the shared VOQ without ingress admission.
    Ptr<UbPort> failedPort = DynamicCast<UbPort>(node->GetDevice(outPort));
    NS_ABORT_MSG_IF(failedPort == nullptr, "Detected failed output has no port");
    Ptr<UbEgressQueue> egressQueue = failedPort->GetUbQueue();
    while (!egressQueue->IsEmpty())
    {
        const PacketEntry entry = egressQueue->DoDequeue();
        Ptr<Packet> packet = std::get<2>(entry);
        if (packet == nullptr)
        {
            break;
        }
        const uint32_t inPort = std::get<0>(entry);
        const uint32_t priority = std::get<1>(entry);
        const uint32_t packetBytes = packet->GetSize();
        m_queueManager->RemoveEgressBufferedBytes(packetBytes);
        if (GetPacketType(packet) == UB_CONTROL_FRAME)
        {
            failedPort->GetReliableRerouting()->DiscardFailedEgress(packetBytes);
            continue;
        }
        m_queueManager->AddPendingRecoveryBytes(inPort, priority, packetBytes);
        ParkRecoveryPacket(packet, inPort, priority, /*ingressHeld=*/false);
    }

    // Old VOQ backlog: packets already admitted at the original ingress keep their reservation and
    //    only change output association.
    for (uint32_t priority = 0; priority < m_vlNum; ++priority)
    {
        const auto groupKey = BuildVoqGroupKey(outPort, priority);
        if (groupKey >= m_voq.groups.size())
        {
            continue;
        }
        auto& group = m_voq.groups[static_cast<size_t>(groupKey)];
        for (auto& voqEntry : group)
        {
            Ptr<UbPacketQueue>& queue = voqEntry.queue;
            if (queue == nullptr)
            {
                continue;
            }
            const uint32_t inPort = voqEntry.inPort;
            while (!queue->IsEmpty())
            {
                // GetNextPacket() already removes the head packet from the queue.
                Ptr<Packet> packet = queue->GetNextPacket();
                const uint32_t packetBytes = packet->GetSize();
                if (GetPacketType(packet) == UB_CONTROL_FRAME)
                {
                    // Link-local credit/PFC frames are not rerouted; dispose them consistently.
                    m_queueManager->PopFromVoq(inPort, outPort, priority, packetBytes);
                    continue;
                }
                m_queueManager->MoveVoqToPendingRecovery(inPort, outPort, priority, packetBytes);
                ParkRecoveryPacket(packet, inPort, priority, /*ingressHeld=*/true);
            }
        }
    }

    for (auto& entry : readyProcessing)
    {
        m_pendingRecovery.push_back(std::move(entry));
    }

    PublishRecoveryBacklogCounters(outPort);
    TryTransferPendingRecovery();
}

bool
UbSwitch::HasPendingRecoveryFor(uint32_t outPort) const
{
    return GetPendingRecoveryPacketCount(outPort) != 0;
}

void
UbSwitch::ParkRecoveryPacket(Ptr<Packet> packet,
                             uint32_t inPort,
                             uint32_t priority,
                             bool ingressHeld)
{
    PendingRecoveryEntry entry;
    entry.packet = packet;
    entry.inPort = inPort;
    entry.priority = priority;
    entry.oldOutput = m_detectedFailedOutput;
    entry.entryId = ++m_nextRecoveryEntryId;
    entry.ingressHeld = ingressHeld;
    m_pendingRecovery.push_back(entry);
    ++m_pendingRecoveryPackets;
    m_pendingRecoveryBytes += packet->GetSize();
    NS_LOG_DEBUG("[UbSwitch] parked recovery entry " << entry.entryId << " inPort " << inPort
                                                     << " pri " << priority << " ingressHeld "
                                                     << ingressHeld);
}

void
UbSwitch::TransferOnePendingEntry(PendingRecoveryEntry& entry)
{
    // Resolve once before transferring the retained packet to the shared VOQ.
    if (!entry.outputResolved)
    {
        uint16_t newOutput = UINT16_MAX;
        if (!ResolveRecoveryOutput(entry.packet, entry.inPort, entry.oldOutput, newOutput))
        {
            // Link-local control frames leave with their recovery accounting.
            m_queueManager->RemovePendingRecoveryBytes(entry.inPort,
                                                       entry.priority,
                                                       entry.packet->GetSize(),
                                                       entry.ingressHeld);
            --m_pendingRecoveryPackets;
            m_pendingRecoveryBytes -= entry.packet->GetSize();
            if (!entry.ingressHeld)
            {
                FailedOutputRerouting()->DiscardFailedEgress(entry.packet->GetSize());
            }
            PublishRecoveryBacklogCounters(entry.oldOutput);
            if (Ptr<UbReliableRerouting> rerouting = FailedOutputRerouting())
            {
                rerouting->PublishBacklogEvent("control_frame_discarded",
                                               entry.entryId,
                                               entry.oldOutput);
            }
            return;
        }
        entry.selectedOutput = newOutput;
        entry.outputResolved = true;
    }

    const uint32_t packetBytes = entry.packet->GetSize();
    // A recovered packet re-enters the healthy VOQ with no new input-processing delay and no repeated
    // receive/congestion effects, but with its original ingress identity and VL.
    ClearOldLinkPiggybackMetadata(entry.packet);
    if (!entry.ingressHeld)
    {
        // The original upstream credit was already returned when this packet left the ingress.
        UbRecoveredCreditTag creditTag;
        creditTag.failedOutput = entry.oldOutput;
        entry.packet->AddPacketTag(creditTag);
    }
    m_queueManager->AdmitPendingRecoveryToVoq(entry.inPort,
                                              entry.selectedOutput,
                                              entry.priority,
                                              packetBytes,
                                              entry.ingressHeld);
    GetOrCreateVoq(entry.selectedOutput, entry.priority, entry.inPort)->Push(entry.packet);
    --m_pendingRecoveryPackets;
    m_pendingRecoveryBytes -= packetBytes;
    if (!entry.ingressHeld)
    {
        FailedOutputRerouting()->EnqueueRecoveryVoq(packetBytes, UbRecoveryStage::FAILED_EGRESS);
    }
    PublishRecoveryBacklogCounters(entry.oldOutput);
    FinalizeForwardedPacketEnqueue(entry.packet,
                                   entry.inPort,
                                   entry.selectedOutput,
                                   entry.priority,
                                   true);
    m_lastRecoveryOutput = entry.selectedOutput;
    ++m_recoveryTransferCount;
    if (Ptr<UbReliableRerouting> rerouting = FailedOutputRerouting())
    {
        rerouting->PublishBacklogEvent("transfer",
                                       entry.entryId,
                                       entry.oldOutput,
                                       entry.selectedOutput);
    }
    NS_LOG_DEBUG("[UbSwitch] recovery transfer entry " << entry.entryId << " inPort "
                                                       << entry.inPort << " oldOutput "
                                                       << entry.oldOutput << " newOutput "
                                                       << entry.selectedOutput);
    return;
}

void
UbSwitch::TryTransferPendingRecovery()
{
    if (!m_recoveryBacklogMigrated || m_recoveryDisposed)
    {
        return;
    }

    while (!m_pendingRecovery.empty())
    {
        TransferOnePendingEntry(m_pendingRecovery.front());
        m_pendingRecovery.erase(m_pendingRecovery.begin());
    }

    PublishRecoveryBacklogCounters(m_detectedFailedOutput);
    if (m_pendingRecovery.empty())
    {
        if (Ptr<UbReliableRerouting> rerouting = FailedOutputRerouting())
        {
            rerouting->NotifyBacklogFinished();
        }
    }
}

void
UbSwitch::ClearRecoveryBacklogForOutput(uint32_t outPort)
{
    uint64_t released = 0;
    for (auto it = m_pendingRecovery.begin(); it != m_pendingRecovery.end();)
    {
        if (it->oldOutput == outPort)
        {
            released += it->packet->GetSize();
            --m_pendingRecoveryPackets;
            m_pendingRecoveryBytes -= it->packet->GetSize();
            it = m_pendingRecovery.erase(it);
        }
        else
        {
            ++it;
        }
    }
    if (released != 0)
    {
        NS_LOG_DEBUG("[UbSwitch] released " << released << " retained recovery bytes on dispose");
    }
}

void
UbSwitch::ClearOldLinkPiggybackMetadata(Ptr<Packet> packet)
{
    if (packet == nullptr)
    {
        return;
    }
    // A retained packet may carry the failed link's piggyback credit or link-ACK indication. The
    // alternate neighbour must never receive either; the new output attaches its own.
    UbDatalinkPacketHeader header;
    packet->RemoveHeader(header);
    if (header.GetCredit() || header.GetACK())
    {
        header.SetCredit(false);
        header.SetACK(false);
        packet->AddHeader(header);
        return;
    }
    packet->AddHeader(header);
}

bool
UbSwitch::ResolveRecoveryOutput(Ptr<Packet> packet,
                                uint32_t inPort,
                                uint32_t oldOutput,
                                uint16_t& newOutput)
{
    ++m_recoveryResolutionCount;
    // Reuse the production classification, parsers, routing-key builders, and route resolution.
    const UbPacketType_t packetType = GetPacketType(packet);
    RoutingKey rtKey;
    bool selectedShortestPath = false;
    std::string packetFamily;
    int resolved = -1;
    switch (packetType)
    {
    case UB_URMA_DATA_PACKET:
    {
        ParsedURMAHeaders headers;
        ParseURMAPacketHeader(packet, headers);
        packetFamily = "RTP/URMA";
        resolved = ResolveURMAOutput(packet, headers, inPort, rtKey, selectedShortestPath);
        break;
    }
    case UB_LDST_DATA_PACKET:
    {
        ParsedLdstHeaders headers;
        ParseLdstPacketHeader(packet, headers);
        packetFamily = "LDST";
        resolved = ResolveLdstOutput(packet, headers, inPort, rtKey, selectedShortestPath);
        break;
    }
    case UB_CTP_DATA_PACKET:
    {
        ParsedCtpHeaders headers;
        ParseCtpPacketHeader(packet, headers);
        packetFamily = "CTP";
        resolved = ResolveCtpOutput(packet, headers, inPort, rtKey, selectedShortestPath);
        break;
    }
    default:
        // Link-local credit/PFC frames stay on the link. Their disposal during recovery cleanup is
        // not user-data loss and must never reach a healthy neighbour.
        NS_LOG_DEBUG("[UbSwitch] link-local control frame is not rerouted on node "
                     << GetObject<Node>()->GetId() << " inPort " << inPort);
        return false;
    }

    if (resolved < 0)
    {
        AbortNoRecoveryRoute(rtKey, inPort, oldOutput, packetFamily);
    }
    NS_ABORT_MSG_IF(static_cast<uint32_t>(resolved) == oldOutput,
                    "Recovery transfer must use a healthy output");
    newOutput = static_cast<uint16_t>(resolved);
    return true;
}

UbRecoverySubmitResult
UbSwitch::SubmitRecoveryPacket(Ptr<Packet> packet,
                               uint32_t inPort,
                               uint32_t priority,
                               uint32_t oldOutput,
                               uint64_t entryId)
{
    NS_ABORT_MSG_IF(packet == nullptr, "Recovery transfer requires a packet");
    NS_ABORT_MSG_IF(m_detectedFailedOutput == UINT32_MAX,
                    "Recovery transfer requires a detected failed output");
    NS_ABORT_MSG_IF(oldOutput != m_detectedFailedOutput,
                    "Recovery transfer requires the detected failed output");

    if (GetPacketType(packet) == UB_CONTROL_FRAME)
    {
        return UbRecoverySubmitResult::DISCARDED;
    }

    uint16_t newOutput = UINT16_MAX;
    if (!ResolveRecoveryOutput(packet, inPort, oldOutput, newOutput))
    {
        return UbRecoverySubmitResult::DISCARDED;
    }

    const uint32_t packetBytes = packet->GetSize();
    ClearOldLinkPiggybackMetadata(packet);
    // The original ingress credit was already returned when this attempt left the ingress, so the
    // allocator releases recovery storage without touching the original ingress reservation.
    UbRecoveredCreditTag creditTag;
    creditTag.failedOutput = oldOutput;
    packet->AddPacketTag(creditTag);
    m_queueManager->PushToVoq(inPort, newOutput, priority, packetBytes, true);
    GetOrCreateVoq(newOutput, priority, inPort)->Push(packet);
    FailedOutputRerouting()->EnqueueRecoveryVoq(packetBytes, UbRecoveryStage::RETRY);
    FinalizeForwardedPacketEnqueue(packet, inPort, newOutput, priority, true);
    m_lastRecoveryOutput = newOutput;
    ++m_recoveryTransferCount;
    NS_LOG_DEBUG("[UbSwitch] recovery transfer node "
                 << GetObject<Node>()->GetId() << " entry " << entryId << " inPort " << inPort
                 << " oldOutput " << oldOutput << " newOutput " << newOutput);
    return UbRecoverySubmitResult::TRANSFERRED;
}


void
UbSwitch::AbortNoRecoveryRoute(const RoutingKey& key,
                               uint32_t inPort,
                               uint32_t outPort,
                               const std::string& packetFamily) const
{
    NS_FATAL_ERROR("No legal recovery output: node "
                   << GetObject<Node>()->GetId() << " inPort " << inPort << " failedOutput "
                   << outPort << " destination " << Ipv4Address(key.dip) << " RoutingType "
                   << static_cast<uint32_t>(key.routingType) << " family " << packetFamily
                   << " (original-ingress exclusion, endpoint restrictions, and shortest-path "
                      "limits stay in effect)");
}

uint64_t
UbSwitch::GetPendingRecoveryPacketCount(uint32_t outPort) const
{
    return outPort == m_detectedFailedOutput ? m_pendingRecoveryPackets : 0;
}

uint64_t
UbSwitch::GetPendingRecoveryByteCount(uint32_t outPort) const
{
    return outPort == m_detectedFailedOutput ? m_pendingRecoveryBytes : 0;
}

void
UbSwitch::PublishRecoveryBacklogCounters(uint32_t outPort)
{
    if (outPort == UINT32_MAX)
    {
        return;
    }
    Ptr<Node> node = GetObject<Node>();
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(outPort));
    if (port == nullptr || port->GetReliableRerouting() == nullptr)
    {
        return;
    }
    port->GetReliableRerouting()->SetPendingRecoveryCounters(GetPendingRecoveryPacketCount(outPort),
                                                             GetPendingRecoveryByteCount(outPort),
                                                             m_pendingFailedProcessing);
}

Ptr<UbReliableRerouting>
UbSwitch::FailedOutputRerouting()
{
    if (m_detectedFailedOutput == UINT32_MAX)
    {
        return nullptr;
    }
    Ptr<UbPort> port = DynamicCast<UbPort>(GetObject<Node>()->GetDevice(m_detectedFailedOutput));
    return port == nullptr ? nullptr : port->GetReliableRerouting();
}

void UbSwitch::NotifySwitchDequeue(uint16_t inPortId, uint32_t outPort, uint32_t priority, Ptr<Packet> packet)
{
    UbRecoveredCreditTag recoveryTag;
    const bool recovery = packet->RemovePacketTag(recoveryTag);
    m_queueManager->PopFromVoq(inPortId, outPort, priority, packet->GetSize(), recovery);
    if (recovery)
    {
        auto origin = DynamicCast<UbPort>(GetObject<Node>()->GetDevice(recoveryTag.failedOutput));
        origin->GetReliableRerouting()->DequeueRecoveryVoq(packet->GetSize());
    }

    // 只有数据报文触发拥塞控制
    UbPacketType_t packetType = GetPacketType(packet);
    if (packetType != UB_CONTROL_FRAME && packetType != UB_LAST_ACK_PACKET && m_congestionCtrl != nullptr) {
        NS_LOG_DEBUG("[QMU] Node:" << GetObject<Node>()->GetId()
              << " port:" << outPort
              << " VOQ outPort buffer:" << m_queueManager->GetTotalOutPortBufferUsed(outPort));
        m_congestionCtrl->OnSwitchPostDequeue(inPortId, outPort, packet);
    }
}

bool UbSwitch::IsCBFCEnable()
{
    return m_flowControlType == FcType::CBFC;
}

bool UbSwitch::IsCBFCSharedEnable()
{
    return m_flowControlType == FcType::CBFC_SHARED;
}

bool UbSwitch::IsPFCEnable()
{
    return m_flowControlType == FcType::PFC_FIXED || m_flowControlType == FcType::PFC_DYNAMIC ||
           m_flowControlType == FcType::PFC_DYNAMIC_PAPER;
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

RoutingKey
UbSwitch::GetLdstRoutingKeyForTest(Ptr<Packet> packet)
{
    ParsedLdstHeaders headers;
    ParseLdstPacketHeader(packet, headers);

    RoutingKey rtKey;
    GetLdstRoutingKey(headers, rtKey);
    return rtKey;
}

RoutingKey
UbSwitch::GetCtpRoutingKeyForTest(Ptr<Packet> packet)
{
    ParsedCtpHeaders headers;
    ParseCtpPacketHeader(packet, headers);

    RoutingKey rtKey;
    GetCtpRoutingKey(headers, rtKey);
    return rtKey;
}

void UbSwitch::LastPacketTraversesNotify(uint32_t nodeId, UbTransportHeader ubTpHeader)
{
    m_traceLastPacketTraversesNotify(nodeId, ubTpHeader);
}

uint32_t
UbSwitch::GetVoqPacketCountForTest(uint32_t outPort, uint32_t priority, uint32_t inPort) const
{
    if (!IsValidVoqIndices(outPort, priority, inPort, m_portsNum, m_vlNum)) {
        return 0;
    }
    const Ptr<UbPacketQueue>* queue = FindVoqSlot(BuildVoqGroupKey(outPort, priority), inPort);
    if (queue == nullptr || *queue == nullptr) {
        return 0;
    }
    return static_cast<uint32_t>((*queue)->GetPacketCount());
}

}  // namespace ns3
