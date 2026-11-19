// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TRANSPORT_CHANNEL_MANAGER_H
#define UB_TRANSPORT_CHANNEL_MANAGER_H

#include "ns3/ub-transport.h"
#include "ub-datatype.h"
#include "ub-lifecycle.h"

#include "ns3/ipv4-address.h"
#include "ns3/object.h"

#include <cstdint>
#include <map>
#include <vector>

namespace ns3
{

class UbCongestionControl;
class UbController;

enum class UbInboundTpState : uint8_t
{
    LIVE,
    RETIRED_EXACT,
    INVALID,
};

struct UbInboundTpResolution
{
    UbInboundTpState state{UbInboundTpState::INVALID};
    Ptr<UbTransportChannel> endpoint;
};

enum class UbTpQuiescenceBlocker : uint8_t
{
    CONTROLLER_UNAVAILABLE,
    CONTROLLER_DISPOSING,
    NODE_UNAVAILABLE,
    WRONG_EXECUTION_OWNER,
    ENDPOINT_MISSING,
    ENDPOINT_NOT_DRAINING,
    LIFECYCLE_OWNER_MISMATCH,
    LIFECYCLE_UNAVAILABLE,
    LIFECYCLE_INACTIVE,
    ACTIVE_WORK_LEASE,
    ACTIVE_CHILD_LEASE,
    ACTIVE_BINDING_LEASE,
    ACTIVE_TRACKED_EVENT_LEASE,
    ACTIVE_TRANSPORT_WORK_LEASE,
    TRANSACTION_UNAVAILABLE,
    TRANSACTION_REFERENCE,
    PENDING_TRANSPORT_WORK,
    ACK_QUEUE,
    CNP_QUEUE,
    WQE_SEGMENT_QUEUE,
    REMOTE_REQUEST,
    INBOUND_WORK,
    BUFFERED_INBOUND_PACKET,
    UNACKNOWLEDGED_PSN,
    RETRANSMISSION_TIMER,
    COUNT,
};

struct UbTpQuiescenceDiagnostics
{
    UbLifecycleResourceSnapshot lifecycle;
    UbTransportQuiescenceSnapshot transport;
    bool transactionReferenced{false};
    bool pendingTransportWork{false};
};

class UbTpQuiescenceAssessment
{
  public:
    bool IsAllowed() const
    {
        return m_blockers == 0;
    }

    bool HasBlocker(UbTpQuiescenceBlocker blocker) const
    {
        return (m_blockers & (uint64_t{1} << static_cast<uint8_t>(blocker))) != 0;
    }

    uint32_t tpn{0};
    UbLifecycleResourceIdentity identity;
    UbTpQuiescenceDiagnostics diagnostics;

  private:
    friend class UbTransportChannelManager;

    void AddBlocker(UbTpQuiescenceBlocker blocker)
    {
        m_blockers |= uint64_t{1} << static_cast<uint8_t>(blocker);
    }

    uint64_t m_blockers{0};
};

class UbTransportChannelManager : public Object
{
  public:
    static TypeId GetTypeId();

    void Init(UbController* controller);
    bool CreateTp(uint32_t src,
                  uint32_t dest,
                  uint8_t sport,
                  uint8_t dport,
                  UbPriority priority,
                  uint32_t srcTpn,
                  uint32_t dstTpn,
                  Ptr<UbCongestionControl> congestionCtrl,
                  bool drainOnly = false);
    /** Observe the materialized TP endpoint without changing lifecycle state. */
    Ptr<UbTransportChannel> PeekLiveTp(uint32_t tpn) const;
    /** Legacy source-compatible alias of PeekLiveTp(). */
    Ptr<UbTransportChannel> GetTp(uint32_t tpn) const;
    bool TryGetEndpointIdentity(uint32_t tpn, UbLifecycleResourceIdentity& identity) const;
    Ptr<UbTransportChannel> CreateReservedTpEndpoint(uint32_t tpn);
    UbInboundTpResolution ResolveInboundTp(uint32_t srcTpn,
                                           uint32_t dstTpn,
                                           Ipv4Address sourceIp,
                                           Ipv4Address destinationIp,
                                           UbPriority priority);
    /** Assess owner-local TP quiescence without mutating endpoint or owner state. */
    UbTpQuiescenceAssessment AssessTpQuiescence(uint32_t tpn) const;
    /** Return the exact materialized endpoint identity. */
    Ptr<UbTransportChannel> ResolveLiveTp(uint32_t tpn,
                                          const UbLifecycleResourceIdentity& expectedIdentity);
    /** Resolve exact backing only for work admitted before endpoint drain began. */
    Ptr<UbTransportChannel> ResolveTpForAcceptedWork(
        uint32_t tpn,
        const UbLifecycleResourceIdentity& expectedIdentity);
    UbDestroyStatus DestroyTp(uint32_t tpn,
                              UbDestroyReason reason = UbDestroyReason::EXPLICIT,
                              UbDestroyCallback callback = {},
                              bool releaseConnectionDrainHold = false);
    /** Freeze an endpoint for a paired connection destroy without deleting its transport state. */
    void FreezeTp(uint32_t tpn);
    void NotifyStateChanged(uint32_t tpn);
    void ForceDispose();

    /** Return whether the logical endpoint record exists. */
    bool Contains(uint32_t tpn) const;
    uint32_t GetCount() const;
    std::map<uint32_t, Ptr<UbTransportChannel>> GetTpnMap() const;

  private:
    enum class EndpointState : uint8_t
    {
        LIVE,
        DRAINING,
    };

    struct EndpointRecord
    {
        Ptr<UbTransportChannel> channel;
        EndpointState state{EndpointState::LIVE};
        UbDestroyReason reason{UbDestroyReason::EXPLICIT};
        std::vector<UbDestroyCallback> callbacks;
        bool connectionDrainHold{false};
    };

    Ptr<UbTransportChannel> CreateReservedTpEndpointLocked(uint32_t tpn, bool drainOnly = false);
    UbTpQuiescenceAssessment AssessTpQuiescenceLocked(uint32_t tpn,
                                                      const EndpointRecord& endpoint) const;
    Ptr<UbTransportChannel> ResolveLiveTpLocked(
        uint32_t tpn,
        const UbLifecycleResourceIdentity& expectedIdentity);
    bool TryGetEndpointIdentityLocked(uint32_t tpn, UbLifecycleResourceIdentity& identity) const;
    void BeginDrainOnlyDestroy(uint32_t tpn);
    bool CanFinalize(const EndpointRecord& record) const;
    void FinalizeDestroy(uint32_t tpn, bool force);
    void NotifyLifecycleStateChanged(UbLifecycleResourceIdentity identity);

    UbController* m_controller{nullptr};
    std::map<uint32_t, EndpointRecord> m_endpoints;
    uint32_t m_transportsCount{0};
};

} // namespace ns3

#endif // UB_TRANSPORT_CHANNEL_MANAGER_H
