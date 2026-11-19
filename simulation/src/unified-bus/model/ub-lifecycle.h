// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_LIFECYCLE_H
#define UB_LIFECYCLE_H

#include "ns3/callback.h"
#include "ns3/event-id.h"
#include "ns3/object.h"
#include "ns3/simulator.h"
#include "ns3/tag.h"
#include "ns3/traced-callback.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <vector>

namespace ns3
{

class Node;
class Packet;
struct UbLifecycleTraceRecord;

enum class UbLifecycleResourceKind : uint8_t
{
    RTP_ENDPOINT = 1,
    WQE = 2,
    TRANSACTION_OPERATION = 3,
    TRANSACTION_SEGMENT = 4,
    JETTY = 5,
    RTP_TPG = 6,
    RTP_CONNECTION = 7,
    CTP_SERVICE = 8,
    LDST_INSTANCE = 9,
    LDST_SEGMENT = 10,
};

enum class UbLifecycleLeaseKind : uint8_t
{
    WORK = 1,
    CHILD = 2,
    TRANSPORT_USE = 3,
    BINDING = 4,
    TRACKED_EVENT = 5,
    TRANSPORT_WORK = 6,
};

enum class UbWorkOutcome : uint8_t
{
    COMPLETED = 0,
    FAILED = 1,
    CANCELED = 2,
    FLUSHED = 3,
};

enum class UbLifecycleControlOperation : uint8_t
{
    ENDPOINT_MATERIALIZED = 3,
    CONNECTION_DESTROY_REQUEST = 4,
    ENDPOINT_DESTROY_REQUEST = 5,
    ENDPOINT_DESTROYED = 6,
    ERASE_CONNECTION_VIEW = 7,
    ERASE_CONNECTION_VIEW_COMPLETE = 8,
    CONNECTION_DESTROY_RESULT = 9,
    CONNECTION_DESTROY_NOTICE = 10,
    CONNECTION_FREEZE_NOTICE = 11,
};

struct UbLifecycleResourceIdentity
{
    uint32_t ownerNodeId{0};
    UbLifecycleResourceKind kind{UbLifecycleResourceKind::RTP_ENDPOINT};
    uint64_t localId{0};
    uint64_t generation{0};

    bool operator==(const UbLifecycleResourceIdentity& other) const
    {
        return ownerNodeId == other.ownerNodeId && kind == other.kind &&
               localId == other.localId && generation == other.generation;
    }
    bool operator!=(const UbLifecycleResourceIdentity& other) const { return !(*this == other); }
};

enum class UbDestroyReason : uint8_t
{
    EXPLICIT = 0,
    PROTOCOL_COMPLETE = 1,
    ERROR = 2,
    SIMULATION_STOP = 3,
};

enum class UbDestroyStatus : uint8_t
{
    COMPLETED = 0,
    PENDING = 1,
    NOT_FOUND = 2,
    REJECTED = 3,
};

using UbDestroyCallback = Callback<void, UbLifecycleResourceIdentity, UbDestroyStatus>;

struct UbLifecycleControlRecord
{
    uint32_t sourceNodeId{Simulator::NO_CONTEXT};
    uint32_t targetNodeId{0};
    uint64_t sequence{0};
    int64_t visibleTs{0};
    UbLifecycleControlOperation operation{UbLifecycleControlOperation::ENDPOINT_MATERIALIZED};
    UbLifecycleResourceIdentity resource;
    uint32_t nodeId{0};
    uint32_t tpn{0};
    uint64_t requestId{0};
    uint64_t value{0};
};

struct UbParallelLifecycleSnapshot
{
    uint64_t ownerSequence{0};
    uint64_t sent{0};
    uint64_t received{0};
    uint64_t applied{0};
    uint64_t duplicate{0};
    uint64_t stale{0};
    uint64_t pending{0};
};

struct UbLifecycleLeaseIdentity
{
    UbLifecycleResourceIdentity owner;
    UbLifecycleResourceIdentity related;
    UbLifecycleLeaseKind kind{UbLifecycleLeaseKind::WORK};
    uint64_t lease{0};

    bool operator==(const UbLifecycleLeaseIdentity& other) const
    {
        return owner == other.owner && related == other.related && kind == other.kind &&
               lease == other.lease;
    }
    bool operator!=(const UbLifecycleLeaseIdentity& other) const { return !(*this == other); }
};

struct UbTransactionCorrelationIdentity
{
    UbLifecycleResourceIdentity operation;
    UbLifecycleResourceIdentity segment;

    bool operator==(const UbTransactionCorrelationIdentity& other) const
    {
        return operation == other.operation && segment == other.segment;
    }
    bool operator!=(const UbTransactionCorrelationIdentity& other) const
    {
        return !(*this == other);
    }
};

std::ostream& operator<<(std::ostream& os, const UbLifecycleResourceIdentity& identity);
Ptr<Packet> CopyPacketView(Ptr<const Packet> packet);
Ptr<Packet> CopyPacketTransfer(Ptr<const Packet> packet);

class UbTransactionCorrelationTag : public Tag
{
  public:
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;

    uint32_t GetSerializedSize() const override;
    void Serialize(TagBuffer buffer) const override;
    void Deserialize(TagBuffer buffer) override;
    void Print(std::ostream& os) const override;

    void SetIdentity(const UbTransactionCorrelationIdentity& identity);
    UbTransactionCorrelationIdentity GetIdentity() const;

  private:
    UbTransactionCorrelationIdentity m_identity;
};

struct UbLifecycleResourceSnapshot
{
    uint64_t activeWorkLeases{0};
    uint64_t activeChildLeases{0};
    uint64_t activeTransportUseLeases{0};
    uint64_t activeBindingLeases{0};
    uint64_t activeTrackedEventLeases{0};
    uint64_t activeTransportWorkLeases{0};
    uint64_t successfulTerminals{0};
    uint64_t completedTerminals{0};
    uint64_t failedTerminals{0};
    uint64_t canceledTerminals{0};
    uint64_t flushedTerminals{0};
    bool retired{false};
};

struct UbLifecycleKindStorageSnapshot
{
    std::size_t resourceCount{0};
    std::size_t compactHistoryRunCount{0};
    std::size_t activeStateCount{0};
    std::size_t retainedSummaryCount{0};
    std::size_t activeLeaseCount{0};
};

struct UbLifecycleStorageSnapshot : UbLifecycleKindStorageSnapshot
{
    std::map<UbLifecycleResourceKind, UbLifecycleKindStorageSnapshot> byKind;
};

class UbLifecycleCoordinator : public Object
{
  public:
    static TypeId GetTypeId();

    static Ptr<UbLifecycleCoordinator> GetOrCreate(Ptr<Node> node);

    UbLifecycleResourceIdentity OpenResource(UbLifecycleResourceKind kind, uint64_t localId);
    void RetireResource(const UbLifecycleResourceIdentity& identity);
    UbLifecycleLeaseIdentity AcquireLease(const UbLifecycleResourceIdentity& owner,
                                          UbLifecycleLeaseKind kind,
                                          const UbLifecycleResourceIdentity& related);
    void ReleaseLease(const UbLifecycleLeaseIdentity& lease);
    void RecordSuccessfulTerminal(const UbLifecycleResourceIdentity& identity);
    void RecordWorkTerminal(const UbLifecycleResourceIdentity& identity, UbWorkOutcome outcome);
    bool IsResourceActive(const UbLifecycleResourceIdentity& identity) const;
    void SetResourceStateChangedCallback(
        const UbLifecycleResourceIdentity& identity,
        Callback<void, UbLifecycleResourceIdentity> callback);

    UbLifecycleResourceSnapshot GetSnapshot(const UbLifecycleResourceIdentity& owner) const;
    UbLifecycleStorageSnapshot GetStorageSnapshot() const;
    UbLifecycleStorageSnapshot GetStorageSnapshotForTest() const
    {
        return GetStorageSnapshot();
    }
    void EmitLifecycleSummary();

  private:
    friend class UbParallelLifecycleAdapter;
    struct ResourceKey
    {
        UbLifecycleResourceKind kind;
        uint64_t localId;

        bool operator<(const ResourceKey& other) const;
    };

    struct ActiveResourceState
    {
        uint64_t nextLease{0};
        uint64_t sequence{0};
        UbLifecycleResourceSnapshot snapshot;
        std::map<uint64_t, UbLifecycleLeaseIdentity> activeLeases;
        Callback<void, UbLifecycleResourceIdentity> stateChangedCallback;
    };

    struct ResourceState
    {
        uint64_t generation{0};
        bool retired{false};
        bool hasTerminalOutcome{false};
        UbWorkOutcome terminalOutcome{UbWorkOutcome::COMPLETED};
        std::unique_ptr<ActiveResourceState> active;
        std::unique_ptr<UbLifecycleResourceSnapshot> retainedSummary;
    };

    struct CompactResourceHistory
    {
        uint64_t generation{0};
        bool hasTerminalOutcome{false};
        UbWorkOutcome terminalOutcome{UbWorkOutcome::COMPLETED};

        bool operator==(const CompactResourceHistory& other) const
        {
            return generation == other.generation &&
                   hasTerminalOutcome == other.hasTerminalOutcome &&
                   terminalOutcome == other.terminalOutcome;
        }
        bool operator!=(const CompactResourceHistory& other) const { return !(*this == other); }
    };

    struct CompactHistoryRun
    {
        uint64_t lastLocalId{0};
        CompactResourceHistory history;
    };

    struct ResourceValidation
    {
        const ResourceState* state{nullptr};
        const CompactResourceHistory* compactHistory{nullptr};
    };

    ResourceState& ValidateMutable(const UbLifecycleResourceIdentity& identity);
    ResourceValidation Validate(const UbLifecycleResourceIdentity& identity) const;
    const CompactResourceHistory* FindCompactHistory(UbLifecycleResourceKind kind,
                                                     uint64_t localId) const;
    bool TakeCompactHistory(UbLifecycleResourceKind kind,
                            uint64_t localId,
                            CompactResourceHistory& history);
    void StoreCompactHistory(UbLifecycleResourceKind kind,
                             uint64_t localId,
                             const CompactResourceHistory& history);
    static UbLifecycleResourceSnapshot BuildSnapshot(const ResourceState& state);
    static UbLifecycleResourceSnapshot BuildSnapshot(const ResourceValidation& validation);
    uint32_t m_ownerNodeId{0};
    uint64_t m_traceSequence{0};
    TracedCallback<const UbLifecycleTraceRecord&> m_lifecycleTrace;
    std::map<ResourceKey, ResourceState> m_resources;
    std::map<UbLifecycleResourceKind, std::map<uint64_t, CompactHistoryRun>> m_compactHistories;
    ResourceState* m_cachedResourceState{nullptr};
    ResourceKey m_cachedResourceKey{UbLifecycleResourceKind::RTP_ENDPOINT, 0};
};

class UbParallelLifecycleAdapter : public Object
{
  public:
    static TypeId GetTypeId();
    static Ptr<UbParallelLifecycleAdapter> GetOrCreate(Ptr<Node> node);

    static bool RouteControl(UbLifecycleControlRecord record);
    static bool IsExecutionOwner(uint32_t nodeId);

    void InjectControlForTest(const UbLifecycleControlRecord& record);
    UbParallelLifecycleSnapshot GetSnapshot() const;

  private:
    struct InboundSequence
    {
        uint64_t next{1};
        std::map<uint64_t, UbLifecycleControlRecord> pending;
    };

    static uint32_t RequireSourceNodeId(uint32_t preferredNodeId = Simulator::NO_CONTEXT);
    static void Route(UbLifecycleControlRecord record);
    static void ReceiveAtTarget(UbLifecycleControlRecord record);
    void Send(UbLifecycleControlRecord record);
    void Receive(UbLifecycleControlRecord record);
    bool Apply(const UbLifecycleControlRecord& record);

    uint32_t m_nodeId{0};
    uint64_t m_ownerSequence{0};
    uint64_t m_sent{0};
    uint64_t m_received{0};
    uint64_t m_applied{0};
    uint64_t m_duplicate{0};
    uint64_t m_stale{0};
    std::map<uint32_t, uint64_t> m_nextOutboundSequence;
    std::map<uint32_t, InboundSequence> m_inbound;
};

class UbTrackedEventSet
{
  public:
    void Init(Ptr<UbLifecycleCoordinator> coordinator);
    uint64_t Track(const UbLifecycleResourceIdentity& owner);
    void Attach(const UbLifecycleResourceIdentity& owner,
                uint64_t token,
                EventId event,
                Callback<void> cancelCleanup);
    bool Begin(const UbLifecycleResourceIdentity& owner, uint64_t token);
    void Resolve(const UbLifecycleResourceIdentity& owner, uint64_t token);
    bool HasEvents(const UbLifecycleResourceIdentity& owner) const;
    std::vector<Callback<void>> CancelPending(const UbLifecycleResourceIdentity& owner);
    bool IsEmpty() const;
    void Clear();

  private:
    enum class Phase : uint8_t
    {
        PENDING = 0,
        RUNNING = 1,
    };

    struct Record
    {
        UbLifecycleResourceIdentity owner;
        UbLifecycleLeaseIdentity lease;
        EventId event;
        Callback<void> cancelCleanup;
        Phase phase{Phase::PENDING};
    };

    Ptr<UbLifecycleCoordinator> m_coordinator;
    uint64_t m_nextToken{0};
    std::map<uint64_t, Record> m_records;
};

} // namespace ns3

#endif // UB_LIFECYCLE_H
