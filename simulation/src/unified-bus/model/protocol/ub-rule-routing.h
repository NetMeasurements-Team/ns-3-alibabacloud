// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_RULE_ROUTING_H
#define UB_RULE_ROUTING_H

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ns3
{

struct UbRuleDestination
{
    static constexpr uint32_t ANY_PORT = std::numeric_limits<uint32_t>::max();

    uint32_t nodeId;
    uint32_t portId{ANY_PORT};
};

struct UbPortGroup
{
    uint32_t nodeId;
    uint32_t groupId;
    std::vector<uint16_t> ports;
};

struct UbRoutingRule
{
    uint32_t currentNodeId;
    UbRuleDestination destination;
    std::vector<uint32_t> shortestPortGroups;
    std::vector<uint32_t> nonShortestPortGroups;
};

struct UbRoutingRangeRule
{
    uint32_t currentNodeId;
    uint32_t destinationStartNodeId;
    uint32_t destinationEndNodeId;
    uint32_t destinationPortId{UbRuleDestination::ANY_PORT};
    std::vector<uint32_t> shortestPortGroups;
    std::vector<uint32_t> nonShortestPortGroups;
};

enum class UbTopologyRuleType
{
    DIMENSION,
    DIRECT_SWITCH,
    TWO_LEVEL_SWITCH,
    DRAGONFLY,
};

enum class UbDimensionConnection
{
    LINE,
    RING,
    FULL_MESH,
};

struct UbTopologyRuleNode
{
    uint32_t nodeId;
    bool isHost;
    std::vector<uint32_t> coordinates;
};

struct UbTopologyRule
{
    UbTopologyRuleType type;
    std::vector<uint32_t> shape;
    std::vector<UbDimensionConnection> connections;
    std::vector<UbTopologyRuleNode> nodes;
};

struct UbTopologyLink
{
    uint32_t node1;
    uint16_t port1;
    uint32_t node2;
    uint16_t port2;
};

// Compiles topology relationships into local output-port candidates without expanding routes for
// every current-node and destination pair.
class UbTopologyNextHopIndex;

class UbRuleRoutingTable
{
  public:
    UbRuleRoutingTable();
    ~UbRuleRoutingTable();

    void AddPortGroup(const UbPortGroup& portGroup);
    void AddRule(const UbRoutingRule& rule);
    void AddRangeRule(const UbRoutingRangeRule& rule);
    void AddTopologyLink(const UbTopologyLink& link);
    void AddTopologyRule(const UbTopologyRule& rule);
    // Builds runtime indexes and releases data used only while adding rules.
    void PrepareForLookup();

    bool TryLookup(uint32_t currentNodeId,
                   uint32_t destinationNodeId,
                   uint32_t destinationPortId,
                   std::vector<uint16_t>& shortestPorts,
                   std::vector<uint16_t>& nonShortestPorts) const;
    bool TryLookupExact(uint32_t currentNodeId,
                        uint32_t destinationNodeId,
                        uint32_t destinationPortId,
                        std::vector<uint16_t>& shortestPorts,
                        std::vector<uint16_t>& nonShortestPorts) const;
    bool TryLookupWithPrimaryFallback(uint32_t currentNodeId,
                                      uint32_t destinationNodeId,
                                      uint32_t destinationPortId,
                                      std::vector<uint16_t>& shortestPorts,
                                      std::vector<uint16_t>& nonShortestPorts) const;

  private:
    friend class UbTopologyNextHopIndex;

    using PortSet = std::shared_ptr<const std::vector<uint16_t>>;

    struct ResolvedRule
    {
        PortSet shortestPorts;
        PortSet nonShortestPorts;
    };

    struct ResolvedRangeRule
    {
        uint32_t destinationStartNodeId;
        uint32_t destinationEndNodeId;
        ResolvedRule candidates;
    };

    struct NodeRules
    {
        std::vector<PortSet> portGroups;
        std::unordered_map<uint64_t, ResolvedRule> exactRules;
        std::unordered_map<uint32_t, std::vector<ResolvedRangeRule>> rangeRulesByPort;
    };

    struct PhysicalNeighbor
    {
        uint32_t peerNodeId;
        uint16_t localPort;
        uint16_t peerPort;
    };

    struct IndexedTopologyRule
    {
        UbTopologyRuleType type;
        std::vector<uint32_t> shape;
        std::vector<UbDimensionConnection> connections;
        std::vector<UbTopologyRuleNode> nodes;
        std::unordered_map<uint32_t, std::size_t> nodeIndex;
        std::vector<uint32_t> hostNodeIds;
        std::unordered_map<uint32_t, uint32_t> attachedRouterByNode;
    };

    NodeRules& GetOrCreateNodeRules(uint32_t currentNodeId);
    PortSet InternPorts(std::vector<uint16_t> ports);
    PortSet ResolvePortGroups(uint32_t currentNodeId, const std::vector<uint32_t>& groupIds);
    ResolvedRule ResolveRule(uint32_t currentNodeId,
                             const std::vector<uint32_t>& shortestPortGroups,
                             const std::vector<uint32_t>& nonShortestPortGroups);
    const ResolvedRule* FindRule(uint32_t currentNodeId,
                                 uint32_t destinationNodeId,
                                 uint32_t destinationPortId) const;
    const ResolvedRule* FindExactRule(uint32_t currentNodeId,
                                      uint32_t destinationNodeId,
                                      uint32_t destinationPortId) const;
    static const ResolvedRule* FindRangeRule(const NodeRules& nodeRules,
                                             uint32_t destinationNodeId,
                                             uint32_t destinationPortId);
    static uint64_t MakeDestinationKey(uint32_t destinationNodeId, uint32_t destinationPortId);
    static uint64_t MakeEndpointKey(uint32_t nodeId, uint32_t portId);
    bool TryLookupTopology(uint32_t currentNodeId,
                           uint32_t destinationNodeId,
                           uint32_t destinationPortId,
                           std::vector<uint16_t>& shortestPorts) const;
    bool TryLookupTopologyPort(uint32_t currentNodeId,
                               uint32_t destinationNodeId,
                               uint32_t destinationPortId,
                               std::vector<uint16_t>& shortestPorts) const;
    bool TryLookupTopologyReference(uint32_t currentNodeId,
                                    uint32_t destinationNodeId,
                                    uint32_t destinationPortId,
                                    std::vector<uint16_t>& shortestPorts) const;
    bool TryLookupTopologyPortReference(uint32_t currentNodeId,
                                        uint32_t destinationNodeId,
                                        uint32_t destinationPortId,
                                        std::vector<uint16_t>& shortestPorts) const;
    bool TryLookupPrimary(uint32_t currentNodeId,
                          uint32_t destinationNodeId,
                          std::vector<uint16_t>& shortestPorts,
                          std::vector<uint16_t>& nonShortestPorts) const;
    const UbTopologyRuleNode* FindTopologyNode(const IndexedTopologyRule& rule,
                                               uint32_t nodeId) const;
    bool AreTopologyNeighbors(const IndexedTopologyRule& rule,
                              uint32_t leftNodeId,
                              uint32_t rightNodeId) const;
    bool ArePhysicallyAdjacent(const IndexedTopologyRule& rule,
                               uint32_t leftNodeId,
                               uint32_t rightNodeId) const;
    uint32_t TopologyDistance(const IndexedTopologyRule& rule,
                              uint32_t leftNodeId,
                              uint32_t rightNodeId) const;
    uint32_t RouterDistance(const IndexedTopologyRule& rule,
                            uint32_t leftRouter,
                            uint32_t rightRouter) const;
    uint32_t GlobalTopologyDistance(uint32_t leftNodeId, uint32_t rightNodeId) const;
    bool HasUpstreamSource(const IndexedTopologyRule& rule,
                           uint32_t currentNodeId,
                           uint32_t destinationNodeId,
                           uint32_t remainingDistance) const;
    void IndexTopologyRule(IndexedTopologyRule& rule, uint32_t ruleId);

    std::vector<std::unique_ptr<NodeRules>> m_nodeRules;
    std::map<std::vector<uint16_t>, PortSet> m_portSetPool;
    std::vector<std::vector<PhysicalNeighbor>> m_physicalAdjacency;
    std::unordered_map<uint64_t, PhysicalNeighbor> m_linkByEndpoint;
    std::vector<IndexedTopologyRule> m_topologyRules;
    std::vector<std::vector<uint32_t>> m_topologyRulesByNode;
    std::unique_ptr<UbTopologyNextHopIndex> m_topologyNextHopIndex;
    bool m_readyForLookup{false};
};

} // namespace ns3

#endif // UB_RULE_ROUTING_H
