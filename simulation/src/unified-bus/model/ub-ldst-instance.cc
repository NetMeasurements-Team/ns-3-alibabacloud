// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-ldst-instance.h"
#include "ns3/ub-ldst-thread.h"

namespace ns3 {
NS_LOG_COMPONENT_DEFINE("UbLdstInstance");

NS_OBJECT_ENSURE_REGISTERED(UbLdstInstance);
TypeId UbLdstInstance::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbLdstInstance")
                            .SetParent<Object>()
                            .SetGroupName("UnifiedBus")
                            .AddAttribute("ThreadNum",
                                          "Number of LDST worker threads.",
                                          UintegerValue(48),
                                          MakeUintegerAccessor(&UbLdstInstance::m_threadNum),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("QueuePriority",
                                          "Queue (VOQ) priority for packets emitted by this thread.",
                                          UintegerValue(1),
                                          MakeUintegerAccessor(&UbLdstInstance::m_queuePriority),
                                          MakeUintegerChecker<uint32_t>())
                            .AddTraceSource("MemTaskStartsNotify",
                                            "Emitted when a memory task starts on this thread.",
                                            MakeTraceSourceAccessor(&UbLdstInstance::m_traceMemTaskStartsNotify),
                                            "ns3::UbLdstInstance::MemTaskStartsNotify")
                            .AddTraceSource("LastPacketACKsNotify",
                                            "Emitted when the last packet of a task is ACKed.",
                                            MakeTraceSourceAccessor(&UbLdstInstance::m_traceLastPacketACKsNotify),
                                            "ns3::UbLdstInstance::LastPacketACKsNotify")
                            .AddTraceSource("MemTaskCompletesNotify",
                                            "Emitted when a memory task completes.",
                                            MakeTraceSourceAccessor(&UbLdstInstance::m_traceMemTaskCompletesNotify),
                                            "ns3::UbLdstInstance::MemTaskCompletesNotify")
                            .AddTraceSource("FirstPacketSendsNotify",
                                            "Emitted when the first packet of a memory task is sent.",
                                            MakeTraceSourceAccessor(&UbLdstInstance::m_traceFirstPacketSendsNotify),
                                            "ns3::UbLdstInstance::FirstPacketSendsNotify")
                            .AddTraceSource("LastPacketSendsNotify",
                                            "Emitted when the last packet of a memory task is sent.",
                                            MakeTraceSourceAccessor(&UbLdstInstance::m_traceLastPacketSendsNotify),
                                            "ns3::UbLdstInstance::LastPacketSendsNotify");

    return tid;
}

UbLdstInstance::UbLdstInstance()
{
}

UbLdstInstance::~UbLdstInstance()
{
}

void UbLdstInstance::Init(uint32_t nodeId)
{
    Ptr<Node> node = NodeList::GetNode(nodeId);
    Ptr<UbLifecycleCoordinator> coordinator = UbLifecycleCoordinator::GetOrCreate(node);
    m_lifecycleOwner =
        coordinator->OpenResource(UbLifecycleResourceKind::LDST_INSTANCE, 0);
    m_trackedActions.Init(coordinator);
    for (uint32_t threadId = 0; threadId < m_threadNum; threadId++) {
        auto ldstThread = CreateObject<UbLdstThread>();
        ldstThread->Init();
        ldstThread->SetNode(nodeId);
        ldstThread->SetThreadId(threadId);
        m_threads.push_back(ldstThread);
    }
}

void UbLdstInstance::DoDispose()
{
    if (m_disposed) {
        return;
    }
    m_disposed = true;

    Ptr<Node> node = GetObject<Node>();
    Ptr<UbLifecycleCoordinator> coordinator =
        node == nullptr ? nullptr : UbLifecycleCoordinator::GetOrCreate(node);
    if (coordinator != nullptr && m_lifecycleOwner.generation != 0) {
        const UbLifecycleResourceSnapshot snapshot = coordinator->GetSnapshot(m_lifecycleOwner);
        m_lastForcedStopResidue.activeTasks = m_taskToSegmentMap.size();
        m_lastForcedStopResidue.activeSegments = m_taskSegmentsMap.size();
        m_lastForcedStopResidue.trackedActions = snapshot.activeTrackedEventLeases;

        const std::vector<Callback<void>> cleanups =
            m_trackedActions.CancelPending(m_lifecycleOwner);
        for (const Callback<void>& cleanup : cleanups) {
            cleanup();
        }
    }

    for (const Ptr<UbLdstThread>& thread : m_threads) {
        const auto [waitingAcks, queuedSegments] = thread->CancelPendingWork();
        m_lastForcedStopResidue.waitingAcks += waitingAcks;
        m_lastForcedStopResidue.queuedSegments += queuedSegments;
    }
    if (coordinator != nullptr) {
        for (const auto& [taskSegmentId, record] : m_taskSegmentsMap) {
            (void)taskSegmentId;
            coordinator->RecordWorkTerminal(record.lifecycle, UbWorkOutcome::CANCELED);
            coordinator->RetireResource(record.lifecycle);
        }
    }
    if (m_lastForcedStopResidue.activeTasks != 0 ||
        m_lastForcedStopResidue.activeSegments != 0 ||
        m_lastForcedStopResidue.waitingAcks != 0 ||
        m_lastForcedStopResidue.queuedSegments != 0 ||
        m_lastForcedStopResidue.trackedActions != 0) {
        NS_LOG_WARN("LDST forced stop residue: tasks=" << m_lastForcedStopResidue.activeTasks
                    << " segments=" << m_lastForcedStopResidue.activeSegments
                    << " waitingAcks=" << m_lastForcedStopResidue.waitingAcks
                    << " queuedSegments=" << m_lastForcedStopResidue.queuedSegments
                    << " trackedActions=" << m_lastForcedStopResidue.trackedActions);
    }
    m_taskToSegmentMap.clear();
    m_taskSegmentsMap.clear();
    m_threads.clear();
    if (m_trackedActions.IsEmpty()) {
        m_trackedActions.Clear();
    }
    Object::DoDispose();
}

void UbLdstInstance::SetClientCallback(Callback<void, uint32_t> cb)
{
    FinishCallback = cb;
}

void
UbLdstInstance::HandleLdstTask(uint32_t src,
                               uint32_t dest,
                               uint32_t length,
                               uint32_t taskId,
                               uint32_t priority,
                               UbMemOperationType type,
                               const std::vector<uint32_t>& threadIds,
                               uint64_t address,
                               uint32_t srcPortId,
                               bool hasSrcPortId,
                               uint32_t dstPortId,
                               bool hasDstPortId)
{
    uint32_t threadsNum = threadIds.size();
    // 将数据均分下发给thread
    uint32_t partSize = length / threadsNum;

    FirstPacketSendsNotify(this->GetObject<Node>()->GetId(), taskId);
    MemTaskStartsNotify(this->GetObject<Node>()->GetId(), taskId);
    for (uint32_t i = 0; i < threadsNum; i++) {
        uint32_t segmentSize = partSize;
        if (i == threadsNum - 1) {
            segmentSize += length - partSize * threadsNum;
        }
        uint32_t threadId = threadIds[i];
        auto ldstThread = GetLdstThread(threadId);
        auto taskSegment = CreateObject<UbLdstTaskSegment>();
        taskSegment->SetSrc(src);
        taskSegment->SetDest(dest);
        if (hasSrcPortId)
        {
            taskSegment->SetSrcPortId(srcPortId);
        }
        if (hasDstPortId)
        {
            taskSegment->SetDstPortId(dstPortId);
        }
        taskSegment->SetPriority(static_cast<uint8_t>(priority)); 
        taskSegment->SetSize(segmentSize);
        taskSegment->SetTaskId(taskId);
        taskSegment->SetTaskSegmentId(m_currentTaskId);
        taskSegment->SetType(type);
        taskSegment->SetThreadId(threadId);
        Ptr<UbLifecycleCoordinator> coordinator =
            UbLifecycleCoordinator::GetOrCreate(GetObject<Node>());
        SegmentRecord record{
            taskSegment,
            coordinator->OpenResource(UbLifecycleResourceKind::LDST_SEGMENT, m_currentTaskId)};
        NS_ABORT_MSG_IF(!m_taskToSegmentMap[taskId].insert(m_currentTaskId).second,
                        "Duplicate LDST task segment ID");
        NS_ABORT_MSG_IF(!m_taskSegmentsMap.emplace(m_currentTaskId, record).second,
                        "Duplicate LDST active segment record");
        m_currentTaskId++;
        ScheduleNow(&UbLdstThread::PushTaskSegment, ldstThread, taskSegment);
    }
}

void UbLdstInstance::OnRecvAck(uint32_t taskSegmentId)
{
    ResolveAck(taskSegmentId);
}

bool UbLdstInstance::TryRecvAck(uint32_t taskSegmentId)
{
    if (m_taskSegmentsMap.find(taskSegmentId) == m_taskSegmentsMap.end())
    {
        return false;
    }
    ScheduleNow(&UbLdstInstance::OnRecvAck, this, taskSegmentId);
    return true;
}

bool UbLdstInstance::OnRecvAckForTest(uint32_t taskSegmentId)
{
    return ResolveAck(taskSegmentId);
}

bool UbLdstInstance::ResolveAck(uint32_t taskSegmentId)
{
    const auto segment = m_taskSegmentsMap.find(taskSegmentId);
    if (segment == m_taskSegmentsMap.end()) {
        NS_LOG_WARN("Ignoring unknown or stale LDST ACK for segment " << taskSegmentId);
        return false;
    }
    Ptr<UbLdstTaskSegment> taskSegment = segment->second.segment;
    uint32_t threadId = taskSegment->GetThreadId();
    auto ldstThread = GetLdstThread(threadId);
    ScheduleNow(&UbLdstThread::UpdateTask, ldstThread, taskSegment);
    return true;
}

void UbLdstInstance::OnTaskSegmentCompleted(Ptr<UbLdstTaskSegment> taskSegment)
{
    NS_ABORT_MSG_IF(taskSegment == nullptr, "LDST completion requires a task segment");
    const uint32_t taskId = taskSegment->GetTaskId();
    const uint32_t taskSegmentId = taskSegment->GetTaskSegmentId();
    auto segment = m_taskSegmentsMap.find(taskSegmentId);
    NS_ABORT_MSG_IF(segment == m_taskSegmentsMap.end() ||
                        segment->second.segment != taskSegment,
                    "LDST completion references an inactive segment");
    auto task = m_taskToSegmentMap.find(taskId);
    NS_ABORT_MSG_IF(task == m_taskToSegmentMap.end() ||
                        task->second.erase(taskSegmentId) != 1,
                    "LDST completion references an inactive task");

    Ptr<UbLifecycleCoordinator> coordinator =
        UbLifecycleCoordinator::GetOrCreate(GetObject<Node>());
    coordinator->RecordWorkTerminal(segment->second.lifecycle, UbWorkOutcome::COMPLETED);
    coordinator->RetireResource(segment->second.lifecycle);
    m_taskSegmentsMap.erase(segment);

    if (task->second.empty()) {
        m_taskToSegmentMap.erase(task);
        LastPacketACKsNotify(this->GetObject<Node>()->GetId(), taskId);
        MemTaskCompletesNotify(this->GetObject<Node>()->GetId(), taskId);
        FinishCallback(taskId);
    }
}

void UbLdstInstance::ScheduleNow(Callback<void> action)
{
    NS_ABORT_MSG_IF(action.IsNull(), "LDST tracked action cannot be null");
    if (m_disposed) {
        return;
    }
    NS_ABORT_MSG_IF(m_lifecycleOwner.generation == 0,
                    "LDST tracked action requires an initialized owner");
    const uint64_t token = m_trackedActions.Track(m_lifecycleOwner);
    EventId event = Simulator::ScheduleNow(&UbLdstInstance::RunTrackedAction,
                                           this,
                                           token,
                                           action);
    m_trackedActions.Attach(m_lifecycleOwner, token, event, Callback<void>());
}

void UbLdstInstance::RunTrackedAction(uint64_t token, Callback<void> action)
{
    if (!m_trackedActions.Begin(m_lifecycleOwner, token)) {
        return;
    }
    action();
    ResolveTrackedAction(token);
}

void
UbLdstInstance::ResolveTrackedAction(uint64_t token)
{
    m_trackedActions.Resolve(m_lifecycleOwner, token);
    if (m_disposed && m_trackedActions.IsEmpty()) {
        m_trackedActions.Clear();
    }
}

size_t UbLdstInstance::GetActiveTaskCount() const
{
    return m_taskToSegmentMap.size();
}

size_t UbLdstInstance::GetActiveSegmentCount() const
{
    return m_taskSegmentsMap.size();
}

Ptr<UbLdstTaskSegment> UbLdstInstance::GetTaskSegmentForTest(uint32_t taskSegmentId) const
{
    const auto segment = m_taskSegmentsMap.find(taskSegmentId);
    return segment == m_taskSegmentsMap.end() ? nullptr : segment->second.segment;
}

UbLifecycleResourceSnapshot UbLdstInstance::GetLifecycleSnapshotForTest() const
{
    NS_ABORT_MSG_IF(m_lifecycleOwner.generation == 0,
                    "LDST lifecycle snapshot requires an initialized owner");
    return UbLifecycleCoordinator::GetOrCreate(GetObject<Node>())
        ->GetSnapshot(m_lifecycleOwner);
}

const UbLdstForcedStopResidue& UbLdstInstance::GetLastForcedStopResidueForTest() const
{
    return m_lastForcedStopResidue;
}

Ptr<UbLdstThread> UbLdstInstance::GetLdstThread(uint32_t threadId)
{
    if (threadId >= m_threads.size()) {
        NS_ASSERT_MSG(0, "Invalid threadId! Cannot Get Ldst Thread.");
    }
    return m_threads[threadId];
}

void UbLdstInstance::LastPacketACKsNotify(uint32_t nodeId, uint32_t taskId)
{
    m_traceLastPacketACKsNotify(nodeId, taskId);
}

void UbLdstInstance::MemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId)
{
    m_traceMemTaskCompletesNotify(nodeId, taskId);
}

void UbLdstInstance::MemTaskStartsNotify(uint32_t nodeId, uint32_t memTaskId)
{
    m_traceMemTaskStartsNotify(nodeId, memTaskId);
}

void UbLdstInstance::FirstPacketSendsNotify(uint32_t nodeId, uint32_t memTaskId)
{
    m_traceFirstPacketSendsNotify(nodeId, memTaskId);
}
 
void UbLdstInstance::LastPacketSendsNotify(uint32_t nodeId, uint32_t memTaskId)
{
    m_traceLastPacketSendsNotify(nodeId, memTaskId);
}
}
