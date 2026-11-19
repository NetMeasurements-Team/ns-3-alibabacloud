// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TRANSACTION_RULES_H
#define UB_TRANSACTION_RULES_H
#include "ns3/ub-datatype.h"
#include <array>
#include <optional>
#include <string>

namespace ns3
{
/**
 * Static, declarative description of one supported transaction opcode.
 *
 * A rule owns the transaction type, the wire role, the ordered request layout, the request payload
 * bounds, and the response contract. Carrier builders and decoders read the ordered layout instead
 * of keeping per-opcode header assembly branches.
 */
struct UbTransactionRule
{
    /** Wire opcode this rule describes. */
    UbTransactionOpcode opcode;
    /** Transaction type of the operation. */
    UbTransactionType type;
    /** Whether this opcode is a request or a response on the wire. */
    UbTransactionSegmentKind role;
    /** Ordered header layout, written in the order the headers appear on the wire. */
    const UbTransactionHeaderKind* headers;
    /** Number of entries in `headers`. */
    uint8_t headerCount;
    /** Minimum request payload in bytes. */
    uint32_t minPayload;
    /** Maximum request payload in bytes. */
    uint32_t maxPayload;
    /** Whether a request must carry a non-zero payload. */
    bool payloadRequired;
    /** Whether the request must carry a positive logical transaction length. */
    bool logicalLengthRequired;
    /** Opcode of the protocol response the target answers with. */
    UbTransactionOpcode responseOpcode{UbTransactionOpcode::TAACK};
    /** Ordered header layout of the response, empty when the response role is unused. */
    const UbTransactionHeaderKind* responseHeaders{nullptr};
    /** Number of entries in `responseHeaders`. */
    uint8_t responseHeaderCount{0};
    /** Minimum response payload in bytes. */
    uint32_t responseMinPayload{0};
    /** Maximum response payload in bytes. */
    uint32_t responseMaxPayload{0};
    /**
     * Whether successful completion of this request requires its transaction response.
     *
     * This is the protocol requirement only. It stays true after the response has arrived; it is
     * never used as the dynamic completion progress of a WQE.
     */
    bool requiresResponse{true};
};

/** Reason why a request was rejected by the single request validation entry. */
enum class UbTransactionErrorCode : uint8_t
{
    NONE = 0,
    UNSUPPORTED_OPCODE,
    WRONG_ROLE,
    MISSING_FIELD,
    INVALID_LENGTH,
    INVALID_ATOMIC_OPTIONS,
};

/** Structured validation failure of one transaction request. */
struct UbTransactionValidationResult
{
    /** Failure class; `NONE` means the request is admissible. */
    UbTransactionErrorCode code{UbTransactionErrorCode::NONE};
    /** Opcode that was validated. */
    UbTransactionOpcode opcode{UbTransactionOpcode::MAX_OPCODE};
    /** Field name of a `MISSING_FIELD` or `INVALID_LENGTH` failure; empty otherwise. */
    std::string field;
    /** Human readable failure reason; empty on success. */
    std::string message;
    /**
     * Fields exactly as validated.
     *
     * For a local submission this copy carries the required extension-header defaults; for an
     * inbound request it is the decoded field set, so a missing required header stays missing and
     * the request is rejected instead of being repaired.
     */
    UbTransactionFields fields;
    /**
     * The one normalized result of this request.
     *
     * It is produced once here and then cached on the WQE or inbound segment; submission,
     * segmentation, transport, and response construction consume the cached copy instead of
     * recomputing payload metadata.
     */
    UbTransactionResolved resolved;
    /** Convenience predicate over `code`. */
    bool IsValid() const { return code == UbTransactionErrorCode::NONE; }
};

/** Origin of a request, which decides whether absent required fields may be defaulted. */
enum class UbTransactionRequestOrigin : uint8_t
{
    /**
     * A locally submitted request: absent required extension-header fields receive rule defaults.
     */
    LOCAL_SUBMISSION = 0,
    /** A decoded inbound request: fields are consumed as decoded and missing ones are rejected. */
    INBOUND_DECODE = 1,
};

/** Input of the single transaction request validation and normalization entry. */
struct UbTransactionRequestInput
{
    /** Wire opcode of the request. */
    UbTransactionOpcode opcode{UbTransactionOpcode::MAX_OPCODE};
    /**
     * Logical transaction length in bytes.
     *
     * For Atomic requests this is the operand width, which must be a power of two from 1 through
     * 64 bytes. It is the request's own length, not the payload carried by one packet.
     */
    uint32_t logicalBytes{0};
    /** Request payload bytes as submitted locally or decoded from the wire. */
    uint32_t payloadBytes{0};
    /** Transaction field values, without the required extension-header defaults. */
    UbTransactionFields fields;
    /**
     * Local Atomic input: sub-operation and explicit request payload override.
     *
     * Absent for non-Atomic requests and for inbound requests, where the payload is taken from the
     * wire and the Load/Store sub-operation is local-only information.
     */
    std::optional<UbAtomicOptions> atomicOptions;
    /** Transport payload limit the request must respect. */
    uint32_t maxPayloadBytes{UB_MTU_BYTE};
    /** Whether this request is a local submission or an inbound decoded request. */
    UbTransactionRequestOrigin origin{UbTransactionRequestOrigin::INBOUND_DECODE};
};

/** Whether the opcode is one of the nine Atomic request opcodes. */
bool IsAtomicTransaction(UbTransactionOpcode opcode);

/**
 * Whether an actual response payload length is acceptable for one request.
 *
 * Every request has one fixed response length, except Atomic_sync whose target executor may
 * choose either zero or the normalized N-byte default. The caller passes the normalized
 * `expectedPayloadBytes` from the request's resolved result, so both carriers share this rule.
 */
bool IsAcceptedResponsePayloadBytes(UbTransactionOpcode requestOpcode,
                                    uint32_t expectedPayloadBytes,
                                    uint32_t actualPayloadBytes);

/** Look up the static rule of one opcode, or `nullptr` when the opcode is unsupported. */
const UbTransactionRule* FindTransactionRule(UbTransactionOpcode opcode);

/**
 * Validate one transaction request and produce its single normalized result.
 *
 * This is the only entry that resolves rule lookup, role, required fields, payload and logical
 * lengths, and Atomic width, sub-operation, and payload constraints. On success the returned
 * `resolved` is the value cached on the WQE or inbound segment and consumed by every later stage.
 */
UbTransactionValidationResult ValidateTransactionRequest(const UbTransactionRequestInput& request);

/** Human readable transaction type name. */
const char* ToString(UbTransactionType type);
} // namespace ns3
#endif
