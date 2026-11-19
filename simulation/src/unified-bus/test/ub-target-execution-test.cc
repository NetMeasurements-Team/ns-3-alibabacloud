// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/test.h"

#include "ub-node-test-fixture.h"

#include "ns3/boolean.h"
#include "ns3/config.h"
#include "ns3/global-value.h"
#include "ns3/simulator.h"
#include "ns3/ub-congestion-control.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-function.h"
#include "ns3/ub-link.h"
#include "ns3/ub-tp-connection-manager.h"
#include "ns3/ub-transaction-rules.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-transport-channel-manager.h"
#include "ns3/ub-transport.h"

namespace ns3
{
namespace
{
constexpr uint32_t kTargetExecutionSenderTpn = 101;
constexpr uint32_t kTargetExecutionReceiverTpn = 202;
constexpr uint32_t kTargetExecutionPriority = 7;
constexpr uint32_t kTargetExecutionJettyNum = 3;

/** One direct-link RTP pair that can carry a real inbound request to a target executor. */
struct TargetExecutionPair
{
    Ptr<Node> sender;
    Ptr<Node> receiver;
    Ptr<UbPort> senderPort;
    Ptr<UbPort> receiverPort;
    Ptr<UbTransportChannel> senderTp;
    Ptr<UbTransportChannel> receiverTp;
    Ptr<UbFunction> senderFunction;
    Ptr<UbTransaction> senderTransaction;
    Ptr<UbTransaction> receiverTransaction;
    Ptr<UbJetty> senderJetty;
};

TargetExecutionPair
BuildTargetExecutionPair()
{
    Config::Reset();
    GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
    TargetExecutionPair pair;
    pair.sender = CreateObject<Node>(0);
    pair.receiver = CreateObject<Node>(0);
    InitNode(pair.sender, UB_DEVICE, 1);
    InitNode(pair.receiver, UB_DEVICE, 1);
    pair.senderPort = DynamicCast<UbPort>(pair.sender->GetDevice(0));
    pair.receiverPort = DynamicCast<UbPort>(pair.receiver->GetDevice(0));

    Ptr<UbLink> link = CreateObject<UbLink>();
    pair.senderPort->Attach(link);
    pair.receiverPort->Attach(link);
    AddShortestRoute(pair.sender,
                     pair.receiver->GetId(),
                     pair.receiverPort->GetIfIndex(),
                     pair.senderPort->GetIfIndex());
    AddShortestRoute(pair.receiver,
                     pair.sender->GetId(),
                     pair.senderPort->GetIfIndex(),
                     pair.receiverPort->GetIfIndex());

    Ptr<UbController> senderCtrl = pair.sender->GetObject<UbController>();
    Ptr<UbController> receiverCtrl = pair.receiver->GetObject<UbController>();
    Connection connection;
    connection.ownerNodeId = pair.sender->GetId();
    connection.node1 = pair.sender->GetId();
    connection.port1 = pair.senderPort->GetIfIndex();
    connection.tpn1 = kTargetExecutionSenderTpn;
    connection.node2 = pair.receiver->GetId();
    connection.port2 = pair.receiverPort->GetIfIndex();
    connection.tpn2 = kTargetExecutionReceiverTpn;
    connection.priority = kTargetExecutionPriority;
    connection.metrics = 0;
    senderCtrl->GetTpConnManager()->AddUnilateralConnection(connection, pair.sender->GetId());
    receiverCtrl->GetTpConnManager()->AddUnilateralConnection(connection, pair.receiver->GetId());
    senderCtrl->GetTransportChannelManager()->CreateTp(pair.sender->GetId(),
                                                       pair.receiver->GetId(),
                                                       pair.senderPort->GetIfIndex(),
                                                       pair.receiverPort->GetIfIndex(),
                                                       kTargetExecutionPriority,
                                                       kTargetExecutionSenderTpn,
                                                       kTargetExecutionReceiverTpn,
                                                       UbCongestionControl::Create(UB_DEVICE));
    receiverCtrl->GetTransportChannelManager()->CreateTp(pair.receiver->GetId(),
                                                         pair.sender->GetId(),
                                                         pair.receiverPort->GetIfIndex(),
                                                         pair.senderPort->GetIfIndex(),
                                                         kTargetExecutionPriority,
                                                         kTargetExecutionReceiverTpn,
                                                         kTargetExecutionSenderTpn,
                                                         UbCongestionControl::Create(UB_DEVICE));

    pair.senderTp = senderCtrl->GetTransportChannelManager()->GetTp(kTargetExecutionSenderTpn);
    pair.receiverTp =
        receiverCtrl->GetTransportChannelManager()->GetTp(kTargetExecutionReceiverTpn);
    pair.senderTransaction = senderCtrl->GetUbTransaction();
    pair.receiverTransaction = receiverCtrl->GetUbTransaction();

    pair.senderFunction = senderCtrl->GetUbFunction();
    pair.senderFunction->CreateJetty(pair.sender->GetId(),
                                     pair.receiver->GetId(),
                                     kTargetExecutionJettyNum);
    NS_ABORT_MSG_IF(!pair.senderTransaction->JettyBindTp(pair.sender->GetId(),
                                                         pair.receiver->GetId(),
                                                         kTargetExecutionJettyNum,
                                                         false,
                                                         {kTargetExecutionSenderTpn}),
                    "sender Jetty must bind to the direct TP");
    pair.senderJetty = pair.senderFunction->GetJetty(kTargetExecutionJettyNum);
    return pair;
}

/** Facts of the request a registered executor observed on the real inbound path. */
struct ObservedTargetRequest
{
    uint32_t calls{0};
    UbTransactionOpcode opcode{UbTransactionOpcode::MAX_OPCODE};
    UbTransactionType type{UbTransactionType::MEMORY};
    uint32_t logicalBytes{0};
    uint32_t payloadBytes{0};
    bool hasResolved{false};
    bool hasMessageTarget{false};
    bool hasImmediate{false};
    bool hasNotify{false};
    bool hasManagement{false};
    bool hasMemoryAccess{false};
    uint32_t managementPduLength{0};
};
} // namespace

/**
 * D7: a real carrier inbound request must reach the registered executor through the common target
 * entry, and its response must travel back to the initiator as a normal transaction completion.
 */
class UbTargetExecutionInboundPathTest : public TestCase
{
  public:
    UbTargetExecutionInboundPathTest()
        : TestCase("all four transaction types reach the executor and complete the initiator")
    {
    }

  private:
    static UbTransactionType RuleTypeOf(UbTransactionOpcode opcode)
    {
        const auto* rule = FindTransactionRule(opcode);
        return rule == nullptr ? UbTransactionType::MEMORY : rule->type;
    }

    void CheckOpcode(UbTransactionOpcode opcode, uint32_t size)
    {
        TargetExecutionPair pair = BuildTargetExecutionPair();
        m_observed = ObservedTargetRequest{};
        NS_TEST_ASSERT_MSG_EQ(
            pair.receiverTransaction->SetTargetExecutor(UbTargetExecutor(
                [this](Ptr<const UbWqeSegment> request,
                       const UbTransactionRule& rule,
                       UbTargetCompletion done) {
                    ++m_observed.calls;
                    m_observed.opcode = request->GetType();
                    m_observed.type = rule.type;
                    m_observed.logicalBytes = request->GetLogicalBytes();
                    m_observed.payloadBytes = request->GetPayloadBytes();
                    m_observed.hasResolved = request->HasTransactionResolved();
                    const UbTransactionFields& fields = request->GetTransactionFields();
                    m_observed.hasMessageTarget = fields.messageTarget.has_value();
                    m_observed.hasImmediate = fields.immediate.has_value();
                    m_observed.hasNotify = fields.notify.has_value();
                    m_observed.hasManagement = fields.management.has_value();
                    m_observed.hasMemoryAccess = fields.memoryAccess.has_value();
                    m_observed.managementPduLength =
                        fields.management ? fields.management->pduLength : 0;
                    done(UbWorkExecutionResult{});
                })),
            true,
            "target executor registration");

        m_completions = 0;
        m_responseCount = 0;
        pair.senderJetty->SetClientCallback(
            MakeCallback(&UbTargetExecutionInboundPathTest::OnCompletion, this));
        pair.senderTp->TraceConnectWithoutContext(
            "LastPacketReceivesNotify",
            MakeCallback(&UbTargetExecutionInboundPathTest::OnResponsePacket, this));

        Ptr<UbWqe> wqe = pair.senderFunction->CreateWqe(pair.sender->GetId(),
                                                        pair.receiver->GetId(),
                                                        size,
                                                        700 + static_cast<uint32_t>(opcode),
                                                        opcode);
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<uint32_t>(
                pair.senderFunction->SubmitWqe(wqe, kTargetExecutionJettyNum).status),
            static_cast<uint32_t>(UbWqeSubmissionStatus::ACCEPTED),
            "the request is submitted through the production path");

        Simulator::Stop(MilliSeconds(1));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(m_observed.calls, 1u, "the executor is invoked exactly once");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint32_t>(m_observed.opcode),
                              static_cast<uint32_t>(opcode),
                              "the executor sees the request opcode");
        NS_TEST_ASSERT_MSG_EQ(m_observed.hasResolved,
                              true,
                              "the inbound request carries its cached normalized result");
        NS_TEST_ASSERT_MSG_EQ(m_completions, 1u, "the initiator completes exactly once");
        NS_TEST_ASSERT_MSG_EQ(m_responseCount,
                              1u,
                              "the response sink delivers one response packet");

        switch (RuleTypeOf(opcode))
        {
        case UbTransactionType::MEMORY:
            NS_TEST_ASSERT_MSG_EQ(m_observed.hasMemoryAccess,
                                  true,
                                  "memory requests carry their normalized MAETAH fields");
            break;
        case UbTransactionType::MESSAGE:
        {
            NS_TEST_ASSERT_MSG_EQ(m_observed.hasMessageTarget,
                                  true,
                                  "message requests carry their required MTETAH fields");
            const bool expectsImmediate = opcode == UbTransactionOpcode::SEND_WITH_IMMEDIATE ||
                                          opcode == UbTransactionOpcode::WRITE_WITH_IMMEDIATE;
            NS_TEST_ASSERT_MSG_EQ(static_cast<uint32_t>(m_observed.hasImmediate),
                                  static_cast<uint32_t>(expectsImmediate),
                                  "immediate requests carry their required IMMETAH fields");
            break;
        }
        case UbTransactionType::MAINTENANCE:
            NS_TEST_ASSERT_MSG_EQ(m_observed.hasMemoryAccess,
                                  true,
                                  "maintenance requests carry their normalized MAETAH fields");
            break;
        case UbTransactionType::MANAGEMENT:
            NS_TEST_ASSERT_MSG_EQ(m_observed.hasManagement,
                                  true,
                                  "management requests carry their required MGMTETAH fields");
            NS_TEST_ASSERT_MSG_EQ(m_observed.managementPduLength,
                                  size,
                                  "the defaulted management PDU length follows the payload");
            break;
        }
        Simulator::Destroy();
    }

    void OnCompletion(uint32_t, uint32_t)
    {
        ++m_completions;
    }

    void OnResponsePacket(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
    {
        ++m_responseCount;
    }

    void DoRun() override
    {
        // Memory, message, maintenance, and management requests all use the same target entry.
        CheckOpcode(UbTransactionOpcode::WRITE, 64);
        CheckOpcode(UbTransactionOpcode::READ, 64);
        CheckOpcode(UbTransactionOpcode::ATOMIC_SWAP, 8);
        CheckOpcode(UbTransactionOpcode::SEND, 64);
        CheckOpcode(UbTransactionOpcode::SEND_WITH_IMMEDIATE, 64);
        CheckOpcode(UbTransactionOpcode::WRITE_WITH_IMMEDIATE, 64);
        CheckOpcode(UbTransactionOpcode::PREFETCH_TGT, 64);
        CheckOpcode(UbTransactionOpcode::MANAGEMENT, 64);
    }

    ObservedTargetRequest m_observed;
    uint32_t m_completions{0};
    uint32_t m_responseCount{0};
};

/** D7: the single target execution delay controls the executor invocation instant. */
class UbTargetExecutionDelayTimingTest : public TestCase
{
  public:
    UbTargetExecutionDelayTimingTest()
        : TestCase("target execution delay moves the executor call by exactly its value")
    {
    }

  private:
    void CheckDelay(Time delay)
    {
        TargetExecutionPair pair = BuildTargetExecutionPair();
        NS_TEST_ASSERT_MSG_EQ(pair.receiverTransaction->SetTargetExecutionDelay(NanoSeconds(-1)),
                              false,
                              "a strictly negative target execution delay is rejected");
        NS_TEST_ASSERT_MSG_EQ(pair.receiverTransaction->SetTargetExecutionDelay(delay),
                              true,
                              "target execution delay before the first request");
        m_arrival = Seconds(0);
        m_executorCall = Seconds(0);
        m_completed = false;
        m_completedDuringArrival = false;
        pair.receiverTp->TraceConnectWithoutContext(
            "WqeSegmentCompletesNotify",
            MakeCallback(&UbTargetExecutionDelayTimingTest::OnInboundComplete, this));
        NS_TEST_ASSERT_MSG_EQ(
            pair.receiverTransaction->SetTargetExecutor(UbTargetExecutor(
                [this](Ptr<const UbWqeSegment>, const UbTransactionRule&, UbTargetCompletion done) {
                    m_executorCall = Simulator::Now();
                    done(UbWorkExecutionResult{});
                })),
            true,
            "target executor registration");
        pair.senderJetty->SetClientCallback(
            MakeCallback(&UbTargetExecutionDelayTimingTest::OnCompletion, this));

        Ptr<UbWqe> wqe = pair.senderFunction->CreateWqe(pair.sender->GetId(),
                                                        pair.receiver->GetId(),
                                                        64,
                                                        800,
                                                        UbTransactionOpcode::WRITE);
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<uint32_t>(
                pair.senderFunction->SubmitWqe(wqe, kTargetExecutionJettyNum).status),
            static_cast<uint32_t>(UbWqeSubmissionStatus::ACCEPTED),
            "the request is submitted through the production path");

        Simulator::Stop(MilliSeconds(1));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(m_completed, true, "the request completes on the initiator");
        NS_TEST_ASSERT_MSG_EQ(m_executorCall - m_arrival,
                              delay,
                              "the executor runs exactly one target execution delay after arrival");
        NS_TEST_ASSERT_MSG_EQ(m_completedDuringArrival,
                              false,
                              "target completion is asynchronous, never inline with the arrival");
        NS_TEST_ASSERT_MSG_EQ(
            pair.receiverTransaction->SetAttributeFailSafe("TargetExecutionDelay",
                                                            TimeValue(NanoSeconds(7))),
            false,
            "target execution delay attribute is rejected after the first request");
        Simulator::Destroy();
    }

    void OnInboundComplete(uint32_t, uint32_t, uint32_t)
    {
        m_arrival = Simulator::Now();
        m_completedDuringArrival = m_completed;
    }

    void OnCompletion(uint32_t, uint32_t)
    {
        m_completed = true;
    }

    void DoRun() override
    {
        CheckDelay(NanoSeconds(0));
        CheckDelay(NanoSeconds(500));
    }

    Time m_arrival{Seconds(0)};
    Time m_executorCall{Seconds(0)};
    bool m_completed{false};
    bool m_completedDuringArrival{false};
};

/**
 * D7: the default executor completes asynchronously, a deferred completion does not publish its
 * response early, and a duplicated completion yields exactly one response and one completion.
 */
class UbTargetExecutorAsyncAndDedupTest : public TestCase
{
  public:
    UbTargetExecutorAsyncAndDedupTest()
        : TestCase("target completion is asynchronous, deferred, and deduplicated")
    {
    }

  private:
    void CheckDefaultExecutorIsAsynchronous()
    {
        TargetExecutionPair pair = BuildTargetExecutionPair();
        m_completed = false;
        m_responseCount = 0;
        m_completedAtDelivery = false;
        pair.receiverTp->TraceConnectWithoutContext(
            "WqeSegmentCompletesNotify",
            MakeCallback(&UbTargetExecutorAsyncAndDedupTest::OnInboundComplete, this));
        pair.senderJetty->SetClientCallback(
            MakeCallback(&UbTargetExecutorAsyncAndDedupTest::OnCompletion, this));
        pair.senderTp->TraceConnectWithoutContext(
            "LastPacketReceivesNotify",
            MakeCallback(&UbTargetExecutorAsyncAndDedupTest::OnResponsePacket, this));

        Ptr<UbWqe> wqe = pair.senderFunction->CreateWqe(pair.sender->GetId(),
                                                        pair.receiver->GetId(),
                                                        64,
                                                        900,
                                                        UbTransactionOpcode::WRITE);
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<uint32_t>(
                pair.senderFunction->SubmitWqe(wqe, kTargetExecutionJettyNum).status),
            static_cast<uint32_t>(UbWqeSubmissionStatus::ACCEPTED),
            "the request is submitted through the production path");

        Simulator::Stop(MilliSeconds(1));
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(m_completed, true, "the default executor completes the WQE");
        NS_TEST_ASSERT_MSG_EQ(m_responseCount, 1u, "the default executor publishes one response");
        NS_TEST_ASSERT_MSG_EQ(m_completedAtDelivery,
                              false,
                              "the default executor completes asynchronously, not inline");
        Simulator::Destroy();
    }

    void CheckDuplicatedCompletion()
    {
        TargetExecutionPair pair = BuildTargetExecutionPair();
        NS_TEST_ASSERT_MSG_EQ(
            pair.receiverTransaction->SetTargetExecutor(UbTargetExecutor(
                [](Ptr<const UbWqeSegment>, const UbTransactionRule&, UbTargetCompletion done) {
                    done(UbWorkExecutionResult{});
                    done(UbWorkExecutionResult{});
                })),
            true,
            "target executor registration");
        m_completions = 0;
        m_responseCount = 0;
        pair.senderJetty->SetClientCallback(
            MakeCallback(&UbTargetExecutorAsyncAndDedupTest::OnCompletion, this));
        pair.senderTp->TraceConnectWithoutContext(
            "LastPacketReceivesNotify",
            MakeCallback(&UbTargetExecutorAsyncAndDedupTest::OnResponsePacket, this));

        Ptr<UbWqe> wqe = pair.senderFunction->CreateWqe(pair.sender->GetId(),
                                                        pair.receiver->GetId(),
                                                        64,
                                                        901,
                                                        UbTransactionOpcode::WRITE);
        NS_TEST_ASSERT_MSG_EQ(
            static_cast<uint32_t>(
                pair.senderFunction->SubmitWqe(wqe, kTargetExecutionJettyNum).status),
            static_cast<uint32_t>(UbWqeSubmissionStatus::ACCEPTED),
            "the request is submitted through the production path");

        Simulator::Stop(MilliSeconds(1));
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(m_completions, 1u, "a duplicated completion completes the WQE once");
        NS_TEST_ASSERT_MSG_EQ(m_responseCount,
                              1u,
                              "a duplicated completion publishes exactly one response");

        // The executor configuration stays frozen once target execution has started.
        NS_TEST_ASSERT_MSG_EQ(
            pair.receiverTransaction->SetTargetExecutor(UbTargetExecutor(
                [](Ptr<const UbWqeSegment>, const UbTransactionRule&, UbTargetCompletion done) {
                    done(UbWorkExecutionResult{});
                })),
            false,
            "executor registration is rejected after the first request executed");
        NS_TEST_ASSERT_MSG_EQ(pair.receiverTransaction->SetTargetExecutionDelay(NanoSeconds(7)),
                              false,
                              "target execution delay is rejected after the first request");
        Simulator::Destroy();
    }

    void OnInboundComplete(uint32_t, uint32_t, uint32_t)
    {
        m_completedAtDelivery = m_completed;
    }

    void OnCompletion(uint32_t, uint32_t)
    {
        m_completed = true;
        ++m_completions;
    }

    void OnResponsePacket(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
    {
        ++m_responseCount;
    }

    void DoRun() override
    {
        CheckDefaultExecutorIsAsynchronous();
        CheckDuplicatedCompletion();
    }

    bool m_completed{false};
    bool m_completedAtDelivery{false};
    uint32_t m_completions{0};
    uint32_t m_responseCount{0};
};

class UbTargetExecutionSeamSuite : public TestSuite
{
  public:
    UbTargetExecutionSeamSuite() : TestSuite("unified-bus-target-execution", Type::UNIT)
    {
        AddTestCase(new UbTargetExecutionInboundPathTest, TestCase::Duration::QUICK);
        AddTestCase(new UbTargetExecutionDelayTimingTest, TestCase::Duration::QUICK);
        AddTestCase(new UbTargetExecutorAsyncAndDedupTest, TestCase::Duration::QUICK);
    }
} g_ubTargetExecutionSeamSuite;
} // namespace ns3
