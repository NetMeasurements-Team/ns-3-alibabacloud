// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_FUNCTION_H
#define UB_FUNCTION_H

#include "ns3/event-id.h"
#include "ns3/node.h"
#include <unordered_map>
#include "ns3/ub-sliding-bitmap-window.h"
#include "ub-transport.h"
#include "ns3/ub-datatype.h"
#include "ns3/ub-network-address.h"
#include "ns3/random-variable-stream.h"
#include "ub-ldst-api.h"
#include "ub-transaction.h"
#include "ub-transaction-rules.h"

namespace ns3 {
    class UbController;
    class UbCtpTransportService;
    class UbJettyManager;
    enum class UbJettyLifecycleState : uint8_t;

    enum class UbJettyLifetime : uint8_t
    {
        REUSABLE = 0,
        TASK_SCOPED = 1,
    };

    enum class UbWqeSubmissionStatus : uint8_t
    {
        ACCEPTED = 0,
        JETTY_NOT_FOUND = 1,
        JETTY_BINDING_MISMATCH = 2,
        NO_DIRECT_TRANSPORT = 3,
        DUPLICATE_OPERATION = 4,
        LEGACY_MULTI_TRANSPORT = 5,
        JETTY_DESTROYING = 6,
        STALE_TRANSPORT_REFERENCE = 7,
        TPG_NOT_ACCEPTING = 8,
        TPG_NO_MEMBER = 9,
        INVALID_ATOMIC_REQUEST = 10,
        INVALID_TRANSACTION_REQUEST = 11,
        CTP_NOT_PREPARED = 12,
        INVALID_CTP_WINDOW = 13,
        CTP_SEQUENCE_EXHAUSTED = 14,
    };

    struct UbWqeSubmissionResult
    {
        UbWqeSubmissionStatus status{UbWqeSubmissionStatus::JETTY_NOT_FOUND};
        UbLifecycleResourceIdentity wqeIdentity;
        UbTransactionValidationResult validation;
    };

    struct UbWqeCompletion
    {
        uint32_t jettyNum{0};
        uint32_t wqeId{0};
        UbWorkOutcome outcome{UbWorkOutcome::COMPLETED};
    };

    struct UbJettyStorageSnapshot
    {
        uint64_t taskScopedJetties{0};
        uint64_t reusableJetties{0};
    };
    /**
    * @brief 管理jetty
    */
    class UbFunction : public Object {
    public:

        static TypeId GetTypeId(void);

        UbFunction();
        virtual ~UbFunction();
        Ptr<UbLdstApi> GetUbLdstApi();

        // Jetty management
        /**
         * @brief Create a new jetty
         * @param src Source identifier
         * @param dest Destination identifier
         * @param jettyNum Jetty identifier defined by user
         * @return Pointer to created jetty
         */
        void CreateJetty(uint32_t src,
                         uint32_t dest,
                         uint32_t jettyNum,
                         UbJettyLifetime lifetime = UbJettyLifetime::REUSABLE);
        void CreateJetty(uint32_t src,
                         uint32_t jettyNum,
                         UbJettyLifetime lifetime = UbJettyLifetime::REUSABLE);

        void Init(uint32_t nodeId);
        void BindTransaction(Ptr<UbTransaction> transaction);

        Ptr<UbTransaction> GetUbTransaction();

        bool IsJettyExists(uint32_t jettyNum);

        Ptr<UbJetty> GetJetty(uint32_t jettyNum);

        /**
         * @brief Destroy jetty by parameters
         * @param jettyNum Jetty identifier
         */
        UbDestroyStatus DestroyJetty(uint32_t jettyNum,
                                     UbDestroyReason reason = UbDestroyReason::EXPLICIT,
                                     UbDestroyCallback callback = {});

        /**
         * @brief Assemble wqe
         * @param Client传入URMA任务
         */
        Ptr<UbWqe> CreateWqe(uint32_t src,
                             uint32_t dest,
                             uint32_t size,
                             uint32_t wqeId,
                             UbTransactionOpcode type);

        Ptr<UbWqe> CreateWqe(uint32_t src, uint32_t dest, uint32_t size, uint32_t wqeId);

        /**
         * @brief Assemble wqe and push back in Jetty
         * @param Client传入URMA任务
         */
        void PushWqeToJetty(Ptr<UbWqe> wqe, uint32_t jettyNum);
        UbWqeSubmissionResult SubmitWqe(Ptr<UbWqe> wqe, uint32_t jettyNum);
        UbWqeSubmissionResult SubmitCtpWqe(Ptr<UbWqe> wqe,
                                           uint32_t jettyNum,
                                           const UbCtpEntityKey& key);
        void ApplyWqeOutcomeFromTransaction(uint32_t jettyNum,
                                            uint32_t wqeId,
                                            const UbLifecycleResourceIdentity& wqeIdentity,
                                            UbWorkOutcome outcome);
        void NotifyJettyProgress(uint32_t jettyNum);
        bool IsJettyAcceptingWqes(uint32_t jettyNum) const;
        bool ShouldContinueAcceptedWqes(uint32_t jettyNum) const;
        bool ShouldForceDrainAcceptedWqes(uint32_t jettyNum) const;
        uint64_t TrackJettyEvent(uint32_t jettyNum);
        void AttachJettyEvent(uint32_t jettyNum,
                              uint64_t token,
                              EventId event,
                              Callback<void> cancelCleanup = {});
        bool BeginJettyEvent(uint32_t jettyNum, uint64_t token);
        void ResolveJettyEvent(uint32_t jettyNum, uint64_t token);
        bool TryGetJettyLifecycleIdentity(uint32_t jettyNum,
                                          UbLifecycleResourceIdentity& identity) const;
        bool TryGetJettyState(uint32_t jettyNum, UbJettyLifecycleState& state) const;
        bool TryGetJettyLifetime(uint32_t jettyNum, UbJettyLifetime& lifetime) const;
        bool IsJettyDrainReady(uint32_t jettyNum) const;
        uint64_t GetResidentWqeCount() const;
        UbJettyStorageSnapshot GetJettyStorageSnapshot() const;
        void RegisterJettyRtpBinding(
            uint32_t jettyNum,
            const UbLifecycleResourceIdentity& transportIdentity);
        void RegisterJettyCtpBinding(uint32_t jettyNum,
                                     Ptr<UbCtpTransportService> service);
        bool UnregisterJettyCtpBinding(uint32_t jettyNum,
                                       Ptr<UbCtpTransportService> service);
    private:
        void DoDispose() override;
        bool ValidateAtomicWqe(Ptr<UbWqe> wqe,
                               uint32_t jettyNum,
                               const UbTransactionResolved& resolved);
        Ptr<UbTransaction> GetTransaction();
        Ptr<UbLdstApi> m_ldstApi;
        uint32_t m_nodeId;
        Ptr<UbTransaction> m_transaction;
        Ptr<UbJettyManager> m_jettyManager;

    };

    enum class UbJettyLifecycleState : uint8_t
    {
        ACTIVE = 0,
        DESTROY_REQUESTED = 1,
        DRAINING = 2,
    };

    class UbJettyManager : public Object
    {
      public:
        static TypeId GetTypeId();

        void Init(uint32_t nodeId, Ptr<UbLifecycleCoordinator> coordinator);
        void BindTransaction(Ptr<UbTransaction> transaction);
        void RegisterJetty(Ptr<UbJetty> jetty, UbJettyLifetime lifetime);
        bool IsJettyExists(uint32_t jettyNum) const;
        Ptr<UbJetty> GetJetty(uint32_t jettyNum) const;
        bool IsAcceptingWqes(uint32_t jettyNum) const;
        bool ShouldContinueAcceptedWqes(uint32_t jettyNum) const;
        bool ShouldForceDrainAcceptedWqes(uint32_t jettyNum) const;
        bool TryGetState(uint32_t jettyNum, UbJettyLifecycleState& state) const;
        bool TryGetLifetime(uint32_t jettyNum, UbJettyLifetime& lifetime) const;
        void RegisterWqe(uint32_t jettyNum,
                         uint32_t wqeId,
                         const UbLifecycleResourceIdentity& identity);
        uint64_t ResolveWqeLifecycleLocalId(uint32_t jettyNum, uint32_t wqeId) const;
        void ApplyWqeOutcome(uint32_t jettyNum,
                             uint32_t wqeId,
                             const UbLifecycleResourceIdentity& identity,
                             UbWorkOutcome outcome);
        UbDestroyStatus DestroyJetty(uint32_t jettyNum,
                                     UbDestroyReason reason,
                                     UbDestroyCallback callback);
        void TryFinalizeJettyDestroy(uint32_t jettyNum);
        bool IsDrainReady(uint32_t jettyNum) const;
        uint64_t GetResidentWqeCount() const;
        UbJettyStorageSnapshot GetStorageSnapshot() const;
        uint64_t TrackEvent(uint32_t jettyNum);
        void AttachEvent(uint32_t jettyNum,
                         uint64_t token,
                         EventId event,
                         Callback<void> cancelCleanup);
        bool BeginEvent(uint32_t jettyNum, uint64_t token);
        void ResolveEvent(uint32_t jettyNum, uint64_t token);
        bool TryGetIdentity(uint32_t jettyNum, UbLifecycleResourceIdentity& identity) const;
        void ReplaceRtpBinding(
            uint32_t jettyNum,
            const UbLifecycleResourceIdentity& transportIdentity);
        void RegisterCtpBinding(uint32_t jettyNum, Ptr<UbCtpTransportService> service);
        bool UnregisterCtpBinding(uint32_t jettyNum, Ptr<UbCtpTransportService> service);
        void DisposeAll();

      private:
        enum class CleanupMode : uint8_t
        {
            GRACEFUL = 0,
            ABORT = 1,
            FORCE = 2,
        };

        struct JettyRecord
        {
            Ptr<UbJetty> jetty;
            UbLifecycleResourceIdentity identity;
            UbJettyLifecycleState state{UbJettyLifecycleState::ACTIVE};
            UbJettyLifetime lifetime{UbJettyLifetime::REUSABLE};
            UbDestroyReason reason{UbDestroyReason::EXPLICIT};
            CleanupMode cleanupMode{CleanupMode::GRACEFUL};
            std::map<uint32_t, UbLifecycleResourceIdentity> wqes;
            bool hasCtpBinding{false};
            std::vector<UbDestroyCallback> callbacks;
        };

        Ptr<UbTransaction> GetTransaction() const;
        static CleanupMode GetCleanupMode(UbDestroyReason reason);
        bool IsDrainReady(const JettyRecord& record) const;
        void FinalizeDestroy(uint32_t jettyNum);
        void DoDispose() override;

        uint32_t m_nodeId{UINT32_MAX};
        Ptr<UbLifecycleCoordinator> m_coordinator;
        Ptr<UbTransaction> m_transaction;
        Ptr<UbCtpTransportService> m_ctpService;
        UbTrackedEventSet m_jettyEvents;
        std::map<uint32_t, JettyRecord> m_jettyRecordsByNum;
    };

    /**
     * @brief Jetty 数据结构
     *
     * 表示一个逻辑传输通道，负责管理特定源-目的对之间的WQE队列和传输状态。
     * 每个Jetty有唯一的标识符，并维护相关的传输参数和回调函数。
     */
    class UbJetty : public Object {
    public:
        static TypeId GetTypeId(void);
        UbJetty();
        Ptr<UbWqeSegment> GetNextWqeSegment();
        Ptr<UbWqeSegment> GetNextWqeSegment(uint32_t maxSegmentBytes);
        Ptr<UbWqeSegment> PrepareNextWqeSegment(Ptr<UbWqe> expectedWqe = nullptr);
        Ptr<UbWqeSegment> PrepareNextWqeSegment(Ptr<UbWqe> expectedWqe,
                                                uint32_t maxSegmentBytes);
        bool CanCommitWqeSegmentAdmission(Ptr<UbWqeSegment> segment,
                                          Ptr<UbWqe> expectedWqe) const;
        void CommitWqeSegmentAdmission(Ptr<UbWqeSegment> segment);
        void AdvanceTaSsnForWqeCleanup(const std::vector<uint32_t>& taSsns);

        [[deprecated("use AdvanceTaSsnForWqeCleanup")]] void ApplyWqeSegmentTerminalProgress(
            const std::vector<uint32_t>& taSsns)
        {
            AdvanceTaSsnForWqeCleanup(taSsns);
        }
        bool ProcessWqeSegmentComplete(uint32_t taSsnFin);
        void RightShiftBitset(uint32_t shiftCount);
        bool SetTaSegmentBytes(uint32_t bytes);
        bool SetInflightMax(uint32_t value);
        uint32_t GetInflightMax() const { return m_inflightMax; }
        bool SetOooAckThreshold(uint32_t value);
        uint32_t GetOooAckThreshold() const;
        bool CanReserveCtpWqe(Ptr<UbWqe> wqe, uint32_t maxSegmentBytes) const;
        uint32_t GetTaSsnSndUna() const { return m_taSsnSndUna; }
        uint32_t GetTaSsnSndNxt() const { return m_taSsnSndNxt; }
        uint32_t GetTaSegmentBytes() const;

        Ptr<UbWqeSegment> GenWqeSegment(Ptr<UbWqe> wqe, uint32_t segment_size);

        void Init();

        void IncreaseTaSsnSndNxt()
        {
            NS_ABORT_MSG_IF(IsCtpRequester() && m_taSsnSndNxt == UINT32_MAX,
                            "CTP exclusive sequence sentinel cannot be assigned");
            m_taSsnSndNxt++;
        }

        uint32_t PeekNextTaSsn() const
        {
            return m_taSsnSndNxt;
        }

        void PushWqe(Ptr<UbWqe> ubQqe);
        void PushWqe(Ptr<UbWqe> ubQqe, uint32_t maxSegmentBytes);

        uint32_t GetJettyNum() const
        {
            return m_jettyNum;
        }

        void SetJettyNum(const uint32_t jettyNum)
        {
            m_jettyNum = jettyNum;
        }

        bool IsValid(); // Jetty是否可用
        bool IsReadyToSend();
        bool IsIdle() const;

        uint32_t GetSrc() const
        {
            return m_src;
        }

        uint32_t GetDest() const
        {
            return m_dest;
        }

        bool HasBoundDest() const
        {
            return m_dest != UINT32_MAX;
        }

        uint8_t GetSport() const
        {
            return m_sport;
        }

        uint8_t GetDport() const
        {
            return m_dport;
        }

        bool HasValidCtpWindow() const
        {
            return m_inflightMax > 0 && m_inflightMax <= 32768 &&
                   m_inflightMax <= m_ssnAckWindow.GetWindowSize();
        }

        bool IsLimited()
        {
            return (m_taSsnSndNxt - m_taSsnSndUna) >= m_inflightMax;
        }

        void SetSrc(uint32_t src)
        {
            m_src = src;
        }

        void SetDest(uint32_t dest)
        {
            m_dest = dest;
        }

        void ClearDest()
        {
            m_dest = UINT32_MAX;
        }

        bool CanAcceptWqe(Ptr<UbWqe> wqe) const;

        void SetSport(uint8_t sport)
        {
            m_sport = sport;
        }

        void SetDport(uint8_t dport)
        {
            m_dport = dport;
        }

        bool ResetSsnAckBitset(uint32_t taOooAckThreshold);

        void SetClientCallback(Callback<void, uint32_t, uint32_t> cb);
        void SetCompletionObserver(Callback<void, uint32_t, uint32_t> cb);
        void SetWqeCompletionCallback(Callback<void, UbWqeCompletion> cb);
        void ApplyWqeTerminal(uint32_t wqeId, UbWorkOutcome outcome);
        void FlushLegacyWqesForDisposal();
        Callback<void, uint32_t, uint32_t> FinishCallback;

        void SetNodeId(uint32_t nodeId) {m_nodeId = nodeId; }
        void SetTransaction(UbTransaction* transaction)
        {
            m_transaction = transaction;
        }

        uint32_t GetNodeId() { return m_nodeId; }

        void SetTaSsnSendWindowForTest(uint32_t sndUna, uint32_t sndNxt)
        {
            NS_ABORT_MSG_IF(sndNxt < sndUna || !m_wqeVector.empty(),
                            "Test cursor positioning requires an empty Jetty and ordered bounds");
            m_taSsnSndUna = sndUna;
            m_taSsnSndNxt = sndNxt;
            m_taSsnCnt = sndNxt;
            m_ssnAckWindow.Reset(m_taSsnSndUna);
        }

        uint32_t GetTaSsnSndUnaForTest() const
        {
            return m_taSsnSndUna;
        }

        uint32_t GetTaSsnSndNxtForTest() const
        {
            return PeekNextTaSsn();
        }

        uint32_t GetActiveWqeCount() const
        {
            return static_cast<uint32_t>(m_wqeVector.size());
        }

    private:
        UbTransaction* GetTransaction();
        void DoDispose() override;
        std::vector<Ptr<UbWqe>> m_wqeVector;
        // ========== Jetty标识信息 ==========
        uint32_t m_jettyNum; // JettyNum UB协议报文头携带（24位）用于标识

        bool IsCtpRequester() const;
        bool CanChangeCtpParameters() const;

        // ========== 传输参数 ==========
        uint32_t m_nodeId;
        UbTransaction* m_transaction{nullptr};
        uint32_t m_src;  // 源节点标识符
        uint32_t m_dest{UINT32_MAX}; // 目的节点标识符；UINT32_MAX 表示 unbound Jetty
        uint8_t m_sport;
        uint8_t m_dport;
        uint32_t m_inflightMax = 512;
        uint32_t m_taSegmentBytes = UB_WQE_TA_SEGMENT_BYTE;

        // ========== Jetty队列信息 ==========
        // 当前只在WQE推入jetty中才会更新
        uint64_t m_taMsnCnt = 0; // 当前jetty层WQE计数器，数字上等于目前已经从client拿到的所有WQE数量

        // 当前jetty WQE Segment计数器，数字上等于目前已经从client拿到的所有WQE产生的WQE segment数量
        uint32_t m_taSsnCnt = 0;

        uint32_t m_oooAckThreshold{2048}; // out of order ACK threshold
        uint32_t m_taSsnSndNxt = 0;                                 // TA层下一个待发送的分段序号
        uint32_t m_taSsnSndUna = 0;                                 // TA层未确认的最小分段序号
        UbSlidingBitmapWindow m_ssnAckWindow{UB_JETTY_TASSN_OOO_THRESHOLD}; // 接收维护的bitmap
        Callback<void, uint32_t, uint32_t> m_completionObserver;
        Callback<void, UbWqeCompletion> m_wqeCompletionCallback;
        // 每个jetty统一编号，单调升，作为WqeSegment的编号统计
        //   |----ssn5----|------ssn4-------|------ssn3----->
        //   |----ssn2----|----ssn1---|---startssn0----->
        //                               taSsnNxt
        //                                  taSsnUna
        // wqe.burst_size = 64*1024
        // m_messages = m_wqeVector
        // WQE0 = WqeSegment0 + WqeSegment1 + WqeSegment2
        // WQE1 = WqeSegment3 + WqeSegment4 + ...

        /**
         * @brief 检查并移除已完成的WQE
         */
        void CheckAndRemoveCompletedWqe();

        /**
         * @brief 检查指定WQE是否已完成
         * @param wqe 要检查的WQE
         * @return true如果WQE已完成，false否则
         */
        bool IsWqeCompleted(Ptr<UbWqe> wqe);
    };

} // namespace ns3

#endif /* UB_FUNCTION_H */
