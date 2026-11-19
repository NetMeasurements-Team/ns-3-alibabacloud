// SPDX-License-Identifier: GPL-2.0-only
#include "ub-function.h"

#include "ub-transaction-rules.h"
#include "ub-ctp.h"
#include "ub-transaction.h"

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-datatype.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbFunction");

NS_OBJECT_ENSURE_REGISTERED(UbFunction);

TypeId UbFunction::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbFunction")
        .SetParent<Object>()
        .SetGroupName("UnifiedBus");
    return tid;
}

UbFunction::UbFunction()
{
    NS_LOG_DEBUG("UbFunction created");
    m_ldstApi = CreateObject<UbLdstApi>();
    m_jettyManager = CreateObject<UbJettyManager>();
}

UbFunction::~UbFunction()
{
}

Ptr<UbLdstApi> UbFunction::GetUbLdstApi()
{
    return m_ldstApi;
}

void UbFunction::Init(uint32_t nodeId)
{
    m_nodeId = nodeId;
    m_ldstApi->SetNodeId(m_nodeId);
    m_jettyManager->Init(nodeId,
                         UbLifecycleCoordinator::GetOrCreate(NodeList::GetNode(nodeId)));
}

void
UbFunction::BindTransaction(Ptr<UbTransaction> transaction)
{
    m_transaction = transaction;
    m_jettyManager->BindTransaction(transaction);
}

void
UbFunction::CreateJetty(uint32_t src, uint32_t dest, uint32_t jettyNum, UbJettyLifetime lifetime)
{
    NS_LOG_DEBUG(this << src << dest << jettyNum);
    // 创建新的Jetty
    Ptr<UbJetty> jetty = CreateObject<UbJetty>();
    jetty->Init();
    jetty->SetNodeId(m_nodeId);
    jetty->SetJettyNum(jettyNum);
    jetty->SetSrc(src);
    jetty->SetDest(dest);
    jetty->SetTransaction(PeekPointer(m_transaction));
    m_jettyManager->RegisterJetty(jetty, lifetime);
    NS_LOG_DEBUG("Created jetty success");
}

void
UbFunction::CreateJetty(uint32_t src, uint32_t jettyNum, UbJettyLifetime lifetime)
{
    NS_LOG_DEBUG(this << src << jettyNum);
    Ptr<UbJetty> jetty = CreateObject<UbJetty>();
    jetty->Init();
    jetty->SetNodeId(m_nodeId);
    jetty->SetJettyNum(jettyNum);
    jetty->SetSrc(src);
    jetty->ClearDest();
    jetty->SetTransaction(PeekPointer(m_transaction));
    m_jettyManager->RegisterJetty(jetty, lifetime);
    NS_LOG_DEBUG("Created unbound jetty success");
}

bool UbFunction::IsJettyExists(uint32_t jettyNum)
{
    return m_jettyManager->IsJettyExists(jettyNum);
}

Ptr<UbJetty> UbFunction::GetJetty(uint32_t jettyNum)
{
    return m_jettyManager->GetJetty(jettyNum);
}

Ptr<UbTransaction> UbFunction::GetTransaction()
{
    NS_ABORT_MSG_IF(m_transaction == nullptr, "Function has no bound Transaction owner");
    return m_transaction;
}

UbDestroyStatus
UbFunction::DestroyJetty(uint32_t jettyNum,
                         UbDestroyReason reason,
                         UbDestroyCallback callback)
{
    return m_jettyManager->DestroyJetty(jettyNum, reason, callback);
}

Ptr<UbWqe> UbFunction::CreateWqe(uint32_t src,
                                 uint32_t dest,
                                 uint32_t size,
                                 uint32_t wqeId,
                                 UbTransactionOpcode type)
{
    NS_LOG_DEBUG(this);
    // 创建新的 WQE
    Ptr<UbWqe> ubWqe = CreateObject<UbWqe>();
    ubWqe->SetSrc(src);
    ubWqe->SetDest(dest);
    ubWqe->SetType(type);
    ubWqe->SetSize(size);
    ubWqe->SetWqeId(wqeId);
    ubWqe->SetSegmentKind(UbTransactionSegmentKind::REQUEST);
    // These fields are undefined before the WQE is enqueued into a concrete Jetty.
    ubWqe->SetOriginJettyNum(UINT32_MAX);
    ubWqe->SetRequestTassn(UINT32_MAX);
    ubWqe->SetRequestOpcode(type);
    ubWqe->SetResponseBytes(type == UbTransactionOpcode::READ ? size : 0);
    ubWqe->SetRemoteAddress(0);
    const auto* rule = FindTransactionRule(type);
    ubWqe->SetNeedsTransactionResponse(rule != nullptr && rule->requiresResponse);
    ubWqe->SetResLenBytes(size);
    ubWqe->SetPayloadBytes(type == UbTransactionOpcode::READ ? 0 : size);
    ubWqe->SetCarrierBytes(type == UbTransactionOpcode::READ ? ubWqe->GetTaSsnSize() : size);
    return ubWqe;
}

Ptr<UbWqe> UbFunction::CreateWqe(uint32_t src, uint32_t dest, uint32_t size, uint32_t wqeId)
{
    return CreateWqe(src, dest, size, wqeId, UbTransactionOpcode::WRITE);
}

void UbFunction::PushWqeToJetty(Ptr<UbWqe> wqe, uint32_t jettyNum)
{
    const UbWqeSubmissionResult result = SubmitWqe(wqe, jettyNum);
    if (result.status != UbWqeSubmissionStatus::ACCEPTED) {
        NS_LOG_WARN("WQE submission rejected with status " << static_cast<uint32_t>(result.status));
    }
}

UbWqeSubmissionResult UbFunction::SubmitWqe(Ptr<UbWqe> wqe, uint32_t jettyNum)
{
    NS_LOG_DEBUG(this);
    Ptr<UbJetty> ubJetty = GetJetty(jettyNum);
    if (ubJetty == nullptr) {
        return {UbWqeSubmissionStatus::JETTY_NOT_FOUND, {}};
    }
    if (!m_jettyManager->IsAcceptingWqes(jettyNum)) {
        return {UbWqeSubmissionStatus::JETTY_DESTROYING, {}};
    }
    if (!ubJetty->CanAcceptWqe(wqe)) {
        return {UbWqeSubmissionStatus::JETTY_BINDING_MISMATCH, {}};
    }
    UbTransactionFields fields = wqe->GetTransactionFields();
    if (wqe->GetType() == UbTransactionOpcode::ATOMIC_SYNC && !fields.taskId)
        fields.taskId = static_cast<uint16_t>(wqe->GetWqeId() & 0xffffu);
    if (!fields.memoryAccess && wqe->GetType() != UbTransactionOpcode::SEND &&
        wqe->GetType() != UbTransactionOpcode::SEND_WITH_IMMEDIATE &&
        wqe->GetType() != UbTransactionOpcode::MANAGEMENT)
    {
        fields.memoryAccess = UbMemoryAccessFields{wqe->GetRemoteAddress(), wqe->GetRemoteTokenId(), wqe->GetSize()};
    }
    const uint32_t validationPayload =
        (wqe->GetPayloadBytes() == 0 && wqe->GetType() != UbTransactionOpcode::READ)
            ? wqe->GetSize() : wqe->GetPayloadBytes();
    UbTransactionRequestInput request;
    request.opcode = wqe->GetType();
    request.logicalBytes = wqe->GetSize();
    request.payloadBytes = validationPayload;
    request.fields = fields;
    request.atomicOptions = wqe->GetAtomicOptions();
    request.maxPayloadBytes =
        IsAtomicTransaction(wqe->GetType()) ? UB_MTU_BYTE : UINT32_MAX;
    request.origin = UbTransactionRequestOrigin::LOCAL_SUBMISSION;
    const auto ruleResult = ValidateTransactionRequest(request);
    if (!ruleResult.IsValid())
    {
        NS_LOG_WARN("WQE transaction validation failed: " << ruleResult.message);
        return {(IsAtomicTransaction(wqe->GetType()) || wqe->GetAtomicOptions().has_value()) ? UbWqeSubmissionStatus::INVALID_ATOMIC_REQUEST
                                     : UbWqeSubmissionStatus::INVALID_TRANSACTION_REQUEST, {}, ruleResult};
    }
    wqe->SetTransactionFields(ruleResult.fields);
    wqe->SetTransactionResolved(ruleResult.resolved);
    if (!ValidateAtomicWqe(wqe, jettyNum, ruleResult.resolved))
    {
        return {UbWqeSubmissionStatus::INVALID_ATOMIC_REQUEST, {}};
    }
    const UbRtpTransportReference* transportReference =
        GetTransaction()->FindRtpTransportReference(jettyNum);
    if (transportReference == nullptr) {
        return {UbWqeSubmissionStatus::NO_DIRECT_TRANSPORT, {}};
    }
    UbTransaction::RtpTransportResolution transportResolution;
    switch (GetTransaction()->PreflightRtpOperation(*transportReference,
                                                    transportResolution))
    {
    case UbRtpOperationPreflightResult::NOT_ACCEPTING:
        return {UbWqeSubmissionStatus::TPG_NOT_ACCEPTING, {}};
    case UbRtpOperationPreflightResult::NO_MEMBER:
        return {UbWqeSubmissionStatus::TPG_NO_MEMBER, {}};
    case UbRtpOperationPreflightResult::STALE_REFERENCE:
        return {UbWqeSubmissionStatus::STALE_TRANSPORT_REFERENCE, {}};
    case UbRtpOperationPreflightResult::ACCEPTING:
        break;
    }
    if (GetTransaction()->HasWqeOperation(jettyNum, wqe->GetWqeId())) {
        return {UbWqeSubmissionStatus::DUPLICATE_OPERATION, {}};
    }

    Ptr<UbLifecycleCoordinator> coordinator =
        UbLifecycleCoordinator::GetOrCreate(NodeList::GetNode(m_nodeId));
    const uint64_t lifecycleLocalId =
        m_jettyManager->ResolveWqeLifecycleLocalId(jettyNum, wqe->GetWqeId());
    const UbLifecycleResourceIdentity wqeIdentity =
        coordinator->OpenResource(UbLifecycleResourceKind::WQE, lifecycleLocalId);
    NS_ABORT_MSG_IF(!GetTransaction()->TryAdmitRtpWqe(jettyNum,
                                                      wqe,
                                                      wqeIdentity,
                                                      transportResolution),
                    "Validated RTP WQE admission changed before commit");

    ubJetty->SetNodeId(m_nodeId);
    ubJetty->PushWqe(wqe);
    m_jettyManager->RegisterWqe(jettyNum, wqe->GetWqeId(), wqeIdentity);
    GetTransaction()->AddWqe(jettyNum, wqe);
    GetTransaction()->TriggerScheduleWqeSegment(jettyNum, wqe, true);
    return {UbWqeSubmissionStatus::ACCEPTED, wqeIdentity};
}

UbWqeSubmissionResult
UbFunction::SubmitCtpWqe(Ptr<UbWqe> wqe,
                         uint32_t jettyNum,
                         const UbCtpEntityKey& key)
{
    Ptr<UbJetty> jetty = GetJetty(jettyNum);
    if (jetty == nullptr)
    {
        return {UbWqeSubmissionStatus::JETTY_NOT_FOUND, {}};
    }
    if (!m_jettyManager->IsAcceptingWqes(jettyNum))
    {
        return {UbWqeSubmissionStatus::JETTY_DESTROYING, {}};
    }
    if (!jetty->CanAcceptWqe(wqe))
    {
        return {UbWqeSubmissionStatus::JETTY_BINDING_MISMATCH, {}};
    }
    auto service = NodeList::GetNode(m_nodeId)->GetObject<UbController>()->PeekCtpTransportService();
    if (service == nullptr || !service->HasJettyPreparation(jettyNum))
    {
        return {UbWqeSubmissionStatus::CTP_NOT_PREPARED, {}};
    }
    if (!service->CanSubmitWqe(jettyNum, wqe, key))
    {
        return {UbWqeSubmissionStatus::JETTY_BINDING_MISMATCH, {}};
    }
    if (!jetty->HasValidCtpWindow())
    {
        return {UbWqeSubmissionStatus::INVALID_CTP_WINDOW, {}};
    }
    UbTransactionFields fields = wqe->GetTransactionFields();
    if (wqe->GetType() == UbTransactionOpcode::ATOMIC_SYNC && !fields.taskId)
        fields.taskId = static_cast<uint16_t>(wqe->GetWqeId() & 0xffffu);
    if (!fields.memoryAccess && wqe->GetType() != UbTransactionOpcode::SEND &&
        wqe->GetType() != UbTransactionOpcode::SEND_WITH_IMMEDIATE &&
        wqe->GetType() != UbTransactionOpcode::MANAGEMENT)
    {
        fields.memoryAccess = UbMemoryAccessFields{wqe->GetRemoteAddress(), wqe->GetRemoteTokenId(), wqe->GetSize()};
    }
    if (fields.memoryAccess &&
        (fields.memoryAccess->length != wqe->GetSize() ||
         wqe->GetSize() > UINT64_MAX - fields.memoryAccess->address))
    {
        return {UbWqeSubmissionStatus::INVALID_TRANSACTION_REQUEST, {}};
    }
    const uint32_t validationPayload =
        (wqe->GetPayloadBytes() == 0 && wqe->GetType() != UbTransactionOpcode::READ)
            ? wqe->GetSize() : wqe->GetPayloadBytes();
    UbTransactionRequestInput request;
    request.opcode = wqe->GetType();
    request.logicalBytes = wqe->GetSize();
    request.payloadBytes = validationPayload;
    request.fields = fields;
    request.atomicOptions = wqe->GetAtomicOptions();
    request.maxPayloadBytes =
        IsAtomicTransaction(wqe->GetType()) ? UbCtpTransportConstraints{}.maxSegmentBytes
                                            : UINT32_MAX;
    request.origin = UbTransactionRequestOrigin::LOCAL_SUBMISSION;
    const auto ruleResult = ValidateTransactionRequest(request);
    if (!ruleResult.IsValid())
    {
        NS_LOG_WARN("WQE transaction validation failed: " << ruleResult.message);
        return {(IsAtomicTransaction(wqe->GetType()) || wqe->GetAtomicOptions().has_value()) ? UbWqeSubmissionStatus::INVALID_ATOMIC_REQUEST
                                     : UbWqeSubmissionStatus::INVALID_TRANSACTION_REQUEST, {}, ruleResult};
    }
    if (!jetty->CanReserveCtpWqe(wqe, UbCtpTransportConstraints{}.maxSegmentBytes))
    {
        return {UbWqeSubmissionStatus::CTP_SEQUENCE_EXHAUSTED, {}};
    }
    wqe->SetTransactionFields(ruleResult.fields);
    wqe->SetTransactionResolved(ruleResult.resolved);
    if (!ValidateAtomicWqe(wqe, jettyNum, ruleResult.resolved))
    {
        return {UbWqeSubmissionStatus::INVALID_ATOMIC_REQUEST, {}};
    }
    if (GetTransaction()->HasWqeOperation(jettyNum, wqe->GetWqeId()))
    {
        return {UbWqeSubmissionStatus::DUPLICATE_OPERATION, {}};
    }

    Ptr<UbLifecycleCoordinator> coordinator =
        UbLifecycleCoordinator::GetOrCreate(NodeList::GetNode(m_nodeId));
    const uint64_t lifecycleLocalId =
        m_jettyManager->ResolveWqeLifecycleLocalId(jettyNum, wqe->GetWqeId());
    const UbLifecycleResourceIdentity wqeIdentity =
        coordinator->OpenResource(UbLifecycleResourceKind::WQE, lifecycleLocalId);
    NS_ABORT_MSG_IF(!GetTransaction()->TryAdmitCtpWqe(jettyNum,
                                                      wqe,
                                                      wqeIdentity,
                                                      key),
                    "Validated CTP WQE admission changed before commit");

    jetty->SetNodeId(m_nodeId);
    jetty->PushWqe(wqe, UbCtpTransportConstraints{}.maxSegmentBytes);
    m_jettyManager->RegisterWqe(jettyNum, wqe->GetWqeId(), wqeIdentity);
    GetTransaction()->AddWqe(jettyNum, wqe);
    GetTransaction()->TriggerScheduleCtpWqe(jettyNum, wqe->GetWqeId());
    return {UbWqeSubmissionStatus::ACCEPTED, wqeIdentity};
}

bool
UbFunction::ValidateAtomicWqe(Ptr<UbWqe> wqe,
                              uint32_t jettyNum,
                              const UbTransactionResolved& resolved)
{
    if (!IsAtomicTransaction(wqe->GetType()))
    {
        return true;
    }
    if (GetTransaction()->GetTransactionServiceMode(jettyNum) != TransactionServiceMode::ROI)
    {
        std::cerr << "Invalid atomic WQE taskId=" << wqe->GetWqeId()
                  << ": atomic serviceMode must be ROI" << '\n';
        return false;
    }
    if (wqe->HasExplicitPayload() &&
        wqe->GetExplicitPayload()->GetSize() != resolved.requestPayloadBytes)
    {
        std::cerr << "Invalid atomic WQE taskId=" << wqe->GetWqeId()
                  << ": explicit payload size must equal requestPayloadBytes" << '\n';
        return false;
    }
    wqe->SetLogicalBytes(wqe->GetSize());
    wqe->SetPayloadBytes(resolved.requestPayloadBytes);
    wqe->SetResponseBytes(resolved.responsePayloadBytes);
    wqe->SetNeedsTransactionResponse(resolved.requiresResponse);
    // WQE admission advances N once; its transport segment advances P.
    wqe->SetCarrierBytes(wqe->GetSize());
    return true;
}

void
UbFunction::ApplyWqeOutcomeFromTransaction(
    uint32_t jettyNum,
    uint32_t wqeId,
    const UbLifecycleResourceIdentity& wqeIdentity,
    UbWorkOutcome outcome)
{
    m_jettyManager->ApplyWqeOutcome(jettyNum, wqeId, wqeIdentity, outcome);
}

void
UbFunction::NotifyJettyProgress(uint32_t jettyNum)
{
    m_jettyManager->TryFinalizeJettyDestroy(jettyNum);
}

bool
UbFunction::IsJettyAcceptingWqes(uint32_t jettyNum) const
{
    return m_jettyManager->IsAcceptingWqes(jettyNum);
}

bool
UbFunction::ShouldContinueAcceptedWqes(uint32_t jettyNum) const
{
    return m_jettyManager->ShouldContinueAcceptedWqes(jettyNum);
}

bool
UbFunction::ShouldForceDrainAcceptedWqes(uint32_t jettyNum) const
{
    return m_jettyManager->ShouldForceDrainAcceptedWqes(jettyNum);
}

uint64_t
UbFunction::TrackJettyEvent(uint32_t jettyNum)
{
    return m_jettyManager->TrackEvent(jettyNum);
}

void
UbFunction::AttachJettyEvent(uint32_t jettyNum,
                             uint64_t token,
                             EventId event,
                             Callback<void> cancelCleanup)
{
    m_jettyManager->AttachEvent(jettyNum, token, event, cancelCleanup);
}

bool
UbFunction::BeginJettyEvent(uint32_t jettyNum, uint64_t token)
{
    return m_jettyManager->BeginEvent(jettyNum, token);
}

void
UbFunction::ResolveJettyEvent(uint32_t jettyNum, uint64_t token)
{
    m_jettyManager->ResolveEvent(jettyNum, token);
}

bool
UbFunction::TryGetJettyLifecycleIdentity(uint32_t jettyNum,
                                         UbLifecycleResourceIdentity& identity) const
{
    return m_jettyManager->TryGetIdentity(jettyNum, identity);
}

bool
UbFunction::TryGetJettyState(uint32_t jettyNum, UbJettyLifecycleState& state) const
{
    return m_jettyManager->TryGetState(jettyNum, state);
}

bool
UbFunction::TryGetJettyLifetime(uint32_t jettyNum, UbJettyLifetime& lifetime) const
{
    return m_jettyManager->TryGetLifetime(jettyNum, lifetime);
}

bool
UbFunction::IsJettyDrainReady(uint32_t jettyNum) const
{
    return m_jettyManager->IsDrainReady(jettyNum);
}

uint64_t
UbFunction::GetResidentWqeCount() const
{
    return m_jettyManager == nullptr ? 0 : m_jettyManager->GetResidentWqeCount();
}

UbJettyStorageSnapshot
UbFunction::GetJettyStorageSnapshot() const
{
    return m_jettyManager == nullptr ? UbJettyStorageSnapshot{}
                                     : m_jettyManager->GetStorageSnapshot();
}

void
UbFunction::RegisterJettyRtpBinding(
    uint32_t jettyNum,
    const UbLifecycleResourceIdentity& transportIdentity)
{
    m_jettyManager->ReplaceRtpBinding(jettyNum, transportIdentity);
}

void
UbFunction::RegisterJettyCtpBinding(uint32_t jettyNum,
                                    Ptr<UbCtpTransportService> service)
{
    m_jettyManager->RegisterCtpBinding(jettyNum, service);
}

bool
UbFunction::UnregisterJettyCtpBinding(uint32_t jettyNum,
                                      Ptr<UbCtpTransportService> service)
{
    return m_jettyManager->UnregisterCtpBinding(jettyNum, service);
}

void UbFunction::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_ldstApi = nullptr;
    if (m_jettyManager != nullptr)
    {
        m_jettyManager->DisposeAll();
    }
    m_jettyManager = nullptr;
    m_transaction = nullptr;

    Object::DoDispose();
}

NS_OBJECT_ENSURE_REGISTERED(UbJettyManager);

TypeId
UbJettyManager::GetTypeId()
{
    static TypeId tid = TypeId("ns3::UbJettyManager")
                            .SetParent<Object>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbJettyManager>();
    return tid;
}

void
UbJettyManager::Init(uint32_t nodeId, Ptr<UbLifecycleCoordinator> coordinator)
{
    m_nodeId = nodeId;
    m_coordinator = coordinator;
    m_jettyEvents.Init(coordinator);
}

void
UbJettyManager::BindTransaction(Ptr<UbTransaction> transaction)
{
    NS_ABORT_MSG_IF(transaction == nullptr, "Jetty manager requires Transaction owner");
    m_transaction = transaction;
}

Ptr<UbTransaction>
UbJettyManager::GetTransaction() const
{
    NS_ABORT_MSG_IF(m_transaction == nullptr, "Jetty manager has no bound Transaction owner");
    return m_transaction;
}

void
UbJettyManager::RegisterJetty(Ptr<UbJetty> jetty, UbJettyLifetime lifetime)
{
    NS_ABORT_MSG_IF(jetty == nullptr, "Cannot register a null Jetty");
    const uint32_t jettyNum = jetty->GetJettyNum();
    NS_ABORT_MSG_IF(m_jettyRecordsByNum.find(jettyNum) != m_jettyRecordsByNum.end(),
                    "Cannot replace an active Jetty collection entry");
    Ptr<UbLifecycleCoordinator> coordinator = m_coordinator;
    NS_ABORT_MSG_IF(coordinator == nullptr, "Jetty manager has no lifecycle coordinator");
    JettyRecord record;
    record.jetty = jetty;
    record.identity = coordinator->OpenResource(UbLifecycleResourceKind::JETTY, jettyNum);
    record.lifetime = lifetime;
    m_jettyRecordsByNum.emplace(jettyNum, std::move(record));
}

bool
UbJettyManager::IsJettyExists(uint32_t jettyNum) const
{
    return m_jettyRecordsByNum.find(jettyNum) != m_jettyRecordsByNum.end();
}

Ptr<UbJetty>
UbJettyManager::GetJetty(uint32_t jettyNum) const
{
    const auto it = m_jettyRecordsByNum.find(jettyNum);
    return it == m_jettyRecordsByNum.end() ? nullptr : it->second.jetty;
}

bool
UbJettyManager::IsAcceptingWqes(uint32_t jettyNum) const
{
    const auto it = m_jettyRecordsByNum.find(jettyNum);
    return it != m_jettyRecordsByNum.end() && it->second.state == UbJettyLifecycleState::ACTIVE;
}

bool
UbJettyManager::ShouldContinueAcceptedWqes(uint32_t jettyNum) const
{
    const auto it = m_jettyRecordsByNum.find(jettyNum);
    if (it == m_jettyRecordsByNum.end())
    {
        return false;
    }
    return it->second.state == UbJettyLifecycleState::ACTIVE ||
           (it->second.state == UbJettyLifecycleState::DRAINING &&
            it->second.cleanupMode == CleanupMode::GRACEFUL);
}

bool
UbJettyManager::ShouldForceDrainAcceptedWqes(uint32_t jettyNum) const
{
    const auto it = m_jettyRecordsByNum.find(jettyNum);
    return it != m_jettyRecordsByNum.end() && it->second.state == UbJettyLifecycleState::DRAINING &&
           it->second.cleanupMode == CleanupMode::FORCE;
}

UbJettyManager::CleanupMode
UbJettyManager::GetCleanupMode(UbDestroyReason reason)
{
    switch (reason)
    {
    case UbDestroyReason::PROTOCOL_COMPLETE:
    case UbDestroyReason::EXPLICIT:
        return CleanupMode::GRACEFUL;
    case UbDestroyReason::ERROR:
        return CleanupMode::ABORT;
    case UbDestroyReason::SIMULATION_STOP:
        return CleanupMode::FORCE;
    }
    NS_ABORT_MSG("Unknown Jetty destroy reason");
}

bool
UbJettyManager::TryGetState(uint32_t jettyNum, UbJettyLifecycleState& state) const
{
    const auto it = m_jettyRecordsByNum.find(jettyNum);
    if (it == m_jettyRecordsByNum.end())
    {
        return false;
    }
    state = it->second.state;
    return true;
}

bool
UbJettyManager::TryGetLifetime(uint32_t jettyNum, UbJettyLifetime& lifetime) const
{
    const auto it = m_jettyRecordsByNum.find(jettyNum);
    if (it == m_jettyRecordsByNum.end())
    {
        return false;
    }
    lifetime = it->second.lifetime;
    return true;
}

bool
UbJettyManager::TryGetIdentity(uint32_t jettyNum,
                               UbLifecycleResourceIdentity& identity) const
{
    const auto it = m_jettyRecordsByNum.find(jettyNum);
    if (it == m_jettyRecordsByNum.end())
    {
        return false;
    }
    identity = it->second.identity;
    return true;
}

void
UbJettyManager::ReplaceRtpBinding(
    uint32_t jettyNum,
    const UbLifecycleResourceIdentity& transportIdentity)
{
    auto it = m_jettyRecordsByNum.find(jettyNum);
    NS_ABORT_MSG_IF(it == m_jettyRecordsByNum.end() ||
                        it->second.state != UbJettyLifecycleState::ACTIVE,
                    "Cannot bind RTP transport to a missing or draining Jetty");
    it->second.hasCtpBinding = false;
    NS_ABORT_MSG_IF(transportIdentity.generation == 0,
                    "Jetty RTP binding requires an exact transport generation");
}

void
UbJettyManager::RegisterCtpBinding(uint32_t jettyNum,
                                   Ptr<UbCtpTransportService> service)
{
    auto it = m_jettyRecordsByNum.find(jettyNum);
    NS_ABORT_MSG_IF(it == m_jettyRecordsByNum.end() ||
                        it->second.state != UbJettyLifecycleState::ACTIVE,
                    "Cannot bind CTP service to a missing or draining Jetty");
    if (it->second.hasCtpBinding)
    {
        return;
    }
    NS_ABORT_MSG_IF(service == nullptr, "CTP Jetty binding requires the persistent service");
    if (m_ctpService == nullptr)
    {
        m_ctpService = service;
    }
    NS_ABORT_MSG_IF(m_ctpService != service,
                    "Jetty manager cannot bind multiple CTP services on one owner");
    it->second.hasCtpBinding = true;
}

bool
UbJettyManager::UnregisterCtpBinding(uint32_t jettyNum,
                                     Ptr<UbCtpTransportService> service)
{
    auto it = m_jettyRecordsByNum.find(jettyNum);
    if (it == m_jettyRecordsByNum.end() ||
        it->second.state != UbJettyLifecycleState::ACTIVE ||
        !it->second.hasCtpBinding || m_ctpService != service)
    {
        return false;
    }
    it->second.hasCtpBinding = false;
    return true;
}

void
UbJettyManager::RegisterWqe(uint32_t jettyNum,
                            uint32_t wqeId,
                            const UbLifecycleResourceIdentity& identity)
{
    auto it = m_jettyRecordsByNum.find(jettyNum);
    NS_ABORT_MSG_IF(it == m_jettyRecordsByNum.end() ||
                        it->second.state != UbJettyLifecycleState::ACTIVE,
                    "Cannot register WQE on a missing or draining Jetty");
    NS_ABORT_MSG_IF(!it->second.wqes.emplace(wqeId, identity).second,
                    "Duplicate manager-owned WQE record");
}

uint64_t
UbJettyManager::ResolveWqeLifecycleLocalId(uint32_t jettyNum, uint32_t wqeId) const
{
    const auto isInUse = [this](uint64_t localId) {
        for (const auto& [candidateJettyNum, record] : m_jettyRecordsByNum)
        {
            (void)candidateJettyNum;
            if (std::any_of(record.wqes.begin(),
                            record.wqes.end(),
                            [localId](const auto& entry) {
                                return entry.second.localId == localId;
                            }))
            {
                return true;
            }
        }
        return false;
    };

    if (!isInUse(wqeId))
    {
        return wqeId;
    }

    uint64_t scope = static_cast<uint64_t>(jettyNum) + 1;
    for (uint64_t attempts = 0; attempts < UINT32_MAX; ++attempts)
    {
        if (scope > UINT32_MAX)
        {
            scope = 1;
        }
        const uint64_t candidate = (scope << 32) | wqeId;
        if (!isInUse(candidate))
        {
            return candidate;
        }
        ++scope;
    }
    NS_ABORT_MSG("Exhausted scoped WQE lifecycle identities");
}

void
UbJettyManager::ApplyWqeOutcome(uint32_t jettyNum,
                                uint32_t wqeId,
                                const UbLifecycleResourceIdentity& identity,
                                UbWorkOutcome outcome)
{
    auto record = m_jettyRecordsByNum.find(jettyNum);
    NS_ABORT_MSG_IF(record == m_jettyRecordsByNum.end(),
                    "Transaction outcome references a stale Jetty generation");
    const auto wqe = record->second.wqes.find(wqeId);
    NS_ABORT_MSG_IF(wqe == record->second.wqes.end() || wqe->second != identity,
                    "Transaction outcome references an unknown, duplicate, or stale WQE");
    record->second.wqes.erase(wqe);
    Ptr<UbJetty> jetty = record->second.jetty;
    jetty->ApplyWqeTerminal(wqeId, outcome);
    TryFinalizeJettyDestroy(jettyNum);
}

bool
UbJettyManager::IsDrainReady(const JettyRecord& record) const
{
    return record.wqes.empty() && record.jetty->GetActiveWqeCount() == 0 &&
           !m_jettyEvents.HasEvents(record.identity) &&
           (!record.hasCtpBinding ||
            (m_coordinator->GetSnapshot(record.identity).activeTrackedEventLeases == 0 &&
             !m_ctpService->HasRequesterPackets(record.identity))) &&
           !GetTransaction()->HasLifecycleOperationForJetty(record.jetty->GetJettyNum());
}

bool
UbJettyManager::IsDrainReady(uint32_t jettyNum) const
{
    const auto it = m_jettyRecordsByNum.find(jettyNum);
    return it != m_jettyRecordsByNum.end() && IsDrainReady(it->second);
}

uint64_t
UbJettyManager::GetResidentWqeCount() const
{
    uint64_t count = 0;
    for (const auto& [jettyNum, record] : m_jettyRecordsByNum)
    {
        (void)jettyNum;
        count += record.wqes.size();
    }
    return count;
}

UbJettyStorageSnapshot
UbJettyManager::GetStorageSnapshot() const
{
    UbJettyStorageSnapshot snapshot;
    for (const auto& [jettyNum, record] : m_jettyRecordsByNum)
    {
        (void)jettyNum;
        if (record.lifetime == UbJettyLifetime::TASK_SCOPED)
        {
            ++snapshot.taskScopedJetties;
        }
        else
        {
            ++snapshot.reusableJetties;
        }
    }
    return snapshot;
}

uint64_t
UbJettyManager::TrackEvent(uint32_t jettyNum)
{
    auto it = m_jettyRecordsByNum.find(jettyNum);
    NS_ABORT_MSG_IF(it == m_jettyRecordsByNum.end(), "Cannot track work for a missing Jetty");
    return m_jettyEvents.Track(it->second.identity);
}

void
UbJettyManager::AttachEvent(uint32_t jettyNum,
                            uint64_t token,
                            EventId event,
                            Callback<void> cancelCleanup)
{
    auto it = m_jettyRecordsByNum.find(jettyNum);
    NS_ABORT_MSG_IF(it == m_jettyRecordsByNum.end(), "Cannot attach work for a missing Jetty");
    m_jettyEvents.Attach(it->second.identity, token, event, cancelCleanup);
}

bool
UbJettyManager::BeginEvent(uint32_t jettyNum, uint64_t token)
{
    auto it = m_jettyRecordsByNum.find(jettyNum);
    if (it == m_jettyRecordsByNum.end())
    {
        return false;
    }
    return m_jettyEvents.Begin(it->second.identity, token);
}

void
UbJettyManager::ResolveEvent(uint32_t jettyNum, uint64_t token)
{
    auto it = m_jettyRecordsByNum.find(jettyNum);
    NS_ABORT_MSG_IF(it == m_jettyRecordsByNum.end(), "Tracked event references a retired Jetty");
    m_jettyEvents.Resolve(it->second.identity, token);
    TryFinalizeJettyDestroy(jettyNum);
}

UbDestroyStatus
UbJettyManager::DestroyJetty(uint32_t jettyNum,
                             UbDestroyReason reason,
                             UbDestroyCallback callback)
{
    auto it = m_jettyRecordsByNum.find(jettyNum);
    if (it == m_jettyRecordsByNum.end())
    {
        return UbDestroyStatus::NOT_FOUND;
    }
    const UbLifecycleResourceIdentity identity = it->second.identity;
    const CleanupMode requestedMode = GetCleanupMode(reason);
    const auto findOriginalRecord = [this, jettyNum, identity]() {
        auto current = m_jettyRecordsByNum.find(jettyNum);
        if (current == m_jettyRecordsByNum.end() || current->second.identity != identity)
        {
            return m_jettyRecordsByNum.end();
        }
        return current;
    };
    if (it->second.state == UbJettyLifecycleState::ACTIVE)
    {
        it->second.state = UbJettyLifecycleState::DESTROY_REQUESTED;
        it->second.reason = reason;
        it->second.cleanupMode = requestedMode;
    }
    else if (static_cast<uint8_t>(requestedMode) >
             static_cast<uint8_t>(it->second.cleanupMode))
    {
        it->second.reason = reason;
        it->second.cleanupMode = requestedMode;
    }
    it->second.state = UbJettyLifecycleState::DRAINING;
    if (it->second.cleanupMode == CleanupMode::ABORT ||
        it->second.cleanupMode == CleanupMode::FORCE)
    {
        if (it->second.hasCtpBinding && m_ctpService != nullptr)
        {
            m_ctpService->CancelRequesterCompletionEvents(identity);
            m_ctpService->RemoveRequesterPackets(identity);
        }
        const UbWorkOutcome outcome = it->second.cleanupMode == CleanupMode::ABORT
                                          ? UbWorkOutcome::FAILED
                                          : UbWorkOutcome::FLUSHED;
        while (!it->second.wqes.empty())
        {
            const uint32_t wqeId = it->second.wqes.begin()->first;
            const UbTransaction::WqeTerminalMode terminalMode =
                it->second.cleanupMode == CleanupMode::FORCE
                    ? UbTransaction::WqeTerminalMode::SIMULATION_STOP
                    : UbTransaction::WqeTerminalMode::STOP_ACCEPTED;
            GetTransaction()->CompleteWqeOperationForDrain(
                jettyNum,
                wqeId,
                outcome,
                terminalMode);
            it = findOriginalRecord();
            if (it == m_jettyRecordsByNum.end())
            {
                return UbDestroyStatus::COMPLETED;
            }
        }
        if (it->second.cleanupMode == CleanupMode::FORCE)
        {
            GetTransaction()->DiscardJettyRetainedWqeSegmentsForSimulationStop(jettyNum);
            if (it->second.jetty->GetActiveWqeCount() != 0)
            {
                Ptr<UbJetty> keepAlive = it->second.jetty;
                keepAlive->FlushLegacyWqesForDisposal();
                it = findOriginalRecord();
                if (it == m_jettyRecordsByNum.end())
                {
                    return UbDestroyStatus::COMPLETED;
                }
            }
        }
    }
    if (it->second.cleanupMode == CleanupMode::FORCE)
    {
        std::vector<Callback<void>> cancelCleanups =
            m_jettyEvents.CancelPending(it->second.identity);
        for (const Callback<void>& cancelCleanup : cancelCleanups)
        {
            cancelCleanup();
        }
        it = findOriginalRecord();
        if (it == m_jettyRecordsByNum.end())
        {
            return UbDestroyStatus::COMPLETED;
        }
    }
    if (IsDrainReady(it->second))
    {
        FinalizeDestroy(jettyNum);
        return UbDestroyStatus::COMPLETED;
    }
    if (!callback.IsNull())
    {
        it->second.callbacks.push_back(callback);
    }
    return UbDestroyStatus::PENDING;
}

void
UbJettyManager::TryFinalizeJettyDestroy(uint32_t jettyNum)
{
    const auto it = m_jettyRecordsByNum.find(jettyNum);
    if (it != m_jettyRecordsByNum.end() && it->second.state == UbJettyLifecycleState::DRAINING &&
        IsDrainReady(it->second))
    {
        FinalizeDestroy(jettyNum);
    }
}

void
UbJettyManager::FinalizeDestroy(uint32_t jettyNum)
{
    auto it = m_jettyRecordsByNum.find(jettyNum);
    NS_ABORT_MSG_IF(it == m_jettyRecordsByNum.end() || !IsDrainReady(it->second),
                    "Jetty finalize requires a drained manager record");
    Ptr<UbJetty> keepAlive = it->second.jetty;
    const UbLifecycleResourceIdentity identity = it->second.identity;
    std::vector<UbDestroyCallback> callbacks = std::move(it->second.callbacks);
    GetTransaction()->DestroyJettyTpMap(jettyNum);
    if (m_ctpService != nullptr)
    {
        m_ctpService->ReleaseJetty(jettyNum);
    }
    GetTransaction()->RetireCtpRequester(jettyNum, identity);
    Ptr<UbLifecycleCoordinator> coordinator = m_coordinator;
    coordinator->RecordSuccessfulTerminal(identity);
    coordinator->RetireResource(identity);
    m_jettyRecordsByNum.erase(it);
    for (const UbDestroyCallback& callback : callbacks)
    {
        callback(identity, UbDestroyStatus::COMPLETED);
    }
}

void
UbJettyManager::DisposeAll()
{
    std::vector<uint32_t> jettyNums;
    jettyNums.reserve(m_jettyRecordsByNum.size());
    for (const auto& [jettyNum, record] : m_jettyRecordsByNum)
    {
        (void)record;
        jettyNums.push_back(jettyNum);
    }
    for (uint32_t jettyNum : jettyNums)
    {
        const UbDestroyStatus status =
            DestroyJetty(jettyNum, UbDestroyReason::SIMULATION_STOP, {});
        NS_ABORT_MSG_IF(status != UbDestroyStatus::COMPLETED,
                        "Forced Jetty manager disposal left active residue");
    }
    NS_ABORT_MSG_IF(!m_jettyRecordsByNum.empty(),
                    "Forced Jetty manager disposal did not erase all records");
    NS_ABORT_MSG_IF(!m_jettyEvents.IsEmpty(), "Forced Jetty manager disposal left tracked events");
    m_jettyEvents.Clear();
    m_ctpService = nullptr;
    m_transaction = nullptr;
    m_coordinator = nullptr;
}

void
UbJettyManager::DoDispose()
{
    NS_ABORT_MSG_IF(!m_jettyRecordsByNum.empty(),
                    "UbJettyManager disposal requires explicit forced record disposal");
    NS_ABORT_MSG_IF(!m_jettyEvents.IsEmpty(),
                    "UbJettyManager disposal requires resolved tracked events");
    m_jettyRecordsByNum.clear();
    m_ctpService = nullptr;
    m_transaction = nullptr;
    m_coordinator = nullptr;
    Object::DoDispose();
}

NS_OBJECT_ENSURE_REGISTERED(UbJetty);

TypeId UbJetty::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbJetty")
                        .SetParent<Object>()
                        .SetGroupName("UnifiedBus")
                        .AddAttribute("JettyOooAckThreshold",
                                      "Maximum out-of-order ACK window tracked per Jetty (TASSN bitmap size).",
                                      UintegerValue(2048),
                                      MakeUintegerAccessor(&UbJetty::SetOooAckThreshold,
                                          &UbJetty::GetOooAckThreshold),
                                      MakeUintegerChecker<uint32_t>())
                        .AddAttribute("UbJettyInflightMax",
                                      "Maximum number of in-flight WQE segments per Jetty before back-pressure.",
                                      UintegerValue(512),
                                      MakeUintegerAccessor(&UbJetty::SetInflightMax,
                                          &UbJetty::GetInflightMax),
                                      MakeUintegerChecker<uint32_t>())
                        .AddAttribute("TaSegmentBytes",
                                      "TA WQE segment size in bytes.",
                                      UintegerValue(UB_WQE_TA_SEGMENT_BYTE),
                                      MakeUintegerAccessor(&UbJetty::SetTaSegmentBytes,
                                          &UbJetty::GetTaSegmentBytes),
                                      MakeUintegerChecker<uint32_t>());
    return tid;
}

UbJetty::UbJetty()
{
}

void UbJetty::Init()
{
    ResetSsnAckBitset(m_oooAckThreshold);
}

void UbJetty::SetClientCallback(Callback<void, uint32_t, uint32_t> cb)
{
    FinishCallback = cb;
}

void UbJetty::SetCompletionObserver(Callback<void, uint32_t, uint32_t> cb)
{
    m_completionObserver = cb;
}

bool
UbJetty::CanAcceptWqe(Ptr<UbWqe> wqe) const
{
    if (wqe == nullptr)
    {
        return false;
    }
    if (wqe->GetSrc() != m_src)
    {
        return false;
    }
    if (HasBoundDest() && wqe->GetDest() != m_dest)
    {
        return false;
    }
    return true;
}

bool
UbJetty::IsCtpRequester() const
{
    return m_transaction != nullptr && m_transaction->GetCtpRequesterRcid(m_jettyNum).has_value();
}

bool
UbJetty::CanChangeCtpParameters() const
{
    return IsIdle() && m_taSsnCnt == m_taSsnSndNxt &&
           m_transaction->GetFunction()->IsJettyDrainReady(m_jettyNum);
}

bool
UbJetty::SetInflightMax(uint32_t value)
{
    if (value == m_inflightMax) return true;
    if (IsCtpRequester() && (value == 0 || value > 32768 ||
                            value > m_ssnAckWindow.GetWindowSize() || !CanChangeCtpParameters()))
        return false;
    m_inflightMax = value;
    return true;
}

uint32_t
UbJetty::GetOooAckThreshold() const
{
    return IsCtpRequester() ? m_ssnAckWindow.GetWindowSize() : m_oooAckThreshold;
}

bool
UbJetty::SetOooAckThreshold(uint32_t value)
{
    if (IsCtpRequester()) return ResetSsnAckBitset(value);
    m_oooAckThreshold = value;
    return true;
}

bool
UbJetty::ResetSsnAckBitset(uint32_t value)
{
    if (IsCtpRequester())
    {
        if (value == m_ssnAckWindow.GetWindowSize()) return true;
        if (value == 0 || value < m_inflightMax || !CanChangeCtpParameters()) return false;
    }
    m_ssnAckWindow.Resize(value);
    m_ssnAckWindow.Reset(m_taSsnSndUna);
    if (IsCtpRequester()) m_oooAckThreshold = value;
    return true;
}

bool
UbJetty::SetTaSegmentBytes(uint32_t bytes)
{
    if (bytes == m_taSegmentBytes) return true;
    if (IsCtpRequester())
    {
        if (bytes == 0 || bytes > UB_WQE_TA_SEGMENT_BYTE || !CanChangeCtpParameters()) return false;
    }
    else
    {
        NS_ABORT_MSG_IF(bytes == 0 || bytes > UB_WQE_TA_SEGMENT_BYTE,
                        "TA segment bytes must be in [1, UB_WQE_TA_SEGMENT_BYTE]");
    }
    m_taSegmentBytes = bytes;
    return true;
}

bool
UbJetty::CanReserveCtpWqe(Ptr<UbWqe> wqe, uint32_t maxSegmentBytes) const
{
    const uint32_t limit = std::min(m_taSegmentBytes, maxSegmentBytes);
    if (wqe == nullptr || limit == 0) return false;
    const uint64_t demand = IsAtomicTransaction(wqe->GetType())
                                ? 1 : (uint64_t{wqe->GetSize()} + limit - 1) / limit;
    return demand > 0 && uint64_t{m_taSsnCnt} + demand <= UINT32_MAX;
}

uint32_t UbJetty::GetTaSegmentBytes() const
{
    return m_taSegmentBytes;
}


// ============================================================================
// UbJetty 实现
// ============================================================================
UbTransaction* UbJetty::GetTransaction()
{
    NS_ABORT_MSG_IF(m_transaction == nullptr, "Jetty has no bound Transaction owner");
    return m_transaction;
}

Ptr<UbWqeSegment> UbJetty::GetNextWqeSegment()
{
    return GetNextWqeSegment(m_taSegmentBytes);
}

Ptr<UbWqeSegment> UbJetty::GetNextWqeSegment(uint32_t maxSegmentBytes)
{
    Ptr<UbWqeSegment> segment = PrepareNextWqeSegment(nullptr, maxSegmentBytes);
    if (segment != nullptr)
    {
        CommitWqeSegmentAdmission(segment);
    }
    return segment;
}

Ptr<UbWqeSegment> UbJetty::PrepareNextWqeSegment(Ptr<UbWqe> expectedWqe)
{
    return PrepareNextWqeSegment(expectedWqe, m_taSegmentBytes);
}

Ptr<UbWqeSegment> UbJetty::PrepareNextWqeSegment(Ptr<UbWqe> expectedWqe,
                                                 uint32_t maxSegmentBytes)
{
    NS_LOG_FUNCTION(this);
    NS_ABORT_MSG_IF(maxSegmentBytes == 0 || maxSegmentBytes > UB_WQE_TA_SEGMENT_BYTE,
                    "Transaction segment limit must be in [1, UB_WQE_TA_SEGMENT_BYTE]");

    if (IsLimited()) {
        NS_LOG_WARN("Inflight reach limit");
        return nullptr;
    }

    if (m_wqeVector.empty()) {
        NS_LOG_DEBUG("No WQE available to send");
        return nullptr;
    }

    Ptr<UbWqe> currentWqe = nullptr;
    if (expectedWqe != nullptr)
    {
        const auto expected = std::find(m_wqeVector.begin(), m_wqeVector.end(), expectedWqe);
        if (expected != m_wqeVector.end() && !expectedWqe->IsSentCompleted())
        {
            currentWqe = expectedWqe;
        }
    }
    else
    {
        // Legacy pull callers select the first unfinished eligible WQE.
        for (auto it = m_wqeVector.begin(); it != m_wqeVector.end(); ++it) {
            if (*it && !(*it)->IsSentCompleted()) {
                currentWqe = *it;
                // WQE初始发送状态为False，当TA根据事务序判断当前WQE可以发送后，修改为True，即可发送。
                if (currentWqe->CanSend()) {
                    break;
                } else {
                    // 调用TA，判断currentWqe是否允许发送
                    auto ubTa = GetTransaction();
                    if (ubTa->IsOrderedByInitiator(m_jettyNum, currentWqe)) {
                        break;
                    }
                }
            }
        }
    }

    if (!currentWqe) {
        NS_LOG_DEBUG("No unfinished WQE available to send");
        return nullptr;
    }

    if (!currentWqe->CanSend() &&
        !GetTransaction()->IsOrderedByInitiator(m_jettyNum, currentWqe)) {
        NS_LOG_DEBUG("No unfinished WQE available to send");
        return nullptr;
    }

    uint64_t segmentSize = currentWqe->GetBytesLeft();
    const uint32_t segmentLimit = std::min(m_taSegmentBytes, maxSegmentBytes);
    if (segmentSize > segmentLimit && !IsAtomicTransaction(currentWqe->GetType()))
    {
        segmentSize = segmentLimit;
    }

    Ptr<UbWqeSegment> segment = GenWqeSegment(currentWqe, segmentSize);
    segment->SetTaskId(currentWqe->GetWqeId());
    segment->SetWqeSize(currentWqe->GetSize());
    return segment;
}

bool
UbJetty::CanCommitWqeSegmentAdmission(Ptr<UbWqeSegment> segment,
                                      Ptr<UbWqe> expectedWqe) const
{
    if (segment == nullptr || expectedWqe == nullptr ||
        segment->GetTaSsn() != m_taSsnSndNxt ||
        segment->GetTaskId() != expectedWqe->GetWqeId() ||
        expectedWqe->GetBytesLeft() < segment->GetSize())
    {
        return false;
    }
    return std::find(m_wqeVector.begin(), m_wqeVector.end(), expectedWqe) != m_wqeVector.end();
}

void UbJetty::CommitWqeSegmentAdmission(Ptr<UbWqeSegment> segment)
{
    NS_ABORT_MSG_IF(segment == nullptr, "Cannot commit a null WQE segment");
    NS_ABORT_MSG_IF(segment->GetTaSsn() != m_taSsnSndNxt,
                    "WQE segment admission references a stale TASSN cursor");

    Ptr<UbWqe> wqe;
    for (const Ptr<UbWqe>& candidate : m_wqeVector)
    {
        if (candidate != nullptr && candidate->GetWqeId() == segment->GetTaskId())
        {
            wqe = candidate;
            break;
        }
    }
    NS_ABORT_MSG_IF(wqe == nullptr, "WQE segment admission references an unknown WQE");
    NS_ABORT_MSG_IF(wqe->GetBytesLeft() < segment->GetSize(),
                    "WQE segment admission would overrun the remaining WQE bytes");
    const bool firstAdmission = !wqe->CanSend();
    if (firstAdmission && wqe->GetTaSsnStart() != segment->GetTaSsn())
    {
        // Push-time TASSN is a reservation. A terminalized predecessor may leave a gap before
        // this WQE's first actual admission, so bind its request identity to the committed cursor.
        wqe->SetTaSsnStart(segment->GetTaSsn());
        wqe->SetRequestTassn(segment->GetTaSsn());
        segment->SetRequestTassn(segment->GetTaSsn());
    }
    else if (!firstAdmission)
    {
        NS_ABORT_MSG_IF(segment->GetRequestTassn() != wqe->GetRequestTassn(),
                        "WQE request identity changed after first segment admission");
    }
    wqe->SetCanSend(true);
    wqe->UpdateSentBytes(segment->GetSize());
    IncreaseTaSsnSndNxt();
}

void
UbJetty::AdvanceTaSsnForWqeCleanup(const std::vector<uint32_t>& taSsns)
{
    if (taSsns.empty())
    {
        return;
    }
    NS_ABORT_MSG_IF(m_ssnAckWindow.GetBase() != m_taSsnSndUna,
                    "Jetty terminal progress lost ACK-window alignment");
    for (uint32_t taSsn : taSsns)
    {
        NS_ABORT_MSG_IF(taSsn < m_taSsnSndUna,
                        "Jetty terminal progress would underflow the completion cursor");
        NS_ABORT_MSG_IF(taSsn >= m_taSsnSndNxt,
                        "Jetty terminal progress references an uncommitted TASSN");
        NS_ABORT_MSG_IF(taSsn - m_taSsnSndUna >= m_ssnAckWindow.GetWindowSize(),
                        "Jetty terminal progress exceeds the ACK window");
        NS_ABORT_MSG_IF(m_ssnAckWindow.Contains(taSsn),
                        "Jetty terminal progress duplicates an existing terminal mark");
    }
    for (uint32_t taSsn : taSsns)
    {
        NS_ABORT_MSG_IF(!m_ssnAckWindow.Mark(taSsn),
                        "Jetty terminal progress could not mark a validated TASSN");
    }
    m_taSsnSndUna += m_ssnAckWindow.AdvanceContiguous();
    NS_ABORT_MSG_IF(m_ssnAckWindow.GetBase() != m_taSsnSndUna,
                    "Jetty terminal progress advanced ACK-window state inconsistently");
    if (m_transaction != nullptr)
    {
        m_transaction->TryContinueJettyTransmission(m_jettyNum);
    }
}

Ptr<UbWqeSegment> UbJetty::GenWqeSegment(Ptr<UbWqe> wqe, uint32_t segmentSize)
{
    NS_LOG_FUNCTION(this << segmentSize);

    if (!wqe) {
        NS_LOG_ERROR("WQE is null");
        return nullptr;
    }

    if (segmentSize == 0) {
        NS_LOG_ERROR("Segment size is zero");
        return nullptr;
    }

    // 创建新的 WQE 分段
    Ptr<UbWqeSegment> segment = CreateObject<UbWqeSegment>();

    // 设置任务描述信息（从WQE复制）
    segment->SetSrc(wqe->GetSrc());
    segment->SetDest(wqe->GetDest());
    if (wqe->HasSrcEntityId())
    {
        segment->SetSrcEntityId(wqe->GetSrcEntityId());
    }
    if (wqe->HasDstEntityId())
    {
        segment->SetDstEntityId(wqe->GetDstEntityId());
    }
    segment->SetSport(wqe->GetSport());
    segment->SetDport(wqe->GetDport());
    segment->SetType(wqe->GetType());
    segment->SetSize(segmentSize);
    segment->SetPriority(wqe->GetPriority());
    // 设置TA层信息
    segment->SetOrderType(wqe->GetOrderType());
    // 设置网络层信息（从WQE复制）
    segment->SetSip(wqe->GetSip());
    segment->SetDip(wqe->GetDip());

    // 设置TA层信息
    segment->SetJettyNum(wqe->GetJettyNum());
    segment->SetTaMsn(wqe->GetTaMsn());
    segment->SetTaSsn(m_taSsnSndNxt); // 使用当前的分段序号
    segment->SetSegmentKind(wqe->GetSegmentKind());
    segment->SetOriginJettyNum(wqe->GetOriginJettyNum());
    segment->SetRequestTassn(wqe->GetRequestTassn());
    segment->SetRequestOpcode(wqe->GetRequestOpcode());
    segment->SetResponseBytes(wqe->GetResponseBytes());
    segment->SetRemoteAddress(wqe->GetRemoteAddress());
    if (wqe->GetType() == UbTransactionOpcode::WRITE_WITH_REMOTE_SYNC ||
        wqe->GetType() == UbTransactionOpcode::WRITE_WITH_ATOMIC_STORE_ADD ||
        wqe->GetType() == UbTransactionOpcode::WRITE_WITH_REMOTE_SYNC_WITH_BE)
    {
        const uint32_t offset = static_cast<uint32_t>(wqe->GetSize() - wqe->GetBytesLeft());
        segment->SetRemoteAddress(wqe->GetRemoteAddress() + offset);
    }
    segment->SetRemoteTokenId(wqe->GetRemoteTokenId());
    segment->SetNeedsTransactionResponse(wqe->NeedsTransactionResponse());
    segment->SetTransactionFields(wqe->GetTransactionFields());
    segment->SetResLenBytes(segmentSize);
    segment->SetPayloadBytes(wqe->GetType() == UbTransactionOpcode::READ ? 0 : segmentSize);
    segment->SetCarrierBytes(wqe->GetType() == UbTransactionOpcode::READ ? 1 : segmentSize);
    const bool atomic = IsAtomicTransaction(wqe->GetType());
    if (atomic)
    {
        segment->SetPayloadBytes(wqe->GetPayloadBytes());
        segment->SetCarrierBytes(wqe->GetPayloadBytes());
    }
    if (wqe->HasTransactionResolved())
    {
        auto resolved = wqe->GetTransactionResolved();
        if (!atomic)
        {
            resolved.logicalBytes = segmentSize;
            resolved.requestPayloadBytes = segment->GetPayloadBytes();
            resolved.responsePayloadBytes = std::min(resolved.responsePayloadBytes, segmentSize);
        }
        segment->SetTransactionResolved(resolved);
        segment->SetResponseBytes(resolved.responsePayloadBytes);
    }
    if (wqe->HasExplicitPayload())
    {
        Ptr<const Packet> payload = wqe->GetExplicitPayload();
        const uint32_t segmentOffset =
            static_cast<uint32_t>(wqe->GetSize() - wqe->GetBytesLeft());
        NS_ABORT_MSG_IF(payload->GetSize() != (atomic ? wqe->GetPayloadBytes() : wqe->GetSize()),
                        "Explicit WQE payload must match the WQE transfer size");
        if (atomic)
        {
            segment->SetExplicitPayload(payload);
        }
        else
        {
            segment->SetExplicitPayload(payload->CreateFragment(segmentOffset, segmentSize));
        }
    }

    // TP layer information will be set during subsequent TP layer scheduling

    NS_LOG_DEBUG("Generated WQE segment: MSN=" << std::to_string(wqe->GetTaMsn())
                                              << ", SSN=" << std::to_string(m_taSsnSndNxt)
                                              << ", size=" << std::to_string(segmentSize)
                                              << ", src=" << std::to_string(wqe->GetSrc())
                                              << ", dest=" << std::to_string(wqe->GetDest()));

    return segment;
}

void UbJetty::PushWqe(Ptr<UbWqe> ubWqe)
{
    PushWqe(ubWqe, m_taSegmentBytes);
}

void UbJetty::PushWqe(Ptr<UbWqe> ubWqe, uint32_t maxSegmentBytes)
{
    NS_ABORT_MSG_IF(!CanAcceptWqe(ubWqe),
                    "WQE is not compatible with Jetty source/destination binding");
    NS_ABORT_MSG_IF(maxSegmentBytes == 0 || maxSegmentBytes > UB_WQE_TA_SEGMENT_BYTE,
                    "WQE segment limit must be in [1, UB_WQE_TA_SEGMENT_BYTE]");
    NS_ABORT_MSG_IF(IsCtpRequester() && !CanReserveCtpWqe(ubWqe, maxSegmentBytes),
                    "CTP whole-WQE sequence reservation exhausted before PushWqe");
    ubWqe->SetJettyNum(m_jettyNum);
    ubWqe->SetTaMsn(m_taMsnCnt);
    ubWqe->SetTaSsnStart(m_taSsnCnt);
    const uint32_t segmentLimit = std::min(m_taSegmentBytes, maxSegmentBytes);
    const bool atomic = IsAtomicTransaction(ubWqe->GetType());
    const uint64_t ssnSize = atomic ? 1 : IsCtpRequester()
        ? (uint64_t{ubWqe->GetSize()} + segmentLimit - 1) / segmentLimit
        : (ubWqe->GetSize() + segmentLimit - 1) / segmentLimit;
    ubWqe->SetTaSsnSize(ssnSize);
    // Assign request identity only after Jetty binding is known.
    ubWqe->SetSegmentKind(UbTransactionSegmentKind::REQUEST);
    ubWqe->SetOriginJettyNum(m_jettyNum);
    ubWqe->SetRequestTassn(ubWqe->GetTaSsnStart());
    ubWqe->SetRequestOpcode(ubWqe->GetType());
    if (!atomic)
    {
        ubWqe->SetResponseBytes(ubWqe->GetType() == UbTransactionOpcode::READ ? ubWqe->GetSize()
                                                                             : 0);
        // The response requirement comes from the static rule and its cached normalized result; it
        // is never rewritten from the opcode here and never cleared when a response arrives.
        ubWqe->SetResLenBytes(ubWqe->GetSize());
        ubWqe->SetPayloadBytes(ubWqe->GetType() == UbTransactionOpcode::READ ? 0 : ubWqe->GetSize());
        ubWqe->SetCarrierBytes(ubWqe->GetType() == UbTransactionOpcode::READ ? ssnSize
                                                                            : ubWqe->GetSize());
    }

    // 更新 Jetty 的 MSN 和 SSN 计数器
    m_taMsnCnt++;
    m_taSsnCnt += ssnSize;

    // 加入队列
    m_wqeVector.push_back(ubWqe);
    NS_LOG_INFO("WQE Starts, jettyNum:{" << std::to_string(m_jettyNum)
                <<  "} taskId:{" << std::to_string(ubWqe->GetWqeId()) << "}");
}

bool UbJetty::IsValid()
{
    return true;
}

bool UbJetty::IsReadyToSend()
{
    return true;
}

bool
UbJetty::IsIdle() const
{
    return m_wqeVector.empty() && m_taSsnSndUna == m_taSsnSndNxt;
}

/*
 * TP调用这个方法，传入taSsnFin。
 */
bool UbJetty::ProcessWqeSegmentComplete(uint32_t taSsnAck)
{
    NS_LOG_DEBUG(this << taSsnAck);
    // 检查 SSN 是否在有效范围内
    if (taSsnAck < m_taSsnSndUna) {
        NS_LOG_WARN("Received ACK for already processed SSN " << std::to_string(taSsnAck)
                                                              << ", current m_taSsnSndUna is "
                                                              << std::to_string(m_taSsnSndUna));
        return false;
    }

    if (taSsnAck >= m_taSsnSndNxt) {
        NS_LOG_WARN("Received ACK for future SSN " << std::to_string(taSsnAck)
                                                   << ", current m_taSsnSndNxt is "
                                                   << std::to_string(m_taSsnSndNxt));
        return false;
    }

    // 计算在 bitset 中的相对位置
    uint32_t bitIndex = taSsnAck - m_taSsnSndUna;

    // 检查是否超出 bitset 范围
    if (bitIndex >= m_ssnAckWindow.GetWindowSize()) {
        NS_LOG_ERROR("SSN " << std::to_string(taSsnAck)
                            << " exceeds bitset capacity, bitIndex=" << std::to_string(bitIndex));
        return false;
    }

    // 设置对应的 bit
    const bool marked = m_ssnAckWindow.Mark(taSsnAck);
    NS_ASSERT_MSG(marked, "Valid SSN ACK should always fit inside ACK window");

    NS_LOG_DEBUG("Set ACK bit for SSN " << std::to_string(taSsnAck) << " at bit index "
                                       << std::to_string(bitIndex));

    // 从最小未确认的 SSN 开始，连续更新已确认的分段
    uint32_t oldSndUna = m_taSsnSndUna;
    while (m_taSsnSndUna < m_taSsnSndNxt) {
        uint32_t currentBitIndex = m_taSsnSndUna - oldSndUna;
        if (currentBitIndex < m_ssnAckWindow.GetWindowSize() &&
            m_ssnAckWindow.Contains(m_taSsnSndUna)) {
            m_taSsnSndUna++;
        } else {
            break; // 遇到未确认的分段，停止
        }
    }

    // 如果 m_taSsnSndUna 有更新，需要清理 bitset
    if (m_taSsnSndUna > oldSndUna) {
        NS_LOG_DEBUG("Updated m_taSsnSndUna from " << std::to_string(oldSndUna) << " to "
                                                  << std::to_string(m_taSsnSndUna));

        // 手动右移 bitset
        uint32_t shiftCount = m_taSsnSndUna - oldSndUna;
        RightShiftBitset(shiftCount);

        // 检查并移除已完成的 WQE
        CheckAndRemoveCompletedWqe();
    }
    if (m_transaction != nullptr)
    {
        m_transaction->TryContinueJettyTransmission(m_jettyNum);
    }
    return true;
}

void UbJetty::RightShiftBitset(uint32_t shiftCount)
{
    (void)shiftCount;
    m_ssnAckWindow.AdvanceContiguous();
}

void UbJetty::CheckAndRemoveCompletedWqe()
{
    NS_LOG_DEBUG(this);
    // 检查并移除已完成的WQE
    for (auto it = m_wqeVector.begin(); it != m_wqeVector.end();) {
        Ptr<UbWqe> wqe = *it;
        uint32_t wqeId = wqe->GetWqeId();
        if (wqe && IsWqeCompleted(wqe)) {
            NS_LOG_INFO("WQE Finishes, jettyNum: {" << m_jettyNum  << "} taskId:{ " << std::to_string(wqeId) <<"}");
            auto ubTa = GetTransaction();
            if (ubTa->HasWqeOperation(m_jettyNum, wqeId)) {
                ubTa->WqeFinish(m_jettyNum, *it, UbWorkOutcome::COMPLETED);
                it = m_wqeVector.begin();
                continue;
            }
            ubTa->WqeFinish(m_jettyNum, *it, UbWorkOutcome::COMPLETED);
            // 从vector中移除已完成的WQE
            it = m_wqeVector.erase(it);
            if (!m_wqeCompletionCallback.IsNull()) {
                m_wqeCompletionCallback({m_jettyNum, wqeId, UbWorkOutcome::COMPLETED});
            }
            if (!m_completionObserver.IsNull()) {
                m_completionObserver(wqeId, m_jettyNum);
            }
            if (!FinishCallback.IsNull()) {
                FinishCallback(wqeId, m_jettyNum); // 调用应用层的回调
            }
            NodeList::GetNode(m_nodeId)
                ->GetObject<UbController>()
                ->GetUbFunction()
                ->NotifyJettyProgress(m_jettyNum);
        } else {
            ++it;
        }
    }

    // 检查Jetty是否还有未完成的工作
    if (m_wqeVector.empty()) {
        NS_LOG_DEBUG("All WQEs in Jetty " << std::to_string(m_jettyNum) << " are completed");
    }
}

void
UbJetty::SetWqeCompletionCallback(Callback<void, UbWqeCompletion> cb)
{
    m_wqeCompletionCallback = cb;
}

void
UbJetty::ApplyWqeTerminal(uint32_t wqeId, UbWorkOutcome outcome)
{
    const auto it = std::find_if(m_wqeVector.begin(),
                                 m_wqeVector.end(),
                                 [wqeId](const Ptr<UbWqe>& wqe) {
                                     return wqe != nullptr && wqe->GetWqeId() == wqeId;
                                 });
    NS_ABORT_MSG_IF(it == m_wqeVector.end(),
                    "Transaction outcome references an unknown or terminal WQE");
    Ptr<UbJetty> keepAlive(this);
    m_wqeVector.erase(it);
    if (m_transaction != nullptr)
    {
        m_transaction->TryContinueJettyTransmission(m_jettyNum);
    }
    if (!m_wqeCompletionCallback.IsNull())
    {
        m_wqeCompletionCallback({m_jettyNum, wqeId, outcome});
    }
    if (outcome == UbWorkOutcome::COMPLETED && !m_completionObserver.IsNull())
    {
        m_completionObserver(wqeId, m_jettyNum);
    }
    if (outcome == UbWorkOutcome::COMPLETED && !FinishCallback.IsNull())
    {
        FinishCallback(wqeId, m_jettyNum);
    }
}

void
UbJetty::FlushLegacyWqesForDisposal()
{
    Ptr<UbJetty> keepAlive(this);
    while (!m_wqeVector.empty())
    {
        const auto current = std::find_if(m_wqeVector.begin(),
                                          m_wqeVector.end(),
                                          [](const Ptr<UbWqe>& wqe) { return wqe != nullptr; });
        if (current == m_wqeVector.end())
        {
            m_wqeVector.clear();
            break;
        }
        Ptr<UbWqe> wqe = *current;
        if (wqe == nullptr)
        {
            m_wqeVector.erase(current);
            continue;
        }
        GetTransaction()->WqeFinish(m_jettyNum, wqe, UbWorkOutcome::FLUSHED);
        const auto it = std::find(m_wqeVector.begin(), m_wqeVector.end(), wqe);
        if (it != m_wqeVector.end())
        {
            m_wqeVector.erase(it);
        }
        if (!m_wqeCompletionCallback.IsNull())
        {
            m_wqeCompletionCallback({m_jettyNum, wqe->GetWqeId(), UbWorkOutcome::FLUSHED});
        }
    }
}

bool UbJetty::IsWqeCompleted(Ptr<UbWqe> wqe)
{
    NS_LOG_DEBUG(this);
    if (!wqe) {
        return false;
    }

    // 检查WQE的所有分段是否都已确认
    uint32_t wqeStartSsn = wqe->GetTaSsnStart();
    uint32_t wqeSsnSize = wqe->GetTaSsnSize();
    uint32_t wqeEndSsn = wqeStartSsn + wqeSsnSize - 1;

    // 如果WQE的最后一个分段已经被确认，则WQE完成
    return (wqeEndSsn < m_taSsnSndUna);
}

void UbJetty::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_wqeVector.clear();
    m_transaction = nullptr;
    m_ssnAckWindow.Resize(0);
    Object::DoDispose();
}


} // namespace ns3
