// SPDX-License-Identifier: GPL-2.0-only
#include "ub-controller.h"
#include <ns3/log.h>
#include <algorithm>
#include <limits>
#include <set>
#include "ns3/ub-transport.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-ldst-endpoint.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-utils.h"

using namespace utils;
namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbController");
NS_OBJECT_ENSURE_REGISTERED(UbController);

TypeId UbController::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::UbController")
            .SetParent<Object>()
            .SetGroupName("UnifiedBus")
            .AddConstructor<UbController>()
            .AddAttribute(
                "InitiatorProtocolDispatchDelay",
                "Delay between synchronous WQE admission and the first transport dispatch.",
                TimeValue(NanoSeconds(0)),
                MakeTimeAccessor(&UbController::m_initiatorProtocolDispatchDelay),
                MakeTimeChecker(TimeStep(0)))
            .AddAttribute(
                "InitiatorProtocolCompletionVisibilityDelay",
                "Delay between protocol completion and initiator application visibility.",
                TimeValue(NanoSeconds(0)),
                MakeTimeAccessor(&UbController::m_initiatorProtocolCompletionVisibilityDelay),
                MakeTimeChecker(TimeStep(0)));
    return tid;
}

UbController::UbController()
{
    NS_LOG_FUNCTION(this);
    m_tpConnectionManager = CreateObject<TpConnectionManager>();
    m_transportChannelManager = CreateObject<UbTransportChannelManager>();
    m_transportChannelManager->Init(this);
    m_ldstEndpoint = CreateObject<UbLdstEndpoint>();
}

void UbController::CreateUbFunction()
{
    m_tpConnectionManager->SetOwnerNodeId(GetObject<Node>()->GetId());
    m_function = CreateObject<UbFunction>();
    m_function->Init(GetObject<Node>()->GetId());
    m_ldstEndpoint->Init(GetObject<Node>()->GetId());
}

void UbController::CreateUbTransaction()
{
    m_transaction = CreateObject<UbTransaction>(GetObject<Node>());
    m_transaction->SetOwnerController(this);
    NS_ABORT_MSG_IF(m_function == nullptr,
                    "UbFunction must be created before UbTransaction binding");
    m_transaction->SetFunction(PeekPointer(m_function));
    m_function->BindTransaction(m_transaction);
}

UbController::~UbController()
{
    NS_LOG_FUNCTION(this);
    NS_ASSERT_MSG(m_rtpTpGroups.empty(),
                  "UbController must force-clean canonical TPGs in DoDispose");
    NS_ASSERT_MSG(m_transportChannelManager == nullptr ||
                      m_transportChannelManager->GetCount() == 0,
                  "UbController must force-clean canonical TPs in DoDispose");
    m_rtpTpGroups.clear();
    m_rtpTpGroupByTpn.clear();
    m_portsByDestination.clear();
    m_ctpEntityPorts.clear();
}

void
UbController::DoDispose()
{
    m_disposing = true;
    m_transportChannelManager->ForceDispose();
    std::vector<UbLifecycleResourceIdentity> groups;
    groups.reserve(m_rtpTpGroups.size());
    for (const auto& [localId, group] : m_rtpTpGroups)
    {
        (void)localId;
        NS_ABORT_MSG_IF(group == nullptr,
                        "Controller canonical TPG collection contains a null owner");
        groups.push_back(group->GetLifecycleIdentity());
    }
    for (const UbLifecycleResourceIdentity& identity : groups)
    {
        const UbDestroyStatus status =
            DestroyRtpTpGroup(identity, UbDestroyReason::SIMULATION_STOP);
        NS_ABORT_MSG_IF(status != UbDestroyStatus::COMPLETED,
                        "Controller dispose failed to force-clean canonical TPG");
    }
    NS_ABORT_MSG_IF(!m_rtpTpGroups.empty() || !m_rtpTpGroupByTpn.empty(),
                    "Controller dispose left canonical TPG ownership behind: groups="
                        << m_rtpTpGroups.size()
                        << ", member-index=" << m_rtpTpGroupByTpn.size());

    if (m_ctpTransportService != nullptr)
    {
        m_ctpTransportService->Dispose();
    }
    m_ctpTransportService = nullptr;
    if (m_function != nullptr)
    {
        m_function->Dispose();
    }
    if (m_transaction != nullptr)
    {
        m_transaction->Dispose();
    }
    m_transaction = nullptr;
    m_function = nullptr;
    m_ldstEndpoint = nullptr;
    m_transportChannelManager = nullptr;
    m_portsByDestination.clear();
    m_ctpEntityPorts.clear();
    m_tpConnectionManager = nullptr;
    Object::DoDispose();
}

Ptr<UbTransportChannelManager>
UbController::GetTransportChannelManager() const
{
    return m_transportChannelManager;
}

Ptr<UbTransportGroup>
UbController::CreateRtpTpGroup(const std::vector<Ptr<UbTransportChannel>>& members,
                               uint32_t initialSelectionCursor)
{
    NS_ABORT_MSG_IF(members.empty(), "Cannot create an empty RTP TPG");
    NS_ABORT_MSG_IF(m_nextRtpTpGroupId == std::numeric_limits<uint64_t>::max(),
                    "RTP TPG identity exhausted");

    std::vector<UbTransportGroupMember> records;
    records.reserve(members.size());
    std::set<uint32_t> seenTpns;
    uint64_t existingGroupId = 0;
    for (const Ptr<UbTransportChannel>& member : members)
    {
        NS_ABORT_MSG_IF(member == nullptr, "Cannot add a null TP to an RTP TPG");
        NS_ABORT_MSG_IF(!seenTpns.insert(member->GetTpn()).second,
                        "Cannot add the same TP to an RTP TPG twice");
        records.push_back({member->GetTpn(), member->GetLifecycleOwner()});
        const auto existing = m_rtpTpGroupByTpn.find(member->GetTpn());
        if (existing != m_rtpTpGroupByTpn.end())
        {
            NS_ABORT_MSG_IF(existingGroupId != 0 && existingGroupId != existing->second,
                            "RTP TPG members already belong to different groups");
            existingGroupId = existing->second;
        }
    }

    if (existingGroupId != 0)
    {
        const auto existing = m_rtpTpGroups.find(existingGroupId);
        NS_ABORT_MSG_IF(existing == m_rtpTpGroups.end() || existing->second == nullptr,
                        "RTP TPG member index references a missing group");
        Ptr<UbTransportGroup> group = existing->second;
        if (!group->IsAcceptingNewOperations())
        {
            return nullptr;
        }
        NS_ABORT_MSG_IF(group->GetMemberCount() != records.size(),
                        "A TP can belong to only one RTP TPG");
        for (uint32_t index = 0; index < records.size(); ++index)
        {
            NS_ABORT_MSG_IF(group->GetMember(index).tpn != records[index].tpn ||
                                group->GetMember(index).tpIdentity != records[index].tpIdentity,
                            "Existing RTP TPG membership does not match the requested group");
        }
        return group;
    }

    const uint64_t localId = ++m_nextRtpTpGroupId;
    Ptr<UbLifecycleCoordinator> coordinator =
        UbLifecycleCoordinator::GetOrCreate(GetObject<Node>());
    const UbLifecycleResourceIdentity identity =
        coordinator->OpenResource(UbLifecycleResourceKind::RTP_TPG, localId);
    Ptr<UbTransportGroup> group = CreateObject<UbTransportGroup>();
    group->Init(identity, records, initialSelectionCursor);
    const bool inserted = m_rtpTpGroups.emplace(localId, group).second;
    NS_ABORT_MSG_IF(!inserted, "Duplicate RTP TPG collection identity");
    for (const UbTransportGroupMember& member : records)
    {
        const bool indexed = m_rtpTpGroupByTpn.emplace(member.tpn, localId).second;
        NS_ABORT_MSG_IF(!indexed, "A TP can belong to only one RTP TPG");
    }
    return group;
}

Ptr<UbTransportGroup>
UbController::ResolveRtpTpGroup(const UbLifecycleResourceIdentity& identity) const
{
    Ptr<Node> node = GetObject<Node>();
    Ptr<UbLifecycleCoordinator> coordinator =
        node == nullptr ? nullptr : node->GetObject<UbLifecycleCoordinator>();
    if (identity.kind != UbLifecycleResourceKind::RTP_TPG || node == nullptr ||
        identity.ownerNodeId != node->GetId() || coordinator == nullptr ||
        !coordinator->IsResourceActive(identity))
    {
        return nullptr;
    }
    const auto group = m_rtpTpGroups.find(identity.localId);
    if (group == m_rtpTpGroups.end() || group->second == nullptr ||
        group->second->GetLifecycleIdentity() != identity)
    {
        return nullptr;
    }
    return group->second;
}

Ptr<UbTransportGroup>
UbController::ResolveRtpTpGroupForMember(
    const UbLifecycleResourceIdentity& tpIdentity) const
{
    if (tpIdentity.kind != UbLifecycleResourceKind::RTP_ENDPOINT)
    {
        return nullptr;
    }
    const auto membership = m_rtpTpGroupByTpn.find(static_cast<uint32_t>(tpIdentity.localId));
    if (membership == m_rtpTpGroupByTpn.end())
    {
        return nullptr;
    }
    const auto group = m_rtpTpGroups.find(membership->second);
    if (group == m_rtpTpGroups.end() || group->second == nullptr)
    {
        return nullptr;
    }
    for (uint32_t index = 0; index < group->second->GetMemberCount(); ++index)
    {
        if (group->second->GetMember(index).tpIdentity == tpIdentity)
        {
            return group->second;
        }
    }
    return nullptr;
}

void
UbController::FinalizeRtpTpGroupMemberRemoval(Ptr<UbTransportGroup> group, uint32_t index)
{
    NS_ABORT_MSG_IF(group == nullptr || index >= group->GetMemberCount(),
                    "TPG member finalize requires an exact member");
    group->MarkMemberRetiring(index);
    const UbTransportGroupMember removed = group->RemoveMember(index);
    const auto membership = m_rtpTpGroupByTpn.find(removed.tpn);
    NS_ABORT_MSG_IF(membership == m_rtpTpGroupByTpn.end() ||
                        membership->second != group->GetLifecycleIdentity().localId,
                    "TPG member reverse index is stale");
    m_rtpTpGroupByTpn.erase(membership);
    if (m_transportChannelManager != nullptr)
    {
        m_transportChannelManager->NotifyStateChanged(removed.tpn);
    }
}

UbTpgMemberMutationStatus
UbController::AddRtpTpGroupMember(
    const UbLifecycleResourceIdentity& groupIdentity,
    const UbLifecycleResourceIdentity& tpIdentity)
{
    Ptr<UbTransportGroup> group = ResolveRtpTpGroup(groupIdentity);
    if (group == nullptr)
    {
        return UbTpgMemberMutationStatus::NOT_FOUND;
    }
    if (group->IsDestroyPending() ||
        (!group->IsAcceptingNewOperations() && !group->HasPendingLastMemberRemoval()))
    {
        return UbTpgMemberMutationStatus::REJECTED;
    }

    Ptr<Node> node = GetObject<Node>();
    Ptr<UbLifecycleCoordinator> coordinator =
        UbLifecycleCoordinator::GetOrCreate(node);
    if (tpIdentity.kind != UbLifecycleResourceKind::RTP_ENDPOINT ||
        tpIdentity.ownerNodeId != node->GetId() ||
        tpIdentity.localId > std::numeric_limits<uint32_t>::max() ||
        !coordinator->IsResourceActive(tpIdentity))
    {
        return UbTpgMemberMutationStatus::REJECTED;
    }
    const uint32_t tpn = static_cast<uint32_t>(tpIdentity.localId);
    Ptr<UbTransportChannel> transport = m_transportChannelManager->ResolveLiveTp(tpn, tpIdentity);
    if (transport == nullptr || group->FindMemberIndex(tpIdentity) >= 0 ||
        m_rtpTpGroupByTpn.find(tpn) != m_rtpTpGroupByTpn.end())
    {
        return UbTpgMemberMutationStatus::REJECTED;
    }

    UbTransportGroupMember member{tpn, tpIdentity};
    bool memberInserted = false;
    try
    {
        group->AddMember(member);
        memberInserted = true;
        const bool indexed = m_rtpTpGroupByTpn.emplace(tpn, groupIdentity.localId).second;
        if (!indexed)
        {
            const int32_t index = group->FindMemberIndex(tpIdentity);
            NS_ABORT_MSG_IF(index < 0, "TPG AddMember rollback lost the inserted member");
            group->RemoveMember(static_cast<uint32_t>(index));
            return UbTpgMemberMutationStatus::REJECTED;
        }
    }
    catch (...)
    {
        if (memberInserted)
        {
            const int32_t index = group->FindMemberIndex(tpIdentity);
            if (index >= 0)
            {
                group->RemoveMember(static_cast<uint32_t>(index));
            }
        }
        throw;
    }

    if (group->HasPendingLastMemberRemoval())
    {
        const int32_t oldIndex =
            group->FindMemberIndex(group->GetPendingLastMemberRemoval());
        NS_ABORT_MSG_IF(oldIndex < 0,
                        "Pending last TPG member disappeared before replacement");
        FinalizeRtpTpGroupMemberRemoval(group, static_cast<uint32_t>(oldIndex));
        group->ClearPendingLastMemberRemoval();
        group->ResumeNewOperations();
    }
    if (m_transaction != nullptr)
    {
        m_transaction->NotifyRtpTpGroupMemberAvailable(groupIdentity);
    }
    return UbTpgMemberMutationStatus::ADDED;
}

UbTpgMemberMutationStatus
UbController::RemoveRtpTpGroupMember(
    const UbLifecycleResourceIdentity& groupIdentity,
    const UbLifecycleResourceIdentity& tpIdentity,
    UbDestroyReason reason)
{
    Ptr<UbTransportGroup> group = ResolveRtpTpGroup(groupIdentity);
    if (group == nullptr)
    {
        return UbTpgMemberMutationStatus::NOT_FOUND;
    }
    const int32_t memberIndex = group->FindMemberIndex(tpIdentity);
    if (memberIndex < 0)
    {
        return UbTpgMemberMutationStatus::NOT_FOUND;
    }
    if (group->HasPendingLastMemberRemoval())
    {
        return group->GetPendingLastMemberRemoval() == tpIdentity
                   ? UbTpgMemberMutationStatus::PENDING
                   : UbTpgMemberMutationStatus::REJECTED;
    }

    if (group->GetMemberCount() > 1)
    {
        FinalizeRtpTpGroupMemberRemoval(group, static_cast<uint32_t>(memberIndex));
        return UbTpgMemberMutationStatus::REMOVED;
    }

    if (m_transaction != nullptr && m_transaction->HasRtpTpGroupWork(groupIdentity))
    {
        if (reason == UbDestroyReason::ERROR ||
            reason == UbDestroyReason::SIMULATION_STOP)
        {
            group->FreezeNewOperations();
            if (m_transaction != nullptr)
            {
                const bool discardRetainedSegments = reason == UbDestroyReason::SIMULATION_STOP;
                m_transaction->TerminateRtpTpGroupOperations(
                    groupIdentity,
                    discardRetainedSegments ? UbWorkOutcome::FLUSHED : UbWorkOutcome::FAILED,
                    discardRetainedSegments);
            }
            const auto canonical = m_rtpTpGroups.find(groupIdentity.localId);
            if (canonical == m_rtpTpGroups.end() || canonical->second != group ||
                ResolveRtpTpGroup(groupIdentity) != group)
            {
                return UbTpgMemberMutationStatus::NOT_FOUND;
            }
            NS_ABORT_MSG_IF(m_transaction != nullptr &&
                                m_transaction->HasRtpTpGroupWork(groupIdentity),
                            "Typed last TPG member cleanup did not drain exact group work");
            const int32_t currentIndex = group->FindMemberIndex(tpIdentity);
            if (currentIndex < 0)
            {
                return UbTpgMemberMutationStatus::NOT_FOUND;
            }
            FinalizeRtpTpGroupMemberRemoval(group, static_cast<uint32_t>(currentIndex));
            group->ResumeNewOperations();
            return UbTpgMemberMutationStatus::REMOVED;
        }
        group->FreezeNewOperations();
        group->SetPendingLastMemberRemoval(tpIdentity);
        return UbTpgMemberMutationStatus::PENDING;
    }

    FinalizeRtpTpGroupMemberRemoval(group, static_cast<uint32_t>(memberIndex));
    return UbTpgMemberMutationStatus::REMOVED;
}

void
UbController::FinalizeRtpTpGroupDestroy(Ptr<UbTransportGroup> group)
{
    NS_ABORT_MSG_IF(group == nullptr, "TPG destroy finalize requires an exact group");
    const UbLifecycleResourceIdentity identity = group->GetLifecycleIdentity();
    Ptr<UbLifecycleCoordinator> coordinator =
        UbLifecycleCoordinator::GetOrCreate(GetObject<Node>());
    NS_ABORT_MSG_IF(m_transaction != nullptr && m_transaction->HasRtpTpGroupWork(identity),
                    "TPG destroy finalize requires drained group work");
    if (m_transaction != nullptr)
    {
        m_transaction->RetireRtpTxQueue(identity);
        m_transaction->EraseRtpTpGroupBindings(identity);
    }
    while (group->GetMemberCount() != 0)
    {
        FinalizeRtpTpGroupMemberRemoval(group, group->GetMemberCount() - 1);
    }
    if (!coordinator->GetSnapshot(identity).retired)
    {
        coordinator->RecordSuccessfulTerminal(identity);
        coordinator->RetireResource(identity);
    }
    const auto record = m_rtpTpGroups.find(identity.localId);
    NS_ABORT_MSG_IF(record == m_rtpTpGroups.end() || record->second != group,
                    "TPG destroy finalize requires the canonical collection record");
    m_rtpTpGroups.erase(record);
}

UbDestroyStatus
UbController::DestroyRtpTpGroup(const UbLifecycleResourceIdentity& groupIdentity,
                                UbDestroyReason reason)
{
    Ptr<UbTransportGroup> group = ResolveRtpTpGroup(groupIdentity);
    if (group == nullptr && m_disposing && reason == UbDestroyReason::SIMULATION_STOP)
    {
        const auto canonical = m_rtpTpGroups.find(groupIdentity.localId);
        if (canonical != m_rtpTpGroups.end() && canonical->second != nullptr &&
            canonical->second->GetLifecycleIdentity() == groupIdentity)
        {
            group = canonical->second;
        }
    }
    if (group == nullptr)
    {
        return UbDestroyStatus::NOT_FOUND;
    }
    const UbTpgCleanupMode requestedMode =
        reason == UbDestroyReason::SIMULATION_STOP
            ? UbTpgCleanupMode::FORCE
            : (reason == UbDestroyReason::ERROR ? UbTpgCleanupMode::ABORT
                                                : UbTpgCleanupMode::GRACEFUL);
    group->RequestDestroy(requestedMode);
    group->FreezeNewOperations();
    if (group->GetCleanupMode() != UbTpgCleanupMode::GRACEFUL && m_transaction != nullptr)
    {
        const bool discardRetainedSegments = group->GetCleanupMode() == UbTpgCleanupMode::FORCE;
        m_transaction->TerminateRtpTpGroupOperations(
            groupIdentity,
            discardRetainedSegments ? UbWorkOutcome::FLUSHED : UbWorkOutcome::FAILED,
            discardRetainedSegments);
        const auto canonical = m_rtpTpGroups.find(groupIdentity.localId);
        if (canonical == m_rtpTpGroups.end() || canonical->second != group)
        {
            return UbDestroyStatus::COMPLETED;
        }
    }
    if (m_transaction != nullptr && m_transaction->HasRtpTpGroupWork(groupIdentity))
    {
        return UbDestroyStatus::PENDING;
    }

    FinalizeRtpTpGroupDestroy(group);
    return UbDestroyStatus::COMPLETED;
}

void
UbController::NotifyRtpTpGroupWorkChanged(
    const UbLifecycleResourceIdentity& groupIdentity)
{
    Ptr<UbTransportGroup> group = ResolveRtpTpGroup(groupIdentity);
    if (group == nullptr ||
        (!group->IsDestroyPending() && !group->HasPendingLastMemberRemoval()))
    {
        return;
    }
    if (m_transaction != nullptr && m_transaction->HasRtpTpGroupWork(groupIdentity))
    {
        return;
    }
    if (group->IsDestroyPending())
    {
        FinalizeRtpTpGroupDestroy(group);
        return;
    }
    const int32_t index = group->FindMemberIndex(group->GetPendingLastMemberRemoval());
    NS_ABORT_MSG_IF(index < 0, "Pending last TPG member disappeared before use-lease drain");
    FinalizeRtpTpGroupMemberRemoval(group, static_cast<uint32_t>(index));
    group->ClearPendingLastMemberRemoval();
    group->ResumeNewOperations();
}

// Port management
void UbController::AddPortDestination(Ptr<UbPort> port, uint32_t destination)
{
    NS_LOG_FUNCTION(this << port << destination);
    if (!port) {
        NS_LOG_WARN("Trying to add null port");
        return;
    }

    m_portsByDestination[destination].push_back(port);
    NS_LOG_DEBUG("Added port to destination " << destination);
}

void UbController::RemovePortDestination(Ptr<UbPort> port, uint32_t destination)
{
    NS_LOG_FUNCTION(this << port << destination);
    if (!port) {
        NS_LOG_WARN("Trying to remove null port");
        return;
    }

    auto it = m_portsByDestination.find(destination);
    if (it != m_portsByDestination.end())
    {
        std::vector<Ptr<UbPort>>& ports = it->second;
        ports.erase(std::remove(ports.begin(), ports.end(), port), ports.end());

        if (ports.empty()) {
            m_portsByDestination.erase(it);
        }
        NS_LOG_DEBUG("Removed port from destination " << destination);
    }
}

std::vector<Ptr<UbPort>> UbController::GetAvailablePorts(uint32_t destination) const
{
    NS_LOG_FUNCTION(this << destination);

    auto it = m_portsByDestination.find(destination);
    if (it != m_portsByDestination.end())
    {
        return it->second;
    }

    return {};
}

Ptr<UbFunction> UbController::GetUbFunction()
{
    return m_function;
}

Ptr<UbTransaction> UbController::GetUbTransaction()
{
    return m_transaction;
}

bool
UbController::SubmitUrmaWqe(uint32_t jettyNum,
                            Ptr<UbWqe> wqe,
                            Callback<void, uint32_t, uint32_t> completionObserver)
{
    if (wqe == nullptr || m_function == nullptr || m_transaction == nullptr)
    {
        return false;
    }
    Ptr<UbJetty> jetty = m_function->GetJetty(jettyNum);
    if (jetty == nullptr)
    {
        return false;
    }

    const bool hasRtpBinding = m_transaction->HasJettyTpBinding(jettyNum);
    const bool hasCtpPreparation =
        m_ctpTransportService != nullptr && m_ctpTransportService->HasJettyPreparation(jettyNum);
    if (hasRtpBinding == hasCtpPreparation)
    {
        return false;
    }
    bool accepted = false;
    if (hasRtpBinding)
    {
        accepted = m_function->SubmitWqe(wqe, jettyNum).status ==
                   UbWqeSubmissionStatus::ACCEPTED;
    }
    else
    {
        accepted = m_ctpTransportService->SubmitPreparedWqe(jettyNum, wqe);
    }
    if (!accepted)
    {
        return false;
    }
    if (!completionObserver.IsNull())
    {
        jetty->SetCompletionObserver(completionObserver);
    }
    return true;
}

Ptr<UbLdstEndpoint> UbController::GetLdstEndpoint()
{
    return m_ldstEndpoint;
}

void
UbController::CreateCtpEntity(uint32_t entityId, const std::vector<uint32_t>& memberPorts)
{
    std::vector<uint16_t> ports = ValidateCtpEntityDefinition(entityId, memberPorts);
    m_ctpEntityPorts.emplace(entityId, std::move(ports));
}

std::vector<uint16_t>
UbController::ValidateCtpEntityDefinition(uint32_t entityId,
                                          const std::vector<uint32_t>& memberPorts) const
{
    NS_ABORT_MSG_IF(m_ctpEntitiesFrozen, "Cannot create a CTP Entity after the registry is frozen");
    NS_ABORT_MSG_IF(entityId == 0 || entityId > UB_COMPACT_EID_MAX,
                    "CTP Entity ID must be in the non-zero compact EID range: " << entityId);
    NS_ABORT_MSG_IF(memberPorts.empty(), "Cannot create an empty CTP Entity " << entityId);
    NS_ABORT_MSG_IF(m_ctpEntityPorts.find(entityId) != m_ctpEntityPorts.end(), "Duplicate CTP Entity ID " << entityId);
    Ptr<Node> node = GetObject<Node>();
    NS_ABORT_MSG_IF(node == nullptr, "CTP Entity registry requires an owning node");
    Ptr<UbSwitch> sw = node->GetObject<UbSwitch>();
    NS_ABORT_MSG_IF(sw == nullptr || sw->GetNodeType() != UB_DEVICE,
                    "CTP Entity registry can only be installed on a UB_DEVICE host");
    std::vector<uint16_t> ports;
    ports.reserve(memberPorts.size());
    for (uint32_t portId : memberPorts)
    {
        NS_ABORT_MSG_IF(portId > CTP_MAX_CNA16_PORT_ID,
                        "CTP Entity " << entityId << " port " << portId
                                      << " cannot be encoded as CNA16");
        Ptr<UbPort> port = nullptr;
        for (uint32_t deviceIndex = 0; deviceIndex < node->GetNDevices(); ++deviceIndex)
        {
            Ptr<UbPort> candidate = DynamicCast<UbPort>(node->GetDevice(deviceIndex));
            if (candidate != nullptr && candidate->GetIfIndex() == portId)
            {
                port = candidate;
                break;
            }
        }
        NS_ABORT_MSG_IF(port == nullptr,
                        "CTP Entity " << entityId << " port " << portId
                                      << " is not a UB port on node " << node->GetId());
        ports.push_back(static_cast<uint16_t>(portId));
    }
    std::sort(ports.begin(), ports.end());
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
    NS_ABORT_MSG_IF(ports.empty(), "CTP Entity " << entityId << " has no unique ports");
    return ports;
}

bool
UbController::HasCtpEntity(uint32_t entityId) const
{
    return m_ctpEntityPorts.find(entityId) != m_ctpEntityPorts.end();
}

bool
UbController::IsCtpEntityPortMember(uint32_t entityId, uint32_t portId) const
{
    const auto entity = m_ctpEntityPorts.find(entityId);
    return entity != m_ctpEntityPorts.end() &&
           std::binary_search(entity->second.begin(), entity->second.end(), portId);
}

const std::vector<uint16_t>&
UbController::GetCtpEntityPorts(uint32_t entityId) const
{
    const auto entity = m_ctpEntityPorts.find(entityId);
    NS_ABORT_MSG_IF(entity == m_ctpEntityPorts.end(), "Unknown CTP Entity ID " << entityId);
    return entity->second;
}

uint16_t
UbController::SelectCtpEntityMemberPort(uint32_t entityId, uint8_t loadBalanceField) const
{
    NS_ABORT_MSG_IF(!m_ctpEntitiesFrozen,
                    "Cannot select a CTP Entity member before the registry is frozen");
    const std::vector<uint16_t>& ports = GetCtpEntityPorts(entityId);
    return ports[loadBalanceField % ports.size()];
}

std::vector<uint32_t>
UbController::CollectDefaultCtpEntityPorts() const
{
    Ptr<Node> node = GetObject<Node>();
    NS_ABORT_MSG_IF(node == nullptr, "Default CTP Entity requires a host node");
    Ptr<UbSwitch> sw = node->GetObject<UbSwitch>();
    NS_ABORT_MSG_IF(sw == nullptr || sw->GetNodeType() != UB_DEVICE,
                    "Default CTP Entity requires a UB_DEVICE host");

    std::vector<uint32_t> memberPorts;
    for (uint32_t deviceIndex = 0; deviceIndex < node->GetNDevices(); ++deviceIndex)
    {
        Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(deviceIndex));
        if (port != nullptr)
        {
            memberPorts.push_back(port->GetIfIndex());
        }
    }
    return memberPorts;
}

void
UbController::CreateDefaultCtpEntity(uint32_t entityId)
{
    NS_ABORT_MSG_IF(HasCtpEntity(entityId), "Default CTP Entity " << entityId << " already exists");
    CreateCtpEntity(entityId, CollectDefaultCtpEntityPorts());
}

void
UbController::FreezeCtpEntities()
{
    m_ctpEntitiesFrozen = true;
}

bool
UbController::AreCtpEntitiesFrozen() const
{
    return m_ctpEntitiesFrozen;
}

Ptr<UbCtpTransportService> UbController::GetCtpTransportService()
{
    if (m_ctpTransportService == nullptr)
    {
        m_ctpTransportService = CreateObject<UbCtpTransportService>();
        m_ctpTransportService->SetNode(GetObject<Node>());
        if (!m_ctpFirstPacketSendsCallback.IsNull())
        {
            m_ctpTransportService->TraceConnectWithoutContext("FirstPacketSendsNotify",
                                                              m_ctpFirstPacketSendsCallback);
        }
        if (!m_ctpLastPacketAcksCallback.IsNull())
        {
            m_ctpTransportService->TraceConnectWithoutContext("LastPacketACKsNotify",
                                                              m_ctpLastPacketAcksCallback);
        }
    }
    return m_ctpTransportService;
}

Ptr<UbCtpTransportService> UbController::PeekCtpTransportService() const
{
    return m_ctpTransportService;
}

void
UbController::SetCtpPacketTimingTraceCallbacks(CtpPacketTimingCallback firstPacketSendsCallback,
                                               CtpPacketTimingCallback lastPacketAcksCallback)
{
    m_ctpFirstPacketSendsCallback = firstPacketSendsCallback;
    m_ctpLastPacketAcksCallback = lastPacketAcksCallback;
    if (m_ctpTransportService != nullptr && !m_ctpFirstPacketSendsCallback.IsNull())
    {
        m_ctpTransportService->TraceConnectWithoutContext("FirstPacketSendsNotify",
                                                          m_ctpFirstPacketSendsCallback);
    }
    if (m_ctpTransportService != nullptr && !m_ctpLastPacketAcksCallback.IsNull())
    {
        m_ctpTransportService->TraceConnectWithoutContext("LastPacketACKsNotify",
                                                          m_ctpLastPacketAcksCallback);
    }
}

} // namespace ns3
