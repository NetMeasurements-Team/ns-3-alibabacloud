// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_CONGESTION_CTRL_H
#define UB_CONGESTION_CTRL_H

#include <stdexcept>
#include "ns3/object.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-header.h"
#include "ns3/ub-datatype.h"
namespace ns3 {

class UbTransportChannel;

// Currently only CAQM is implemented, other algorithms to be added
enum CongestionCtrlAlgo {
    CAQM,
    LDCP,
    DCQCN
};

/**
 * @brief UB Congestion control parent class.
 * Only Caqm can be used now.
 */
class UbCongestionControl : public Object {
public:
    static TypeId GetTypeId();
    UbCongestionControl();
    ~UbCongestionControl() override;

    CongestionCtrlAlgo GetCongestionAlgo() {return m_algoType;}

    // 获取剩余窗口，CAQM LDCP需要
    virtual uint32_t GetRestCwnd() {return UB_MTU_BYTE;}

    // 发送端生成networkHeader包头
    virtual UbNetworkHeader SenderGenNetworkHeader()
    {
        throw std::runtime_error("Congestion Ctrl not available");
    }

    // 发送端发包，更新数据
    virtual void SenderUpdateCongestionCtrlData(uint32_t psn, uint32_t size) {}

    // 交换机收到包进行转发，对其进行处理
    virtual void SwitchForwardPacket(uint32_t inPort, uint32_t outPort, Ptr<Packet> p) {}

    // 接收端接到数据包后记录数据
    virtual void RecverRecordPacketData(uint32_t psn, uint32_t size, UbNetworkHeader header) {}

    // 接收端生成ack的cetph头
    virtual UbCongestionExtTph RecverGenAckCeTphHeader(uint32_t psnStart, uint32_t psnEnd)
    {
        throw std::runtime_error("Congestion Ctrl not available");
    }

    // 发送端收到ack，调整窗口、速率等数据
    virtual void SenderRecvAck(uint32_t psn, UbCongestionExtTph header) {}

    // 绑定switch，初始化参数
    virtual void SwitchInit(Ptr<UbSwitch> sw) {}

    // 绑定tp，初始化参数
    virtual void TpInit(Ptr<UbTransportChannel> tp) {}

    static Ptr<UbCongestionControl> Create(UbNodeType_t nodeType);

    // 根据caqm使能情况获取
    TpOpcode GetTpAckOpcode()
    {
        if (m_congestionCtrlEnabled)
            return TpOpcode::TP_OPCODE_ACK_WITH_CETPH;
        else
            return TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH;
    };

protected:
    CongestionCtrlAlgo m_algoType;  // 拥塞控制算法类型
    bool m_congestionCtrlEnabled;   // 开关
};

}

#endif