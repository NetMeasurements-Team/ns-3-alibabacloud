// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_CAQM_H
#define UB_CAQM_H
#include <unordered_map>
#include <vector>
#include <time.h>
#include <random>
#include "ns3/ub-congestion-control.h"
#include "ns3/ptr.h"
#include "ns3/packet.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-header.h"
#include "ns3/simulator.h"
#include "ns3/data-rate.h"
#include "ns3/random-variable-stream.h"
namespace ns3 {
class UbCongestionExtTph;
class UbNetworkHeader;
class UbSwitch;
class UbTransportChannel;

/**
 * @brief Caqm algo, inherit from UbCongestionControl,
 * define some params that caqm algo used.
 */
class UbCaqm : public UbCongestionControl {
public:
    UbCaqm();
    ~UbCaqm() override;
    static TypeId GetTypeId(void);

protected:
    UbNodeType_t m_nodeType;    // 所属节点的类型

    double m_alpha;             // 拥塞避免阶段增窗系数，α / cwnd，
    double m_beta;              // 减窗系数，β * mtu，
    double m_gamma;             // 窗口下限系数， γ * mtu
    uint32_t m_theta;           // 状态重置时间系数，θ * rtt时间内没有收到阻塞 or 拒绝增窗ack，状态重置为慢启动
    double m_lambda;            // CC计算系数

    uint32_t m_idealQueueSize;  // 期望的最大队列size
    uint32_t m_ccUnit;          // 慢启动阶段一个hint表示的字节数
    double m_markProbability;   // CC大于零但不足时包被标记的概率
};

/**
 * @brief Caqm algo host part.
 */
class UbHostCaqm : public UbCaqm {
public:
    UbHostCaqm();
    ~UbHostCaqm() override;
    static TypeId GetTypeId(void);

    // 初始化
    void TpInit(Ptr<UbTransportChannel> tp) override;

    // 获取剩余窗口，CAQM LDCP需要
    uint32_t GetRestCwnd() override;

    // 发送端生成拥塞控制算法需要的header
    UbNetworkHeader SenderGenNetworkHeader() override;

    // 发送端发包，更新数据
    void SenderUpdateCongestionCtrlData(uint32_t psn, uint32_t size) override;

    // 接收端接到数据包后记录数据
    void RecverRecordPacketData(uint32_t psn, uint32_t size, UbNetworkHeader header) override;

    // 接收端生成拥塞控制算法需要的ack header
    UbCongestionExtTph RecverGenAckCeTphHeader(uint32_t psnStart, uint32_t psnEnd) override;

    // 发送端收到ack，调整窗口、速率等数据
    void SenderRecvAck(uint32_t psn, UbCongestionExtTph header) override;

private:
    void StateReset();

    void DoDispose() override;

    uint32_t m_src;
    uint32_t m_dst;
    uint32_t m_tpn;

    enum CongestionState {
        SLOW_START,
        CONGESTION_AVOIDANCE
    };
    CongestionState m_congestionState = SLOW_START;

    uint32_t    m_dataByteSent = 0;     // 总计发送数据量
    uint32_t    m_dataByteRecvd = 0;    // 总计收到数据量
    uint32_t    m_inFlight = 0;         // 已发送但还没有收到ack的数据量
    uint32_t    m_cwnd;                 // 发送窗口大小

    uint32_t    m_lastSequence = 0;

    std::unordered_map<uint32_t, uint32_t> m_recvdPsnPacketSizeMap;
    std::unordered_map<uint32_t, uint16_t> m_recvdPsnHintMap;
    std::unordered_map<uint32_t, uint8_t> m_recvdPsnCMap;
    std::unordered_map<uint32_t, uint8_t> m_recvdPsnIMap;

    std::unordered_map<uint32_t, Time> m_psnSendTimeMap;

    Time m_rtt = NanoSeconds(0);
    EventId m_congestionStateResetEvent{};

    uint16_t m_HintE = 0;           // 聚合hint
    uint8_t m_CE = 0;               // 聚合ce
    uint8_t m_IE = 0;               // 聚合ie
    double m_accumulateHint = 0;    // 累计的增窗请求之和，大于1时才会将hint设置数字
};

/**
 * @brief Caqm algo switch part.
 */
class UbSwitchCaqm : public UbCaqm {
public:
    static TypeId GetTypeId(void);
    UbSwitchCaqm();
    ~UbSwitchCaqm() override;
    // 初始化
    void SwitchInit(Ptr<UbSwitch> sw) override;

    // 设置每个端口的带宽
    void SetDataRate(uint32_t portId, DataRate bps);

    // 交换机收到包进行转发，对其进行处理
    void SwitchForwardPacket(uint32_t inPort, uint32_t outPort, Ptr<Packet> p) override;

    // switch自动更新cc
    void ResetLocalCc();

private:

    void DoDispose() override;

    Time m_ccUpdatePeriod;                      // 交换机自动更新CC的周期
    std::vector<int64_t> m_cc;                  // Credit Counter，端口可用信用证的数量，代表端口空闲转发能力
    std::vector<uint64_t> m_txSize ;            // 实际吞吐量
    std::vector<int64_t> m_DC;                  // Deficit Counter，赤字计数器
    std::vector<int64_t> m_creditAllocated;     // 上一次循环中分配除去的信用证
    std::vector<DataRate> m_bps;                // port带宽

    uint32_t m_nodeId;                          // 绑定的switch节点号

    Ptr<UniformRandomVariable> m_random;        // 随机数产生工具，伪随机，多次仿真可复现
};
}
#endif