// SPDX-License-Identifier: GPL-2.0-only
#include "ub-traffic-gen.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "ns3/callback.h"
#include "ns3/ub-datatype.h"
#include "ns3/ub-function.h"
#include "ub-app.h"
#include "ub-utils.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbTrafficGen");

NS_OBJECT_ENSURE_REGISTERED(UbTrafficGen);
TypeId UbTrafficGen::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbTrafficGen")
            .SetParent<Application>()
            .SetGroupName("UnifiedBus")
            .AddConstructor<UbTrafficGen>();
    return tid;
}

UbTrafficGen::UbTrafficGen()
{
}

UbTrafficGen::~UbTrafficGen()
{
}

void UbTrafficGen::AddTask(TrafficRecord record)
{
    uint32_t taskId = record.taskId;
    if (m_tasks.find(taskId) != m_tasks.end()) {
        NS_LOG_ERROR("TaskId " << taskId << " already exists, cannot add duplicate task!");
        return;
    }
    m_tasks[taskId] = record;
    for (const auto &dependId : record.dependOnPhases) {
        m_dependencies[taskId].insert(m_dependOnPhasesToTaskId[dependId].begin(),
            m_dependOnPhasesToTaskId[dependId].end());
    }

    // 设置初始状态
    if (m_dependencies[taskId].empty()) {
        m_taskStates[taskId] = TaskState::READY;
        m_readyTasks.insert(taskId);
    } else {
        m_taskStates[taskId] = TaskState::PENDING;
    }

    // 建立反向依赖映射
    for (uint32_t depId : m_dependencies[taskId]) {
        m_dependents[depId].insert(taskId);
    }

    NS_LOG_DEBUG("Added task " << taskId << " with " << m_dependencies[taskId].size() << " dependencies");
}

TrafficRecord UbTrafficGen::GetTaskById(uint32_t taskId)
{
    auto it = m_tasks.find(taskId);
    if (it != m_tasks.end()) {
        return it->second;
    } else {
        NS_ASSERT_MSG(0, "Can't find task from TrafficRecord.");
    }
}

void UbTrafficGen::MarkTaskCompleted(uint32_t taskId)
{
    // 检查任务是否正在运行
    if (m_taskStates[taskId] != TaskState::RUNNING) {
        return;
    }

    // 更新状态
    m_taskStates[taskId] = TaskState::COMPLETED;
    NS_LOG_DEBUG("Task " << taskId << " completed");

    // 检查依赖该任务的任务
    auto dependentIt = m_dependents.find(taskId);
    if (dependentIt != m_dependents.end()) {
        for (uint32_t dependentId : dependentIt->second) {
            // 只处理等待状态的任务
            if (m_taskStates[dependentId] != TaskState::PENDING) {
                continue;
            }

            // 检查该任务的所有依赖是否都已完成
            bool allDepsCompleted = true;
            auto depIt = m_dependencies.find(dependentId);
            if (depIt != m_dependencies.end()) {
                for (uint32_t depId : depIt->second) {
                    if (m_taskStates[depId] != TaskState::COMPLETED) {
                        allDepsCompleted = false;
                        break;
                    }
                }
            }

            if (allDepsCompleted) {
                m_taskStates[dependentId] = TaskState::READY;
                m_readyTasks.insert(dependentId);
            }
        }
    }

    UbTrafficGen::Get()->ScheduleNextTasks();

    // 检查是否全部完成
    if (IsCompleted()) {
        NS_LOG_DEBUG("[APPLICATION INFO] All tasks completed");
    }
}

bool UbTrafficGen::IsCompleted() const
{
    for (const auto &statePair : m_taskStates)
        if (statePair.second != TaskState::COMPLETED) {
            return false;
        }

    return true;
}

void UbTrafficGen::ScheduleNextTasks()
{
    for (auto it = m_readyTasks.begin(); it != m_readyTasks.end();) {
        uint32_t taskId = *it;
        // 确认任务的就绪状态
        if (m_taskStates[taskId] == TaskState::READY) {
            m_taskStates[taskId] = TaskState::RUNNING;

            auto taskIt = m_tasks.find(taskId);
            if (taskIt != m_tasks.end()) {
                if (taskIt->second.priority == 0) {
                    NS_LOG_WARN("It is strongly recommended not to set the task priority to 0. " <<
                                "Priority level 0 is reserved for control frames.");
                }
                auto app = DynamicCast<UbApp>(NodeList::GetNode(taskIt->second.sourceNode)->GetApplication(0));
                Time taskDelay = Time(0);
                if (!taskIt->second.delay.empty()) {
                    taskDelay = Time(taskIt->second.delay);
                }
                Simulator::ScheduleWithContext(app->GetNode()->GetId(), taskDelay, &UbApp::SendTraffic, app, taskIt->second);
                NS_LOG_DEBUG("Scheduled task " << taskId);
            }
        }
        it = m_readyTasks.erase(it);
    }
}

void UbTrafficGen::OnTaskCompleted(uint32_t taskId)
{
    MarkTaskCompleted(taskId);
}

} // namespace ns3
