// SPDX-License-Identifier: GPL-2.0-only
#include "ub-node-test-fixture.h"

#include "ns3/test.h"
#include "ns3/ub-ldst-api.h"
#include "ns3/ub-ldst-endpoint.h"
#include "ns3/ub-ldst-instance.h"
#include "ns3/ub-ldst-packet-utils.h"
#include "ns3/ub-link.h"
#include "ns3/ub-utils.h"
#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

using namespace ns3;

namespace
{
std::string
PacketHex(Ptr<const Packet> packet)
{
    std::vector<uint8_t> bytes(packet->GetSize());
    packet->CopyData(bytes.data(), bytes.size());
    std::ostringstream out;
    for (uint8_t byte : bytes)
    {
        out << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
    }
    return out.str();
}

// Uses the same node/route fixture as the existing LDST and CTP tests. No TP is required.
struct LdstTopology
{
    Ptr<Node> source = CreateObject<Node>();
    Ptr<Node> target = CreateObject<Node>();
    Ptr<Node> middle = CreateObject<Node>();

    LdstTopology()
    {
        InitNode(source, UB_DEVICE, 1);
        InitNode(target, UB_DEVICE, 1);
        InitNode(middle, UB_SWITCH, 2);
        for (uint32_t i = 0; i < 2; ++i)
        {
            Ptr<Node> host = i == 0 ? source : target;
            auto link = CreateObject<UbLink>();
            DynamicCast<UbPort>(host->GetDevice(0))->Attach(link);
            DynamicCast<UbPort>(middle->GetDevice(i))->Attach(link);
            AddShortestRoute(host, i == 0 ? target->GetId() : source->GetId(), 0, 0);
            AddShortestRoute(middle, host->GetId(), 0, i);
        }
    }

    Ptr<UbLdstApi> Api(Ptr<Node> node)
    {
        return node->GetObject<UbController>()->GetUbFunction()->GetUbLdstApi();
    }
};

class LdstRoundTripTest : public TestCase
{
  public:
    LdstRoundTripTest(bool endpoint,
                      bool load,
                      uint8_t config = 9,
                      bool explicitPorts = false,
                      bool delayed = false)
        : TestCase(std::string(endpoint ? "Endpoint" : "Api") + (load ? " LOAD" : " STORE") +
                   " CFG" + std::to_string(config) +
                   (explicitPorts ? " explicit ports" : " wildcard") +
                   (delayed ? " delayed" : " immediate") + " round trip"),
          m_endpoint(endpoint),
          m_load(load),
          m_config(config),
          m_explicitPorts(explicitPorts),
          m_delayed(delayed)
    {
    }

  private:
    void Observe(Ptr<Packet> packet)
    {
        UbDatalinkHeader dl;
        packet->PeekHeader(dl);
        if (dl.IsControlCreditHeader())
        {
            return;
        }
        m_bytes.push_back(PacketHex(packet));
        Ptr<Packet> view = packet->Copy();
        UbDatalinkPacketHeader dlh;
        UbDummyTransactionHeader tah;
        view->RemoveHeader(dlh);
        uint8_t nlp = 0;
        UbLdstAddress src;
        UbLdstAddress dst;
        if (dlh.GetConfig() == 7)
        {
            UbCna24NetworkHeader nth;
            view->RemoveHeader(nth);
            nlp = nth.GetNlp();
            src = DecodeLdstAddress(7, nth.GetScna());
            dst = DecodeLdstAddress(7, nth.GetDcna());
        }
        else
        {
            UbCna16NetworkHeader nth;
            view->RemoveHeader(nth);
            nlp = nth.GetNlp();
            src = DecodeLdstAddress(9, nth.GetScna());
            dst = DecodeLdstAddress(9, nth.GetDcna());
        }
        view->PeekHeader(tah);
        NS_TEST_ASSERT_MSG_EQ(dlh.GetConfig(), m_config, "Request and response use chosen CFG");
        NS_TEST_ASSERT_MSG_EQ(nlp, (m_config == 7 ? 4 : 0), "Format-specific NLP");
        NS_TEST_ASSERT_MSG_EQ(src.hasPort, m_explicitPorts, "Source port presence preserved");
        NS_TEST_ASSERT_MSG_EQ(dst.hasPort, m_explicitPorts, "Destination port presence preserved");
        const bool request = tah.GetUbTransactionOpcode() == uint8_t(UbTransactionOpcode::WRITE) ||
                             tah.GetUbTransactionOpcode() == uint8_t(UbTransactionOpcode::READ);
        const uint32_t expectedPayload = request ? (m_load ? 64 : 512) : (m_load ? 512 : 0);
        if (request)
        {
            UbCompactTransactionHeader compact;
            UbCompactMAExtTah memory;
            view->RemoveHeader(compact);
            view->RemoveHeader(memory);
            NS_TEST_ASSERT_MSG_EQ(memory.GetLength(), 3, "Original 512-byte length encoding");
        }
        else
        {
            UbCompactAckTransactionHeader compact;
            view->RemoveHeader(compact);
        }
        NS_TEST_ASSERT_MSG_EQ(view->GetSize(), expectedPayload, "Compact headers and payload");
        const uint32_t legacySize = request ? (m_load ? 92 : 540) : (m_load ? 528 : 16);
        NS_TEST_ASSERT_MSG_EQ(packet->GetSize(),
                              legacySize + (m_config == 7 ? 2 : 0),
                              "CNA24 adds exactly two bytes");
        UbFlowTag flow;
        NS_TEST_ASSERT_MSG_EQ(packet->PeekPacketTag(flow),
                              !m_endpoint && request,
                              "Preserve Api request flow tags and response behavior");
    }

    void Forwarded(Ptr<Packet> packet, Time)
    {
        UbDatalinkHeader dl;
        packet->PeekHeader(dl);
        if (!dl.IsControlCreditHeader())
        {
            ++m_forwarded;
        }
    }

    bool RemoteRequest(const UbLdstRemoteRequestContext& context, UbLdstResponseSender sender)
    {
        NS_TEST_ASSERT_MSG_EQ_RETURNS_BOOL(context.requestBytes,
                                           (m_load ? 64u : 512u),
                                           "Remote request payload contract");
        const auto opcode =
            m_load ? UbTransactionOpcode::READ_RESPONSE : UbTransactionOpcode::TAACK;
        Ptr<Packet> response;
        if (m_endpoint)
        {
            if (!m_load)
            {
                NS_TEST_ASSERT_MSG_EQ_RETURNS_BOOL(PacketHex(context.payload),
                                                   PacketHex(Payload()),
                                                   "STORE bytes arrive unchanged");
            }
            else
            {
                response = Payload();
            }
        }
        const auto respond = [sender, opcode, context, response]() {
            sender(opcode, context.responseBytes, response);
        };
        if (m_delayed)
        {
            Simulator::Schedule(NanoSeconds(50), respond);
        }
        else
        {
            respond();
        }
        return true;
    }

    void ApiComplete(uint32_t taskId)
    {
        NS_TEST_ASSERT_MSG_EQ(taskId, 41u, "Original task completion");
        ++m_completed;
    }

    Ptr<Packet> Payload()
    {
        std::vector<uint8_t> bytes(512);
        for (uint32_t i = 0; i < bytes.size(); ++i)
        {
            bytes[i] = i % 251;
        }
        return Create<Packet>(bytes.data(), bytes.size());
    }

    void EndpointComplete(uint16_t id, bool success, Ptr<Packet> payload)
    {
        NS_TEST_ASSERT_MSG_EQ(id, 123u, "Endpoint request identity");
        NS_TEST_ASSERT_MSG_EQ(success, true, "Real response completes request");
        NS_TEST_ASSERT_MSG_EQ(payload->GetSize(), (m_load ? 512u : 0u), "Response length");
        if (m_load && (m_config == 7 || m_delayed))
        {
            NS_TEST_ASSERT_MSG_EQ(PacketHex(payload),
                                  PacketHex(Payload()),
                                  "LOAD callback receives the original response bytes");
        }
        ++m_completed;
    }

    void DoRun() override
    {
        LdstTopology topo;
        if (m_config != 9)
        {
            Ptr<Object> outgoing =
                m_endpoint
                    ? StaticCast<Object>(topo.source->GetObject<UbController>()->GetLdstEndpoint())
                    : StaticCast<Object>(topo.Api(topo.source));
            const bool set = outgoing->SetAttributeFailSafe("HeaderConfig",
                                                            StringValue(std::to_string(m_config)));
            NS_TEST_ASSERT_MSG_EQ(set, true, "Sending entry exposes independent HeaderConfig");
            if (!set)
            {
                Simulator::Destroy();
                return;
            }
        }
        if (m_delayed || (m_endpoint && m_config == 7))
        {
            topo.Api(topo.target)
                ->SetRemoteRequestProcessor(MakeCallback(&LdstRoundTripTest::RemoteRequest, this));
        }
        for (uint32_t i = 0; i < 2; ++i)
        {
            topo.middle->GetDevice(i)->TraceConnectWithoutContext(
                "TraComEventNotify",
                MakeCallback(&LdstRoundTripTest::Forwarded, this));
        }
        for (auto node : {topo.source, topo.target})
        {
            node->GetDevice(0)->TraceConnectWithoutContext(
                "PktRcvNotify",
                MakeCallback(&LdstRoundTripTest::Observe, this));
        }
        Ptr<UbLdstInstance> instance;
        if (m_endpoint)
        {
            UbLdstRequest request;
            request.requestId = 123;
            request.destNode = topo.target->GetId();
            request.type = m_load ? UbMemOperationType::LOAD : UbMemOperationType::STORE;
            request.requestBytes = m_load ? 64 : 512;
            request.responseBytes = m_load ? 512 : 0;
            request.priority = 7;
            request.completion = MakeCallback(&LdstRoundTripTest::EndpointComplete, this);
            if (!m_load && (m_config == 7 || m_delayed))
            {
                request.payload = Payload();
            }
            // Rejection must not retain this ID or overwrite another accepted request.
            auto endpoint = topo.source->GetObject<UbController>()->GetLdstEndpoint();
            UbLdstRequest invalid = request;
            invalid.destNode = m_config == 7 ? 65536 : 4096;
            const bool rejectedBefore = endpoint->SendRequest(invalid);
            NS_TEST_ASSERT_MSG_EQ(rejectedBefore,
                                  false,
                                  "CNA16 overflow must be rejected before inflight admission");
            NS_TEST_ASSERT_MSG_EQ(
                topo.source->GetObject<UbSwitch>()->GetVoqPacketCountForTest(0, 7, 0),
                0u,
                "Rejected request must not queue a packet");
            NS_TEST_ASSERT_MSG_EQ(
                topo.source->GetObject<UbController>()->GetLdstEndpoint()->SendRequest(request),
                true,
                "Public Endpoint accepts request");
            invalid.requestId = request.requestId;
            const bool rejectedDuplicate = endpoint->SendRequest(invalid);
            NS_TEST_ASSERT_MSG_EQ(rejectedDuplicate,
                                  false,
                                  "Rejected duplicate must preserve the accepted request");
            invalid.requestId = request.requestId + 1;
            const bool rejectedAfter = endpoint->SendRequest(invalid);
            NS_TEST_ASSERT_MSG_EQ(rejectedAfter,
                                  false,
                                  "Rejected new ID must not disturb the accepted request");
        }
        else
        {
            instance = CreateObject<UbLdstInstance>();
            topo.source->AggregateObject(instance);
            instance->SetAttribute("ThreadNum", UintegerValue(1));
            instance->Init(topo.source->GetId());
            instance->SetClientCallback(MakeCallback(&LdstRoundTripTest::ApiComplete, this));
            instance->HandleLdstTask(topo.source->GetId(),
                                     topo.target->GetId(),
                                     512,
                                     41,
                                     7,
                                     m_load ? UbMemOperationType::LOAD : UbMemOperationType::STORE,
                                     {0},
                                     0,
                                     0,
                                     m_explicitPorts,
                                     0,
                                     m_explicitPorts);
        }
        Simulator::Stop(MilliSeconds(1));
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(m_completed, 1u, "Exactly one real completion");
        NS_TEST_ASSERT_MSG_EQ(m_forwarded, 2u, "Middle Switch forwards request and response");
        NS_TEST_ASSERT_MSG_EQ(m_bytes.size(), 2u, "One request and one response");
        if (instance)
        {
            NS_TEST_ASSERT_MSG_EQ(instance->GetActiveTaskCount(), 0u, "Task retired");
            NS_TEST_ASSERT_MSG_EQ(instance->GetActiveSegmentCount(), 0u, "Segment retired");
        }
        // Capture only when explicitly requested, before any production refactor.
        // Later milestones compare these immutable files; expected bytes never use a builder.
        const std::string name =
            std::string(m_endpoint ? "endpoint" : "api") + (m_load ? "-load" : "-store");
        if (const char* directory = std::getenv("UB_LDST_CAPTURE_BASELINE");
            directory && m_config == 9 && !m_explicitPorts && !m_delayed)
        {
            std::ofstream out(std::string(directory) + "/" + name + ".hex");
            for (const auto& bytes : m_bytes)
            {
                out << bytes << '\n';
            }
            NS_TEST_ASSERT_MSG_EQ(out.good(), true, "Baseline capture written");
        }
        if (const char* directory = std::getenv("UB_LDST_COMPARE_BASELINE");
            directory && m_config == 9 && !m_explicitPorts && !m_delayed)
        {
            std::ifstream in(std::string(directory) + "/" + name + ".hex");
            NS_TEST_ASSERT_MSG_EQ(in.good(), true, "Frozen pre-refactor baseline exists");
            for (const auto& bytes : m_bytes)
            {
                std::string expected;
                std::getline(in, expected);
                NS_TEST_ASSERT_MSG_EQ(bytes, expected, "Real CFG9 bytes match pre-refactor path");
            }
            std::string extra;
            NS_TEST_ASSERT_MSG_EQ(bool(std::getline(in, extra)), false, "No missing packets");
        }
        Simulator::Destroy();
    }

    bool m_endpoint;
    bool m_load;
    uint8_t m_config;
    bool m_explicitPorts;
    bool m_delayed;
    uint32_t m_forwarded{0};
    uint32_t m_completed{0};
    std::vector<std::string> m_bytes;
};

class LdstAddressTest : public TestCase
{
  public:
    LdstAddressTest()
        : TestCase("CNA address boundaries and optional port presence")
    {
    }

  private:
    void DoRun() override
    {
        for (uint8_t cfg : {7, 9})
        {
            const uint32_t maxNode = cfg == 7 ? 65535 : 4095;
            const uint32_t maxPort = cfg == 7 ? 254 : 14;
            for (uint32_t node : {0u, maxNode})
            {
                uint32_t cna = 0;
                bool encoded = TryEncodeLdstAddress(cfg, {node, UINT32_MAX, false}, cna);
                NS_TEST_ASSERT_MSG_EQ(encoded, true, "Unused port storage must be ignored");
                auto address = DecodeLdstAddress(cfg, cna);
                NS_TEST_ASSERT_MSG_EQ(address.nodeId, node, "Node survives encoding");
                NS_TEST_ASSERT_MSG_EQ(address.hasPort, false, "Wildcard is retained");
                for (uint32_t port : {0u, maxPort})
                {
                    encoded = TryEncodeLdstAddress(cfg, {node, port, true}, cna);
                    NS_TEST_ASSERT_MSG_EQ(encoded, true, "Legal explicit port");
                    address = DecodeLdstAddress(cfg, cna);
                    NS_TEST_ASSERT_MSG_EQ(address.nodeId, node, "Explicit node survives");
                    NS_TEST_ASSERT_MSG_EQ(address.hasPort, true, "Port zero is explicit");
                    NS_TEST_ASSERT_MSG_EQ(address.portId, port, "Port survives encoding");
                }
            }
            uint32_t cna = 0xabcdef;
            bool encoded = TryEncodeLdstAddress(cfg, {maxNode + 1, 0, false}, cna);
            NS_TEST_ASSERT_MSG_EQ(encoded, false, "Node overflow rejected");
            NS_TEST_ASSERT_MSG_EQ(cna, 0xabcdefu, "Failed encoding leaves output untouched");
            encoded = TryEncodeLdstAddress(cfg, {0, maxPort + 1, true}, cna);
            NS_TEST_ASSERT_MSG_EQ(encoded, false, "Port overflow rejected");
        }
        uint32_t cna = 0;
        bool encoded = TryEncodeLdstAddress(7, {4096, 15, true}, cna);
        NS_TEST_ASSERT_MSG_EQ(encoded, true, "CNA24 accepts values just beyond CNA16");
        NS_TEST_ASSERT_MSG_EQ(cna, 0x100010u, "CNA24 node and explicit port mapping");
        encoded = TryEncodeLdstAddress(7, {65535, 254, true}, cna);
        NS_TEST_ASSERT_MSG_EQ(encoded, true, "CNA24 upper boundary");
        NS_TEST_ASSERT_MSG_EQ(cna, 0xffffffu, "CNA24 maximum encoded address");
        encoded = TryEncodeLdstAddress(9, {4095, 14, true}, cna);
        NS_TEST_ASSERT_MSG_EQ(encoded, true, "CNA16 upper boundary");
        NS_TEST_ASSERT_MSG_EQ(cna, 0xffffu, "CNA16 maximum encoded address");
        for (uint8_t cfg : {6, 8, 10})
        {
            NS_TEST_ASSERT_MSG_EQ(IsLdstAddressEncodable(cfg, {0, 0, false}),
                                  false,
                                  "Unsupported send CFG is rejected");
        }
    }
};

class LdstObservationTest : public TestCase
{
  public:
    LdstObservationTest()
        : TestCase("Switch observation preserves CNA16 and CNA24 CCI bytes and tags")
    {
    }

  private:
    void DoRun() override
    {
        for (auto cfg : {UbDatalinkHeaderConfig::PACKET_UB_MEM,
                         UbDatalinkHeaderConfig::PACKET_CNA16,
                         UbDatalinkHeaderConfig::PACKET_CNA24})
        {
            // Deserialize known nonzero CCI bytes through the production Header parser.
            const uint8_t wire[] = {0x71, 0, 0x81, 0, 0xa5, 0x12, 3, 0x72};
            auto packet = Create<Packet>(64);
            UbCompactTransactionHeader tah;
            tah.SetUbTransactionOpcode(UbTransactionOpcode::WRITE);
            packet->AddHeader(tah);
            if (cfg == UbDatalinkHeaderConfig::PACKET_CNA24)
            {
                const uint8_t wire24[] = {0, 7, 1, 0, 8, 1, 0xa5, 0x12, 3, 0x74};
                auto networkPacket = Create<Packet>(wire24, sizeof(wire24));
                UbCna24NetworkHeader nth;
                networkPacket->RemoveHeader(nth);
                packet->AddHeader(nth);
            }
            else
            {
                auto networkPacket = Create<Packet>(wire, sizeof(wire));
                UbCna16NetworkHeader nth;
                networkPacket->RemoveHeader(nth);
                packet->AddHeader(nth);
            }
            UbDataLink::AddPacketHeader(packet,
                                        false,
                                        false,
                                        7,
                                        7,
                                        RoutingType::PER_FLOW_SHORTEST_PATHS,
                                        cfg);
            packet->AddPacketTag(UbFlowTag(41, 64));
            const auto before = PacketHex(packet);
            auto sw = CreateObject<UbSwitch>();
            const auto key = sw->GetLdstRoutingKeyForTest(packet);
            NS_TEST_ASSERT_MSG_EQ(key.sip, utils::NodeIdToIp(7, 0).Get(), "Source port zero");
            NS_TEST_ASSERT_MSG_EQ(key.dip, utils::NodeIdToIp(8, 0).Get(), "Destination port zero");
            NS_TEST_ASSERT_MSG_EQ(PacketHex(packet), before, "Observation cannot clear CCI");
            UbFlowTag tag;
            NS_TEST_ASSERT_MSG_EQ(packet->PeekPacketTag(tag), true, "Packet tag retained");
            NS_TEST_ASSERT_MSG_EQ(tag.GetFlowId(), 41u, "Tag content retained");
        }
    }
};

#ifndef _WIN32
// In the child, prove the public Api abort occurs before send progress changes.
const UbLdstTaskSegment* g_rejectedSegment = nullptr;

void
CheckAbortProgress(int)
{
    std::_Exit(g_rejectedSegment->GetBytesLeft() == 64 ? 42 : 43);
}

class LdstApiAddressFailureTest : public TestCase
{
  public:
    LdstApiAddressFailureTest(uint8_t config = 9)
        : TestCase("Api rejects unencodable CFG" + std::to_string(config) +
                   " before send progress"),
          m_config(config)
    {
    }

  private:
    void DoRun() override
    {
        for (unsigned kind = 0; kind < 4; ++kind)
        {
            pid_t child = fork();
            NS_TEST_ASSERT_MSG_NE(child, -1, "Fork succeeds");
            if (child == 0)
            {
                auto api = CreateObject<UbLdstApi>();
                api->SetAttribute("HeaderConfig", StringValue(std::to_string(m_config)));
                const uint32_t overflowNode = m_config == 7 ? 65536 : 4096;
                const uint32_t overflowPort = m_config == 7 ? 255 : 15;
                auto segment = CreateObject<UbLdstTaskSegment>();
                segment->SetSrc(kind == 0 ? overflowNode : 0);
                segment->SetDest(kind == 1 ? overflowNode : 1);
                if (kind == 2)
                {
                    segment->SetSrcPortId(overflowPort);
                }
                if (kind == 3)
                {
                    segment->SetDstPortId(overflowPort);
                }
                segment->SetSize(64);
                segment->SetPacketInfo(64, 0);
                segment->SetPriority(7);
                g_rejectedSegment = PeekPointer(segment);
                std::signal(SIGABRT, CheckAbortProgress);
                api->LdstProcess(segment);
                std::_Exit(1);
            }
            int status = 0;
            const auto waited = waitpid(child, &status, 0);
            NS_TEST_ASSERT_MSG_EQ(waited, child, "Child collected");
            NS_TEST_ASSERT_MSG_EQ(WIFEXITED(status), true, "Abort progress handler ran");
            NS_TEST_ASSERT_MSG_EQ(WEXITSTATUS(status), 42, "Address abort precedes progress");
        }
    }

    uint8_t m_config;
};
#endif

class LdstConfigStoreTest : public TestCase
{
  public:
    LdstConfigStoreTest(bool endpoint = false)
        : TestCase(std::string(endpoint ? "Endpoint" : "Api") +
                   " ConfigStore accepts only 7 and 9"),
          m_endpoint(endpoint)
    {
    }

  private:
    void DoRun() override
    {
        const std::string type = m_endpoint ? "ns3::UbLdstEndpoint" : "ns3::UbLdstApi";
        const auto filename = CreateTempDirFilename("header-config.txt");
        for (int cfg : {7, 9})
        {
            {
                std::ofstream file(filename);
                file << "default " << type << "::HeaderConfig \"" << cfg << "\"\n";
            }
            utils::UbUtils::Get()->SetComponentsAttribute(filename);
            Ptr<Object> object = m_endpoint ? StaticCast<Object>(CreateObject<UbLdstEndpoint>())
                                            : StaticCast<Object>(CreateObject<UbLdstApi>());
            StringValue value;
            object->GetAttribute("HeaderConfig", value);
            NS_TEST_ASSERT_MSG_EQ(value.Get(), std::to_string(cfg), "Text reaches real attribute");
        }
#ifndef _WIN32
        for (int cfg : {6, 8, 10})
        {
            {
                std::ofstream file(filename);
                file << "default " << type << "::HeaderConfig \"" << cfg << "\"\n";
            }
            pid_t child = fork();
            NS_TEST_ASSERT_MSG_NE(child, -1, "Fork succeeds");
            if (child == 0)
            {
                utils::UbUtils::Get()->SetComponentsAttribute(filename);
                std::_Exit(0);
            }
            int status = 0;
            const auto waited = waitpid(child, &status, 0);
            NS_TEST_ASSERT_MSG_EQ(waited, child, "Child collected");
            NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), true, "Invalid text aborts ConfigStore");
            NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status), SIGABRT, "No silent default fallback");
        }
#endif
    }

    bool m_endpoint;
};

class LdstDispatchTest : public TestCase
{
  public:
    LdstDispatchTest()
        : TestCase("CFG7 dispatch reserves NLP4 for LDST and preserves CTP and RTP")
    {
    }

  private:
    void DoRun() override
    {
        auto sw = CreateObject<UbSwitch>();
        for (uint8_t nlp = 0; nlp < 8; ++nlp)
        {
            auto packet = Create<Packet>(64);
            UbCna24NetworkHeader nth;
            nth.SetNlp(nlp);
            packet->AddHeader(nth);
            UbDataLink::AddPacketHeader(packet,
                                        false,
                                        false,
                                        7,
                                        7,
                                        RoutingType::PER_FLOW_SHORTEST_PATHS,
                                        UbDatalinkHeaderConfig::PACKET_CNA24);
            const auto expected =
                nlp == 0 ? UB_CTP_DATA_PACKET : (nlp == 4 ? UB_LDST_DATA_PACKET : UNKOWN_TYPE);
            NS_TEST_ASSERT_MSG_EQ(sw->GetPacketTypeForTest(packet), expected, "Exact NLP dispatch");
        }
        for (uint8_t nlp : {0, 2})
        {
            auto packet = Create<Packet>(64);
            UbCna16NetworkHeader nth;
            nth.SetNlp(nlp);
            packet->AddHeader(nth);
            UbDataLink::AddPacketHeader(packet,
                                        false,
                                        false,
                                        7,
                                        7,
                                        RoutingType::PER_FLOW_SHORTEST_PATHS,
                                        UbDatalinkHeaderConfig::PACKET_CNA16);
            NS_TEST_ASSERT_MSG_EQ(sw->GetPacketTypeForTest(packet),
                                  (nlp == 0 ? UB_CTP_DATA_PACKET : UB_LDST_DATA_PACKET),
                                  "Historical CFG6 branch unchanged");
        }
        auto ipv4 = Create<Packet>(64);
        UbDataLink::AddPacketHeader(ipv4,
                                    false,
                                    false,
                                    7,
                                    7,
                                    RoutingType::PER_FLOW_SHORTEST_PATHS,
                                    UbDatalinkHeaderConfig::PACKET_IPV4);
        NS_TEST_ASSERT_MSG_EQ(sw->GetPacketTypeForTest(ipv4),
                              UB_URMA_DATA_PACKET,
                              "IPv4 RTP dispatch unchanged");
    }
};

#ifndef _WIN32
class LdstExplicitPortFailureTest : public TestCase
{
  public:
    LdstExplicitPortFailureTest(bool targetMismatch)
        : TestCase(targetMismatch ? "CFG7 target rejects wrong ingress port"
                                  : "CFG7 Api rejects explicit port without exact route"),
          m_targetMismatch(targetMismatch)
    {
    }

  private:
    void DoRun() override
    {
        pid_t child = fork();
        NS_TEST_ASSERT_MSG_NE(child, -1, "Fork succeeds");
        if (child == 0)
        {
            LdstTopology topo;
            topo.Api(topo.source)->SetAttribute("HeaderConfig", StringValue("7"));
            auto segment = CreateObject<UbLdstTaskSegment>();
            segment->SetSrc(topo.source->GetId());
            segment->SetDest(topo.target->GetId());
            segment->SetDstPortId(1);
            segment->SetSize(64);
            segment->SetPacketInfo(64, 0);
            segment->SetPriority(7);
            if (m_targetMismatch)
            {
                AddShortestRoute(topo.source, topo.target->GetId(), 1, 0);
                AddShortestRoute(topo.middle, topo.target->GetId(), 1, 1);
            }
            topo.Api(topo.source)->LdstProcess(segment);
            Simulator::Stop(MilliSeconds(1));
            Simulator::Run();
            std::_Exit(0);
        }
        int status = 0;
        const auto waited = waitpid(child, &status, 0);
        NS_TEST_ASSERT_MSG_EQ(waited, child, "Child collected");
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), true, "Explicit port violation rejected");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status), SIGABRT, "Exact port contract fails fast");
    }

    bool m_targetMismatch;
};
#endif

class LdstMiddleExactRouteTest : public TestCase
{
  public:
    LdstMiddleExactRouteTest()
        : TestCase("CFG7 middle Switch refuses primary fallback for explicit port")
    {
    }

  private:
    void DoRun() override
    {
        utils::UbUtils::ResetRuntimeDropDiagnostics();
        LdstTopology topo;
        topo.Api(topo.source)->SetAttribute("HeaderConfig", StringValue("7"));
        AddShortestRoute(topo.source, topo.target->GetId(), 1, 0);
        auto segment = CreateObject<UbLdstTaskSegment>();
        segment->SetSrc(topo.source->GetId());
        segment->SetDest(topo.target->GetId());
        segment->SetDstPortId(1);
        segment->SetSize(64);
        segment->SetPacketInfo(64, 0);
        segment->SetPriority(7);
        topo.Api(topo.source)->LdstProcess(segment);
        Simulator::Stop(MilliSeconds(1));
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::GetRuntimePacketDropCount(),
                              1u,
                              "Middle cannot use its existing primary or port-zero route");
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::GetRuntimePacketDropReason(),
                              "route cannot be found",
                              "Exact route failure is the cause");
        Simulator::Destroy();
        utils::UbUtils::ResetRuntimeDropDiagnostics();
    }
};

class LdstIndependentEntriesTest : public TestCase
{
  public:
    LdstIndependentEntriesTest(uint8_t apiConfig, bool load)
        : TestCase("Simultaneous Api CFG" + std::to_string(apiConfig) + " and Endpoint CFG" +
                   std::to_string(apiConfig == 7 ? 9 : 7) + (load ? " LOAD" : " STORE")),
          m_apiConfig(apiConfig),
          m_load(load)
    {
    }

  private:
    void Observe(Ptr<Packet> packet)
    {
        UbDatalinkHeader dl;
        packet->PeekHeader(dl);
        if (dl.IsControlCreditHeader())
        {
            return;
        }
        auto view = packet->Copy();
        UbDatalinkPacketHeader header;
        view->RemoveHeader(header);
        RemoveLdstNetworkHeader(view, header.GetConfig());
        UbDummyTransactionHeader dummy;
        view->PeekHeader(dummy);
        uint16_t requestId = 0;
        if (dummy.GetUbTransactionOpcode() == uint8_t(UbTransactionOpcode::WRITE) ||
            dummy.GetUbTransactionOpcode() == uint8_t(UbTransactionOpcode::READ))
        {
            UbCompactTransactionHeader tah;
            view->RemoveHeader(tah);
            requestId = tah.GetIniTaSsn();
        }
        else
        {
            UbCompactAckTransactionHeader tah;
            view->RemoveHeader(tah);
            requestId = tah.GetIniTaSsn();
        }
        const bool endpoint = requestId == 123;
        const uint8_t expected = endpoint ? (m_apiConfig == 7 ? 9 : 7) : m_apiConfig;
        NS_TEST_ASSERT_MSG_EQ(header.GetConfig(),
                              expected,
                              "Both source entries and replies keep their independent format");
        if (endpoint)
        {
            ++m_endpointPackets;
        }
        else
        {
            ++m_apiPackets;
        }
    }

    void ApiComplete(uint32_t id)
    {
        NS_TEST_ASSERT_MSG_EQ(id, 41u, "Api task identity");
        ++m_apiCompleted;
    }

    void EndpointComplete(uint16_t id, bool success, Ptr<Packet> payload)
    {
        NS_TEST_ASSERT_MSG_EQ(id, 123u, "Endpoint identity");
        NS_TEST_ASSERT_MSG_EQ(success, true, "Endpoint real response");
        NS_TEST_ASSERT_MSG_EQ(payload->GetSize(), (m_load ? 512u : 0u), "Response size");
        ++m_endpointCompleted;
    }

    void DoRun() override
    {
        LdstTopology topo;
        auto endpoint = topo.source->GetObject<UbController>()->GetLdstEndpoint();
        topo.Api(topo.source)
            ->SetAttribute("HeaderConfig", StringValue(std::to_string(m_apiConfig)));
        endpoint->SetAttribute("HeaderConfig", StringValue(m_apiConfig == 7 ? "9" : "7"));
        topo.Api(topo.target)
            ->SetAttribute("HeaderConfig", StringValue(m_apiConfig == 7 ? "9" : "7"));
        for (auto node : {topo.source, topo.target})
        {
            node->GetDevice(0)->TraceConnectWithoutContext(
                "PktRcvNotify",
                MakeCallback(&LdstIndependentEntriesTest::Observe, this));
        }
        auto instance = CreateObject<UbLdstInstance>();
        topo.source->AggregateObject(instance);
        instance->SetAttribute("ThreadNum", UintegerValue(1));
        instance->Init(topo.source->GetId());
        instance->SetClientCallback(MakeCallback(&LdstIndependentEntriesTest::ApiComplete, this));
        instance->HandleLdstTask(topo.source->GetId(),
                                 topo.target->GetId(),
                                 512,
                                 41,
                                 7,
                                 m_load ? UbMemOperationType::LOAD : UbMemOperationType::STORE,
                                 {0},
                                 0);
        UbLdstRequest request;
        request.requestId = 123;
        request.destNode = topo.target->GetId();
        request.type = m_load ? UbMemOperationType::LOAD : UbMemOperationType::STORE;
        request.requestBytes = m_load ? 64 : 512;
        request.responseBytes = m_load ? 512 : 0;
        request.priority = 7;
        request.completion = MakeCallback(&LdstIndependentEntriesTest::EndpointComplete, this);
        const bool admitted = endpoint->SendRequest(request);
        NS_TEST_ASSERT_MSG_EQ(admitted, true, "Endpoint admits alongside Api work");
        Simulator::Stop(MilliSeconds(1));
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(m_apiCompleted, 1u, "Api completes exactly once");
        NS_TEST_ASSERT_MSG_EQ(m_endpointCompleted, 1u, "Endpoint completes exactly once");
        NS_TEST_ASSERT_MSG_EQ(m_apiPackets, 2u, "Api request and response observed");
        NS_TEST_ASSERT_MSG_EQ(m_endpointPackets, 2u, "Endpoint request and response observed");
        NS_TEST_ASSERT_MSG_EQ(instance->GetActiveTaskCount(), 0u, "Api task retired");
        Simulator::Destroy();
    }

    uint8_t m_apiConfig;
    bool m_load;
    uint32_t m_apiPackets{0};
    uint32_t m_endpointPackets{0};
    uint32_t m_apiCompleted{0};
    uint32_t m_endpointCompleted{0};
};

class LdstCfgSuite : public TestSuite
{
  public:
    LdstCfgSuite()
        : TestSuite("unified-bus-ldst-cfg", Type::UNIT)
    {
        for (uint8_t apiConfig : {7, 9})
        {
            for (bool load : {false, true})
            {
                AddTestCase(new LdstIndependentEntriesTest(apiConfig, load),
                            TestCase::Duration::QUICK);
            }
        }
        for (bool load : {false, true})
        {
            for (bool delayed : {false, true})
            {
                AddTestCase(new LdstRoundTripTest(true, load, 7, false, delayed),
                            TestCase::Duration::QUICK);
            }
            AddTestCase(new LdstRoundTripTest(true, load, 9, false, true),
                        TestCase::Duration::QUICK);
        }
        for (bool load : {false, true})
        {
            for (bool explicitPorts : {false, true})
            {
                for (bool delayed : {false, true})
                {
                    AddTestCase(new LdstRoundTripTest(false, load, 7, explicitPorts, delayed),
                                TestCase::Duration::QUICK);
                }
            }
        }
        AddTestCase(new LdstConfigStoreTest(true), TestCase::Duration::QUICK);
        AddTestCase(new LdstConfigStoreTest(), TestCase::Duration::QUICK);
        AddTestCase(new LdstMiddleExactRouteTest(), TestCase::Duration::QUICK);
        AddTestCase(new LdstDispatchTest(), TestCase::Duration::QUICK);
#ifndef _WIN32
        AddTestCase(new LdstApiAddressFailureTest(7), TestCase::Duration::QUICK);
        AddTestCase(new LdstExplicitPortFailureTest(false), TestCase::Duration::QUICK);
        AddTestCase(new LdstExplicitPortFailureTest(true), TestCase::Duration::QUICK);
#endif
        AddTestCase(new LdstAddressTest(), TestCase::Duration::QUICK);
        AddTestCase(new LdstObservationTest(), TestCase::Duration::QUICK);
#ifndef _WIN32
        AddTestCase(new LdstApiAddressFailureTest(), TestCase::Duration::QUICK);
#endif
        for (bool endpoint : {false, true})
        {
            for (bool load : {false, true})
            {
                AddTestCase(new LdstRoundTripTest(endpoint, load), TestCase::Duration::QUICK);
            }
        }
    }
} g_ldstCfgSuite;
} // namespace
