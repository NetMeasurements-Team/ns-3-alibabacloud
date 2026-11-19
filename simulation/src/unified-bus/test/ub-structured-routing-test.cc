// SPDX-License-Identifier: GPL-2.0-only

#include "ns3/config.h"
#include "ns3/enum.h"
#include "ns3/simulator.h"
#include "ns3/test.h"
#include "ns3/ub-next-hop-index.h"
#include "ns3/ub-routing-process.h"
#include "ns3/ub-rule-routing.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-utils.h"

#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace ns3;

namespace
{

void
AppendUint16(std::string& output, uint16_t value)
{
    output.push_back(static_cast<char>(value));
    output.push_back(static_cast<char>(value >> 8));
}

void
AppendUint32(std::string& output, uint32_t value)
{
    for (uint32_t index = 0; index < 4; ++index)
    {
        output.push_back(static_cast<char>(value >> (index * 8)));
    }
}

void
AppendUint64(std::string& output, uint64_t value)
{
    for (uint32_t index = 0; index < 8; ++index)
    {
        output.push_back(static_cast<char>(value >> (index * 8)));
    }
}

uint32_t
CalculateFixtureCrc32(const std::string& bytes)
{
    uint32_t crc = 0xffffffffU;
    for (unsigned char byte : bytes)
    {
        crc ^= byte;
        for (uint32_t bit = 0; bit < 8; ++bit)
        {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xedb88320U & mask);
        }
    }
    return crc ^ 0xffffffffU;
}

void
ReplaceUint32(std::string& output, std::size_t offset, uint32_t value)
{
    for (uint32_t index = 0; index < 4; ++index)
    {
        output[offset + index] = static_cast<char>(value >> (index * 8));
    }
}

void
RewriteFixtureCrc32(std::string& artifact)
{
    constexpr std::size_t headerSize = 88;
    constexpr std::size_t crcOffset = 84;
    const std::string payload = artifact.substr(headerSize);
    ReplaceUint32(artifact, crcOffset, CalculateFixtureCrc32(payload));
}

std::string
BuildCompressedRoutingFixture(bool corruptChecksum = false,
                              uint32_t shortestCount = 2,
                              uint16_t firstShortestPort = 2,
                              uint64_t extraDeclaredPayloadBytes = 0,
                              uint16_t secondShortestPort = 3,
                              uint16_t nonShortestPort = 4)
{
    constexpr uint32_t nodeCount = 44;
    constexpr uint32_t invalid = UINT32_MAX;
    std::string payload;
    for (uint32_t nodeId = 0; nodeId < nodeCount; ++nodeId)
    {
        AppendUint32(payload, nodeId == 7 ? 0 : invalid);
    }
    for (uint32_t nodeId = 0; nodeId < nodeCount; ++nodeId)
    {
        AppendUint32(payload, nodeId == 42 ? 0 : invalid);
    }
    for (uint32_t address = 0; address < nodeCount * 256; ++address)
    {
        AppendUint32(payload, address == 42 * 256 + 2 ? 0 : invalid);
    }
    AppendUint32(payload, 0);
    AppendUint32(payload, 0);
    AppendUint32(payload, shortestCount);
    AppendUint32(payload, 1);
    AppendUint16(payload, firstShortestPort);
    AppendUint16(payload, secondShortestPort);
    AppendUint16(payload, nonShortestPort);
    payload.push_back(1);
    payload.push_back(1);

    std::string artifact("UBCRIDX\0", 8);
    AppendUint16(artifact, 1);
    AppendUint16(artifact, 0);
    AppendUint32(artifact, 0x01020304);
    AppendUint32(artifact, 88);
    AppendUint32(artifact, nodeCount);
    AppendUint32(artifact, 1);
    AppendUint32(artifact, 1);
    AppendUint32(artifact, 1);
    AppendUint32(artifact, 2);
    artifact.push_back(1);
    artifact.push_back(1);
    AppendUint16(artifact, 0);
    artifact.append(32, '\0');
    AppendUint64(artifact, payload.size() + extraDeclaredPayloadBytes);
    AppendUint32(artifact, CalculateFixtureCrc32(payload) ^ (corruptChecksum ? 1U : 0U));
    artifact += payload;
    return artifact;
}

std::vector<std::vector<bool>>
BuildFixtureConnectedPorts()
{
    std::vector<std::vector<bool>> ports(44, std::vector<bool>(16, false));
    ports[7][2] = true;
    ports[7][3] = true;
    ports[7][4] = true;
    return ports;
}

#ifndef _WIN32
int
RunStructuredTestInChild(const std::function<void()>& function)
{
    const pid_t pid = fork();
    if (pid == -1)
    {
        return -1;
    }
    if (pid == 0)
    {
        function();
        std::_Exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
    {
        return -1;
    }
    return status;
}
#endif

class UbRuleRoutingTableTest : public TestCase
{
  public:
    UbRuleRoutingTableTest()
        : TestCase("Structured rule routing preserves exact and range precedence")
    {
    }

    void DoRun() override
    {
        auto rules = std::make_shared<UbRuleRoutingTable>();
        rules->AddPortGroup({7, 10, {2, 3}});
        rules->AddPortGroup({7, 11, {4}});
        rules->AddPortGroup({7, 12, {9}});
        rules->AddRule({7, {42, UbRuleDestination::ANY_PORT}, {10}, {11}});
        rules->AddRule({7, {42, 2}, {12}, {}});
        rules->AddRangeRule({7, 100, 199, 2, {10}, {11}});
        rules->AddRule({7, {150, 2}, {12}, {}});
        rules->PrepareForLookup();

        std::vector<uint16_t> shortestPorts;
        std::vector<uint16_t> nonShortestPorts;
        bool handled = rules->TryLookup(7, 42, 2, shortestPorts, nonShortestPorts);
        NS_TEST_ASSERT_MSG_EQ(handled, true, "exact destination-port rule should be handled");
        NS_TEST_ASSERT_MSG_EQ((shortestPorts == std::vector<uint16_t>({9})),
                              true,
                              "exact rule should win");
        NS_TEST_ASSERT_MSG_EQ(nonShortestPorts.empty(), true, "exact result should be complete");

        handled = rules->TryLookup(7, 42, 3, shortestPorts, nonShortestPorts);
        NS_TEST_ASSERT_MSG_EQ(handled, true, "node rule should serve unmatched ports");
        NS_TEST_ASSERT_MSG_EQ((shortestPorts == std::vector<uint16_t>({2, 3})),
                              true,
                              "node rule ports");
        NS_TEST_ASSERT_MSG_EQ((nonShortestPorts == std::vector<uint16_t>({4})),
                              true,
                              "alternate ports");

        handled = rules->TryLookupExact(7, 42, 3, shortestPorts, nonShortestPorts);
        NS_TEST_ASSERT_MSG_EQ(handled,
                              false,
                              "exact endpoint lookup must not consume an ANY_PORT rule");
        NS_TEST_ASSERT_MSG_EQ(shortestPorts.empty(),
                              true,
                              "exact lookup should clear shortest ports");
        NS_TEST_ASSERT_MSG_EQ(nonShortestPorts.empty(),
                              true,
                              "exact lookup should clear non-shortest ports");

        handled = rules->TryLookup(7, 120, 2, shortestPorts, nonShortestPorts);
        NS_TEST_ASSERT_MSG_EQ(handled, true, "range should own an in-range lookup");
        NS_TEST_ASSERT_MSG_EQ((shortestPorts == std::vector<uint16_t>({2, 3})),
                              true,
                              "range ports");

        handled = rules->TryLookup(7, 150, 2, shortestPorts, nonShortestPorts);
        NS_TEST_ASSERT_MSG_EQ(handled, true, "exact rule should override range");
        NS_TEST_ASSERT_MSG_EQ((shortestPorts == std::vector<uint16_t>({9})),
                              true,
                              "exact range override");

        Ptr<UbRoutingProcess> routing = CreateObject<UbRoutingProcess>();
        routing->SetNodeId(7);
        routing->SetRuleRoutingTable(rules);
        routing->AddShortestRoute(utils::NodeIdToIp(43).Get(), {6});

        RoutingKey key{};
        key.sip = utils::NodeIdToIp(1).Get();
        key.dip = utils::NodeIdToIp(42, 2).Get();
        key.routingType = RoutingType::PER_FLOW_SHORTEST_PATHS;
        bool selectedShortestPath = false;
        NS_TEST_ASSERT_MSG_EQ(routing->GetOutPort(key, selectedShortestPath),
                              9,
                              "rule source should feed the existing selector");

        key.dip = utils::NodeIdToIp(42, 3).Get();
        NS_TEST_ASSERT_MSG_EQ(routing->GetOutPortForEndpoint(key, selectedShortestPath),
                              -1,
                              "endpoint lookup must reject a missing exact destination-port rule");

        key.dip = utils::NodeIdToIp(43).Get();
        NS_TEST_ASSERT_MSG_EQ(routing->GetOutPort(key, selectedShortestPath),
                              6,
                              "rule miss should preserve CSV table fallback");
    }
};

class UbNextHopIndexTest : public TestCase
{
  public:
    UbNextHopIndexTest()
        : TestCase("Packed next-hop index preserves complete table candidates")
    {
    }

    void DoRun() override
    {
        const std::vector<utils::UbStructuredTableRoute> routes = {
            {7, 42, 2, {3, 2}, {4}},
            {7, 42, 3, {5}, {6}},
            {7, 42, 2, {7}, {8}},
        };
        const auto index = UbNextHopIndex::Build(routes, 44);
        NS_TEST_ASSERT_MSG_EQ(index->GetPortIdWidthBits(), 8u, "small IDs should use 8 bits");

        std::vector<uint16_t> shortestPorts;
        std::vector<uint16_t> nonShortestPorts;
        bool handled = index->TryLookup(7, 42, 2, shortestPorts, nonShortestPorts);
        NS_TEST_ASSERT_MSG_EQ(handled, true, "exact destination port should be indexed");
        NS_TEST_ASSERT_MSG_EQ((shortestPorts == std::vector<uint16_t>({2, 3, 7})),
                              true,
                              "duplicate rows should merge shortest candidates");
        NS_TEST_ASSERT_MSG_EQ((nonShortestPorts == std::vector<uint16_t>({4, 8})),
                              true,
                              "duplicate rows should merge non-shortest candidates");

        handled = index->TryLookup(7, 42, 10, shortestPorts, nonShortestPorts);
        NS_TEST_ASSERT_MSG_EQ(handled, false, "exact lookup must not hide a missing port");

        handled =
            index->TryLookup(7, 42, UbRuleDestination::ANY_PORT, shortestPorts, nonShortestPorts);
        NS_TEST_ASSERT_MSG_EQ(handled, true, "primary address should aggregate destination ports");
        NS_TEST_ASSERT_MSG_EQ((shortestPorts == std::vector<uint16_t>({2, 3, 5, 7})),
                              true,
                              "primary address should aggregate shortest candidates");

        Ptr<UbRoutingProcess> routing = CreateObject<UbRoutingProcess>();
        routing->SetNodeId(7);
        routing->SetNextHopIndex(index);
        routing->GetShortestOutPorts(utils::NodeIdToIp(42, 2).Get(), shortestPorts);
        NS_TEST_ASSERT_MSG_EQ((shortestPorts == std::vector<uint16_t>({2, 3, 7})),
                              true,
                              "direct lookup should use the packed exact index");

        routing->GetShortestOutPorts(utils::NodeIdToIp(42, 10).Get(), shortestPorts);
        NS_TEST_ASSERT_MSG_EQ(shortestPorts.empty(),
                              true,
                              "direct lookup should not hide a missing destination port");

        routing->AddShortestRouteRange(40, 45, 2, {11});
        routing->GetShortestOutPorts(utils::NodeIdToIp(42, 2).Get(), shortestPorts);
        NS_TEST_ASSERT_MSG_EQ((shortestPorts == std::vector<uint16_t>({2, 3, 7})),
                              true,
                              "indexed exact route should take precedence over a CSV range");
        routing->GetShortestOutPorts(utils::NodeIdToIp(43, 2).Get(), shortestPorts);
        NS_TEST_ASSERT_MSG_EQ((shortestPorts == std::vector<uint16_t>({11})),
                              true,
                              "indexed exact miss should fall back to a CSV range");

        RoutingKey key{};
        key.sip = utils::NodeIdToIp(1).Get();
        key.dip = utils::NodeIdToIp(42, 10).Get();
        key.routingType = RoutingType::PER_FLOW_SHORTEST_PATHS;
        bool selectedShortestPath = false;
        const int outPort = routing->GetOutPort(key, selectedShortestPath);
        const bool foundOutPort = outPort >= 0;
        const bool usedAggregatedShortestPort =
            outPort == 2 || outPort == 3 || outPort == 5 || outPort == 7;
        NS_TEST_ASSERT_MSG_EQ(foundOutPort,
                              true,
                              "packet lookup should preserve primary-address fallback");
        NS_TEST_ASSERT_MSG_EQ(usedAggregatedShortestPort,
                              true,
                              "primary-address fallback should use aggregated shortest candidates");

        const auto fallbackIndex = UbNextHopIndex::Build(
            {
                {7, 42, 2, {2}, {}},
                {7, 42, 3, {5}, {6}},
            },
            44);
        auto fallbackRules = std::make_shared<UbRuleRoutingTable>();
        fallbackRules->AddPortGroup({7, 20, {2}});
        fallbackRules->AddPortGroup({7, 21, {5}});
        fallbackRules->AddPortGroup({7, 22, {6}});
        fallbackRules->AddRule({7, {42, 2}, {20}, {}});
        fallbackRules->AddRule({7, {42, 3}, {21}, {22}});
        fallbackRules->AddTopologyLink({42, 2, 100, 0});
        fallbackRules->AddTopologyLink({42, 3, 101, 0});
        fallbackRules->PrepareForLookup();

        Ptr<UbRoutingProcess> indexedRouting = CreateObject<UbRoutingProcess>();
        indexedRouting->SetNodeId(7);
        indexedRouting->SetNextHopIndex(fallbackIndex);
        indexedRouting->SetAttribute("MultipathSelector",
                                     EnumValue(MultipathSelector::ROUND_ROBIN));
        Ptr<UbRoutingProcess> ruleRouting = CreateObject<UbRoutingProcess>();
        ruleRouting->SetNodeId(7);
        ruleRouting->SetRuleRoutingTable(fallbackRules);
        ruleRouting->SetAttribute("MultipathSelector", EnumValue(MultipathSelector::ROUND_ROBIN));

        key.dip = utils::NodeIdToIp(42, 2).Get();
        key.routingType = RoutingType::PER_PACKET_ALL_PATHS;
        bool indexedShortest = false;
        bool ruleShortest = false;
        const int indexedPort = indexedRouting->GetOutPort(key, indexedShortest);
        const int rulePort = ruleRouting->GetOutPort(key, ruleShortest);
        NS_TEST_ASSERT_MSG_EQ(indexedPort,
                              rulePort,
                              "rule and index should fill missing candidate groups identically");
        NS_TEST_ASSERT_MSG_EQ(indexedShortest,
                              ruleShortest,
                              "rule and index should agree on detour selection");
        NS_TEST_ASSERT_MSG_EQ(
            indexedRouting->GetOutPort(key, indexedShortest),
            ruleRouting->GetOutPort(key, ruleShortest),
            "round robin should see the same second candidate without ANY_PORT rules");
        NS_TEST_ASSERT_MSG_EQ(indexedShortest,
                              ruleShortest,
                              "round robin detour marking should match without ANY_PORT rules");

        key.routingType = RoutingType::PER_PACKET_SHORTEST_PATHS;
        NS_TEST_ASSERT_MSG_EQ(indexedRouting->GetOutPort(key, indexedShortest, 2),
                              -1,
                              "ingress filtering must happen after primary fallback");
        NS_TEST_ASSERT_MSG_EQ(ruleRouting->GetOutPort(key, ruleShortest, 2),
                              -1,
                              "rule ingress filtering must match the index");

        key.routingType = RoutingType::PER_PACKET_ALL_PATHS;
        NS_TEST_ASSERT_MSG_EQ(indexedRouting->GetOutPort(key, indexedShortest, 2),
                              6,
                              "a valid non-shortest candidate remains after ingress filtering");
        NS_TEST_ASSERT_MSG_EQ(ruleRouting->GetOutPort(key, ruleShortest, 2),
                              6,
                              "rule and index should use the same remaining candidate");
        NS_TEST_ASSERT_MSG_EQ(indexedShortest,
                              false,
                              "the remaining candidate must retain non-shortest semantics");
        NS_TEST_ASSERT_MSG_EQ(ruleShortest, false, "rule detour marking must match the index");

        key.dip = utils::NodeIdToIp(42).Get();
        NS_TEST_ASSERT_MSG_EQ(indexedRouting->GetOutPort(key, indexedShortest),
                              ruleRouting->GetOutPort(key, ruleShortest),
                              "node-primary lookup should match across runtime modes");
    }
};

class UbCsvRoutingMetricAggregationTest : public TestCase
{
  public:
    UbCsvRoutingMetricAggregationTest()
        : TestCase("CSV routing preserves global node and exact-port metric groups")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        const std::filesystem::path directory =
            std::filesystem::path(CreateTempDirFilename("ub-routing-metric-aggregation"));
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        NS_TEST_ASSERT_MSG_EQ(error.value(), 0, "temporary directory creation should succeed");

        const auto nodeFile = directory / "node.csv";
        std::ofstream nodes(nodeFile);
        nodes << "nodeId,nodeType,portNum,allocationDelay,forwardDelay\n"
              << "0,DEVICE,4,1ns,0ns\n"
              << "1,DEVICE,2,1ns,0ns\n";
        nodes.close();
        utils::UbUtils::Get()->CreateNode(nodeFile.string());

        const auto routeFile = directory / "routing_table.csv";
        std::ofstream routes(routeFile);
        routes << "nodeId,dstNodeId,dstPortId,outPorts,metrics\n"
               << "0,1,0,0,1\n"
               << "0,1,0,1,3\n"
               << "0,1,1,2 3,2 4\n";
        routes.close();
        utils::UbUtils::Get()->AddRoutingTable(routeFile.string());

        auto routing = NodeList::GetNode(0)->GetObject<UbSwitch>()->GetRoutingProcess();
        std::vector<uint16_t> shortestPorts;
        std::vector<uint16_t> nonShortestPorts;
        routing->GetShortestOutPorts(utils::NodeIdToIp(1, 0).Get(), shortestPorts);
        routing->GetOtherOutPorts(utils::NodeIdToIp(1, 0).Get(), nonShortestPorts);
        NS_TEST_ASSERT_MSG_EQ((shortestPorts == std::vector<uint16_t>({0})),
                              true,
                              "duplicate exact rows must share one minimum metric");
        NS_TEST_ASSERT_MSG_EQ((nonShortestPorts == std::vector<uint16_t>({1})),
                              true,
                              "duplicate exact rows must retain higher metrics");

        routing->GetShortestOutPorts(utils::NodeIdToIp(1).Get(), shortestPorts);
        routing->GetOtherOutPorts(utils::NodeIdToIp(1).Get(), nonShortestPorts);
        NS_TEST_ASSERT_MSG_EQ((shortestPorts == std::vector<uint16_t>({0})),
                              true,
                              "node-primary route must use the global minimum metric");
        NS_TEST_ASSERT_MSG_EQ((nonShortestPorts == std::vector<uint16_t>({1, 2, 3})),
                              true,
                              "node-primary route must combine all higher metric groups");

        Simulator::Destroy();
        Config::Reset();
        std::filesystem::remove_all(directory, error);
    }
};

class UbCompressedNextHopArtifactTest : public TestCase
{
  public:
    UbCompressedNextHopArtifactTest()
        : TestCase("Portable compressed next-hop artifact loads without rebuilding routes")
    {
    }

    void DoRun() override
    {
        std::stringstream input(BuildCompressedRoutingFixture());
        const auto index =
            UbNextHopIndex::Read(input, BuildFixtureConnectedPorts(), std::string(64, '0'));
        std::vector<uint16_t> shortestPorts;
        std::vector<uint16_t> nonShortestPorts;
        NS_TEST_ASSERT_MSG_EQ(index->TryLookup(7, 42, 2, shortestPorts, nonShortestPorts),
                              true,
                              "binary exact route should resolve");
        NS_TEST_ASSERT_MSG_EQ((shortestPorts == std::vector<uint16_t>({2, 3})),
                              true,
                              "binary shortest candidates");
        NS_TEST_ASSERT_MSG_EQ((nonShortestPorts == std::vector<uint16_t>({4})),
                              true,
                              "binary non-shortest candidates");
        NS_TEST_ASSERT_MSG_EQ(
            index->TryLookup(7, 42, UbRuleDestination::ANY_PORT, shortestPorts, nonShortestPorts),
            true,
            "binary node-primary route should resolve");
    }
};

class UbStructuredTopologyInstallTest : public TestCase
{
  public:
    UbStructuredTopologyInstallTest()
        : TestCase("Prepared structured protocol installs topology and routing")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Time::SetResolution(Time::PS);
        std::stringstream payload;
        payload << "NS3_UB_STRUCTURED 3\n"
                << "NODES 2\n"
                << "1 DEVICE 1 1ns 2ns 0\n"
                << "0 DEVICE 1 1ns 2ns 0\n"
                << "LINKS 1\n"
                << "0 0 1 0 400Gbps 10ns\n"
                << "TABLE 1\n"
                << "0 1 0 1 0 0\n"
                << "PORT_GROUPS 1\n"
                << "1 0 1 0\n"
                << "RULES 1\n"
                << "1 0 0 1 0 0\n"
                << "RULE_RANGES 0\n"
                << "TOPOLOGY_RULES 1\n"
                << "dimension 1 2 1 fullMesh 2\n"
                << "0 HOST 1 0\n"
                << "1 HOST 1 1\n"
                << "END\n";

        const auto config = utils::UbUtils::ReadStructuredTopologyConfig(payload);
        NS_TEST_ASSERT_MSG_EQ(config.nodes.size(), 2u, "parser should retain both nodes");
        NS_TEST_ASSERT_MSG_EQ(config.nodes[0].nodeId,
                              0u,
                              "parser should normalize unambiguous node record order");
        NS_TEST_ASSERT_MSG_EQ(config.nodes[1].nodeId,
                              1u,
                              "parser should preserve canonical node identity after normalization");
        NS_TEST_ASSERT_MSG_EQ(config.tableRouteCount, 1u, "parser should count table routes");
        NS_TEST_ASSERT_MSG_EQ((config.tableNextHopIndex != nullptr),
                              true,
                              "table should compile while reading");
        NS_TEST_ASSERT_MSG_EQ(config.topologyRules.size(),
                              1u,
                              "parser should retain topology rules");

        utils::UbUtils::Get()->InstallStructuredTopology(config, Time(0), 1);
        const auto tablePorts =
            NodeList::GetNode(0)->GetObject<UbSwitch>()->GetRoutingProcess()->GetAllOutPorts(
                utils::NodeIdToIp(1, 0).Get());
        const auto rulePorts =
            NodeList::GetNode(1)->GetObject<UbSwitch>()->GetRoutingProcess()->GetAllOutPorts(
                utils::NodeIdToIp(0, 0).Get());
        NS_TEST_ASSERT_MSG_EQ((tablePorts == std::vector<uint16_t>({0})),
                              true,
                              "table route should resolve");
        NS_TEST_ASSERT_MSG_EQ((rulePorts == std::vector<uint16_t>({0})),
                              true,
                              "rule route should resolve");

        Simulator::Destroy();
        Config::Reset();
    }
};

#ifndef _WIN32
class UbStructuredTopologyRejectsInvalidInputTest : public TestCase
{
  public:
    UbStructuredTopologyRejectsInvalidInputTest()
        : TestCase("Prepared structured protocol rejects invalid input")
    {
    }

    void DoRun() override
    {
        int status = RunStructuredTestInChild([]() {
            std::stringstream payload("NOT_STRUCTURED 1\n");
            (void)utils::UbUtils::ReadStructuredTopologyConfig(payload);
        });
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), 1, "invalid header should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status), SIGABRT, "invalid header should use SIGABRT");

        status = RunStructuredTestInChild([]() {
            std::stringstream payload;
            payload << "NS3_UB_STRUCTURED 1\n"
                    << "NODES 2\n"
                    << "0 DEVICE 1 1ns 0ns 0\n"
                    << "1 DEVICE 1 1ns 0ns 0\n"
                    << "LINKS 1\n"
                    << "0 1 1 0 400Gbps 10ns\n"
                    << "TABLE 0\n"
                    << "PORT_GROUPS 0\n"
                    << "RULES 0\n"
                    << "END\n";
            const auto config = utils::UbUtils::ReadStructuredTopologyConfig(payload);
            utils::UbUtils::Get()->InstallStructuredTopology(config, Time(0), 1);
        });
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), 1, "invalid physical port should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status), SIGABRT, "invalid port should use SIGABRT");

        status = RunStructuredTestInChild([]() {
            std::stringstream payload;
            payload << "NS3_UB_STRUCTURED 1\n"
                    << "NODES 2\n"
                    << "0 DEVICE 1 1ns 0ns 0\n"
                    << "0 DEVICE 1 1ns 0ns 0\n";
            (void)utils::UbUtils::ReadStructuredTopologyConfig(payload);
        });
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), 1, "duplicate node IDs should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status), SIGABRT, "duplicate IDs should use SIGABRT");

        status = RunStructuredTestInChild([]() {
            std::stringstream input(BuildCompressedRoutingFixture(true));
            (void)UbNextHopIndex::Read(input, BuildFixtureConnectedPorts(), std::string(64, '0'));
        });
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), 1, "bad binary checksum should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status), SIGABRT, "bad binary checksum should use SIGABRT");

        status = RunStructuredTestInChild([]() {
            std::stringstream input(BuildCompressedRoutingFixture(false, UINT32_MAX, 2));
            (void)UbNextHopIndex::Read(input, BuildFixtureConnectedPorts(), std::string(64, '0'));
        });
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), 1, "oversized candidate count should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status), SIGABRT, "oversized count should use SIGABRT");

        status = RunStructuredTestInChild([]() {
            std::stringstream input(BuildCompressedRoutingFixture(false, 2, 3, 0, UINT16_MAX));
            (void)UbNextHopIndex::Read(input, BuildFixtureConnectedPorts(), std::string(64, '0'));
        });
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), 1, "out-of-range candidate port should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status), SIGABRT, "invalid port should use SIGABRT");

        status = RunStructuredTestInChild([]() {
            std::stringstream input(BuildCompressedRoutingFixture(false, 2, 2, 1ULL << 32));
            (void)UbNextHopIndex::Read(input, BuildFixtureConnectedPorts(), std::string(64, '0'));
        });
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), 1, "truncated declared payload should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status), SIGABRT, "truncated payload should use SIGABRT");

        status = RunStructuredTestInChild([]() {
            std::stringstream input(BuildCompressedRoutingFixture(false, 2, 2, 0, 5, 6));
            (void)UbNextHopIndex::Read(input, BuildFixtureConnectedPorts(), std::string(64, '0'));
        });
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), 1, "disconnected candidate port should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status), SIGABRT, "disconnected port should use SIGABRT");

        status = RunStructuredTestInChild([]() {
            std::stringstream input(BuildCompressedRoutingFixture(false, 2, 2, 0, 2));
            (void)UbNextHopIndex::Read(input, BuildFixtureConnectedPorts(), std::string(64, '0'));
        });
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), 1, "duplicate candidate port should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status), SIGABRT, "duplicate port should use SIGABRT");

        status = RunStructuredTestInChild([]() {
            std::stringstream input(BuildCompressedRoutingFixture(false, 2, 3, 0, 2));
            (void)UbNextHopIndex::Read(input, BuildFixtureConnectedPorts(), std::string(64, '0'));
        });
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), 1, "unsorted candidate ports should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status), SIGABRT, "unsorted ports should use SIGABRT");

        status = RunStructuredTestInChild([]() {
            std::stringstream input(BuildCompressedRoutingFixture(false, 2, 2, 0, 3, 3));
            (void)UbNextHopIndex::Read(input, BuildFixtureConnectedPorts(), std::string(64, '0'));
        });
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), 1, "overlapping candidate groups should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status), SIGABRT, "overlapping groups should use SIGABRT");

        status = RunStructuredTestInChild([]() {
            UbNextHopIndexBuilder builder(2, 1);
            builder.AddRoute({0, 1, 0, {0}, {1}});
            builder.AddPrimaryRoute(0, 1, {0}, {0, 1});
        });
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status),
                              1,
                              "overlapping node-primary candidate groups should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status),
                              SIGABRT,
                              "overlapping node-primary groups should use SIGABRT");

        status = RunStructuredTestInChild([]() {
            std::string artifact = BuildCompressedRoutingFixture();
            constexpr std::size_t headerSize = 88;
            constexpr std::size_t destinationMapping = headerSize + 44 * sizeof(uint32_t);
            ReplaceUint32(artifact, destinationMapping + 41 * sizeof(uint32_t), 0);
            RewriteFixtureCrc32(artifact);
            std::stringstream input(artifact);
            (void)UbNextHopIndex::Read(input, BuildFixtureConnectedPorts(), std::string(64, '0'));
        });
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), 1, "duplicate destination mapping should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status),
                              SIGABRT,
                              "duplicate destination should use SIGABRT");

        status = RunStructuredTestInChild([]() {
            std::string artifact = BuildCompressedRoutingFixture();
            constexpr std::size_t headerSize = 88;
            constexpr std::size_t destinationMapping = headerSize + 44 * sizeof(uint32_t);
            ReplaceUint32(artifact, destinationMapping + 42 * sizeof(uint32_t), UINT32_MAX);
            RewriteFixtureCrc32(artifact);
            std::stringstream input(artifact);
            (void)UbNextHopIndex::Read(input, BuildFixtureConnectedPorts(), std::string(64, '0'));
        });
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), 1, "missing destination mapping should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status), SIGABRT, "missing destination should use SIGABRT");

        status = RunStructuredTestInChild([]() {
            std::string artifact = BuildCompressedRoutingFixture();
            constexpr std::size_t headerSize = 88;
            constexpr std::size_t endpointMapping = headerSize + 88 * sizeof(uint32_t);
            ReplaceUint32(artifact, endpointMapping + (42 * 256 + 3) * sizeof(uint32_t), 0);
            RewriteFixtureCrc32(artifact);
            std::stringstream input(artifact);
            (void)UbNextHopIndex::Read(input, BuildFixtureConnectedPorts(), std::string(64, '0'));
        });
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), 1, "duplicate endpoint mapping should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status), SIGABRT, "duplicate endpoint should use SIGABRT");

        status = RunStructuredTestInChild([]() {
            std::string artifact = BuildCompressedRoutingFixture();
            constexpr std::size_t headerSize = 88;
            constexpr std::size_t endpointMapping = headerSize + 88 * sizeof(uint32_t);
            ReplaceUint32(artifact,
                          endpointMapping + (42 * 256 + 2) * sizeof(uint32_t),
                          UINT32_MAX);
            RewriteFixtureCrc32(artifact);
            std::stringstream input(artifact);
            (void)UbNextHopIndex::Read(input, BuildFixtureConnectedPorts(), std::string(64, '0'));
        });
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), 1, "missing endpoint mapping should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status), SIGABRT, "missing endpoint should use SIGABRT");

        status = RunStructuredTestInChild([]() {
            std::stringstream payload;
            payload << "NS3_UB_STRUCTURED 4\n"
                    << "NODES 1\n"
                    << "0 DEVICE 1 1ns 0ns 0\n"
                    << "LINKS 0\n"
                    << "ROUTING_FINGERPRINT not-a-sha256\n";
            (void)utils::UbUtils::ReadStructuredTopologyConfig(payload);
        });
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), 1, "invalid routing fingerprint should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status), SIGABRT, "invalid fingerprint should use SIGABRT");
    }
};
#endif

class UbStructuredRoutingTestSuite : public TestSuite
{
  public:
    UbStructuredRoutingTestSuite()
        : TestSuite("unified-bus-structured-routing", Type::UNIT)
    {
        AddTestCase(new UbRuleRoutingTableTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbNextHopIndexTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbCsvRoutingMetricAggregationTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbCompressedNextHopArtifactTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbStructuredTopologyInstallTest(), TestCase::Duration::QUICK);
#ifndef _WIN32
        AddTestCase(new UbStructuredTopologyRejectsInvalidInputTest(), TestCase::Duration::QUICK);
#endif
    }
};

static UbStructuredRoutingTestSuite g_ubStructuredRoutingTestSuite;

} // namespace
