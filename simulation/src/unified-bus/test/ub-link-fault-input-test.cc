// SPDX-License-Identifier: GPL-2.0-only
//
// Focused suite for the file-driven `link_fault.csv` schedule: strict parsing, topology
// resolution, and scheduling through the public `UbLink::Fail()` action.
#include "ns3/test.h"
#include "ns3/ub-congestion-control.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-link-fault-schedule.h"
#include "ns3/ub-link.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-reliable-rerouting.h"
#include "ns3/ub-port.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-utils.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ns3;

namespace {

class TempScheduleFile
{
  public:
    explicit TempScheduleFile(const std::string& contents)
    {
        static uint32_t counter = 0;
        std::filesystem::create_directories("testpy-output");
        m_path = "testpy-output/ub-link-fault-" + std::to_string(++counter) + ".csv";
        std::ofstream out(m_path, std::ios::binary);
        out << contents;
        out.close();
    }

    ~TempScheduleFile() { std::remove(m_path.c_str()); }

    const std::string& Path() const { return m_path; }

    void TruncateToEmpty() const
    {
        std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
    }

  private:
    std::string m_path;
};

Ptr<UbPort>
CreateFaultTestPort(Ptr<Node> node)
{
    Ptr<UbPort> port = CreateObject<UbPort>();
    port->SetAddress(Mac48Address::Allocate());
    node->AddDevice(port);
    return port;
}

void
InitFaultTestNode(Ptr<Node> node, UbNodeType_t nodeType, uint32_t portCount)
{
    Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
    node->AggregateObject(sw);
    sw->SetNodeType(nodeType);
    if (nodeType == UB_DEVICE)
    {
        Ptr<UbController> controller = CreateObject<UbController>();
        node->AggregateObject(controller);
        controller->CreateUbFunction();
        controller->CreateUbTransaction();
    }
    for (uint32_t index = 0; index < portCount; ++index)
    {
        CreateFaultTestPort(node);
    }
    sw->SetAttribute("FlowControl", EnumValue(FcType::NONE));
    sw->Init();
    Ptr<UbCongestionControl> congestionCtrl = UbCongestionControl::Create(UB_SWITCH);
    congestionCtrl->OnSwitchAttached(sw);
}

Ptr<UbLink>
ConnectFaultTestPorts(Ptr<UbPort> left, Ptr<UbPort> right, bool enableRerouting)
{
    Ptr<UbLink> link = CreateObject<UbLink>();
    link->SetAttribute("Delay", TimeValue(NanoSeconds(20)));
    link->SetAttribute("EnableReliableRerouting", BooleanValue(enableRerouting));
    left->Attach(link);
    right->Attach(link);
    return link;
}

struct ReroutingStateWatcher
{
    Time failureTime;
    uint32_t failureCount{0};
};

void
WatchReroutingState(ReroutingStateWatcher* watcher, const UbReliableReroutingEvent& record)
{
    if (record.event == "state:DETECTION_PENDING")
    {
        ++watcher->failureCount;
        if (watcher->failureTime.IsZero())
        {
            watcher->failureTime = Simulator::Now();
        }
    }
}

/** Two switches with a failed link, an alternate link, and one ordinary host access link. */
struct FaultInputTopology
{
    Ptr<Node> host;
    Ptr<Node> swLeft;
    Ptr<Node> swRight;
    Ptr<UbLink> failedLink;
    Ptr<UbLink> alternateLink;
    Ptr<UbLink> hostLink;

    explicit FaultInputTopology(bool armLinks = true)
    {
        host = CreateObject<Node>(0);
        swLeft = CreateObject<Node>(1);
        swRight = CreateObject<Node>(2);
        InitFaultTestNode(host, UB_DEVICE, 1);
        InitFaultTestNode(swLeft, UB_SWITCH, 3);
        InitFaultTestNode(swRight, UB_SWITCH, 3);

        Ptr<UbPort> h0 = DynamicCast<UbPort>(host->GetDevice(0));
        Ptr<UbPort> l0 = DynamicCast<UbPort>(swLeft->GetDevice(0));
        Ptr<UbPort> l1 = DynamicCast<UbPort>(swLeft->GetDevice(1));
        Ptr<UbPort> l2 = DynamicCast<UbPort>(swLeft->GetDevice(2));
        Ptr<UbPort> r0 = DynamicCast<UbPort>(swRight->GetDevice(0));
        Ptr<UbPort> r1 = DynamicCast<UbPort>(swRight->GetDevice(1));
        Ptr<UbPort> r2 = DynamicCast<UbPort>(swRight->GetDevice(2));
        hostLink = ConnectFaultTestPorts(h0, l0, armLinks);
        Ptr<UbLink> link = CreateObject<UbLink>();
        link->SetAttribute("Delay", TimeValue(NanoSeconds(20)));
        link->SetAttribute("EnableReliableRerouting", BooleanValue(armLinks));
        // Arming captures the immutable configuration when the link completes, so the recovery
        // delays must be in place before the second endpoint attaches.
        link->SetAttribute("FailureDetectionDelay", TimeValue(MicroSeconds(5)));
        link->SetAttribute("LastAckTimeout", TimeValue(MicroSeconds(10)));
        l1->Attach(link);
        r1->Attach(link);
        failedLink = link;
        alternateLink = ConnectFaultTestPorts(l2, r2, armLinks);
        swLeft->GetObject<UbSwitch>()->GetRoutingProcess()->AddShortestRoute(
            utils::NodeIdToIp(swRight->GetId()).Get(), {1, 2});
        swRight->GetObject<UbSwitch>()->GetRoutingProcess()->AddShortestRoute(
            utils::NodeIdToIp(swLeft->GetId()).Get(), {1, 2});
    }
};

/**
 * @brief Strict parsing of the optional case-local schedule file.
 */
class UbLinkFaultParseTest : public TestCase
{
  public:
    UbLinkFaultParseTest()
        : TestCase("Link fault schedule - strict three-column parsing")
    {
    }

  private:
    static bool Parse(const std::string& contents,
                      std::vector<UbLinkFaultScheduleRow>& rows,
                      std::string& error)
    {
        const TempScheduleFile file(contents);
        return TryReadLinkFaultSchedule(file.Path(), rows, error);
    }

    void ExpectRejected(const std::string& contents, const std::string& expectedFragment)
    {
        std::vector<UbLinkFaultScheduleRow> rows;
        std::string error;
        const bool ok = Parse(contents, rows, error);
        NS_TEST_ASSERT_MSG_EQ(ok, false, "malformed schedule is rejected: " + contents);
        NS_TEST_ASSERT_MSG_NE(error.find(expectedFragment),
                              std::string::npos,
                              "diagnostic mentions '" + expectedFragment + "' but was: " + error);
    }

    void DoRun() override
    {
        {
            // Whitespace, blank lines, duplicate and unsorted rows keep file order.
            std::vector<UbLinkFaultScheduleRow> rows;
            std::string error;
            const bool ok = Parse("nodeId,portId,failTimeNs\n"
                                  "\n"
                                  " 1 , 1 , 200000 \n"
                                  "\n"
                                  "2,1,100000\n"
                                  "1,1,300000\n",
                                  rows,
                                  error);
            NS_TEST_ASSERT_MSG_EQ(ok, true, "well-formed schedule parses: " + error);
            NS_TEST_ASSERT_MSG_EQ(rows.size(), static_cast<size_t>(3), "all rows parsed");
            NS_TEST_ASSERT_MSG_EQ(rows[0].nodeId, 1u, "trimmed node id");
            NS_TEST_ASSERT_MSG_EQ(rows[0].portId, 1u, "trimmed port id");
            NS_TEST_ASSERT_MSG_EQ(rows[0].failTimeNs, 200000u, "trimmed fail time");
            NS_TEST_ASSERT_MSG_EQ(rows[0].line, 3u, "line numbers count blank lines");
            NS_TEST_ASSERT_MSG_EQ(rows[2].failTimeNs, 300000u, "file order preserved");
        }
        {
            // Missing file, zero-length file, and header-only file all mean "no failures".
            std::vector<UbLinkFaultScheduleRow> rows;
            std::string error;
            NS_TEST_ASSERT_MSG_EQ(TryReadLinkFaultSchedule("testpy-output/does-not-exist.csv",
                                                           rows,
                                                           error),
                                  true,
                                  "missing file means no scheduled failures");
            NS_TEST_ASSERT_MSG_EQ(rows.empty(), true, "no rows from a missing file");

            const TempScheduleFile empty("");
            empty.TruncateToEmpty();
            NS_TEST_ASSERT_MSG_EQ(TryReadLinkFaultSchedule(empty.Path(), rows, error),
                                  true,
                                  "zero-length file means no scheduled failures");
            NS_TEST_ASSERT_MSG_EQ(rows.empty(), true, "no rows from an empty file");

            NS_TEST_ASSERT_MSG_EQ(Parse("nodeId,portId,failTimeNs\n", rows, error),
                                  true,
                                  "header-only file means no scheduled failures");
            NS_TEST_ASSERT_MSG_EQ(rows.empty(), true, "no rows from a header-only file");
        }

        ExpectRejected("node,port,time\n1,1,1000\n", "header must be exactly");
        ExpectRejected("nodeId,portId,failTimeNs\n1,1\n", "expected exactly 3 fields");
        ExpectRejected("nodeId,portId,failTimeNs\n1,1,1000,extra\n", "expected exactly 3 fields");
        ExpectRejected("nodeId,portId,failTimeNs\n1,,1000\n", "'portId' is empty");
        ExpectRejected("nodeId,portId,failTimeNs\n1,1,\n", "'failTimeNs' is empty");
        ExpectRejected("nodeId,portId,failTimeNs\n1,1,-5\n", "must be a non-negative integer");
        ExpectRejected("nodeId,portId,failTimeNs\n1,1,1.5\n", "must be a non-negative integer");
        ExpectRejected("nodeId,portId,failTimeNs\n1,1,1e6\n", "must be a non-negative integer");
        ExpectRejected("nodeId,portId,failTimeNs\n1,1,12abc\n", "must be a non-negative integer");
        ExpectRejected("nodeId,portId,failTimeNs\n1,1,18446744073709551616\n",
                       "does not fit an unsigned 64-bit integer");
        ExpectRejected("nodeId,portId,failTimeNs\n1,1,9223372036854775808\n",
                       "exceeds the simulator time range");

        // An unreadable directory path is an error, not absence.
        std::vector<UbLinkFaultScheduleRow> rows;
        std::string error;
        NS_TEST_ASSERT_MSG_EQ(TryReadLinkFaultSchedule("scratch", rows, error),
                              false,
                              "an existing unreadable path is an error");
    }
};

/**
 * @brief Topology resolution: endpoint aliasing, eligibility, and the one-link rule.
 */
class UbLinkFaultResolveTest : public TestCase
{
  public:
    UbLinkFaultResolveTest()
        : TestCase("Link fault schedule - topology resolution and one-link rule")
    {
    }

  private:
    static bool Resolve(const std::string& contents,
                        std::vector<UbLinkFaultTarget>& targets,
                        std::string& error)
    {
        std::vector<UbLinkFaultScheduleRow> rows;
        const TempScheduleFile file(contents);
        if (!TryReadLinkFaultSchedule(file.Path(), rows, error))
        {
            return false;
        }
        return TryResolveLinkFaultSchedule(rows, targets, error);
    }

    void ExpectRejected(const std::string& contents, const std::string& fragment)
    {
        std::vector<UbLinkFaultTarget> targets;
        std::string error;
        NS_TEST_ASSERT_MSG_EQ(Resolve(contents, targets, error), false, "row rejected");
        NS_TEST_ASSERT_MSG_NE(error.find(fragment),
                              std::string::npos,
                              "diagnostic mentions '" + fragment + "': " + error);
        NS_TEST_ASSERT_MSG_EQ(targets.empty(), true, "no targets on a rejected schedule");
    }

    void DoRun() override
    {
        {
            FaultInputTopology topo;
            std::vector<UbLinkFaultTarget> targets;
            std::string error;
            // Either end identifies the same physical link; unsorted rows stay usable.
            const bool ok = Resolve("nodeId,portId,failTimeNs\n"
                                    "2,1,300000\n"
                                    "1,1,100000\n",
                                    targets,
                                    error);
            NS_TEST_ASSERT_MSG_EQ(ok, true, "both ends resolve: " + error);
            NS_TEST_ASSERT_MSG_EQ(targets.size(), static_cast<size_t>(2), "both rows resolved");
            const bool firstIsFailedLink =
                PeekPointer(targets[0].link) == PeekPointer(topo.failedLink);
            NS_TEST_ASSERT_MSG_EQ(firstIsFailedLink, true, "first row binds the failed link");
            const bool secondIsFailedLink =
                PeekPointer(targets[1].link) == PeekPointer(topo.failedLink);
            NS_TEST_ASSERT_MSG_EQ(secondIsFailedLink,
                                  true,
                                  "opposite-end row binds the same physical link");
            NS_TEST_ASSERT_MSG_EQ(targets[0].failTime,
                                  NanoSeconds(100000),
                                  "earliest action sorts first");

            ExpectRejected("nodeId,portId,failTimeNs\n9,0,1000\n", "does not exist in topology.csv");
            ExpectRejected("nodeId,portId,failTimeNs\n1,7,1000\n", "does not exist in topology.csv");
            ExpectRejected("nodeId,portId,failTimeNs\n0,0,1000\n",
                           "not an eligible armed switch-to-switch link");
            ExpectRejected("nodeId,portId,failTimeNs\n1,1,1000\n1,2,2000\n",
                           "different physical link");
            Simulator::Destroy();
        }

        {
            // Without the feature enabled, an otherwise eligible link is rejected.
            FaultInputTopology topo(false);
            ExpectRejected("nodeId,portId,failTimeNs\n1,1,1000\n",
                           "not an eligible armed switch-to-switch link");
            Simulator::Destroy();
        }
    }
};

/**
 * @brief Scheduling uses absolute simulation time and repeated actions are idempotent.
 */
class UbLinkFaultScheduleTest : public TestCase
{
  public:
    UbLinkFaultScheduleTest()
        : TestCase("Link fault schedule - absolute-time actions are idempotent")
    {
    }

  private:
    void DoRun() override
    {
        FaultInputTopology topo;
        Ptr<UbReliableRerouting> rerouting =
            DynamicCast<UbPort>(topo.swLeft->GetDevice(1))->GetReliableRerouting();
        ReroutingStateWatcher watcher;
        rerouting->TraceConnectWithoutContext("Rerouting",
                                              MakeBoundCallback(&WatchReroutingState, &watcher));

        std::vector<UbLinkFaultTarget> targets;
        std::string error;
        std::vector<UbLinkFaultScheduleRow> rows;
        const TempScheduleFile file("nodeId,portId,failTimeNs\n2,1,3000\n1,1,3000\n");
        const bool ok = TryReadLinkFaultSchedule(file.Path(), rows, error) &&
                        TryResolveLinkFaultSchedule(rows, targets, error);
        NS_TEST_ASSERT_MSG_EQ(ok, true, "schedule resolves: " + error);
        ScheduleLinkFaults(targets);

        Simulator::Stop(MicroSeconds(50));
        Simulator::Run();
        const bool failed = topo.failedLink->IsFailed();
        const auto state = rerouting->GetState();
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_EQ(failed, true, "link failed through the scheduled action");
        NS_TEST_ASSERT_MSG_EQ(watcher.failureTime, NanoSeconds(3000), "absolute failure honored");
        NS_TEST_ASSERT_MSG_EQ(watcher.failureCount, 1u,
                              "duplicate same-time rows do not restart recovery");
        NS_TEST_ASSERT_MSG_EQ(state, UbReliableRerouting::State::DRAINED,
                              "scheduled recovery completes");
    }
};

class UbLinkFaultInputSuite : public TestSuite
{
  public:
    UbLinkFaultInputSuite()
        : TestSuite("unified-bus-link-fault-input", Type::UNIT)
    {
        AddTestCase(new UbLinkFaultParseTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbLinkFaultResolveTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbLinkFaultScheduleTest(), TestCase::Duration::QUICK);
    }
};

UbLinkFaultInputSuite g_linkFaultInputSuite;

} // namespace
