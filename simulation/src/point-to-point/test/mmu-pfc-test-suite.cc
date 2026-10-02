// Ordinary-switch MMU/PFC system tests; run test-runner --suite=mmu-pfc.
#include "ns3/test.h"
#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/global-value.h"
#include "ns3/string.h"
#include "ns3/simulator.h"
#include "ns3/internet-stack-helper.h"
#include "ns3/ipv4-address-helper.h"
#include "ns3/ipv4-header.h"
#include "ns3/udp-header.h"
#include "ns3/qbb-helper.h"
#include "ns3/qbb-channel.h"
#include "ns3/switch-node.h"
#include "ns3/simple-seq-ts-header.h"
#include "ns3/ppp-header.h"
#include "ns3/pause-header.h"
#include "ns3/int-header.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <vector>
using namespace ns3;
namespace {

// Synthetic host: use the actual Qbb priority queue and PFC receiver, without
// RDMA retransmission/congestion control or a switch MMU at the traffic source.
class TestSource : public Node {
public:
  TestSource() { m_node_type = 1; }
  void SwitchNotifyDequeue(uint32_t, uint32_t, Ptr<Packet>) override {}
};
class MmuPfcTestCase : public TestCase {
public:
  explicit MmuPfcTestCase(const std::string& scenario)
    : TestCase("mmu-pfc-" + scenario), mode(scenario) {}
private:
  void Check(bool condition, const char* message) {
    NS_TEST_EXPECT_MSG_EQ(condition, true, message);
  }
  void DoTeardown() override {
    Simulator::Destroy();
    samples.close(); events.close();
    devices.clear(); switches.clear();
    IntHeader::mode = savedIntMode;
    GlobalValue::Bind("SimulatorImplementationType", savedSimulator);
  }
  std::string mode;
  decltype(IntHeader::mode) savedIntMode;
  StringValue savedSimulator;
  static constexpr uint32_t payload = 1000, count = 4096, priority = 3;
  std::vector<Ptr<SwitchNode>> switches;
  std::vector<Ptr<QbbNetDevice>> devices;
  std::vector<uint64_t> xoff, xon;
  std::vector<bool> seen = std::vector<bool>(2 * count, false);
  uint64_t received = 0, peak = 0, peakHeadroom = 0;
  std::ofstream samples, events;

  static void Pfc(MmuPfcTestCase* self, uint32_t id, uint32_t pause) {
    (pause ? self->xoff[id] : self->xon[id])++;
    self->events << Simulator::Now().GetNanoSeconds() << ',' << id << ',' << pause << '\n';
  }
  int Receive(Ptr<Packet>, CustomHeader& h) {
    const uint32_t source = (h.sip >> 8) & 0xffff;
    Check(!(source >= 2 || h.udp.seq >= count), "Unexpected source or sequence");
    if (source >= 2 || h.udp.seq >= count) return 0;
    const uint32_t id = source * count + h.udp.seq;
    Check(!(seen[id]), "Duplicate delivery");
    seen[id] = true;
    received++;
    return 0;
  }
  void Burst(Ptr<QbbNetDevice> dev, uint32_t source, Ipv4Address destination) {
    for (uint32_t seq = 0; seq < count; ++seq) {
      Ptr<Packet> p = Create<Packet>(payload);
      SimpleSeqTsHeader seqh;
      seqh.SetSeq(seq); seqh.SetPG(priority); p->AddHeader(seqh);
      UdpHeader udp; udp.SetSourcePort(100 + source); udp.SetDestinationPort(200); p->AddHeader(udp);
      Ipv4Header ip; ip.SetSource(Ipv4Address(0x0a000001 + (source << 8)));
      ip.SetDestination(destination); ip.SetProtocol(0x11); ip.SetPayloadSize(p->GetSize());
      ip.SetTtl(64); p->AddHeader(ip);
      PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp);
      CustomHeader h(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
      p->PeekHeader(h);
      dev->SwitchSend(priority, p, h);
    }
  }
  void Sample() {
    for (uint32_t s = 0; s < switches.size(); ++s) {
      auto m = switches[s]->m_mmu;
      uint64_t ingress = 0, egress = 0, shared = 0, headroom = 0;
      for (uint32_t p = 1; p < switches[s]->GetNDevices(); ++p) {
        for (uint32_t q = 1; q < SwitchMmu::qCnt; ++q) {
          ingress += uint64_t{m->ingress_bytes[p][q]} + m->hdrm_bytes[p][q];
          egress += m->egress_bytes[p][q];
          shared += m->GetSharedUsed(p, q);
          headroom += m->hdrm_bytes[p][q];
          Check(!(m->hdrm_bytes[p][q] > m->headroom[p]), "Headroom exceeded");
        }
      }
      Check(!(ingress != egress || shared != m->shared_used_bytes), "MMU ingress/egress/shared accounting mismatch");
      Check(!(ingress > m->buffer_size), "Physical buffer exceeded");
      peak = std::max(peak, ingress); peakHeadroom = std::max(peakHeadroom, headroom);
      samples << Simulator::Now().GetNanoSeconds() << ',' << s << ',' << ingress
              << ',' << shared << ',' << headroom << '\n';
    }
    if (Simulator::Now() < MilliSeconds(5)) Simulator::Schedule(NanoSeconds(100), &MmuPfcTestCase::Sample, this);
  }
  void DoRun() override {
    savedIntMode = IntHeader::mode;
    GlobalValue::GetValueByName("SimulatorImplementationType", savedSimulator);
    const bool baseline = mode == "baseline", enabled = mode != "off";
    // Single-threaded event execution makes the test reproducible even in MTP builds.
    GlobalValue::Bind("SimulatorImplementationType", StringValue("ns3::DefaultSimulatorImpl"));
    IntHeader::mode = IntHeader::NONE;
    auto a = CreateObject<TestSource>(); auto b = CreateObject<TestSource>();
    auto sink = CreateObject<Node>();
    switches = {CreateObject<SwitchNode>(), CreateObject<SwitchNode>()};
    NodeContainer nodes; nodes.Add(a); nodes.Add(b); nodes.Add(sink);
    for (auto sw : switches) nodes.Add(sw);
    InternetStackHelper stack; stack.Install(nodes);
    samples.open(CreateTempDirFilename(mode + "-occupancy.csv"));
    events.open(CreateTempDirFilename(mode + "-pfc.csv"));
    Check(!(!samples || !events), "Cannot create test output files");
    samples << "time_ns,switch,occupancy,shared,headroom\n";
    events << "time_ns,device,pause\n";
    QbbHelper helper;
    helper.SetChannelAttribute("Delay", TimeValue(MicroSeconds(1)));
    auto link = [&](Ptr<Node> from, Ptr<Node> to, const char* rate) {
      helper.SetDeviceAttribute("DataRate", DataRateValue(DataRate(rate)));
      auto pair = helper.Install(from, to);
      Ipv4AddressHelper ip; // Addresses only provide interfaces for generated PFC.
      const uint32_t subnet = 0x0b000000 + (devices.size() << 8);
      ip.SetBase(Ipv4Address(subnet), Ipv4Mask("255.255.255.0")); ip.Assign(pair);
      for (uint32_t i = 0; i < 2; ++i) devices.push_back(DynamicCast<QbbNetDevice>(pair.Get(i)));
      return pair;
    };
    link(a, switches[0], "100Gbps"); // device IDs 0,1
    link(b, switches[0], "100Gbps"); // 2,3
    auto trunk = link(switches[0], switches[1], "100Gbps"); // 4,5
    auto last = link(switches[1], sink, baseline ? "100Gbps" : "25Gbps"); // 6,7
    Ipv4Address dst(0x0a000001 + (sink->GetId() << 8)); // Match Qbb destination decoding.
    switches[0]->AddTableEntry(dst, trunk.Get(0)->GetIfIndex());
    switches[1]->AddTableEntry(dst, last.Get(0)->GetIfIndex());
    devices[7]->m_rdmaReceiveCb = MakeCallback(&MmuPfcTestCase::Receive, this);
    xoff.resize(devices.size()); xon.resize(devices.size());
    for (uint32_t i = 0; i < devices.size(); ++i)
      Check(!(!devices[i]->TraceConnectWithoutContext("QbbPfc", MakeBoundCallback(&MmuPfcTestCase::Pfc, this, i))), "Cannot attach PFC trace");
    for (auto sw : switches) {
      sw->SetAttribute("PfcEnabled", BooleanValue(enabled));
      sw->SetAttribute("ForwardDelay", DoubleValue(1.5));
      sw->SetAttribute("EcnEnabled", BooleanValue(false));
      auto m = sw->m_mmu; m->node_id = sw->GetId();
      for (uint32_t p = 1; p < sw->GetNDevices(); ++p) {
        auto dev = DynamicCast<QbbNetDevice>(sw->GetDevice(p));
        const auto delay = DynamicCast<QbbChannel>(dev->GetChannel())->GetDelay().GetNanoSeconds();
        // Same sizing rule as common.h; complete modeled packet sizes, not payload only.
        const uint64_t h = std::ceil(static_cast<long double>(dev->GetDataRate().GetBitRate()) *
            (2.L * delay + 1500.L) / 8e9L) +
            3 * (payload + CustomHeader::GetStaticWholeHeaderSize()) +
            PppHeader::GetStaticSize() + Ipv4Header().GetSerializedSize() + PauseHeader().GetSerializedSize();
        m->ConfigHdrm(p, h); m->pfc_a_shift[p] = 3;
      }
      m->ConfigNPort(sw->GetNDevices() - 1); m->ConfigBufferSize(512 * 1024);
    }
    Simulator::Schedule(MicroSeconds(10), &MmuPfcTestCase::Burst, this, devices[0], 0, dst);
    if (!baseline) Simulator::Schedule(MicroSeconds(10), &MmuPfcTestCase::Burst, this, devices[2], 1, dst);
    Simulator::Schedule(NanoSeconds(0), &MmuPfcTestCase::Sample, this);
    Simulator::Stop(MilliSeconds(5) + NanoSeconds(1)); Simulator::Run();
    const uint64_t expected = (baseline ? 1 : 2) * count;
    uint64_t pauses = 0, resumes = 0;
    for (uint32_t i = 0; i < devices.size(); ++i) {
      pauses += xoff[i]; resumes += xon[i];
      Check(!(xoff[i] != xon[i]), "Unbalanced pause/resume on a device");
      for (uint32_t q = 0; q < SwitchMmu::qCnt; ++q)
        Check(!(devices[i]->GetQueue()->GetNBytes(q) != 0), "Egress queue did not drain");
    }
    for (auto sw : switches) {
      auto m = sw->m_mmu;
      Check(!(m->shared_used_bytes != 0), "Shared pool did not drain");
      for (uint32_t p = 1; p < sw->GetNDevices(); ++p)
        for (uint32_t q = 1; q < SwitchMmu::qCnt; ++q)
          Check(!(m->ingress_bytes[p][q] || m->hdrm_bytes[p][q] ||
                          m->egress_bytes[p][q] || m->paused[p][q]), "MMU did not drain/resume");
    }
    if (mode == "on") {
      Check(!(received != expected || !peakHeadroom), "Loss or no headroom exercise");
      Check(!(!xoff[4] || !(xoff[0] + xoff[2])), "PFC did not propagate hop by hop");
    } else if (baseline) {
      Check(!(received != expected || pauses), "Baseline lost packets or unexpectedly paused");
    } else {
      Check(!(received >= expected || pauses), "Negative control did not exhibit loss without PFC");
    }
    std::cout << "MMU/PFC mode=" << mode << " received=" << received << '/' << expected
              << " xoff=" << pauses << " xon=" << resumes << " peak_bytes=" << peak
              << " peak_headroom_bytes=" << peakHeadroom << '\n';
  }
};
class MmuPfcTestSuite : public TestSuite {
public:
  MmuPfcTestSuite() : TestSuite("mmu-pfc", SYSTEM) {
    AddTestCase(new MmuPfcTestCase("baseline"), TestCase::QUICK);
    AddTestCase(new MmuPfcTestCase("on"), TestCase::QUICK);
    AddTestCase(new MmuPfcTestCase("off"), TestCase::QUICK);
  }
};
static MmuPfcTestSuite g_mmuPfcTestSuite;
} // namespace

