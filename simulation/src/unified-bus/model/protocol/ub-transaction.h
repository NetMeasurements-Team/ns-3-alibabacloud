// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TRANSACTION_H
#define UB_TRANSACTION_H

#include "ns3/callback.h"
#include "ns3/random-variable-stream.h"
#include "ns3/ub-datatype.h"
#include "ub-transaction-rules.h"
#include "ns3/ub-lifecycle.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-sliding-bitmap-window.h"
#include <ns3/event-id.h>
#include <ns3/node.h>
#include <ns3/simple-ref-count.h>

#include <deque>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace ns3 {
    /**
     * @brief Transaction Service Mode enumeration
     */
    enum class TransactionServiceMode : uint8_t {
        ROI = 0,    // Reliable Ordered by Initiator
        ROT = 1,    // Reliable Ordered by Target
        ROL = 2,    // Reliable Ordered by Lower Layer
        UNO = 3     // Unreliable No Order
    };

    class UbController;
    class UbJetty;
    class UbFunction;
    class UbTransportChannel;
    class UbTransportGroup;

    struct UbCtpEntityKey
    {
        uint32_t srcEntityId{0};
        uint32_t dstNodeId{0};
        uint32_t dstEntityId{0};
        uint8_t vl{0};

        bool operator<(const UbCtpEntityKey& other) const;
        bool operator==(const UbCtpEntityKey& other) const;
    };

    enum class UbCtpPreparationStatus : uint8_t
    {
        PREPARED,
        ID_SPACE_EXHAUSTED,
        INVALID_WINDOW,
        BINDING_MISMATCH,
        JETTY_NOT_ACCEPTING,
    };

    struct UbCtpPreparationResult
    {
        UbCtpPreparationStatus status{UbCtpPreparationStatus::BINDING_MISMATCH};
        std::optional<uint32_t> rcid;
        std::string message;
        bool IsSuccess() const { return status == UbCtpPreparationStatus::PREPARED; }
    };

    enum class UbRtpTransportReferenceKind : uint8_t
    {
        DIRECT_TP = 0,
        TPG = 1,
    };

    enum class UbRtpOperationPreflightResult : uint8_t
    {
        ACCEPTING = 0,
        NOT_ACCEPTING = 1,
        NO_MEMBER = 2,
        STALE_REFERENCE = 3,
    };

    class UbRtpTransportReference
    {
      public:
        static UbRtpTransportReference ForDirectTp(
            uint32_t tpn,
            const UbLifecycleResourceIdentity& identity);
        static UbRtpTransportReference ForTpGroup(
            const UbLifecycleResourceIdentity& identity);

        UbRtpTransportReferenceKind GetKind() const;
        const UbLifecycleResourceIdentity& GetIdentity() const;
        bool IsDirectTp() const;
        uint32_t GetDirectTpn() const;

      private:
        UbRtpTransportReference() = default;
        UbRtpTransportReference(UbRtpTransportReferenceKind kind,
                                const UbLifecycleResourceIdentity& identity,
                                uint32_t directTpn);

        UbRtpTransportReferenceKind m_kind{UbRtpTransportReferenceKind::DIRECT_TP};
        UbLifecycleResourceIdentity m_identity;
        uint32_t m_directTpn{UINT32_MAX};

        friend class UbTransaction;
        friend struct UbTransactionOperationSnapshot;
    };

    enum class UbTransactionProtocolStatus : uint8_t
    {
        /** The target executed the request successfully. */
        SUCCESS = 0,
    };
    /** Outcome one target executor reports back through its completion callback. */
    struct UbWorkExecutionResult
    {
        /** Transport/lifecycle outcome of the execution attempt. */
        UbWorkOutcome outcome{UbWorkOutcome::COMPLETED};
        /** Protocol status of a completed execution. */
        UbTransactionProtocolStatus protocolStatus{UbTransactionProtocolStatus::SUCCESS};
        /** Optional target-selected response payload length (currently Atomic_sync: 0 or N). */
        std::optional<uint32_t> responsePayloadBytes;
    };
    /**
     * Completion callback handed to a registered target executor.
     *
     * It is asynchronous and may be called from any later simulation event; the first call decides
     * the result and later calls are ignored, so an executor can defer or duplicate it safely.
     */
    using UbTargetCompletion = Callback<void, UbWorkExecutionResult>;
    /**
     * Registered target executor: the only target execution seam of a transaction.
     *
     * It receives one complete, already validated inbound request (with its cached
     * `UbTransactionResolved`), its static rule, and the completion callback to report the result.
     * The executor never builds a response packet; the transaction layer does that from the result.
     */
    using UbTargetExecutor = Callback<void,
                                      Ptr<const UbWqeSegment>,
                                      const UbTransactionRule&,
                                      UbTargetCompletion>;
    /**
     * Carrier response sink: where a built response segment goes when the carrier is not the local
     * RTP queue.
     */
    using UbTransactionResponseSink = Callback<void, Ptr<UbWqeSegment>>;

    struct UbTransactionOperationSnapshot
    {
        UbLifecycleResourceIdentity wqeIdentity;
        UbLifecycleResourceIdentity operationIdentity;
        UbRtpTransportReference transportReference;
        uint32_t directTpn{UINT32_MAX};
        uint32_t activeSegments{0};
        uint32_t activeTransportWorkLeases{0};
        std::vector<UbLifecycleResourceIdentity> segmentIdentities;
        std::vector<UbLifecycleResourceIdentity> resolvedTpIdentities;
    };

    /**
    * @brief
    */
    class UbTransaction : public Object {
    public:

        struct RtpTransportResolution
        {
            UbRtpTransportReferenceKind kind{UbRtpTransportReferenceKind::DIRECT_TP};
            UbLifecycleResourceIdentity identity;
            uint32_t directTpn{UINT32_MAX};
            Ptr<UbTransportChannel> directTp;
            Ptr<UbTransportGroup> group;
        };

        static TypeId GetTypeId(void);

        UbTransaction();
        UbTransaction(Ptr<Node> node);
        virtual ~UbTransaction();

        UbFunction* GetFunction();
        void SetFunction(UbFunction* function);
        void SetOwnerController(UbController* controller);

        Ptr<UbJetty> GetJetty(uint32_t jettyNum);

        bool JettyBindTp(uint32_t src,
                         uint32_t dest,
                         uint32_t jettyNum,
                         bool multiPath,
                         std::vector<uint32_t> tpns,
                         uint32_t taskId = 0);

        bool HasJettyTpBinding(uint32_t jettyNum) const;

        void DestroyJettyTpMap(uint32_t jettyNum);
        /** Release one completed task's binding and optionally retire its now-unused TPG. */
        void ReleaseCompletedJettyTransportBinding(uint32_t jettyNum, bool destroyTpGroup);
        void EraseRtpTpGroupBindings(
            const UbLifecycleResourceIdentity& groupIdentity);

        const std::vector<Ptr<UbTransportChannel>> GetJettyLiveTps(uint32_t jettyNum);

        [[deprecated("use GetJettyLiveTps")]] const std::vector<Ptr<UbTransportChannel>>
        GetJettyRelatedTpVec(uint32_t jettyNum)
        {
            return GetJettyLiveTps(jettyNum);
        }

        std::vector<Ptr<UbJetty>> GetJettiesUsingTpn(uint32_t tpn);

        [[deprecated("use GetJettiesUsingTpn")]] std::vector<Ptr<UbJetty>> GetTpRelatedJettyVec(
            uint32_t tpn)
        {
            return GetJettiesUsingTpn(tpn);
        }

        // function层调用
        void TriggerScheduleWqeSegment(uint32_t jettyNum,
                                       Ptr<UbWqe> originatingWqe,
                                       bool lifecycleOperation);
        void TriggerScheduleCtpWqe(uint32_t jettyNum, uint32_t wqeId);
        void TryContinueJettyTransmission(uint32_t jettyNum);
        void TryContinueJettyTransmission(Ptr<UbTransportChannel> tp);
        void TryContinueJettyTransmission(Ptr<UbTransportGroup> tpGroup);

        // tp调用
        std::optional<uint32_t> GetCtpRequesterRcid(uint32_t jettyNum) const;
        const UbLifecycleResourceIdentity* FindCtpRequester(uint32_t rcid) const;
        Ptr<UbWqeSegment> FindCtpOutstanding(uint32_t rcid,
                                              uint16_t wireSsn,
                                              UbCtpEntityKey& key,
                                              std::string& rejection);
        void RetireCtpRequester(uint32_t jettyNum, const UbLifecycleResourceIdentity& identity);
        uint32_t GetNextCtpRcidForTest() const { return m_nextCtpRcid; }
        void SetNextCtpRcidForTest(uint32_t next);
        Ptr<UbWqeSegment> PrepareNextCtpWqeSegment(Ptr<UbJetty> jetty,
                                                   uint32_t maxSegmentBytes);
        void CommitCtpWqeSegment(Ptr<UbWqeSegment> segment);
        bool ProcessWqeSegmentComplete(Ptr<UbWqeSegment> wqeSegment);
        Ptr<UbWqeSegment> BuildTransactionResponse(uint32_t localTpn,
                                                    Ptr<UbWqeSegment> request,
                                                    const UbTransactionRule& rule,
                                                    UbWorkExecutionResult result) const;
        void HandleInboundTaUnit(uint32_t localTpn, Ptr<UbWqeSegment> segment);
        /**
         * Deliver one decoded inbound request through the asynchronous target seam.
         *
         * This is the only target execution entry for both carriers. It consumes the request's
         * cached `UbTransactionResolved` (a request without one is rejected as a programming
         * error), applies `TargetExecutionDelay` exactly once, and then invokes the registered
         * executor or the built-in default executor. Completion is asynchronous: the response is
         * built and handed to the sink from the completion callback.
         *
         * @param localTpn TPN the request arrived on, or UINT32_MAX for carriers without one.
         * @param request Complete inbound request with its cached normalized result.
         * @param sink Carrier response sink; the local RTP queue is used when it is null.
         */
        void ExecuteInboundTransaction(uint32_t localTpn,
                                       Ptr<UbWqeSegment> request,
                                       UbTransactionResponseSink sink = {});
        /**
         * Register the asynchronous target callback.
         *
         * Registration is frozen once the first request has entered target execution, so a run
         * cannot change target semantics halfway.
         *
         * @return false when execution already started.
         */
        bool SetTargetExecutor(UbTargetExecutor executor);
        /**
         * Set the single delay applied between inbound receipt and target execution.
         *
         * The default is 0ns and both carriers share this one delay. A strictly negative delay is
         * rejected, and the value is frozen once execution has started.
         *
         * @return false when the delay is negative or execution already started.
         */
        bool SetTargetExecutionDelay(Time delay);
        /** Current target execution delay. */
        Time GetTargetExecutionDelay() const { return m_targetExecutionDelay; }
        bool ProcessInboundTaResponse(Ptr<UbWqeSegment> response);
        /**
         * Return the request opcode of an outstanding operation.
         *
         * A transaction response's extension-header layout belongs to its request rule, not to the
         * shared response opcode. The RTP carrier asks this before decoding a response whose opcode
         * cannot distinguish the request layout (currently Atomic_sync's TAIDETAH echo). The
         * initiator jetty and WQE/task id identify the operation; every segment of one WQE carries
         * the same request opcode, so the segment TASSN is not needed here.
         */
        std::optional<UbTransactionOpcode> FindOutstandingRequestOpcode(uint32_t originJettyNum,
                                                                        uint32_t taskId) const;
        uint64_t DeriveRemoteAddressForTest(const Ptr<UbWqeSegment>& request) const
        {
            return DeriveRemoteAddress(request);
        }

        void TpInit(Ptr<UbTransportChannel> tp);

        void TpDeinit(uint32_t tpn);

        // 判断wqe是否能发送
        bool IsOrderedByInitiator(uint32_t jettyNum, Ptr<UbWqe> wqe);
        // TODO: support ROT
        bool IsOrderedByTarget(Ptr<UbWqe> wqe);
        // TODO: support Reliable
        bool IsReliable(Ptr<UbWqe> wqe);
        bool IsUnreliable(Ptr<UbWqe> wqe);

        void SetTransactionServiceMode(uint32_t jettyNum, TransactionServiceMode mode);
        TransactionServiceMode GetTransactionServiceMode(uint32_t jettyNum);

        // 新增wqe任务
        void AddWqe(uint32_t jettyNum, Ptr<UbWqe> wqe);

        const UbRtpTransportReference* FindRtpTransportReference(uint32_t jettyNum) const;
        bool IsRtpTransportReferenceCurrent(
            const UbRtpTransportReference& reference) const;
        UbRtpOperationPreflightResult PreflightRtpOperation(
            const UbRtpTransportReference& reference) const;
        UbRtpOperationPreflightResult PreflightRtpOperation(
            const UbRtpTransportReference& reference,
            RtpTransportResolution& resolution) const;
        bool TryAdmitRtpWqe(uint32_t jettyNum,
                            Ptr<UbWqe> wqe,
                            const UbLifecycleResourceIdentity& wqeIdentity,
                            const RtpTransportResolution& resolution);
        bool TryAdmitCtpWqe(uint32_t jettyNum,
                            Ptr<UbWqe> wqe,
                            const UbLifecycleResourceIdentity& wqeIdentity,
                            const UbCtpEntityKey& key);
        bool IsCtpWqeDispatchReady(uint32_t jettyNum, uint32_t wqeId) const;
        bool HasWqeOperation(uint32_t jettyNum, uint32_t wqeId) const;
        bool HasLifecycleOperationForJetty(uint32_t jettyNum) const;
        bool HasRtpTpGroupWork(
            const UbLifecycleResourceIdentity& groupIdentity) const;
        uint32_t GetResidentOperationCount() const;
        uint64_t GetResidentSegmentCount() const;
        uint32_t GetActiveOperationCount() const;
        uint32_t GetRtpTxQueuePendingEntryCountForTest(
            const UbLifecycleResourceIdentity& identity) const;
        bool IsRtpTxQueueWaitingForTransportForTest(
            const UbLifecycleResourceIdentity& identity) const;
        bool IsRtpTxQueueEntryActiveForTest(
            const UbLifecycleResourceIdentity& identity) const;
        bool IsRtpTxQueueEventPendingForTest(
            const UbLifecycleResourceIdentity& identity) const;
        uint64_t GetRtpTxQueueCapacityRecoveryCountForTest(
            const UbLifecycleResourceIdentity& identity) const;
        uint32_t GetRtpTxQueueCountForTest() const;
        void ResetRtpResolutionCountersForTest();
        uint64_t GetRtpResolutionCountForTest() const;
        uint64_t GetRtpTpgMaterializationCountForTest() const;
        void NotifyRtpTpGroupMemberAvailable(
            const UbLifecycleResourceIdentity& groupIdentity);
        void TerminateRtpTpGroupOperations(const UbLifecycleResourceIdentity& groupIdentity,
                                           UbWorkOutcome outcome,
                                           bool discardRetainedSegments);
        void RetireRtpTxQueue(
            const UbLifecycleResourceIdentity& identity);
        bool TryGetOperationSnapshot(uint32_t jettyNum,
                                     uint32_t wqeId,
                                     UbTransactionOperationSnapshot& snapshot) const;
        uint32_t GetTpRoundRobinIndexForTest(uint32_t tpn) const;
        void SetTpRoundRobinIndexForTest(uint32_t tpn, uint32_t index);

        enum class WqeTerminalMode : uint8_t
        {
            CONTINUE_ACCEPTED = 0,
            STOP_ACCEPTED = 1,
            SIMULATION_STOP = 2,
            FORCE_DRAIN [[deprecated("use SIMULATION_STOP")]] = SIMULATION_STOP,
        };

        // 某个wqe完成，刷新状态
        void WqeFinish(uint32_t jettyNum,
                       Ptr<UbWqe> wqe,
                       UbWorkOutcome outcome = UbWorkOutcome::COMPLETED);
        void CompleteWqeOperation(uint32_t jettyNum,
                                  uint32_t wqeId,
                                  UbWorkOutcome outcome);
        void CompleteWqeOperationForDrain(uint32_t jettyNum,
                                          uint32_t wqeId,
                                          UbWorkOutcome outcome,
                                          WqeTerminalMode terminalMode);
        void DiscardJettyRetainedWqeSegmentsForSimulationStop(uint32_t jettyNum);

        [[deprecated("use DiscardJettyRetainedWqeSegmentsForSimulationStop")]] void
        ForceDrainTerminalPendingOperations(uint32_t jettyNum)
        {
            DiscardJettyRetainedWqeSegmentsForSimulationStop(jettyNum);
        }

        // 判断本地tp是否处于使用状态
        bool IsTpInUse(uint32_t tpn);
        bool HasPendingTransportWork(uint32_t tpn) const;

        // 判断与本地tp配对的远程tp是否处于使用状态
        bool IsPeerTpInUse(uint32_t tpn);

        std::vector<uint32_t> GetUselessTpns();


    private:

        friend class UbCtpTransportService;
        UbCtpPreparationResult PrepareCtpRequester(Ptr<UbJetty> jetty);
        uint32_t m_nextCtpRcid{0};
        std::map<uint32_t, UbLifecycleResourceIdentity> m_ctpRequesters;
        std::map<uint32_t, uint32_t> m_ctpRcidByJetty;

        void DoDispose() override;

        struct RtpTxQueueKey;
        struct OperationRecord;
        void RunTrackedPrepareRtpWqeSegment(uint32_t jettyNum,
                                            Ptr<UbWqe> originatingWqe,
                                            RtpTxQueueKey queueKey,
                                            uint64_t eventToken);
        void RunTrackedAdmitRtpWqeSegment(Ptr<UbWqeSegment> segment,
                                          Ptr<UbWqe> originatingWqe,
                                          RtpTxQueueKey queueKey,
                                          uint64_t eventToken);
        enum class RtpSegmentAdmissionResult : uint8_t
        {
            ADMITTED,
            STALE_PREPARED_CANDIDATE,
            DEFERRED,
            REJECTED,
        };
        RtpSegmentAdmissionResult TryAdmitRtpSegment(
            uint32_t jettyNum,
            Ptr<UbWqe> originatingWqe,
            Ptr<UbWqeSegment> segment,
            Ptr<UbTransportChannel>& admittedTp);
        void ClearRtpWqeSegmentTxState(uint32_t jettyNum, Ptr<UbWqe> originatingWqe);
        struct RtpTxQueueKey
        {
            UbRtpTransportReferenceKind kind{UbRtpTransportReferenceKind::DIRECT_TP};
            UbLifecycleResourceIdentity identity;

            bool operator==(const RtpTxQueueKey& other) const
            {
                return kind == other.kind && identity == other.identity;
            }
            bool operator!=(const RtpTxQueueKey& other) const { return !(*this == other); }
            bool operator<(const RtpTxQueueKey& other) const
            {
                return std::tie(kind,
                                identity.ownerNodeId,
                                identity.kind,
                                identity.localId,
                                identity.generation) <
                       std::tie(other.kind,
                                other.identity.ownerNodeId,
                                other.identity.kind,
                                other.identity.localId,
                                other.identity.generation);
            }
        };
        struct RtpTxQueueEntry
        {
            enum class Kind : uint8_t
            {
                LOCAL_WQE,
                REMOTE_WQE_SEGMENT,
            };

            Kind kind{Kind::LOCAL_WQE};
            uint32_t jettyNum{0};
            uint32_t wqeId{0};
            UbLifecycleResourceIdentity operationIdentity;
        };
        struct RemoteRtpWqeSegmentKey
        {
            uint32_t originJettyNum{0};
            uint32_t taskId{0};
            UbLifecycleResourceIdentity operationIdentity;

            bool operator<(const RemoteRtpWqeSegmentKey& other) const
            {
                return std::tie(originJettyNum,
                                taskId,
                                operationIdentity.ownerNodeId,
                                operationIdentity.kind,
                                operationIdentity.localId,
                                operationIdentity.generation) <
                       std::tie(other.originJettyNum,
                                other.taskId,
                                other.operationIdentity.ownerNodeId,
                                other.operationIdentity.kind,
                                other.operationIdentity.localId,
                                other.operationIdentity.generation);
            }
        };
        struct RemoteRtpWqeSegmentState
        {
            std::deque<Ptr<UbWqeSegment>> segments;
            bool txEntryQueued{false};
        };
        struct RtpTxQueue
        {
            std::deque<RtpTxQueueEntry> pendingTxEntries;
            bool waitingForTransportCapacity{false};
            bool txEventPending{false};
            bool txEntryActive{false};
            EventId pendingEvent;
            std::optional<RtpTxQueueEntry> activeTxEntry;
            uint64_t transportCapacityRecoveryEvents{0};
        };

        static RtpTxQueueKey MakeRtpTxQueueKey(const UbRtpTransportReference& reference);
        void StartRtpWqeSegmentTx(uint32_t jettyNum, Ptr<UbWqe> originatingWqe);
        void EnqueueRtpWqe(uint32_t jettyNum, Ptr<UbWqe> originatingWqe, bool restoreWaitingFront);
        void EnqueueRemoteRtpWqeSegment(uint32_t tpn, Ptr<UbWqeSegment> segment);
        void RunRemoteRtpWqeSegmentTx(const RtpTxQueueKey& queueKey, const RtpTxQueueEntry& entry);
        void ScheduleRtpTx(const RtpTxQueueKey& queueKey);
        void FinishActiveRtpTxEntry(uint32_t jettyNum, Ptr<UbWqe> originatingWqe);
        void CancelRtpWqeSegmentTx(RtpTxQueueKey queueKey,
                                    uint32_t jettyNum,
                                    Ptr<UbWqe> originatingWqe);
        void DetachRtpTxEntry(OperationRecord& operation);
        void FinishWqe(uint32_t jettyNum,
                       Ptr<UbWqe> wqe,
                       UbWorkOutcome outcome,
                       WqeTerminalMode terminalMode);
        void DiscardRetainedWqeSegmentsForSimulationStop(uint32_t jettyNum, uint32_t wqeId);
        void EnqueueRtpWqeAfterBackpressure(uint32_t jettyNum, Ptr<UbWqe> originatingWqe);
        void TryContinueRtpTxQueue(const RtpTxQueueKey& key, bool capacityTransition);
        void RunRtpTx(RtpTxQueueKey queueKey);
        bool IsCurrentWqeOperation(uint32_t jettyNum, Ptr<UbWqe> wqe) const;
        bool RecordSegmentSuccess(uint32_t jettyNum,
                                  uint32_t wqeId,
                                  uint32_t taSsn,
                                  Ptr<UbWqeSegment> completion);
        struct RtpTpgMaterialization
        {
            UbLifecycleResourceIdentity groupIdentity;
            uint64_t membershipVersion{0};
            std::vector<Ptr<UbTransportChannel>> indexedTransports;
            std::vector<Ptr<UbTransportChannel>> liveTransports;
        };
        Ptr<UbController> GetOwnerController() const;
        bool ResolveRtpTransportReference(
            const UbRtpTransportReference& reference,
            RtpTransportResolution& resolution) const;
        const RtpTpgMaterialization& MaterializeRtpTpGroup(
            uint32_t jettyNum,
            const UbRtpTransportReference& reference,
            Ptr<UbTransportGroup> group);


        bool IsRoiMemoryRequest(const Ptr<UbWqeSegment>& segment);
        void ValidateRoiServiceModeOrDie(uint32_t jettyNum, const Ptr<UbWqeSegment>& segment);
        void EnqueueInboundTaResponse(uint32_t localTpn, Ptr<UbWqeSegment> response);
        void CompleteLocalRequestFromResponse(Ptr<UbWqeSegment> response);
        uint64_t DeriveRemoteAddress(const Ptr<UbWqeSegment>& request) const;

        void NotifyRtpTpGroupWorkChanged(
            const UbRtpTransportReference& reference);
        void StartDispatchGate(uint32_t jettyNum, uint32_t wqeId, bool ctp);
        void RunDispatchGate(uint32_t jettyNum,
                             uint32_t wqeId,
                             UbLifecycleResourceIdentity operationIdentity);
        void CancelDispatchEvent(OperationRecord& operation);
        /**
         * Invoke the registered target executor for one already validated inbound request.
         *
         * This is the delayed continuation of ExecuteInboundTransaction: the single target
         * execution delay has already been applied when it is reached.
         */
        void RunInboundTransaction(uint32_t localTpn,
                                   Ptr<UbWqeSegment> request,
                                   UbTransactionResponseSink sink);
        /** Drop the pending-inbound bookkeeping of one completed or rejected inbound request. */
        void ReleasePendingInboundRequest(uint32_t localTpn);
        std::vector<EventId> m_delayedInboundEvents;
        std::map<uint32_t, uint32_t> m_pendingInboundRequestsByTpn;

        uint32_t m_nodeId;
        UbController* m_ownerController{nullptr};
        UbFunction* m_function{nullptr};
        Ptr<UbLifecycleCoordinator> m_coordinator;

        // Tpn和Tp的对应map
        std::map<uint32_t, Ptr<UbTransportChannel>> m_transportByTpn;
        std::map<uint32_t, UbRtpTransportReference> m_jettyRtpReferences;
        std::map<uint32_t, RtpTpgMaterialization> m_jettyTpgMaterializations;
        // 每个 Jetty 下次从哪个 TP 开始调度，避免短 WQE 总被第一个 TP 抢占。
        std::map<uint32_t, uint32_t> m_jettyTpNextIndex;
        // 新 Jetty 的初始 TP 轮转位置；一任务一 Jetty 的短流也能展开到多 TP。
        uint32_t m_nextJettyTpStartIndex = 0;
        std::map<uint32_t, std::map<RemoteRtpWqeSegmentKey, RemoteRtpWqeSegmentState>>
            m_remoteRtpWqeSegmentsByTpn;

        std::map<uint32_t, uint32_t> m_tpRRIndex;
        Ptr<UniformRandomVariable> m_random;        //随机数产生工具，伪随机，多次仿真可复现

        // 记录每个jetty对应的serviceMode
        std::map<uint32_t, TransactionServiceMode> m_serviceMode;
        // 每个jetty存储wqe的id顺序
        std::map<uint32_t, std::vector<uint32_t>> m_orderedWqeIdsByJetty;

        struct OperationRecord
        {
            Ptr<UbWqe> wqe;
            UbLifecycleResourceIdentity wqeIdentity;
            UbLifecycleResourceIdentity operationIdentity;
            UbRtpTransportReference transportReference;
            uint32_t directTpn{UINT32_MAX};
            UbLifecycleLeaseIdentity workLease;
            bool rtpWqeSegmentTxActive{false};
            bool txEntryQueued{false};
            bool awaitingTpSegmentCleanup{false};
            bool dispatchReady{false};
            bool dispatchEventPending{false};
            EventId dispatchEvent;
            bool ctp{false};
            UbCtpEntityKey ctpKey;
            struct SegmentRecord
            {
                Ptr<UbWqeSegment> segment;
                UbLifecycleResourceIdentity identity;
                UbLifecycleLeaseIdentity childLease;
                UbLifecycleResourceIdentity resolvedTpIdentity;
                UbLifecycleLeaseIdentity transportWorkLease;
            };
            std::map<uint32_t, SegmentRecord> segments;
        };

        std::map<std::pair<uint32_t, uint32_t>, OperationRecord> m_operations;
        std::map<RtpTxQueueKey, RtpTxQueue> m_rtpTxQueues;
        std::vector<UbLifecycleResourceIdentity> m_tpgsBlockingJettyRetry;
        bool m_collectRtpResolutionCountersForTest{false};
        mutable uint64_t m_rtpResolutionCount{0};
        uint64_t m_rtpTpgMaterializationCount{0};
        UbTargetExecutor m_targetExecutor;
        Time m_targetExecutionDelay{NanoSeconds(0)};
        bool m_targetExecutionStarted{false};
    };

} // namespace ns3

#endif /* UB_TRANSACTION_H */
