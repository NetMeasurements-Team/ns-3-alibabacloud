// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TA_CODEC_H
#define UB_TA_CODEC_H

#include "ns3/ub-datatype.h"
#include "ns3/ub-header.h"
#include "ns3/ub-transaction-rules.h"
#include "ns3/packet.h"

#include <array>

namespace ns3
{

/**
 * Wire presence of the transaction headers that a basic transaction header flags.
 *
 * TV_EN selects TVETAH, MT_EN selects MTETAH, and UD_FLG selects UDETAH. Headers without such a
 * flag are planned by the rule layout alone and are never guessed from the remaining bytes.
 */
struct UbTaHeaderPresence
{
    /** TV_EN / TK_VLD: the unit carries a TVETAH. */
    bool tokenValue{false};
    /** MT_EN: the unit carries a MTETAH. */
    bool messageTarget{false};
    /** UD_FLG: the unit carries a UDETAH. */
    bool userData{false};
};

/**
 * Ordered transaction-header plan of one TA unit, derived from its static transaction rule.
 *
 * The plan is the single source of wire header order and presence: a carrier serializes exactly the
 * planned headers and a decoder consumes exactly the planned headers, so the two carriers and the
 * two directions cannot drift apart. The basic header (BTAH or ATAH) is not part of the plan
 * because its format is carrier specific; it is always written last by a builder and read first by
 * a decoder.
 */
class UbTaHeaderPlan
{
  public:
    /**
     * Plan the extension headers of one request.
     *
     * Flag-driven headers are planned only when their flag is set; every other header listed by the
     * rule is operation-required and is always planned.
     */
    static UbTaHeaderPlan ForRequest(const UbTransactionRule& rule, UbTaHeaderPresence presence);
    /**
     * Plan the extension headers of one response.
     *
     * A response carries ATAH, which has no header presence flags, so flag-driven headers are never
     * planned for a response.
     */
    static UbTaHeaderPlan ForResponse(const UbTransactionRule& rule);

    /** Number of planned extension headers, excluding the carrier basic header. */
    uint8_t GetCount() const { return m_count; }
    /** Header kind at one wire position in wire order; index 0 follows the basic header. */
    UbTransactionHeaderKind GetKind(uint8_t wireIndex) const { return m_kinds[wireIndex]; }
    /** Whether one header kind is planned for this unit. */
    bool Contains(UbTransactionHeaderKind kind) const;
    /** Presence this plan was built with, so a builder can declare the same basic-header flags. */
    const UbTaHeaderPresence& GetPresence() const { return m_presence; }
    /**
     * Serialized size of the planned extension headers, excluding the basic header.
     *
     * @param maetahBytes MAETAH size of the carrier format that will write this plan.
     */
    uint32_t GetExtensionBytes(uint32_t maetahBytes) const;
    uint32_t GetExtensionBytes(uint32_t maetahBytes, const UbTransactionFields& fields) const;

  private:
    static UbTaHeaderPlan Build(const UbTransactionHeaderKind* kinds,
                                uint8_t count,
                                UbTaHeaderPresence presence);

    std::array<UbTransactionHeaderKind, 8> m_kinds{};
    uint8_t m_count{0};
    UbTaHeaderPresence m_presence{};
};

/** Serialized MAETAH size in bytes of the full (RTP and CFG6 CTP) transaction header format. */
constexpr uint32_t UB_TA_FULL_MAETAH_BYTES = 16;
/** Serialized MAETAH size in bytes of the compact (LDST) transaction header format. */
constexpr uint32_t UB_TA_COMPACT_MAETAH_BYTES = 12;
/** Serialized BTAH/ATAH size in bytes of the full (RTP and CFG6 CTP) transaction header format. */
constexpr uint32_t UB_TA_FULL_BASE_HEADER_BYTES = 8;
/** Serialized BTAH/ATAH size in bytes of the compact (LDST) transaction header format. */
constexpr uint32_t UB_TA_COMPACT_BASE_HEADER_BYTES = 4;

/**
 * Declare the header presence a builder must announce for one request.
 *
 * A flag-driven header is announced when the rule lists it and its normalized field value exists;
 * the phase-one field model has no user-data value, so UD_FLG stays off.
 */
UbTaHeaderPresence DeclareRequestHeaderPresence(const UbTransactionRule& rule,
                                                const UbTransactionFields& fields);

/**
 * The rule-owned header presence a compact (LDST) basic transaction header cannot flag.
 *
 * The compact basic header has no MT_EN bit, so MTETAH is carried whenever the rule lists it.
 * UDETAH stays absent here; the compact UD flag supplies that presence at the carrier call site.
 */
UbTaHeaderPresence CompactHeaderPresence(const UbTransactionRule& rule);

/**
 * Write the field values of one planned extension header.
 *
 * @return false when the header kind carries no normalized field value in this phase; the caller
 * must then leave the header out instead of inventing a default.
 */
bool WriteTransactionExtension(const UbTransactionFields& fields,
                               UbTransactionExtensionHeader& header);

/** Read one decoded extension header into the normalized field set. */
void ReadTransactionExtension(const UbTransactionExtensionHeader& header,
                              UbTransactionFields& fields);

/** Full base header, preserving all response fields for the carrier's validation policy. */
struct UbFullTaHeader
{
    bool isResponse{false};
    UbTransactionHeader request;
    UbAckTransactionHeader response;
};

enum class UbTaDecodeError
{
    NONE,
    TRUNCATED_BASE,
    TRUNCATED_EXTENSION,
    INVALID_EXTENSION_LENGTH,
};

/** Structural failure only; transaction rules and the carrier own semantic rejection. */
struct UbTaDecodeResult
{
    UbTaDecodeError error{UbTaDecodeError::NONE};
    UbTransactionHeaderKind header{UbTransactionHeaderKind::BTAH};
    bool IsSuccess() const { return error == UbTaDecodeError::NONE; }
};

uint32_t GetFullTaHeaderBytes(const UbTaHeaderPlan& plan, const UbTransactionFields& fields);

/** Prepend full headers to carrier-owned payload without inspecting tags or owner state. */
void PrependFullTaHeaders(Ptr<Packet> packet,
                         const UbFullTaHeader& base,
                         const UbTaHeaderPlan& plan,
                         const UbMAExtTah& memory,
                         const UbTransactionFields& fields);

/** Decode the base before the caller resolves a request-dependent response layout. */
UbTaDecodeResult DecodeFullTaBase(Ptr<Packet> packet, UbFullTaHeader& base);
UbTaDecodeResult DecodeFullTaExtensions(Ptr<Packet> packet,
                                      const UbTaHeaderPlan& plan,
                                      UbMAExtTah& memory,
                                      UbTransactionFields& fields);

} // namespace ns3

#endif
