// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_PFC_H
#define UB_PFC_H

#include "ns3/ub-flow-control.h"

namespace ns3 {
struct UbPfcDecision {
    bool pause {false};
    bool resume {false};
    uint64_t ingress_total_bytes {0};
    uint64_t ingress_shared_bytes {0};
    uint64_t ingress_headroom_bytes {0};
    uint64_t switch_shared_pool_used_bytes {0};
    uint64_t switch_total_buffered_bytes {0};
    uint64_t xoff_threshold_bytes {0};
    uint64_t xon_threshold_bytes {0};
};

class UbPfcDecisionHook : public Object {
public:
    static TypeId GetTypeId(void);
    ~UbPfcDecisionHook() override = default;

    virtual UbPfcDecision Evaluate(Ptr<UbQueueManager> queueManager,
                                   uint32_t inPort,
                                   uint32_t priority) const = 0;
};

class UbPfcFixedDecisionHook : public UbPfcDecisionHook {
public:
    static TypeId GetTypeId(void);
    void Init(uint64_t xoffThresholdBytes, uint64_t xonThresholdBytes);
    UbPfcDecision Evaluate(Ptr<UbQueueManager> queueManager,
                           uint32_t inPort,
                           uint32_t priority) const override;

private:
    uint64_t m_xoffThresholdBytes {0};
    uint64_t m_xonThresholdBytes {0};
};

class UbPfcDynamicDecisionHook : public UbPfcDecisionHook {
public:
    static TypeId GetTypeId(void);
    UbPfcDecision Evaluate(Ptr<UbQueueManager> queueManager,
                           uint32_t inPort,
                           uint32_t priority) const override;
};

// Paper dynamic PFC mode for reproducing the threshold rule used with DCQCN in
// "Congestion Control for Large-Scale RDMA Deployments" (SIGCOMM 2015).
class UbPfcPaperDynamicDecisionHook : public UbPfcDecisionHook {
public:
    static TypeId GetTypeId(void);
    UbPfcDecision Evaluate(Ptr<UbQueueManager> queueManager,
                           uint32_t inPort,
                           uint32_t priority) const override;
};

/**
 * @brief 端口Pfc
 */
class UbPfc : public UbFlowControl {
public:
    static TypeId GetTypeId (void);
    UbPfc() = default;
    ~UbPfc() override = default;
    FcType GetFcType() override;

    void Init(FcType mode, int32_t portpfcUpThld, int32_t portpfcLowThld, uint32_t nodeId, uint32_t portId);

    bool IsFcLimited(Ptr<UbIngressQueue> ingressQ) override;
    void OnIngressReleased(const UbFlowControlEventContext& context) override;
    void OnEgressEnqueued(const UbFlowControlEventContext& context) override;
    void OnControlFrameReceived(Ptr<Packet> p) override;
    void OnIngressEnqueued(const UbFlowControlEventContext& context) override;
    void SendPfc(Ptr<Packet> pfcPacket, uint32_t targetPortId);
    Ptr<Packet> CheckPfcThreshold(Ptr<Packet> p, uint32_t portId);
    void DoDispose() override;

    FcType m_fcType { FcType::PFC_FIXED };
    uint32_t m_portId;
    uint32_t m_nodeId;

    /**
    * @brief pfc水线参数配置
    */
    struct pfcCfg_t {
        int32_t                         m_portpfcUpThld;        // 缓冲阈值以生成PFC
        int32_t                         m_portpfcLowThld;       // 缓冲阈值以生成PFC
    } m_pfcCfg {};
    /**
    * @brief pfc端口状态
    */
    struct pfcStatus_t {
        std::vector<uint8_t>    m_portCredits;           // pfc状态, 每个端口各优先级pfc状态
        std::vector<uint8_t>    m_pfcSndCredits;         // pfc状态, 用于发送pfc报文
        std::vector<uint8_t>    m_pfcLastSndCredits;     // pfc状态, 用于发送pfc报文
        std::vector<uint8_t>    m_pfcDynamicLastTracePause; // dynamic PFC 上次已记录的pause状态
        uint32_t                m_pfcSndCnt;             // 发送的pfc包统计
        uint32_t                m_pfcRcvCnt;             // 接收的pfc包统计
        pfcStatus_t(uint32_t totVlNum)
        {
            m_portCredits.resize(totVlNum, UB_CREDIT_MAX_VALUE);
            m_pfcSndCredits.resize(totVlNum, UB_CREDIT_MAX_VALUE);
            m_pfcLastSndCredits.resize(totVlNum, UB_CREDIT_MAX_VALUE);
            m_pfcDynamicLastTracePause.resize(totVlNum, 2);
            m_pfcSndCnt = 0;
            m_pfcRcvCnt = 0;
        }
    } m_pfcStatus {0};
    Ptr<UbPfcDecisionHook> m_decisionHook;

private:
    bool UpdatePfcStatus(Ptr<Packet> p);
    void InitRuntimeState(FcType mode,
                          int32_t portpfcUpThld,
                          int32_t portpfcLowThld,
                          uint32_t nodeId,
                          uint32_t portId,
                          uint32_t vlNum);
    void BindDecisionHook(Ptr<UbQueueManager> queueManager);
    void BindFixedDecisionHook();
    void BindDynamicDecisionHook(Ptr<UbQueueManager> queueManager);
    void BindPaperDynamicDecisionHook(Ptr<UbQueueManager> queueManager);
};

} // namespace ns3

#endif // UB_PFC_H
