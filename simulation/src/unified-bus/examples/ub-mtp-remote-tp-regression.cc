#include "ns3/command-line.h"
#include "ns3/core-module.h"
#include "ns3/mpi-interface.h"
#include "ns3/mtp-interface.h"
#include "ns3/network-module.h"
#include "ns3/node.h"
#include "ns3/packet.h"
#include "ns3/ub-congestion-control.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-datatype.h"
#include "ns3/ub-function.h"
#include "ns3/ub-link.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-port.h"
#include "ns3/ub-remote-link.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-tp-connection-manager.h"
#include "ns3/ub-utils.h"

using namespace ns3;
namespace {
constexpr uint32_t kJettyNum = 0;
constexpr uint32_t kTaskId = 1;
constexpr uint32_t kSenderTpn = 100;
constexpr uint32_t kReceiverTpn = 200;
constexpr uint32_t kRemoteLinkDelayNs = 10;
constexpr uint32_t kTaskStartDelayNs = 10;
constexpr auto kTpPriority = UB_PRIORITY_DEFAULT;

enum class FlowControlMode
{
    NONE,
    CBFC,
};

struct HybridTpTopology
{
    Ptr<Node> device0;
    Ptr<Node> switch0;
    Ptr<Node> switch1;
    Ptr<Node> device1;
    Ptr<UbPort> device0Port;
    Ptr<UbPort> switch0DevicePort;
    Ptr<UbPort> switch0RemotePort;
    Ptr<UbPort> switch1RemotePort;
    Ptr<UbPort> switch1DevicePort;
    Ptr<UbPort> device1Port;
    Ptr<UbRemoteLink> remoteLink;
};

bool g_test = false;
bool g_senderObservedAck = false;
bool g_receiverObservedData = false;
bool g_senderTaskComplete = false;
bool g_enableCbfc = false;
bool g_destroyAfterAdmission = false;
bool g_closeStarted = false;
bool g_newWorkRejected = false;
bool g_jettyDestroyPending = false;
bool g_connectionDestroyed = false;
bool g_completionBeforeDestroy = false;
Ptr<Node> g_sender;
Ptr<Node> g_receiver;

void
TestLog(const std::string& message)
{
    if (g_test)
    {
        std::cout << "TEST " << message << std::endl;
    }
}

FlowControlMode
ParseFlowControlMode(const std::string& mode)
{
    if (mode == "none")
    {
        return FlowControlMode::NONE;
    }
    if (mode == "cbfc")
    {
        return FlowControlMode::CBFC;
    }
    NS_ABORT_MSG("Unsupported flow-control mode: " << mode);
}

void
ObserveSenderTpRecv(uint32_t,
                    uint32_t,
                    uint32_t,
                    uint32_t,
                    uint32_t srcTpn,
                    uint32_t dstTpn,
                    PacketType type,
                    uint32_t size,
                    uint32_t,
                    std::string,
                    UbPacketTraceTag)
{
    if (type != PacketType::ACK || srcTpn != kReceiverTpn || dstTpn != kSenderTpn)
    {
        return;
    }

    if (!g_senderObservedAck)
    {
        g_senderObservedAck = true;
        TestLog("Sender observed ack");
    }
}

void
ObserveReceiverTpRecv(uint32_t,
                      uint32_t,
                      uint32_t,
                      uint32_t,
                      uint32_t srcTpn,
                      uint32_t dstTpn,
                      PacketType type,
                      uint32_t size,
                      uint32_t,
                      std::string,
                      UbPacketTraceTag)
{
    if (type != PacketType::PACKET || srcTpn != kSenderTpn || dstTpn != kReceiverTpn)
    {
        return;
    }

    if (!g_receiverObservedData)
    {
        g_receiverObservedData = true;
        TestLog("Receiver observed data");
    }
}

void
OnTpTaskCompleted(uint32_t taskId, uint32_t jettyNum)
{
    g_senderTaskComplete = true;
    TestLog("Task complete");
}

void
OnTpWqeCompleted(UbWqeCompletion completion)
{
    g_senderTaskComplete = completion.outcome == UbWorkOutcome::COMPLETED;
    TestLog("Task complete");
}

void
OnConnectionDestroyed(UbLifecycleResourceIdentity, UbDestroyStatus status)
{
    g_completionBeforeDestroy = g_senderTaskComplete;
    g_connectionDestroyed = status == UbDestroyStatus::COMPLETED;
    TestLog("Connection destroyed");
}

void
BeginConnectionClose()
{
    Ptr<UbController> controller = g_sender->GetObject<UbController>();
    Ptr<UbFunction> function = controller->GetUbFunction();
    const UbDestroyStatus connectionStatus = controller->GetTpConnManager()->DestroyConnection(
        g_sender->GetId(),
        kSenderTpn,
        UbDestroyReason::EXPLICIT,
        MakeCallback(&OnConnectionDestroyed));
    NS_ABORT_MSG_IF(connectionStatus != UbDestroyStatus::PENDING,
                    "Admitted work must keep connection destroy pending");

    Ptr<UbWqe> rejected = function->CreateWqe(g_sender->GetId(),
                                              g_receiver->GetId(),
                                              UB_MTU_BYTE,
                                              kTaskId + 1,
                                              UbTransactionOpcode::WRITE);
    g_newWorkRejected =
        function->SubmitWqe(rejected, kJettyNum).status != UbWqeSubmissionStatus::ACCEPTED;
    const UbDestroyStatus jettyStatus = function->DestroyJetty(kJettyNum);
    g_jettyDestroyPending = jettyStatus == UbDestroyStatus::PENDING;
    TestLog("Connection close started");
}

void
ObserveWqeSegmentAdmission(uint32_t, uint32_t taskId, uint32_t)
{
    if (!g_destroyAfterAdmission || taskId != kTaskId || g_closeStarted)
    {
        return;
    }
    g_closeStarted = true;
    Simulator::ScheduleNow(&BeginConnectionClose);
}

Ptr<UbPort>
CreatePort(Ptr<Node> node)
{
    Ptr<UbPort> port = CreateObject<UbPort>();
    port->SetAddress(Mac48Address::Allocate());
    node->AddDevice(port);
    return port;
}

void
InitNode(Ptr<Node> node, UbNodeType_t nodeType, uint32_t portCount)
{
    Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
    node->AggregateObject(sw);
    sw->SetNodeType(nodeType);
    if (g_enableCbfc && nodeType == UB_SWITCH)
    {
        sw->SetAttribute("FlowControl", EnumValue(FcType::CBFC));
    }

    if (nodeType == UB_DEVICE)
    {
        Ptr<UbController> controller = CreateObject<UbController>();
        node->AggregateObject(controller);
        controller->CreateUbFunction();
        controller->CreateUbTransaction();
    }

    for (uint32_t index = 0; index < portCount; ++index)
    {
        CreatePort(node);
    }

    sw->Init();
    Ptr<UbCongestionControl> congestionCtrl = UbCongestionControl::Create(UB_SWITCH);
    congestionCtrl->OnSwitchAttached(sw);
}

void
AddShortestRoute(Ptr<Node> node, uint32_t destNodeId, uint32_t destPortId, uint16_t outPort)
{
    std::vector<uint16_t> outPorts = {outPort};
    Ptr<UbRoutingProcess> routing = node->GetObject<UbSwitch>()->GetRoutingProcess();
    routing->GetRoutingTable().AddShortestRoute(NodeIdToIp(destNodeId).Get(), outPorts);
    routing->GetRoutingTable().AddShortestRoute(NodeIdToIp(destNodeId, destPortId).Get(), outPorts);
}

HybridTpTopology
BuildTpTopology()
{
    HybridTpTopology topo;
    topo.device0 = CreateObject<Node>(0);
    topo.switch0 = CreateObject<Node>(0);
    topo.switch1 = CreateObject<Node>(1);
    topo.device1 = CreateObject<Node>(1);

    InitNode(topo.device0, UB_DEVICE, 1);
    InitNode(topo.switch0, UB_SWITCH, 2);
    InitNode(topo.switch1, UB_SWITCH, 2);
    InitNode(topo.device1, UB_DEVICE, 1);

    topo.device0Port = DynamicCast<UbPort>(topo.device0->GetDevice(0));
    topo.switch0DevicePort = DynamicCast<UbPort>(topo.switch0->GetDevice(0));
    topo.switch0RemotePort = DynamicCast<UbPort>(topo.switch0->GetDevice(1));
    topo.switch1RemotePort = DynamicCast<UbPort>(topo.switch1->GetDevice(0));
    topo.switch1DevicePort = DynamicCast<UbPort>(topo.switch1->GetDevice(1));
    topo.device1Port = DynamicCast<UbPort>(topo.device1->GetDevice(0));

    Ptr<UbLink> leftLink = CreateObject<UbLink>();
    topo.device0Port->Attach(leftLink);
    topo.switch0DevicePort->Attach(leftLink);

    topo.remoteLink = CreateObject<UbRemoteLink>();
    // A positive remote-link delay gives the hybrid runtime valid lookahead. This value belongs to
    // the regression topology and is not a recommended UB link delay.
    topo.remoteLink->SetAttribute("Delay", TimeValue(NanoSeconds(kRemoteLinkDelayNs)));
    topo.switch0RemotePort->Attach(topo.remoteLink);
    topo.switch1RemotePort->Attach(topo.remoteLink);
    topo.switch0RemotePort->EnableMpiReceive();
    topo.switch1RemotePort->EnableMpiReceive();

    Ptr<UbLink> rightLink = CreateObject<UbLink>();
    topo.switch1DevicePort->Attach(rightLink);
    topo.device1Port->Attach(rightLink);

    AddShortestRoute(topo.device0, topo.device1->GetId(), topo.device1Port->GetIfIndex(), topo.device0Port->GetIfIndex());
    AddShortestRoute(topo.switch0, topo.device1->GetId(), topo.device1Port->GetIfIndex(), topo.switch0RemotePort->GetIfIndex());
    AddShortestRoute(topo.switch1, topo.device1->GetId(), topo.device1Port->GetIfIndex(), topo.switch1DevicePort->GetIfIndex());

    AddShortestRoute(topo.device1, topo.device0->GetId(), topo.device0Port->GetIfIndex(), topo.device1Port->GetIfIndex());
    AddShortestRoute(topo.switch1, topo.device0->GetId(), topo.device0Port->GetIfIndex(), topo.switch1RemotePort->GetIfIndex());
    AddShortestRoute(topo.switch0, topo.device0->GetId(), topo.device0Port->GetIfIndex(), topo.switch0DevicePort->GetIfIndex());

    return topo;
}

void
EnqueueTpWqe(Ptr<Node> sender, uint32_t receiverId, uint32_t flowSize)
{
    Ptr<UbController> controller = sender->GetObject<UbController>();
    Ptr<UbFunction> function = controller->GetUbFunction();
    Ptr<UbWqe> wqe = function->CreateWqe(sender->GetId(), receiverId, flowSize, kTaskId);
    function->PushWqeToJetty(wqe, kJettyNum);
    TestLog("TP flow enqueued");
}

void
InstallStaticConnectionViews(const HybridTpTopology& topo)
{
    Ptr<UbController> senderCtrl = topo.device0->GetObject<UbController>();
    Ptr<UbController> receiverCtrl = topo.device1->GetObject<UbController>();

    Connection connection;
    connection.ownerNodeId = topo.device0->GetId();
    connection.node1 = topo.device0->GetId();
    connection.port1 = topo.device0Port->GetIfIndex();
    connection.tpn1 = kSenderTpn;
    connection.node2 = topo.device1->GetId();
    connection.port2 = topo.device1Port->GetIfIndex();
    connection.tpn2 = kReceiverTpn;
    connection.priority = kTpPriority;
    Connection existing;
    if (!senderCtrl->GetTpConnManager()->TryGetLocalConnection(connection.node1,
                                                               connection.tpn1,
                                                               existing))
    {
        senderCtrl->GetTpConnManager()->AddUnilateralConnection(connection, connection.node1);
    }
    if (!receiverCtrl->GetTpConnManager()->TryGetLocalConnection(connection.node2,
                                                                 connection.tpn2,
                                                                 existing))
    {
        receiverCtrl->GetTpConnManager()->AddUnilateralConnection(connection, connection.node2);
    }
}

void
InstallStaticTpEndpoint(uint32_t systemId, const HybridTpTopology& topo)
{
    Ptr<UbController> senderCtrl = topo.device0->GetObject<UbController>();
    Ptr<UbController> receiverCtrl = topo.device1->GetObject<UbController>();

    if (systemId == 0 && !senderCtrl->GetTransportChannelManager()->Contains(kSenderTpn))
    {
        Ptr<UbCongestionControl> senderCc = UbCongestionControl::Create(UB_DEVICE);
        senderCtrl->GetTransportChannelManager()->CreateTp(topo.device0->GetId(),
                             topo.device1->GetId(),
                             topo.device0Port->GetIfIndex(),
                             topo.device1Port->GetIfIndex(),
                             kTpPriority,
                             kSenderTpn,
                             kReceiverTpn,
                             senderCc);
    }

    if (systemId == 1 && !receiverCtrl->GetTransportChannelManager()->Contains(kReceiverTpn))
    {
        Ptr<UbCongestionControl> receiverCc = UbCongestionControl::Create(UB_DEVICE);
        receiverCtrl->GetTransportChannelManager()->CreateTp(topo.device1->GetId(),
                               topo.device0->GetId(),
                               topo.device1Port->GetIfIndex(),
                               topo.device0Port->GetIfIndex(),
                               kTpPriority,
                               kReceiverTpn,
                               kSenderTpn,
                               receiverCc);
    }
}

void
InstallStaticTpPair(const HybridTpTopology& topo)
{
    Ptr<UbController> senderCtrl = topo.device0->GetObject<UbController>();
    Ptr<UbController> receiverCtrl = topo.device1->GetObject<UbController>();

    if (!senderCtrl->GetTransportChannelManager()->Contains(kSenderTpn))
    {
        Ptr<UbCongestionControl> senderCc = UbCongestionControl::Create(UB_DEVICE);
        senderCtrl->GetTransportChannelManager()->CreateTp(topo.device0->GetId(),
                             topo.device1->GetId(),
                             topo.device0Port->GetIfIndex(),
                             topo.device1Port->GetIfIndex(),
                             kTpPriority,
                             kSenderTpn,
                             kReceiverTpn,
                             senderCc);
    }

    if (!receiverCtrl->GetTransportChannelManager()->Contains(kReceiverTpn))
    {
        Ptr<UbCongestionControl> receiverCc = UbCongestionControl::Create(UB_DEVICE);
        receiverCtrl->GetTransportChannelManager()->CreateTp(topo.device1->GetId(),
                               topo.device0->GetId(),
                               topo.device1Port->GetIfIndex(),
                               topo.device0Port->GetIfIndex(),
                               kTpPriority,
                               kReceiverTpn,
                               kSenderTpn,
                               receiverCc);
    }
}

void
PrepareTpFlow(Ptr<Node> sender, uint32_t receiverId, uint32_t flowSize)
{
    Ptr<UbController> controller = sender->GetObject<UbController>();
    Ptr<UbFunction> function = controller->GetUbFunction();
    Ptr<UbTransaction> transaction = controller->GetUbTransaction();

    function->CreateJetty(sender->GetId(), receiverId, kJettyNum);
    std::vector<uint32_t> tpns = {kSenderTpn};
    bool bindOk = transaction->JettyBindTp(sender->GetId(), receiverId, kJettyNum, false, tpns);
    NS_ABORT_MSG_IF(!bindOk, "Failed to bind Jetty to TP");

    Ptr<UbJetty> jetty = function->GetJetty(kJettyNum);
    NS_ABORT_MSG_IF(jetty == nullptr, "Failed to get sender jetty");
    jetty->SetClientCallback(MakeCallback(&OnTpTaskCompleted));

    TestLog("TP mode start");
    Simulator::Schedule(NanoSeconds(1), &EnqueueTpWqe, sender, receiverId, flowSize);
}

void
SetupTpMode(uint32_t systemId, const HybridTpTopology& topo, uint32_t flowSize)
{
    Ptr<UbTransportChannel> senderTp = topo.device0->GetObject<UbController>()->GetTransportChannelManager()->GetTp(kSenderTpn);
    Ptr<UbTransportChannel> receiverTp = topo.device1->GetObject<UbController>()->GetTransportChannelManager()->GetTp(kReceiverTpn);
    NS_ABORT_MSG_IF(senderTp == nullptr, "Failed to get sender TP");
    NS_ABORT_MSG_IF(receiverTp == nullptr, "Failed to get receiver TP");
    senderTp->TraceConnectWithoutContext("TpRecvNotify", MakeCallback(&ObserveSenderTpRecv));
    receiverTp->TraceConnectWithoutContext("TpRecvNotify", MakeCallback(&ObserveReceiverTpRecv));

    if (systemId == 0)
    {
        Simulator::Schedule(NanoSeconds(kTaskStartDelayNs),
                            &PrepareTpFlow,
                            topo.device0,
                            topo.device1->GetId(),
                            flowSize);
    }
}

void
PrepareDestroyTpFlow(Ptr<Node> sender, uint32_t receiverId, uint32_t flowSize)
{
    Ptr<UbController> controller = sender->GetObject<UbController>();
    Ptr<UbFunction> function = controller->GetUbFunction();
    Ptr<UbTransaction> transaction = controller->GetUbTransaction();

    function->CreateJetty(sender->GetId(), receiverId, kJettyNum);
    const bool bindOk = transaction->JettyBindTp(sender->GetId(),
                                                 receiverId,
                                                 kJettyNum,
                                                 false,
                                                 {kSenderTpn});
    NS_ABORT_MSG_IF(!bindOk, "Failed to bind delayed-destroy Jetty to TP");
    Ptr<UbJetty> jetty = function->GetJetty(kJettyNum);
    jetty->SetTaSegmentBytes(UB_WQE_TA_SEGMENT_BYTE / 2);
    jetty->SetWqeCompletionCallback(MakeCallback(&OnTpWqeCompleted));
    controller->GetTransportChannelManager()
        ->GetTp(kSenderTpn)
        ->TraceConnectWithoutContext("WqeSegmentSendsNotify",
                                     MakeCallback(&ObserveWqeSegmentAdmission));

    Ptr<UbWqe> wqe = function->CreateWqe(sender->GetId(),
                                         receiverId,
                                         flowSize,
                                         kTaskId,
                                         UbTransactionOpcode::WRITE);
    NS_ABORT_MSG_IF(function->SubmitWqe(wqe, kJettyNum).status !=
                        UbWqeSubmissionStatus::ACCEPTED,
                    "Failed to submit delayed-destroy WRITE");
    TestLog("TP mode start");
}

void
SetupDestroyTpMode(uint32_t systemId, const HybridTpTopology& topo, uint32_t flowSize)
{
    InstallStaticTpEndpoint(systemId, topo);
    if (systemId == 0)
    {
        Ptr<UbTransportChannel> senderTp = topo.device0->GetObject<UbController>()
                                               ->GetTransportChannelManager()
                                               ->GetTp(kSenderTpn);
        senderTp->SetMaxQueueSizeForTest(1);
        senderTp->TraceConnectWithoutContext("TpRecvNotify", MakeCallback(&ObserveSenderTpRecv));
        Simulator::Schedule(NanoSeconds(kTaskStartDelayNs + kRemoteLinkDelayNs),
                            &PrepareDestroyTpFlow,
                            topo.device0,
                            topo.device1->GetId(),
                            flowSize);
    }
}
} // namespace

int
main(int argc, char* argv[])
{
    bool testing = false;
    bool destroyAfterAdmission = false;
    uint32_t mtpThreads = 2;
    uint32_t flowSize = UB_MTU_BYTE;
    uint32_t stopMs = 20;
    std::string mode = "tp";
    std::string flowControl = "none";

    CommandLine cmd(__FILE__);
    cmd.AddValue("test", "Enable deterministic smoke output", testing);
    cmd.AddValue("destroy-after-admission",
                 "Destroy the TP connection after its first admitted segment",
                 destroyAfterAdmission);
    cmd.AddValue("mtp-threads", "Number of MTP threads to use", mtpThreads);
    cmd.AddValue("mode", "Smoke mode; only 'tp' is supported", mode);
    cmd.AddValue("flow-control", "Flow control mode: none or cbfc", flowControl);
    cmd.AddValue("flow-size", "TP flow size in bytes", flowSize);
    cmd.AddValue("stop-ms", "Simulation stop time in milliseconds", stopMs);
    cmd.Parse(argc, argv);

    g_test = testing;
    g_destroyAfterAdmission = destroyAfterAdmission;
    FlowControlMode flowControlMode = ParseFlowControlMode(flowControl);

    if (mtpThreads < 1)
    {
        mtpThreads = 1;
    }

    if (!destroyAfterAdmission || mtpThreads > 1)
    {
        MtpInterface::Enable(mtpThreads);
    }
    MpiInterface::Enable(&argc, &argv);

    const uint32_t systemId = MpiInterface::GetSystemId();
    const uint32_t systemCount = MpiInterface::GetSize();

    if (systemCount != 2)
    {
        std::cerr << "ub-mtp-remote-tp-regression requires exactly 2 MPI ranks" << std::endl;
        return 1;
    }
    if (mode != "tp")
    {
        if (systemId == 0)
        {
            if (testing)
            {
                TestLog("ERROR interceptor mode has been removed; use tp");
            }
            else
            {
                std::cerr << "interceptor mode has been removed; use tp" << std::endl;
            }
        }
        MpiInterface::Disable();
        return 1;
    }

    if (flowSize == 0)
    {
        std::cerr << "flow-size must be greater than 0" << std::endl;
        return 1;
    }

    Time::SetResolution(Time::NS);
    (void)UbFlowTag::GetTypeId();
    (void)UbPacketTraceTag::GetTypeId();
    // `UB_RECORD_PKT_TRACE` is registered during UbUtils singleton construction.
    (void)utils::UbUtils::Get();
    GlobalValue::Bind("UB_RECORD_PKT_TRACE", BooleanValue(true));
    if (flowControlMode == FlowControlMode::CBFC)
    {
        g_enableCbfc = true;
        TestLog("CBFC enabled");
    }
    HybridTpTopology topo = BuildTpTopology();
    if (destroyAfterAdmission)
    {
        MpiInterface::BoundLookAhead(NanoSeconds(kRemoteLinkDelayNs));
        g_sender = topo.device0;
        g_receiver = topo.device1;
        InstallStaticConnectionViews(topo);
        const uint32_t localNodeId =
            systemId == 0 ? topo.device0->GetId() : topo.device1->GetId();
        Simulator::ScheduleWithContext(localNodeId,
                                       NanoSeconds(1),
                                       &SetupDestroyTpMode,
                                       systemId,
                                       topo,
                                       flowSize);
    }
    else
    {
        InstallStaticTpPair(topo);
    }

    TestLog(std::string("Rank ") + std::to_string(systemId) + " initialized");
    if (!destroyAfterAdmission)
    {
        SetupTpMode(systemId, topo, flowSize);
    }

    Simulator::Stop(MilliSeconds(stopMs));
    Simulator::Run();

    if (destroyAfterAdmission)
    {
        Connection ignored;
        const bool senderViewPresent = topo.device0->GetObject<UbController>()
                                           ->GetTpConnManager()
                                           ->TryGetLocalConnection(
                                               topo.device0->GetId(),
                                               kSenderTpn,
                                               ignored);
        const bool receiverViewPresent = topo.device1->GetObject<UbController>()
                                             ->GetTpConnManager()
                                             ->TryGetLocalConnection(
                                                 topo.device1->GetId(),
                                                 kReceiverTpn,
                                                 ignored);
        const bool localPassed =
            systemId == 0
                ? g_senderObservedAck && g_senderTaskComplete && g_closeStarted &&
                      g_newWorkRejected && g_jettyDestroyPending && g_connectionDestroyed &&
                      g_completionBeforeDestroy && !senderViewPresent
                : !receiverViewPresent;
        int localResult = localPassed ? 1 : 0;
        int globalResult = localResult;
        MPI_Allreduce(&localResult,
                      &globalResult,
                      1,
                      MPI_INT,
                      MPI_MIN,
                      MpiInterface::GetCommunicator());
        if (testing)
        {
            std::cout << "TEST : " << systemId << " : "
                      << (globalResult ? "PASSED" : "FAILED") << " local=" << localResult
                      << " ack=" << g_senderObservedAck << " task=" << g_senderTaskComplete
                      << " close=" << g_closeStarted << " rejected=" << g_newWorkRejected
                      << " destroyed=" << g_connectionDestroyed
                      << " sender-view=" << senderViewPresent
                      << " receiver-view=" << receiverViewPresent << std::endl;
        }
        Simulator::Destroy();
        MpiInterface::Disable();
        if (!globalResult)
        {
            return systemId == 0 ? 4 : 6;
        }
        return 0;
    }

    Simulator::Destroy();
    MpiInterface::Disable();
    if (systemId == 0)
    {
        if (!g_senderObservedAck)
        {
            std::cerr << "Rank 0 did not observe TP ack" << std::endl;
            return 4;
        }
        if (!g_senderTaskComplete)
        {
            std::cerr << "Rank 0 did not complete TP task" << std::endl;
            return 5;
        }
        TestLog("PASS");
    }
    else if (systemId == 1 && !g_receiverObservedData)
    {
        std::cerr << "Rank 1 did not observe TP data" << std::endl;
        return 6;
    }

    return 0;
}
