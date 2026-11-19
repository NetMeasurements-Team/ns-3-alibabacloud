// SPDX-License-Identifier: GPL-2.0-only
#include "ub-transaction-rules.h"
#include <algorithm>
#include <array>
#include <optional>
namespace ns3 {
namespace {
using K=UbTransactionHeaderKind; using O=UbTransactionOpcode; using T=UbTransactionType; using S=UbTransactionSegmentKind;
constexpr K W[]={K::BTAH,K::UDETAH,K::MAETAH};
constexpr K WB[]={K::BTAH,K::UDETAH,K::MAETAH,K::BEETAH};
constexpr K N[]={K::BTAH,K::UDETAH,K::MAETAH,K::NTFETAH};
constexpr K R[]={K::BTAH,K::UDETAH,K::MAETAH};
constexpr K A[]={K::BTAH,K::UDETAH,K::MAETAH};
constexpr K QUEUE[]={K::BTAH,K::MAETAH};
constexpr K RS[]={K::BTAH,K::UDETAH,K::MAETAH};
constexpr K RSB[]={K::BTAH,K::UDETAH,K::MAETAH,K::BEETAH};
constexpr K AS[]={K::BTAH,K::UDETAH,K::MAETAH,K::TAIDETAH};
constexpr K ASR[]={K::ATAH,K::TAIDETAH};
constexpr K SND[]={K::BTAH,K::UDETAH,K::MTETAH};
constexpr K SIM[]={K::BTAH,K::UDETAH,K::MTETAH,K::IMMETAH};
constexpr K WI[]={K::BTAH,K::UDETAH,K::MTETAH,K::MAETAH,K::IMMETAH};
constexpr K P[]={K::BTAH,K::UDETAH,K::MAETAH};
constexpr K M[]={K::BTAH,K::UDETAH,K::MGMTETAH};
constexpr K ACK[]={K::ATAH}; constexpr K RR[]={K::ATAH}; constexpr K AR[]={K::ATAH};
constexpr bool AtomicOpcode(O o)
{
    return o >= O::ATOMIC_COMPARE_SWAP && o <= O::ATOMIC_FETCH_XOR;
}
constexpr UbTransactionOpcode ResponseFor(O o)
{
    return o == O::READ ? O::READ_RESPONSE
                        : (AtomicOpcode(o) || o == O::ATOMIC_SYNC ? O::ATOMIC_RESPONSE
                                                                    : O::TAACK);
}
constexpr const K* ResponseHeadersFor(O o)
{
    return o == O::ATOMIC_SYNC ? ASR : (o == O::READ ? RR : (AtomicOpcode(o) ? AR : ACK));
}
constexpr UbTransactionRule Q(O o,
                              T t,
                              const K* h,
                              uint8_t n,
                              bool payloadRequired = false,
                              bool logicalRequired = true,
                              uint32_t max = UINT32_MAX,
                              bool requiresResponse = true)
{
    return {o,
            t,
            S::REQUEST,
            h,
            n,
            payloadRequired ? 1u : 0u,
            max,
            payloadRequired,
            logicalRequired,
            ResponseFor(o),
            ResponseHeadersFor(o),
            1,
            0,
            o == O::READ ? UINT32_MAX : (AtomicOpcode(o) ? UB_MTU_BYTE : 0),
            requiresResponse};
}
constexpr UbTransactionRule Resp(O o, T t, uint32_t min, uint32_t max)
{
    return {o, t, S::RESPONSE, ACK, 1, min, max, min != 0, false, o, nullptr, 0, 0, 0, false};
}
constexpr std::array<UbTransactionRule,28> RULES={{
 Q(O::SEND,T::MESSAGE,SND,std::size(SND),true),Q(O::SEND_WITH_IMMEDIATE,T::MESSAGE,SIM,std::size(SIM),true),
 Q(O::WRITE,T::MEMORY,W,std::size(W),true),Q(O::WRITE_WITH_IMMEDIATE,T::MESSAGE,WI,std::size(WI),true),
 Q(O::WRITE_WITH_NOTIFY,T::MEMORY,N,std::size(N),true),Q(O::READ,T::MEMORY,R,std::size(R)),
 Q(O::ATOMIC_COMPARE_SWAP,T::MEMORY,A,std::size(A),true,true,UB_MTU_BYTE),Q(O::ATOMIC_SWAP,T::MEMORY,A,std::size(A),true,true,UB_MTU_BYTE),
 Q(O::ATOMIC_STORE,T::MEMORY,A,std::size(A),true,true,UB_MTU_BYTE),Q(O::ATOMIC_LOAD,T::MEMORY,A,std::size(A),false,true,UB_MTU_BYTE),
 Q(O::ATOMIC_FETCH_ADD,T::MEMORY,A,std::size(A),true,true,UB_MTU_BYTE),Q(O::ATOMIC_FETCH_SUB,T::MEMORY,A,std::size(A),true,true,UB_MTU_BYTE),
 Q(O::ATOMIC_FETCH_AND,T::MEMORY,A,std::size(A),true,true,UB_MTU_BYTE),Q(O::ATOMIC_FETCH_OR,T::MEMORY,A,std::size(A),true,true,UB_MTU_BYTE),
 Q(O::ATOMIC_FETCH_XOR,T::MEMORY,A,std::size(A),true,true,UB_MTU_BYTE),Resp(O::TAACK,T::MEMORY,0,0),
 {O::READ_RESPONSE,T::MEMORY,S::RESPONSE,RR,std::size(RR),1,UINT32_MAX,true,false},{O::ATOMIC_RESPONSE,T::MEMORY,S::RESPONSE,AR,std::size(AR),1,UB_MTU_BYTE,true,false},
 Q(O::PREFETCH_TGT,T::MAINTENANCE,P,std::size(P)),
 Q(O::WRITEBACK,T::MEMORY,W,std::size(W),true),
 Q(O::MANAGEMENT,T::MANAGEMENT,M,std::size(M),false,false,1024),
 Q(O::WRITE_WITH_BE,T::MEMORY,WB,std::size(WB),true,true,1024),
 Q(O::WRITEBACK_WITH_BE,T::MEMORY,WB,std::size(WB),true,true,1024),
 Q(O::WRITE_WITH_ATOMIC_STORE_ADD,T::MEMORY,N,std::size(N),true),
 Q(O::WRITE_WITH_REMOTE_SYNC,T::MEMORY,RS,std::size(RS),true),
 Q(O::WRITE_WITH_REMOTE_SYNC_WITH_BE,T::MEMORY,RSB,std::size(RSB),true,true,1024),
 Q(O::ATOMIC_STORE_WITH_RETURN_STATUS,T::MEMORY,QUEUE,std::size(QUEUE),true,true,UB_MTU_BYTE),
 {O::ATOMIC_SYNC,T::MEMORY,S::REQUEST,AS,std::size(AS),1,UB_MTU_BYTE,true,true,O::ATOMIC_RESPONSE,ASR,std::size(ASR),0,UB_MTU_BYTE,true}}};
const UbTransactionRule* Lookup(O o){for(const auto&r:RULES)if(r.opcode==o)return &r;return nullptr;}

/**
 * Fill the required extension-header fields a local submission may omit.
 *
 * Fixed default fields are zero; length fields follow the transaction payload length. Inbound
 * requests never reach this function, so a missing required header stays missing and is rejected.
 */
void ApplyRequiredFieldDefaults(O opcode, UbTransactionFields& fields, uint32_t payloadBytes)
{
    switch (opcode)
    {
    case O::SEND:
        if (!fields.messageTarget)
            fields.messageTarget = UbMessageTargetFields{};
        break;
    case O::SEND_WITH_IMMEDIATE:
    case O::WRITE_WITH_IMMEDIATE:
        if (!fields.messageTarget)
            fields.messageTarget = UbMessageTargetFields{};
        if (!fields.immediate)
        {
            fields.immediate = UbImmediateFields{};
            fields.immediate->messageLength = payloadBytes;
        }
        break;
    case O::WRITE_WITH_NOTIFY:
        if (!fields.notify)
            fields.notify = UbNotifyFields{};
        break;
    case O::WRITE_WITH_ATOMIC_STORE_ADD:
        if (!fields.notify) fields.notify = UbNotifyFields{};
        break;
    case O::WRITE_WITH_REMOTE_SYNC:
    case O::WRITE_WITH_REMOTE_SYNC_WITH_BE:
        if (!fields.userData) fields.userData = UbUserDataFields{2, 0, 0, 0};
        if (opcode == O::WRITE_WITH_REMOTE_SYNC_WITH_BE && !fields.byteEnable)
            fields.byteEnable = UbByteEnableFields{std::vector<uint8_t>(payloadBytes / 8, 0xff)};
        break;
    case O::WRITE_WITH_BE:
    case O::WRITEBACK_WITH_BE:
        if (!fields.byteEnable)
            fields.byteEnable = UbByteEnableFields{std::vector<uint8_t>(payloadBytes / 8, 0xff)};
        break;
    case O::ATOMIC_SYNC:
        if (!fields.taskId) fields.taskId = 0;
        break;
    case O::MANAGEMENT:
        if (!fields.management)
        {
            fields.management = UbManagementFields{};
            fields.management->pduLength = static_cast<uint16_t>(payloadBytes);
        }
        break;
    default:
        break;
    }
}

/** Validate the payload and logical-length bounds declared by the rule. */
bool CheckRuleLengths(const UbTransactionRule& rule, uint32_t logicalBytes, uint32_t payloadBytes,
                      uint32_t maxPayloadBytes, UbTransactionValidationResult& result)
{
    const uint32_t limit = std::min(rule.maxPayload, maxPayloadBytes);
    if (payloadBytes < rule.minPayload || payloadBytes > limit ||
        (rule.payloadRequired && payloadBytes == 0) ||
        (rule.opcode == O::READ && payloadBytes != 0))
    {
        result.code = UbTransactionErrorCode::INVALID_LENGTH;
        result.message = "payload length outside transaction rule";
        return false;
    }
    if (rule.logicalLengthRequired && logicalBytes == 0)
    {
        result.code = UbTransactionErrorCode::INVALID_LENGTH;
        result.message = "logical transaction length must be positive";
        return false;
    }
    const auto op = rule.opcode;
    if (op == O::ATOMIC_STORE_WITH_RETURN_STATUS || op == O::ATOMIC_SYNC)
    {
        if (logicalBytes == 0 || logicalBytes > 64 || (logicalBytes & (logicalBytes - 1)) != 0 ||
            payloadBytes != logicalBytes)
        {
            result.code = UbTransactionErrorCode::INVALID_LENGTH;
            result.message = "queue transaction width must be a power of two from 1 through 64 and P=N";
            return false;
        }
    }
    if (op == O::WRITE_WITH_BE || op == O::WRITEBACK_WITH_BE ||
        op == O::WRITE_WITH_REMOTE_SYNC_WITH_BE)
    {
        if (payloadBytes != logicalBytes ||
            !(logicalBytes == 64 || logicalBytes == 128 || logicalBytes == 256 ||
              logicalBytes == 512 || logicalBytes == 1024))
        {
            result.code = UbTransactionErrorCode::INVALID_LENGTH;
            result.message = "byte-enable writes require 64/128/256/512/1024 bytes in one packet";
            return false;
        }
    }
    return true;
}

/** Check the presence of the transaction fields the opcode requires on the wire. */
bool CheckRequiredFields(UbTransactionOpcode opcode, const UbTransactionFields& fields,
                         uint32_t payloadBytes, UbTransactionValidationResult& result)
{
    std::string missing;
    const auto requireMemoryAccess = [&]() {
        if (!fields.memoryAccess)
            missing = "memoryAccess";
    };
    switch (opcode)
    {
    case O::SEND:
        if (!fields.messageTarget)
            missing = "messageTarget";
        break;
    case O::SEND_WITH_IMMEDIATE:
    case O::WRITE_WITH_IMMEDIATE:
        if (!fields.messageTarget)
            missing = "messageTarget";
        else if (!fields.immediate)
            missing = "immediate";
        break;
    case O::WRITE_WITH_NOTIFY:
        requireMemoryAccess();
        if (missing.empty() && !fields.notify)
            missing = "notify";
        break;
    case O::WRITE_WITH_ATOMIC_STORE_ADD:
        requireMemoryAccess();
        if (missing.empty() && !fields.notify) missing = "notify";
        break;
    case O::WRITE_WITH_REMOTE_SYNC:
        requireMemoryAccess();
        if (missing.empty() && !fields.userData) missing = "userData";
        break;
    case O::WRITE_WITH_REMOTE_SYNC_WITH_BE:
        requireMemoryAccess();
        if (missing.empty() && !fields.userData) missing = "userData";
        if (missing.empty() && !fields.byteEnable) missing = "byteEnable";
        break;
    case O::ATOMIC_STORE_WITH_RETURN_STATUS:
        requireMemoryAccess();
        break;
    case O::ATOMIC_SYNC:
        requireMemoryAccess();
        if (missing.empty() && !fields.taskId) missing = "taskId";
        break;
    case O::WRITE_WITH_BE:
    case O::WRITEBACK_WITH_BE:
        requireMemoryAccess();
        if (missing.empty() && !fields.byteEnable) missing = "byteEnable";
        break;
    case O::MANAGEMENT:
        if (!fields.management)
        {
            missing = "management";
        }
        else if (fields.management->pduLength > 1024 ||
                 fields.management->pduLength != payloadBytes)
        {
            result.code = UbTransactionErrorCode::INVALID_LENGTH;
            result.field = "management.pduLength";
            result.message = "management PDU length must match payload";
            return false;
        }
        break;
    case O::WRITE:
    case O::READ:
    case O::WRITEBACK:
    case O::PREFETCH_TGT:
    case O::ATOMIC_COMPARE_SWAP:
    case O::ATOMIC_SWAP:
    case O::ATOMIC_STORE:
    case O::ATOMIC_LOAD:
    case O::ATOMIC_FETCH_ADD:
    case O::ATOMIC_FETCH_SUB:
    case O::ATOMIC_FETCH_AND:
    case O::ATOMIC_FETCH_OR:
    case O::ATOMIC_FETCH_XOR:
        requireMemoryAccess();
        break;
    default:
        break;
    }
    if (!missing.empty())
    {
        result.code = UbTransactionErrorCode::MISSING_FIELD;
        result.field = missing;
        result.message = "required transaction field is missing";
        return false;
    }
    return true;
}

/**
 * Resolve the Atomic request payload.
 *
 * A local submission resolves the request payload from its local options; omitting them applies the
 * opcode defaults, so a Compare/Swap still carries 2N and a Load/Store still has to name its
 * sub-operation. An inbound request takes the payload that the wire carried, because the
 * Load/Store sub-operation is not encoded.
 */
bool ResolveAtomicPayloads(UbTransactionOpcode opcode, uint32_t logicalBytes, uint32_t payloadBytes,
                           const std::optional<UbAtomicOptions>& options,
                           UbTransactionRequestOrigin origin, uint32_t maxPayloadBytes,
                           UbTransactionValidationResult& result, uint32_t& requestPayloadBytes)
{
    if (logicalBytes == 0 || logicalBytes > 64 || (logicalBytes & (logicalBytes - 1)) != 0)
    {
        result.code = UbTransactionErrorCode::INVALID_LENGTH;
        result.field = "logicalBytes";
        result.message = "operand width must be a power of two from 1 through 64";
        return false;
    }
    const bool loadStore = opcode == O::ATOMIC_LOAD || opcode == O::ATOMIC_STORE;
    std::optional<UbAtomicOptions> resolvedOptions = options;
    if (!resolvedOptions && origin == UbTransactionRequestOrigin::LOCAL_SUBMISSION)
    {
        resolvedOptions = UbAtomicOptions{};
    }
    uint32_t payload = payloadBytes;
    if (resolvedOptions)
    {
        if (loadStore != resolvedOptions->subOperation.has_value())
        {
            result.code = UbTransactionErrorCode::INVALID_ATOMIC_OPTIONS;
            result.field = "atomicOptions.subOperation";
            result.message = "subOperation is required only for Load/Store";
            return false;
        }
        if (resolvedOptions->subOperation &&
            *resolvedOptions->subOperation > UbAtomicSubOperation::UMIN)
        {
            result.code = UbTransactionErrorCode::INVALID_ATOMIC_OPTIONS;
            result.field = "atomicOptions.subOperation";
            result.message = "invalid atomic suboperation";
            return false;
        }
        payload = resolvedOptions->requestPayloadBytes.value_or(opcode == O::ATOMIC_COMPARE_SWAP
                                                                    ? logicalBytes * 2
                                                                    : logicalBytes);
    }
    if (payload == 0 || payload > maxPayloadBytes)
    {
        result.code = UbTransactionErrorCode::INVALID_LENGTH;
        result.field = "payloadBytes";
        result.message = "atomic payload exceeds transport limit";
        return false;
    }
    requestPayloadBytes = payload;
    return true;
}
}
const UbTransactionRule* FindTransactionRule(UbTransactionOpcode o){return Lookup(o);}
UbTransactionValidationResult ValidateTransactionRequest(const UbTransactionRequestInput& request)
{
    UbTransactionValidationResult result;
    result.opcode = request.opcode;
    result.fields = request.fields;
    const auto* rule = Lookup(request.opcode);
    if (!rule)
    {
        result.code = UbTransactionErrorCode::UNSUPPORTED_OPCODE;
        result.message = "unsupported or reserved transaction opcode";
        return result;
    }
    if (rule->role != S::REQUEST)
    {
        result.code = UbTransactionErrorCode::WRONG_ROLE;
        result.message = "response opcode cannot be submitted as request";
        return result;
    }
    if (request.atomicOptions && !AtomicOpcode(request.opcode))
    {
        result.code = UbTransactionErrorCode::INVALID_ATOMIC_OPTIONS;
        result.field = "atomicOptions";
        result.message = "atomic options are forbidden for non-atomic requests";
        return result;
    }
    if (!CheckRuleLengths(*rule, request.logicalBytes, request.payloadBytes,
                          request.maxPayloadBytes, result))
    {
        return result;
    }
    if (request.origin == UbTransactionRequestOrigin::LOCAL_SUBMISSION)
    {
        ApplyRequiredFieldDefaults(request.opcode, result.fields, request.payloadBytes);
    }
    if (!CheckRequiredFields(request.opcode, result.fields, request.payloadBytes, result))
    {
        return result;
    }
    if (result.fields.memoryAccess &&
        (request.opcode == O::ATOMIC_STORE_WITH_RETURN_STATUS || request.opcode == O::ATOMIC_SYNC) &&
        (result.fields.memoryAccess->address % request.logicalBytes != 0))
    {
        result.code = UbTransactionErrorCode::INVALID_LENGTH;
        result.field = "memoryAccess.address";
        result.message = "queue transaction address must be aligned to N";
        return result;
    }
    if ((request.opcode == O::WRITE_WITH_BE || request.opcode == O::WRITEBACK_WITH_BE ||
         request.opcode == O::WRITE_WITH_REMOTE_SYNC_WITH_BE) && result.fields.byteEnable &&
        result.fields.byteEnable->bytes.size() != request.payloadBytes / 8)
    {
        result.code = UbTransactionErrorCode::INVALID_LENGTH;
        result.field = "byteEnable";
        result.message = "byte-enable bitmap length must equal payloadBytes/8";
        return result;
    }
    if (request.opcode == O::WRITE_WITH_REMOTE_SYNC ||
        request.opcode == O::WRITE_WITH_REMOTE_SYNC_WITH_BE)
    {
        const auto& ud = *result.fields.userData;
        if (ud.type != 2 || ud.opcode != 0 || ud.offset > 7)
        {
            result.code = UbTransactionErrorCode::INVALID_LENGTH;
            result.field = "userData";
            result.message = "remote-sync UDETAH requires Type=2, UD_Opcode=0 and Offset 0..7";
            return result;
        }
    }
    uint32_t requestPayloadBytes = request.payloadBytes;
    uint32_t responsePayloadBytes = request.opcode == O::READ ? request.logicalBytes : 0;
    if (AtomicOpcode(request.opcode))
    {
        if (!ResolveAtomicPayloads(request.opcode, request.logicalBytes, request.payloadBytes,
                                   request.atomicOptions, request.origin, request.maxPayloadBytes,
                                   result, requestPayloadBytes))
        {
            return result;
        }
        responsePayloadBytes = (request.opcode == O::ATOMIC_STORE ||
                                request.opcode == O::ATOMIC_STORE_WITH_RETURN_STATUS)
                                   ? 0
                                   : request.logicalBytes;
    }
    else if (request.opcode == O::ATOMIC_SYNC)
    {
        responsePayloadBytes = request.logicalBytes;
    }
    result.resolved.type = rule->type;
    result.resolved.requestOpcode = request.opcode;
    result.resolved.logicalBytes = request.logicalBytes;
    result.resolved.requestPayloadBytes = requestPayloadBytes;
    result.resolved.responseOpcode = rule->responseOpcode;
    result.resolved.responsePayloadBytes = responsePayloadBytes;
    result.resolved.requiresResponse = rule->requiresResponse;
    return result;
}

bool IsAtomicTransaction(UbTransactionOpcode opcode)
{
    return opcode >= O::ATOMIC_COMPARE_SWAP && opcode <= O::ATOMIC_FETCH_XOR;
}

bool IsAcceptedResponsePayloadBytes(UbTransactionOpcode requestOpcode,
                                    uint32_t expectedPayloadBytes,
                                    uint32_t actualPayloadBytes)
{
    if (requestOpcode == UbTransactionOpcode::ATOMIC_SYNC)
    {
        return actualPayloadBytes == 0 || actualPayloadBytes == expectedPayloadBytes;
    }
    return actualPayloadBytes == expectedPayloadBytes;
}

const char* ToString(UbTransactionType t){switch(t){case T::MEMORY:return "Memory";case T::MESSAGE:return "Message";case T::MAINTENANCE:return "Maintenance";case T::MANAGEMENT:return "Management";}return "Unknown";}
}
