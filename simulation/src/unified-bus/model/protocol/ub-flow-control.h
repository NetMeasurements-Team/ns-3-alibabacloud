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
 * @brief Flow-control 事件上下文
 *
 * 这不是一层新抽象，只是把事件发生点已经知道的事实打成一个小包，
 * 避免 On* hook 继续膨胀成一长串位置参数。
 *
 * 当前约定：
 * - packet: 事件对应的报文。运行时主路径应始终非空。
 * - ingressQueue: 只有 allocator 正在处理某个真实 ingress queue 时才有。
 *   典型是 VOQ -> egress queue 的 OnEgressEnqueued / OnIngressReleased 路径。
 *   对于 sink/local ingress 这类“只有报文、没有 queue 对象”的事件，这里可以为空。
 * - inPortId: 该报文当前 ingress ownership 对应的端口。
 * - outPortId: 该报文当前关联的 egress 端口；本地 sink 路径通常等于 inPortId。
 * - priority: 报文所在 VL / priority。
 * - upstreamCreditAlreadyReturned: 只有恢复重入路径会置位。报文离开原始 ingress
 *   （进入 egress 或 retry 存储）时上游信用证已经归还；恢复后的重新入队必须继续释放真实
 *   缓冲区占用，但不得再次归还同一份上游信用证。
 */
struct UbFlowControlEventContext {
    Ptr<Packet> packet;
    Ptr<UbIngressQueue> ingressQueue;
    uint32_t inPortId {0};
    uint32_t outPortId {0};
    uint32_t priority {0};
    bool upstreamCreditAlreadyReturned {false};
};

/**
 * @brief 端口流量控制
 */
class UbFlowControl : public Object {
public:
    static TypeId GetTypeId(void);
    UbFlowControl() {}
    virtual ~UbFlowControl() {}
    // 发送 gating 判定：allocator 在选择某个 ingress queue 发送前调用。
    virtual bool IsFcLimited(Ptr<UbIngressQueue> ingressQ)
    {
        return false;
    }
    // ingress 占用已经由 queue-manager 真正释放后调用。
    virtual void OnIngressReleased(const UbFlowControlEventContext& context) {}
    // 报文已经进入 egress queue 后调用。
    virtual void OnEgressEnqueued(const UbFlowControlEventContext& context) {}
    // 对端发来的 flow-control 控制帧到达后调用。
    virtual void OnControlFrameReceived(Ptr<Packet> p) {}
    // 普通 DLLDP 数据包到达后调用；用于消费本跳 piggyback credit 等 data-link 本地语义。
    virtual void OnDataPacketReceived(Ptr<Packet> p) {}
    // 报文已经完成 ingress 入账后调用。
    virtual void OnIngressEnqueued(const UbFlowControlEventContext& context) {}
    virtual FcType GetFcType()
    {
        return FcType::NONE;
    }
};


} // namespace ns3

#include "ns3/ub-cbfc.h"
#include "ns3/ub-pfc.h"

#endif // UB_FLOWCONTROL_H
