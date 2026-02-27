// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_APP_H
#define UB_APP_H

#include <vector>
#include <set>
#include "ns3/application.h"
#include "ns3/event-id.h"
#include "ns3/ptr.h"
#include "ns3/ipv4-address.h"
#include "ns3/ub-datatype.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-ldst-api.h"
#include "ub-tp-connection-manager.h"
#include "ub-network-address.h"
#include "ns3/random-variable-stream.h"

using namespace utils;
namespace ns3 {

/**
 * @brief 任务图应用,管理多个wqe任务及依赖关系
 */
class UbApp : public Application {
public:
    static TypeId GetTypeId(void);

    UbApp();
    virtual ~UbApp();

    void SendTraffic(TrafficRecord record);
    void SendTrafficForTest(TrafficRecord record);

    void SetNode(Ptr<Node> node); // 设置当前节点

    void SetGetTpnRule(GetTpnRuleT type)
    {
        m_getTpnRule = type;
    }

    void SetUseShortestPaths(bool useShortestPaths)
    {
        m_useShortestPaths = useShortestPaths;
    }

    /**
     * @brief 任务完成回调
     */
    void OnTaskCompleted(uint32_t taskId, uint32_t jettyNum);
    void OnTestTaskCompleted(uint32_t taskId, uint32_t jettyNum);
    void OnMemTaskCompleted(uint32_t taskId);

    // ========== 回调函数 ==========
    void SetFinishCallback(Callback<void, uint32_t, uint32_t> cb, Ptr<UbJetty> jetty);
    void SetFinishCallback(Callback<void, uint32_t> cb, Ptr<UbLdstInstance> ubLdstInstance);

protected:
    void DoDispose(void) override;

private:
    TracedCallback<uint32_t, uint32_t> m_traceMemTaskStartsNotify;
    TracedCallback<uint32_t, uint32_t> m_traceMemTaskCompletesNotify;
    TracedCallback<uint32_t, uint32_t, uint32_t> m_traceWqeTaskStartsNotify;
    TracedCallback<uint32_t, uint32_t, uint32_t> m_traceWqeTaskCompletesNotify;

    void MemTaskStartsNotify(uint32_t nodeId, uint32_t taskId);
    void MemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId);
    void WqeTaskStartsNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId);
    void WqeTaskCompletesNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId);

    map<std::string, TaOpcode> TaOpcodeMap = {
        {"URMA_WRITE", TaOpcode::TA_OPCODE_WRITE},
        {"MEM_STORE", TaOpcode::TA_OPCODE_WRITE},
        {"MEM_LOAD", TaOpcode::TA_OPCODE_READ}
    };

    // 控制器
    bool m_multiPathEnable;

    GetTpnRuleT m_getTpnRule = GetTpnRuleT::BY_PEERNODE_PRIORITY; // 获取Tpn的方式
    bool m_useShortestPaths = true; // 是否根据跳数寻找最短路径

    Ptr<Node> m_node;              // 当前节点

    uint32_t m_jettyNum = 0;       // 当前节点维护的jettynum,不会重复
    Ptr<UniformRandomVariable> m_random;

};

} // namespace ns3

#endif // UB_APP_H
