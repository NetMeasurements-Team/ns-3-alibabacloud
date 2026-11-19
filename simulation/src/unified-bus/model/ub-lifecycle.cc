// SPDX-License-Identifier: GPL-2.0-only
#include "ub-lifecycle.h"
#include "ns3/ub-mtp-compat.h"

#include "ns3/abort.h"
#include "ns3/node.h"
#include "ns3/node-list.h"
#include "ns3/simulator.h"
#include "ns3/ub-lifecycle-trace.h"
#include "ns3/ub-tp-connection-manager.h"
#include <optional>

#ifdef NS3_MPI
#include "ns3/mpi-interface.h"
#endif
#ifdef NS3_MTP
#include "ns3/mtp-interface.h"
#endif

#include <algorithm>
#include <limits>
#include <tuple>

namespace ns3
{

NS_OBJECT_ENSURE_REGISTERED(UbTransactionCorrelationTag);
NS_OBJECT_ENSURE_REGISTERED(UbLifecycleCoordinator);
NS_OBJECT_ENSURE_REGISTERED(UbParallelLifecycleAdapter);

namespace
{

UbLifecycleTraceRecord
MakeTraceRecord(const UbLifecycleResourceIdentity& identity,
                uint64_t ownerSequence,
                UbLifecycleTraceRecordType recordType,
                UbLifecycleTraceSubjectKind subjectKind,
                uint64_t subjectId,
                UbLifecycleTraceEvent event,
                int64_t before,
                int64_t delta,
                int64_t after,
                UbLifecycleTraceOutcome outcome,
                UbLifecycleTraceReason reason,
                UbLifecycleTraceSite site)
{
    return {Simulator::Now().GetNanoSeconds(),
            identity.ownerNodeId,
            ownerSequence,
            recordType,
            identity.kind,
            identity.localId,
            identity.generation,
            subjectKind,
            subjectId,
            event,
            before,
            delta,
            after,
            outcome,
            reason,
            site};
}

UbLifecycleTraceReason
ToTraceReason(UbLifecycleLeaseKind kind)
{
    switch (kind)
    {
    case UbLifecycleLeaseKind::WORK:
        return UbLifecycleTraceReason::WORK_LEASE;
    case UbLifecycleLeaseKind::CHILD:
        return UbLifecycleTraceReason::CHILD_LEASE;
    case UbLifecycleLeaseKind::TRANSPORT_USE:
        return UbLifecycleTraceReason::TRANSPORT_USE_LEASE;
    case UbLifecycleLeaseKind::BINDING:
        return UbLifecycleTraceReason::BINDING_LEASE;
    case UbLifecycleLeaseKind::TRACKED_EVENT:
        return UbLifecycleTraceReason::TRACKED_EVENT_LEASE;
    case UbLifecycleLeaseKind::TRANSPORT_WORK:
        return UbLifecycleTraceReason::TRANSPORT_WORK_LEASE;
    }
    NS_ABORT_MSG("Unknown lifecycle lease trace reason");
}

UbLifecycleTraceOutcome
ToTraceOutcome(UbWorkOutcome outcome)
{
    switch (outcome)
    {
    case UbWorkOutcome::COMPLETED:
        return UbLifecycleTraceOutcome::COMPLETED;
    case UbWorkOutcome::FAILED:
        return UbLifecycleTraceOutcome::FAILED;
    case UbWorkOutcome::CANCELED:
        return UbLifecycleTraceOutcome::CANCELED;
    case UbWorkOutcome::FLUSHED:
        return UbLifecycleTraceOutcome::FLUSHED;
    }
    NS_ABORT_MSG("Unknown work trace outcome");
}

bool
ShouldTraceResource(const UbLifecycleResourceIdentity& identity)
{
    return UbLifecycleTrace::IsEventEnabled() &&
           (!UbLifecycleTrace::IsDetailEnabled() || UbLifecycleTrace::SelectResource(identity));
}

#ifdef NS3_MPI
constexpr uint32_t UB_LIFECYCLE_CAUSAL_PROTOCOL = 0x55424c31;

ParallelCommunicationInterface::CausalControlRecord
EncodeLifecycleControl(const UbLifecycleControlRecord& record)
{
    ParallelCommunicationInterface::CausalControlRecord wire;
    wire.targetNodeId = record.targetNodeId;
    wire.protocol = UB_LIFECYCLE_CAUSAL_PROTOCOL;
    wire.visibleTs = static_cast<uint64_t>(record.visibleTs);
    wire.orderKey = record.sequence;
    wire.payload[0] = record.sourceNodeId;
    wire.payload[1] = static_cast<uint64_t>(record.operation);
    wire.payload[2] = record.resource.ownerNodeId;
    wire.payload[3] = static_cast<uint64_t>(record.resource.kind);
    wire.payload[4] = record.resource.localId;
    wire.payload[5] = record.resource.generation;
    wire.payload[6] = record.nodeId;
    wire.payload[7] = record.value;
    wire.payload[8] = record.tpn;
    wire.payload[9] = record.requestId;
    return wire;
}

UbLifecycleControlRecord
DecodeLifecycleControl(const ParallelCommunicationInterface::CausalControlRecord& wire)
{
    NS_ABORT_MSG_IF(wire.protocol != UB_LIFECYCLE_CAUSAL_PROTOCOL,
                    "Unknown Unified Bus lifecycle causal-control protocol");
    UbLifecycleControlRecord record;
    record.sourceNodeId = static_cast<uint32_t>(wire.payload[0]);
    record.targetNodeId = wire.targetNodeId;
    record.sequence = wire.orderKey;
    record.visibleTs = static_cast<int64_t>(wire.visibleTs);
    record.operation = static_cast<UbLifecycleControlOperation>(wire.payload[1]);
    record.resource.ownerNodeId = static_cast<uint32_t>(wire.payload[2]);
    record.resource.kind = static_cast<UbLifecycleResourceKind>(wire.payload[3]);
    record.resource.localId = wire.payload[4];
    record.resource.generation = wire.payload[5];
    record.nodeId = static_cast<uint32_t>(wire.payload[6]);
    record.value = wire.payload[7];
    record.tpn = static_cast<uint32_t>(wire.payload[8]);
    record.requestId = wire.payload[9];
    return record;
}

void
ReceiveLifecycleControl(const ParallelCommunicationInterface::CausalControlRecord& wire)
{
    UbParallelLifecycleAdapter::GetOrCreate(NodeList::GetNode(wire.targetNodeId))
        ->InjectControlForTest(DecodeLifecycleControl(wire));
}
#endif

} // namespace

Ptr<Packet>
CopyPacketView(Ptr<const Packet> packet)
{
    NS_ABORT_MSG_IF(packet == nullptr, "Cannot create a parser view of a null packet");
    return packet->Copy();
}

Ptr<Packet>
CopyPacketTransfer(Ptr<const Packet> packet)
{
    NS_ABORT_MSG_IF(packet == nullptr, "Cannot create a transfer copy of a null packet");
    return packet->Copy();
}

namespace
{

void
SerializeResourceIdentity(TagBuffer& buffer, const UbLifecycleResourceIdentity& identity)
{
    buffer.WriteU32(identity.ownerNodeId);
    buffer.WriteU8(static_cast<uint8_t>(identity.kind));
    buffer.WriteU64(identity.localId);
    buffer.WriteU64(identity.generation);
}

UbLifecycleResourceIdentity
DeserializeResourceIdentity(TagBuffer& buffer)
{
    UbLifecycleResourceIdentity identity;
    identity.ownerNodeId = buffer.ReadU32();
    identity.kind = static_cast<UbLifecycleResourceKind>(buffer.ReadU8());
    identity.localId = buffer.ReadU64();
    identity.generation = buffer.ReadU64();
    return identity;
}

} // namespace

std::ostream&
operator<<(std::ostream& os, const UbLifecycleResourceIdentity& identity)
{
    os << "node=" << identity.ownerNodeId << ",kind=" << static_cast<uint32_t>(identity.kind)
       << ",local=" << identity.localId << ",generation=" << identity.generation;
    return os;
}

TypeId
UbTransactionCorrelationTag::GetTypeId()
{
    static TypeId tid = TypeId("ns3::UbTransactionCorrelationTag")
                            .SetParent<Tag>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbTransactionCorrelationTag>();
    return tid;
}

TypeId
UbTransactionCorrelationTag::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
UbTransactionCorrelationTag::GetSerializedSize() const
{
    return 2 * (sizeof(uint32_t) + sizeof(uint8_t) + 2 * sizeof(uint64_t));
}

void
UbTransactionCorrelationTag::Serialize(TagBuffer buffer) const
{
    SerializeResourceIdentity(buffer, m_identity.operation);
    SerializeResourceIdentity(buffer, m_identity.segment);
}

void
UbTransactionCorrelationTag::Deserialize(TagBuffer buffer)
{
    m_identity.operation = DeserializeResourceIdentity(buffer);
    m_identity.segment = DeserializeResourceIdentity(buffer);
}

void
UbTransactionCorrelationTag::Print(std::ostream& os) const
{
    os << "operation={" << m_identity.operation << "},segment={" << m_identity.segment << '}';
}

void
UbTransactionCorrelationTag::SetIdentity(const UbTransactionCorrelationIdentity& identity)
{
    m_identity = identity;
}

UbTransactionCorrelationIdentity
UbTransactionCorrelationTag::GetIdentity() const
{
    return m_identity;
}

TypeId
UbLifecycleCoordinator::GetTypeId()
{
    static TypeId tid = TypeId("ns3::UbLifecycleCoordinator")
                            .SetParent<Object>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbLifecycleCoordinator>()
                            .AddTraceSource("LifecycleEvent",
                                            "Typed lifecycle state and identity event.",
                                            MakeTraceSourceAccessor(
                                                &UbLifecycleCoordinator::m_lifecycleTrace),
                                            "ns3::TracedCallback::UbLifecycleTraceRecord");
    return tid;
}

Ptr<UbLifecycleCoordinator>
UbLifecycleCoordinator::GetOrCreate(Ptr<Node> node)
{
    NS_ABORT_MSG_IF(node == nullptr, "Lifecycle coordinator requires an owning node");
    Ptr<UbLifecycleCoordinator> coordinator = node->GetObject<UbLifecycleCoordinator>();
    if (coordinator == nullptr)
    {
        coordinator = CreateObject<UbLifecycleCoordinator>();
        coordinator->m_ownerNodeId = node->GetId();
        UbLifecycleTrace::PrepareOwner(coordinator->m_ownerNodeId);
        if (UbLifecycleTrace::IsEventEnabled())
        {
            coordinator->m_lifecycleTrace.ConnectWithoutContext(
                MakeCallback(&UbLifecycleTrace::Emit));
        }
        node->AggregateObject(coordinator);
    }
    NS_ABORT_MSG_IF(coordinator->m_ownerNodeId != node->GetId(),
                    "Lifecycle coordinator is aggregated on the wrong node");
    UbParallelLifecycleAdapter::GetOrCreate(node);
    return coordinator;
}

bool
UbLifecycleCoordinator::ResourceKey::operator<(const ResourceKey& other) const
{
    return std::tie(kind, localId) < std::tie(other.kind, other.localId);
}

UbLifecycleResourceIdentity
UbLifecycleCoordinator::OpenResource(UbLifecycleResourceKind kind, uint64_t localId)
{
    const ResourceKey key{kind, localId};
    auto resource = m_resources.find(key);
    if (resource != m_resources.end() && !resource->second.retired)
    {
        m_cachedResourceKey = key;
        m_cachedResourceState = &resource->second;
        return {m_ownerNodeId, kind, localId, resource->second.generation};
    }

    CompactResourceHistory compactHistory;
    const bool reusedCompactHistory =
        resource == m_resources.end() && TakeCompactHistory(kind, localId, compactHistory);
    if (resource == m_resources.end())
    {
        resource = m_resources.try_emplace(key).first;
        if (reusedCompactHistory)
        {
            resource->second.generation = compactHistory.generation;
        }
    }

    ResourceState& state = resource->second;
    m_cachedResourceKey = key;
    m_cachedResourceState = &state;

    NS_ABORT_MSG_IF(state.generation == std::numeric_limits<uint64_t>::max(),
                    "Lifecycle generation exhausted");
    ++state.generation;
    state.retired = false;
    state.hasTerminalOutcome = false;
    state.terminalOutcome = UbWorkOutcome::COMPLETED;
    state.retainedSummary.reset();
    state.active = std::make_unique<ActiveResourceState>();
    const UbLifecycleResourceIdentity identity{m_ownerNodeId, kind, localId, state.generation};
    if (ShouldTraceResource(identity))
    {
        m_lifecycleTrace(MakeTraceRecord(identity,
                                         ++m_traceSequence,
                                         UbLifecycleTraceRecordType::RESOURCE_STATE,
                                         UbLifecycleTraceSubjectKind::RESOURCE,
                                         localId,
                                         UbLifecycleTraceEvent::RESOURCE_OPEN,
                                         0,
                                         1,
                                         1,
                                         UbLifecycleTraceOutcome::NONE,
                                         UbLifecycleTraceReason::NONE,
                                         UbLifecycleTraceSite::COORDINATOR_OPEN));
    }
    return identity;
}

void
UbLifecycleCoordinator::RetireResource(const UbLifecycleResourceIdentity& identity)
{
    ResourceState& state = ValidateMutable(identity);
    ActiveResourceState& active = *state.active;
    NS_ABORT_MSG_IF(!active.activeLeases.empty(),
                    "Cannot retire lifecycle generation with active leases");
    active.snapshot.retired = true;
    if (active.snapshot.completedTerminals != 0)
    {
        state.hasTerminalOutcome = true;
        state.terminalOutcome = UbWorkOutcome::COMPLETED;
    }
    else if (active.snapshot.failedTerminals != 0)
    {
        state.hasTerminalOutcome = true;
        state.terminalOutcome = UbWorkOutcome::FAILED;
    }
    else if (active.snapshot.canceledTerminals != 0)
    {
        state.hasTerminalOutcome = true;
        state.terminalOutcome = UbWorkOutcome::CANCELED;
    }
    else if (active.snapshot.flushedTerminals != 0)
    {
        state.hasTerminalOutcome = true;
        state.terminalOutcome = UbWorkOutcome::FLUSHED;
    }
    if (UbLifecycleTrace::GetLevel() != UbLifecycleTraceLevel::OFF)
    {
        state.retainedSummary =
            std::make_unique<UbLifecycleResourceSnapshot>(active.snapshot);
    }
    state.retired = true;
    if (ShouldTraceResource(identity))
    {
        m_lifecycleTrace(MakeTraceRecord(identity,
                                                ++m_traceSequence,
                                                UbLifecycleTraceRecordType::RESOURCE_STATE,
                                                UbLifecycleTraceSubjectKind::RESOURCE,
                                                identity.localId,
                                                UbLifecycleTraceEvent::RESOURCE_RETIRE,
                                                0,
                                                1,
                                                1,
                                                UbLifecycleTraceOutcome::NONE,
                                                UbLifecycleTraceReason::NONE,
                                         UbLifecycleTraceSite::COORDINATOR_RETIRE));
    }
    state.active.reset();
    if (UbLifecycleTrace::GetLevel() == UbLifecycleTraceLevel::OFF)
    {
        const CompactResourceHistory history{state.generation,
                                             state.hasTerminalOutcome,
                                             state.terminalOutcome};
        const ResourceKey key{identity.kind, identity.localId};
        m_cachedResourceState = nullptr;
        const std::size_t erased = m_resources.erase(key);
        NS_ABORT_MSG_IF(erased != 1, "Retired lifecycle resource lost its canonical state");
        StoreCompactHistory(identity.kind, identity.localId, history);
    }
}

bool
UbLifecycleCoordinator::IsResourceActive(const UbLifecycleResourceIdentity& identity) const
{
    if (identity.ownerNodeId != m_ownerNodeId || identity.generation == 0)
    {
        return false;
    }
    const auto resource = m_resources.find({identity.kind, identity.localId});
    return resource != m_resources.end() &&
           resource->second.generation == identity.generation &&
           !resource->second.retired;
}

void
UbLifecycleCoordinator::SetResourceStateChangedCallback(
    const UbLifecycleResourceIdentity& identity,
    Callback<void, UbLifecycleResourceIdentity> callback)
{
    ValidateMutable(identity).active->stateChangedCallback = callback;
}

UbLifecycleLeaseIdentity
UbLifecycleCoordinator::AcquireLease(const UbLifecycleResourceIdentity& owner,
                                     UbLifecycleLeaseKind kind,
                                     const UbLifecycleResourceIdentity& related)
{
    ResourceState& state = ValidateMutable(owner);
    const ResourceValidation relatedState = Validate(related);
    NS_ABORT_MSG_IF(relatedState.compactHistory != nullptr || relatedState.state->retired,
                    "Lifecycle lease cannot reference a retired resource");
    ActiveResourceState& active = *state.active;
    NS_ABORT_MSG_IF(active.nextLease == std::numeric_limits<uint64_t>::max(),
                    "Lifecycle lease identity exhausted");
    UbLifecycleLeaseIdentity lease{owner, related, kind, ++active.nextLease};
    const bool inserted = active.activeLeases.emplace(lease.lease, lease).second;
    NS_ABORT_MSG_IF(!inserted, "Duplicate lifecycle lease identity");
    switch (kind)
    {
    case UbLifecycleLeaseKind::WORK:
        ++active.snapshot.activeWorkLeases;
        break;
    case UbLifecycleLeaseKind::CHILD:
        ++active.snapshot.activeChildLeases;
        break;
    case UbLifecycleLeaseKind::TRANSPORT_USE:
        ++active.snapshot.activeTransportUseLeases;
        break;
    case UbLifecycleLeaseKind::BINDING:
        ++active.snapshot.activeBindingLeases;
        break;
    case UbLifecycleLeaseKind::TRACKED_EVENT:
        ++active.snapshot.activeTrackedEventLeases;
        break;
    case UbLifecycleLeaseKind::TRANSPORT_WORK:
        ++active.snapshot.activeTransportWorkLeases;
        break;
    default:
        NS_ABORT_MSG("Unknown lifecycle lease kind");
    }
    if (UbLifecycleTrace::IsDetailEnabled() && UbLifecycleTrace::SelectResource(owner))
    {
        m_lifecycleTrace(MakeTraceRecord(owner,
                                                ++m_traceSequence,
                                                UbLifecycleTraceRecordType::LEDGER_DELTA,
                                                UbLifecycleTraceSubjectKind::LEASE,
                                                lease.lease,
                                                UbLifecycleTraceEvent::LEASE_ACQUIRE,
                                                0,
                                                1,
                                                1,
                                                UbLifecycleTraceOutcome::NONE,
                                                ToTraceReason(kind),
                                                UbLifecycleTraceSite::COORDINATOR_ACQUIRE_LEASE));
    }
    return lease;
}

void
UbLifecycleCoordinator::ReleaseLease(const UbLifecycleLeaseIdentity& lease)
{
    ResourceState& state = ValidateMutable(lease.owner);
    ActiveResourceState& active = *state.active;
    const auto it = active.activeLeases.find(lease.lease);
    NS_ABORT_MSG_IF(it == active.activeLeases.end(),
                    "Lifecycle lease release would underflow the active balance");
    NS_ABORT_MSG_IF(it->second != lease, "Lifecycle lease identity mismatch");
    switch (lease.kind)
    {
    case UbLifecycleLeaseKind::WORK:
        NS_ABORT_MSG_IF(active.snapshot.activeWorkLeases == 0,
                        "Lifecycle work lease counter underflow");
        --active.snapshot.activeWorkLeases;
        break;
    case UbLifecycleLeaseKind::CHILD:
        NS_ABORT_MSG_IF(active.snapshot.activeChildLeases == 0,
                        "Lifecycle child lease counter underflow");
        --active.snapshot.activeChildLeases;
        break;
    case UbLifecycleLeaseKind::TRANSPORT_USE:
        NS_ABORT_MSG_IF(active.snapshot.activeTransportUseLeases == 0,
                        "Lifecycle transport-use lease counter underflow");
        --active.snapshot.activeTransportUseLeases;
        break;
    case UbLifecycleLeaseKind::BINDING:
        NS_ABORT_MSG_IF(active.snapshot.activeBindingLeases == 0,
                        "Lifecycle binding lease counter underflow");
        --active.snapshot.activeBindingLeases;
        break;
    case UbLifecycleLeaseKind::TRACKED_EVENT:
        NS_ABORT_MSG_IF(active.snapshot.activeTrackedEventLeases == 0,
                        "Lifecycle tracked-event lease counter underflow");
        --active.snapshot.activeTrackedEventLeases;
        break;
    case UbLifecycleLeaseKind::TRANSPORT_WORK:
        NS_ABORT_MSG_IF(active.snapshot.activeTransportWorkLeases == 0,
                        "Lifecycle transport-work lease counter underflow");
        --active.snapshot.activeTransportWorkLeases;
        break;
    default:
        NS_ABORT_MSG("Unknown lifecycle lease kind");
    }
    active.activeLeases.erase(it);
    if (UbLifecycleTrace::IsDetailEnabled() && UbLifecycleTrace::SelectResource(lease.owner))
    {
        m_lifecycleTrace(MakeTraceRecord(lease.owner,
                                                ++m_traceSequence,
                                                UbLifecycleTraceRecordType::LEDGER_DELTA,
                                                UbLifecycleTraceSubjectKind::LEASE,
                                                lease.lease,
                                                UbLifecycleTraceEvent::LEASE_RELEASE,
                                                1,
                                                -1,
                                                0,
                                                UbLifecycleTraceOutcome::NONE,
                                                ToTraceReason(lease.kind),
                                                UbLifecycleTraceSite::COORDINATOR_RELEASE_LEASE));
    }
    const auto callback = active.stateChangedCallback;
    if (!callback.IsNull())
    {
        callback(lease.owner);
    }
}

void
UbLifecycleCoordinator::RecordSuccessfulTerminal(const UbLifecycleResourceIdentity& identity)
{
    RecordWorkTerminal(identity, UbWorkOutcome::COMPLETED);
}

void
UbLifecycleCoordinator::RecordWorkTerminal(const UbLifecycleResourceIdentity& identity,
                                           UbWorkOutcome outcome)
{
    ResourceState& state = ValidateMutable(identity);
    ActiveResourceState& active = *state.active;
    const uint64_t terminalCount = active.snapshot.completedTerminals +
                                   active.snapshot.failedTerminals +
                                   active.snapshot.canceledTerminals +
                                   active.snapshot.flushedTerminals;
    NS_ABORT_MSG_IF(terminalCount != 0,
                    "Lifecycle resource already has a terminal outcome");
    switch (outcome)
    {
    case UbWorkOutcome::COMPLETED:
        ++active.snapshot.completedTerminals;
        ++active.snapshot.successfulTerminals;
        break;
    case UbWorkOutcome::FAILED:
        ++active.snapshot.failedTerminals;
        break;
    case UbWorkOutcome::CANCELED:
        ++active.snapshot.canceledTerminals;
        break;
    case UbWorkOutcome::FLUSHED:
        ++active.snapshot.flushedTerminals;
        break;
    default:
        NS_ABORT_MSG("Unknown work terminal outcome");
    }
    if (ShouldTraceResource(identity))
    {
        m_lifecycleTrace(MakeTraceRecord(identity,
                                                ++m_traceSequence,
                                                UbLifecycleTraceRecordType::TERMINAL_OUTCOME,
                                                UbLifecycleTraceSubjectKind::WORK,
                                                identity.localId,
                                                UbLifecycleTraceEvent::WORK_TERMINAL,
                                                0,
                                                1,
                                                1,
                                                ToTraceOutcome(outcome),
                                                UbLifecycleTraceReason::NONE,
                                                UbLifecycleTraceSite::COORDINATOR_WORK_TERMINAL));
    }
}

void
UbLifecycleCoordinator::EmitLifecycleSummary()
{
    if (UbLifecycleTrace::GetLevel() == UbLifecycleTraceLevel::OFF)
    {
        return;
    }

    constexpr std::size_t kSummaryRecordsPerResource = 12;
    std::vector<UbLifecycleTraceRecord> records;
    records.reserve(m_resources.size() * kSummaryRecordsPerResource);
    for (const auto& [key, state] : m_resources)
    {
        const UbLifecycleResourceSnapshot snapshot = BuildSnapshot(state);
        const UbLifecycleResourceIdentity identity{m_ownerNodeId,
                                                   key.kind,
                                                   key.localId,
                                                   state.generation};
        const auto emit = [&](UbLifecycleTraceEvent event, uint64_t value) {
            records.push_back(MakeTraceRecord(identity,
                                              ++m_traceSequence,
                                              UbLifecycleTraceRecordType::SUMMARY,
                                              UbLifecycleTraceSubjectKind::RESOURCE,
                                              key.localId,
                                              event,
                                              UbLifecycleTrace::NOT_APPLICABLE,
                                              UbLifecycleTrace::NOT_APPLICABLE,
                                              static_cast<int64_t>(value),
                                              UbLifecycleTraceOutcome::NONE,
                                              UbLifecycleTraceReason::NONE,
                                              UbLifecycleTraceSite::COORDINATOR_SUMMARY));
        };
        emit(UbLifecycleTraceEvent::SUMMARY_ACTIVE_WORK_LEASES,
             snapshot.activeWorkLeases);
        emit(UbLifecycleTraceEvent::SUMMARY_ACTIVE_CHILD_LEASES,
             snapshot.activeChildLeases);
        emit(UbLifecycleTraceEvent::SUMMARY_ACTIVE_TRANSPORT_USE_LEASES,
             snapshot.activeTransportUseLeases);
        emit(UbLifecycleTraceEvent::SUMMARY_ACTIVE_BINDING_LEASES,
             snapshot.activeBindingLeases);
        emit(UbLifecycleTraceEvent::SUMMARY_ACTIVE_TRACKED_EVENT_LEASES,
             snapshot.activeTrackedEventLeases);
        emit(UbLifecycleTraceEvent::SUMMARY_ACTIVE_TRANSPORT_WORK_LEASES,
             snapshot.activeTransportWorkLeases);
        emit(UbLifecycleTraceEvent::SUMMARY_SUCCESSFUL_TERMINALS,
             snapshot.successfulTerminals);
        emit(UbLifecycleTraceEvent::SUMMARY_COMPLETED_TERMINALS,
             snapshot.completedTerminals);
        emit(UbLifecycleTraceEvent::SUMMARY_FAILED_TERMINALS, snapshot.failedTerminals);
        emit(UbLifecycleTraceEvent::SUMMARY_CANCELED_TERMINALS, snapshot.canceledTerminals);
        emit(UbLifecycleTraceEvent::SUMMARY_FLUSHED_TERMINALS, snapshot.flushedTerminals);
        emit(UbLifecycleTraceEvent::SUMMARY_RETIRED, snapshot.retired ? 1 : 0);
    }
    UbLifecycleTrace::EmitBatch(records);
}

UbLifecycleResourceSnapshot
UbLifecycleCoordinator::GetSnapshot(const UbLifecycleResourceIdentity& owner) const
{
    return BuildSnapshot(Validate(owner));
}

UbLifecycleStorageSnapshot
UbLifecycleCoordinator::GetStorageSnapshot() const
{
    UbLifecycleStorageSnapshot storage;
    storage.resourceCount = m_resources.size();
    for (const auto& [kind, runs] : m_compactHistories)
    {
        UbLifecycleKindStorageSnapshot& kindStorage = storage.byKind[kind];
        storage.compactHistoryRunCount += runs.size();
        kindStorage.compactHistoryRunCount += runs.size();
        for (const auto& [firstLocalId, run] : runs)
        {
            const std::size_t resourceCount =
                static_cast<std::size_t>(run.lastLocalId - firstLocalId) + 1;
            storage.resourceCount += resourceCount;
            kindStorage.resourceCount += resourceCount;
        }
    }
    for (const auto& [key, state] : m_resources)
    {
        UbLifecycleKindStorageSnapshot& kindStorage = storage.byKind[key.kind];
        ++kindStorage.resourceCount;
        if (state.active != nullptr)
        {
            ++storage.activeStateCount;
            ++kindStorage.activeStateCount;
            storage.activeLeaseCount += state.active->activeLeases.size();
            kindStorage.activeLeaseCount += state.active->activeLeases.size();
        }
        if (state.retainedSummary != nullptr)
        {
            ++storage.retainedSummaryCount;
            ++kindStorage.retainedSummaryCount;
        }
    }
    return storage;
}

TypeId
UbParallelLifecycleAdapter::GetTypeId()
{
    static TypeId tid = TypeId("ns3::UbParallelLifecycleAdapter")
                            .SetParent<Object>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbParallelLifecycleAdapter>();
    return tid;
}

Ptr<UbParallelLifecycleAdapter>
UbParallelLifecycleAdapter::GetOrCreate(Ptr<Node> node)
{
    NS_ABORT_MSG_IF(node == nullptr, "Parallel lifecycle adapter requires an owning node");
    Ptr<UbParallelLifecycleAdapter> adapter = node->GetObject<UbParallelLifecycleAdapter>();
    if (adapter == nullptr)
    {
        adapter = CreateObject<UbParallelLifecycleAdapter>();
        adapter->m_nodeId = node->GetId();
        node->AggregateObject(adapter);
    }
    NS_ABORT_MSG_IF(adapter->m_nodeId != node->GetId(),
                    "Parallel lifecycle adapter is aggregated on the wrong node");
#ifdef NS3_MPI
    if (MpiInterface::IsEnabled())
    {
        MpiInterface::SetCausalControlHandler(&ReceiveLifecycleControl);
    }
#endif
    return adapter;
}

bool
UbParallelLifecycleAdapter::IsExecutionOwner(uint32_t nodeId)
{
    NS_ABORT_MSG_IF(nodeId >= NodeList::GetNNodes(),
                    "Lifecycle control references an unknown owner node");
#ifdef NS3_MPI
    if (MpiInterface::IsEnabled() &&
        NodeList::GetNode(nodeId)->GetSystemId() != MpiInterface::GetSystemId())
    {
        return false;
    }
#endif
#ifdef NS3_MTP
    if (MtpInterface::isEnabled())
    {
        if (!MtpInterface::isPartitioned())
        {
            return true;
        }
        const std::optional<uint32_t> ownerLp = UbFindNodeLocalLpId(nodeId);
        LogicalProcess* current = MtpInterface::GetSystem();
        if (!ownerLp.has_value() || current == nullptr || *ownerLp != current->GetSystemId())
        {
            return false;
        }
    }
#endif
    return true;
}

uint32_t
UbParallelLifecycleAdapter::RequireSourceNodeId(uint32_t preferredNodeId)
{
    const uint32_t sourceNodeId = preferredNodeId == Simulator::NO_CONTEXT
                                      ? Simulator::GetContext()
                                      : preferredNodeId;
    NS_ABORT_MSG_IF(sourceNodeId == Simulator::NO_CONTEXT || sourceNodeId >= NodeList::GetNNodes(),
                    "Cross-owner lifecycle control requires a source node context");
    return sourceNodeId;
}

void
UbParallelLifecycleAdapter::Route(UbLifecycleControlRecord record)
{
    GetOrCreate(NodeList::GetNode(record.sourceNodeId))->Send(record);
}

bool
UbParallelLifecycleAdapter::RouteControl(UbLifecycleControlRecord record)
{
    if (IsExecutionOwner(record.targetNodeId))
    {
        return GetOrCreate(NodeList::GetNode(record.targetNodeId))->Apply(record);
    }
    record.sourceNodeId = RequireSourceNodeId(record.sourceNodeId);
    Route(record);
    return true;
}

void
UbParallelLifecycleAdapter::Send(UbLifecycleControlRecord record)
{
    record.sequence = ++m_nextOutboundSequence[record.targetNodeId];
    ++m_sent;
#ifdef NS3_MPI
    if (MpiInterface::IsEnabled() &&
        NodeList::GetNode(record.targetNodeId)->GetSystemId() != MpiInterface::GetSystemId())
    {
        NS_ABORT_MSG_IF(!MpiInterface::SupportsCausalControl(),
                        "Active MPI transport does not support lifecycle causal control");
        record.visibleTs =
            (Simulator::Now() + MpiInterface::GetCausalControlLookAhead()).GetInteger();
        MpiInterface::SendCausalControl(EncodeLifecycleControl(record));
        return;
    }
#endif
#ifdef NS3_MTP
    if (MtpInterface::isEnabled())
    {
        const Time visibilityDelay =
            UbGetCausalDeliveryDelay(record.targetNodeId);
        record.visibleTs = (Simulator::Now() + visibilityDelay).GetInteger();
        Simulator::ScheduleWithContext(record.targetNodeId,
                                       visibilityDelay,
                                       &UbParallelLifecycleAdapter::ReceiveAtTarget,
                                       record);
        return;
    }
#endif
    NS_ABORT_MSG("Cross-owner lifecycle control has no supported transport");
}

void
UbParallelLifecycleAdapter::ReceiveAtTarget(UbLifecycleControlRecord record)
{
    GetOrCreate(NodeList::GetNode(record.targetNodeId))->Receive(record);
}

void
UbParallelLifecycleAdapter::InjectControlForTest(const UbLifecycleControlRecord& record)
{
    Receive(record);
}

void
UbParallelLifecycleAdapter::Receive(UbLifecycleControlRecord record)
{
    NS_ABORT_MSG_IF(record.targetNodeId != m_nodeId,
                    "Lifecycle control was delivered to the wrong target adapter");
    NS_ABORT_MSG_IF(!IsExecutionOwner(m_nodeId),
                    "Lifecycle control was not delivered to the target execution owner");
    ++m_received;
    InboundSequence& inbound = m_inbound[record.sourceNodeId];
    if (record.sequence < inbound.next)
    {
        ++m_stale;
        return;
    }
    if (record.sequence > inbound.next)
    {
        if (!inbound.pending.emplace(record.sequence, record).second)
        {
            ++m_duplicate;
        }
        return;
    }

    while (true)
    {
        if (Apply(record))
        {
            ++m_applied;
            ++m_ownerSequence;
        }
        else
        {
            ++m_stale;
        }
        ++inbound.next;
        auto pending = inbound.pending.find(inbound.next);
        if (pending == inbound.pending.end())
        {
            break;
        }
        record = pending->second;
        inbound.pending.erase(pending);
    }
}

bool
UbParallelLifecycleAdapter::Apply(const UbLifecycleControlRecord& record)
{
    return utils::TpConnectionManager::ApplyLifecycleControl(record);
}

UbParallelLifecycleSnapshot
UbParallelLifecycleAdapter::GetSnapshot() const
{
    UbParallelLifecycleSnapshot snapshot;
    snapshot.ownerSequence = m_ownerSequence;
    snapshot.sent = m_sent;
    snapshot.received = m_received;
    snapshot.applied = m_applied;
    snapshot.duplicate = m_duplicate;
    snapshot.stale = m_stale;
    for (const auto& [source, inbound] : m_inbound)
    {
        (void)source;
        snapshot.pending += inbound.pending.size();
    }
    return snapshot;
}

UbLifecycleCoordinator::ResourceState&
UbLifecycleCoordinator::ValidateMutable(const UbLifecycleResourceIdentity& identity)
{
    NS_ABORT_MSG_IF(identity.ownerNodeId != m_ownerNodeId,
                    "Lifecycle mutation routed to the wrong node coordinator");
    if (m_cachedResourceState == nullptr || m_cachedResourceKey.kind != identity.kind ||
        m_cachedResourceKey.localId != identity.localId)
    {
        const auto it = m_resources.find({identity.kind, identity.localId});
        if (it == m_resources.end())
        {
            const CompactResourceHistory* history =
                FindCompactHistory(identity.kind, identity.localId);
            NS_ABORT_MSG_IF(history == nullptr,
                            "Lifecycle mutation references an unknown resource");
            NS_ABORT_MSG_IF(identity.generation != history->generation,
                            "Lifecycle mutation references a stale generation");
            NS_ABORT_MSG("Lifecycle mutation after resource retirement");
        }
        m_cachedResourceKey = it->first;
        m_cachedResourceState = &it->second;
    }
    NS_ABORT_MSG_IF(identity.generation != m_cachedResourceState->generation,
                    "Lifecycle mutation references a stale generation");
    NS_ABORT_MSG_IF(m_cachedResourceState->retired,
                    "Lifecycle mutation after resource retirement");
    NS_ABORT_MSG_IF(m_cachedResourceState->active == nullptr,
                    "Active lifecycle generation has no mutable state");
    return *m_cachedResourceState;
}

UbLifecycleCoordinator::ResourceValidation
UbLifecycleCoordinator::Validate(const UbLifecycleResourceIdentity& identity) const
{
    NS_ABORT_MSG_IF(identity.ownerNodeId != m_ownerNodeId,
                    "Lifecycle query routed to the wrong node coordinator");
    const auto it = m_resources.find({identity.kind, identity.localId});
    if (it != m_resources.end())
    {
        NS_ABORT_MSG_IF(identity.generation != it->second.generation,
                        "Lifecycle query references a stale generation");
        return {&it->second, nullptr};
    }

    const CompactResourceHistory* history =
        FindCompactHistory(identity.kind, identity.localId);
    NS_ABORT_MSG_IF(history == nullptr, "Lifecycle query references an unknown resource");
    NS_ABORT_MSG_IF(identity.generation != history->generation,
                    "Lifecycle query references a stale generation");
    return {nullptr, history};
}

const UbLifecycleCoordinator::CompactResourceHistory*
UbLifecycleCoordinator::FindCompactHistory(UbLifecycleResourceKind kind, uint64_t localId) const
{
    const auto histories = m_compactHistories.find(kind);
    if (histories == m_compactHistories.end())
    {
        return nullptr;
    }
    auto run = histories->second.upper_bound(localId);
    if (run == histories->second.begin())
    {
        return nullptr;
    }
    --run;
    return localId <= run->second.lastLocalId ? &run->second.history : nullptr;
}

bool
UbLifecycleCoordinator::TakeCompactHistory(UbLifecycleResourceKind kind,
                                           uint64_t localId,
                                           CompactResourceHistory& history)
{
    auto histories = m_compactHistories.find(kind);
    if (histories == m_compactHistories.end())
    {
        return false;
    }
    auto& runs = histories->second;
    auto run = runs.upper_bound(localId);
    if (run == runs.begin())
    {
        return false;
    }
    --run;
    if (localId > run->second.lastLocalId)
    {
        return false;
    }

    const uint64_t firstLocalId = run->first;
    const uint64_t lastLocalId = run->second.lastLocalId;
    history = run->second.history;
    runs.erase(run);
    if (firstLocalId < localId)
    {
        runs.emplace(firstLocalId, CompactHistoryRun{localId - 1, history});
    }
    if (localId < lastLocalId)
    {
        runs.emplace(localId + 1, CompactHistoryRun{lastLocalId, history});
    }
    if (runs.empty())
    {
        m_compactHistories.erase(histories);
    }
    return true;
}

void
UbLifecycleCoordinator::StoreCompactHistory(UbLifecycleResourceKind kind,
                                            uint64_t localId,
                                            const CompactResourceHistory& history)
{
    NS_ABORT_MSG_IF(FindCompactHistory(kind, localId) != nullptr,
                    "Lifecycle compact history already contains the retired local ID");
    auto& runs = m_compactHistories[kind];
    auto [run, inserted] =
        runs.emplace(localId, CompactHistoryRun{localId, history});
    NS_ABORT_MSG_IF(!inserted, "Duplicate lifecycle compact history run");

    if (run != runs.begin())
    {
        auto previous = std::prev(run);
        if (previous->second.lastLocalId != std::numeric_limits<uint64_t>::max() &&
            previous->second.lastLocalId + 1 == run->first &&
            previous->second.history == run->second.history)
        {
            previous->second.lastLocalId = run->second.lastLocalId;
            runs.erase(run);
            run = previous;
        }
    }

    auto next = std::next(run);
    if (next != runs.end() && run->second.lastLocalId != std::numeric_limits<uint64_t>::max() &&
        run->second.lastLocalId + 1 == next->first && run->second.history == next->second.history)
    {
        run->second.lastLocalId = next->second.lastLocalId;
        runs.erase(next);
    }
}

UbLifecycleResourceSnapshot
UbLifecycleCoordinator::BuildSnapshot(const ResourceState& state)
{
    if (state.active != nullptr)
    {
        return state.active->snapshot;
    }
    if (state.retainedSummary != nullptr)
    {
        return *state.retainedSummary;
    }

    UbLifecycleResourceSnapshot snapshot;
    snapshot.retired = state.retired;
    if (!state.hasTerminalOutcome)
    {
        return snapshot;
    }
    switch (state.terminalOutcome)
    {
    case UbWorkOutcome::COMPLETED:
        snapshot.successfulTerminals = 1;
        snapshot.completedTerminals = 1;
        break;
    case UbWorkOutcome::FAILED:
        snapshot.failedTerminals = 1;
        break;
    case UbWorkOutcome::CANCELED:
        snapshot.canceledTerminals = 1;
        break;
    case UbWorkOutcome::FLUSHED:
        snapshot.flushedTerminals = 1;
        break;
    default:
        NS_ABORT_MSG("Unknown compact lifecycle terminal outcome");
    }
    return snapshot;
}

UbLifecycleResourceSnapshot
UbLifecycleCoordinator::BuildSnapshot(const ResourceValidation& validation)
{
    if (validation.state != nullptr)
    {
        return BuildSnapshot(*validation.state);
    }

    NS_ABORT_MSG_IF(validation.compactHistory == nullptr,
                    "Lifecycle validation has no resource state");
    ResourceState compactState;
    compactState.generation = validation.compactHistory->generation;
    compactState.retired = true;
    compactState.hasTerminalOutcome = validation.compactHistory->hasTerminalOutcome;
    compactState.terminalOutcome = validation.compactHistory->terminalOutcome;
    return BuildSnapshot(compactState);
}

void
UbTrackedEventSet::Init(Ptr<UbLifecycleCoordinator> coordinator)
{
    NS_ABORT_MSG_IF(coordinator == nullptr, "Tracked events require a lifecycle coordinator");
    NS_ABORT_MSG_IF(m_coordinator != nullptr && m_coordinator != coordinator,
                    "Tracked events cannot replace their lifecycle coordinator");
    m_coordinator = coordinator;
}

uint64_t
UbTrackedEventSet::Track(const UbLifecycleResourceIdentity& owner)
{
    NS_ABORT_MSG_IF(m_coordinator == nullptr, "Tracked events have no lifecycle coordinator");
    NS_ABORT_MSG_IF(m_nextToken == std::numeric_limits<uint64_t>::max(),
                    "Tracked-event token exhausted");
    const uint64_t token = ++m_nextToken;
    Record record;
    record.owner = owner;
    record.lease = m_coordinator->AcquireLease(owner,
                                               UbLifecycleLeaseKind::TRACKED_EVENT,
                                               owner);
    NS_ABORT_MSG_IF(!m_records.emplace(token, record).second,
                    "Duplicate tracked-event token");
    return token;
}

void
UbTrackedEventSet::Attach(const UbLifecycleResourceIdentity& owner,
                          uint64_t token,
                          EventId event,
                          Callback<void> cancelCleanup)
{
    auto tracked = m_records.find(token);
    NS_ABORT_MSG_IF(tracked == m_records.end() || tracked->second.owner != owner,
                    "Cannot attach an unknown tracked event");
    NS_ABORT_MSG_IF(!tracked->second.event.IsExpired(),
                    "Tracked event already has an attached EventId");
    NS_ABORT_MSG_IF(event.IsExpired(), "Tracked event must attach a pending EventId");
    tracked->second.event = event;
    tracked->second.cancelCleanup = cancelCleanup;
}

bool
UbTrackedEventSet::Begin(const UbLifecycleResourceIdentity& owner, uint64_t token)
{
    auto tracked = m_records.find(token);
    if (tracked == m_records.end() || tracked->second.owner != owner)
    {
        return false;
    }
    NS_ABORT_MSG_IF(tracked->second.phase != Phase::PENDING,
                    "Tracked event began more than once");
    tracked->second.phase = Phase::RUNNING;
    return true;
}

void
UbTrackedEventSet::Resolve(const UbLifecycleResourceIdentity& owner, uint64_t token)
{
    const auto event = m_records.find(token);
    NS_ABORT_MSG_IF(event == m_records.end() || event->second.owner != owner,
                    "Tracked event release would underflow");
    NS_ABORT_MSG_IF(event->second.phase != Phase::RUNNING,
                    "Tracked event resolved before its handler began");
    m_coordinator->ReleaseLease(event->second.lease);
    m_records.erase(event);
}

bool
UbTrackedEventSet::HasEvents(const UbLifecycleResourceIdentity& owner) const
{
    return std::any_of(m_records.begin(),
                       m_records.end(),
                       [&owner](const auto& entry) { return entry.second.owner == owner; });
}

std::vector<Callback<void>>
UbTrackedEventSet::CancelPending(const UbLifecycleResourceIdentity& owner)
{
    std::vector<Callback<void>> cancelCleanups;
    for (auto tracked = m_records.begin(); tracked != m_records.end();)
    {
        if (tracked->second.owner != owner || tracked->second.phase == Phase::RUNNING)
        {
            ++tracked;
            continue;
        }
        if (!tracked->second.event.IsExpired())
        {
            tracked->second.event.Cancel();
        }
        Callback<void> cancelCleanup = tracked->second.cancelCleanup;
        tracked->second.cancelCleanup = {};
        if (!cancelCleanup.IsNull())
        {
            cancelCleanups.push_back(cancelCleanup);
        }
        m_coordinator->ReleaseLease(tracked->second.lease);
        tracked = m_records.erase(tracked);
    }
    return cancelCleanups;
}

bool
UbTrackedEventSet::IsEmpty() const
{
    return m_records.empty();
}

void
UbTrackedEventSet::Clear()
{
    NS_ABORT_MSG_IF(!m_records.empty(), "Cannot clear active tracked events");
    m_coordinator = nullptr;
}

} // namespace ns3
