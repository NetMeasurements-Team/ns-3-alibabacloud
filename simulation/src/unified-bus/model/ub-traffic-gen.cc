// SPDX-License-Identifier: GPL-2.0-only
#include "ub-traffic-gen.h"
#include "ns3/ub-mtp-compat.h"

#include "ns3/ub-transaction-rules.h"
#include "ub-app.h"
#include "ub-utils.h"

#include "ns3/callback.h"
#include "ns3/log.h"
#include "ns3/node-list.h"
#include "ns3/simulator.h"
#include "ns3/ub-datatype.h"
#include "ns3/ub-function.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iostream>
#include <limits>
#include <tuple>
#include <utility>
#include <optional>
#include <string_view>

#ifdef NS3_MPI
#include "ns3/mpi-interface.h"
#endif
#ifdef NS3_MTP
#include "ns3/mtp-interface.h"
#endif

namespace
{
uint64_t
MixTaskStartOffsetHash(uint64_t value)
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

uint32_t
ParseUint32Token(std::string_view field)
{
    NS_ABORT_MSG_IF(field.empty(), "Missing uint32 field in traffic.csv");
    uint32_t value = 0;
    for (char c : field)
    {
        NS_ABORT_MSG_IF(c < '0' || c > '9',
                        "Invalid uint32 field in traffic.csv; signed values are not supported");
        const uint32_t digit = static_cast<uint32_t>(c - '0');
        NS_ABORT_MSG_IF(value > (std::numeric_limits<uint32_t>::max() - digit) / 10,
                        "traffic.csv uint32 field overflow");
        value = value * 10 + digit;
    }
    return value;
}

bool
ReadNextDependencyPhase(std::string_view* field, uint32_t* phaseId)
{
    while (!field->empty() &&
           (field->front() == ' ' || field->front() == '\t' || field->front() == '\r'))
    {
        field->remove_prefix(1);
    }
    if (field->empty())
    {
        return false;
    }

    size_t tokenEnd = 0;
    while (tokenEnd < field->size() && (*field)[tokenEnd] != ' ' && (*field)[tokenEnd] != '\t' &&
           (*field)[tokenEnd] != '\r')
    {
        ++tokenEnd;
    }

    *phaseId = ParseUint32Token(field->substr(0, tokenEnd));
    field->remove_prefix(tokenEnd);
    return true;
}
} // namespace

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
    return false;
}

Time
UbTrafficGen::SelectTaskCleanupDeliveryDelay(bool partitioned, Time causalDeliveryDelay)
{
    return partitioned ? causalDeliveryDelay : TimeStep(0);
}

void UbTrafficGen::AddTask(const TrafficRecord& record)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    AddTaskLocked(record, false);
}

bool
UbTrafficGen::AddTaskDuringInitialLoad(const TrafficRecord& record)
{
    return AddTaskLocked(record, true);
}

bool
UbTrafficGen::AddTaskDuringInitialLoad(const TrafficRecordView& record)
{
    return AddTaskLocked(record, true);
}

bool
UbTrafficGen::AddTaskLocked(const TrafficRecord& record, bool phaseAlreadyIndexed)
{
    uint32_t taskId = record.taskId;
    RuntimeTask task = ConvertToRuntimeTask(record);
    if (!SetTaskLocked(task))
    {
        NS_LOG_ERROR("TaskId " << taskId << " already exists, cannot add duplicate task!");
        return false;
    }
    if (!phaseAlreadyIndexed) {
        RegisterTaskPhaseLocked(record.phaseId, taskId);
    }

    uint32_t pendingPhaseCount = 0;
    const auto addDependencyPhase = [&](uint32_t dependPhaseId) {
        auto& phaseState = m_phaseStates[dependPhaseId];
        if (phaseState.completedTasks < phaseState.totalTasks) {
            phaseState.dependentTasks.push_back(taskId);
            ++pendingPhaseCount;
        }
    };

    uint32_t uniqueDependencyPhaseCount = static_cast<uint32_t>(record.dependOnPhases.size());
    if (record.dependOnPhases.size() == 1) {
        addDependencyPhase(record.dependOnPhases[0]);
    } else if (record.dependOnPhases.size() > 1) {
        std::vector<uint32_t> uniqueDependencyPhases = record.dependOnPhases;
        std::sort(uniqueDependencyPhases.begin(), uniqueDependencyPhases.end());
        uniqueDependencyPhases.erase(std::unique(uniqueDependencyPhases.begin(),
                                                 uniqueDependencyPhases.end()),
                                     uniqueDependencyPhases.end());
        uniqueDependencyPhaseCount = static_cast<uint32_t>(uniqueDependencyPhases.size());
        for (const auto& dependPhaseId : uniqueDependencyPhases) {
            addDependencyPhase(dependPhaseId);
        }
    }
    SetPendingPhaseCountLocked(taskId, pendingPhaseCount);

    // 设置初始状态
    if (pendingPhaseCount == 0) {
        SetTaskStateLocked(taskId, TaskState::READY);
        AddReadyTaskLocked(taskId);
    } else {
        SetTaskStateLocked(taskId, TaskState::PENDING);
    }

    NS_LOG_DEBUG("Added task " << taskId << " with " << uniqueDependencyPhaseCount
                               << " phase dependencies");
    return true;
}

bool
UbTrafficGen::AddTaskLocked(const TrafficRecordView& record, bool phaseAlreadyIndexed)
{
    const uint32_t taskId = record.taskId;
    RuntimeTask task = ConvertToRuntimeTask(record);
    if (!SetTaskLocked(task))
    {
        NS_LOG_ERROR("TaskId " << taskId << " already exists, cannot add duplicate task!");
        return false;
    }
    if (!phaseAlreadyIndexed) {
        RegisterTaskPhaseLocked(record.phaseId, taskId);
    }

    uint32_t pendingPhaseCount = 0;
    const auto addDependencyPhase = [&](uint32_t dependPhaseId) {
        auto& phaseState = m_phaseStates[dependPhaseId];
        if (phaseState.completedTasks < phaseState.totalTasks) {
            phaseState.dependentTasks.push_back(taskId);
            ++pendingPhaseCount;
        }
    };

    uint32_t firstDependencyPhase = 0;
    bool hasFirstDependencyPhase = false;
    std::vector<uint32_t> uniqueDependencyPhases;
    std::string_view dependencyField = record.dependOnPhases;
    uint32_t dependencyPhase = 0;
    while (ReadNextDependencyPhase(&dependencyField, &dependencyPhase)) {
        if (!hasFirstDependencyPhase) {
            firstDependencyPhase = dependencyPhase;
            hasFirstDependencyPhase = true;
            continue;
        }
        if (uniqueDependencyPhases.empty()) {
            uniqueDependencyPhases.reserve(4);
            uniqueDependencyPhases.push_back(firstDependencyPhase);
        }
        uniqueDependencyPhases.push_back(dependencyPhase);
    }

    uint32_t uniqueDependencyPhaseCount = 0;
    if (!uniqueDependencyPhases.empty()) {
        std::sort(uniqueDependencyPhases.begin(), uniqueDependencyPhases.end());
        uniqueDependencyPhases.erase(std::unique(uniqueDependencyPhases.begin(),
                                                 uniqueDependencyPhases.end()),
                                     uniqueDependencyPhases.end());
        uniqueDependencyPhaseCount = static_cast<uint32_t>(uniqueDependencyPhases.size());
        for (const auto& dependPhaseId : uniqueDependencyPhases) {
            addDependencyPhase(dependPhaseId);
        }
    } else if (hasFirstDependencyPhase) {
        uniqueDependencyPhaseCount = 1;
        addDependencyPhase(firstDependencyPhase);
    }

    SetPendingPhaseCountLocked(taskId, pendingPhaseCount);

    if (pendingPhaseCount == 0) {
        SetTaskStateLocked(taskId, TaskState::READY);
        AddReadyTaskLocked(taskId);
    } else {
        SetTaskStateLocked(taskId, TaskState::PENDING);
    }

    NS_LOG_DEBUG("Added task " << taskId << " with " << uniqueDependencyPhaseCount
                               << " phase dependencies");
    return true;
}

void
UbTrafficGen::RegisterPlannedTpConnections(
    uint32_t taskId,
    const std::vector<UbLifecycleResourceIdentity>& connections)
{
    if (connections.empty())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    NS_ABORT_MSG_IF(FindTaskLocked(taskId) == nullptr,
                    "Cannot register planned TP connections for an unknown task " << taskId);
    const auto [task, inserted] = m_taskPlannedTpConnections.emplace(taskId, connections);
    NS_ABORT_MSG_IF(!inserted,
                    "Planned TP connections already registered for task " << taskId);
    for (const auto& identity : task->second)
    {
        NS_ABORT_MSG_IF(identity.kind != UbLifecycleResourceKind::RTP_CONNECTION,
                        "Planned TP policy accepts only connection identities");
        const PlannedTpConnectionKey key{identity.ownerNodeId,
                                         identity.localId,
                                         identity.generation};
        auto [use, created] = m_plannedTpUses.emplace(key, PlannedTpUseRecord{identity, 0});
        NS_ABORT_MSG_IF(!created && use->second.identity != identity,
                        "Planned TP identity key collision");
        ++use->second.remainingUses;
    }
}

void
UbTrafficGen::SetPhaseDepend(uint32_t phaseId, uint32_t taskId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    RegisterTaskPhaseLocked(phaseId, taskId);
}

void
UbTrafficGen::SetPhaseDependDuringInitialLoad(uint32_t phaseId, uint32_t taskId)
{
    RegisterTaskPhaseLocked(phaseId, taskId);
}

void
UbTrafficGen::RegisterPhaseTaskDuringInitialLoad(uint32_t phaseId)
{
    auto& phaseState = m_phaseStates[phaseId];
    ++phaseState.totalTasks;
}

void
UbTrafficGen::ReserveTasksForTraffic(uint64_t recordCount, uint32_t maxTaskId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto reserveCount = static_cast<size_t>(std::min<uint64_t>(
        recordCount, static_cast<uint64_t>(std::numeric_limits<size_t>::max())));

    if (recordCount > 0 && maxTaskId != std::numeric_limits<uint32_t>::max() &&
        static_cast<uint64_t>(maxTaskId) + 1 == recordCount) {
        const auto denseReserve = static_cast<size_t>(maxTaskId) + 1;
        m_denseTasks.reserve(denseReserve);
        m_denseTaskStates.reserve(denseReserve);
        m_densePendingPhaseCounts.reserve(denseReserve);
    } else {
        m_tasks.reserve(reserveCount);
        m_taskStates.reserve(reserveCount);
        m_taskPhaseIds.reserve(reserveCount);
        m_pendingPhaseCounts.reserve(reserveCount);
    }
}

void
UbTrafficGen::RegisterSourceAppDuringInitialLoad(uint32_t sourceNode, Ptr<UbApp> app)
{
    if (sourceNode >= m_sourceApps.size()) {
        m_sourceApps.resize(static_cast<size_t>(sourceNode) + 1);
    }
    m_sourceApps[sourceNode] = app;
}

UbTrafficGen::RuntimeTask
UbTrafficGen::ConvertToRuntimeTask(const TrafficRecord& record)
{
    NS_ABORT_MSG_IF(record.priority < 0 ||
                        static_cast<uint32_t>(record.priority) > UB_PRIORITY_MAX,
                    "Invalid priority field in traffic.csv; valid range is 0.."
                        << static_cast<uint32_t>(UB_PRIORITY_MAX));

    RuntimeTask task;
    task.taskId = static_cast<uint32_t>(record.taskId);
    task.sourceNode = static_cast<uint32_t>(record.sourceNode);
    task.destNode = static_cast<uint32_t>(record.destNode);
    task.dataSize = static_cast<uint32_t>(record.dataSize);
    task.phaseId = static_cast<uint32_t>(record.phaseId);
    task.priority = static_cast<uint8_t>(record.priority);
    task.delay = ParseDelayLocked(record.delay);
    ParseTaskOperation(task, record.opType, record.atomicSubOperation, record.requestPayloadBytes);
    task.srcPortId = record.srcPortId;
    task.dstPortId = record.dstPortId;
    task.hasSrcPortId = record.hasSrcPortId;
    task.hasDstPortId = record.hasDstPortId;
    task.hasPhaseDependencies = !record.dependOnPhases.empty();

    return task;
}

UbTrafficGen::RuntimeTask
UbTrafficGen::ConvertToRuntimeTask(const TrafficRecordView& record)
{
    NS_ABORT_MSG_IF(record.priority > UB_PRIORITY_MAX,
                    "Invalid priority field in traffic.csv; valid range is 0.."
                        << static_cast<uint32_t>(UB_PRIORITY_MAX));

    RuntimeTask task;
    task.taskId = record.taskId;
    task.sourceNode = record.sourceNode;
    task.destNode = record.destNode;
    task.dataSize = record.dataSize;
    task.phaseId = record.phaseId;
    task.priority = record.priority;
    task.delay = ParseDelayLocked(record.delay);
    ParseTaskOperation(task, record.opType, record.atomicSubOperation, record.requestPayloadBytes);
    task.srcPortId = record.srcPortId;
    task.dstPortId = record.dstPortId;
    task.hasSrcPortId = record.hasSrcPortId;
    task.hasDstPortId = record.hasDstPortId;
    task.hasPhaseDependencies = !record.dependOnPhases.empty();

    return task;
}

bool
UbTrafficGen::IsAtomicTask(RuntimeTaskOp op)
{
    return op >= RuntimeTaskOp::URMA_ATOMIC_COMPARE_SWAP &&
           op <= RuntimeTaskOp::URMA_ATOMIC_FETCH_XOR;
}

bool
UbTrafficGen::IsUrmaTask(RuntimeTaskOp op)
{
    return op == RuntimeTaskOp::URMA_WRITE || op == RuntimeTaskOp::URMA_READ ||
           IsAtomicTask(op) || op >= RuntimeTaskOp::URMA_SEND;
}

UbTransactionOpcode
UbTrafficGen::GetUrmaTaskOpcode(RuntimeTaskOp op)
{
    if (IsAtomicTask(op))
    {
        return static_cast<UbTransactionOpcode>(static_cast<uint8_t>(UbTransactionOpcode::ATOMIC_COMPARE_SWAP) +
                                     static_cast<uint8_t>(op) -
                                     static_cast<uint8_t>(RuntimeTaskOp::URMA_ATOMIC_COMPARE_SWAP));
    }
    switch (op)
    {
    case RuntimeTaskOp::URMA_WRITE: return UbTransactionOpcode::WRITE;
    case RuntimeTaskOp::URMA_READ: return UbTransactionOpcode::READ;
    case RuntimeTaskOp::URMA_SEND: return UbTransactionOpcode::SEND;
    case RuntimeTaskOp::URMA_SEND_WITH_IMMEDIATE: return UbTransactionOpcode::SEND_WITH_IMMEDIATE;
    case RuntimeTaskOp::URMA_WRITE_WITH_IMMEDIATE: return UbTransactionOpcode::WRITE_WITH_IMMEDIATE;
    case RuntimeTaskOp::URMA_WRITE_WITH_NOTIFY: return UbTransactionOpcode::WRITE_WITH_NOTIFY;
    case RuntimeTaskOp::URMA_PREFETCH_TGT: return UbTransactionOpcode::PREFETCH_TGT;
    case RuntimeTaskOp::URMA_WRITEBACK: return UbTransactionOpcode::WRITEBACK;
    case RuntimeTaskOp::URMA_MANAGEMENT: return UbTransactionOpcode::MANAGEMENT;
    case RuntimeTaskOp::URMA_WRITE_WITH_ATOMIC_STORE_ADD: return UbTransactionOpcode::WRITE_WITH_ATOMIC_STORE_ADD;
    case RuntimeTaskOp::URMA_WRITE_WITH_REMOTE_SYNC: return UbTransactionOpcode::WRITE_WITH_REMOTE_SYNC;
    case RuntimeTaskOp::URMA_WRITE_WITH_REMOTE_SYNC_WITH_BE: return UbTransactionOpcode::WRITE_WITH_REMOTE_SYNC_WITH_BE;
    case RuntimeTaskOp::URMA_ATOMIC_STORE_WITH_RETURN_STATUS: return UbTransactionOpcode::ATOMIC_STORE_WITH_RETURN_STATUS;
    case RuntimeTaskOp::URMA_ATOMIC_SYNC: return UbTransactionOpcode::ATOMIC_SYNC;
    case RuntimeTaskOp::URMA_WRITE_WITH_BE: return UbTransactionOpcode::WRITE_WITH_BE;
    case RuntimeTaskOp::URMA_WRITEBACK_WITH_BE: return UbTransactionOpcode::WRITEBACK_WITH_BE;
    default: break;
    }
    NS_ABORT_MSG("Runtime task is not an URMA operation");
    return UbTransactionOpcode::MAX_OPCODE;
}

void
UbTrafficGen::ParseTaskOperation(RuntimeTask& task,
                                 std::string_view opType,
                                 std::string_view subOperation,
                                 std::string_view requestPayloadBytes)
{
    task.op = ParseOpLocked(opType);
    task.atomicOptions.reset();
    NS_ABORT_MSG_IF(task.op == RuntimeTaskOp::UNKNOWN,
                    "traffic.csv taskId=" << task.taskId << " opType: unknown value " << opType);
    if (!IsAtomicTask(task.op))
    {
        NS_ABORT_MSG_IF(
            !subOperation.empty() || !requestPayloadBytes.empty(),
            "traffic.csv taskId="
                << task.taskId
                << " atomicSubOperation/requestPayloadBytes: forbidden on non-atomic tasks");
        return;
    }
    UbAtomicOptions options;
    if (!subOperation.empty())
    {
        static constexpr std::array<std::string_view, 8> names =
            {"ADD", "CLR", "EOR", "SET", "SMAX", "SMIN", "UMAX", "UMIN"};
        const auto found = std::find(names.begin(), names.end(), subOperation);
        NS_ABORT_MSG_IF(found == names.end(),
                        "traffic.csv taskId="
                            << task.taskId
                            << " atomicSubOperation: expected ADD/CLR/EOR/SET/SMAX/SMIN/UMAX/UMIN");
        options.subOperation =
            static_cast<UbAtomicSubOperation>(std::distance(names.begin(), found));
    }
    if (!requestPayloadBytes.empty())
    {
        uint32_t payload = 0;
        const auto parsed = std::from_chars(requestPayloadBytes.data(),
                                            requestPayloadBytes.data() + requestPayloadBytes.size(),
                                            payload);
        NS_ABORT_MSG_IF(parsed.ec != std::errc{} ||
                            parsed.ptr != requestPayloadBytes.data() + requestPayloadBytes.size(),
                        "traffic.csv taskId=" << task.taskId
                                              << " requestPayloadBytes: expected uint32 decimal");
        options.requestPayloadBytes = payload;
    }
    // Reuse the single transaction validation entry so CSV input obeys exactly the constraints a
    // local WQE submission will enforce later.
    UbTransactionRequestInput request;
    request.opcode = GetUrmaTaskOpcode(task.op);
    request.logicalBytes = task.dataSize;
    request.payloadBytes = options.requestPayloadBytes.value_or(task.dataSize);
    request.fields.memoryAccess = UbMemoryAccessFields{0, 0, task.dataSize};
    request.atomicOptions = options;
    request.maxPayloadBytes = UB_MTU_BYTE;
    request.origin = UbTransactionRequestOrigin::LOCAL_SUBMISSION;
    const auto validation = ValidateTransactionRequest(request);
    NS_ABORT_MSG_IF(!validation.IsValid(),
                    "traffic.csv taskId=" << task.taskId << ": " << validation.message);
    task.atomicOptions = options;
}

UbTrafficGen::RuntimeTaskOp
UbTrafficGen::ParseOpLocked(const std::string& opType)
{
    return ParseOpLocked(std::string_view(opType));
}

UbTrafficGen::RuntimeTaskOp
UbTrafficGen::ParseOpLocked(std::string_view opType)
{
    if (opType == "URMA_WRITE") {
        return RuntimeTaskOp::URMA_WRITE;
    }
    static constexpr std::pair<std::string_view, RuntimeTaskOp> extra[] = {
        {"URMA_SEND", RuntimeTaskOp::URMA_SEND}, {"URMA_SEND_WITH_IMMEDIATE", RuntimeTaskOp::URMA_SEND_WITH_IMMEDIATE},
        {"URMA_WRITE_WITH_IMMEDIATE", RuntimeTaskOp::URMA_WRITE_WITH_IMMEDIATE}, {"URMA_WRITE_WITH_NOTIFY", RuntimeTaskOp::URMA_WRITE_WITH_NOTIFY},
        {"URMA_PREFETCH_TGT", RuntimeTaskOp::URMA_PREFETCH_TGT}, {"URMA_WRITEBACK", RuntimeTaskOp::URMA_WRITEBACK},
        {"URMA_MANAGEMENT", RuntimeTaskOp::URMA_MANAGEMENT}, {"URMA_WRITE_WITH_ATOMIC_STORE_ADD", RuntimeTaskOp::URMA_WRITE_WITH_ATOMIC_STORE_ADD},
        {"URMA_WRITE_WITH_REMOTE_SYNC", RuntimeTaskOp::URMA_WRITE_WITH_REMOTE_SYNC}, {"URMA_WRITE_WITH_REMOTE_SYNC_WITH_BE", RuntimeTaskOp::URMA_WRITE_WITH_REMOTE_SYNC_WITH_BE},
        {"URMA_ATOMIC_STORE_WITH_RETURN_STATUS", RuntimeTaskOp::URMA_ATOMIC_STORE_WITH_RETURN_STATUS}, {"URMA_ATOMIC_SYNC", RuntimeTaskOp::URMA_ATOMIC_SYNC},
        {"URMA_WRITE_WITH_BE", RuntimeTaskOp::URMA_WRITE_WITH_BE}, {"URMA_WRITEBACK_WITH_BE", RuntimeTaskOp::URMA_WRITEBACK_WITH_BE}};
    for (const auto& [name, value] : extra) if (opType == name) return value;
    if (opType == "URMA_READ") {
        return RuntimeTaskOp::URMA_READ;
    }
    if (opType == "MEM_STORE") {
        return RuntimeTaskOp::MEM_STORE;
    }
    if (opType == "MEM_LOAD") {
        return RuntimeTaskOp::MEM_LOAD;
    }
    if (opType == "COMPUTE") {
        return RuntimeTaskOp::COMPUTE;
    }
    if (opType == "LOCAL_READY") {
        return RuntimeTaskOp::LOCAL_READY;
    }
    static constexpr std::array<std::string_view, 9> atomicNames = {"URMA_ATOMIC_COMPARE_SWAP",
                                                                    "URMA_ATOMIC_SWAP",
                                                                    "URMA_ATOMIC_STORE",
                                                                    "URMA_ATOMIC_LOAD",
                                                                    "URMA_ATOMIC_FETCH_ADD",
                                                                    "URMA_ATOMIC_FETCH_SUB",
                                                                    "URMA_ATOMIC_FETCH_AND",
                                                                    "URMA_ATOMIC_FETCH_OR",
                                                                    "URMA_ATOMIC_FETCH_XOR"};
    const auto found = std::find(atomicNames.begin(), atomicNames.end(), opType);
    if (found != atomicNames.end())
    {
        return static_cast<RuntimeTaskOp>(
            static_cast<uint8_t>(RuntimeTaskOp::URMA_ATOMIC_COMPARE_SWAP) +
            std::distance(atomicNames.begin(), found));
    }
    return RuntimeTaskOp::UNKNOWN;
}

Ptr<UbApp>
UbTrafficGen::GetSourceAppLocked(uint32_t sourceNode) const
{
    if (sourceNode >= m_sourceApps.size()) {
        return nullptr;
    }
    return m_sourceApps[sourceNode];
}

Time
UbTrafficGen::ParseDelayLocked(const std::string& delay)
{
    return ParseDelayLocked(std::string_view(delay));
}

Time
UbTrafficGen::ParseDelayLocked(std::string_view delay)
{
    if (delay.empty()) {
        return Time(0);
    }
    if (delay == "0" || delay == "0s" || delay == "0ms" || delay == "0us" || delay == "0ns" ||
        delay == "0ps" || delay == "0fs") {
        return Time(0);
    }

    Time parsed;
    if (TryParseIntegerDelay(delay, &parsed)) {
        return parsed;
    }

    std::string delayString(delay);
    auto cacheIt = m_delayParseCache.find(delayString);
    if (cacheIt != m_delayParseCache.end()) {
        return cacheIt->second;
    }

    parsed = Time(delayString);
    if (m_delayParseCache.size() < kMaxDelayParseCacheEntries) {
        m_delayParseCache.emplace(std::move(delayString), parsed);
    }
    return parsed;
}

bool
UbTrafficGen::TryParseIntegerDelay(const std::string& delay, Time* out)
{
    return TryParseIntegerDelay(std::string_view(delay), out);
}

bool
UbTrafficGen::TryParseIntegerDelay(std::string_view delay, Time* out)
{
    if (delay.empty() || out == nullptr) {
        return false;
    }

    uint64_t value = 0;
    std::size_t pos = 0;
    for (; pos < delay.size(); ++pos) {
        const char c = delay[pos];
        if (c < '0' || c > '9') {
            break;
        }
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }

    if (pos == 0) {
        return false;
    }

    const char* unit = delay.data() + pos;
    const std::size_t unitLength = delay.size() - pos;
    if (unitLength == 0 || (unitLength == 1 && unit[0] == 's')) {
        *out = Time::FromInteger(value, Time::S);
    } else if (unitLength == 2 && unit[0] == 'm' && unit[1] == 's') {
        *out = Time::FromInteger(value, Time::MS);
    } else if (unitLength == 2 && unit[0] == 'u' && unit[1] == 's') {
        *out = Time::FromInteger(value, Time::US);
    } else if (unitLength == 2 && unit[0] == 'n' && unit[1] == 's') {
        *out = Time::FromInteger(value, Time::NS);
    } else if (unitLength == 2 && unit[0] == 'p' && unit[1] == 's') {
        *out = Time::FromInteger(value, Time::PS);
    } else if (unitLength == 2 && unit[0] == 'f' && unit[1] == 's') {
        *out = Time::FromInteger(value, Time::FS);
    } else if (unitLength == 3 && unit[0] == 'm' && unit[1] == 'i' && unit[2] == 'n') {
        *out = Time::FromInteger(value, Time::MIN);
    } else if (unitLength == 1 && unit[0] == 'h') {
        *out = Time::FromInteger(value, Time::H);
    } else if (unitLength == 1 && unit[0] == 'd') {
        *out = Time::FromInteger(value, Time::D);
    } else if (unitLength == 1 && unit[0] == 'y') {
        *out = Time::FromInteger(value, Time::Y);
    } else {
        return false;
    }

    return true;
}

void
UbTrafficGen::SwitchDenseTaskStoreToMapLocked()
{
    if (!m_useDenseTaskStore) {
        return;
    }

    for (uint32_t taskId = 0; taskId < m_denseTasks.size(); ++taskId) {
        m_tasks[taskId] = m_denseTasks[taskId];
    }

    m_denseTasks.clear();
    m_useDenseTaskStore = false;
}

bool
UbTrafficGen::HasTaskLocked(uint32_t taskId) const
{
    if (m_useDenseTaskStore) {
        return taskId < m_denseTasks.size();
    }
    return m_tasks.find(taskId) != m_tasks.end();
}

UbTrafficGen::RuntimeTask*
UbTrafficGen::FindTaskLocked(uint32_t taskId)
{
    if (m_useDenseTaskStore) {
        if (taskId >= m_denseTasks.size()) {
            return nullptr;
        }
        return &m_denseTasks[taskId];
    }

    auto taskIt = m_tasks.find(taskId);
    if (taskIt == m_tasks.end()) {
        return nullptr;
    }
    return &taskIt->second;
}

const UbTrafficGen::RuntimeTask*
UbTrafficGen::FindTaskLocked(uint32_t taskId) const
{
    if (m_useDenseTaskStore) {
        if (taskId >= m_denseTasks.size()) {
            return nullptr;
        }
        return &m_denseTasks[taskId];
    }

    auto taskIt = m_tasks.find(taskId);
    if (taskIt == m_tasks.end()) {
        return nullptr;
    }
    return &taskIt->second;
}

bool
UbTrafficGen::SetTaskLocked(RuntimeTask task)
{
    const uint32_t taskId = task.taskId;
    if (HasTaskLocked(taskId)) {
        return false;
    }

    if (m_useDenseTaskStore) {
        if (taskId == m_denseTasks.size()) {
            m_denseTasks.push_back(task);
            return true;
        }

        SwitchDenseTaskStoreToMapLocked();
    }

    m_tasks[taskId] = task;
    return true;
}

void
UbTrafficGen::RegisterTaskPhaseLocked(uint32_t phaseId, uint32_t taskId)
{
    if (HasTaskPhaseLocked(taskId)) {
        return;
    }

    SetTaskPhaseLocked(taskId, phaseId);
    auto& phaseState = m_phaseStates[phaseId];
    ++phaseState.totalTasks;
}

void
UbTrafficGen::AddReadyTaskLocked(uint32_t taskId)
{
    if (!m_readyTasks.empty() && taskId < m_readyTasks.back()) {
        m_readyTasksSorted = false;
    }
    m_readyTasks.push_back(taskId);
}

void
UbTrafficGen::SwitchDenseTaskStateToMapLocked()
{
    if (!m_useDenseTaskState) {
        return;
    }

    for (uint32_t taskId = 0; taskId < m_denseTaskStates.size(); ++taskId) {
        m_taskStates[taskId] = m_denseTaskStates[taskId];
        m_pendingPhaseCounts[taskId] = m_densePendingPhaseCounts[taskId];
        const RuntimeTask* task = FindTaskLocked(taskId);
        if (task != nullptr) {
            m_taskPhaseIds.emplace(taskId, task->phaseId);
        }
    }

    m_denseTaskStates.clear();
    m_densePendingPhaseCounts.clear();
    m_useDenseTaskState = false;
}

bool
UbTrafficGen::HasTaskStateLocked(uint32_t taskId) const
{
    if (m_useDenseTaskState) {
        return taskId < m_denseTaskStates.size();
    }
    return m_taskStates.find(taskId) != m_taskStates.end();
}

UbTrafficGen::TaskState*
UbTrafficGen::FindTaskStateLocked(uint32_t taskId)
{
    if (m_useDenseTaskState) {
        if (taskId >= m_denseTaskStates.size()) {
            return nullptr;
        }
        return &m_denseTaskStates[taskId];
    }

    auto stateIt = m_taskStates.find(taskId);
    if (stateIt == m_taskStates.end()) {
        return nullptr;
    }
    return &stateIt->second;
}

void
UbTrafficGen::SetTaskStateLocked(uint32_t taskId, TaskState state)
{
    if (m_useDenseTaskState) {
        if (taskId == m_denseTaskStates.size()) {
            m_denseTaskStates.push_back(state);
            m_densePendingPhaseCounts.push_back(0);
            return;
        }

        if (taskId < m_denseTaskStates.size()) {
            m_denseTaskStates[taskId] = state;
            return;
        }

        SwitchDenseTaskStateToMapLocked();
    }

    m_taskStates[taskId] = state;
}

bool
UbTrafficGen::HasTaskPhaseLocked(uint32_t taskId) const
{
    return m_taskPhaseIds.find(taskId) != m_taskPhaseIds.end();
}

void
UbTrafficGen::SetTaskPhaseLocked(uint32_t taskId, uint32_t phaseId)
{
    m_taskPhaseIds[taskId] = phaseId;
}

uint32_t
UbTrafficGen::GetPendingPhaseCountLocked(uint32_t taskId) const
{
    if (m_useDenseTaskState) {
        if (taskId >= m_denseTaskStates.size()) {
            return 0;
        }
        return m_densePendingPhaseCounts[taskId];
    }

    auto pendingIt = m_pendingPhaseCounts.find(taskId);
    if (pendingIt == m_pendingPhaseCounts.end()) {
        return 0;
    }
    return pendingIt->second;
}

bool
UbTrafficGen::DecrementPendingPhaseCountLocked(uint32_t taskId)
{
    if (m_useDenseTaskState) {
        if (taskId >= m_denseTaskStates.size() || m_densePendingPhaseCounts[taskId] == 0) {
            return false;
        }
        --m_densePendingPhaseCounts[taskId];
        return m_densePendingPhaseCounts[taskId] == 0;
    }

    auto pendingIt = m_pendingPhaseCounts.find(taskId);
    if (pendingIt == m_pendingPhaseCounts.end() || pendingIt->second == 0) {
        return false;
    }

    --pendingIt->second;
    return pendingIt->second == 0;
}

void
UbTrafficGen::SetPendingPhaseCountLocked(uint32_t taskId, uint32_t count)
{
    if (m_useDenseTaskState) {
        if (taskId == m_denseTaskStates.size()) {
            m_denseTaskStates.push_back(TaskState::PENDING);
            m_densePendingPhaseCounts.push_back(count);
            return;
        }

        if (taskId < m_denseTaskStates.size()) {
            m_densePendingPhaseCounts[taskId] = count;
            return;
        }

        SwitchDenseTaskStateToMapLocked();
    }

    m_pendingPhaseCounts[taskId] = count;
}

UbTrafficGen::TaskCleanupInfo
UbTrafficGen::GetTaskCleanupInfoById(uint32_t taskId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const RuntimeTask* task = FindTaskLocked(taskId);
    NS_ABORT_MSG_IF(task == nullptr, "Can't find task from TrafficRecord.");
    TaskCleanupInfo info;
    info.sourceNode = task->sourceNode;
    info.destNode = task->destNode;
    info.priority = task->priority;
    return info;
}

void UbTrafficGen::MarkTaskCompleted(uint32_t taskId)
{
    SubmitTaskCompletion(taskId, std::nullopt);
}

void
UbTrafficGen::SubmitTaskCompletion(uint32_t taskId,
                                   std::optional<TaskCleanupRequest> cleanupRequest)
{
    Time completionVisibleTs;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        TaskState* state = FindTaskStateLocked(taskId);
        if (state == nullptr || *state != TaskState::RUNNING) {
            return;
        }

        if (cleanupRequest.has_value())
        {
            const auto [cleanupIt, inserted] =
                m_pendingTaskCleanups.emplace(taskId, *cleanupRequest);
            NS_ABORT_MSG_IF(!inserted &&
                                (cleanupIt->second.ownerNodeId != cleanupRequest->ownerNodeId ||
                                 cleanupIt->second.ownerApplicationIndex !=
                                     cleanupRequest->ownerApplicationIndex ||
                                 cleanupIt->second.jettyNum != cleanupRequest->jettyNum ||
                                 cleanupIt->second.destroyTaskScopedJetty !=
                                     cleanupRequest->destroyTaskScopedJetty ||
                                 cleanupIt->second.destroyPlannedTpConnections !=
                                     cleanupRequest->destroyPlannedTpConnections),
                            "Conflicting task cleanup request for task " << taskId);
            if (!inserted)
            {
                return;
            }
        }

        const Time delay = ResolveCompletionVisibleDelayLocked();
        completionVisibleTs = Simulator::Now() + delay;
    }

    ScheduleTaskCompletionVisibility(taskId, completionVisibleTs);
}

void
UbTrafficGen::SetDependencyVisibilityDelay(Time delay)
{
    NS_ABORT_MSG_IF(delay.IsStrictlyNegative(),
                    "dependency visibility delay must be non-negative");
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dependencyVisibilityDelay = delay;
}

Time
UbTrafficGen::GetDependencyVisibilityDelay() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return ResolveCompletionVisibleDelayLocked();
}

void
UbTrafficGen::ValidateDependencyVisibilityDelay(bool requireStrictlyPositive) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!HasDependenciesLocked()) {
        return;
    }

    NS_ABORT_MSG_IF(requireStrictlyPositive && !m_dependencyVisibilityDelay.IsStrictlyPositive(),
                    "parallel traffic.csv dependencies require a positive dependency visibility delay.");
}

void
UbTrafficGen::SetInitialTaskStartOffsetWindow(Time window, uint32_t seed)
{
    NS_ABORT_MSG_IF(window.IsStrictlyNegative(),
                    "initial task start offset window must be non-negative");
    std::lock_guard<std::mutex> lock(m_mutex);
    m_initialTaskStartOffsetWindow = window;
    m_initialTaskStartOffsetSeed = seed;
}

void
UbTrafficGen::EnableCanonicalOutput(std::string path, uint32_t rank)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_canonicalOutputEnabled = true;
    m_canonicalOutputPath = std::move(path);
    m_canonicalRank = rank;
    m_canonicalEvents.clear();
}

void
UbTrafficGen::RecordCanonicalEvent(const std::string& type, const RuntimeTask& task)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_canonicalOutputEnabled || !ShouldRecordCanonicalEventLocked(task)) {
        return;
    }

    m_canonicalEvents.push_back(CanonicalEvent{
        static_cast<uint64_t>(Simulator::Now().GetNanoSeconds()),
        type,
        task.taskId,
        task.sourceNode,
        task.destNode,
    });
}

void
UbTrafficGen::RecordCanonicalCompletionLocked(const RuntimeTask& task)
{
    if (!m_canonicalOutputEnabled) {
        return;
    }

    if (!ShouldRecordCanonicalEventLocked(task)) {
        return;
    }

    m_canonicalEvents.push_back(CanonicalEvent{
        static_cast<uint64_t>(Simulator::Now().GetNanoSeconds()),
        "COMPLETE_VISIBLE",
        task.taskId,
        task.sourceNode,
        task.destNode,
    });
}

bool
UbTrafficGen::ShouldRecordCanonicalEventLocked(const RuntimeTask& task) const
{
#ifdef NS3_MPI
    if (MpiInterface::IsEnabled() && MpiInterface::GetSize() > 1) {
        Ptr<Node> sourceNode = NodeList::GetNode(task.sourceNode);
        return UbUtils::ExtractMpiRank(sourceNode->GetSystemId()) == m_canonicalRank;
    }
#endif
    return true;
}

void
UbTrafficGen::WriteCanonicalOutput() const
{
    std::vector<CanonicalEvent> events;
    std::string outputPath;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_canonicalOutputEnabled) {
            return;
        }
        events = m_canonicalEvents;
        outputPath = m_canonicalOutputPath + ".rank" + std::to_string(m_canonicalRank) + ".txt";
    }

    std::sort(events.begin(), events.end(), [](const CanonicalEvent& lhs, const CanonicalEvent& rhs) {
        return std::tie(lhs.timeNs, lhs.type, lhs.taskId, lhs.sourceNode, lhs.destNode) <
               std::tie(rhs.timeNs, rhs.type, rhs.taskId, rhs.sourceNode, rhs.destNode);
    });

    std::ofstream output(outputPath);
    NS_ABORT_MSG_IF(!output.is_open(), "cannot open canonical output file: " << outputPath);
    for (const auto& event : events) {
        output << event.timeNs << ',' << event.type << ',' << event.taskId << ','
               << event.sourceNode << ',' << event.destNode << '\n';
    }
}

void
UbTrafficGen::ApplyTaskCompletion(uint32_t taskId)
{
    ReadyTaskBatch readyBatch;
    std::optional<TaskCleanupRequest> cleanupRequest;
    bool releasePlannedTpBinding = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        TaskState* state = FindTaskStateLocked(taskId);
        NS_ABORT_MSG_IF(state == nullptr, "Unknown completed taskId " << taskId);
        if (*state == TaskState::COMPLETED) {
            return;
        }

        const RuntimeTask* completedTask = FindTaskLocked(taskId);
        NS_ABORT_MSG_IF(completedTask == nullptr, "Unknown completed taskId " << taskId);

        *state = TaskState::COMPLETED;
        RecordCanonicalCompletionLocked(*completedTask);
        NS_LOG_DEBUG("Task " << taskId << " completion visible");

        bool hasPhaseId = false;
        uint32_t phaseId = 0;
        if (completedTask != nullptr) {
            phaseId = completedTask->phaseId;
            hasPhaseId = true;
        } else {
            const auto phaseIt = m_taskPhaseIds.find(taskId);
            if (phaseIt != m_taskPhaseIds.end()) {
                phaseId = phaseIt->second;
                hasPhaseId = true;
            }
        }

        if (hasPhaseId) {
            auto phaseStateIt = m_phaseStates.find(phaseId);
            if (phaseStateIt != m_phaseStates.end()) {
                ++phaseStateIt->second.completedTasks;

                if (phaseStateIt->second.completedTasks >= phaseStateIt->second.totalTasks) {
                    for (uint32_t dependentId : phaseStateIt->second.dependentTasks) {
                        TaskState* dependentState = FindTaskStateLocked(dependentId);
                        if (dependentState == nullptr || *dependentState != TaskState::PENDING) {
                            continue;
                        }

                        if (DecrementPendingPhaseCountLocked(dependentId)) {
                            *dependentState = TaskState::READY;
                            AddReadyTaskLocked(dependentId);
                        }
                    }
                    phaseStateIt->second.dependentTasks.clear();
                }
            }
        }

        readyBatch = CollectReadyTaskBatchLocked();
        const auto cleanupIt = m_pendingTaskCleanups.find(taskId);
        if (cleanupIt != m_pendingTaskCleanups.end())
        {
            cleanupRequest = cleanupIt->second;
            m_pendingTaskCleanups.erase(cleanupIt);
        }

        if (!m_taskPlannedTpConnections.empty())
        {
            const auto plannedTask = m_taskPlannedTpConnections.find(taskId);
            if (plannedTask != m_taskPlannedTpConnections.end())
            {
                releasePlannedTpBinding = cleanupRequest.has_value() &&
                                          cleanupRequest->destroyPlannedTpConnections;
                for (const auto& identity : plannedTask->second)
                {
                    const PlannedTpConnectionKey key{identity.ownerNodeId,
                                                     identity.localId,
                                                     identity.generation};
                    auto use = m_plannedTpUses.find(key);
                    NS_ABORT_MSG_IF(use == m_plannedTpUses.end() ||
                                        use->second.remainingUses == 0,
                                    "Planned TP reference balance is corrupt for task " << taskId);
                    --use->second.remainingUses;
                    if (use->second.remainingUses == 0)
                    {
                        if (cleanupRequest.has_value() &&
                            cleanupRequest->destroyPlannedTpConnections)
                        {
                            cleanupRequest->plannedTpConnections.push_back(identity);
                        }
                        m_plannedTpUses.erase(use);
                    }
                }
                m_taskPlannedTpConnections.erase(plannedTask);
            }
        }
    }

    ScheduleTasks(readyBatch);

    if (cleanupRequest.has_value())
    {
        Time deliveryDelay = TimeStep(0);
#ifdef NS3_MTP
        if (MtpInterface::isPartitioned())
        {
            deliveryDelay = SelectTaskCleanupDeliveryDelay(
                true,
                UbGetCausalDeliveryDelay(cleanupRequest->ownerNodeId));
        }
#endif
        const auto cleanup = *cleanupRequest;
        Simulator::ScheduleWithContext(cleanup.ownerNodeId,
                                       deliveryDelay,
                                       [cleanup, taskId, releasePlannedTpBinding]() {
                                           UbTrafficGen::ApplyTaskCleanupOnOwner(
                                               cleanup.ownerNodeId,
                                               cleanup.ownerApplicationIndex,
                                               taskId,
                                               cleanup.jettyNum,
                                               cleanup.destroyTaskScopedJetty,
                                               releasePlannedTpBinding,
                                               cleanup.plannedTpConnections);
                                       });
    }

    if (IsCompleted()) {
        NS_LOG_DEBUG("[APPLICATION INFO] All tasks completed");
    }
}

void
UbTrafficGen::ApplyTaskCleanupOnOwner(uint32_t ownerNodeId,
                                      uint32_t ownerApplicationIndex,
                                      uint32_t taskId,
                                      uint32_t jettyNum,
                                      bool destroyTaskScopedJetty,
                                      bool releasePlannedTpBinding,
                                      std::vector<UbLifecycleResourceIdentity> plannedTpConnections)
{
    Ptr<Node> ownerNode = NodeList::GetNode(ownerNodeId);
    NS_ABORT_MSG_IF(ownerNode == nullptr || ownerApplicationIndex >= ownerNode->GetNApplications(),
                    "Task cleanup owner is unavailable for task " << taskId);
    Ptr<UbApp> cleanupOwner = DynamicCast<UbApp>(ownerNode->GetApplication(ownerApplicationIndex));
    NS_ABORT_MSG_IF(cleanupOwner == nullptr,
                    "Task cleanup owner application is not UbApp for task " << taskId);
    cleanupOwner->CleanupCompletedTask(taskId,
                                       jettyNum,
                                       destroyTaskScopedJetty,
                                       releasePlannedTpBinding,
                                       plannedTpConnections);
}

bool UbTrafficGen::IsCompleted() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_useDenseTaskState) {
        for (const TaskState state : m_denseTaskStates) {
            if (state != TaskState::COMPLETED) {
                return false;
            }
        }
        return true;
    }

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
    if (m_useDenseTaskState)
    {
        for (const TaskState state : m_denseTaskStates)
        {
            if (state == TaskState::COMPLETED)
            {
                ++completedTasks;
            }
        }
        return completedTasks;
    }

    for (const auto& statePair : m_taskStates)
    {
        if (statePair.second == TaskState::COMPLETED)
        {
            ++completedTasks;
        }
    }
    return completedTasks;
}

uint64_t
UbTrafficGen::GetDependencyReferenceCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t referenceCount = 0;
    for (const auto& phaseState : m_phaseStates) {
        referenceCount += phaseState.second.dependentTasks.size();
    }
    return referenceCount;
}

uint32_t
UbTrafficGen::GetPendingPhaseCountForTesting(uint32_t taskId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return GetPendingPhaseCountLocked(taskId);
}

bool
UbTrafficGen::IsUsingDenseTaskStoreForTesting() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_useDenseTaskStore;
}

uint32_t
UbTrafficGen::GetStoredTaskCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_useDenseTaskStore) {
        return static_cast<uint32_t>(m_tasks.size());
    }

    return static_cast<uint32_t>(m_denseTasks.size());
}

std::vector<uint32_t>
UbTrafficGen::CollectReadyTaskIdsForTesting()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<uint32_t> ids = m_readyTasks;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

void UbTrafficGen::ScheduleNextTasks()
{
    ReadyTaskBatch readyBatch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        readyBatch = CollectReadyTaskBatchLocked();
    }
    ScheduleTasks(readyBatch);
}

UbTrafficGen::ReadyTaskBatch
UbTrafficGen::CollectReadyTaskBatchLocked()
{
#ifdef NS3_MPI
    const bool filterByOwner = MpiInterface::IsEnabled() && MpiInterface::GetSize() > 1;
    uint32_t currentRank = 0;
    if (filterByOwner) {
        currentRank = MpiInterface::GetSystemId();
    }
#else
    const bool filterByOwner = false;
    const uint32_t currentRank = 0;
#endif

    if (!m_readyTasksSorted) {
        std::sort(m_readyTasks.begin(), m_readyTasks.end());
        m_readyTasks.erase(std::unique(m_readyTasks.begin(), m_readyTasks.end()),
                           m_readyTasks.end());
        m_readyTasksSorted = true;
    }

    ReadyTaskBatch batch;
    batch.tasks.reserve(m_readyTasks.size());
    batch.sourceApps.reserve(m_readyTasks.size());
    std::vector<uint32_t> remainingReadyTasks;
    remainingReadyTasks.reserve(m_readyTasks.size());
    for (uint32_t taskId : m_readyTasks) {
        TaskState* state = FindTaskStateLocked(taskId);
        if (state != nullptr && *state == TaskState::READY) {
            RuntimeTask* task = FindTaskLocked(taskId);
            if (task != nullptr) {
                const bool ownedByCurrentRank =
                    !filterByOwner ||
                    UbUtils::ExtractMpiRank(NodeList::GetNode(task->sourceNode)->GetSystemId()) ==
                        currentRank;
                if (!ownedByCurrentRank) {
                    remainingReadyTasks.push_back(taskId);
                    continue;
                }

                // COMPUTE acquires its UBPU only when its scheduled start event fires. Keep it
                // READY during a non-zero task delay so resource ownership starts at execution,
                // while communication retains the existing scheduled RUNNING transition.
                if (task->op != RuntimeTaskOp::COMPUTE)
                {
                    *state = TaskState::RUNNING;
                }
                batch.tasks.push_back(*task);
                batch.sourceApps.push_back(GetSourceAppLocked(task->sourceNode));
            }
        }
    }
    m_readyTasks = std::move(remainingReadyTasks);
    m_readyTasksSorted = true;
    return batch;
}

bool
UbTrafficGen::HasDependenciesLocked() const
{
    if (m_useDenseTaskState) {
        for (const uint32_t pendingPhaseCount : m_densePendingPhaseCounts) {
            if (pendingPhaseCount > 0) {
                return true;
            }
        }
    }

    for (const auto& pendingPhasePair : m_pendingPhaseCounts) {
        if (pendingPhasePair.second > 0) {
            return true;
        }
    }

    for (const auto& phaseStatePair : m_phaseStates) {
        if (!phaseStatePair.second.dependentTasks.empty()) {
            return true;
        }
    }
    return false;
}

Time
UbTrafficGen::ResolveCompletionVisibleDelayLocked() const
{
    return m_dependencyVisibilityDelay;
}

uint64_t
UbTrafficGen::GetInitialTaskSourceOffsetHash(uint32_t sourceNode, uint32_t seed) const
{
    uint64_t hash =
        MixTaskStartOffsetHash(static_cast<uint64_t>(seed) ^ 0x726f6f742d66726fULL);
    return MixTaskStartOffsetHash(hash ^ sourceNode);
}

std::vector<Time>
UbTrafficGen::GetInitialTaskStartOffsets(const ReadyTaskBatch& batch) const
{
    std::vector<Time> offsets(batch.tasks.size(), Time(0));
    Time offsetWindow;
    uint32_t offsetSeed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        offsetWindow = m_initialTaskStartOffsetWindow;
        offsetSeed = m_initialTaskStartOffsetSeed;
    }

    const int64_t maxSteps = offsetWindow.GetTimeStep();
    if (maxSteps <= 0)
    {
        return offsets;
    }

    const auto slotCount = static_cast<uint64_t>(maxSteps);
    std::unordered_map<uint32_t, uint64_t> sourceSlots;
    for (const auto& task : batch.tasks)
    {
        if (!task.hasPhaseDependencies)
        {
            sourceSlots.try_emplace(task.sourceNode,
                                    GetInitialTaskSourceOffsetHash(task.sourceNode, offsetSeed) %
                                        slotCount);
        }
    }

    std::unordered_map<uint64_t, size_t> slotOccupancy;
    size_t maxSlotOccupancy = 0;
    for (const auto& [sourceNode, slot] : sourceSlots)
    {
        (void)sourceNode;
        maxSlotOccupancy = std::max(maxSlotOccupancy, ++slotOccupancy[slot]);
    }

    for (size_t index = 0; index < batch.tasks.size(); ++index)
    {
        const auto& task = batch.tasks[index];
        if (!task.hasPhaseDependencies)
        {
            offsets[index] = TimeStep(static_cast<int64_t>(sourceSlots.at(task.sourceNode)));
        }
    }

    const size_t collisionCount = sourceSlots.size() - slotOccupancy.size();
    if (collisionCount > 0)
    {
        std::cerr << "[WARNING] Initial task start offsets mapped " << sourceSlots.size()
                  << " unique sources to " << slotOccupancy.size()
                  << " distinct offsets: offset-reuses=" << collisionCount
                  << ", max-sources-per-offset=" << maxSlotOccupancy
                  << "; some initial tasks still share a timestamp" << std::endl;
    }
    return offsets;
}

void
UbTrafficGen::ScheduleTaskCompletionVisibility(uint32_t taskId, Time completionVisibleTs)
{
    const Time now = Simulator::Now();
    NS_ABORT_MSG_IF(completionVisibleTs < now,
                    "task completion visibility timestamp is earlier than Simulator::Now()");

#ifdef NS3_MPI
    if (MpiInterface::IsEnabled() && MpiInterface::GetSize() > 1) {
        for (uint32_t rank = 0; rank < MpiInterface::GetSize(); ++rank) {
            if (rank == MpiInterface::GetSystemId()) {
                continue;
            }
            MpiInterface::SendTaskCompletion(taskId, completionVisibleTs, rank);
        }
    }
#endif

#ifdef NS3_MTP
    if (MtpInterface::isPartitioned()) {
        MtpInterface::ScheduleGlobalAtOrdered(completionVisibleTs, taskId, [this, taskId]() {
            ApplyTaskCompletion(taskId);
        });
        return;
    }
#endif

    Simulator::Schedule(completionVisibleTs - now,
                        &UbTrafficGen::ApplyTaskCompletion,
                        this,
                        taskId);
}

void UbTrafficGen::ScheduleTasks(const ReadyTaskBatch& batch)
{
    const auto initialTaskStartOffsets = GetInitialTaskStartOffsets(batch);
    for (size_t index = 0; index < batch.tasks.size(); ++index) {
        const auto& task = batch.tasks[index];
        if (task.priority == 0) {
            NS_LOG_WARN("It is strongly recommended not to set the task priority to 0. " <<
                        "Priority level 0 is reserved for control frames.");
        }
        Ptr<UbApp> app = index < batch.sourceApps.size() ? batch.sourceApps[index] : nullptr;
        if (app == nullptr) {
            app = DynamicCast<UbApp>(NodeList::GetNode(task.sourceNode)->GetApplication(0));
        }
        NS_ABORT_MSG_IF(app == nullptr, "No UbApp registered for traffic task source node "
                                            << task.sourceNode);
        Simulator::ScheduleWithContext(app->GetNode()->GetId(),
                                       task.delay + initialTaskStartOffsets[index],
                                       &UbTrafficGen::StartTask,
                                       this,
                                       app,
                                       task);
        NS_LOG_DEBUG("Scheduled task " << task.taskId);
    }
}

void
UbTrafficGen::StartTask(Ptr<UbApp> app, RuntimeTask task)
{
    if (task.op == RuntimeTaskOp::LOCAL_READY)
    {
        RecordCanonicalEvent("START", task);
        MarkTaskCompleted(task.taskId);
        return;
    }
    if (task.op == RuntimeTaskOp::COMPUTE)
    {
        SubmitComputeTask(app, task);
        return;
    }

    RecordCanonicalEvent("START", task);
    app->SendTraffic(task);
}

void
UbTrafficGen::SubmitComputeTask(Ptr<UbApp> app, RuntimeTask task)
{
    NS_ABORT_MSG_IF(app == nullptr, "COMPUTE task requires a source application");

    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        TaskState* state = FindTaskStateLocked(task.taskId);
        NS_ABORT_MSG_IF(state == nullptr || *state != TaskState::READY,
                        "COMPUTE task must be READY before it can acquire or queue: "
                            << task.taskId);
        const auto running = m_computeRunningTasks.find(task.sourceNode);
        if (running == m_computeRunningTasks.end())
        {
            m_computeRunningTasks.emplace(task.sourceNode, task.taskId);
            m_computeRunningApps.emplace(task.sourceNode, app);
            *state = TaskState::RUNNING;
        }
        else
        {
            m_computeQueues[task.sourceNode].push_back(ComputeTaskRequest{app, task});
            *state = TaskState::QUEUED;
            queued = true;
        }
    }

    if (queued)
    {
        app->ComputeTaskQueuedNotify(task.sourceNode, task.taskId);
        NS_LOG_DEBUG("Queued COMPUTE task " << task.taskId << " on UBPU " << task.sourceNode);
        return;
    }

    app->ComputeTaskStartsNotify(task.sourceNode, task.taskId);
    RecordCanonicalEvent("START", task);
    app->SendTraffic(task);
}

void UbTrafficGen::OnTaskCompleted(uint32_t taskId)
{
    bool isCompute = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const RuntimeTask* task = FindTaskLocked(taskId);
        isCompute = task != nullptr && task->op == RuntimeTaskOp::COMPUTE;
    }
    if (isCompute)
    {
        OnComputeTaskCompleted(taskId);
        return;
    }
    MarkTaskCompleted(taskId);
}

void
UbTrafficGen::OnComputeTaskCompleted(uint32_t taskId)
{
    RuntimeTask completedTask;
    Ptr<UbApp> completedApp;
    std::optional<ComputeTaskRequest> nextTask;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const RuntimeTask* task = FindTaskLocked(taskId);
        if (task == nullptr || task->op != RuntimeTaskOp::COMPUTE)
        {
            return;
        }

        const auto running = m_computeRunningTasks.find(task->sourceNode);
        if (running == m_computeRunningTasks.end() || running->second != taskId)
        {
            return;
        }

        completedTask = *task;
        const auto appIt = m_computeRunningApps.find(task->sourceNode);
        if (appIt != m_computeRunningApps.end())
        {
            completedApp = appIt->second;
            m_computeRunningApps.erase(appIt);
        }
        m_computeRunningTasks.erase(running);

        auto queueIt = m_computeQueues.find(task->sourceNode);
        if (queueIt != m_computeQueues.end() && !queueIt->second.empty())
        {
            nextTask = std::move(queueIt->second.front());
            queueIt->second.pop_front();
            TaskState* nextState = FindTaskStateLocked(nextTask->task.taskId);
            NS_ABORT_MSG_IF(nextState == nullptr || *nextState != TaskState::QUEUED,
                            "COMPUTE queue state is not QUEUED for task "
                                << nextTask->task.taskId);
            *nextState = TaskState::RUNNING;
            m_computeRunningTasks.emplace(task->sourceNode, nextTask->task.taskId);
            m_computeRunningApps.emplace(task->sourceNode, nextTask->app);
            if (queueIt->second.empty())
            {
                m_computeQueues.erase(queueIt);
            }
        }
    }

    if (completedApp != nullptr)
    {
        completedApp->ComputeTaskCompletesNotify(completedTask.sourceNode, completedTask.taskId);
    }

    if (nextTask.has_value())
    {
        nextTask->app->ComputeTaskStartsNotify(nextTask->task.sourceNode,
                                               nextTask->task.taskId);
        RecordCanonicalEvent("START", nextTask->task);
        nextTask->app->SendTraffic(nextTask->task);
    }

    // Release the local compute slot before entering the existing visibility path.
    MarkTaskCompleted(taskId);
}

void
UbTrafficGen::OnTaskCompleted(uint32_t taskId,
                              Ptr<UbApp> cleanupOwner,
                              uint32_t jettyNum,
                              bool destroyTaskScopedJetty,
                              bool destroyPlannedTpConnections)
{
    NS_ABORT_MSG_IF(cleanupOwner == nullptr, "Task cleanup owner must not be null");
    Ptr<Node> ownerNode = cleanupOwner->GetNode();
    NS_ABORT_MSG_IF(ownerNode == nullptr, "Task cleanup owner must belong to a node");
    uint32_t applicationIndex = ownerNode->GetNApplications();
    for (uint32_t index = 0; index < ownerNode->GetNApplications(); ++index)
    {
        if (PeekPointer(ownerNode->GetApplication(index)) == PeekPointer(cleanupOwner))
        {
            applicationIndex = index;
            break;
        }
    }
    NS_ABORT_MSG_IF(applicationIndex == ownerNode->GetNApplications(),
                    "Task cleanup owner must be installed on its node");
    SubmitTaskCompletion(taskId,
                         TaskCleanupRequest{ownerNode->GetId(),
                                            applicationIndex,
                                            jettyNum,
                                            destroyTaskScopedJetty,
                                            destroyPlannedTpConnections,
                                            {}});
}

void
UbTrafficGen::RegisterMpiTaskCompletionHandler()
{
#ifdef NS3_MPI
    if (MpiInterface::IsEnabled() && MpiInterface::GetSize() > 1) {
        MpiInterface::SetTaskCompletionHandler(
            [this](uint32_t taskId) { this->ApplyTaskCompletion(taskId); });
    }
#endif
}

} // namespace ns3
