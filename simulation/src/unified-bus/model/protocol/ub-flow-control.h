// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_FLOWCONTROL_H
#define UB_FLOWCONTROL_H

#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <string.h>
#include "ns3/ipv4-header.h"
#include "ns3/ipv4.h"
#include "ns3/udp-header.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/ub-transport.h"
#include "ns3/ub-link.h"
#include "ns3/packet.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/enum.h"
#include "ns3/ub-datalink.h"
#include "ns3/ub-datatype.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-routing-process.h"
#include "ns3/ub-queue-manager.h"

namespace ns3 {
class UbSwitch;
class UbFlowControl;
class UbIngressQueue;
class UbCbfc;
class UbPfc;
class UbPort;

/**
 * @brief 端口流量控制
 */
class UbFlowControl : public Object {
public:
    static TypeId GetTypeId(void);
    UbFlowControl() {}
    virtual ~UbFlowControl() {}
    virtual bool IsFcLimited(Ptr<UbIngressQueue> ingressQ)
    {
        return false;
    }
    virtual void HandleReleaseOccupiedFlowControl(Ptr<Packet> p, uint32_t inPortId, uint32_t outPortId) {}
    virtual void HandleSentPacket(Ptr<Packet> p, Ptr<UbIngressQueue> ingressQ) {}
    virtual void HandleReceivedControlPacket(Ptr<Packet> p) {}
    virtual void HandleReceivedPacket(Ptr<Packet> p) {}
    virtual FcType GetFcType()
    {
        return FcType::NONE;
    }
};

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
              uint8_t retCellGrainControlPacket, int32_t portTxfree,
              uint32_t nodeId, uint32_t portId);

    virtual bool IsFcLimited(Ptr<UbIngressQueue> ingressQ) override;
    virtual void HandleReleaseOccupiedFlowControl(Ptr<Packet> p,
                                                  uint32_t inPortId, uint32_t outPortId) override;
    virtual void HandleSentPacket(Ptr<Packet> p, Ptr<UbIngressQueue> ingressQ) override;
    virtual void HandleReceivedControlPacket(Ptr<Packet> p) override;
    virtual void HandleReceivedPacket(Ptr<Packet> p) override;
    int32_t GetCrdToReturn(uint8_t vlId);
    void SetCrdToReturn(uint8_t vlId, int32_t consumeCell, Ptr<UbPort> targetPort);
    void UpdateCrdToReturn(uint8_t vlId, int32_t consumeCell, Ptr<UbPort> targetPort);
    bool CbfcConsumeCrd(Ptr<Packet> p);
    bool CbfcRestoreCrd(Ptr<Packet> p);
    void SendCrdAck(Ptr<Packet> cbfcPkt, uint32_t targetPortId);
    Ptr<Packet> ReleaseOccupiedCrd(Ptr<Packet> p, uint32_t targetPortId);

protected:
    FcType m_fcType { FcType::CBFC };
    void DoDispose() override;
    uint32_t m_portId;
    uint32_t m_nodeId;

    /**
    * @brief cbfc相关参数配置
    */
    struct cbfcCfg_t {
        uint8_t  m_flitLen;                      // flit长度，默认 20 Bytes
        uint8_t  m_nFlitPerCell;                 // N值 {1, 2, 4, 8, 16, 32}
        uint8_t  m_retCellGrainDataPacket;       // 返回的CRD的粒度，通常从以下选项中选择 {1, 2, 4, 8, 16, 32, 64, 128}
        uint8_t  m_retCellGrainControlPacket;    // 返回的CRD的粒度，通常从以下选项中选择 {1, 2, 4, 8, 16, 32, 64, 128}
    };

    cbfcCfg_t *m_cbfcCfg;                   // cbfc相关配置
    std::vector<int32_t>  m_crdTxfree;      // 发送端口每个vl信用证
    std::vector<int32_t>  m_crdToReturn;    // 用于记录每个vl需要返回的信用证
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
              int32_t sharedInitCells, uint32_t nodeId, uint32_t portId);

    bool IsFcLimited(Ptr<UbIngressQueue> ingressQ) override;
    void HandleSentPacket(Ptr<Packet> p, Ptr<UbIngressQueue> ingressQ) override;
    void HandleReceivedControlPacket(Ptr<Packet> p) override;
    bool CbfcSharedConsumeCrd(Ptr<Packet> p);
    bool CbfcSharedRestoreCrd(Ptr<Packet> p);

private:
    int32_t m_shareCrd {0};
    int32_t m_reservedPerVlCells {0};
};


/**
 * @brief 端口Pfc
 */
class UbPfc : public UbFlowControl {
public:
    static TypeId GetTypeId (void);
    UbPfc() {}
    virtual ~UbPfc() {}
    virtual FcType GetFcType() override;
    
    void Init(int32_t portpfcUpThld, int32_t portpfcLowThld, uint32_t nodeId, uint32_t portId);
    
    virtual bool IsFcLimited(Ptr<UbIngressQueue> ingressQ) override;
    virtual void HandleReleaseOccupiedFlowControl(Ptr<Packet> p,
                                                  uint32_t inPortId, uint32_t outPortId) override;
    virtual void HandleSentPacket(Ptr<Packet> p, Ptr<UbIngressQueue> ingressQ) override;
    virtual void HandleReceivedControlPacket(Ptr<Packet> p) override;
    virtual void HandleReceivedPacket(Ptr<Packet> p) override;
    bool UpdatePfcStatus(Ptr<Packet> p);
    void SendPfc(Ptr<Packet> pfcPacket, uint32_t targetPortId);
    Ptr<Packet> CheckPfcThreshold(Ptr<Packet> p, uint32_t portId);
public:
    FcType m_fcType { FcType::PFC };
    uint32_t m_portId;
    uint32_t m_nodeId;
    void DoDispose() override;

    /**
    * @brief pfc水线参数配置
    */
    struct pfcCfg_t {
        int32_t                         m_portpfcUpThld;        // 缓冲阈值以生成PFC
        int32_t                         m_portpfcLowThld;       // 缓冲阈值以生成PFC
    };
    /**
    * @brief pfc端口状态
    */
    struct pfcStatus_t {
        std::vector<uint8_t>    m_portCredits;           // pfc状态, 每个端口各优先级pfc状态
        std::vector<uint8_t>    m_pfcSndCredits;         // pfc状态, 用于发送pfc报文
        std::vector<uint8_t>    m_pfcLastSndCredits;     // pfc状态, 用于发送pfc报文
        uint32_t                m_pfcSndCnt;             // 发送的pfc包统计
        uint32_t                m_pfcRcvCnt;             // 接收的pfc包统计
        pfcStatus_t(uint32_t totVlNum)
        {
            m_portCredits.resize(totVlNum, UB_CREDIT_MAX_VALUE);
            m_pfcSndCredits.resize(totVlNum, UB_CREDIT_MAX_VALUE);
            m_pfcLastSndCredits.resize(totVlNum, UB_CREDIT_MAX_VALUE);
            m_pfcSndCnt = 0;
            m_pfcRcvCnt = 0;
        }
    };
    pfcCfg_t *m_pfcCfg;         // pfc相关配置
    pfcStatus_t *m_pfcStatus;
};

} // namespace ns3

#endif // UB_FLOWCONTROL_H