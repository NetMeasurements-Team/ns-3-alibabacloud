// SPDX-License-Identifier: GPL-2.0-only
#include "ub-ta-codec.h"

#include "ns3/abort.h"

namespace ns3
{

namespace
{
using K = UbTransactionHeaderKind;

bool LayoutContains(const K* kinds, uint8_t count, K kind)
{
    for (uint8_t index = 0; index < count; ++index)
    {
        if (kinds[index] == kind)
        {
            return true;
        }
    }
    return false;
}
} // namespace

UbTaHeaderPlan
UbTaHeaderPlan::Build(const K* kinds, uint8_t count, UbTaHeaderPresence presence)
{
    UbTaHeaderPlan plan;
    plan.m_presence = presence;
    for (uint8_t index = 0; index < count; ++index)
    {
        const K kind = kinds[index];
        // The basic header is written by the carrier in its own format.
        if (kind == K::BTAH || kind == K::ATAH)
        {
            continue;
        }
        // Flag-driven headers exist on the wire only when their flag is set.
        if (kind == K::TVETAH && !presence.tokenValue)
        {
            continue;
        }
        if (kind == K::MTETAH && !presence.messageTarget)
        {
            continue;
        }
        if (kind == K::UDETAH && !presence.userData)
        {
            continue;
        }
        NS_ABORT_MSG_IF(plan.m_count == plan.m_kinds.size(),
                        "transaction rule layout exceeds the planned header capacity");
        plan.m_kinds[plan.m_count] = kind;
        ++plan.m_count;
    }
    return plan;
}

UbTaHeaderPlan
UbTaHeaderPlan::ForRequest(const UbTransactionRule& rule, UbTaHeaderPresence presence)
{
    return Build(rule.headers, rule.headerCount, presence);
}

UbTaHeaderPlan
UbTaHeaderPlan::ForResponse(const UbTransactionRule& rule)
{
    return Build(rule.responseHeaders, rule.responseHeaderCount, UbTaHeaderPresence{});
}

bool
UbTaHeaderPlan::Contains(K kind) const
{
    return LayoutContains(m_kinds.data(), m_count, kind);
}

uint32_t
UbTaHeaderPlan::GetExtensionBytes(uint32_t maetahBytes) const
{
    uint32_t bytes = 0;
    for (uint8_t index = 0; index < m_count; ++index)
    {
        if (m_kinds[index] == K::MAETAH)
        {
            bytes += maetahBytes;
            continue;
        }
        const UbTransactionExtensionHeader header(m_kinds[index]);
        bytes += header.GetSerializedSize();
    }
    return bytes;
}

uint32_t
UbTaHeaderPlan::GetExtensionBytes(uint32_t maetahBytes, const UbTransactionFields& fields) const
{
    uint32_t bytes = 0;
    for (uint8_t index = 0; index < m_count; ++index)
    {
        const auto kind = m_kinds[index];
        if (kind == K::MAETAH) { bytes += maetahBytes; continue; }
        UbTransactionExtensionHeader header(kind);
        if (kind == K::BEETAH && fields.byteEnable)
            header.SetRaw(fields.byteEnable->bytes);
        bytes += header.GetSerializedSize();
    }
    return bytes;
}

UbTaHeaderPresence
DeclareRequestHeaderPresence(const UbTransactionRule& rule, const UbTransactionFields& fields)
{
    // Every active request layout starts with the basic header, so each rule member is fixed.
    const auto* kinds = rule.headers;
    const uint8_t count = rule.headerCount;
    UbTaHeaderPresence presence;
    presence.tokenValue = fields.tokenValue.has_value() &&
                          LayoutContains(kinds, count, K::TVETAH);
    presence.messageTarget = fields.messageTarget.has_value() &&
                             LayoutContains(kinds, count, K::MTETAH);
    // The phase-one field model has no user-data value, so UD_FLG stays off.
    presence.userData = false;
    if (fields.userData && LayoutContains(kinds, count, K::UDETAH))
        presence.userData = true;
    return presence;
}

UbTaHeaderPresence
CompactHeaderPresence(const UbTransactionRule& rule)
{
    UbTaHeaderPresence presence;
    presence.messageTarget = LayoutContains(rule.headers, rule.headerCount, K::MTETAH);
    return presence;
}

bool
WriteTransactionExtension(const UbTransactionFields& fields, UbTransactionExtensionHeader& header)
{
    switch (header.GetKind())
    {
    case K::TVETAH:
        if (!fields.tokenValue)
        {
            return false;
        }
        header.SetTokenValue(*fields.tokenValue);
        return true;
    case K::MTETAH:
        if (!fields.messageTarget)
        {
            return false;
        }
        header.SetTargetContext(fields.messageTarget->targetContextType,
                                fields.messageTarget->targetContextId,
                                fields.messageTarget->hint);
        return true;
    case K::IMMETAH:
        if (!fields.immediate)
        {
            return false;
        }
        header.SetImmediate(fields.immediate->data,
                            fields.immediate->tokenValue,
                            fields.immediate->messageLength);
        return true;
    case K::NTFETAH:
        if (!fields.notify)
        {
            return false;
        }
        header.SetNotify(fields.notify->address,
                         fields.notify->tokenId,
                         fields.notify->tokenValue,
                         fields.notify->data);
        return true;
    case K::BEETAH:
        if (!fields.byteEnable)
        {
            return false;
        }
        header.SetRaw(fields.byteEnable->bytes);
        return true;
    case K::UDETAH:
        if (!fields.userData) return false;
        header.SetUserData(fields.userData->type, fields.userData->opcode,
                           fields.userData->offset, fields.userData->reserved);
        return true;
    case K::TAIDETAH:
        if (!fields.taskId) return false;
        header.SetTaskId(*fields.taskId);
        return true;
    case K::MGMTETAH:
        if (!fields.management)
        {
            return false;
        }
        header.SetManagement(fields.management->code,
                             fields.management->subCode,
                             fields.management->response,
                             fields.management->status,
                             fields.management->pduLength);
        return true;
    default:
        // UDETAH and the inactive TAIDETAH/OFSTETAH carry no normalized field in this phase.
        return false;
    }
}

void
ReadTransactionExtension(const UbTransactionExtensionHeader& header, UbTransactionFields& fields)
{
    switch (header.GetKind())
    {
    case K::TVETAH:
        fields.tokenValue = header.GetTokenValue();
        break;
    case K::MTETAH:
        fields.messageTarget = UbMessageTargetFields{header.GetTargetContextType(),
                                                     header.GetTargetContextId(),
                                                     header.GetHint()};
        break;
    case K::IMMETAH:
        fields.immediate = UbImmediateFields{header.GetImmediateData(),
                                             header.GetImmediateTokenValue(),
                                             header.GetMessageLength()};
        break;
    case K::NTFETAH:
        fields.notify = UbNotifyFields{header.GetNotifyAddress(),
                                       header.GetNotifyTokenId(),
                                       header.GetNotifyTokenValue(),
                                       header.GetNotifyData()};
        break;
    case K::BEETAH:
        fields.byteEnable = UbByteEnableFields{header.GetRaw()};
        break;
    case K::UDETAH:
        fields.userData = UbUserDataFields{header.GetUserDataType(), header.GetUserDataOpcode(),
                                           header.GetUserDataOffset(), 0};
        break;
    case K::TAIDETAH:
        fields.taskId = header.GetTaskId();
        break;
    case K::MGMTETAH:
        fields.management = header.GetManagement();
        break;
    default:
        break;
    }
}

uint32_t
GetFullTaHeaderBytes(const UbTaHeaderPlan& plan, const UbTransactionFields& fields)
{
    return UB_TA_FULL_BASE_HEADER_BYTES + plan.GetExtensionBytes(UB_TA_FULL_MAETAH_BYTES, fields);
}

void
PrependFullTaHeaders(Ptr<Packet> packet,
                     const UbFullTaHeader& base,
                     const UbTaHeaderPlan& plan,
                     const UbMAExtTah& memory,
                     const UbTransactionFields& fields)
{
    // Extracted from BuildRtpDataPacket: AddHeader prepends, so emit the plan in reverse.
    for (int index = static_cast<int>(plan.GetCount()) - 1; index >= 0; --index)
    {
        const auto kind = plan.GetKind(static_cast<uint8_t>(index));
        if (kind == UbTransactionHeaderKind::MAETAH)
        {
            packet->AddHeader(memory);
            continue;
        }
        UbTransactionExtensionHeader header(kind);
        if (!WriteTransactionExtension(fields, header))
        {
            continue;
        }
        packet->AddHeader(header);
    }
    if (base.isResponse)
    {
        packet->AddHeader(base.response);
    }
    else
    {
        auto request = base.request;
        request.SetTvEn(plan.GetPresence().tokenValue);
        request.SetMtEn(plan.GetPresence().messageTarget);
        request.SetUdFlg(plan.GetPresence().userData);
        packet->AddHeader(request);
    }
}

UbTaDecodeResult
DecodeFullTaBase(Ptr<Packet> packet, UbFullTaHeader& base)
{
    if (packet == nullptr || packet->GetSize() < UB_TA_FULL_BASE_HEADER_BYTES)
    {
        return {UbTaDecodeError::TRUNCATED_BASE, K::BTAH};
    }
    UbTransactionHeader prefix;
    packet->PeekHeader(prefix);
    const auto opcode = static_cast<UbTransactionOpcode>(prefix.GetUbTransactionOpcode());
    base = UbFullTaHeader{};
    base.isResponse = opcode == UbTransactionOpcode::TAACK ||
                      opcode == UbTransactionOpcode::READ_RESPONSE ||
                      opcode == UbTransactionOpcode::ATOMIC_RESPONSE;
    if (base.isResponse)
    {
        packet->RemoveHeader(base.response);
    }
    else
    {
        packet->RemoveHeader(base.request);
    }
    return {};
}

UbTaDecodeResult
DecodeFullTaExtensions(Ptr<Packet> packet,
                       const UbTaHeaderPlan& plan,
                       UbMAExtTah& memory,
                       UbTransactionFields& fields)
{
    for (uint8_t index = 0; index < plan.GetCount(); ++index)
    {
        const auto kind = plan.GetKind(index);
        if (kind == K::MAETAH)
        {
            if (packet == nullptr || packet->GetSize() < memory.GetSerializedSize())
            {
                return {UbTaDecodeError::TRUNCATED_EXTENSION, kind};
            }
            packet->RemoveHeader(memory);
            continue;
        }
        UbTransactionExtensionHeader extension(kind);
        // The existing BE model supports P = 64, 128, 256, 512 or 1024 bytes.
        // Validate the width before allocation or Deserialize, never round malformed lengths.
        if (kind == K::BEETAH)
        {
            const uint32_t length = memory.GetLength();
            if (length != 64 && length != 128 && length != 256 && length != 512 && length != 1024)
            {
                return {UbTaDecodeError::INVALID_EXTENSION_LENGTH, kind};
            }
            extension.SetRaw(std::vector<uint8_t>(length / 8, 0));
        }
        if (packet == nullptr || packet->GetSize() < extension.GetSerializedSize())
        {
            return {UbTaDecodeError::TRUNCATED_EXTENSION, kind};
        }
        if (packet->RemoveHeader(extension) == 0)
        {
            return {UbTaDecodeError::INVALID_EXTENSION_LENGTH, kind};
        }
        ReadTransactionExtension(extension, fields);
    }
    return {};
}

} // namespace ns3
