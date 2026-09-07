// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TRAFFIC_GEN_H
#define UB_TRAFFIC_GEN_H

#include <vector>
#include <unordered_map>
#include <set>
#include <mutex>
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

    static bool IsMultiProcessRuntimeUnsupported();

    static inline std::string GetMultiProcessUnsupportedMessage()
    {
        return "UbTrafficGen does not support MPI multi-process usage in this branch. "
               "Supported: unified-bus multi-process data path and UbTrafficGen multithreading. "
               "Unsupported: distributed/operator-style synchronization via traffic.csv. "
               "Use local quick-example runs or build distributed coordination separately.";
    }

    void AddTask(TrafficRecord record);

    void SetPhaseDepend(uint32_t phaseId, uint32_t taskId);

    TrafficRecord GetTaskById(uint32_t taskId);

    void MarkTaskCompleted(uint32_t taskId);

    bool IsCompleted() const;

    uint32_t GetCompletedTaskCount() const;

    /**
     * @brief 任务完成回调
     */
    void OnTaskCompleted(uint32_t taskId);

    /**
     * @brief 调度下一批可执行的任务
     */
    void ScheduleNextTasks();

  private:
    enum class TaskState {
        PENDING,
        READY,
        RUNNING,
        COMPLETED
    };

    map<std::string, TaOpcode> TaOpcodeMap = {
        {"URMA_WRITE", TaOpcode::TA_OPCODE_WRITE},
        {"URMA_READ", TaOpcode::TA_OPCODE_READ},
        {"MEM_STORE", TaOpcode::TA_OPCODE_WRITE},
        {"MEM_LOAD", TaOpcode::TA_OPCODE_READ}
    };

    std::unordered_map<uint32_t, TrafficRecord> m_tasks{};
    std::unordered_map<uint32_t, std::set<uint32_t>> m_dependencies{};
    std::unordered_map<uint32_t, std::set<uint32_t>> m_dependents{};
    std::unordered_map<uint32_t, TaskState> m_taskStates{};
    std::set<uint32_t> m_readyTasks{};
    std::map<uint32_t, set<uint32_t>> m_dependOnPhasesToTaskId{};

    std::vector<TrafficRecord> CollectReadyTasksLocked();

    void ScheduleTasks(const std::vector<TrafficRecord>& tasks);

    mutable std::mutex m_mutex;
};

} // namespace ns3

#endif // UB_TRAFFIC_GEN_H
