// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/test.h"
#include "ns3/node.h"
#include "ns3/ub-ctp.h"
#include "ns3/ub-function.h"
#include "ns3/ub-header.h"
#include "ns3/ub-ta-codec.h"
#include "ns3/ub-transaction-rules.h"
#include "ns3/ub-transport.h"

#include <vector>

namespace ns3
{
class UbTransactionExtensionCodecTest : public TestCase
{
 public: UbTransactionExtensionCodecTest() : TestCase("transaction extension headers encode in declared order") {}
 void DoRun() override
 {
   UbTransactionExtensionHeader tv(UbTransactionHeaderKind::TVETAH); tv.SetTokenValue(0x12345678); Packet p; p.AddHeader(tv); UbTransactionExtensionHeader tv2(UbTransactionHeaderKind::TVETAH); p.RemoveHeader(tv2); NS_TEST_ASSERT_MSG_EQ(tv2.GetTokenValue(), 0x12345678u, "TVETAH round trip");
   UbTransactionExtensionHeader mt(UbTransactionHeaderKind::MTETAH); mt.SetTargetContext(2, 0xABCDE, 0x42); NS_TEST_ASSERT_MSG_EQ(mt.GetSerializedSize(), 4u, "MTETAH size"); Packet q; q.AddHeader(mt); UbTransactionExtensionHeader mt2(UbTransactionHeaderKind::MTETAH); q.RemoveHeader(mt2); NS_TEST_ASSERT_MSG_EQ(mt2.GetTargetContextType(), 2u, "MT type"); NS_TEST_ASSERT_MSG_EQ(mt2.GetTargetContextId(), 0xABCDEu, "MT id"); NS_TEST_ASSERT_MSG_EQ(mt2.GetHint(), 0x42u, "MT hint");
   UbTransactionExtensionHeader im(UbTransactionHeaderKind::IMMETAH); im.SetImmediate(0x1122334455667788ULL, 0xAABBCCDD, 0x01020304); Packet r; r.AddHeader(im); UbTransactionExtensionHeader im2(UbTransactionHeaderKind::IMMETAH); r.RemoveHeader(im2); NS_TEST_ASSERT_MSG_EQ(im2.GetImmediateData(), 0x1122334455667788ULL, "immediate data"); NS_TEST_ASSERT_MSG_EQ(im2.GetImmediateTokenValue(), 0xAABBCCDDu, "immediate token"); NS_TEST_ASSERT_MSG_EQ(im2.GetMessageLength(), 0x01020304u, "message length");
   UbAckTransactionHeader ack; ack.SetIniRcId(0xABCDE); Packet a; a.AddHeader(ack); UbAckTransactionHeader ack2; a.RemoveHeader(ack2); NS_TEST_ASSERT_MSG_EQ(ack2.GetIniRcId(), 0xABCDEu, "full ATAH keeps 20 bit INI_RC_ID");
   UbTransactionExtensionHeader ud(UbTransactionHeaderKind::UDETAH); ud.SetUserData(2, 0, 0, 0); Packet u; u.AddHeader(ud); UbTransactionExtensionHeader ud2(UbTransactionHeaderKind::UDETAH); u.RemoveHeader(ud2); NS_TEST_ASSERT_MSG_EQ(ud2.GetUserDataType(), 2u, "UDETAH type");
   UbTransactionExtensionHeader taid(UbTransactionHeaderKind::TAIDETAH); taid.SetTaskId(0x1234); Packet t; t.AddHeader(taid); UbTransactionExtensionHeader taid2(UbTransactionHeaderKind::TAIDETAH); t.RemoveHeader(taid2); NS_TEST_ASSERT_MSG_EQ(taid2.GetTaskId(), 0x1234u, "TAIDETAH echo");
  }
};

/** The rule-driven plan is the single source of transaction header order and presence. */
class UbTaHeaderPlanTest : public TestCase
{
  public:
    UbTaHeaderPlanTest() : TestCase("rule layout plans the ordered transaction header sequence")
    {
    }

  private:
    using K = UbTransactionHeaderKind;

    /** Wire order of one plan, rendered as the header-kind names used by the expectations. */
    static std::string
    Kinds(const UbTaHeaderPlan& plan)
    {
        std::string text;
        for (uint8_t index = 0; index < plan.GetCount(); ++index)
        {
            if (!text.empty())
            {
                text += ",";
            }
            switch (plan.GetKind(index))
            {
            case K::BTAH:
                text += "BTAH";
                break;
            case K::ATAH:
                text += "ATAH";
                break;
            case K::MAETAH:
                text += "MAETAH";
                break;
            case K::MTETAH:
                text += "MTETAH";
                break;
            case K::TVETAH:
                text += "TVETAH";
                break;
            case K::IMMETAH:
                text += "IMMETAH";
                break;
            case K::NTFETAH:
                text += "NTFETAH";
                break;
            case K::MGMTETAH:
                text += "MGMTETAH";
                break;
            case K::BEETAH:
                text += "BEETAH";
                break;
            case K::UDETAH:
                text += "UDETAH";
                break;
            default:
                text += "INVALID";
                break;
            }
        }
        return text;
    }

    UbTaHeaderPlan PlanFor(UbTransactionOpcode opcode,
                           bool messageTarget = false,
                           bool userData = false)
    {
        const auto* rule = FindTransactionRule(opcode);
        if (rule == nullptr)
        {
            return UbTaHeaderPlan();
        }
        return UbTaHeaderPlan::ForRequest(*rule,
                                          UbTaHeaderPresence{false, messageTarget, userData});
    }

    void DoRun() override
    {
        // Operation-required headers follow the rule layout and never the remaining bytes.
        NS_TEST_ASSERT_MSG_EQ(Kinds(PlanFor(UbTransactionOpcode::WRITE)), std::string("MAETAH"),
                              "WRITE plans MAETAH only");
        NS_TEST_ASSERT_MSG_EQ(Kinds(PlanFor(UbTransactionOpcode::READ)), std::string("MAETAH"),
                              "READ plans MAETAH only");
        NS_TEST_ASSERT_MSG_EQ(Kinds(PlanFor(UbTransactionOpcode::ATOMIC_COMPARE_SWAP)),
                              std::string("MAETAH"), "Atomic requests plan MAETAH only");
        NS_TEST_ASSERT_MSG_EQ(Kinds(PlanFor(UbTransactionOpcode::WRITE_WITH_NOTIFY)),
                              std::string("MAETAH,NTFETAH"),
                              "notify header follows memory access");
        NS_TEST_ASSERT_MSG_EQ(Kinds(PlanFor(UbTransactionOpcode::WRITE_WITH_ATOMIC_STORE_ADD)),
                              std::string("MAETAH,NTFETAH"),
                              "store-add omits optional UDETAH by default");
        NS_TEST_ASSERT_MSG_EQ(
            Kinds(PlanFor(UbTransactionOpcode::WRITE_WITH_ATOMIC_STORE_ADD, false, true)),
            std::string("UDETAH,MAETAH,NTFETAH"),
            "store-add carries optional UDETAH when enabled");
        NS_TEST_ASSERT_MSG_EQ(Kinds(PlanFor(UbTransactionOpcode::MANAGEMENT)),
                              std::string("MGMTETAH"), "management plans MGMTETAH only");
        NS_TEST_ASSERT_MSG_EQ(Kinds(PlanFor(UbTransactionOpcode::WRITE_WITH_IMMEDIATE, true)),
                              std::string("MTETAH,MAETAH,IMMETAH"),
                              "write-with-immediate keeps the rule layout order");
        NS_TEST_ASSERT_MSG_EQ(Kinds(PlanFor(UbTransactionOpcode::SEND_WITH_IMMEDIATE, true)),
                              std::string("MTETAH,IMMETAH"),
                              "message request plans both headers");

        // A flag-driven header exists only when the wire flag is set.
        NS_TEST_ASSERT_MSG_EQ(Kinds(PlanFor(UbTransactionOpcode::SEND)), std::string(""),
                              "SEND plans no header while MT_EN is off");
        NS_TEST_ASSERT_MSG_EQ(Kinds(PlanFor(UbTransactionOpcode::SEND, true)),
                              std::string("MTETAH"), "SEND plans MTETAH when MT_EN is set");
        NS_TEST_ASSERT_MSG_EQ(Kinds(PlanFor(UbTransactionOpcode::WRITE_WITH_IMMEDIATE, false)),
                              std::string("MAETAH,IMMETAH"),
                              "WRITE_WITH_IMMEDIATE keeps its operation-required headers");
        NS_TEST_ASSERT_MSG_EQ(Kinds(PlanFor(UbTransactionOpcode::WRITE_WITH_BE)),
                              std::string("MAETAH,BEETAH"),
                              "WRITE_WITH_BE keeps the variable BE header in rule order");
        NS_TEST_ASSERT_MSG_EQ(Kinds(PlanFor(UbTransactionOpcode::WRITE_WITH_BE, false, true)),
                              std::string("UDETAH,MAETAH,BEETAH"),
                              "WRITE_WITH_BE carries optional UDETAH when enabled");
        NS_TEST_ASSERT_MSG_EQ(PlanFor(UbTransactionOpcode::SEND).Contains(K::MAETAH), false,
                              "SEND never plans MAETAH");
        NS_TEST_ASSERT_MSG_EQ(PlanFor(UbTransactionOpcode::SEND, true).Contains(K::MAETAH), false,
                              "message requests carry no memory access header");

        // Responses carry the basic header only in this phase.
        const auto* readRule = FindTransactionRule(UbTransactionOpcode::READ);
        const auto* atomicRule = FindTransactionRule(UbTransactionOpcode::ATOMIC_SWAP);
        NS_TEST_ASSERT_MSG_NE(readRule, nullptr, "READ rule exists");
        NS_TEST_ASSERT_MSG_NE(atomicRule, nullptr, "Atomic rule exists");
        NS_TEST_ASSERT_MSG_EQ(Kinds(UbTaHeaderPlan::ForResponse(*readRule)), std::string(""),
                              "READ_RESPONSE plans no extension header");
        NS_TEST_ASSERT_MSG_EQ(Kinds(UbTaHeaderPlan::ForResponse(*atomicRule)), std::string(""),
                              "ATOMIC_RESPONSE plans no extension header");

        // Serialized size of the planned extension headers, in both carrier formats.
        NS_TEST_ASSERT_MSG_EQ(PlanFor(UbTransactionOpcode::WRITE).GetExtensionBytes(
                                  UB_TA_FULL_MAETAH_BYTES),
                              UB_TA_FULL_MAETAH_BYTES, "full WRITE carries one full MAETAH");
        NS_TEST_ASSERT_MSG_EQ(PlanFor(UbTransactionOpcode::WRITE).GetExtensionBytes(
                                  UB_TA_COMPACT_MAETAH_BYTES),
                              UB_TA_COMPACT_MAETAH_BYTES,
                              "compact WRITE carries one compact MAETAH");
        NS_TEST_ASSERT_MSG_EQ(
            PlanFor(UbTransactionOpcode::WRITE_WITH_IMMEDIATE, true).GetExtensionBytes(
                UB_TA_FULL_MAETAH_BYTES),
            4u + UB_TA_FULL_MAETAH_BYTES + 16u,
            "write-with-immediate extension bytes follow the rule layout");

        // Declared presence follows the normalized field set.
        const auto* sendRule = FindTransactionRule(UbTransactionOpcode::SEND);
        const auto* writeRule = FindTransactionRule(UbTransactionOpcode::WRITE);
        NS_TEST_ASSERT_MSG_NE(sendRule, nullptr, "SEND rule exists");
        NS_TEST_ASSERT_MSG_NE(writeRule, nullptr, "WRITE rule exists");
        NS_TEST_ASSERT_MSG_EQ(
            DeclareRequestHeaderPresence(*sendRule, UbTransactionFields{}).messageTarget, false,
            "an unnormalized SEND does not declare MTETAH");
        UbTransactionFields sendFields;
        sendFields.messageTarget = UbMessageTargetFields{};
        NS_TEST_ASSERT_MSG_EQ(DeclareRequestHeaderPresence(*sendRule, sendFields).messageTarget,
            true, "a normalized SEND declares MTETAH");
        NS_TEST_ASSERT_MSG_EQ(
            DeclareRequestHeaderPresence(*writeRule, UbTransactionFields{}).messageTarget,
            false, "WRITE never declares MTETAH");
        // The compact basic header cannot flag MTETAH, so its parser must consume the
        // rule-owned header rather than relying on an absent MT_EN bit.
        NS_TEST_ASSERT_MSG_EQ(CompactHeaderPresence(*sendRule).messageTarget, true,
                              "compact SEND always plans MTETAH");
        NS_TEST_ASSERT_MSG_EQ(CompactHeaderPresence(*sendRule).userData, false,
                              "compact UDETAH presence stays with the UD flag");
        NS_TEST_ASSERT_MSG_EQ(Kinds(UbTaHeaderPlan::ForRequest(
                                  *sendRule, CompactHeaderPresence(*sendRule))),
                              std::string("MTETAH"),
                              "compact SEND parser consumes the rule-owned MTETAH");

        // The shared field codec round-trips every active extension header.
        UbTransactionFields written;
        written.tokenValue = 0x1234u;
        written.messageTarget = UbMessageTargetFields{2, 0xABCDE, 0x42};
        written.immediate = UbImmediateFields{0x1122334455667788ull, 0xAABBCCDD, 0x01020304};
        written.notify = UbNotifyFields{0x123456789ABull, 0x54321, 0xDEADBEEF, 0x99ull};
        written.management = UbManagementFields{1, 2, false, 0, 64};
        for (const K kind :
             {K::TVETAH, K::MTETAH, K::IMMETAH, K::NTFETAH, K::MGMTETAH})
        {
            UbTransactionExtensionHeader header(kind);
            NS_TEST_ASSERT_MSG_EQ(WriteTransactionExtension(written, header), true,
                                  "planned header carries its field value");
            Packet packet;
            packet.AddHeader(header);
            UbTransactionExtensionHeader decoded(kind);
            NS_TEST_ASSERT_MSG_NE(packet.RemoveHeader(decoded), 0u, "planned header decodes");
            UbTransactionFields read;
            ReadTransactionExtension(decoded, read);
            // Only the decoded header populates its own field; the others stay untouched.
            NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(read.messageTarget.has_value()),
                                  static_cast<uint8_t>(kind == K::MTETAH),
                                  "decoded MTETAH presence follows the header kind");
            NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(read.immediate.has_value()),
                                  static_cast<uint8_t>(kind == K::IMMETAH),
                                  "decoded IMMETAH presence follows the header kind");
            NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(read.notify.has_value()),
                                  static_cast<uint8_t>(kind == K::NTFETAH),
                                  "decoded NTFETAH presence follows the header kind");
            NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(read.management.has_value()),
                                  static_cast<uint8_t>(kind == K::MGMTETAH),
                                  "decoded MGMTETAH presence follows the header kind");
            NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(read.tokenValue.has_value()),
                                  static_cast<uint8_t>(kind == K::TVETAH),
                                  "decoded TVETAH presence follows the header kind");
            if (kind == K::MTETAH)
            {
                NS_TEST_ASSERT_MSG_EQ(read.messageTarget->targetContextId, 0xABCDEu,
                                      "MTETAH context id round trip");
                NS_TEST_ASSERT_MSG_EQ(read.messageTarget->hint, 0x42u, "MTETAH hint round trip");
            }
            if (kind == K::IMMETAH)
            {
                NS_TEST_ASSERT_MSG_EQ(read.immediate->messageLength, 0x01020304u,
                                      "IMMETAH message length round trip");
                NS_TEST_ASSERT_MSG_EQ(read.immediate->data, 0x1122334455667788ull,
                                      "IMMETAH data round trip");
            }
            if (kind == K::NTFETAH)
            {
                NS_TEST_ASSERT_MSG_EQ(read.notify->tokenValue, 0xDEADBEEFu,
                                      "NTFETAH token value round trip");
            }
            if (kind == K::MGMTETAH)
            {
                NS_TEST_ASSERT_MSG_EQ(read.management->pduLength, 64u,
                                      "MGMTETAH PDU length round trip");
            }
            if (kind == K::TVETAH)
            {
                NS_TEST_ASSERT_MSG_EQ(read.tokenValue.value_or(0u), 0x1234u,
                                      "TVETAH token round trip");
            }
        }
        written.byteEnable = UbByteEnableFields{std::vector<uint8_t>(32, 0xff)};
        UbTransactionExtensionHeader beetah(K::BEETAH);
        NS_TEST_ASSERT_MSG_EQ(WriteTransactionExtension(written, beetah), true,
                              "BEETAH carries the normalized bitmap");
        NS_TEST_ASSERT_MSG_EQ(beetah.GetSerializedSize(), 32u,
                              "BEETAH size follows payload/8");
        Packet bePacket;
        bePacket.AddHeader(beetah);
        UbTransactionExtensionHeader decodedBe(K::BEETAH);
        decodedBe.SetRaw(std::vector<uint8_t>(32, 0));
        NS_TEST_ASSERT_MSG_NE(bePacket.RemoveHeader(decodedBe), 0u,
                              "variable-size BEETAH decodes with its planned width");
        NS_TEST_ASSERT_MSG_EQ(decodedBe.GetRaw().size(), 32u,
                              "BEETAH preserves the complete bitmap");
        // A field-less planned header is left out instead of being defaulted.
        UbTransactionExtensionHeader noField(K::IMMETAH);
        NS_TEST_ASSERT_MSG_EQ(WriteTransactionExtension(UbTransactionFields{}, noField), false,
                              "a missing field is never repaired by the codec");
    }
};

/**
 * Ticket 02: the compact carrier writes exactly the rule-driven plan, and never a fixed MAETAH.
 */
class UbCtpRuleDrivenLayoutTest : public TestCase
{
  public:
    UbCtpRuleDrivenLayoutTest() : TestCase("CFG6 carrier writes the planned transaction headers")
    {
    }

  private:
    using K = UbTransactionHeaderKind;

    static UbTransactionFields RequestFields(UbTransactionOpcode opcode)
    {
        UbTransactionFields fields;
        switch (opcode)
        {
        case UbTransactionOpcode::SEND:
        case UbTransactionOpcode::SEND_WITH_IMMEDIATE:
        case UbTransactionOpcode::WRITE_WITH_IMMEDIATE:
            fields.messageTarget = UbMessageTargetFields{1, 0x12345, 2};
            break;
        case UbTransactionOpcode::WRITE_WITH_NOTIFY:
            fields.notify = UbNotifyFields{0x1000, 0x42, 0x99, 0xAB};
            break;
        default:
            break;
        }
        if (opcode == UbTransactionOpcode::SEND_WITH_IMMEDIATE ||
            opcode == UbTransactionOpcode::WRITE_WITH_IMMEDIATE)
        {
            fields.immediate = UbImmediateFields{0x1112131415161718ull, 0x99, 32};
        }
        if (opcode == UbTransactionOpcode::MANAGEMENT)
        {
            fields.management = UbManagementFields{1, 0, false, 0, 32};
        }
        return fields;
    }

    Ptr<UbWqeSegment> RequestSegment(UbTransactionOpcode opcode,
                                     uint32_t logicalBytes,
                                     uint32_t payloadBytes,
                                     const UbTransactionFields& fields)
    {
        Ptr<UbWqeSegment> segment = CreateObject<UbWqeSegment>();
        segment->SetSrc(0);
        segment->SetDest(1);
        segment->SetSport(utils::CTP_WILDCARD_PORT);
        segment->SetDport(utils::CTP_WILDCARD_PORT);
        segment->SetPriority(7);
        segment->SetType(opcode);
        segment->SetSize(logicalBytes);
        segment->SetTaSsn(0);
        segment->SetSegmentKind(UbTransactionSegmentKind::REQUEST);
        segment->SetRequestOpcode(opcode);
        segment->SetLogicalBytes(logicalBytes);
        segment->SetPayloadBytes(payloadBytes);
        segment->SetCarrierBytes(opcode == UbTransactionOpcode::READ ? 1 : payloadBytes);
        segment->SetResLenBytes(logicalBytes);
        segment->SetRemoteAddress(0x1000);
        segment->SetRemoteTokenId(0x42);
        segment->SetTransactionFields(fields);
        segment->SetCtpRequesterIdentity(0, 1);
        return segment;
    }

    /** Strip the CFG6 carrier wrapper down to the transaction unit. */
    Ptr<Packet> StripCompactCarrier(Ptr<Packet> packet)
    {
        UbDatalinkPacketHeader dataLinkHeader;
        UbCna16NetworkHeader cnaHeader;
        UbCtpHeader ctpHeader;
        UbCompactUpiHeader upiHeader;
        UbCompactEidHeader eidHeader;
        NS_TEST_EXPECT_MSG_NE(packet->RemoveHeader(dataLinkHeader), 0u, "CTP datalink header");
        NS_TEST_EXPECT_MSG_NE(packet->RemoveHeader(cnaHeader), 0u, "CTP CNA16 header");
        NS_TEST_EXPECT_MSG_NE(packet->RemoveHeader(ctpHeader), 0u, "CTP header");
        NS_TEST_EXPECT_MSG_NE(packet->RemoveHeader(upiHeader), 0u, "CTP UPI header");
        NS_TEST_EXPECT_MSG_NE(packet->RemoveHeader(eidHeader), 0u, "CTP EID header");
        packet->RemoveAtEnd(ctpHeader.GetPadding());
        return packet;
    }

    /** Consume the expected extension sequence and return the bytes it occupied. */
    uint32_t ConsumeExpected(Ptr<Packet> packet, const std::string& expected)
    {
        uint32_t bytes = 0;
        for (const std::string& name : SplitNames(expected))
        {
            if (name == "MAETAH")
            {
                UbMAExtTah maHeader;
                NS_TEST_EXPECT_MSG_NE(packet->RemoveHeader(maHeader), 0u, "full MAETAH present");
                NS_TEST_EXPECT_MSG_EQ(maHeader.GetSerializedSize(), UB_TA_FULL_MAETAH_BYTES,
                                      "full MAETAH size");
                NS_TEST_EXPECT_MSG_EQ(maHeader.GetTokenId(), 0x42u, "full MAETAH token");
                bytes += UB_TA_FULL_MAETAH_BYTES;
                continue;
            }
            UbTransactionExtensionHeader header(KindOf(name));
            NS_TEST_EXPECT_MSG_NE(packet->RemoveHeader(header), 0u, name + " is present");
            NS_TEST_EXPECT_MSG_EQ(header.GetSerializedSize(),
                                  UbTransactionExtensionHeader(KindOf(name)).GetSerializedSize(),
                                  name + " size follows the header kind");
            bytes += header.GetSerializedSize();
        }
        return bytes;
    }

    static K KindOf(const std::string& name)
    {
        if (name == "MTETAH")
            return K::MTETAH;
        if (name == "IMMETAH")
            return K::IMMETAH;
        if (name == "NTFETAH")
            return K::NTFETAH;
        if (name == "MGMTETAH")
            return K::MGMTETAH;
        if (name == "TVETAH")
            return K::TVETAH;
        return K::MAETAH;
    }

    static std::vector<std::string> SplitNames(const std::string& expected)
    {
        std::vector<std::string> names;
        std::string current;
        for (const char character : expected)
        {
            if (character == ',')
            {
                names.push_back(current);
                current.clear();
                continue;
            }
            current += character;
        }
        if (!current.empty())
        {
            names.push_back(current);
        }
        return names;
    }

    void CheckRequestLayout(const std::string& expected,
                            UbTransactionOpcode opcode,
                            uint32_t logicalBytes,
                            uint32_t payloadBytes)
    {
        Ptr<UbCtpTransportService> service = CreateObject<UbCtpTransportService>();
        Ptr<Node> node = CreateObject<Node>();
        service->SetNode(node);
        const UbCtpEntityKey key{.srcEntityId = 1, .dstNodeId = 1, .dstEntityId = 1, .vl = 7};
        Ptr<UbWqeSegment> segment =
            RequestSegment(opcode, logicalBytes, payloadBytes, RequestFields(opcode));
        Ptr<Packet> packet = service->BuildDataPacket(segment, key);
        NS_TEST_ASSERT_MSG_NE(packet, nullptr, "CTP packet is built");

        Ptr<Packet> body = StripCompactCarrier(packet);
        UbTransactionHeader taHeader;
        NS_TEST_ASSERT_MSG_NE(body->RemoveHeader(taHeader), 0u, "full TA header decodes");
        NS_TEST_ASSERT_MSG_EQ(taHeader.GetUbTransactionOpcode(), static_cast<uint8_t>(opcode),
                              "full TA header carries the segment opcode");
        const uint32_t extensionBytes = ConsumeExpected(body, expected);
        NS_TEST_ASSERT_MSG_EQ(body->GetSize(), payloadBytes,
                              "the declared headers never leak into the compact payload");
        if (!expected.empty())
        {
            NS_TEST_ASSERT_MSG_GT(extensionBytes, 0u, "the plan declares its extension bytes");
        }
    }

    void DoRun() override
    {
        // Operation-required headers follow the rule layout; message requests never carry MAETAH.
        CheckRequestLayout("MTETAH", UbTransactionOpcode::SEND, 32, 32);
        CheckRequestLayout("MTETAH,IMMETAH", UbTransactionOpcode::SEND_WITH_IMMEDIATE, 32, 32);
        CheckRequestLayout("MTETAH,MAETAH,IMMETAH", UbTransactionOpcode::WRITE_WITH_IMMEDIATE, 32,
                           32);
        CheckRequestLayout("MAETAH,NTFETAH", UbTransactionOpcode::WRITE_WITH_NOTIFY, 32, 32);
        CheckRequestLayout("MAETAH", UbTransactionOpcode::WRITE, 64, 64);
        CheckRequestLayout("MAETAH", UbTransactionOpcode::READ, 64, 0);
        CheckRequestLayout("MAETAH", UbTransactionOpcode::ATOMIC_SWAP, 8, 8);
        CheckRequestLayout("MGMTETAH", UbTransactionOpcode::MANAGEMENT, 32, 32);
    }
};

/** Full TA decode preserves response semantics and never reads beyond the wire unit. */
class UbFullTaDecodeBoundsTest : public TestCase
{
  public:
    UbFullTaDecodeBoundsTest()
        : TestCase("full TA decode preserves ATAH fields and bounds every planned header")
    {
    }

    void DoRun() override
    {
        // Independently specified full ATAH bytes: Atomic response, version 2, SV/poison,
        // TASSN=0x1234, status=5/info=17, SQ type=1 and RCID=0xabcde.
        const uint8_t wire[]{0x13, 0x8c, 0x12, 0x34, 0xb1, 0x1a, 0xbc, 0xde};
        for (uint32_t length = 0; length < sizeof(wire); ++length)
        {
            auto packet = Create<Packet>(wire, length);
            UbFullTaHeader decoded;
            auto result = DecodeFullTaBase(packet, decoded);
            NS_TEST_ASSERT_MSG_EQ(static_cast<int>(result.error),
                                  static_cast<int>(UbTaDecodeError::TRUNCATED_BASE),
                                  "a truncated base header is rejected before Deserialize");
            NS_TEST_ASSERT_MSG_EQ(packet->GetSize(), length, "rejection does not consume bytes");
        }
        auto packet = Create<Packet>(wire, sizeof(wire));
        UbFullTaHeader decoded;
        NS_TEST_ASSERT_MSG_EQ(DecodeFullTaBase(packet, decoded).IsSuccess(), true,
                              "a complete response header decodes");
        NS_TEST_ASSERT_MSG_EQ(decoded.isResponse, true, "ATAH retains its response type");
        NS_TEST_ASSERT_MSG_EQ(decoded.response.GetTaVersion(), 2, "TA version is retained");
        NS_TEST_ASSERT_MSG_EQ(decoded.response.GetSV(), 1, "SV is retained");
        NS_TEST_ASSERT_MSG_EQ(decoded.response.GetPoison(), true, "poison is retained");
        NS_TEST_ASSERT_MSG_EQ(decoded.response.GetRspStatus(), 5, "status is retained");
        NS_TEST_ASSERT_MSG_EQ(decoded.response.GetRspInfo(), 17, "aggregation info is retained");
        NS_TEST_ASSERT_MSG_EQ(decoded.response.GetIniRcId(), 0xabcdeu, "20-bit RCID is retained");

        const auto* rule = FindTransactionRule(UbTransactionOpcode::WRITE_WITH_BE);
        NS_TEST_ASSERT_MSG_NE(rule, nullptr, "existing BE request rule");
        const auto plan = UbTaHeaderPlan::ForRequest(*rule, {});
        for (uint32_t payload : {64u, 128u, 256u, 512u, 1024u})
        {
            // Use production Header primitives, independently of the new full encoder.
            UbMAExtTah ma;
            ma.SetLength(payload);
            ma.SetVirtualAddress(0x12340000);
            auto complete = Create<Packet>(payload / 8);
            complete->AddHeader(ma);
            for (uint32_t length = 0; length < complete->GetSize(); ++length)
            {
                auto shortPacket = complete->CreateFragment(0, length);
                UbMAExtTah memory;
                UbTransactionFields fields;
                auto result = DecodeFullTaExtensions(shortPacket, plan, memory, fields);
                NS_TEST_ASSERT_MSG_EQ(result.IsSuccess(), false,
                                      "truncated MAETAH or variable BEETAH must fail");
            }
            UbMAExtTah memory;
            UbTransactionFields fields;
            NS_TEST_ASSERT_MSG_EQ(
                DecodeFullTaExtensions(complete, plan, memory, fields).IsSuccess(), true,
                "complete variable-size BEETAH decodes");
            NS_TEST_ASSERT_MSG_EQ(fields.byteEnable->bytes.size(), payload / 8,
                                  "BEETAH consumes exactly its declared width");
            NS_TEST_ASSERT_MSG_EQ(complete->GetSize(), 0u, "no header bytes leak as payload");
        }
    }
};

class UbTransactionExtensionCodecSuite : public TestSuite
{
  public:
    UbTransactionExtensionCodecSuite()
        : TestSuite("unified-bus-transaction-header-codecs", Type::UNIT)
    {
        AddTestCase(new UbFullTaDecodeBoundsTest, TestCase::Duration::QUICK);
        AddTestCase(new UbTransactionExtensionCodecTest, TestCase::Duration::QUICK);
        AddTestCase(new UbTaHeaderPlanTest, TestCase::Duration::QUICK);
        AddTestCase(new UbCtpRuleDrivenLayoutTest, TestCase::Duration::QUICK);
    }
} g_ubTransactionExtensionCodecSuite;
} // namespace ns3
