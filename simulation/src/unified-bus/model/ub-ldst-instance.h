// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_LDST_INSTANCE_H
#define UB_LDST_INSTANCE_H

#include "ns3/node-list.h"
#include "ns3/node.h"
#include "ns3/simulator.h"
#include "ns3/ub-datatype.h"
#include "ns3/ub-lifecycle.h"
#include "ns3/ub-network-address.h"

#include <functional>
#include <type_traits>
#include <unordered_set>

namespace ns3 {
class UbLdstThread;

struct UbLdstForcedStopResidue
{
    uint64_t activeTasks{0};
    uint64_t activeSegments{0};
    uint64_t waitingAcks{0};
    uint64_t queuedSegments{0};
    uint64_t trackedActions{0};
};

class UbLdstInstance : public Object {
public:
    static TypeId GetTypeId(void);
    UbLdstInstance();
    virtual ~UbLdstInstance();
    void DoDispose(void) override;
    void Init(uint32_t nodeId);
    // 接收任务接口，分配给thread
    void HandleLdstTask(uint32_t src,
                        uint32_t dest,
                        uint32_t size,
                        uint32_t taskId,
                        uint32_t priority,
                        UbMemOperationType type,
                        const std::vector<uint32_t>& threadIds,
                        uint64_t address,
                        uint32_t srcPortId = 0,
                        bool hasSrcPortId = false,
                        uint32_t dstPortId = 0,
                        bool hasDstPortId = false);

    void SetClientCallback(Callback<void, uint32_t> cb);
    Ptr<UbLdstThread> GetLdstThread(uint32_t threadId);
    Callback<void, uint32_t> FinishCallback;
    void OnRecvAck(uint32_t taskSegmentId);
    bool TryRecvAck(uint32_t taskSegmentId);
    bool OnRecvAckForTest(uint32_t taskSegmentId);
    void OnTaskSegmentCompleted(Ptr<UbLdstTaskSegment> taskSegment);

    template <typename MemPtr,
              typename... Args,
              typename std::enable_if<std::is_member_function_pointer<MemPtr>::value,
                                      int>::type = 0>
    void ScheduleNow(MemPtr memPtr, Args... args)
    {
        NS_ABORT_MSG_IF(memPtr == nullptr, "LDST tracked action cannot be null");
        if (m_disposed)
        {
            return;
        }
        NS_ABORT_MSG_IF(m_lifecycleOwner.generation == 0,
                        "LDST tracked action requires an initialized owner");
        const uint64_t token = m_trackedActions.Track(m_lifecycleOwner);
        EventId event =
            Simulator::ScheduleNow(&UbLdstInstance::RunTypedTrackedAction<MemPtr, Args...>,
                                   this,
                                   token,
                                   memPtr,
                                   args...);
        m_trackedActions.Attach(m_lifecycleOwner, token, event, Callback<void>());
    }
    void ScheduleNow(Callback<void> action);

    size_t GetActiveTaskCount() const;
    size_t GetActiveSegmentCount() const;
    size_t GetActiveTaskCountForTest() const
    {
        return GetActiveTaskCount();
    }
    size_t GetActiveSegmentCountForTest() const
    {
        return GetActiveSegmentCount();
    }
    Ptr<UbLdstTaskSegment> GetTaskSegmentForTest(uint32_t taskSegmentId) const;
    UbLifecycleResourceSnapshot GetLifecycleSnapshotForTest() const;
    const UbLdstForcedStopResidue& GetLastForcedStopResidueForTest() const;

private:
    struct SegmentRecord
    {
        Ptr<UbLdstTaskSegment> segment;
        UbLifecycleResourceIdentity lifecycle;
    };

    template <typename MemPtr,
              typename... Args,
              typename std::enable_if<std::is_member_function_pointer<MemPtr>::value,
                                      int>::type = 0>
    void RunTypedTrackedAction(uint64_t token, MemPtr memPtr, Args... args)
    {
        if (!m_trackedActions.Begin(m_lifecycleOwner, token))
        {
            return;
        }
        std::invoke(memPtr, args...);
        ResolveTrackedAction(token);
    }

    void RunTrackedAction(uint64_t token, Callback<void> action);
    void ResolveTrackedAction(uint64_t token);
    bool ResolveAck(uint32_t taskSegmentId);
    void MemTaskStartsNotify(uint32_t nodeId, uint32_t memTaskId);
    void LastPacketACKsNotify(uint32_t nodeId, uint32_t taskId);
    void MemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId);
    void FirstPacketSendsNotify(uint32_t nodeId, uint32_t memTaskId);
    void LastPacketSendsNotify(uint32_t nodeId, uint32_t memTaskId);
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> m_taskToSegmentMap;
    std::vector<Ptr<UbLdstThread>> m_threads;
    std::unordered_map<uint32_t, SegmentRecord> m_taskSegmentsMap;
    uint32_t m_currentTaskId = 0;
    uint32_t m_threadNum = 0;
    uint32_t m_queuePriority = 0;
    UbLifecycleResourceIdentity m_lifecycleOwner;
    UbTrackedEventSet m_trackedActions;
    UbLdstForcedStopResidue m_lastForcedStopResidue;
    bool m_disposed{false};
    
    
    TracedCallback<uint32_t, uint32_t> m_traceLastPacketACKsNotify;
    TracedCallback<uint32_t, uint32_t> m_traceMemTaskCompletesNotify;
    TracedCallback<uint32_t, uint32_t> m_traceMemTaskStartsNotify;
    TracedCallback<uint32_t, uint32_t> m_traceFirstPacketSendsNotify;
    TracedCallback<uint32_t, uint32_t> m_traceLastPacketSendsNotify;
}; 
} 

#endif /* UB_LDST_INSTANCE_H*/
