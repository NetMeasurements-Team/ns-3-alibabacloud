// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-switch-allocator.h"

#include "ns3/ub-routing-process.h"
#include "ns3/ub-transport.h"
#include "ub-queue-manager.h"

#include "ns3/ub-controller.h"
#include "ns3/hash.h"
#include "ns3/ub-port.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-tag.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <optional>

namespace ns3 {

namespace {

uint32_t
ComputeInitialIngressPhase(uint32_t nodeId, uint32_t outPortId, size_t qSize)
{
    if (qSize == 0) {
        return 0;
    }
    return static_cast<uint32_t>((nodeId + outPortId) % qSize);
}

uint32_t
ComputeSeededIngressPhase(uint32_t seed,
                          uint32_t nodeId,
                          uint32_t outPortId,
                          uint32_t priority,
                          size_t qSize)
{
    std::ostringstream key;
    key << "ALLOCATOR_RR_PHASE:" << seed << ':' << nodeId << ':' << outPortId << ':'
        << priority << ':' << qSize;
    return qSize == 0 ? 0 : static_cast<uint32_t>(Hash64(key.str()) % qSize);
}

} // namespace

NS_OBJECT_ENSURE_REGISTERED(UbSwitchAllocator);
NS_OBJECT_ENSURE_REGISTERED(UbDwrrAllocator);
NS_LOG_COMPONENT_DEFINE("UbSwitchAllocator");

namespace {

UbFlowControlEventContext
MakeAllocatorFlowControlEventContext(Ptr<Packet> packet,
                                     Ptr<UbIngressQueue> ingressQueue,
                                     uint32_t inPortId,
                                     uint32_t outPortId,
                                     uint32_t priority)
{
    // Capture the switch-local marker before the successful service transaction. Consume it
    // only after the switch has used it for queue accounting; neither step changes wire bytes.
    UbRecoveredCreditTag recoveredCreditTag;
    const bool upstreamCreditAlreadyReturned = packet->PeekPacketTag(recoveredCreditTag);
    return {
        .packet = packet,
        .ingressQueue = ingressQueue,
        .inPortId = inPortId,
        .outPortId = outPortId,
        .priority = priority,
        .upstreamCreditAlreadyReturned = upstreamCreditAlreadyReturned,
    };
}

} // namespace

TypeId UbSwitchAllocator::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbSwitchAllocator")
        .SetParent<Object>()
        .AddConstructor<UbSwitchAllocator>()
        .AddAttribute("AllocationTime",
                      "Latency of the switch allocation pipeline per scheduling round.",
                      TimeValue(NanoSeconds(5)),
                      MakeTimeAccessor(&UbSwitchAllocator::m_allocationTime),
                      MakeTimeChecker())
        .AddAttribute("EnableDirectAllocationCompletion",
                      "Synchronously start an idle egress port after allocation completes.",
                      BooleanValue(true),
                      MakeBooleanAccessor(&UbSwitchAllocator::m_enableDirectAllocationCompletion),
                      MakeBooleanChecker());
    return tid;
}
UbSwitchAllocator::UbSwitchAllocator()
{
}

UbSwitchAllocator::~UbSwitchAllocator()
{
}

void UbSwitchAllocator::DoDispose()
{
    if (m_disposed) {
        return;
    }
    m_disposed = true;
    m_forcedEventResidue = GetPendingEventCountForTest();
    for (auto& state : m_portEventState) {
        if (state.activeIngressQueue != nullptr) {
            ++m_forcedEventResidue;
        }
        if (state.allocationPending) {
            state.allocation.Cancel();
        }
        for (auto& entry : state.notifications) {
            entry.second.Cancel();
        }
        state = PortEventState{};
    }
    if (m_deadlockPending) {
        m_deadlockEvent.Cancel();
    }
    m_deadlockPending = false;
    std::fill(m_running.begin(), m_running.end(), false);
    std::fill(m_runAgain.begin(), m_runAgain.end(), false);
    if (m_runningAllocationCallbacks == 0) {
        FinalizeDisposeStorage();
    } else {
        m_deferredDisposeCleanup = true;
    }
    Object::DoDispose();
}

void UbSwitchAllocator::FinalizeDisposeStorage()
{
    for (auto& byPriority : m_ingressSources)
    {
        for (auto& queues : byPriority)
        {
            for (const auto& queue : queues)
            {
                UnbindIngressSummary(queue);
            }
        }
    }
    m_portEventState.clear();
    m_ingressSources.clear();
    m_running.clear();
    m_runAgain.clear();
    m_nonEmptyIngressCount.clear();
    m_deferredDisposeCleanup = false;
}

void UbSwitchAllocator::InitializePortEventState(uint32_t portsNum)
{
    m_portEventState.resize(portsNum);
}

void UbSwitchAllocator::ScheduleAllocation(Ptr<UbPort> outPort)
{
    if (m_disposed) {
        return;
    }
    const uint32_t outPortId = outPort->GetIfIndex();
    if (!GetEgressStatus(outPortId)) {
        return;
    }
    NS_ASSERT_MSG(outPortId < m_portEventState.size(),
                  "Allocation requires initialized output-port ownership state");
    auto& state = m_portEventState[outPortId];
    NS_ASSERT_MSG(!state.allocationPending,
                  "Only one allocation callback may be pending for an output port");
    state.allocationPending = true;
    ++state.schedulingWorkCount;
    state.allocation = Simulator::Schedule(m_allocationTime,
                                           &UbSwitchAllocator::RunAllocation,
                                           this,
                                           outPort);
}

void UbSwitchAllocator::SchedulePortNotification(Ptr<UbPort> outPort)
{
    if (m_disposed) {
        return;
    }
    const uint32_t outPortId = outPort->GetIfIndex();
    NS_ASSERT_MSG(outPortId < m_portEventState.size(),
                  "Port notification requires initialized output-port ownership state");
    auto& state = m_portEventState[outPortId];
    NS_ABORT_MSG_IF(m_nextEventToken == std::numeric_limits<uint64_t>::max(),
                    "Allocator event token exhausted");
    const uint64_t token = ++m_nextEventToken;
    EventId event = Simulator::ScheduleNow(&UbSwitchAllocator::RunPortNotification,
                                           this,
                                           outPortId,
                                           token,
                                           outPort);
    const bool inserted = state.notifications.emplace(token, event).second;
    NS_ABORT_MSG_IF(!inserted, "Allocator notification event token collision");
}

void UbSwitchAllocator::ScheduleDeadlockCheck()
{
    if (m_disposed) {
        return;
    }
    NS_ASSERT_MSG(!m_deadlockPending, "Only one deadlock callback may be pending");
    m_deadlockPending = true;
    m_deadlockEvent = Simulator::Schedule(MilliSeconds(10),
                                          &UbSwitchAllocator::RunDeadlockCheck,
                                          this);
}

void UbSwitchAllocator::RunAllocation(Ptr<UbPort> outPort)
{
    const uint32_t outPortId = outPort->GetIfIndex();
    NS_ASSERT_MSG(outPortId < m_portEventState.size(),
                  "Allocation callback requires retained output-port ownership state");
    auto& state = m_portEventState[outPortId];
    NS_ASSERT(state.allocationPending && state.schedulingWorkCount > 0);
    state.allocationPending = false;
    --state.schedulingWorkCount;
    ++m_runningAllocationCallbacks;
    AllocateNextPacket(outPort);
    NS_ASSERT(m_runningAllocationCallbacks > 0);
    --m_runningAllocationCallbacks;
    if (m_deferredDisposeCleanup && m_runningAllocationCallbacks == 0) {
        FinalizeDisposeStorage();
    }
}

void UbSwitchAllocator::RunPortNotification(uint32_t outPortId,
                                            uint64_t token,
                                            Ptr<UbPort> outPort)
{
    NS_ASSERT_MSG(outPortId < m_portEventState.size(),
                  "Port notification callback requires retained output-port ownership state");
    auto& state = m_portEventState[outPortId];
    const size_t erased = state.notifications.erase(token);
    NS_ASSERT(erased == 1);
    NotifyAllocationFinish(outPort);
}

void UbSwitchAllocator::RunDeadlockCheck()
{
    NS_ASSERT(m_deadlockPending);
    m_deadlockPending = false;
    CheckDeadlock();
}

uint32_t
UbSwitchAllocator::GetPendingEventCountForTest() const
{
    uint32_t count = m_deadlockPending ? 1u : 0u;
    for (const auto& state : m_portEventState) {
        count += state.allocationPending ? 1u : 0u;
        count += static_cast<uint32_t>(state.notifications.size());
    }
    return count;
}

bool
UbSwitchAllocator::HasPendingOrActiveWork(Ptr<UbIngressQueue> ingressQueue) const
{
    if (ingressQueue == nullptr) {
        return false;
    }
    const uint32_t outPortId = ingressQueue->GetOutPortId();
    if (outPortId >= m_portEventState.size()) {
        return false;
    }
    const auto& state = m_portEventState[outPortId];
    return state.activeIngressQueue == ingressQueue || state.schedulingWorkCount != 0;
}

void
UbSwitchAllocator::SetActiveIngressQueue(Ptr<UbIngressQueue> ingressQueue)
{
    if (ingressQueue != nullptr) {
        const uint32_t outPortId = ingressQueue->GetOutPortId();
        NS_ASSERT_MSG(outPortId < m_portEventState.size(),
                      "Active ingress queue requires initialized output-port ownership state");
        m_portEventState[outPortId].activeIngressQueue = ingressQueue;
    }
}

Ptr<UbIngressQueue>
UbSwitchAllocator::GetActiveIngressQueue(uint32_t outPortId) const
{
    NS_ASSERT_MSG(outPortId < m_portEventState.size(),
                  "Active ingress lookup requires initialized output-port ownership state");
    return m_portEventState[outPortId].activeIngressQueue;
}

void
UbSwitchAllocator::ClearActiveIngressQueue(uint32_t outPortId)
{
    NS_ASSERT_MSG(outPortId < m_portEventState.size(),
                  "Active ingress clear requires initialized output-port ownership state");
    m_portEventState[outPortId].activeIngressQueue = nullptr;
}

void
UbSwitchAllocator::NotifyAllocationFinish(Ptr<UbPort> outPort)
{
    outPort->NotifyAllocationFinish();
}

void UbSwitchAllocator::TriggerAllocator(Ptr<UbPort> outPort)
{
    std::string typeName = GetInstanceTypeId().GetName();
    NS_LOG_DEBUG("[" << typeName << " TriggerAllocator] portId: " << outPort->GetIfIndex());

    auto outPortId = outPort->GetIfIndex();

    if (m_disposed || outPortId >= m_running.size()) {
         NS_LOG_WARN("Port ID out of range in Allocator");
         return;
    }

    if (!GetEgressStatus(outPortId)) {
        // Detected failed output: no grant may commit to it.
        NS_LOG_DEBUG("[" << typeName << " TriggerAllocator] output suspended: " << outPortId);
        return;
    }

    if (m_running[outPortId]) {
        m_runAgain[outPortId] = true;
        NS_LOG_DEBUG("[" << typeName << " TriggerAllocator] Allocator is running, will run again.");
        return;
    }
    m_running[outPortId] = true;
    m_runAgain[outPortId] = false;
    SetActiveIngressQueue(PickNextIngressQueue(outPort));
    if (m_disposed) {
        m_running[outPortId] = false;
        return;
    }
    ScheduleAllocation(outPort);
}

void
UbSwitchAllocator::ScheduleTriggerAllocator(Ptr<UbPort> outPort)
{
    TriggerAllocator(outPort);
}

void UbSwitchAllocator::AllocateNextPacket(Ptr<UbPort> outPort)
{
}

Ptr<UbIngressQueue>
UbSwitchAllocator::PickNextIngressQueue(Ptr<UbPort> outPort)
{
    return nullptr;
}

void UbSwitchAllocator::Init()
{
    ScheduleDeadlockCheck();
}

void UbSwitchAllocator::RegisterUbIngressQueue(Ptr<UbIngressQueue> ingressQueue, uint32_t outPort, uint32_t priority)
{
    NS_ABORT_MSG_IF(ingressQueue == nullptr, "Cannot register a null ingress queue");
    NS_ABORT_MSG_IF(outPort >= m_ingressSources.size() ||
                        priority >= m_ingressSources[outPort].size(),
                    "Invalid ingress registration coordinates");
    NS_ABORT_MSG_IF(ingressQueue->GetOutPortId() != outPort ||
                        ingressQueue->GetIngressPriority() != priority,
                    "Ingress registration coordinates must match the queue");
    auto& queues = m_ingressSources[outPort][priority];
    if (std::find(queues.begin(), queues.end(), ingressQueue) != queues.end())
    {
        return;
    }
    NS_ABORT_MSG_IF(ingressQueue->m_allocatorOwner != nullptr,
                    "Ingress queue is already registered with an allocator");
    if (ingressQueue->GetIngressQueueType() != IngressQueueType::VOQ) {
        queues.push_back(ingressQueue);
        BindIngressSummary(ingressQueue);
        return;
    }

    const uint32_t inPort = ingressQueue->GetInPortId();
    auto insertAt = queues.begin();
    for (; insertAt != queues.end(); ++insertAt) {
        Ptr<UbIngressQueue> existing = *insertAt;
        if (existing->GetIngressQueueType() != IngressQueueType::VOQ ||
            existing->GetInPortId() > inPort) {
            break;
        }
    }
    queues.insert(insertAt, ingressQueue);
    BindIngressSummary(ingressQueue);
}

void
UbSwitchAllocator::BindIngressSummary(Ptr<UbIngressQueue> ingressQueue)
{
    ingressQueue->BindAllocatorSummary(this);
}

void
UbSwitchAllocator::UnbindIngressSummary(Ptr<UbIngressQueue> ingressQueue)
{
    if (ingressQueue != nullptr)
    {
        ingressQueue->UnbindAllocatorSummary(this);
    }
}

void
UbSwitchAllocator::UpdateIngressSummary(UbIngressQueue* ingressQueue, bool nonEmpty)
{
    NS_ASSERT_MSG(ingressQueue != nullptr, "Cannot update summary for a null ingress queue");
    NS_ABORT_MSG_IF(ingressQueue->m_allocatorOwner != this,
                    "Ingress summary updates must come from a bound queue");
    const uint32_t outPortId = ingressQueue->GetOutPortId();
    NS_ABORT_MSG_IF(outPortId >= m_nonEmptyIngressCount.size(),
                    "Ingress summary requires initialized output-port state");
    if (nonEmpty)
    {
        ++m_nonEmptyIngressCount[outPortId];
        return;
    }
    NS_ABORT_MSG_IF(m_nonEmptyIngressCount[outPortId] == 0,
                    "Ingress summary count cannot underflow");
    --m_nonEmptyIngressCount[outPortId];
}

bool
UbSwitchAllocator::HasNonEmptyIngress(uint32_t outPortId) const
{
    return outPortId < m_nonEmptyIngressCount.size() && m_nonEmptyIngressCount[outPortId] != 0;
}

uint32_t
UbSwitchAllocator::GetIngressQueueSlotCount(uint32_t outPort, uint32_t priority) const
{
    const auto& queues = m_ingressSources[outPort][priority];
    uint32_t nonVoqCount = 0;
    for (const auto& queue : queues) {
        if (queue->GetIngressQueueType() != IngressQueueType::VOQ) {
            ++nonVoqCount;
        }
    }
    return m_voqIngressSlotCount + nonVoqCount;
}

std::optional<UbSwitchAllocator::IngressCandidate>
UbSwitchAllocator::FindNextEligibleIngressQueue(uint32_t outPort,
                                                uint32_t priority,
                                                uint32_t startIngressQueueSlot,
                                                Ptr<UbPort> egressPort) const
{
    const auto& queues = m_ingressSources[outPort][priority];
    const uint32_t ingressQueueSlotCount = GetIngressQueueSlotCount(outPort, priority);
    if (ingressQueueSlotCount == 0 || queues.empty()) {
        return std::nullopt;
    }

    std::optional<IngressCandidate> bestAtOrAfterStart;
    std::optional<IngressCandidate> bestWrapped;
    uint32_t nonVoqQueueOrdinal = 0;

    for (const auto& queue : queues) {
        uint32_t ingressQueueSlot;
        if (queue->GetIngressQueueType() == IngressQueueType::VOQ) {
            ingressQueueSlot = queue->GetInPortId();
        } else {
            ingressQueueSlot = m_voqIngressSlotCount + nonVoqQueueOrdinal;
            ++nonVoqQueueOrdinal;
        }

        if (ingressQueueSlot >= ingressQueueSlotCount || queue->IsEmpty() || queue->IsLimited()) {
            continue;
        }
        const bool fcLimited = egressPort->GetFlowControl()->IsFcLimited(queue);
        if (m_disposed) {
            return std::nullopt;
        }
        if (fcLimited) {
            continue;
        }

        IngressCandidate candidate{queue, ingressQueueSlot};
        if (ingressQueueSlot >= startIngressQueueSlot) {
            if (!bestAtOrAfterStart.has_value() ||
                ingressQueueSlot < bestAtOrAfterStart->ingressQueueSlot) {
                bestAtOrAfterStart = candidate;
            }
        } else if (!bestWrapped.has_value() ||
                   ingressQueueSlot < bestWrapped->ingressQueueSlot) {
            bestWrapped = candidate;
        }
    }

    if (bestAtOrAfterStart.has_value()) {
        return bestAtOrAfterStart;
    }
    return bestWrapped;
}

void UbSwitchAllocator::CheckDeadlock()
{
    Time now = Simulator::Now();
    Time threshold = MilliSeconds(10);

    for (uint32_t outPort = 0; outPort < m_ingressSources.size(); ++outPort) {
        for (uint32_t pri = 0; pri < m_ingressSources[outPort].size(); ++pri) {
            for (const auto& queue : m_ingressSources[outPort][pri]) {
                if (queue && !queue->IsEmpty()) {
                    if (now - queue->GetHeadArrivalTime() > threshold) {
                        std::stringstream ss;
                        ss << "Potential Deadlock in Node " << m_nodeId
                           << " OutPort:" << outPort << " Pri:" << pri;

                        if (queue->GetIngressQueueType() == IngressQueueType::VOQ) {
                            ss << " QueueType:VOQ InPort:" << queue->GetInPortId();
                        } else if (queue->GetIngressQueueType() == IngressQueueType::TP &&
                                   !queue->IsGeneratedDataPacket()) {
                            auto tp = DynamicCast<UbTransportChannel>(queue);
                            if (tp) {
                                ss << " QueueType:TP TPN:" << tp->GetTpn();
                            } else {
                                ss << " QueueType:TP (Cast Failed)";
                            }
                        } else if (queue->GetIngressQueueType() == IngressQueueType::CTP) {
                            ss << " QueueType:CTP";
                        } else {
                            ss << " QueueType:Unknown(" << (int)queue->GetIngressQueueType() << ")";
                        }

                        ss << " Head packet stuck for " << (now - queue->GetHeadArrivalTime()).GetMilliSeconds() << " ms";
                        NS_LOG_WARN(ss.str());
                    }
                }
            }
        }
    }
    ScheduleDeadlockCheck();
}

void UbSwitchAllocator::UnregisterUbIngressQueue(Ptr<UbIngressQueue> ingressQueue, uint32_t outPort, uint32_t priority)
{
    if (outPort >= m_ingressSources.size() || priority >= m_ingressSources[outPort].size())
    {
        return;
    }
    if (ingressQueue == nullptr || ingressQueue->GetOutPortId() != outPort ||
        ingressQueue->GetIngressPriority() != priority)
    {
        return;
    }
    auto& queues = m_ingressSources[outPort][priority];
    if (std::find(queues.begin(), queues.end(), ingressQueue) == queues.end())
    {
        return;
    }
    queues.erase(std::remove(queues.begin(), queues.end(), ingressQueue), queues.end());
    UnbindIngressSummary(ingressQueue);
}

void UbSwitchAllocator::RegisterEgressStauts(uint32_t portsNum)
{
    m_egressStatus.resize(portsNum, true);
}

void UbSwitchAllocator::SetEgressStatus(uint32_t portId, bool status)
{
    if (portId >= m_egressStatus.size())
    {
        m_egressStatus.resize(portId + 1, true);
    }
    m_egressStatus[portId] = status;
}

bool UbSwitchAllocator::GetEgressStatus(uint32_t portId)
{
    // An unregistered port is a healthy output: the vector is only sized by the derived Init().
    return portId >= m_egressStatus.size() || m_egressStatus[portId];
}

void
UbSwitchAllocator::SuspendOutput(uint32_t portId)
{
    if (portId >= m_egressStatus.size())
    {
        m_egressStatus.resize(portId + 1, true);
    }
    m_egressStatus[portId] = false;

    auto& state = m_portEventState[portId];
    if (state.allocationPending)
    {
        state.allocation.Cancel();
        state.allocationPending = false;
        NS_ASSERT(state.schedulingWorkCount > 0);
        --state.schedulingWorkCount;
    }
    for (auto& [token, event] : state.notifications)
    {
        (void)token;
        event.Cancel();
    }
    state.notifications.clear();
    NS_LOG_DEBUG("[UbSwitchAllocator] suspended output port " << portId);
}

bool
UbSwitchAllocator::IsOutputSuspended(uint32_t portId) const
{
    return portId < m_egressStatus.size() && !m_egressStatus[portId];
}

/*-----------------------------------------UbRoundRobinAllocator----------------------------------------------*/
TypeId UbRoundRobinAllocator::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbRoundRobinAllocator")
        .SetParent<UbSwitchAllocator>()
        .AddConstructor<UbRoundRobinAllocator>();
    return tid;
}

void UbRoundRobinAllocator::Init()
{
    UbSwitchAllocator::Init();
    auto node = NodeList::GetNode(m_nodeId);
    uint32_t portsNum = node->GetNDevices();
    InitializePortEventState(portsNum);
    auto vlNum = node->GetObject<UbSwitch>()->GetVLNum();
    m_rrIdx.resize(portsNum);
    m_rrPhaseSeeded.resize(portsNum);
    for (auto &v: m_rrIdx) {
        v.resize(vlNum, 0);
    }
    for (auto &v: m_rrPhaseSeeded) {
        v.resize(vlNum, false);
    }
    m_selectedIngressQueueSlot.assign(portsNum, 0);
    RegisterEgressStauts(portsNum);
    m_ingressSources.resize(portsNum);
    m_running.assign(portsNum, false);
    m_runAgain.assign(portsNum, false);
    m_nonEmptyIngressCount.assign(portsNum, 0);
    m_voqIngressSlotCount = portsNum;
    for (auto &i : m_ingressSources) {
        i.resize(vlNum);
    }
}

void UbRoundRobinAllocator::AllocateNextPacket(Ptr<UbPort> outPort)
{
    // 轮询调度
    NS_LOG_DEBUG("[UbRoundRobinAllocator AllocateNextPacket] portId: " << outPort->GetIfIndex());
    auto outPortId = outPort->GetIfIndex();
    const bool allocatorOwnedEpisode = m_running[outPortId];
    auto ingressQueue = GetActiveIngressQueue(outPortId);
    if (!allocatorOwnedEpisode && ingressQueue == nullptr) {
        ingressQueue = SelectNextIngressQueue(outPort);
        SetActiveIngressQueue(ingressQueue);
    }
    if (m_disposed) {
        m_running[outPortId] = false;
        return;
    }
    if (!GetEgressStatus(outPortId)) {
        // Detected failed output: keep the packet in its ingress queue for recovery.
        ClearActiveIngressQueue(outPortId);
        m_running[outPortId] = false;
        return;
    }
    SetActiveIngressQueue(ingressQueue);
    // 调度得到的ingressqueue加入egressqueue
    if (ingressQueue != nullptr) {
        const uint32_t nextPacketBytes = ingressQueue->GetNextPacketSize();
        if (!outPort->GetUbQueue()->CanEnqueue(nextPacketBytes)) {
            NS_LOG_DEBUG("[UbRoundRobinAllocator AllocateNextPacket] egress queue full, keep packet in ingress queue"
                         << " outPortId=" << outPortId
                         << " bytes=" << nextPacketBytes);
            const uint32_t priority = ingressQueue->GetIngressPriority();
            m_rrIdx[outPortId][priority] = m_selectedIngressQueueSlot[outPortId];
            ClearActiveIngressQueue(outPortId);
            m_running[outPortId] = false;
            return;
        }

        auto packet = ingressQueue->GetNextPacket();
        auto inPortId = ingressQueue->GetInPortId();
        auto priority = ingressQueue->GetIngressPriority();
        auto packetEntry = std::make_tuple(inPortId, priority, packet);
        auto context = MakeAllocatorFlowControlEventContext(packet,
                                                            ingressQueue,
                                                            inPortId,
                                                            outPortId,
                                                            priority);
        const bool enqueueOk = outPort->EnqueueToEgress(packetEntry);
        NS_ASSERT_MSG(enqueueOk,
                      "allocator pre-check promised egress queue capacity, but DoEnqueue still failed");
        outPort->GetFlowControl()->OnEgressEnqueued(context);

        // Packet moved from VOQ to EgressQueue, notify Switch to update buffer statistics
        if (ingressQueue->GetIngressQueueType() != IngressQueueType::TP &&
            ingressQueue->GetIngressQueueType() != IngressQueueType::CTP &&
            !ingressQueue->IsGeneratedDataPacket()) {
            // Forwarded packet (not locally generated)
            auto node = NodeList::GetNode(m_nodeId);
            auto inPort = DynamicCast<UbPort>(node->GetDevice(inPortId));
            node->GetObject<UbSwitch>()->NotifySwitchDequeue(inPortId, outPortId, priority, packet);
            inPort->GetFlowControl()->OnIngressReleased(context);
            if (m_disposed) {
                ClearActiveIngressQueue(outPortId);
                m_running[outPortId] = false;
                return;
            }
        }
        if (m_disposed) {
            ClearActiveIngressQueue(outPortId);
            m_running[outPortId] = false;
            return;
        }
    }
    ClearActiveIngressQueue(outPortId);
    if (m_enableDirectAllocationCompletion) {
        if (!outPort->GetUbQueue()->IsEmpty()) {
            outPort->NotifyAllocationFinish();
        }
    } else {
        SchedulePortNotification(outPort);
    }
    const bool shouldRunAgain =
        allocatorOwnedEpisode && m_runAgain[outPortId] && HasNonEmptyIngress(outPortId);
    m_runAgain[outPortId] = false;
    m_running[outPortId] = false;
    if (shouldRunAgain) {
        TriggerAllocator(outPort);
    }
}

Ptr<UbIngressQueue>
UbRoundRobinAllocator::PickNextIngressQueue(Ptr<UbPort> outPort)
{
    return SelectNextIngressQueue(outPort);
}

Ptr<UbIngressQueue> UbRoundRobinAllocator::SelectNextIngressQueue(Ptr<UbPort> outPort)
{
    uint32_t pi;
    uint32_t outPortId = outPort->GetIfIndex();
    auto node = NodeList::GetNode(m_nodeId);
    auto vlNum = node->GetObject<UbSwitch>()->GetVLNum();
    for (pi = 0 ; pi < vlNum; pi++) {
        const uint32_t ingressQueueSlotCount = GetIngressQueueSlotCount(outPortId, pi);
        if (ingressQueueSlotCount == 0) {
            continue;
        }
        if (!m_rrPhaseSeeded[outPortId][pi]) {
            BooleanValue enabled;
            UintegerValue seed;
            const bool useSeededPhase =
                GlobalValue::GetValueByNameFailSafe("UB_ALLOCATOR_RR_PHASE_HASH_ENABLE", enabled) &&
                enabled.Get() &&
                GlobalValue::GetValueByNameFailSafe("UB_ALLOCATOR_RR_PHASE_SEED", seed);
            m_rrIdx[outPortId][pi] = useSeededPhase
                                         ? ComputeSeededIngressPhase(seed.Get(),
                                                                      m_nodeId,
                                                                      outPortId,
                                                                      pi,
                                                                      ingressQueueSlotCount)
                                         : ComputeInitialIngressPhase(m_nodeId,
                                                                       outPortId,
                                                                       ingressQueueSlotCount);
            m_rrPhaseSeeded[outPortId][pi] = true;
        } else if (m_rrIdx[outPortId][pi] >= ingressQueueSlotCount) {
            m_rrIdx[outPortId][pi] %= ingressQueueSlotCount;
        }
        auto candidate =
            FindNextEligibleIngressQueue(outPortId, pi, m_rrIdx[outPortId][pi], outPort);
        if (m_disposed) {
            return nullptr;
        }
        if (candidate.has_value()) {
            m_selectedIngressQueueSlot[outPortId] = candidate->ingressQueueSlot;
            m_rrIdx[outPortId][pi] = (candidate->ingressQueueSlot + 1) % ingressQueueSlotCount;
            NS_LOG_DEBUG("[UbSwitchAllocator DispatchPacket] " << " NodeId: " << node->GetId()
                                                              << " PortId: " << outPortId
                                                              << " ingressQueueSlot: "
                                                              << candidate->ingressQueueSlot);
            return candidate->queue;
        }
    }
    return nullptr;
}


/*-----------------------------------------UbDwrrAllocator----------------------------------------------*/

TypeId UbDwrrAllocator::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbDwrrAllocator")
        .SetParent<UbSwitchAllocator>()
        .AddConstructor<UbDwrrAllocator>()
        .AddAttribute("DefaultQuantum",
                      "Default DWRR quantum in bytes for all VLs.",
                      UintegerValue(1500),
                      MakeUintegerAccessor(&UbDwrrAllocator::m_defaultQuantum),
                      MakeUintegerChecker<uint32_t>(500, 1<<30))
        .AddAttribute("VlQuantums",
                      "Per-VL quantum overrides as 'vl:bytes,vl:bytes', e.g. '7:6000,8:12000'.",
                      StringValue(""),
                      MakeStringAccessor(&UbDwrrAllocator::m_vlQuantumsStr),
                      MakeStringChecker())
        ;
    return tid;
}

void UbDwrrAllocator::Init()
{
    auto node = NodeList::GetNode(m_nodeId);
    uint32_t portsNum = node->GetNDevices();
    InitializePortEventState(portsNum);
    auto vlNum = node->GetObject<UbSwitch>()->GetVLNum();

    m_rrIdx.assign(portsNum, std::vector<uint32_t>(vlNum, 0));
    m_rrPhaseSeeded.assign(portsNum, std::vector<bool>(vlNum, false));
    m_quantum.assign(portsNum, std::vector<uint32_t>(vlNum, 0));
    m_deficit.assign(portsNum, std::vector<uint32_t>(vlNum, 0));
    m_lastSelectedIngressQueueSlot.assign(portsNum, std::vector<uint32_t>(vlNum, 0));
    m_currVlIdx.assign(portsNum, 0);

    m_running.assign(portsNum, false);
    m_runAgain.assign(portsNum, false);
    m_nonEmptyIngressCount.assign(portsNum, 0);
    m_voqIngressSlotCount = portsNum;
    RegisterEgressStauts(portsNum);

    m_ingressSources.resize(portsNum);
    for (auto &i : m_ingressSources) {
        i.resize(vlNum);
    }

    ApplyDefaultQuantum();
    ParseAndApplyVlQuantums(m_vlQuantumsStr);
}

void UbDwrrAllocator::SetQuantum(uint32_t priority, uint32_t quantum)
{
    for (uint32_t port = 0; port < m_quantum.size(); ++port) {
        if (priority < m_quantum[port].size()) {
            m_quantum[port][priority] = quantum;
        }
    }
}

void UbDwrrAllocator::SetQuantum(uint32_t outPort, uint32_t priority, uint32_t quantum)
{
    if (outPort >= m_quantum.size()) {
        return;
    }
    if (priority >= m_quantum[outPort].size()) {
        return;
    }
    m_quantum[outPort][priority] = quantum;
}

Ptr<UbIngressQueue> UbDwrrAllocator::SelectNextIngressQueue(Ptr<UbPort> outPort)
{
    uint32_t outPortId = outPort->GetIfIndex();
    auto node = NodeList::GetNode(m_nodeId);
    auto vlNum = node->GetObject<UbSwitch>()->GetVLNum();

    if (vlNum == 0) {
        return nullptr;
    }

    uint32_t startVl = m_currVlIdx[outPortId] % vlNum;

    for (uint32_t cnt = 0; cnt < vlNum; ++cnt) {
        uint32_t pi = (startVl + cnt) % vlNum;
        const uint32_t ingressQueueSlotCount = GetIngressQueueSlotCount(outPortId, pi);

        if (ingressQueueSlotCount == 0) {
            m_deficit[outPortId][pi] = 0;
            continue;
        }

        if (!m_rrPhaseSeeded[outPortId][pi]) {
            BooleanValue enabled;
            UintegerValue seed;
            const bool useSeededPhase =
                GlobalValue::GetValueByNameFailSafe("UB_ALLOCATOR_RR_PHASE_HASH_ENABLE", enabled) &&
                enabled.Get() &&
                GlobalValue::GetValueByNameFailSafe("UB_ALLOCATOR_RR_PHASE_SEED", seed);
            m_rrIdx[outPortId][pi] = useSeededPhase
                                         ? ComputeSeededIngressPhase(seed.Get(),
                                                                      m_nodeId,
                                                                      outPortId,
                                                                      pi,
                                                                      ingressQueueSlotCount)
                                         : ComputeInitialIngressPhase(m_nodeId,
                                                                       outPortId,
                                                                       ingressQueueSlotCount);
            m_rrPhaseSeeded[outPortId][pi] = true;
        } else if (m_rrIdx[outPortId][pi] >= ingressQueueSlotCount) {
            m_rrIdx[outPortId][pi] %= ingressQueueSlotCount;
        }

        auto candidate =
            FindNextEligibleIngressQueue(outPortId, pi, m_rrIdx[outPortId][pi], outPort);
        if (m_disposed) {
            return nullptr;
        }
        if (!candidate.has_value()) {
            m_deficit[outPortId][pi] = 0;
            continue;
        }

        m_deficit[outPortId][pi] += m_quantum[outPortId][pi];
        m_lastSelectedIngressQueueSlot[outPortId][pi] = candidate->ingressQueueSlot;
        m_rrIdx[outPortId][pi] = (candidate->ingressQueueSlot + 1) % ingressQueueSlotCount;
        m_currVlIdx[outPortId] = (pi + 1) % vlNum;

        NS_LOG_DEBUG("[UbDwrrAllocator SelectNextIngressQueue]"
                     << " NodeId: " << node->GetId()
                     << " OutPortId: " << outPortId
                     << " VL: " << pi
                     << " ingressQueueSlot: " << candidate->ingressQueueSlot
                     << " deficit: " << m_deficit[outPortId][pi]);
        return candidate->queue;
    }

    return nullptr;
}

void UbDwrrAllocator::AllocateNextPacket(Ptr<UbPort> outPort)
{
    NS_LOG_DEBUG("[UbDwrrAllocator AllocateNextPacket] portId: " << outPort->GetIfIndex());
    auto outPortId = outPort->GetIfIndex();
    const bool allocatorOwnedEpisode = m_running[outPortId];

    // 标记这轮调度是否实际发出了至少一个包
    bool sentAny = false;
    bool egressBlocked = false;

    auto ingressQueue = GetActiveIngressQueue(outPortId);
    if (!allocatorOwnedEpisode && ingressQueue == nullptr) {
        ingressQueue = SelectNextIngressQueue(outPort);
        SetActiveIngressQueue(ingressQueue);
    }
    if (m_disposed) {
        m_running[outPortId] = false;
        return;
    }
    if (!GetEgressStatus(outPortId)) {
        // Detected failed output: keep the packet in its ingress queue for recovery.
        ClearActiveIngressQueue(outPortId);
        m_running[outPortId] = false;
        return;
    }
    SetActiveIngressQueue(ingressQueue);
    if (ingressQueue != nullptr) {
        auto priority = ingressQueue->GetIngressPriority();
        uint32_t pi = priority;
        auto &queues = m_ingressSources[outPortId][pi];
        uint32_t ingressQueueSlotCount = GetIngressQueueSlotCount(outPortId, pi);
        if (ingressQueueSlotCount > 0) {
            uint32_t ingressQueueSlot =
                m_lastSelectedIngressQueueSlot[outPortId][pi] % ingressQueueSlotCount;
            uint32_t &deficit = m_deficit[outPortId][pi];

            bool first = true;

            while (deficit > 0 && ingressQueueSlotCount > 0) {
                Ptr<UbIngressQueue> q;
                if (first) {
                    // The first grant was selected at wakeup; AllocationTime models the
                    // computation of that decision, so later flow-control changes do not
                    // revoke it. Subsequent packets in the burst are selected at commit time.
                    q = ingressQueue;
                } else {
                    auto candidate =
                        FindNextEligibleIngressQueue(outPortId, pi, ingressQueueSlot, outPort);
                    if (!candidate.has_value()) {
                        deficit = 0;
                        break;
                    }
                    q = candidate->queue;
                    ingressQueueSlot = candidate->ingressQueueSlot;
                }
                SetActiveIngressQueue(q);

                uint32_t pktSize = q->GetNextPacketSize();
                if (!outPort->GetUbQueue()->CanEnqueue(pktSize)) {
                    NS_LOG_DEBUG("[UbDwrrAllocator AllocateNextPacket] egress queue full, keep packet in ingress queue"
                                 << " outPortId=" << outPortId
                                 << " bytes=" << pktSize);
                    egressBlocked = true;
                    break;
                }

                // 第一个包就发不出去：不扣赤字，并回滚 m_rrIdx
                if (pktSize > deficit) {
                    if (!sentAny) {
                        m_rrIdx[outPortId][pi] = ingressQueueSlot;
                    }
                    break;
                }

                auto packet = q->GetNextPacket();
                auto inPortId = q->GetInPortId();
                auto packetEntry = std::make_tuple(inPortId, priority, packet);
                auto context = MakeAllocatorFlowControlEventContext(packet,
                                                                    q,
                                                                    inPortId,
                                                                    outPortId,
                                                                    priority);
                const bool enqueueOk = outPort->EnqueueToEgress(packetEntry);
                NS_ASSERT_MSG(enqueueOk,
                              "allocator pre-check promised egress queue capacity, but DoEnqueue still failed");
                outPort->GetFlowControl()->OnEgressEnqueued(context);

                // Packet moved from VOQ to EgressQueue, notify Switch to update buffer statistics
                if (q->GetIngressQueueType() != IngressQueueType::TP &&
                    q->GetIngressQueueType() != IngressQueueType::CTP &&
                    !q->IsGeneratedDataPacket()) {
                    // Forwarded packet (not locally generated)
                    auto node = NodeList::GetNode(m_nodeId);
                    auto inPort = DynamicCast<UbPort>(node->GetDevice(inPortId));
                    node->GetObject<UbSwitch>()->NotifySwitchDequeue(inPortId, outPortId, priority, packet);
                    inPort->GetFlowControl()->OnIngressReleased(context);
                    if (m_disposed) {
                        ClearActiveIngressQueue(outPortId);
                        m_running[outPortId] = false;
                        return;
                    }
                }
                if (m_disposed) {
                    ClearActiveIngressQueue(outPortId);
                    m_running[outPortId] = false;
                    return;
                }

                sentAny = true;
                deficit -= pktSize;

                if (first) {
                    ingressQueueSlot = m_rrIdx[outPortId][pi];
                    first = false;
                } else {
                    ingressQueueSlotCount = GetIngressQueueSlotCount(outPortId, pi);
                    if (ingressQueueSlotCount == 0) {
                        break;
                    }
                    m_rrIdx[outPortId][pi] =
                        (ingressQueueSlot + 1) % ingressQueueSlotCount;
                    ingressQueueSlot = m_rrIdx[outPortId][pi];
                }
            }

            bool vlHasNoEligibleQueue = true;
            for (auto &qq : queues) {
                if (qq->IsEmpty()) {
                    continue;
                }
                const bool fcLimited = outPort->GetFlowControl()->IsFcLimited(qq);
                if (m_disposed) {
                    ClearActiveIngressQueue(outPortId);
                    m_running[outPortId] = false;
                    return;
                }
                if (!fcLimited) {
                    vlHasNoEligibleQueue = false;
                    break;
                }
            }
            // Preserve the wakeup grant's pre-selection deficit when its first
            // commit is rejected by egress capacity; the rollback below must
            // subtract the quantum that SelectNextIngressQueue added.
            if (vlHasNoEligibleQueue && !egressBlocked) {
                deficit = 0;
            }
        }
    }

    if (ingressQueue != nullptr && !sentAny) {
        if (egressBlocked) {
            const uint32_t priority = ingressQueue->GetIngressPriority();
            m_deficit[outPortId][priority] -= m_quantum[outPortId][priority];
            m_rrIdx[outPortId][priority] =
                m_lastSelectedIngressQueueSlot[outPortId][priority];
            m_currVlIdx[outPortId] = priority;
        }
        ClearActiveIngressQueue(outPortId);
        m_running[outPortId] = false;
        if (egressBlocked) {
            m_runAgain[outPortId] = false;
            return;
        }
        TriggerAllocator(outPort);
        return;
    }

    ClearActiveIngressQueue(outPortId);
    if (m_enableDirectAllocationCompletion) {
        if (!outPort->GetUbQueue()->IsEmpty()) {
            outPort->NotifyAllocationFinish();
        }
    } else {
        SchedulePortNotification(outPort);
    }
    const bool shouldRunAgain =
        allocatorOwnedEpisode && m_runAgain[outPortId] && HasNonEmptyIngress(outPortId);
    m_runAgain[outPortId] = false;
    m_running[outPortId] = false;
    if (shouldRunAgain) {
        TriggerAllocator(outPort);
    }
}

Ptr<UbIngressQueue>
UbDwrrAllocator::PickNextIngressQueue(Ptr<UbPort> outPort)
{
    return SelectNextIngressQueue(outPort);
}

void UbDwrrAllocator::ApplyDefaultQuantum()
{
    auto node = NodeList::GetNode(m_nodeId);
    uint32_t portsNum = node->GetNDevices();
    auto vlNum = node->GetObject<UbSwitch>()->GetVLNum();

    uint32_t q = std::max<uint32_t>(m_defaultQuantum, 64);
    for (uint32_t p = 0; p < portsNum; ++p) {
        for (uint32_t vl = 0; vl < vlNum; ++vl) {
            m_quantum[p][vl] = q;
        }
    }
}

void UbDwrrAllocator::ParseAndApplyVlQuantums(const std::string& s)
{
    if (s.empty()) return;

    std::istringstream ss(s);
    std::string token;

    auto node = NodeList::GetNode(m_nodeId);
    uint32_t portsNum = node->GetNDevices();
    auto vlNum = node->GetObject<UbSwitch>()->GetVLNum();

    while (std::getline(ss, token, ',')) {
        if (token.empty()) continue;

        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);

        auto pos = token.find(':');
        if (pos == std::string::npos) continue;

        std::string vlStr = token.substr(0, pos);
        std::string qStr  = token.substr(pos + 1);

        vlStr.erase(0, vlStr.find_first_not_of(" \t"));
        vlStr.erase(vlStr.find_last_not_of(" \t") + 1);
        qStr.erase(0, qStr.find_first_not_of(" \t"));
        qStr.erase(qStr.find_last_not_of(" \t") + 1);

        char* endp = nullptr;
        long vl = std::strtol(vlStr.c_str(), &endp, 10);
        if (endp == vlStr.c_str() || vl < 0 || (uint32_t)vl >= vlNum) {
            NS_LOG_WARN("UbDwrrAllocator::VlQuantums invalid vl: " << vlStr);
            continue;
        }

        endp = nullptr;
        long q = std::strtol(qStr.c_str(), &endp, 10);
        if (endp == qStr.c_str() || q <= 0) {
            NS_LOG_WARN("UbDwrrAllocator::VlQuantums invalid quantum: " << qStr);
            continue;
        }

        for (uint32_t p = 0; p < portsNum; ++p) {
            m_quantum[p][(uint32_t)vl] = (uint32_t)q;
        }
    }
}

} // namespae ns3
