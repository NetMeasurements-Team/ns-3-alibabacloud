// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TRAFFIC_GEN_H
#define UB_TRAFFIC_GEN_H

#include <vector>
#include <unordered_map>
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

using namespace utils;
namespace ns3 {
    class UbApp;

/**
 * @brief 任务图应用,管理多个WQE任务及依赖关系
 */
class UbTrafficGen : public Object, public Singleton<UbTrafficGen> {
public:
    static UbTrafficGen& GetInstance() {
        static UbTrafficGen instance;
        return instance;
    }

    UbTrafficGen(const UbTrafficGen&) = delete;
    UbTrafficGen& operator = (const UbTrafficGen&) = delete;

public:
    static TypeId GetTypeId(void);

    UbTrafficGen();
    virtual ~UbTrafficGen();

    void AddTask(TrafficRecord record);

    TrafficRecord GetTaskById(uint32_t taskId);

    void MarkTaskCompleted(uint32_t taskId);

    bool IsCompleted() const;

    /**
     * @brief 任务完成回调
     */
    void OnTaskCompleted(uint32_t taskId);

    /**
     * @brief 调度下一批可执行的任务
     */
    void ScheduleNextTasks();

    void SetPhaseDepend(uint32_t phaseId, uint32_t taskId)
    {
        m_dependOnPhasesToTaskId[phaseId].insert(taskId);
    }

    // ========== 数据成员 ==========
    // 任务状态枚举
    enum class TaskState {
        PENDING,  // 等待依赖完成
        READY,    // 就绪,可以调度
        RUNNING,  // 正在执行
        COMPLETED // 已完成
    };

    map<std::string, TaOpcode> TaOpcodeMap = {
        {"URMA_WRITE", TaOpcode::TA_OPCODE_WRITE},
        {"MEM_STORE", TaOpcode::TA_OPCODE_WRITE},
        {"MEM_LOAD", TaOpcode::TA_OPCODE_READ}
    };
    // DAG结构
    std::unordered_map<uint32_t, TrafficRecord> m_tasks{};
    std::unordered_map<uint32_t, std::set<uint32_t>>
        m_dependencies{}; // 每个任务ID对应其依赖的任务ID集合(即该任务必须等待这些任务完成)
    std::unordered_map<uint32_t, std::set<uint32_t>>
        m_dependents{}; // 每个任务ID对应所有依赖它的任务ID集合(即这些任务等待该任务完成)
    std::unordered_map<uint32_t, TaskState> m_taskStates{};
    std::set<uint32_t> m_readyTasks{};
    std::map<uint32_t, set<uint32_t>> m_dependOnPhasesToTaskId{};
};

} // namespace ns3

#endif // UB_TRAFFIC_GEN_H
