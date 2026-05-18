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

#ifdef NS3_MPI
#include "ns3/mpi-interface.h"
#endif

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

bool
UbTrafficGen::IsMultiProcessRuntimeUnsupported()
{
#ifdef NS3_MPI
    return MpiInterface::IsEnabled() && MpiInterface::GetSize() > 1;
#else
    return false;
#endif
}

void UbTrafficGen::AddTask(TrafficRecord record)
{
    NS_ABORT_MSG_IF(IsMultiProcessRuntimeUnsupported(), GetMultiProcessUnsupportedMessage());
    std::lock_guard<std::mutex> lock(m_mutex);
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

void
UbTrafficGen::SetPhaseDepend(uint32_t phaseId, uint32_t taskId)
{
    NS_ABORT_MSG_IF(IsMultiProcessRuntimeUnsupported(), GetMultiProcessUnsupportedMessage());
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dependOnPhasesToTaskId[phaseId].insert(taskId);
}

TrafficRecord UbTrafficGen::GetTaskById(uint32_t taskId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(taskId);
    if (it != m_tasks.end()) {
        return it->second;
    } else {
        NS_ASSERT_MSG(0, "Can't find task from TrafficRecord.");
    }
}

void UbTrafficGen::MarkTaskCompleted(uint32_t taskId)
{
    std::vector<TrafficRecord> readyTasks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto stateIt = m_taskStates.find(taskId);
        if (stateIt == m_taskStates.end() || stateIt->second != TaskState::RUNNING) {
            return;
        }

        stateIt->second = TaskState::COMPLETED;
        NS_LOG_DEBUG("Task " << taskId << " completed");

        auto dependentIt = m_dependents.find(taskId);
        if (dependentIt != m_dependents.end()) {
            for (uint32_t dependentId : dependentIt->second) {
                auto dependentStateIt = m_taskStates.find(dependentId);
                if (dependentStateIt == m_taskStates.end() ||
                    dependentStateIt->second != TaskState::PENDING) {
                    continue;
                }

                bool allDepsCompleted = true;
                auto depIt = m_dependencies.find(dependentId);
                if (depIt != m_dependencies.end()) {
                    for (uint32_t depId : depIt->second) {
                        auto depStateIt = m_taskStates.find(depId);
                        if (depStateIt == m_taskStates.end() ||
                            depStateIt->second != TaskState::COMPLETED) {
                            allDepsCompleted = false;
                            break;
                        }
                    }
                }

                if (allDepsCompleted) {
                    dependentStateIt->second = TaskState::READY;
                    m_readyTasks.insert(dependentId);
                }
            }
        }

        readyTasks = CollectReadyTasksLocked();
    }

    ScheduleTasks(readyTasks);

    if (IsCompleted()) {
        NS_LOG_DEBUG("[APPLICATION INFO] All tasks completed");
    }
}

bool UbTrafficGen::IsCompleted() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto &statePair : m_taskStates)
        if (statePair.second != TaskState::COMPLETED) {
            return false;
        }

    return true;
}

uint32_t UbTrafficGen::GetCompletedTaskCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    uint32_t completedTasks = 0;
    for (const auto& statePair : m_taskStates)
    {
        if (statePair.second == TaskState::COMPLETED)
        {
            ++completedTasks;
        }
    }
    return completedTasks;
}

void UbTrafficGen::ScheduleNextTasks()
{
    std::vector<TrafficRecord> readyTasks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        readyTasks = CollectReadyTasksLocked();
    }
    ScheduleTasks(readyTasks);
}

std::vector<TrafficRecord> UbTrafficGen::CollectReadyTasksLocked()
{
    std::vector<TrafficRecord> readyTasks;
    for (auto it = m_readyTasks.begin(); it != m_readyTasks.end();) {
        uint32_t taskId = *it;
        auto stateIt = m_taskStates.find(taskId);
        if (stateIt != m_taskStates.end() && stateIt->second == TaskState::READY) {
            stateIt->second = TaskState::RUNNING;
            auto taskIt = m_tasks.find(taskId);
            if (taskIt != m_tasks.end()) {
                readyTasks.push_back(taskIt->second);
            }
        }
        it = m_readyTasks.erase(it);
    }
    return readyTasks;
}

void UbTrafficGen::ScheduleTasks(const std::vector<TrafficRecord>& tasks)
{
    for (const auto& task : tasks) {
        if (task.priority == 0) {
            NS_LOG_WARN("It is strongly recommended not to set the task priority to 0. " <<
                        "Priority level 0 is reserved for control frames.");
        }
        auto app = DynamicCast<UbApp>(NodeList::GetNode(task.sourceNode)->GetApplication(0));
        Time taskDelay = Time(0);
        if (!task.delay.empty()) {
            taskDelay = Time(task.delay);
        }
        Simulator::ScheduleWithContext(app->GetNode()->GetId(), taskDelay, &UbApp::SendTraffic, app, task);
        NS_LOG_DEBUG("Scheduled task " << task.taskId);
    }
}

void UbTrafficGen::OnTaskCompleted(uint32_t taskId)
{
    MarkTaskCompleted(taskId);
}

} // namespace ns3
