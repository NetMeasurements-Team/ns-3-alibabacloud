// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_CBFC_H
#define UB_CBFC_H

#include "ns3/ub-flow-control.h"

namespace ns3 {
/**
 * @brief 端口Cbfc
 */
class UbCbfc : public UbFlowControl {
public:
    static TypeId GetTypeId (void);
    UbCbfc() {}
    virtual ~UbCbfc() {}
    virtual FcType GetFcType() override;

    void Init(uint8_t flitLen, uint8_t nFlitPerCell, uint8_t retCellGrainDataPacket,
              uint8_t retCellGrainControlPacket, int32_t portTxfree, int32_t ctrlCrdRtrThldCells,
              uint32_t nodeId, uint32_t portId);

    virtual bool IsFcLimited(Ptr<UbIngressQueue> ingressQ) override;
    virtual void OnIngressReleased(const UbFlowControlEventContext& context) override;
    virtual void OnEgressEnqueued(const UbFlowControlEventContext& context) override;
    virtual void OnControlFrameReceived(Ptr<Packet> p) override;
    virtual void OnDataPacketReceived(Ptr<Packet> p) override;
    virtual void OnIngressEnqueued(const UbFlowControlEventContext& context) override;
    int32_t GetCrdToReturn(uint8_t vlId);
    void SetCrdToReturn(uint8_t vlId, int32_t consumeCell, Ptr<UbPort> targetPort);
    void UpdateCrdToReturn(uint8_t vlId, int32_t consumeCell, Ptr<UbPort> targetPort);
    bool CbfcConsumeCrd(Ptr<Packet> p);
    void SendCrdAck(Ptr<Packet> cbfcPkt, uint32_t targetPortId);

protected:
    void ControlCreditRestoreNotify(uint32_t nodeId,
                                    uint32_t portId,
                                    const std::vector<uint8_t>& credits);
    FcType m_fcType { FcType::CBFC };
    void DoDispose() override;
    uint32_t m_portId;
    uint32_t m_nodeId;
    TracedCallback<uint32_t, uint32_t, std::vector<uint8_t>> m_traceControlCreditRestoreNotify;

    /**
    * @brief cbfc相关参数配置
    */
    struct cbfcCfg_t {
        uint8_t  m_flitLen;                      // flit长度，默认 20 Bytes
        uint8_t  m_nFlitPerCell;                 // N值 {1, 2, 4, 8, 16, 32}
        uint8_t  m_retCellGrainDataPacket;       // 返回的CRD的粒度，通常从以下选项中选择 {1, 2, 4, 8, 16, 32, 64, 128}
        uint8_t  m_retCellGrainControlPacket;    // 返回的CRD的粒度，通常从以下选项中选择 {1, 2, 4, 8, 16, 32, 64, 128}
        int32_t  m_ctrlCrdRtrThldCells;          // 触发 control-frame credit return 的 per-VL pending-return 阈值（单位：cell）
    } m_cbfcCfg {};

    std::vector<int32_t>  m_crdTxfree;      // 发送端口每个vl信用证
    std::vector<int32_t>  m_crdToReturn;    // 用于记录每个vl需要返回的信用证
    std::vector<bool> m_creditBlockedLast;  // 上一次是否因 credit 不足而阻塞
    uint8_t m_lastPiggybackVl {0};

protected:
    bool TryAttachPiggybackCredit(Ptr<Packet> p);
    bool ShouldForceControlReturn() const;
    void AccumulateReturnedCredit(Ptr<Packet> p, uint32_t targetPortId);
    Ptr<Packet> BuildControlReturnPacket(uint32_t targetPortId,
                                         int32_t minPendingCells,
                                         const std::string& reason);
    Ptr<Packet> MaybeBuildControlReturnPacket(uint32_t targetPortId);
    void MaybeQueueControlReturn(uint32_t targetPortId);
    static constexpr uint8_t kMaxCtrlCreditGrainsPerFrame = 63;

private:
    bool CbfcRestoreCrd(Ptr<Packet> p);
    bool RestoreDataPacketCredit(Ptr<Packet> p);
    uint8_t SelectPiggybackCreditVl() const;
};


/**
 * @brief 端口Cbfc (Shared credit)
 */
class UbCbfcSharedCredit : public UbCbfc {
public:
    static TypeId GetTypeId (void);
    UbCbfcSharedCredit() {}
    ~UbCbfcSharedCredit() override {}

    FcType GetFcType() override;

    void Init(uint8_t flitLen, uint8_t nFlitPerCell, uint8_t retCellGrainDataPacket,
              uint8_t retCellGrainControlPacket, int32_t reservedPerVlCells,
              int32_t ctrlCrdRtrThldCells,
              int32_t sharedInitCells, uint32_t nodeId, uint32_t portId);

    bool IsFcLimited(Ptr<UbIngressQueue> ingressQ) override;
    void OnEgressEnqueued(const UbFlowControlEventContext& context) override;
    void OnControlFrameReceived(Ptr<Packet> p) override;
    void OnDataPacketReceived(Ptr<Packet> p) override;

private:
    bool CbfcSharedConsumeCrd(Ptr<Packet> p);
    bool CbfcSharedRestoreCrd(Ptr<Packet> p);
    bool RestoreSharedDataPacketCredit(Ptr<Packet> p);
    int32_t m_shareCrd {0};
    int32_t m_reservedPerVlCells {0};
};

} // namespace ns3

#endif // UB_CBFC_H
