// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/test.h"
#include "ns3/ub-transaction-rules.h"

namespace ns3
{
namespace
{
UbTransactionRequestInput
InboundRequest(UbTransactionOpcode opcode,
               uint32_t logicalBytes,
               uint32_t payloadBytes,
               UbTransactionFields fields = {})
{
    UbTransactionRequestInput request;
    request.opcode = opcode;
    request.logicalBytes = logicalBytes;
    request.payloadBytes = payloadBytes;
    request.fields = fields;
    request.maxPayloadBytes = UB_MTU_BYTE;
    request.origin = UbTransactionRequestOrigin::INBOUND_DECODE;
    return request;
}

UbTransactionRequestInput
LocalRequest(UbTransactionOpcode opcode,
             uint32_t logicalBytes,
             uint32_t payloadBytes,
             UbTransactionFields fields = {},
             std::optional<UbAtomicOptions> atomicOptions = std::nullopt)
{
    UbTransactionRequestInput request = InboundRequest(opcode, logicalBytes, payloadBytes, fields);
    request.origin = UbTransactionRequestOrigin::LOCAL_SUBMISSION;
    request.atomicOptions = atomicOptions;
    return request;
}

/** Memory-access fields exactly as a WQE submission supplies them from its remote address. */
UbTransactionFields
MemoryFields(uint32_t bytes)
{
    UbTransactionFields fields;
    fields.memoryAccess = UbMemoryAccessFields{0, 0, bytes};
    return fields;
}
} // namespace

class UbTransactionRulesTest : public TestCase
{
  public:
    UbTransactionRulesTest() : TestCase("Table 7-2 roles, reserved values and request constraints")
    {
    }

    void DoRun() override
    {
        for (uint8_t opcode = 0; opcode <= 0x1d; ++opcode)
        {
            const auto* rule = FindTransactionRule(static_cast<UbTransactionOpcode>(opcode));
            const bool supported =
                opcode != 0x02 && opcode != 0x16;
            NS_TEST_ASSERT_MSG_EQ(static_cast<bool>(rule != nullptr), supported,
                                  "Table 7-2 opcode coverage");
            if (rule)
            {
                const bool responseRole = opcode >= 0x11 && opcode <= 0x13;
                NS_TEST_ASSERT_MSG_EQ(
                    static_cast<uint8_t>(rule->role == UbTransactionSegmentKind::RESPONSE),
                    static_cast<uint8_t>(responseRole), "wire role");
            }
        }
        const auto* message = FindTransactionRule(UbTransactionOpcode::WRITE_WITH_IMMEDIATE);
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<uint8_t>(message->type == UbTransactionType::MESSAGE), true,
            "Write_with_immediate is a Message transaction");

        UbTransactionFields fields;
        auto result = ValidateTransactionRequest(
            InboundRequest(UbTransactionOpcode::RESERVED_02, 8, 8, fields));
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<uint8_t>(result.code == UbTransactionErrorCode::UNSUPPORTED_OPCODE), true,
            "Reserved never becomes an operation");
        result =
            ValidateTransactionRequest(InboundRequest(UbTransactionOpcode::TAACK, 8, 0, fields));
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<uint8_t>(result.code == UbTransactionErrorCode::WRONG_ROLE), true,
            "response cannot be submitted as a request");
        result =
            ValidateTransactionRequest(InboundRequest(UbTransactionOpcode::SEND, 8, 8, fields));
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<uint8_t>(result.code == UbTransactionErrorCode::MISSING_FIELD), true,
            "Decoded Send without a message target is rejected");
        result =
            ValidateTransactionRequest(InboundRequest(UbTransactionOpcode::READ, 8, 8, fields));
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<uint8_t>(result.code == UbTransactionErrorCode::INVALID_LENGTH), true,
            "Read requests have no payload");
        fields.memoryAccess = UbMemoryAccessFields{0, 0, 8};
        result =
            ValidateTransactionRequest(InboundRequest(UbTransactionOpcode::WRITE, 8, 8, fields));
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(result.IsValid()), true,
                              "ROI Write remains valid");
        result = ValidateTransactionRequest(
            LocalRequest(UbTransactionOpcode::WRITE_WITH_BE, 64, 64, fields));
        NS_TEST_ASSERT_MSG_EQ(result.IsValid(), true, "BE writes are supported");
        fields.taskId = 4;
        result =
            ValidateTransactionRequest(InboundRequest(UbTransactionOpcode::WRITE, 8, 8, fields));
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(result.IsValid()), true,
                              "correlation fields stay outside wire validation");
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<uint8_t>(result.resolved.responseOpcode == UbTransactionOpcode::TAACK),
            true, "resolved response");
        NS_TEST_ASSERT_MSG_EQ(result.resolved.requiresResponse, true, "all requests await response");

        result =
            ValidateTransactionRequest(InboundRequest(UbTransactionOpcode::READ, 8, 0, fields));
        NS_TEST_ASSERT_MSG_EQ(result.IsValid(), true, "READ request remains valid without payload");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(result.resolved.responseOpcode),
                              static_cast<uint8_t>(UbTransactionOpcode::READ_RESPONSE),
                              "READ owns its response opcode");
        NS_TEST_ASSERT_MSG_EQ(result.resolved.responsePayloadBytes, 8u,
                              "READ response length is the logical request length");
        NS_TEST_ASSERT_MSG_EQ(result.resolved.requiresResponse, true,
                              "READ waits for its response");

        result = ValidateTransactionRequest(
            InboundRequest(UbTransactionOpcode::ATOMIC_STORE, 8, 8, fields));
        NS_TEST_ASSERT_MSG_EQ(result.IsValid(), true, "Atomic Store remains valid");
        NS_TEST_ASSERT_MSG_EQ(result.resolved.responsePayloadBytes, 0u,
                              "Atomic Store has no response payload");
        NS_TEST_ASSERT_MSG_EQ(result.resolved.requiresResponse, true,
                              "Atomic Store waits for its response");

        fields.management = UbManagementFields{.code = 1, .subCode = 2, .response = 0,
                                               .status = 0, .pduLength = 8};
        result = ValidateTransactionRequest(
            InboundRequest(UbTransactionOpcode::MANAGEMENT, 8, 8, fields));
        NS_TEST_ASSERT_MSG_EQ(result.IsValid(), true, "Management request remains valid");
        NS_TEST_ASSERT_MSG_EQ(result.resolved.requiresResponse, true,
                              "Management waits for its response");

        for (uint32_t n : {1u, 2u, 4u, 8u, 16u, 32u, 64u})
        {
            auto queue = ValidateTransactionRequest(
                LocalRequest(UbTransactionOpcode::ATOMIC_STORE_WITH_RETURN_STATUS,
                             n,
                             n,
                             MemoryFields(n)));
            NS_TEST_ASSERT_MSG_EQ(queue.IsValid(), true, "Store-with-status width is accepted");
            NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(queue.resolved.responseOpcode), 0x11,
                                  "Store-with-status uses TAACK");
            NS_TEST_ASSERT_MSG_EQ(queue.resolved.responsePayloadBytes, 0u,
                                  "Store-with-status has no response payload");
        }
        auto badQueue = ValidateTransactionRequest(
            LocalRequest(UbTransactionOpcode::ATOMIC_STORE_WITH_RETURN_STATUS, 3, 3,
                         MemoryFields(3)));
        NS_TEST_ASSERT_MSG_EQ(badQueue.IsValid(), false, "invalid queue width is rejected");
        auto sync = ValidateTransactionRequest(
            LocalRequest(UbTransactionOpcode::ATOMIC_SYNC, 8, 8, MemoryFields(8)));
        NS_TEST_ASSERT_MSG_EQ(sync.IsValid(), true, "Atomic_sync is accepted");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(sync.resolved.responseOpcode), 0x13,
                              "Atomic_sync uses atomic response");
        NS_TEST_ASSERT_MSG_EQ(sync.resolved.responsePayloadBytes, 8u,
                              "Atomic_sync defaults to N response bytes");
        NS_TEST_ASSERT_MSG_EQ(
            IsAcceptedResponsePayloadBytes(UbTransactionOpcode::ATOMIC_SYNC, 8, 0),
            true,
            "Atomic_sync accepts the explicit zero response");
        NS_TEST_ASSERT_MSG_EQ(
            IsAcceptedResponsePayloadBytes(UbTransactionOpcode::ATOMIC_SYNC, 8, 8),
            true,
            "Atomic_sync accepts the default N response");
        for (uint32_t bytes : {1u, 4u, 9u})
        {
            NS_TEST_ASSERT_MSG_EQ(
                IsAcceptedResponsePayloadBytes(UbTransactionOpcode::ATOMIC_SYNC, 8, bytes),
                false,
                "Atomic_sync rejects a response length outside 0/N");
        }
        NS_TEST_ASSERT_MSG_EQ(
            IsAcceptedResponsePayloadBytes(UbTransactionOpcode::ATOMIC_FETCH_ADD, 8, 0),
            false,
            "A fixed-length atomic does not accept the Atomic_sync zero option");
        auto remote = ValidateTransactionRequest(
            LocalRequest(UbTransactionOpcode::WRITE_WITH_REMOTE_SYNC, 4096, 4096,
                         MemoryFields(4096)));
        NS_TEST_ASSERT_MSG_EQ(remote.IsValid(), true, "remote-sync write is accepted");
        NS_TEST_ASSERT_MSG_EQ(remote.fields.userData->type, 2u, "remote-sync UDETAH Type=2");

        for (const uint32_t bytes : {1u, 64u, 4096u})
        {
            auto storeAdd = ValidateTransactionRequest(LocalRequest(
                UbTransactionOpcode::WRITE_WITH_ATOMIC_STORE_ADD,
                bytes,
                bytes,
                MemoryFields(bytes)));
            NS_TEST_ASSERT_MSG_EQ(storeAdd.IsValid(), true,
                                  "store-add writes remain multi-packet memory operations");
            NS_TEST_ASSERT_MSG_EQ(storeAdd.fields.notify.has_value(), true,
                                  "store-add always carries its required notify header");
            NS_TEST_ASSERT_MSG_EQ(storeAdd.fields.userData.has_value(), false,
                                  "store-add leaves optional UDETAH disabled by default");
        }
        UbTransactionFields explicitNotify = MemoryFields(4096);
        explicitNotify.notify = UbNotifyFields{0x12340000, 7, 8, 9};
        explicitNotify.userData = UbUserDataFields{2, 0, 3, 0};
        const auto explicitStoreAdd = ValidateTransactionRequest(LocalRequest(
            UbTransactionOpcode::WRITE_WITH_ATOMIC_STORE_ADD, 4096, 4096, explicitNotify));
        NS_TEST_ASSERT_MSG_EQ(explicitStoreAdd.IsValid(), true,
                              "store-add accepts an explicitly supplied optional UDETAH");
        NS_TEST_ASSERT_MSG_EQ(explicitStoreAdd.fields.notify->address, 0x12340000u,
                              "store-add preserves notify address");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(ValidateTransactionRequest(InboundRequest(
                                         UbTransactionOpcode::WRITE_WITH_ATOMIC_STORE_ADD,
                                         4096,
                                         4096,
                                         MemoryFields(4096)))
                                         .code == UbTransactionErrorCode::MISSING_FIELD),
                              true,
                              "inbound store-add without NTFETAH is rejected");
    }
};

/** D1: required extension-header defaults are filled only for local submissions. */
class UbTransactionRequiredFieldDefaultsTest : public TestCase
{
  public:
    UbTransactionRequiredFieldDefaultsTest()
        : TestCase("required extension headers default locally and stay absent inbound")
    {
    }

    void DoRun() override
    {
        auto send = ValidateTransactionRequest(LocalRequest(UbTransactionOpcode::SEND, 64, 64));
        NS_TEST_ASSERT_MSG_EQ(send.IsValid(), true,
                              "local SEND receives its required MTETAH fields");
        NS_TEST_ASSERT_MSG_EQ(send.fields.messageTarget.has_value(), true,
                              "local SEND owns a message target");
        NS_TEST_ASSERT_MSG_EQ(send.fields.messageTarget->targetContextType, 0u,
                              "fixed default field is zero");
        NS_TEST_ASSERT_MSG_EQ(send.fields.messageTarget->targetContextId, 0u,
                              "fixed default field is zero");
        NS_TEST_ASSERT_MSG_EQ(send.fields.messageTarget->hint, 0u, "fixed default field is zero");

        auto immediate = ValidateTransactionRequest(
            LocalRequest(UbTransactionOpcode::SEND_WITH_IMMEDIATE, 64, 64));
        NS_TEST_ASSERT_MSG_EQ(immediate.IsValid(), true,
                              "local SEND_WITH_IMMEDIATE receives both required headers");
        NS_TEST_ASSERT_MSG_EQ(immediate.fields.immediate.has_value(), true,
                              "local immediate header is filled");
        NS_TEST_ASSERT_MSG_EQ(immediate.fields.immediate->messageLength, 64u,
                              "IMMETAH message length follows the transaction payload length");
        NS_TEST_ASSERT_MSG_EQ(immediate.fields.immediate->data, 0ull,
                              "IMMETAH immediate data defaults to zero");
        NS_TEST_ASSERT_MSG_EQ(immediate.fields.immediate->tokenValue, 0u,
                              "IMMETAH token defaults to zero");

        auto writeImmediate = ValidateTransactionRequest(
            LocalRequest(UbTransactionOpcode::WRITE_WITH_IMMEDIATE, 32, 32));
        NS_TEST_ASSERT_MSG_EQ(writeImmediate.IsValid(), true,
                              "local WRITE_WITH_IMMEDIATE is admissible");
        NS_TEST_ASSERT_MSG_EQ(writeImmediate.fields.immediate->messageLength, 32u,
                              "length field follows the submitted payload length");
        NS_TEST_ASSERT_MSG_EQ(writeImmediate.fields.messageTarget.has_value(), true,
                              "MTETAH default is independent of IMMETAH");

        UbTransactionFields notifyFields{.memoryAccess = UbMemoryAccessFields{0, 0, 16}};
        auto notify = ValidateTransactionRequest(
            LocalRequest(UbTransactionOpcode::WRITE_WITH_NOTIFY, 16, 16, notifyFields));
        NS_TEST_ASSERT_MSG_EQ(notify.IsValid(), true, "local WRITE_WITH_NOTIFY is admissible");
        NS_TEST_ASSERT_MSG_EQ(notify.fields.notify.has_value(), true,
                              "NTFETAH fields default for a local submission");
        NS_TEST_ASSERT_MSG_EQ(notify.fields.notify->address, 0ull, "NTFETAH address defaults to 0");
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<uint8_t>(ValidateTransactionRequest(InboundRequest(
                                     UbTransactionOpcode::WRITE_WITH_NOTIFY, 16, 16, notifyFields))
                                     .code == UbTransactionErrorCode::MISSING_FIELD),
            true, "decoded WRITE_WITH_NOTIFY without NTFETAH is rejected");

        auto management =
            ValidateTransactionRequest(LocalRequest(UbTransactionOpcode::MANAGEMENT, 128, 128));
        NS_TEST_ASSERT_MSG_EQ(management.IsValid(), true, "local MANAGEMENT with a PDU is valid");
        NS_TEST_ASSERT_MSG_EQ(management.fields.management.has_value(), true,
                              "MGMTETAH fields default for a local submission");
        NS_TEST_ASSERT_MSG_EQ(management.fields.management->pduLength, 128u,
                              "PDU length follows the transaction payload length");
        NS_TEST_ASSERT_MSG_EQ(management.fields.management->code, 0u,
                              "MGMTETAH code defaults to zero");
        NS_TEST_ASSERT_MSG_EQ(management.fields.management->status, 0u,
                              "MGMTETAH status defaults to zero");

        NS_TEST_ASSERT_MSG_EQ(
            static_cast<uint8_t>(ValidateTransactionRequest(
                                     InboundRequest(UbTransactionOpcode::MANAGEMENT, 128, 128))
                                     .code == UbTransactionErrorCode::MISSING_FIELD),
            true, "decoded MANAGEMENT without MGMTETAH is rejected");
        UbTransactionFields mismatchedManagement{
            .management = UbManagementFields{.pduLength = 120}};
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<uint8_t>(ValidateTransactionRequest(InboundRequest(
                                     UbTransactionOpcode::MANAGEMENT, 128, 128,
                                     mismatchedManagement))
                                     .code == UbTransactionErrorCode::INVALID_LENGTH),
            true, "decoded MANAGEMENT PDU length must match its payload");

        auto read = ValidateTransactionRequest(LocalRequest(
            UbTransactionOpcode::READ, 64, 0,
            UbTransactionFields{.memoryAccess = UbMemoryAccessFields{0, 0, 64}}));
        NS_TEST_ASSERT_MSG_EQ(read.IsValid(), true, "READ needs no extension default");
        NS_TEST_ASSERT_MSG_EQ(read.fields.messageTarget.has_value(), false,
                              "defaulting never invents a header the rule does not carry");
    }
};

/** D2: the single entry owns the Atomic width, sub-operation, and payload constraints. */
class UbTransactionAtomicEntryTest : public TestCase
{
  public:
    UbTransactionAtomicEntryTest()
        : TestCase("Atomic constraints resolve in the single request entry")
    {
    }

    void DoRun() override
    {
        for (uint8_t code = 7; code <= 15; ++code)
        {
            const auto opcode = static_cast<UbTransactionOpcode>(code);
            for (uint32_t width : {1u, 2u, 4u, 8u, 16u, 32u, 64u})
            {
                UbAtomicOptions options;
                if (opcode == UbTransactionOpcode::ATOMIC_STORE ||
                    opcode == UbTransactionOpcode::ATOMIC_LOAD)
                {
                    options.subOperation = UbAtomicSubOperation::ADD;
                }
                const auto result = ValidateTransactionRequest(
                    LocalRequest(opcode, width, width, MemoryFields(width), options));
                NS_TEST_ASSERT_MSG_EQ(result.IsValid(), true, result.message);
                NS_TEST_ASSERT_MSG_EQ(result.resolved.requestPayloadBytes,
                                      (opcode == UbTransactionOpcode::ATOMIC_COMPARE_SWAP
                                           ? 2 * width
                                           : width),
                                      "Only CAS defaults to two operand slots");
                NS_TEST_ASSERT_MSG_EQ(result.resolved.responsePayloadBytes,
                                      (opcode == UbTransactionOpcode::ATOMIC_STORE ? 0u : width),
                                      "Store is the only zero-payload atomic response");
                NS_TEST_ASSERT_MSG_EQ(result.resolved.requiresResponse, true,
                                      "Atomic requests await their response");
            }
        }

        UbAtomicOptions overridePayload;
        overridePayload.subOperation = UbAtomicSubOperation::ADD;
        overridePayload.requestPayloadBytes = 32;
        const auto overridden = ValidateTransactionRequest(
            LocalRequest(UbTransactionOpcode::ATOMIC_STORE, 8, 8, MemoryFields(8),
                         overridePayload));
        NS_TEST_ASSERT_MSG_EQ(overridden.IsValid(), true, overridden.message);
        NS_TEST_ASSERT_MSG_EQ(overridden.resolved.requestPayloadBytes, 32u,
                              "Explicit P takes priority");

        for (uint32_t width : {0u, 3u, 7u, 65u, UINT32_MAX})
        {
            NS_TEST_ASSERT_MSG_EQ(
                ValidateTransactionRequest(
                    InboundRequest(UbTransactionOpcode::ATOMIC_SWAP, width, 8, MemoryFields(8)))
                    .IsValid(),
                false, "Invalid operand width must be rejected");
        }
        // A decoded Load/Store carries no local sub-operation; its wire payload is authoritative.
        for (auto opcode :
             {UbTransactionOpcode::ATOMIC_STORE, UbTransactionOpcode::ATOMIC_LOAD})
        {
            const auto decoded =
                ValidateTransactionRequest(InboundRequest(opcode, 8, 8, MemoryFields(8)));
            NS_TEST_ASSERT_MSG_EQ(decoded.IsValid(), true, decoded.message);
            NS_TEST_ASSERT_MSG_EQ(decoded.resolved.requestPayloadBytes, 8u,
                                  "Decoded request payload is authoritative");
            NS_TEST_ASSERT_MSG_EQ(
                static_cast<uint8_t>(ValidateTransactionRequest(LocalRequest(
                                         opcode, 8, 8, MemoryFields(8), UbAtomicOptions{}))
                                         .code ==
                                     UbTransactionErrorCode::INVALID_ATOMIC_OPTIONS),
                true, "A local Load/Store needs its local suboperation");
        }
        UbAtomicOptions noSubOperation;
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<uint8_t>(
                ValidateTransactionRequest(
                    LocalRequest(UbTransactionOpcode::ATOMIC_STORE, 8, 8, MemoryFields(8),
                                 noSubOperation))
                    .code == UbTransactionErrorCode::INVALID_ATOMIC_OPTIONS),
            true, "Local Store without a suboperation is rejected");
        UbAtomicOptions badSubOperation;
        badSubOperation.subOperation = static_cast<UbAtomicSubOperation>(9);
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<uint8_t>(
                ValidateTransactionRequest(
                    LocalRequest(UbTransactionOpcode::ATOMIC_LOAD, 8, 8, MemoryFields(8),
                                 badSubOperation))
                    .code == UbTransactionErrorCode::INVALID_ATOMIC_OPTIONS),
            true, "Unknown local suboperation is rejected");
        UbAtomicOptions forbiddenOptions;
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<uint8_t>(
                ValidateTransactionRequest(
                    LocalRequest(UbTransactionOpcode::WRITE, 8, 8,
                                 UbTransactionFields{.memoryAccess = UbMemoryAccessFields{0, 0, 8}},
                                 forbiddenOptions))
                    .code == UbTransactionErrorCode::INVALID_ATOMIC_OPTIONS),
            true, "Atomic options are forbidden on a non-atomic request");
        UbAtomicOptions oversizedPayload;
        oversizedPayload.subOperation = UbAtomicSubOperation::ADD;
        oversizedPayload.requestPayloadBytes = UB_MTU_BYTE + 1;
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<uint8_t>(
                ValidateTransactionRequest(
                    LocalRequest(UbTransactionOpcode::ATOMIC_STORE, 8, 8, MemoryFields(8),
                                 oversizedPayload))
                    .code == UbTransactionErrorCode::INVALID_LENGTH),
            true, "Atomic payload above the transport limit is rejected");
    }
};

class UbTransactionRulesSuite : public TestSuite
{
  public:
    UbTransactionRulesSuite() : TestSuite("unified-bus-transaction-rules", Type::UNIT)
    {
        AddTestCase(new UbTransactionRulesTest, TestCase::Duration::QUICK);
        AddTestCase(new UbTransactionRequiredFieldDefaultsTest, TestCase::Duration::QUICK);
        AddTestCase(new UbTransactionAtomicEntryTest, TestCase::Duration::QUICK);
    }
} g_ubTransactionRulesSuite;
} // namespace ns3
