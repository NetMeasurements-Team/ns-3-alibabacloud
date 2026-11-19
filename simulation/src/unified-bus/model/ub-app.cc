// SPDX-License-Identifier: GPL-2.0-only
#include "ub-app.h"
#include "ns3/ub-enum-accessor.h"

#include "ub-traffic-gen.h"

#include "ns3/callback.h"
#include "ns3/enum.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/ub-ctp.h"
#include "ns3/ub-datatype.h"
#include "ns3/ub-function.h"
#include "ns3/ub-port.h"
#include "ns3/ub-routing-process.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-utils.h"
#include "ns3/uinteger.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("UbApp");

NS_OBJECT_ENSURE_REGISTERED(UbApp);

TypeId
UbApp::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbApp")
            .SetParent<Application>()
            .SetGroupName("UnifiedBus")
            .AddConstructor<UbApp>()
            .AddAttribute("EnableMultiPath",
                          "Enable multi-path transport: create TPs on multiple source ports toward "
                          "the same destination.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&UbApp::m_multiPathEnable),
                          MakeBooleanChecker())
            .AddAttribute("EnableTaskScopedJettyDestroy",
                          "Destroy standard-driver task-scoped Jetties after task completion.",
                          BooleanValue(true),
                          MakeBooleanAccessor(&UbApp::m_enableTaskScopedJettyDestroy),
                          MakeBooleanChecker())
            .AddAttribute(
                "EnablePlannedTpConnectionDestroy",
                "Destroy exact standard-driver-owned TP connections after their last planned use.",
                BooleanValue(true),
                MakeBooleanAccessor(&UbApp::m_enablePlannedTpConnectionDestroy),
                MakeBooleanChecker())
            .AddAttribute("RoutingType",
                          "UB routing type used by application-created transport channels.",
                          MakeUbEnumValue(RoutingType::PER_FLOW_SHORTEST_PATHS),
                          MakeUbEnumAccessor(&UbApp::m_routingType),
                          MakeUbEnumChecker(RoutingType::PER_FLOW_ALL_PATHS,
                                          "PER_FLOW_ALL_PATHS",
                                          RoutingType::PER_PACKET_ALL_PATHS,
                                          "PER_PACKET_ALL_PATHS",
                                          RoutingType::PER_FLOW_SHORTEST_PATHS,
                                          "PER_FLOW_SHORTEST_PATHS",
                                          RoutingType::PER_PACKET_SHORTEST_PATHS,
                                          "PER_PACKET_SHORTEST_PATHS"))
            .AddAttribute("TransportMode",
                          "Transport mode used for URMA traffic.",
                          MakeUbEnumValue(TransportMode::RTP),
                          MakeUbEnumAccessor(&UbApp::m_transportMode),
                          MakeUbEnumChecker(TransportMode::RTP, "RTP", TransportMode::CTP, "CTP"))
            .AddAttribute("LocalEntityId",
                          "Default local entity id used for CTP URMA traffic.",
                          UintegerValue(1),
                          MakeUintegerAccessor(&UbApp::m_localEntityId),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("PeerEntityId",
                          "Default peer entity id used for CTP URMA traffic.",
                          UintegerValue(1),
                          MakeUintegerAccessor(&UbApp::m_peerEntityId),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute(
                "CtpUseUnboundSourceJetty",
                "When true, CTP reuses one unbound source Jetty per source entity and VL.",
                BooleanValue(false),
                MakeBooleanAccessor(&UbApp::m_ctpUseUnboundSourceJetty),
                MakeBooleanChecker())
            .AddTraceSource("MemTaskStartsNotify",
                            "MEM Task Starts, taskId",
                            MakeTraceSourceAccessor(&UbApp::m_traceMemTaskStartsNotify),
                            "ns3::UbApp::MemTaskStartsNotify")
            .AddTraceSource("MemTaskCompletesNotify",
                            "MEM Task Completes, taskId",
                            MakeTraceSourceAccessor(&UbApp::m_traceMemTaskCompletesNotify),
                            "ns3::UbApp::MemTaskCompletesNotify")
            .AddTraceSource("ComputeTaskQueuedNotify",
                            "COMPUTE Task Queued, taskId",
                            MakeTraceSourceAccessor(&UbApp::m_traceComputeTaskQueuedNotify),
                            "ns3::UbApp::ComputeTaskQueuedNotify")
            .AddTraceSource("ComputeTaskStartsNotify",
                            "COMPUTE Task Starts, taskId",
                            MakeTraceSourceAccessor(&UbApp::m_traceComputeTaskStartsNotify),
                            "ns3::UbApp::ComputeTaskStartsNotify")
            .AddTraceSource("ComputeTaskCompletesNotify",
                            "COMPUTE Task Completes, taskId",
                            MakeTraceSourceAccessor(&UbApp::m_traceComputeTaskCompletesNotify),
                            "ns3::UbApp::ComputeTaskCompletesNotify")
            .AddTraceSource("WqeTaskStartsNotify",
                            "WQE Task Starts, taskId",
                            MakeTraceSourceAccessor(&UbApp::m_traceWqeTaskStartsNotify),
                            "ns3::UbApp::WqeTaskStartsNotify")
            .AddTraceSource("WqeTaskCompletesNotify",
                            "WQE Task Completes, taskId",
                            MakeTraceSourceAccessor(&UbApp::m_traceWqeTaskCompletesNotify),
                            "ns3::UbApp::WqeTaskCompletesNotify");
    return tid;
}

UbApp::UbApp()
{
}

UbApp::~UbApp()
{
}

void
UbApp::SetFinishCallback(Callback<void, uint32_t, uint32_t> cb, Ptr<UbJetty> jetty)
{
    jetty->SetClientCallback(cb);
}

void
UbApp::SetFinishCallback(Callback<void, uint32_t> cb, Ptr<UbLdstInstance> ubLdstInstance)
{
    ubLdstInstance->SetClientCallback(cb);
}

void
UbApp::DoDispose(void)
{
    for (auto& entry : m_completionVisibilityEvents)
    {
        Simulator::Cancel(entry.second);
    }
    m_completionVisibilityEvents.clear();
    Application::DoDispose();
}

void
UbApp::SetTransportMode(TransportMode mode)
{
    m_transportMode = mode;
}

void
UbApp::SetLocalEntityId(uint32_t localEntityId)
{
    m_localEntityId = localEntityId;
}

void
UbApp::SetPeerEntityId(uint32_t peerEntityId)
{
    m_peerEntityId = peerEntityId;
}

void
UbApp::SendTraffic(TrafficRecord record)
{
    NS_ABORT_MSG_IF(record.priority < 0 || static_cast<uint32_t>(record.priority) > UB_PRIORITY_MAX,
                    "Invalid priority field in traffic.csv; valid range is 0.."
                        << static_cast<uint32_t>(UB_PRIORITY_MAX));

    UbTrafficGen::RuntimeTask task;
    task.taskId = static_cast<uint32_t>(record.taskId);
    task.sourceNode = static_cast<uint32_t>(record.sourceNode);
    task.destNode = static_cast<uint32_t>(record.destNode);
    task.dataSize = static_cast<uint32_t>(record.dataSize);
    task.priority = static_cast<uint8_t>(record.priority);
    task.delay = record.delay.empty() ? Time(0) : Time(record.delay);
    task.srcPortId = record.srcPortId;
    task.dstPortId = record.dstPortId;
    task.hasSrcPortId = record.hasSrcPortId;
    task.hasDstPortId = record.hasDstPortId;
    UbTrafficGen::ParseTaskOperation(task,
                                     record.opType,
                                     record.atomicSubOperation,
                                     record.requestPayloadBytes);
    SendTraffic(task);
}

void
UbApp::SendCtpUrmaTraffic(TrafficRecord record)
{
    NS_ABORT_MSG_IF(record.priority < 0 || static_cast<uint32_t>(record.priority) > UB_PRIORITY_MAX,
                    "Invalid priority field in traffic.csv; valid range is 0.."
                        << static_cast<uint32_t>(UB_PRIORITY_MAX));

    UbTrafficGen::RuntimeTask task;
    task.taskId = static_cast<uint32_t>(record.taskId);
    task.sourceNode = static_cast<uint32_t>(record.sourceNode);
    task.destNode = static_cast<uint32_t>(record.destNode);
    task.dataSize = static_cast<uint32_t>(record.dataSize);
    task.priority = static_cast<uint8_t>(record.priority);
    task.srcPortId = record.srcPortId;
    task.dstPortId = record.dstPortId;
    task.hasSrcPortId = record.hasSrcPortId;
    task.hasDstPortId = record.hasDstPortId;
    UbTrafficGen::ParseTaskOperation(task,
                                     record.opType,
                                     record.atomicSubOperation,
                                     record.requestPayloadBytes);
    SendCtpUrmaTraffic(task);
}

void
UbApp::SendCtpUrmaTraffic(UbTrafficGen::RuntimeTask task)
{
    NS_ABORT_MSG_IF(!UbTrafficGen::IsUrmaTask(task.op),
                    "CTP traffic requires an URMA Read/Write/atomic operation");

    Ptr<UbController> controller = GetNode()->GetObject<UbController>();
    NS_ABORT_MSG_IF(controller == nullptr, "CTP transport mode requires UbController");
    Ptr<UbFunction> ubFunc = controller->GetUbFunction();
    NS_ABORT_MSG_IF(ubFunc == nullptr, "CTP transport mode requires UbFunction");
    NS_ABORT_MSG_IF(controller->GetUbTransaction() == nullptr,
                    "CTP transport mode requires UbTransaction");

    Ptr<UbCtpTransportService> service = controller->GetCtpTransportService();
    service->SetRoutingType(m_routingType);

    const uint32_t srcEntityId = m_localEntityId;
    const uint32_t dstEntityId = m_peerEntityId;
    NS_ABORT_MSG_IF(task.hasSrcPortId && task.srcPortId > CTP_MAX_CNA16_PORT_ID,
                    "CTP source port " << task.srcPortId << " cannot be encoded as CNA16");
    NS_ABORT_MSG_IF(task.hasDstPortId && task.dstPortId > CTP_MAX_CNA16_PORT_ID,
                    "CTP destination port " << task.dstPortId << " cannot be encoded as CNA16");
    NS_ABORT_MSG_IF(!controller->AreCtpEntitiesFrozen(),
                    "CTP source Entity registry must be frozen before traffic activation");
    NS_ABORT_MSG_IF(!controller->HasCtpEntity(srcEntityId),
                    "CTP source Entity " << srcEntityId << " is not configured on node "
                                          << GetNode()->GetId());
    NS_ABORT_MSG_IF(task.hasSrcPortId &&
                        !controller->IsCtpEntityPortMember(srcEntityId, task.srcPortId),
                    "CTP source port " << task.srcPortId << " is not a member of Entity "
                                       << srcEntityId);

    Ptr<Node> peerNode = NodeList::GetNode(task.destNode);
    Ptr<UbController> peerController =
        peerNode == nullptr ? nullptr : peerNode->GetObject<UbController>();
    NS_ABORT_MSG_IF(peerController == nullptr, "CTP destination requires a peer UbController");
    NS_ABORT_MSG_IF(!peerController->AreCtpEntitiesFrozen(),
                    "CTP destination Entity registry must be frozen before traffic activation");
    NS_ABORT_MSG_IF(!peerController->HasCtpEntity(dstEntityId),
                    "CTP destination Entity " << dstEntityId << " is not configured on node "
                                               << task.destNode);
    NS_ABORT_MSG_IF(task.hasDstPortId &&
                        !peerController->IsCtpEntityPortMember(dstEntityId, task.dstPortId),
                    "CTP destination port " << task.dstPortId << " is not a member of Entity "
                                            << dstEntityId);
    const uint8_t vl = task.priority;
    uint32_t jettyNum = m_jettyNum;
    Ptr<UbJetty> jetty;
    if (m_ctpUseUnboundSourceJetty)
    {
        jetty = GetOrCreateCtpUnboundJetty(task.sourceNode, srcEntityId, vl, &jettyNum);
    }
    else
    {
        jetty = GetOrCreateCtpBoundJetty(task.sourceNode,
                                         task.destNode,
                                         srcEntityId,
                                         dstEntityId,
                                         vl,
                                         &jettyNum);
    }
    if (!ubFunc->IsJettyAcceptingWqes(jettyNum))
    {
        NS_LOG_WARN("CTP WQE submission rejected because Jetty " << jettyNum << " is draining");
        return;
    }
    service->SetRoutingType(m_routingType);
    UbCtpEntityKey key{srcEntityId, task.destNode, dstEntityId, vl};
    const UbCtpEntityKey jettyKey =
        m_ctpUseUnboundSourceJetty ? service->MakeSourceGroupKeyForRequest(key) : key;
    const auto preparation = service->PrepareJettyWithResult(jetty, jettyKey);
    NS_ABORT_MSG_IF(!preparation.IsSuccess(), preparation.message);

    NS_LOG_INFO("CTP WQE Starts, jettyNum: " << jettyNum << " taskId: " << task.taskId);
    WqeTaskStartsNotify(GetNode()->GetId(), jettyNum, task.taskId);

    UbTransactionOpcode type = UbTrafficGen::GetUrmaTaskOpcode(task.op);
    Ptr<UbWqe> wqe =
        ubFunc->CreateWqe(task.sourceNode, task.destNode, task.dataSize, task.taskId, type);
    if (task.atomicOptions)
    {
        wqe->SetAtomicOptions(*task.atomicOptions);
    }
    wqe->SetSrcEntityId(srcEntityId);
    wqe->SetDstEntityId(dstEntityId);
    wqe->SetSport(task.hasSrcPortId ? static_cast<uint8_t>(task.srcPortId)
                                    : CTP_WILDCARD_PORT);
    wqe->SetDport(task.hasDstPortId ? static_cast<uint8_t>(task.dstPortId)
                                    : CTP_WILDCARD_PORT);
    wqe->SetPriority(task.priority);
    controller->GetUbTransaction()->SetTransactionServiceMode(jettyNum,
                                                              TransactionServiceMode::ROI);
    jetty->SetNodeId(GetNode()->GetId());

    const UbWqeSubmissionResult submission = ubFunc->SubmitCtpWqe(wqe, jettyNum, jettyKey);
    NS_ABORT_MSG_IF(submission.status != UbWqeSubmissionStatus::ACCEPTED,
                    "Validated CTP WQE submission failed before transport start");
    service->StartJetty(jetty, jettyKey);
}

Ptr<UbJetty>
UbApp::GetOrCreateCtpBoundJetty(uint32_t sourceNode,
                                uint32_t destNode,
                                uint32_t srcEntityId,
                                uint32_t dstEntityId,
                                uint8_t vl,
                                uint32_t* jettyNum)
{
    NS_ABORT_MSG_IF(jettyNum == nullptr, "CTP bound Jetty output number is required");
    Ptr<UbFunction> ubFunc = GetNode()->GetObject<UbController>()->GetUbFunction();
    const auto key = std::make_tuple(sourceNode, destNode, srcEntityId, dstEntityId, vl);
    auto it = m_ctpBoundJettyByFullKey.find(key);
    if (it != m_ctpBoundJettyByFullKey.end())
    {
        *jettyNum = it->second;
        Ptr<UbJetty> jetty = ubFunc->GetJetty(*jettyNum);
        NS_ABORT_MSG_IF(jetty == nullptr, "CTP bound Jetty table points to missing Jetty");
        return jetty;
    }

    while (ubFunc->IsJettyExists(m_jettyNum))
    {
        ++m_jettyNum;
    }
    ubFunc->CreateJetty(sourceNode, destNode, m_jettyNum);
    Ptr<UbJetty> jetty = ubFunc->GetJetty(m_jettyNum);
    NS_ABORT_MSG_IF(jetty == nullptr, "CTP URMA failed to create bound Jetty");
    SetFinishCallback(MakeCallback(&UbApp::OnTaskCompleted, this), jetty);
    *jettyNum = m_jettyNum;
    m_ctpBoundJettyByFullKey[key] = m_jettyNum;
    ++m_jettyNum;
    return jetty;
}

Ptr<UbJetty>
UbApp::GetOrCreateCtpUnboundJetty(uint32_t sourceNode,
                                  uint32_t srcEntityId,
                                  uint8_t vl,
                                  uint32_t* jettyNum)
{
    NS_ABORT_MSG_IF(jettyNum == nullptr, "CTP unbound Jetty output number is required");
    Ptr<UbFunction> ubFunc = GetNode()->GetObject<UbController>()->GetUbFunction();
    const auto key = std::make_tuple(sourceNode, srcEntityId, vl);
    auto it = m_ctpUnboundJettyBySourceEntityVl.find(key);
    if (it != m_ctpUnboundJettyBySourceEntityVl.end())
    {
        *jettyNum = it->second;
        Ptr<UbJetty> jetty = ubFunc->GetJetty(*jettyNum);
        NS_ABORT_MSG_IF(jetty == nullptr, "CTP unbound Jetty table points to missing Jetty");
        return jetty;
    }

    while (ubFunc->IsJettyExists(m_jettyNum))
    {
        ++m_jettyNum;
    }
    ubFunc->CreateJetty(sourceNode, m_jettyNum);
    Ptr<UbJetty> jetty = ubFunc->GetJetty(m_jettyNum);
    NS_ABORT_MSG_IF(jetty == nullptr, "CTP URMA failed to create unbound Jetty");
    SetFinishCallback(MakeCallback(&UbApp::OnTaskCompleted, this), jetty);
    *jettyNum = m_jettyNum;
    m_ctpUnboundJettyBySourceEntityVl[key] = m_jettyNum;
    ++m_jettyNum;
    return jetty;
}

void
UbApp::SendTraffic(UbTrafficGen::RuntimeTask task)
{
    if (task.op == UbTrafficGen::RuntimeTaskOp::LOCAL_READY)
    {
        UbTrafficGen::Get()->MarkTaskCompleted(task.taskId);
        return;
    }
    if (task.op == UbTrafficGen::RuntimeTaskOp::COMPUTE)
    {
        NS_LOG_INFO("COMPUTE task starts, taskId: " << task.taskId
                                                    << ", duration(ns): " << task.dataSize);
        Simulator::Schedule(NanoSeconds(task.dataSize),
                            &UbApp::CompleteComputeTask,
                            this,
                            task.taskId);
        return;
    }
    if (task.priority == 0)
    {
        NS_LOG_DEBUG("Task uses the highest priority, not recommended.");
    }

    if (task.op == UbTrafficGen::RuntimeTaskOp::MEM_STORE ||
        task.op == UbTrafficGen::RuntimeTaskOp::MEM_LOAD)
    {
        // 内存语义发送
        UbMemOperationType type = UbMemOperationType::STORE;
        if (task.op == UbTrafficGen::RuntimeTaskOp::MEM_LOAD)
        {
            type = UbMemOperationType::LOAD;
        }
        auto ldstInstance = GetNode()->GetObject<UbLdstInstance>();
        SetFinishCallback(MakeCallback(&UbApp::OnMemTaskCompleted, this), ldstInstance);
        NS_LOG_INFO("MEM Task Starts, taskId: " << task.taskId);
        MemTaskStartsNotify(GetNode()->GetId(), task.taskId);
        std::vector<uint32_t> threadIds = {0, 1};
        UintegerValue threadNum;
        ldstInstance->GetAttribute("ThreadNum", threadNum);
        if (threadNum.Get() == 1)
        {
            threadIds.resize(1);
        }
        ldstInstance->HandleLdstTask(task.sourceNode,
                                     task.destNode,
                                     task.dataSize,
                                     task.taskId,
                                     task.priority,
                                     type,
                                     threadIds,
                                     0,
                                     task.srcPortId,
                                     task.hasSrcPortId,
                                     task.dstPortId,
                                     task.hasDstPortId);
    }
    else if (UbTrafficGen::IsUrmaTask(task.op))
    {
        if (m_transportMode == TransportMode::CTP)
        {
            SendCtpUrmaTraffic(task);
            return;
        }

        // URMA发送
        Ptr<UbFunction> ubFunc = GetNode()->GetObject<UbController>()->GetUbFunction();
        Ptr<UbTransaction> ubTa = GetNode()->GetObject<UbController>()->GetUbTransaction();
        bool jettyExist = ubFunc->IsJettyExists(m_jettyNum);
        if (jettyExist)
        {
            NS_LOG_ERROR("Jetty already exists");
            return;
        }
        ubFunc->CreateJetty(task.sourceNode,
                            task.destNode,
                            m_jettyNum,
                            UbJettyLifetime::TASK_SCOPED);
        vector<uint32_t> tpns = GetNode()->GetObject<UbController>()->GetTpConnManager()->GetTpns(
            m_getTpnRule,
            RoutingTypeUsesShortestPaths(m_routingType),
            m_multiPathEnable,
            task.sourceNode,
            task.destNode,
            task.hasSrcPortId ? task.srcPortId : UINT32_MAX,
            task.hasDstPortId ? task.dstPortId : UINT32_MAX,
            task.priority);
        bool bindRst = ubTa->JettyBindTp(task.sourceNode,
                                         task.destNode,
                                         m_jettyNum,
                                         m_multiPathEnable,
                                         tpns,
                                         task.taskId);
        if (bindRst)
        {
            Ptr<UbJetty> curr_jetty = ubFunc->GetJetty(m_jettyNum);
            SetFinishCallback(MakeCallback(&UbApp::OnTaskCompleted, this), curr_jetty);
            NS_LOG_INFO("WQE Starts, jettyNum: " << m_jettyNum << " taskId: " << task.taskId);
            NS_LOG_INFO("Src: " << task.sourceNode << " Dst: " << task.destNode);
            WqeTaskStartsNotify(GetNode()->GetId(), m_jettyNum, task.taskId);
            NS_LOG_INFO("[APPLICATION INFO] taskId: " << task.taskId << ",start time:"
                                                      << Simulator::Now().GetNanoSeconds() << "ns");
            UbTransactionOpcode type = UbTrafficGen::GetUrmaTaskOpcode(task.op);
            Ptr<UbWqe> wqe =
                ubFunc->CreateWqe(task.sourceNode, task.destNode, task.dataSize, task.taskId, type);
            if (task.atomicOptions)
            {
                wqe->SetAtomicOptions(*task.atomicOptions);
            }
            ubFunc->PushWqeToJetty(wqe, m_jettyNum);
        }
        m_jettyNum++; // m_jettyNum 在client里是唯一的，不重复的
    }
    else
    {
        NS_ASSERT_MSG(0, "UbTransactionOpcode Not Exist");
    }
}

void
UbApp::CompleteComputeTask(uint32_t taskId)
{
    UbTrafficGen::Get()->OnComputeTaskCompleted(taskId);
}

void
UbApp::OnTaskCompleted(uint32_t taskId, uint32_t jettyNum)
{
    Ptr<UbController> controller =
        GetNode() == nullptr ? nullptr : GetNode()->GetObject<UbController>();
    const Time delay = controller == nullptr
                           ? NanoSeconds(0)
                           : controller->GetInitiatorProtocolCompletionVisibilityDelay();
    if (delay.IsStrictlyPositive())
    {
        const auto key = std::make_pair(taskId, jettyNum);
        auto it = m_completionVisibilityEvents.find(key);
        if (it != m_completionVisibilityEvents.end())
        {
            Simulator::Cancel(it->second);
        }
        m_completionVisibilityEvents[key] =
            Simulator::Schedule(delay, &UbApp::OnTaskCompletedVisible, this, taskId, jettyNum);
        return;
    }
    OnTaskCompletedVisible(taskId, jettyNum);
}

void
UbApp::OnTaskCompletedVisible(uint32_t taskId, uint32_t jettyNum)
{
    m_completionVisibilityEvents.erase(std::make_pair(taskId, jettyNum));
    Ptr<Node> node = GetNode();
    if (node == nullptr)
    {
        UbTrafficGen::Get()->OnTaskCompleted(taskId);
        return;
    }
    NS_LOG_FUNCTION(this << taskId);
    NS_LOG_INFO("WQE Completes, jettyNum: " << jettyNum << " taskId: " << taskId);
    WqeTaskCompletesNotify(node->GetId(), jettyNum, taskId);
    NS_LOG_INFO("[APPLICATION INFO] taskId: " << taskId << ",finish time:"
                                              << Simulator::Now().GetNanoSeconds() << "ns");
    if (m_transportMode == TransportMode::RTP)
    {
        Ptr<UbController> controller = node->GetObject<UbController>();
        if (controller != nullptr && !controller->IsDisposing() &&
            controller->GetUbFunction() != nullptr)
        {
            Ptr<UbFunction> function = controller->GetUbFunction();
            UbJettyLifetime lifetime = UbJettyLifetime::REUSABLE;
            const bool destroyTaskScopedJetty = m_enableTaskScopedJettyDestroy &&
                                                function->TryGetJettyLifetime(jettyNum, lifetime) &&
                                                lifetime == UbJettyLifetime::TASK_SCOPED;
            if (destroyTaskScopedJetty || m_enablePlannedTpConnectionDestroy)
            {
                UbTrafficGen::Get()->OnTaskCompleted(taskId,
                                                     this,
                                                     jettyNum,
                                                     destroyTaskScopedJetty,
                                                     m_enablePlannedTpConnectionDestroy);
                return;
            }
        }
    }
    UbTrafficGen::Get()->OnTaskCompleted(taskId);
}

void
UbApp::CleanupCompletedTask(uint32_t taskId,
                            uint32_t jettyNum,
                            bool destroyTaskScopedJetty,
                            bool releasePlannedTpBinding,
                            const std::vector<UbLifecycleResourceIdentity>& plannedTpConnections)
{
    if (m_transportMode == TransportMode::RTP)
    {
        Ptr<UbTransaction> transaction = GetNode()->GetObject<UbController>()->GetUbTransaction();
        if (releasePlannedTpBinding)
        {
            transaction->ReleaseCompletedJettyTransportBinding(jettyNum,
                                                               !plannedTpConnections.empty());
        }

        Ptr<UbFunction> function = GetNode()->GetObject<UbController>()->GetUbFunction();
        UbJettyLifetime lifetime = UbJettyLifetime::REUSABLE;
        if (destroyTaskScopedJetty && function->TryGetJettyLifetime(jettyNum, lifetime) &&
            lifetime == UbJettyLifetime::TASK_SCOPED)
        {
            const UbDestroyStatus status =
                function->DestroyJetty(jettyNum, UbDestroyReason::PROTOCOL_COMPLETE);
            NS_ABORT_MSG_IF(status == UbDestroyStatus::REJECTED,
                            "Task-scoped Jetty cleanup was rejected for task " << taskId);
        }

        Ptr<TpConnectionManager> manager = GetNode()->GetObject<UbController>()->GetTpConnManager();
        for (const auto& identity : plannedTpConnections)
        {
            const UbDestroyStatus status =
                manager->DestroyConnection(identity, UbDestroyReason::PROTOCOL_COMPLETE);
            NS_ABORT_MSG_IF(status == UbDestroyStatus::REJECTED,
                            "Planned TP connection cleanup rejected a stale identity for task "
                                << taskId);
        }
    }
}

void
UbApp::OnTestTaskCompleted(uint32_t taskId, uint32_t jettyNum)
{
    NS_LOG_FUNCTION(this << taskId);
    NS_LOG_INFO("WQE Completes, jettyNum:" << jettyNum << " taskId:" << taskId);
    WqeTaskCompletesNotify(GetNode()->GetId(), jettyNum, taskId);
    NS_LOG_INFO("[APPLICATION INFO] taskId:" << taskId << ",finish time:"
                                             << Simulator::Now().GetNanoSeconds() << "ns");
    Ptr<UbFunction> ubFunc = GetNode()->GetObject<UbController>()->GetUbFunction();
    UbTrafficGen::Get()->OnTaskCompleted(taskId);
}

void
UbApp::OnMemTaskCompleted(uint32_t taskId)
{
    NS_LOG_FUNCTION(this << taskId);
    NS_LOG_INFO("MEM Task Completes, taskId: " << taskId);
    MemTaskCompletesNotify(GetNode()->GetId(), taskId);
    NS_LOG_INFO("[APPLICATION INFO] taskId: " << taskId << ",finish time:"
                                              << Simulator::Now().GetNanoSeconds() << "ns");
    UbTrafficGen::Get()->OnTaskCompleted(taskId);
}

void
UbApp::MemTaskStartsNotify(uint32_t nodeId, uint32_t taskId)
{
    m_traceMemTaskStartsNotify(nodeId, taskId);
}

void
UbApp::MemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId)
{
    m_traceMemTaskCompletesNotify(nodeId, taskId);
}

void
UbApp::ComputeTaskQueuedNotify(uint32_t nodeId, uint32_t taskId)
{
    m_traceComputeTaskQueuedNotify(nodeId, taskId);
}

void
UbApp::ComputeTaskStartsNotify(uint32_t nodeId, uint32_t taskId)
{
    m_traceComputeTaskStartsNotify(nodeId, taskId);
}

void
UbApp::ComputeTaskCompletesNotify(uint32_t nodeId, uint32_t taskId)
{
    m_traceComputeTaskCompletesNotify(nodeId, taskId);
}

void
UbApp::WqeTaskStartsNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId)
{
    m_traceWqeTaskStartsNotify(nodeId, jettyNum, taskId);
}

void
UbApp::WqeTaskCompletesNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId)
{
    m_traceWqeTaskCompletesNotify(nodeId, jettyNum, taskId);
}

} // namespace ns3
