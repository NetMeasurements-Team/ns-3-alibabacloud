// SPDX-License-Identifier: GPL-2.0-only

#include "ns3/data-rate.h"
#include "ns3/enum.h"
#include "ns3/mac48-address.h"
#include "ns3/node.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/test.h"
#include "ns3/uinteger.h"
#include "ns3/ub-datalink.h"
#include "ns3/ub-datatype.h"
#include "ns3/ub-header.h"
#include "ns3/ub-link.h"
#include "ns3/ub-port.h"

#include <vector>

using namespace ns3;

namespace
{

/// Link rate used by every case, so a 20-byte flit takes exactly 4 ns and every
/// expected duration below is a whole number of nanoseconds. ns-3 only allows
/// the global time resolution to be set once, before any event is scheduled, so
/// the cases cannot switch to picoseconds to test fractional values.
constexpr const char* RATE = "40Gbps";
constexpr uint32_t FLIT_BYTES = 20;
constexpr uint32_t MIN_INTERVAL_FLITS = 32;

/**
 * @brief Build a DLLDP whose payload pads the data link header to payloadBytes.
 *
 * The header contributes 4 bytes, so a payload of 196 bytes makes a 200-byte
 * DLLDP, which is exactly 10 flits at the default flit size.
 */
Ptr<Packet>
MakeDllDp(uint32_t payloadBytes)
{
    Ptr<Packet> packet = Create<Packet>(payloadBytes);
    UbDatalinkPacketHeader header;
    // The model reserves VL 0 for locally generated control frames.
    header.SetPacketVL(1);
    header.SetConfig(static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_UB_MEM));
    packet->AddHeader(header);
    return packet;
}

/// Build a link control frame (DLLCB) through the production helper.
Ptr<Packet>
MakeDllCb()
{
    uint8_t credits[UB_PRIORITY_NUM_DEFAULT] = {0};
    return UbDataLink::BuildControlCreditPacket(credits);
}

/// Apply the packet rate control attributes shared by every case.
void
ConfigurePacketRate(Ptr<UbPort> port,
                    PacketRateControlMode mode,
                    uint32_t flitBytes,
                    uint32_t minIntervalFlits,
                    uint32_t averagePacketLengthFlits = 0,
                    uint32_t averageDuLengthFlits = 0,
                    uint32_t deviceDuLengthFlits = 0,
                    uint32_t burstSizeFlits = 0,
                    const char* interframeGap = "0ns")
{
    port->SetDataRate(DataRate(RATE));
    port->SetAttribute("UbInterframeGap", TimeValue(Time(interframeGap)));
    port->SetAttribute("CbfcFlitLenByte", UintegerValue(flitBytes));
    port->SetAttribute("PacketRateControl", EnumValue(mode));
    port->SetAttribute("PacketRateMinIntervalFlits", UintegerValue(minIntervalFlits));
    port->SetAttribute("PacketRateAveragePacketLengthFlits", UintegerValue(averagePacketLengthFlits));
    port->SetAttribute("PacketRateAverageDuLengthFlits", UintegerValue(averageDuLengthFlits));
    port->SetAttribute("PacketRateDeviceDuLengthFlits", UintegerValue(deviceDuLengthFlits));
    port->SetAttribute("PacketRateBurstSizeFlits", UintegerValue(burstSizeFlits));
}

/// A port with packet rate control configured and no node attached.
Ptr<UbPort>
MakePort(PacketRateControlMode mode,
         uint32_t flitBytes,
         uint32_t minIntervalFlits,
         uint32_t averagePacketLengthFlits = 0,
         uint32_t averageDuLengthFlits = 0,
         uint32_t deviceDuLengthFlits = 0,
         uint32_t burstSizeFlits = 0,
         const char* interframeGap = "0ns")
{
    Ptr<UbPort> port = CreateObject<UbPort>();
    ConfigurePacketRate(port,
                        mode,
                        flitBytes,
                        minIntervalFlits,
                        averagePacketLengthFlits,
                        averageDuLengthFlits,
                        deviceDuLengthFlits,
                        burstSizeFlits,
                        interframeGap);
    return port;
}

/**
 * @brief A UbLink that records transmit attempts and delivers nothing.
 *
 * This drives the real egress path -- egress queue, port state machine,
 * TransmitComplete -- without standing up a switch, an allocator or a peer, and
 * without a peer receive path reacting to what was sent.
 */
class RecordingLink : public UbLink
{
  public:
    bool TransmitStart(Ptr<Packet> packet, Ptr<UbPort>, Time txTime) override
    {
        (void)packet;
        m_sopTimes.push_back(Simulator::Now());
        m_txTimes.push_back(txTime);
        return m_accepts;
    }

    uint32_t GetSendCount() const
    {
        return static_cast<uint32_t>(m_sopTimes.size());
    }

    Time GetSop(uint32_t index) const
    {
        return m_sopTimes.at(index);
    }

    Time GetTxTime(uint32_t index) const
    {
        return m_txTimes.at(index);
    }

    /// When false every attempt is rejected, the way a busy wire would reject it.
    bool m_accepts{true};

  private:
    std::vector<Time> m_sopTimes;
    std::vector<Time> m_txTimes;
};

/// A device node with one packet-rate-controlled port on a recording link.
struct PacketRateFixture
{
    Ptr<Node> node;
    Ptr<UbPort> port;
    Ptr<RecordingLink> link;
};

PacketRateFixture
MakePacketRateFixture(uint32_t nodeId,
                      PacketRateControlMode mode,
                      uint32_t averagePacketLengthFlits = 0,
                      uint32_t averageDuLengthFlits = 0,
                      uint32_t deviceDuLengthFlits = 0,
                      uint32_t burstSizeFlits = 0)
{
    PacketRateFixture fixture;
    fixture.node = CreateObject<Node>(nodeId);
    fixture.port = CreateObject<UbPort>();
    fixture.port->SetAddress(Mac48Address::Allocate());
    fixture.node->AddDevice(fixture.port);
    ConfigurePacketRate(fixture.port,
                        mode,
                        FLIT_BYTES,
                        MIN_INTERVAL_FLITS,
                        averagePacketLengthFlits,
                        averageDuLengthFlits,
                        deviceDuLengthFlits,
                        burstSizeFlits);

    fixture.link = CreateObject<RecordingLink>();
    fixture.port->Attach(fixture.link);
    return fixture;
}

/**
 * Acceptance condition for the static model documented on the PacketRateControl
 * attribute: a DLLDP shorter than PACKET_MIN_INTERVAL is padded until the next
 * SOP is exactly PACKET_MIN_INTERVAL away.
 */
class UbStaticPacketRatePadsShortDllDpTest : public TestCase
{
  public:
    UbStaticPacketRatePadsShortDllDpTest()
        : TestCase("static mode pads a DLLDP below the minimum interval")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<UbPort> port =
            MakePort(PacketRateControlMode::STATIC, FLIT_BYTES, MIN_INTERVAL_FLITS);
        // 10 flits on the wire (40 ns); PACKET_MIN_INTERVAL is 32 flits (128 ns).
        Ptr<Packet> packet = MakeDllDp(196);

        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateNobTimeForTest(packet),
                              NanoSeconds(88),
                              "22 missing flits at 4 ns each");
    }
};

/// A DLLDP already at or above PACKET_MIN_INTERVAL gets no NOB at all.
class UbStaticPacketRateLeavesLongDllDpTest : public TestCase
{
  public:
    UbStaticPacketRateLeavesLongDllDpTest()
        : TestCase("static mode leaves a DLLDP at or above the interval alone")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<UbPort> port =
            MakePort(PacketRateControlMode::STATIC, FLIT_BYTES, MIN_INTERVAL_FLITS);

        // 100 flits, far above the 32-flit minimum.
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateNobTimeForTest(MakeDllDp(1996)),
                              Time(0),
                              "a long DLLDP needs no padding");

        // Exactly 32 flits: the interval already meets the minimum, so the model
        // inserts nothing.
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateNobTimeForTest(MakeDllDp(636)),
                              Time(0),
                              "an exactly-minimum DLLDP needs no padding");
    }
};

/**
 * A DLLDP that does not end on a flit boundary still occupies whole flits, and
 * the tail is covered by the same subtraction.
 */
class UbStaticPacketRateRoundsPartialFlitTest : public TestCase
{
  public:
    UbStaticPacketRateRoundsPartialFlitTest()
        : TestCase("static mode rounds a partial trailing flit up")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<UbPort> port =
            MakePort(PacketRateControlMode::STATIC, FLIT_BYTES, MIN_INTERVAL_FLITS);
        // 65 bytes = 4 flits, 5 bytes into the last one, so the DLLDP itself
        // takes only 13 ns of the 16 ns those four flit slots occupy.
        Ptr<Packet> packet = MakeDllDp(61);

        const Time nob = port->GetPacketRateNobTimeForTest(packet);
        NS_TEST_ASSERT_MSG_EQ(nob, NanoSeconds(115), "128 ns target minus the 13 ns DLLDP");

        // The port must stay occupied for the whole 32-flit minimum, not merely
        // for the 13 ns the DLLDP itself takes.
        const Time occupied = port->GetInterframeGap() + nob +
                              DataRate(RATE).CalculateBytesTxTime(packet->GetSize());
        NS_TEST_ASSERT_MSG_EQ(occupied, NanoSeconds(128), "SOP-to-SOP meets the minimum exactly");
    }
};

/// The interframe gap already counts towards PACKET_INTERVAL.
class UbStaticPacketRateCountsInterframeGapTest : public TestCase
{
  public:
    UbStaticPacketRateCountsInterframeGapTest()
        : TestCase("static mode counts the interframe gap as part of the interval")
    {
    }

  private:
    void DoRun() override
    {
        // 40 ns of DLLDP plus a 100 ns gap already exceeds the 128 ns target.
        Ptr<UbPort> port = MakePort(PacketRateControlMode::STATIC,
                                    FLIT_BYTES,
                                    MIN_INTERVAL_FLITS,
                                    0,
                                    0,
                                    0,
                                    0,
                                    "100ns");
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateNobTimeForTest(MakeDllDp(196)),
                              Time(0),
                              "the interframe gap alone satisfies the minimum interval");
    }
};

/// Control frames share the egress queue but are never padded.
class UbPacketRateNeverPadsControlFrameTest : public TestCase
{
  public:
    UbPacketRateNeverPadsControlFrameTest()
        : TestCase("packet rate control never pads a control frame")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<UbPort> port =
            MakePort(PacketRateControlMode::STATIC, FLIT_BYTES, MIN_INTERVAL_FLITS);
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateNobTimeForTest(MakeDllCb()),
                              Time(0),
                              "a DLLCB must not be padded");
    }
};

/**
 * Mode NONE short-circuits before any parameter is read, so an enabled-mode
 * configuration is not required and zero-valued parameters stay legal.
 */
class UbPacketRateNoneModeIsInertTest : public TestCase
{
  public:
    UbPacketRateNoneModeIsInertTest()
        : TestCase("packet rate control in NONE mode is inert")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<UbPort> port = MakePort(PacketRateControlMode::NONE, 0, 0);
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateNobTimeForTest(MakeDllDp(196)),
                              Time(0),
                              "NONE must not add NOBs");
    }
};

/**
 * Dynamic mode walks the §4.7.3.2 branch table: the deficit counters start
 * below the burst budget, so the first DLLDPs only satisfy PKT_FLIT_NUM, and
 * once CNT_1 passes the budget the padding switches to
 * AVERAGE_PACKET_LENGTH.
 */
class UbDynamicPacketRateBranchSelectionTest : public TestCase
{
  public:
    UbDynamicPacketRateBranchSelectionTest()
        : TestCase("dynamic mode selects branches from the deficit counters")
    {
    }

  private:
    void DoRun() override
    {
        // AVERAGE_PACKET_LENGTH 64, AVERAGE_DU_LENGTH 32, DEVICE_DU_LENGTH 8,
        // burst budget 64 flits.
        Ptr<UbPort> port = MakePort(PacketRateControlMode::DYNAMIC,
                                    FLIT_BYTES,
                                    MIN_INTERVAL_FLITS,
                                    64,
                                    32,
                                    8,
                                    64);
        // 4 flits: PKT_FLIT_NUM = 32, PKT_DU_LENGTH = 32 * (floor(3/8) + 1) = 32.
        Ptr<Packet> packet = MakeDllDp(76);

        // Branch (4): neither counter is over budget, so pad to PKT_FLIT_NUM.
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateNobTimeForTest(packet),
                              NanoSeconds(112),
                              "28 flits to reach PKT_FLIT_NUM");
        port->CommitPacketRateCountersForTest(packet);
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateCounter1ForTest(), 32, "deficit carried forward");
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateCounter2ForTest(), 0, "PKT_DU_LENGTH met the target");

        // Second DLLDP: CNT_1 + 64 - 32 == 64 is not yet over the 64-flit budget.
        port->CommitPacketRateCountersForTest(packet);
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateCounter1ForTest(), 64, "deficit carried forward");

        // Third DLLDP: CNT_1 + 64 - 32 == 96 exceeds the budget, so branch (3)
        // pads to AVERAGE_PACKET_LENGTH and freezes CNT_1.
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateNobTimeForTest(packet),
                              NanoSeconds(240),
                              "60 flits to reach AVERAGE_PACKET_LENGTH");
        port->CommitPacketRateCountersForTest(packet);
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateCounter1ForTest(),
                              64,
                              "a counter over budget stays put");
    }
};

/**
 * Branches (2) and (3) pad to a single target: PKT_DU_LENGTH when only the DU
 * deficit is over budget, AVERAGE_PACKET_LENGTH when only the packet-length
 * deficit is.
 */
class UbDynamicPacketRateSingleTargetTest : public TestCase
{
  public:
    UbDynamicPacketRateSingleTargetTest()
        : TestCase("dynamic mode pads to the one target whose deficit is over budget")
    {
    }

  private:
    void DoRun() override
    {
        // 4 flits on the wire. A burst budget of 0 means any positive deficit is
        // over it, so which counter is over budget is decided by the targets alone.
        Ptr<Packet> packet = MakeDllDp(76);

        // Branch (2): AVERAGE_PACKET_LENGTH 32 holds deficit1 at 0 while
        // PKT_DU_LENGTH 64 pushes deficit2 to 32.
        Ptr<UbPort> duOverBudget = MakePort(PacketRateControlMode::DYNAMIC,
                                            FLIT_BYTES,
                                            MIN_INTERVAL_FLITS,
                                            32,
                                            64,
                                            8,
                                            0);
        NS_TEST_ASSERT_MSG_EQ(duOverBudget->GetPacketRateNobTimeForTest(packet),
                              NanoSeconds(240),
                              "60 flits to reach PKT_DU_LENGTH");

        // Branch (3): swapping the two parameters leaves only deficit1 over
        // budget, and an implementation that always took PKT_DU_LENGTH would
        // answer 28 flits here.
        Ptr<UbPort> packetLengthOverBudget = MakePort(PacketRateControlMode::DYNAMIC,
                                                      FLIT_BYTES,
                                                      MIN_INTERVAL_FLITS,
                                                      64,
                                                      32,
                                                      8,
                                                      0);
        NS_TEST_ASSERT_MSG_EQ(packetLengthOverBudget->GetPacketRateNobTimeForTest(packet),
                              NanoSeconds(240),
                              "60 flits to reach AVERAGE_PACKET_LENGTH");
    }
};

/// Branch (1) pads to whichever of AVERAGE_PACKET_LENGTH and PKT_DU_LENGTH is longer.
class UbDynamicPacketRateBothCountersOverBurstTest : public TestCase
{
  public:
    UbDynamicPacketRateBothCountersOverBurstTest()
        : TestCase("dynamic mode pads to the longer target when both "
                   "counters are over budget")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Packet> packet = MakeDllDp(76); // 4 flits

        // PKT_DU_LENGTH 96 is the longer target: 96 - 4 = 92 flits.
        Ptr<UbPort> duLonger = MakePort(PacketRateControlMode::DYNAMIC,
                                        FLIT_BYTES,
                                        MIN_INTERVAL_FLITS,
                                        64,
                                        96,
                                        8,
                                        0);
        NS_TEST_ASSERT_MSG_EQ(duLonger->GetPacketRateNobTimeForTest(packet),
                              NanoSeconds(368),
                              "92 flits to reach PKT_DU_LENGTH");

        // Swapping the two makes AVERAGE_PACKET_LENGTH the longer one. Taking
        // either target unconditionally would answer 60 flits for one of the
        // two ports, so the pair pins the max() down in both directions.
        Ptr<UbPort> packetLengthLonger = MakePort(PacketRateControlMode::DYNAMIC,
                                                  FLIT_BYTES,
                                                  MIN_INTERVAL_FLITS,
                                                  96,
                                                  64,
                                                  8,
                                                  0);
        NS_TEST_ASSERT_MSG_EQ(packetLengthLonger->GetPacketRateNobTimeForTest(packet),
                              NanoSeconds(368),
                              "92 flits to reach AVERAGE_PACKET_LENGTH");
    }
};

/**
 * Regression for unsigned wraparound: a DLLDP far longer than the configured
 * averages makes both deficits negative, which must not read as "over budget".
 *
 * With unsigned arithmetic CNT_1 + AVERAGE_PACKET_LENGTH - PKT_FLIT_NUM wraps to
 * a huge positive value, both branches look satisfied, and the port would emit
 * a nonsense NOB count instead of leaving the long DLLDP alone.
 */
class UbDynamicPacketRateNegativeDeficitTest : public TestCase
{
  public:
    UbDynamicPacketRateNegativeDeficitTest()
        : TestCase("dynamic mode does not underflow on a negative deficit")
    {
    }

  private:
    void DoRun() override
    {
        // Burst budget 0 is legal and means the peer allows no burst at all.
        Ptr<UbPort> port = MakePort(PacketRateControlMode::DYNAMIC,
                                    FLIT_BYTES,
                                    MIN_INTERVAL_FLITS,
                                    64,
                                    1,
                                    8,
                                    0);
        // 128 flits against PKT_DU_LENGTH = 1 * (floor(127/8) + 1) = 16.
        Ptr<Packet> packet = MakeDllDp(2556);

        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateNobTimeForTest(packet),
                              Time(0),
                              "a DLLDP above every target needs no padding");
        port->CommitPacketRateCountersForTest(packet);
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateCounter1ForTest(),
                              0,
                              "a deficit below zero resets the counter");
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateCounter2ForTest(),
                              0,
                              "a deficit below zero resets the counter");
    }
};

/**
 * A long DLLDP after a run of short ones resets both counters instead of driving
 * them negative, and a counter that is over budget keeps its value.
 *
 * DEVICE_DU_LENGTH 255 holds PKT_DU_LENGTH at AVERAGE_DU_LENGTH for every DLLDP
 * this port can build, which is what lets a long DLLDP undercut it.
 */
class UbDynamicPacketRateCounterResetTest : public TestCase
{
  public:
    UbDynamicPacketRateCounterResetTest()
        : TestCase("dynamic mode resets a counter overshot by a long DLLDP")
    {
    }

  private:
    void DoRun() override
    {
        // A generous burst budget keeps the branch-(4) path in use.
        Ptr<UbPort> port = MakePort(PacketRateControlMode::DYNAMIC,
                                    FLIT_BYTES,
                                    MIN_INTERVAL_FLITS,
                                    64,
                                    64,
                                    255,
                                    1000);
        Ptr<Packet> shortPacket = MakeDllDp(76);
        port->CommitPacketRateCountersForTest(shortPacket);
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateCounter1ForTest(), 32, "one 32-flit deficit");
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateCounter2ForTest(), 32, "one 32-flit deficit");

        // 128 flits against AVERAGE_PACKET_LENGTH 64 and PKT_DU_LENGTH 64. The
        // DLLDP is long enough that both accumulated deficits plus their targets
        // fall below PKT_FLIT_NUM, which resets the counters instead of driving
        // them negative.
        Ptr<Packet> longPacket = MakeDllDp(2556);
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateNobTimeForTest(longPacket), Time(0), "no padding");
        port->CommitPacketRateCountersForTest(longPacket);
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateCounter1ForTest(),
                              0,
                              "CNT_1 + AVERAGE_PACKET_LENGTH fell below PKT_FLIT_NUM");
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateCounter2ForTest(),
                              0,
                              "CNT_2 + PKT_DU_LENGTH fell below PKT_FLIT_NUM");
    }
};

/// An over-budget counter keeps its value rather than advancing or resetting.
class UbDynamicPacketRateCounterFreezeTest : public TestCase
{
  public:
    UbDynamicPacketRateCounterFreezeTest()
        : TestCase("dynamic mode freezes a counter that is over budget")
    {
    }

  private:
    void DoRun() override
    {
        // PKT_DU_LENGTH 64 leaves deficit2 at 32, just inside the 40-flit
        // budget, while AVERAGE_PACKET_LENGTH 32 holds deficit1 at 0.
        Ptr<UbPort> port = MakePort(PacketRateControlMode::DYNAMIC,
                                    FLIT_BYTES,
                                    MIN_INTERVAL_FLITS,
                                    32,
                                    64,
                                    255,
                                    40);
        Ptr<Packet> packet = MakeDllDp(76);

        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateNobTimeForTest(packet),
                              NanoSeconds(112),
                              "28 flits to reach PKT_FLIT_NUM");
        port->CommitPacketRateCountersForTest(packet);
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateCounter2ForTest(), 32, "deficit carried forward");

        // The next DLLDP pushes deficit2 to 64, past the budget, so CNT_2 holds
        // at 32 rather than advancing or resetting.
        port->CommitPacketRateCountersForTest(packet);
        NS_TEST_ASSERT_MSG_EQ(port->GetPacketRateCounter2ForTest(), 32, "an over-budget counter holds");
    }
};

/// Deficit counters belong to one egress direction of one port.
class UbPacketRatePerPortStateTest : public TestCase
{
  public:
    UbPacketRatePerPortStateTest()
        : TestCase("packet rate control keeps per-port state separate")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<UbPort> padded = MakePort(PacketRateControlMode::DYNAMIC,
                                      FLIT_BYTES,
                                      MIN_INTERVAL_FLITS,
                                      64,
                                      32,
                                      8,
                                      1000);
        Ptr<UbPort> idle = MakePort(PacketRateControlMode::DYNAMIC,
                                    FLIT_BYTES,
                                    MIN_INTERVAL_FLITS,
                                    64,
                                    32,
                                    8,
                                    1000);

        padded->CommitPacketRateCountersForTest(MakeDllDp(76));
        NS_TEST_ASSERT_MSG_EQ(padded->GetPacketRateCounter1ForTest(), 32, "sending port advanced");
        NS_TEST_ASSERT_MSG_EQ(idle->GetPacketRateCounter1ForTest(), 0, "the other port is untouched");
        NS_TEST_ASSERT_MSG_EQ(idle->GetPacketRateCounter2ForTest(), 0, "the other port is untouched");
    }
};

/**
 * The cases above call the computation directly. These three drive the real
 * egress path -- queue, port state machine, TransmitComplete -- so the padding
 * is observed where it actually has to hold, and a mistake such as adding it to
 * the packet's serialization time instead of to the port occupancy is caught.
 */
class UbPacketRateProductionPathTest : public TestCase
{
  public:
    UbPacketRateProductionPathTest()
        : TestCase("static mode spaces consecutive DLLDPs on the real egress path")
    {
    }

  private:
    void DoRun() override
    {
        PacketRateFixture fixture = MakePacketRateFixture(0, PacketRateControlMode::STATIC);

        const uint32_t packets = 3;
        for (uint32_t i = 0; i < packets; ++i) {
            fixture.port->EnqueueToEgress({0, 1, MakeDllDp(196)});
        }
        // The first DLLDP goes out synchronously; the others follow the
        // TransmitComplete events this port schedules.
        fixture.port->TriggerTransmit();
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(fixture.link->GetSendCount(), packets, "every DLLDP was transmitted");

        // 10 flits of DLLDP plus 22 flits of NOB put each next SOP 32 flits later.
        for (uint32_t i = 1; i < packets; ++i) {
            NS_TEST_ASSERT_MSG_EQ(fixture.link->GetSop(i) - fixture.link->GetSop(i - 1),
                                  NanoSeconds(128),
                                  "consecutive SOPs are PACKET_MIN_INTERVAL apart");
        }

        // The padding is port occupancy only: it must not stretch the DLLDP, or
        // the peer would receive it later than its serialization time says.
        for (uint32_t i = 0; i < packets; ++i) {
            NS_TEST_ASSERT_MSG_EQ(fixture.link->GetTxTime(i),
                                  NanoSeconds(40),
                                  "NOBs stay out of the packet's serialization time");
        }

        Simulator::Destroy();
    }
};

/// NONE mode leaves the egress path as it was before the feature existed.
class UbPacketRateProductionPathNoneTest : public TestCase
{
  public:
    UbPacketRateProductionPathNoneTest()
        : TestCase("NONE mode does not space DLLDPs on the real egress path")
    {
    }

  private:
    void DoRun() override
    {
        PacketRateFixture fixture = MakePacketRateFixture(1, PacketRateControlMode::NONE);

        const uint32_t packets = 3;
        for (uint32_t i = 0; i < packets; ++i) {
            fixture.port->EnqueueToEgress({0, 1, MakeDllDp(196)});
        }
        fixture.port->TriggerTransmit();
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(fixture.link->GetSendCount(), packets, "every DLLDP was transmitted");
        for (uint32_t i = 1; i < packets; ++i) {
            NS_TEST_ASSERT_MSG_EQ(fixture.link->GetSop(i) - fixture.link->GetSop(i - 1),
                                  NanoSeconds(40),
                                  "without NOBs the next SOP follows the serialization time");
        }

        Simulator::Destroy();
    }
};

/**
 * A rejected transmission must not consume burst budget: the counters advance
 * only after UbLink::TransmitStart has accepted the DLLDP.
 */
class UbPacketRateProductionPathRejectedTest : public TestCase
{
  public:
    UbPacketRateProductionPathRejectedTest()
        : TestCase("a rejected transmission does not advance the deficit counters")
    {
    }

  private:
    void DoRun() override
    {
        PacketRateFixture fixture =
            MakePacketRateFixture(2, PacketRateControlMode::DYNAMIC, 64, 32, 8, 1000);
        fixture.link->m_accepts = false;

        fixture.port->EnqueueToEgress({0, 1, MakeDllDp(76)});
        fixture.port->TriggerTransmit();
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(fixture.link->GetSendCount(), 1, "the attempt reached the link");
        NS_TEST_ASSERT_MSG_EQ(fixture.port->GetPacketRateCounter1ForTest(),
                              0,
                              "a rejected attempt must not consume burst budget");

        // A rejected DLLDP is dropped, not retried: TransmitPacket resets the
        // port state and re-triggers the queue. Enqueue a fresh one over a
        // link that now accepts.
        fixture.link->m_accepts = true;
        fixture.port->EnqueueToEgress({0, 1, MakeDllDp(76)});
        fixture.port->TriggerTransmit();
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(fixture.link->GetSendCount(), 2, "the second send went out");
        NS_TEST_ASSERT_MSG_EQ(fixture.port->GetPacketRateCounter1ForTest(),
                              32,
                              "the accepted send carries the deficit forward");

        Simulator::Destroy();
    }
};

class UbPacketRateControlTestSuite : public TestSuite
{
  public:
    UbPacketRateControlTestSuite()
        : TestSuite("unified-bus-packet-rate-control", Type::UNIT)
    {
        AddTestCase(new UbStaticPacketRatePadsShortDllDpTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbStaticPacketRateLeavesLongDllDpTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbStaticPacketRateRoundsPartialFlitTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbStaticPacketRateCountsInterframeGapTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbPacketRateNeverPadsControlFrameTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbPacketRateNoneModeIsInertTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbDynamicPacketRateBranchSelectionTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbDynamicPacketRateSingleTargetTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbDynamicPacketRateBothCountersOverBurstTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbDynamicPacketRateNegativeDeficitTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbDynamicPacketRateCounterResetTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbDynamicPacketRateCounterFreezeTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbPacketRatePerPortStateTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbPacketRateProductionPathTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbPacketRateProductionPathNoneTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbPacketRateProductionPathRejectedTest(), TestCase::Duration::QUICK);
    }
};

static UbPacketRateControlTestSuite g_ubPacketRateControlTestSuite;

} // namespace
