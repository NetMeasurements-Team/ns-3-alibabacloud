// SPDX-License-Identifier: GPL-2.0-only
#include "ub-transport-channel-manager.h"
#include "ns3/ub-mtp-compat.h"

#include "ns3/ub-congestion-control.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-transport.h"
#include "ub-controller.h"
#include "ub-network-address.h"
#include "ub-switch.h"
#include "ub-tp-connection-manager.h"
#include "ub-utils.h"

#include "ns3/log.h"
#include <optional>
#ifdef NS3_MTP
#include "ns3/mtp-interface.h"
#endif
#include "ns3/node.h"

#include <limits>

namespace ns3
{

namespace
{

bool
IsTpBackingExecutionOwner(uint32_t nodeId)
{
#ifdef NS3_MTP
    if (MtpInterface::isEnabled())
    {
        if (!MtpInterface::isPartitioned())
        {
            return true;
        }
        const std::optional<uint32_t> ownerLp = UbFindNodeLocalLpId(nodeId);
        LogicalProcess* currentLp = MtpInterface::GetSystem();
        if (!ownerLp.has_value() || currentLp == nullptr ||
            *ownerLp != currentLp->GetSystemId())
        {
            return false;
        }
    }
#endif
    return UbParallelLifecycleAdapter::IsExecutionOwner(nodeId);
}

} // namespace

NS_LOG_COMPONENT_DEFINE("UbTransportChannelManager");
NS_OBJECT_ENSURE_REGISTERED(UbTransportChannelManager);

TypeId
UbTransportChannelManager::GetTypeId()
{
    static TypeId tid = TypeId("ns3::UbTransportChannelManager")
                            .SetParent<Object>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbTransportChannelManager>();
    return tid;
}

void
UbTransportChannelManager::Init(UbController* controller)
{
    NS_ABORT_MSG_IF(controller == nullptr, "Transport channel manager requires a Controller");
    m_controller = controller;
}

Ptr<UbTransportChannel>
UbTransportChannelManager::PeekLiveTp(uint32_t tpn) const
{
    const auto it = m_endpoints.find(tpn);
    return it == m_endpoints.end() ? nullptr : it->second.channel;
}

Ptr<UbTransportChannel>
UbTransportChannelManager::GetTp(uint32_t tpn) const
{
    return PeekLiveTp(tpn);
}

bool
UbTransportChannelManager::TryGetEndpointIdentity(uint32_t tpn,
                                                  UbLifecycleResourceIdentity& identity) const
{
    std::lock_guard<std::mutex> lock(TpConnectionManager::g_tpConnectionControlLock);
    return TryGetEndpointIdentityLocked(tpn, identity);
}

bool
UbTransportChannelManager::TryGetEndpointIdentityLocked(uint32_t tpn,
                                                        UbLifecycleResourceIdentity& identity) const
{
    const auto it = m_endpoints.find(tpn);
    if (it == m_endpoints.end() || it->second.state == EndpointState::DRAINING)
    {
        return false;
    }
    if (it->second.channel == nullptr)
    {
        return false;
    }
    identity = it->second.channel->GetLifecycleOwner();
    return true;
}

bool
UbTransportChannelManager::CreateTp(uint32_t src,
                                    uint32_t dest,
                                    uint8_t sport,
                                    uint8_t dport,
                                    UbPriority priority,
                                    uint32_t srcTpn,
                                    uint32_t dstTpn,
                                    Ptr<UbCongestionControl> congestionCtrl,
                                    bool drainOnly)
{
    NS_LOG_FUNCTION(this << src << dest << sport << dport << priority << srcTpn);
    if (m_endpoints.find(srcTpn) != m_endpoints.end())
    {
        NS_LOG_DEBUG("Transport channel already exists");
        return false;
    }

    Ptr<Node> currentNode = m_controller->GetObject<Node>();
    if (!currentNode)
    {
        NS_LOG_WARN("No associated node found for UbController");
        currentNode = CreateObject<Node>();
    }

    const Ipv4Address sip = NodeIdToIp(src, sport);
    const Ipv4Address dip = NodeIdToIp(dest, dport);
    Ptr<UbTransportChannel> tp = CreateObject<UbTransportChannel>();
    tp->SetUbTransport(currentNode->GetId(),
                       src,
                       dest,
                       srcTpn,
                       dstTpn,
                       0,
                       static_cast<uint8_t>(priority),
                       static_cast<uint16_t>(sport),
                       static_cast<uint16_t>(dport),
                       sip,
                       dip,
                       congestionCtrl);
    if (drainOnly)
    {
        tp->FreezeNewWork();
    }
    tp->SetLifecycleStateChangedCallback(
        MakeCallback(&UbTransportChannelManager::NotifyStateChanged, this));
    UbLifecycleCoordinator::GetOrCreate(currentNode)
        ->SetResourceStateChangedCallback(
            tp->GetLifecycleOwner(),
            MakeCallback(&UbTransportChannelManager::NotifyLifecycleStateChanged, this));
    m_controller->GetUbTransaction()->TpInit(tp);
    m_endpoints.emplace(srcTpn, EndpointRecord{tp});
    ++m_transportsCount;
    currentNode->GetObject<UbSwitch>()->RegisterTpWithAllocator(tp, sport, priority);
    m_controller->GetTpConnManager()->NotifyEndpointMaterialized(currentNode->GetId(),
                                                                  srcTpn,
                                                                  drainOnly);
    utils::UbUtils::Get()->SingleTpTraceConnect(src, srcTpn);
    NS_LOG_DEBUG("Created transport channel success");
    return true;
}

Ptr<UbTransportChannel>
UbTransportChannelManager::CreateReservedTpEndpoint(uint32_t tpn)
{
    std::lock_guard<std::mutex> lock(TpConnectionManager::g_tpConnectionControlLock);
    Ptr<Node> currentNode = m_controller->GetObject<Node>();
    if (currentNode == nullptr || !IsTpBackingExecutionOwner(currentNode->GetId()))
    {
        return nullptr;
    }
    if (currentNode != nullptr &&
        m_controller->GetTpConnManager()->IsConnectionDestroyRequested(currentNode->GetId(), tpn))
    {
        return nullptr;
    }
    return CreateReservedTpEndpointLocked(tpn);
}

Ptr<UbTransportChannel>
UbTransportChannelManager::CreateReservedTpEndpointLocked(uint32_t tpn, bool drainOnly)
{
    const auto existing = m_endpoints.find(tpn);
    if (existing != m_endpoints.end())
    {
        return existing->second.state == EndpointState::LIVE ? existing->second.channel : nullptr;
    }

    Ptr<Node> currentNode = m_controller->GetObject<Node>();
    NS_ASSERT_MSG(currentNode != nullptr, "Reserved TP endpoint requires an attached node");
    NS_ASSERT_MSG(m_controller->GetUbTransaction() != nullptr,
                  "Reserved TP endpoint requires UbTransaction");
    Ptr<TpConnectionManager> connections = m_controller->GetTpConnManager();
    NS_ASSERT_MSG(connections != nullptr,
                  "Reserved TP endpoint requires TpConnectionManager");

    Connection conn;
    if (!connections->TryGetLocalConnection(currentNode->GetId(), tpn, conn))
    {
        return nullptr;
    }

    auto congestionCtrl = UbCongestionControl::Create(UB_DEVICE);
    CreateTp(conn.node1,
             conn.node2,
             conn.port1,
             conn.port2,
             static_cast<UbPriority>(conn.priority),
             conn.tpn1,
             conn.tpn2,
             congestionCtrl,
             drainOnly);
    if (drainOnly)
    {
        Simulator::ScheduleNow(&UbTransportChannelManager::BeginDrainOnlyDestroy, this, tpn);
    }
    return PeekLiveTp(tpn);
}

void
UbTransportChannelManager::BeginDrainOnlyDestroy(uint32_t tpn)
{
    Ptr<Node> node = m_controller->GetObject<Node>();
    if (node != nullptr)
    {
        m_controller->GetTpConnManager()->DestroyDrainOnlyEndpoint(node->GetId(), tpn);
    }
}

UbInboundTpResolution
UbTransportChannelManager::ResolveInboundTp(uint32_t srcTpn,
                                            uint32_t dstTpn,
                                            Ipv4Address sourceIp,
                                            Ipv4Address destinationIp,
                                            UbPriority priority)
{
    if (m_controller == nullptr || m_controller->IsDisposing())
    {
        return {};
    }
    Ptr<Node> currentNode = m_controller->GetObject<Node>();
    NS_ASSERT_MSG(currentNode != nullptr, "Inbound TP resolution requires an attached node");
    if (!IsTpBackingExecutionOwner(currentNode->GetId()))
    {
        return {};
    }

    const auto endpoint = m_endpoints.find(dstTpn);
    if (endpoint != m_endpoints.end())
    {
        const EndpointRecord& record = endpoint->second;
        if (record.channel == nullptr)
        {
            return {};
        }
        const uint32_t expectedSrcTpn = record.channel->GetDestTpn();
        const Ipv4Address expectedSourceIp = record.channel->GetDip();
        const Ipv4Address expectedDestinationIp = record.channel->GetSip();
        const UbPriority expectedPriority =
            static_cast<UbPriority>(record.channel->GetPriority());
        if (srcTpn != expectedSrcTpn || sourceIp != expectedSourceIp ||
            destinationIp != expectedDestinationIp || priority != expectedPriority)
        {
            return {};
        }
        return {UbInboundTpState::LIVE, record.channel};
    }

    Connection conn;
    const TpConnectionManager::InboundConnectionState connectionState =
        m_controller->GetTpConnManager()->ResolveInboundConnection(currentNode->GetId(),
                                                                   srcTpn,
                                                                   dstTpn,
                                                                   sourceIp,
                                                                   destinationIp,
                                                                   priority,
                                                                   conn);
    if (connectionState == TpConnectionManager::InboundConnectionState::RETIRED_EXACT)
    {
        return {UbInboundTpState::RETIRED_EXACT, nullptr};
    }
    if (connectionState != TpConnectionManager::InboundConnectionState::LIVE)
    {
        return {};
    }
    const bool drainOnly =
        m_controller->GetTpConnManager()->IsConnectionDestroyRequested(currentNode->GetId(),
                                                                       dstTpn);
    Ptr<UbTransportChannel> resolved = CreateReservedTpEndpointLocked(dstTpn, drainOnly);
    return resolved == nullptr ? UbInboundTpResolution{}
                               : UbInboundTpResolution{UbInboundTpState::LIVE, resolved};
}

UbTpQuiescenceAssessment
UbTransportChannelManager::AssessTpQuiescence(uint32_t tpn) const
{
    std::lock_guard<std::mutex> lock(TpConnectionManager::g_tpConnectionControlLock);
    const auto it = m_endpoints.find(tpn);
    if (it == m_endpoints.end())
    {
        UbTpQuiescenceAssessment assessment;
        assessment.tpn = tpn;
        assessment.AddBlocker(UbTpQuiescenceBlocker::ENDPOINT_MISSING);
        return assessment;
    }
    return AssessTpQuiescenceLocked(tpn, it->second);
}

UbTpQuiescenceAssessment
UbTransportChannelManager::AssessTpQuiescenceLocked(uint32_t tpn,
                                                    const EndpointRecord& endpoint) const
{
    static_assert(static_cast<uint8_t>(UbTpQuiescenceBlocker::COUNT) <= 64);

    UbTpQuiescenceAssessment assessment;
    assessment.tpn = tpn;
    if (m_controller == nullptr)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::CONTROLLER_UNAVAILABLE);
        return assessment;
    }
    if (m_controller->IsDisposing())
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::CONTROLLER_DISPOSING);
        return assessment;
    }

    Ptr<Node> node = m_controller->GetObject<Node>();
    if (node == nullptr)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::NODE_UNAVAILABLE);
        return assessment;
    }
    if (!IsTpBackingExecutionOwner(node->GetId()))
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::WRONG_EXECUTION_OWNER);
        return assessment;
    }

    if (endpoint.state != EndpointState::DRAINING)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::ENDPOINT_NOT_DRAINING);
    }

    const Ptr<UbTransportChannel>& tp = endpoint.channel;
    if (tp == nullptr)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::ENDPOINT_MISSING);
        return assessment;
    }
    assessment.identity = tp->GetLifecycleOwner();
    if (assessment.identity.ownerNodeId != node->GetId())
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::LIFECYCLE_OWNER_MISMATCH);
        return assessment;
    }

    Ptr<UbLifecycleCoordinator> coordinator = node->GetObject<UbLifecycleCoordinator>();
    if (coordinator == nullptr)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::LIFECYCLE_UNAVAILABLE);
        return assessment;
    }
    if (!coordinator->IsResourceActive(assessment.identity))
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::LIFECYCLE_INACTIVE);
        return assessment;
    }
    assessment.diagnostics.lifecycle = coordinator->GetSnapshot(assessment.identity);
    const UbLifecycleResourceSnapshot& lifecycle = assessment.diagnostics.lifecycle;
    if (lifecycle.activeWorkLeases != 0)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::ACTIVE_WORK_LEASE);
    }
    if (lifecycle.activeChildLeases != 0)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::ACTIVE_CHILD_LEASE);
    }
    if (m_controller->ResolveRtpTpGroupForMember(assessment.identity) != nullptr)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::ACTIVE_BINDING_LEASE);
    }
    if (lifecycle.activeTrackedEventLeases != 0)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::ACTIVE_TRACKED_EVENT_LEASE);
    }
    if (lifecycle.activeTransportWorkLeases != 0)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::ACTIVE_TRANSPORT_WORK_LEASE);
    }

    Ptr<UbTransaction> transaction = m_controller->GetUbTransaction();
    if (transaction == nullptr)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::TRANSACTION_UNAVAILABLE);
    }
    else
    {
        assessment.diagnostics.transactionReferenced = transaction->IsTpInUse(tpn);
        assessment.diagnostics.pendingTransportWork = transaction->HasPendingTransportWork(tpn);
        if (assessment.diagnostics.transactionReferenced)
        {
            assessment.AddBlocker(UbTpQuiescenceBlocker::TRANSACTION_REFERENCE);
        }
        if (assessment.diagnostics.pendingTransportWork)
        {
            assessment.AddBlocker(UbTpQuiescenceBlocker::PENDING_TRANSPORT_WORK);
        }
    }

    assessment.diagnostics.transport = tp->GetQuiescenceSnapshot();
    const UbTransportQuiescenceSnapshot& transport = assessment.diagnostics.transport;
    if (transport.pendingAckPackets != 0)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::ACK_QUEUE);
    }
    if (transport.pendingCnpPackets != 0)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::CNP_QUEUE);
    }
    if (transport.pendingWqeSegments != 0)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::WQE_SEGMENT_QUEUE);
    }
    if (transport.pendingRemoteRequests != 0)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::REMOTE_REQUEST);
    }
    if (transport.pendingInboundTaUnits != 0)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::INBOUND_WORK);
    }
    if (transport.bufferedInboundPackets != 0)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::BUFFERED_INBOUND_PACKET);
    }
    if (transport.hasUnacknowledgedPsn)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::UNACKNOWLEDGED_PSN);
    }
    if (transport.retransmissionTimerRunning)
    {
        assessment.AddBlocker(UbTpQuiescenceBlocker::RETRANSMISSION_TIMER);
    }
    return assessment;
}

Ptr<UbTransportChannel>
UbTransportChannelManager::ResolveLiveTp(uint32_t tpn,
                                         const UbLifecycleResourceIdentity& expectedIdentity)
{
    std::lock_guard<std::mutex> lock(TpConnectionManager::g_tpConnectionControlLock);
    return ResolveLiveTpLocked(tpn, expectedIdentity);
}

Ptr<UbTransportChannel>
UbTransportChannelManager::ResolveTpForAcceptedWork(
    uint32_t tpn,
    const UbLifecycleResourceIdentity& expectedIdentity)
{
    if (m_controller == nullptr || m_controller->IsDisposing())
    {
        return nullptr;
    }
    Ptr<Node> node = m_controller->GetObject<Node>();
    if (node == nullptr || expectedIdentity.ownerNodeId != node->GetId() ||
        !IsTpBackingExecutionOwner(node->GetId()))
    {
        return nullptr;
    }
    const auto endpoint = m_endpoints.find(tpn);
    if (endpoint == m_endpoints.end() || endpoint->second.channel == nullptr ||
        endpoint->second.channel->GetLifecycleOwner() != expectedIdentity)
    {
        return nullptr;
    }
    Ptr<UbLifecycleCoordinator> coordinator = UbLifecycleCoordinator::GetOrCreate(node);
    return coordinator->IsResourceActive(expectedIdentity) ? endpoint->second.channel : nullptr;
}

Ptr<UbTransportChannel>
UbTransportChannelManager::ResolveLiveTpLocked(uint32_t tpn,
                                               const UbLifecycleResourceIdentity& expectedIdentity)
{
    if (m_controller == nullptr || m_controller->IsDisposing())
    {
        return nullptr;
    }
    Ptr<Node> node = m_controller->GetObject<Node>();
    if (node == nullptr || expectedIdentity.ownerNodeId != node->GetId() ||
        !IsTpBackingExecutionOwner(node->GetId()))
    {
        return nullptr;
    }
    const auto it = m_endpoints.find(tpn);
    if (it == m_endpoints.end() || it->second.state == EndpointState::DRAINING)
    {
        return nullptr;
    }
    const Ptr<UbTransportChannel>& channel = it->second.channel;
    return channel != nullptr && channel->GetLifecycleOwner() == expectedIdentity ? channel
                                                                                  : nullptr;
}

UbDestroyStatus
UbTransportChannelManager::DestroyTp(uint32_t tpn,
                                     UbDestroyReason reason,
                                     UbDestroyCallback callback,
                                     bool releaseConnectionDrainHold)
{
    auto it = m_endpoints.find(tpn);
    if (it == m_endpoints.end())
    {
        return UbDestroyStatus::NOT_FOUND;
    }
    EndpointRecord& record = it->second;
    if (releaseConnectionDrainHold)
    {
        record.connectionDrainHold = false;
    }
    const bool beginDrain = record.state == EndpointState::LIVE;
    record.state = EndpointState::DRAINING;
    record.reason = reason;
    if (!callback.IsNull())
    {
        record.callbacks.push_back(callback);
    }
    if (beginDrain)
    {
        UbLifecycleCoordinator::GetOrCreate(m_controller->GetObject<Node>())
            ->SetResourceStateChangedCallback(
                record.channel->GetLifecycleOwner(),
                MakeCallback(&UbTransportChannelManager::NotifyLifecycleStateChanged, this));
    }
    record.channel->FreezeNewWork();
    it = m_endpoints.find(tpn);
    if (it == m_endpoints.end())
    {
        return UbDestroyStatus::COMPLETED;
    }
    if (it->second.connectionDrainHold || !CanFinalize(it->second))
    {
        return UbDestroyStatus::PENDING;
    }
    FinalizeDestroy(tpn, false);
    return UbDestroyStatus::COMPLETED;
}

void
UbTransportChannelManager::FreezeTp(uint32_t tpn)
{
    const auto it = m_endpoints.find(tpn);
    if (it == m_endpoints.end() || it->second.channel == nullptr)
    {
        return;
    }
    it->second.connectionDrainHold = true;
    it->second.channel->FreezeNewWork();
}

bool
UbTransportChannelManager::CanFinalize(const EndpointRecord& record) const
{
    return record.channel != nullptr && AssessTpQuiescenceLocked(record.channel->GetTpn(), record)
                                            .IsAllowed();
}

void
UbTransportChannelManager::NotifyStateChanged(uint32_t tpn)
{
    const auto it = m_endpoints.find(tpn);
    if (it == m_endpoints.end())
    {
        return;
    }
    if (it->second.state == EndpointState::DRAINING && !it->second.connectionDrainHold &&
        CanFinalize(it->second))
    {
        FinalizeDestroy(tpn, false);
    }
}

void
UbTransportChannelManager::NotifyLifecycleStateChanged(UbLifecycleResourceIdentity identity)
{
    if (identity.kind == UbLifecycleResourceKind::RTP_ENDPOINT &&
        identity.localId <= std::numeric_limits<uint32_t>::max())
    {
        NotifyStateChanged(static_cast<uint32_t>(identity.localId));
    }
}

void
UbTransportChannelManager::FinalizeDestroy(uint32_t tpn, bool force)
{
    const auto it = m_endpoints.find(tpn);
    NS_ABORT_MSG_IF(it == m_endpoints.end(), "Cannot finalize a missing TP endpoint");
    NS_ABORT_MSG_IF(it->second.channel == nullptr, "Cannot finalize TP without live endpoint state");
    NS_ABORT_MSG_IF(!force && !CanFinalize(it->second), "Cannot finalize a busy TP endpoint");

    Ptr<UbTransportChannel> tp = it->second.channel;
    const UbLifecycleResourceIdentity identity = tp->GetLifecycleOwner();
    std::vector<UbDestroyCallback> callbacks = std::move(it->second.callbacks);
    tp->PrepareForExplicitDestroy();
    Ptr<UbLifecycleCoordinator> coordinator =
        UbLifecycleCoordinator::GetOrCreate(m_controller->GetObject<Node>());
    if (coordinator->IsResourceActive(identity))
    {
        coordinator->SetResourceStateChangedCallback(identity, {});
    }
    if (!force)
    {
        m_controller->GetObject<Node>()->GetObject<UbSwitch>()->RemoveTpFromAllocator(tp);
        m_controller->GetUbTransaction()->TpDeinit(tpn);
        coordinator->RetireResource(identity);
    }
    m_endpoints.erase(it);
    if (m_transportsCount > 0)
    {
        --m_transportsCount;
    }
    // Force disposal runs while NodeList teardown is already in progress. A pending
    // endpoint callback may route lifecycle control through NodeList, which is no longer
    // a valid dependency at this point. Normal drain/finalize still delivers callbacks.
    if (!force)
    {
        for (const UbDestroyCallback& callback : callbacks)
        {
            callback(identity, UbDestroyStatus::COMPLETED);
        }
    }
}

void
UbTransportChannelManager::ForceDispose()
{
    std::vector<uint32_t> tpns;
    tpns.reserve(m_endpoints.size());
    for (const auto& [tpn, record] : m_endpoints)
    {
        if (record.channel != nullptr)
        {
            record.channel->MarkOwnerControllerDisposing();
        }
        tpns.push_back(tpn);
    }
    for (uint32_t tpn : tpns)
    {
        FinalizeDestroy(tpn, true);
    }
}

bool
UbTransportChannelManager::Contains(uint32_t tpn) const
{
    return m_endpoints.find(tpn) != m_endpoints.end();
}

uint32_t
UbTransportChannelManager::GetCount() const
{
    return static_cast<uint32_t>(m_endpoints.size());
}

std::map<uint32_t, Ptr<UbTransportChannel>>
UbTransportChannelManager::GetTpnMap() const
{
    std::map<uint32_t, Ptr<UbTransportChannel>> result;
    for (const auto& [tpn, record] : m_endpoints)
    {
        result.emplace(tpn, record.channel);
    }
    return result;
}

} // namespace ns3
