// SPDX-License-Identifier: GPL-2.0-only
#include "ub-rule-routing.h"

#include "ns3/abort.h"

#include <algorithm>
#include <set>

namespace ns3
{

namespace
{

void
NormalizePorts(std::vector<uint16_t>& ports)
{
    std::sort(ports.begin(), ports.end());
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
}

} // namespace

class UbTopologyNextHopIndex
{
  public:
    explicit UbTopologyNextHopIndex(const UbRuleRoutingTable& table)
        : m_table(table)
    {
        Build();
    }

    bool TryLookupPort(uint32_t currentNodeId,
                       uint32_t destinationNodeId,
                       uint32_t destinationPortId,
                       std::vector<uint16_t>& shortestPorts) const;
    bool TryLookupNode(uint32_t currentNodeId,
                       uint32_t destinationNodeId,
                       std::vector<uint16_t>& shortestPorts) const;

  private:
    struct LocalNextHop
    {
        uint32_t peerNodeId;
        uint16_t localPort;
    };

    struct RuleNextHops
    {
        std::vector<std::vector<LocalNextHop>> byNodeIndex;
    };

    struct DestinationPort
    {
        uint32_t predecessorNodeId;
        uint16_t predecessorPort;
        std::vector<uint32_t> ruleIds;
    };

    void Build();
    const std::vector<LocalNextHop>* FindLocalNextHops(uint32_t ruleId, uint32_t nodeId) const;

    const UbRuleRoutingTable& m_table;
    std::vector<RuleNextHops> m_ruleNextHops;
    std::unordered_map<uint64_t, DestinationPort> m_destinationPorts;
};

UbRuleRoutingTable::UbRuleRoutingTable() = default;

UbRuleRoutingTable::~UbRuleRoutingTable() = default;

void
UbTopologyNextHopIndex::Build()
{
    m_ruleNextHops.resize(m_table.m_topologyRules.size());
    for (uint32_t ruleId = 0; ruleId < m_table.m_topologyRules.size(); ++ruleId)
    {
        const auto& rule = m_table.m_topologyRules[ruleId];
        auto& nextHops = m_ruleNextHops[ruleId].byNodeIndex;
        nextHops.resize(rule.nodes.size());
        for (std::size_t nodeIndex = 0; nodeIndex < rule.nodes.size(); ++nodeIndex)
        {
            const uint32_t nodeId = rule.nodes[nodeIndex].nodeId;
            if (nodeId >= m_table.m_physicalAdjacency.size())
            {
                continue;
            }
            for (const auto& neighbor : m_table.m_physicalAdjacency[nodeId])
            {
                if (m_table.FindTopologyNode(rule, neighbor.peerNodeId) != nullptr &&
                    m_table.AreTopologyNeighbors(rule, nodeId, neighbor.peerNodeId))
                {
                    nextHops[nodeIndex].push_back({neighbor.peerNodeId, neighbor.localPort});
                }
            }
        }
    }

    for (const auto& [endpointKey, link] : m_table.m_linkByEndpoint)
    {
        const uint32_t destinationNodeId = static_cast<uint32_t>(endpointKey >> 32);
        if (destinationNodeId >= m_table.m_topologyRulesByNode.size())
        {
            continue;
        }
        DestinationPort destinationPort{link.peerNodeId, link.peerPort, {}};
        for (uint32_t ruleId : m_table.m_topologyRulesByNode[destinationNodeId])
        {
            const auto& rule = m_table.m_topologyRules[ruleId];
            if (m_table.FindTopologyNode(rule, link.peerNodeId) != nullptr &&
                m_table.AreTopologyNeighbors(rule, destinationNodeId, link.peerNodeId))
            {
                destinationPort.ruleIds.push_back(ruleId);
            }
        }
        if (!destinationPort.ruleIds.empty())
        {
            m_destinationPorts.emplace(endpointKey, std::move(destinationPort));
        }
    }
}

const std::vector<UbTopologyNextHopIndex::LocalNextHop>*
UbTopologyNextHopIndex::FindLocalNextHops(uint32_t ruleId, uint32_t nodeId) const
{
    const auto& rule = m_table.m_topologyRules[ruleId];
    const auto position = rule.nodeIndex.find(nodeId);
    if (position == rule.nodeIndex.end())
    {
        return nullptr;
    }
    return &m_ruleNextHops[ruleId].byNodeIndex[position->second];
}

bool
UbTopologyNextHopIndex::TryLookupPort(uint32_t currentNodeId,
                                      uint32_t destinationNodeId,
                                      uint32_t destinationPortId,
                                      std::vector<uint16_t>& shortestPorts) const
{
    const auto destination = m_destinationPorts.find(
        UbRuleRoutingTable::MakeEndpointKey(destinationNodeId, destinationPortId));
    if (destination == m_destinationPorts.end())
    {
        return false;
    }

    const uint32_t globalDistance =
        m_table.GlobalTopologyDistance(currentNodeId, destinationNodeId);
    if (globalDistance == std::numeric_limits<uint32_t>::max())
    {
        return false;
    }

    const auto& branch = destination->second;
    for (uint32_t ruleId : branch.ruleIds)
    {
        const auto& rule = m_table.m_topologyRules[ruleId];
        if (m_table.FindTopologyNode(rule, currentNodeId) == nullptr)
        {
            continue;
        }
        const uint32_t distanceToPredecessor =
            m_table.TopologyDistance(rule, currentNodeId, branch.predecessorNodeId);
        const uint32_t remainingDistance = distanceToPredecessor + 1;
        if (remainingDistance != globalDistance ||
            !m_table.HasUpstreamSource(rule, currentNodeId, destinationNodeId, remainingDistance))
        {
            continue;
        }
        if (currentNodeId == branch.predecessorNodeId)
        {
            shortestPorts.push_back(branch.predecessorPort);
            continue;
        }
        const auto* nextHops = FindLocalNextHops(ruleId, currentNodeId);
        if (nextHops == nullptr)
        {
            continue;
        }
        for (const auto& nextHop : *nextHops)
        {
            if (m_table.TopologyDistance(rule, nextHop.peerNodeId, branch.predecessorNodeId) + 1 ==
                distanceToPredecessor)
            {
                shortestPorts.push_back(nextHop.localPort);
            }
        }
    }
    NormalizePorts(shortestPorts);
    return !shortestPorts.empty();
}

bool
UbTopologyNextHopIndex::TryLookupNode(uint32_t currentNodeId,
                                      uint32_t destinationNodeId,
                                      std::vector<uint16_t>& shortestPorts) const
{
    if (destinationNodeId >= m_table.m_topologyRulesByNode.size())
    {
        return false;
    }
    const uint32_t globalDistance =
        m_table.GlobalTopologyDistance(currentNodeId, destinationNodeId);
    if (globalDistance == std::numeric_limits<uint32_t>::max())
    {
        return false;
    }

    for (uint32_t ruleId : m_table.m_topologyRulesByNode[destinationNodeId])
    {
        const auto& rule = m_table.m_topologyRules[ruleId];
        const auto* nextHops = FindLocalNextHops(ruleId, currentNodeId);
        const auto destinationPosition = rule.nodeIndex.find(destinationNodeId);
        if (nextHops == nullptr || destinationPosition == rule.nodeIndex.end() ||
            m_ruleNextHops[ruleId].byNodeIndex[destinationPosition->second].empty())
        {
            continue;
        }
        const uint32_t ruleDistance =
            m_table.TopologyDistance(rule, currentNodeId, destinationNodeId);
        if (ruleDistance != globalDistance ||
            !m_table.HasUpstreamSource(rule, currentNodeId, destinationNodeId, ruleDistance))
        {
            continue;
        }
        for (const auto& nextHop : *nextHops)
        {
            if (m_table.TopologyDistance(rule, nextHop.peerNodeId, destinationNodeId) + 1 ==
                ruleDistance)
            {
                shortestPorts.push_back(nextHop.localPort);
            }
        }
    }
    NormalizePorts(shortestPorts);
    return !shortestPorts.empty();
}

void
UbRuleRoutingTable::AddPortGroup(const UbPortGroup& portGroup)
{
    NS_ABORT_MSG_IF(m_readyForLookup,
                    "cannot add a rule-routing port group after preparing the table");
    std::vector<uint16_t> ports = portGroup.ports;
    NormalizePorts(ports);
    NS_ABORT_MSG_IF(ports.empty(), "rule-routing port group must not be empty");

    NodeRules& nodeRules = GetOrCreateNodeRules(portGroup.nodeId);
    if (nodeRules.portGroups.size() <= portGroup.groupId)
    {
        nodeRules.portGroups.resize(portGroup.groupId + 1);
    }
    NS_ABORT_MSG_IF(nodeRules.portGroups[portGroup.groupId] != nullptr,
                    "duplicate rule-routing port group");
    nodeRules.portGroups[portGroup.groupId] = InternPorts(std::move(ports));
}

void
UbRuleRoutingTable::AddRule(const UbRoutingRule& rule)
{
    NS_ABORT_MSG_IF(m_readyForLookup, "cannot add a rule-routing rule after preparing the table");
    NS_ABORT_MSG_IF(rule.shortestPortGroups.empty() && rule.nonShortestPortGroups.empty(),
                    "rule-routing rule must reference at least one port group");
    NodeRules& nodeRules = GetOrCreateNodeRules(rule.currentNodeId);
    const uint64_t key = MakeDestinationKey(rule.destination.nodeId, rule.destination.portId);
    const auto [_, inserted] = nodeRules.exactRules.emplace(
        key,
        ResolveRule(rule.currentNodeId, rule.shortestPortGroups, rule.nonShortestPortGroups));
    NS_ABORT_MSG_IF(!inserted, "duplicate rule-routing rule");
}

void
UbRuleRoutingTable::AddRangeRule(const UbRoutingRangeRule& rule)
{
    NS_ABORT_MSG_IF(m_readyForLookup, "cannot add a rule-routing range after preparing the table");
    NS_ABORT_MSG_IF(rule.destinationStartNodeId > rule.destinationEndNodeId,
                    "rule-routing range start must not exceed its end");
    NS_ABORT_MSG_IF(rule.shortestPortGroups.empty() && rule.nonShortestPortGroups.empty(),
                    "rule-routing range must reference at least one port group");

    NodeRules& nodeRules = GetOrCreateNodeRules(rule.currentNodeId);
    auto& ranges = nodeRules.rangeRulesByPort[rule.destinationPortId];
    const auto position =
        std::lower_bound(ranges.begin(),
                         ranges.end(),
                         rule.destinationStartNodeId,
                         [](const ResolvedRangeRule& entry, uint32_t startNodeId) {
                             return entry.destinationStartNodeId < startNodeId;
                         });
    NS_ABORT_MSG_IF(position != ranges.end() &&
                        position->destinationStartNodeId <= rule.destinationEndNodeId,
                    "overlapping rule-routing destination ranges");
    NS_ABORT_MSG_IF(position != ranges.begin() &&
                        std::prev(position)->destinationEndNodeId >= rule.destinationStartNodeId,
                    "overlapping rule-routing destination ranges");
    ranges.insert(
        position,
        ResolvedRangeRule{
            rule.destinationStartNodeId,
            rule.destinationEndNodeId,
            ResolveRule(rule.currentNodeId, rule.shortestPortGroups, rule.nonShortestPortGroups),
        });
}

void
UbRuleRoutingTable::AddTopologyLink(const UbTopologyLink& link)
{
    NS_ABORT_MSG_IF(m_readyForLookup, "cannot add a topology link after preparing the rule table");
    NS_ABORT_MSG_IF(link.node1 == link.node2, "topology routing does not support self links");
    const uint64_t firstKey = MakeEndpointKey(link.node1, link.port1);
    const uint64_t secondKey = MakeEndpointKey(link.node2, link.port2);
    NS_ABORT_MSG_IF(m_linkByEndpoint.find(firstKey) != m_linkByEndpoint.end() || m_linkByEndpoint.find(secondKey) != m_linkByEndpoint.end(),
                    "a physical port must belong to exactly one topology link");

    const uint32_t largestNodeId = std::max(link.node1, link.node2);
    if (m_physicalAdjacency.size() <= largestNodeId)
    {
        m_physicalAdjacency.resize(largestNodeId + 1);
    }
    const PhysicalNeighbor first{link.node2, link.port1, link.port2};
    const PhysicalNeighbor second{link.node1, link.port2, link.port1};
    m_physicalAdjacency[link.node1].push_back(first);
    m_physicalAdjacency[link.node2].push_back(second);
    m_linkByEndpoint.emplace(firstKey, first);
    m_linkByEndpoint.emplace(secondKey, second);
}

void
UbRuleRoutingTable::AddTopologyRule(const UbTopologyRule& rule)
{
    NS_ABORT_MSG_IF(m_readyForLookup, "cannot add a topology rule after preparing the rule table");
    NS_ABORT_MSG_IF(rule.nodes.empty(), "topology rule must contain at least one node");
    if (rule.type == UbTopologyRuleType::DIMENSION)
    {
        NS_ABORT_MSG_IF(rule.shape.empty() || rule.shape.size() != rule.connections.size(),
                        "dimension topology rule shape and connections must have equal sizes");
        uint64_t nodeCount = 1;
        for (uint32_t size : rule.shape)
        {
            NS_ABORT_MSG_IF(size < 2, "dimension topology rule sizes must be at least two");
            nodeCount *= size;
        }
        NS_ABORT_MSG_IF(nodeCount != rule.nodes.size(),
                        "dimension topology rule shape does not match its node count");
    }
    else
    {
        NS_ABORT_MSG_IF(!rule.shape.empty() || !rule.connections.empty(),
                        "non-dimension topology rules must not contain dimension parameters");
    }

    IndexedTopologyRule indexed{
        rule.type,
        rule.shape,
        rule.connections,
        rule.nodes,
        {},
        {},
        {},
    };
    m_topologyRules.push_back(std::move(indexed));
}

void
UbRuleRoutingTable::PrepareForLookup()
{
    if (m_readyForLookup)
    {
        return;
    }
    for (auto& nodeRules : m_nodeRules)
    {
        if (nodeRules != nullptr)
        {
            nodeRules->portGroups.clear();
            nodeRules->portGroups.shrink_to_fit();
        }
    }
    for (uint32_t ruleId = 0; ruleId < m_topologyRules.size(); ++ruleId)
    {
        IndexTopologyRule(m_topologyRules[ruleId], ruleId);
    }
    m_topologyNextHopIndex = std::make_unique<UbTopologyNextHopIndex>(*this);
    m_portSetPool.clear();
    m_readyForLookup = true;
}

UbRuleRoutingTable::NodeRules&
UbRuleRoutingTable::GetOrCreateNodeRules(uint32_t currentNodeId)
{
    if (m_nodeRules.size() <= currentNodeId)
    {
        m_nodeRules.resize(currentNodeId + 1);
    }
    if (m_nodeRules[currentNodeId] == nullptr)
    {
        m_nodeRules[currentNodeId] = std::make_unique<NodeRules>();
    }
    return *m_nodeRules[currentNodeId];
}

UbRuleRoutingTable::PortSet
UbRuleRoutingTable::InternPorts(std::vector<uint16_t> ports)
{
    NormalizePorts(ports);
    auto existing = m_portSetPool.find(ports);
    if (existing != m_portSetPool.end())
    {
        return existing->second;
    }
    auto sharedPorts = std::make_shared<const std::vector<uint16_t>>(std::move(ports));
    m_portSetPool.emplace(*sharedPorts, sharedPorts);
    return sharedPorts;
}

UbRuleRoutingTable::PortSet
UbRuleRoutingTable::ResolvePortGroups(uint32_t currentNodeId, const std::vector<uint32_t>& groupIds)
{
    if (groupIds.empty())
    {
        return nullptr;
    }
    NodeRules& nodeRules = GetOrCreateNodeRules(currentNodeId);
    std::vector<uint16_t> ports;
    for (uint32_t groupId : groupIds)
    {
        NS_ABORT_MSG_IF(groupId >= nodeRules.portGroups.size() ||
                            nodeRules.portGroups[groupId] == nullptr,
                        "rule-routing rule references an unknown local port group");
        const auto& group = *nodeRules.portGroups[groupId];
        ports.insert(ports.end(), group.begin(), group.end());
    }
    return InternPorts(std::move(ports));
}

UbRuleRoutingTable::ResolvedRule
UbRuleRoutingTable::ResolveRule(uint32_t currentNodeId,
                                const std::vector<uint32_t>& shortestPortGroups,
                                const std::vector<uint32_t>& nonShortestPortGroups)
{
    return ResolvedRule{
        ResolvePortGroups(currentNodeId, shortestPortGroups),
        ResolvePortGroups(currentNodeId, nonShortestPortGroups),
    };
}

uint64_t
UbRuleRoutingTable::MakeDestinationKey(uint32_t destinationNodeId, uint32_t destinationPortId)
{
    return (static_cast<uint64_t>(destinationNodeId) << 32) | destinationPortId;
}

uint64_t
UbRuleRoutingTable::MakeEndpointKey(uint32_t nodeId, uint32_t portId)
{
    return (static_cast<uint64_t>(nodeId) << 32) | portId;
}

void
UbRuleRoutingTable::IndexTopologyRule(IndexedTopologyRule& rule, uint32_t ruleId)
{
    std::set<std::vector<uint32_t>> coordinates;
    for (std::size_t index = 0; index < rule.nodes.size(); ++index)
    {
        const auto& node = rule.nodes[index];
        const std::size_t expectedCoordinates =
            rule.type == UbTopologyRuleType::DIMENSION          ? rule.shape.size()
            : rule.type == UbTopologyRuleType::DIRECT_SWITCH    ? 2
            : rule.type == UbTopologyRuleType::TWO_LEVEL_SWITCH ? 3
                                                                : 4;
        NS_ABORT_MSG_IF(node.coordinates.size() != expectedCoordinates,
                        "topology rule node has the wrong coordinate count");
        NS_ABORT_MSG_IF(!coordinates.insert(node.coordinates).second,
                        "topology rule coordinates must be unique");
        if (rule.type == UbTopologyRuleType::DIMENSION)
        {
            for (std::size_t dimension = 0; dimension < rule.shape.size(); ++dimension)
            {
                NS_ABORT_MSG_IF(node.coordinates[dimension] >= rule.shape[dimension],
                                "dimension topology coordinate exceeds its shape");
            }
        }
        else
        {
            const uint32_t role = node.coordinates[0];
            NS_ABORT_MSG_IF((node.isHost && role != 0) || (!node.isHost && role == 0),
                            "topology rule node role differs from its node type");
            NS_ABORT_MSG_IF(rule.type == UbTopologyRuleType::DIRECT_SWITCH && role > 1,
                            "direct-switch topology rule has an invalid role");
            NS_ABORT_MSG_IF(rule.type == UbTopologyRuleType::TWO_LEVEL_SWITCH && role > 2,
                            "two-level topology rule has an invalid role");
            NS_ABORT_MSG_IF(rule.type == UbTopologyRuleType::DRAGONFLY && role > 1,
                            "Dragonfly topology rule has an invalid role");
        }
        const auto [_, inserted] = rule.nodeIndex.emplace(node.nodeId, index);
        NS_ABORT_MSG_IF(!inserted, "topology rule contains a duplicate node");
        if (node.isHost)
        {
            rule.hostNodeIds.push_back(node.nodeId);
        }
        if (m_topologyRulesByNode.size() <= node.nodeId)
        {
            m_topologyRulesByNode.resize(node.nodeId + 1);
        }
        m_topologyRulesByNode[node.nodeId].push_back(ruleId);
    }

    if (rule.type != UbTopologyRuleType::DRAGONFLY)
    {
        return;
    }
    for (const auto& node : rule.nodes)
    {
        if (!node.isHost)
        {
            rule.attachedRouterByNode.emplace(node.nodeId, node.nodeId);
            continue;
        }
        const auto router =
            std::find_if(rule.nodes.begin(),
                         rule.nodes.end(),
                         [&node](const UbTopologyRuleNode& candidate) {
                             return !candidate.isHost && candidate.coordinates[0] == 1 &&
                                    candidate.coordinates[1] == node.coordinates[1] &&
                                    candidate.coordinates[2] == node.coordinates[2];
                         });
        NS_ABORT_MSG_IF(router == rule.nodes.end(),
                        "Dragonfly host coordinates do not identify an attached router");
        rule.attachedRouterByNode.emplace(node.nodeId, router->nodeId);
    }
}

const UbTopologyRuleNode*
UbRuleRoutingTable::FindTopologyNode(const IndexedTopologyRule& rule, uint32_t nodeId) const
{
    const auto position = rule.nodeIndex.find(nodeId);
    return position == rule.nodeIndex.end() ? nullptr : &rule.nodes[position->second];
}

bool
UbRuleRoutingTable::AreTopologyNeighbors(const IndexedTopologyRule& rule,
                                         uint32_t leftNodeId,
                                         uint32_t rightNodeId) const
{
    const auto* left = FindTopologyNode(rule, leftNodeId);
    const auto* right = FindTopologyNode(rule, rightNodeId);
    if (left == nullptr || right == nullptr || leftNodeId == rightNodeId)
    {
        return false;
    }
    const auto& leftCoordinates = left->coordinates;
    const auto& rightCoordinates = right->coordinates;
    if (rule.type == UbTopologyRuleType::DIMENSION)
    {
        std::size_t changedDimension = rule.shape.size();
        for (std::size_t dimension = 0; dimension < rule.shape.size(); ++dimension)
        {
            if (leftCoordinates[dimension] == rightCoordinates[dimension])
            {
                continue;
            }
            if (changedDimension != rule.shape.size())
            {
                return false;
            }
            changedDimension = dimension;
        }
        if (changedDimension == rule.shape.size())
        {
            return false;
        }
        const uint32_t leftValue = leftCoordinates[changedDimension];
        const uint32_t rightValue = rightCoordinates[changedDimension];
        const uint32_t delta =
            leftValue > rightValue ? leftValue - rightValue : rightValue - leftValue;
        const auto connection = rule.connections[changedDimension];
        return connection == UbDimensionConnection::FULL_MESH || delta == 1 ||
               (connection == UbDimensionConnection::RING &&
                delta == rule.shape[changedDimension] - 1);
    }
    if (rule.type == UbTopologyRuleType::DIRECT_SWITCH)
    {
        return leftCoordinates[0] != rightCoordinates[0];
    }
    if (rule.type == UbTopologyRuleType::TWO_LEVEL_SWITCH)
    {
        const uint32_t leftRole = leftCoordinates[0];
        const uint32_t rightRole = rightCoordinates[0];
        if ((leftRole == 0 && rightRole == 1) || (leftRole == 1 && rightRole == 0))
        {
            const auto& host = leftRole == 0 ? leftCoordinates : rightCoordinates;
            const auto& leaf = leftRole == 1 ? leftCoordinates : rightCoordinates;
            return host[1] == leaf[1];
        }
        return (leftRole == 1 && rightRole == 2) || (leftRole == 2 && rightRole == 1);
    }

    const uint32_t leftRole = leftCoordinates[0];
    const uint32_t rightRole = rightCoordinates[0];
    if ((leftRole == 0 && rightRole == 1) || (leftRole == 1 && rightRole == 0))
    {
        const auto& host = leftRole == 0 ? leftCoordinates : rightCoordinates;
        const auto& router = leftRole == 1 ? leftCoordinates : rightCoordinates;
        return host[1] == router[1] && host[2] == router[2];
    }
    return leftRole == 1 && rightRole == 1;
}

bool
UbRuleRoutingTable::ArePhysicallyAdjacent(const IndexedTopologyRule& rule,
                                          uint32_t leftNodeId,
                                          uint32_t rightNodeId) const
{
    if (leftNodeId >= m_physicalAdjacency.size())
    {
        return false;
    }
    return std::any_of(m_physicalAdjacency[leftNodeId].begin(),
                       m_physicalAdjacency[leftNodeId].end(),
                       [&](const PhysicalNeighbor& neighbor) {
                           return neighbor.peerNodeId == rightNodeId &&
                                  AreTopologyNeighbors(rule, leftNodeId, rightNodeId);
                       });
}

uint32_t
UbRuleRoutingTable::RouterDistance(const IndexedTopologyRule& rule,
                                   uint32_t leftRouter,
                                   uint32_t rightRouter) const
{
    if (leftRouter == rightRouter)
    {
        return 0;
    }
    if (ArePhysicallyAdjacent(rule, leftRouter, rightRouter))
    {
        return 1;
    }
    if (leftRouter < m_physicalAdjacency.size())
    {
        for (const auto& neighbor : m_physicalAdjacency[leftRouter])
        {
            const auto* neighborNode = FindTopologyNode(rule, neighbor.peerNodeId);
            if (neighborNode != nullptr && !neighborNode->isHost &&
                AreTopologyNeighbors(rule, leftRouter, neighbor.peerNodeId) &&
                ArePhysicallyAdjacent(rule, neighbor.peerNodeId, rightRouter))
            {
                return 2;
            }
        }
    }
    // A verified classic Dragonfly has full-mesh groups and one direct link per group pair.
    return 3;
}

uint32_t
UbRuleRoutingTable::TopologyDistance(const IndexedTopologyRule& rule,
                                     uint32_t leftNodeId,
                                     uint32_t rightNodeId) const
{
    if (leftNodeId == rightNodeId)
    {
        return 0;
    }
    const auto* left = FindTopologyNode(rule, leftNodeId);
    const auto* right = FindTopologyNode(rule, rightNodeId);
    NS_ABORT_MSG_IF(left == nullptr || right == nullptr,
                    "topology distance requires both nodes in one rule");
    const auto& leftCoordinates = left->coordinates;
    const auto& rightCoordinates = right->coordinates;
    if (rule.type == UbTopologyRuleType::DIMENSION)
    {
        uint32_t distance = 0;
        for (std::size_t dimension = 0; dimension < rule.shape.size(); ++dimension)
        {
            const uint32_t leftValue = leftCoordinates[dimension];
            const uint32_t rightValue = rightCoordinates[dimension];
            const uint32_t delta =
                leftValue > rightValue ? leftValue - rightValue : rightValue - leftValue;
            if (rule.connections[dimension] == UbDimensionConnection::LINE)
            {
                distance += delta;
            }
            else if (rule.connections[dimension] == UbDimensionConnection::RING)
            {
                distance += std::min(delta, rule.shape[dimension] - delta);
            }
            else
            {
                distance += delta != 0;
            }
        }
        return distance;
    }
    if (rule.type == UbTopologyRuleType::DIRECT_SWITCH)
    {
        return leftCoordinates[0] == rightCoordinates[0] ? 2 : 1;
    }
    if (rule.type == UbTopologyRuleType::TWO_LEVEL_SWITCH)
    {
        const uint32_t leftRole = leftCoordinates[0];
        const uint32_t rightRole = rightCoordinates[0];
        auto attachedLeaf = [&](uint32_t hostNodeId) {
            NS_ABORT_MSG_IF(hostNodeId >= m_physicalAdjacency.size(),
                            "two-level topology host has no physical adjacency");
            const auto leaf = std::find_if(m_physicalAdjacency[hostNodeId].begin(),
                                           m_physicalAdjacency[hostNodeId].end(),
                                           [&](const PhysicalNeighbor& neighbor) {
                                               const auto* node =
                                                   FindTopologyNode(rule, neighbor.peerNodeId);
                                               return node != nullptr && node->coordinates[0] == 1;
                                           });
            NS_ABORT_MSG_IF(leaf == m_physicalAdjacency[hostNodeId].end(),
                            "two-level topology host has no attached leaf");
            return leaf->peerNodeId;
        };
        if (leftRole == 0 && rightRole == 0)
        {
            return attachedLeaf(leftNodeId) == attachedLeaf(rightNodeId) ? 2 : 4;
        }
        if (leftRole == 2 && rightRole == 2)
        {
            if (leftNodeId < m_physicalAdjacency.size())
            {
                for (const auto& neighbor : m_physicalAdjacency[leftNodeId])
                {
                    const auto* node = FindTopologyNode(rule, neighbor.peerNodeId);
                    if (node != nullptr && node->coordinates[0] == 1 &&
                        ArePhysicallyAdjacent(rule, neighbor.peerNodeId, rightNodeId))
                    {
                        return 2;
                    }
                }
            }
            return 4;
        }
        if ((leftRole == 0 && rightRole == 2) || (leftRole == 2 && rightRole == 0))
        {
            const uint32_t hostNodeId = leftRole == 0 ? leftNodeId : rightNodeId;
            const uint32_t upperNodeId = leftRole == 2 ? leftNodeId : rightNodeId;
            return ArePhysicallyAdjacent(rule, attachedLeaf(hostNodeId), upperNodeId) ? 2 : 4;
        }
        if ((leftRole == 1 && rightRole == 2) || (leftRole == 2 && rightRole == 1))
        {
            return ArePhysicallyAdjacent(rule, leftNodeId, rightNodeId) ? 1 : 3;
        }
        if (leftRole == 1 && rightRole == 1)
        {
            return 2;
        }
        const uint32_t hostNodeId = leftRole == 0 ? leftNodeId : rightNodeId;
        const uint32_t leafNodeId = leftRole == 1 ? leftNodeId : rightNodeId;
        return attachedLeaf(hostNodeId) == leafNodeId ? 1 : 3;
    }

    const uint32_t leftRouter = rule.attachedRouterByNode.at(leftNodeId);
    const uint32_t rightRouter = rule.attachedRouterByNode.at(rightNodeId);
    return static_cast<uint32_t>(left->isHost) + RouterDistance(rule, leftRouter, rightRouter) +
           static_cast<uint32_t>(right->isHost);
}

uint32_t
UbRuleRoutingTable::GlobalTopologyDistance(uint32_t leftNodeId, uint32_t rightNodeId) const
{
    if (leftNodeId >= m_topologyRulesByNode.size())
    {
        return std::numeric_limits<uint32_t>::max();
    }
    uint32_t distance = std::numeric_limits<uint32_t>::max();
    for (uint32_t ruleId : m_topologyRulesByNode[leftNodeId])
    {
        const auto& rule = m_topologyRules[ruleId];
        if (FindTopologyNode(rule, rightNodeId) != nullptr)
        {
            distance = std::min(distance, TopologyDistance(rule, leftNodeId, rightNodeId));
        }
    }
    return distance;
}

bool
UbRuleRoutingTable::HasUpstreamSource(const IndexedTopologyRule& rule,
                                      uint32_t currentNodeId,
                                      uint32_t destinationNodeId,
                                      uint32_t remainingDistance) const
{
    const auto* current = FindTopologyNode(rule, currentNodeId);
    if (current->isHost)
    {
        return currentNodeId != destinationNodeId;
    }
    if (rule.type == UbTopologyRuleType::TWO_LEVEL_SWITCH)
    {
        const auto destinationLeaf =
            std::find_if(m_physicalAdjacency[destinationNodeId].begin(),
                         m_physicalAdjacency[destinationNodeId].end(),
                         [&](const PhysicalNeighbor& neighbor) {
                             const auto* node = FindTopologyNode(rule, neighbor.peerNodeId);
                             return node != nullptr && node->coordinates[0] == 1;
                         });
        NS_ABORT_MSG_IF(destinationLeaf == m_physicalAdjacency[destinationNodeId].end(),
                        "two-level topology destination has no attached leaf");
        const uint32_t destinationLeafId = destinationLeaf->peerNodeId;
        if (current->coordinates[0] == 1)
        {
            if (currentNodeId != destinationLeafId)
            {
                return true;
            }
            return std::any_of(rule.nodes.begin(),
                               rule.nodes.end(),
                               [&](const UbTopologyRuleNode& node) {
                                   return node.coordinates[0] == 1 &&
                                          node.nodeId != destinationLeafId;
                               });
        }
        return ArePhysicallyAdjacent(rule, currentNodeId, destinationLeafId) &&
               std::any_of(m_physicalAdjacency[currentNodeId].begin(),
                           m_physicalAdjacency[currentNodeId].end(),
                           [&](const PhysicalNeighbor& neighbor) {
                               const auto* node = FindTopologyNode(rule, neighbor.peerNodeId);
                               return node != nullptr && node->coordinates[0] == 1 &&
                                      node->nodeId != destinationLeafId;
                           });
    }
    if (rule.type == UbTopologyRuleType::DRAGONFLY)
    {
        const uint32_t destinationRouter = rule.attachedRouterByNode.at(destinationNodeId);
        if (currentNodeId != destinationRouter)
        {
            return std::any_of(m_physicalAdjacency[currentNodeId].begin(),
                               m_physicalAdjacency[currentNodeId].end(),
                               [&](const PhysicalNeighbor& neighbor) {
                                   const auto* node = FindTopologyNode(rule, neighbor.peerNodeId);
                                   return node != nullptr && node->isHost &&
                                          node->nodeId != destinationNodeId;
                               });
        }
        return std::any_of(rule.hostNodeIds.begin(),
                           rule.hostNodeIds.end(),
                           [&](uint32_t sourceNodeId) {
                               return sourceNodeId != destinationNodeId &&
                                      rule.attachedRouterByNode.at(sourceNodeId) !=
                                          destinationRouter;
                           });
    }

    // A direct switch can overlap a host-only structure. Retain the global-distance
    // proof so the switch is not selected when every source has a shorter direct path.
    for (uint32_t sourceNodeId : rule.hostNodeIds)
    {
        if (sourceNodeId == destinationNodeId)
        {
            continue;
        }
        const uint32_t sourceToCurrent = TopologyDistance(rule, sourceNodeId, currentNodeId);
        const uint32_t sourceToDestination =
            TopologyDistance(rule, sourceNodeId, destinationNodeId);
        if (sourceToCurrent + remainingDistance == sourceToDestination &&
            GlobalTopologyDistance(sourceNodeId, destinationNodeId) == sourceToDestination)
        {
            return true;
        }
    }
    return false;
}

const UbRuleRoutingTable::ResolvedRule*
UbRuleRoutingTable::FindExactRule(uint32_t currentNodeId,
                                  uint32_t destinationNodeId,
                                  uint32_t destinationPortId) const
{
    if (currentNodeId >= m_nodeRules.size() || m_nodeRules[currentNodeId] == nullptr)
    {
        return nullptr;
    }
    const NodeRules& nodeRules = *m_nodeRules[currentNodeId];
    auto exact =
        nodeRules.exactRules.find(MakeDestinationKey(destinationNodeId, destinationPortId));
    if (exact != nodeRules.exactRules.end())
    {
        return &exact->second;
    }
    return FindRangeRule(nodeRules, destinationNodeId, destinationPortId);
}

const UbRuleRoutingTable::ResolvedRule*
UbRuleRoutingTable::FindRule(uint32_t currentNodeId,
                             uint32_t destinationNodeId,
                             uint32_t destinationPortId) const
{
    const ResolvedRule* exact = FindExactRule(currentNodeId, destinationNodeId, destinationPortId);
    if (exact != nullptr)
    {
        return exact;
    }

    if (currentNodeId >= m_nodeRules.size() || m_nodeRules[currentNodeId] == nullptr)
    {
        return nullptr;
    }
    const NodeRules& nodeRules = *m_nodeRules[currentNodeId];
    if (destinationPortId != UbRuleDestination::ANY_PORT)
    {
        const auto wildcard = nodeRules.exactRules.find(
            MakeDestinationKey(destinationNodeId, UbRuleDestination::ANY_PORT));
        if (wildcard != nodeRules.exactRules.end())
        {
            return &wildcard->second;
        }
    }

    const ResolvedRule* range = FindRangeRule(nodeRules, destinationNodeId, destinationPortId);
    if (range == nullptr && destinationPortId != UbRuleDestination::ANY_PORT)
    {
        range = FindRangeRule(nodeRules, destinationNodeId, UbRuleDestination::ANY_PORT);
    }
    return range;
}

const UbRuleRoutingTable::ResolvedRule*
UbRuleRoutingTable::FindRangeRule(const NodeRules& nodeRules,
                                  uint32_t destinationNodeId,
                                  uint32_t destinationPortId)
{
    const auto rangesByPort = nodeRules.rangeRulesByPort.find(destinationPortId);
    if (rangesByPort == nodeRules.rangeRulesByPort.end())
    {
        return nullptr;
    }
    const auto& ranges = rangesByPort->second;
    auto position = std::upper_bound(ranges.begin(),
                                     ranges.end(),
                                     destinationNodeId,
                                     [](uint32_t nodeId, const ResolvedRangeRule& entry) {
                                         return nodeId < entry.destinationStartNodeId;
                                     });
    if (position == ranges.begin())
    {
        return nullptr;
    }
    --position;
    return destinationNodeId <= position->destinationEndNodeId ? &position->candidates : nullptr;
}

bool
UbRuleRoutingTable::TryLookupTopologyPort(uint32_t currentNodeId,
                                          uint32_t destinationNodeId,
                                          uint32_t destinationPortId,
                                          std::vector<uint16_t>& shortestPorts) const
{
    if (m_topologyNextHopIndex != nullptr &&
        m_topologyNextHopIndex->TryLookupPort(currentNodeId,
                                              destinationNodeId,
                                              destinationPortId,
                                              shortestPorts))
    {
        return true;
    }
    shortestPorts.clear();
    return TryLookupTopologyPortReference(currentNodeId,
                                          destinationNodeId,
                                          destinationPortId,
                                          shortestPorts);
}

bool
UbRuleRoutingTable::TryLookupTopologyPortReference(uint32_t currentNodeId,
                                                   uint32_t destinationNodeId,
                                                   uint32_t destinationPortId,
                                                   std::vector<uint16_t>& shortestPorts) const
{
    const auto finalLink =
        m_linkByEndpoint.find(MakeEndpointKey(destinationNodeId, destinationPortId));
    if (finalLink == m_linkByEndpoint.end())
    {
        return false;
    }
    const uint32_t predecessorNodeId = finalLink->second.peerNodeId;
    const uint16_t predecessorPort = finalLink->second.peerPort;
    const uint32_t globalDistance = GlobalTopologyDistance(currentNodeId, destinationNodeId);
    if (globalDistance == std::numeric_limits<uint32_t>::max() ||
        destinationNodeId >= m_topologyRulesByNode.size())
    {
        return false;
    }

    bool handled = false;
    for (uint32_t ruleId : m_topologyRulesByNode[destinationNodeId])
    {
        const auto& rule = m_topologyRules[ruleId];
        if (FindTopologyNode(rule, predecessorNodeId) == nullptr ||
            FindTopologyNode(rule, currentNodeId) == nullptr ||
            !AreTopologyNeighbors(rule, destinationNodeId, predecessorNodeId))
        {
            continue;
        }
        const uint32_t distanceToPredecessor =
            TopologyDistance(rule, currentNodeId, predecessorNodeId);
        const uint32_t remainingDistance = distanceToPredecessor + 1;
        if (remainingDistance != globalDistance ||
            !HasUpstreamSource(rule, currentNodeId, destinationNodeId, remainingDistance))
        {
            continue;
        }

        handled = true;
        if (currentNodeId == predecessorNodeId)
        {
            shortestPorts.push_back(predecessorPort);
            continue;
        }
        if (currentNodeId >= m_physicalAdjacency.size())
        {
            continue;
        }
        for (const auto& neighbor : m_physicalAdjacency[currentNodeId])
        {
            if (FindTopologyNode(rule, neighbor.peerNodeId) != nullptr &&
                AreTopologyNeighbors(rule, currentNodeId, neighbor.peerNodeId) &&
                TopologyDistance(rule, neighbor.peerNodeId, predecessorNodeId) + 1 ==
                    distanceToPredecessor)
            {
                shortestPorts.push_back(neighbor.localPort);
            }
        }
    }
    NormalizePorts(shortestPorts);
    return handled && !shortestPorts.empty();
}

bool
UbRuleRoutingTable::TryLookupTopology(uint32_t currentNodeId,
                                      uint32_t destinationNodeId,
                                      uint32_t destinationPortId,
                                      std::vector<uint16_t>& shortestPorts) const
{
    shortestPorts.clear();
    if (m_topologyRules.empty() || currentNodeId == destinationNodeId)
    {
        return false;
    }
    if (destinationPortId != UbRuleDestination::ANY_PORT)
    {
        return TryLookupTopologyPort(currentNodeId,
                                     destinationNodeId,
                                     destinationPortId,
                                     shortestPorts);
    }
    if (m_topologyNextHopIndex != nullptr &&
        m_topologyNextHopIndex->TryLookupNode(currentNodeId, destinationNodeId, shortestPorts))
    {
        return true;
    }
    shortestPorts.clear();
    return TryLookupTopologyReference(currentNodeId,
                                      destinationNodeId,
                                      destinationPortId,
                                      shortestPorts);
}

bool
UbRuleRoutingTable::TryLookupTopologyReference(uint32_t currentNodeId,
                                               uint32_t destinationNodeId,
                                               uint32_t destinationPortId,
                                               std::vector<uint16_t>& shortestPorts) const
{
    shortestPorts.clear();
    if (m_topologyRules.empty() || currentNodeId == destinationNodeId)
    {
        return false;
    }
    if (destinationPortId != UbRuleDestination::ANY_PORT)
    {
        return TryLookupTopologyPortReference(currentNodeId,
                                              destinationNodeId,
                                              destinationPortId,
                                              shortestPorts);
    }
    if (destinationNodeId >= m_physicalAdjacency.size())
    {
        return false;
    }

    bool handled = false;
    std::vector<uint16_t> ports;
    for (const auto& link : m_physicalAdjacency[destinationNodeId])
    {
        ports.clear();
        if (TryLookupTopologyPortReference(currentNodeId, destinationNodeId, link.localPort, ports))
        {
            shortestPorts.insert(shortestPorts.end(), ports.begin(), ports.end());
            handled = true;
        }
    }
    NormalizePorts(shortestPorts);
    return handled && !shortestPorts.empty();
}

bool
UbRuleRoutingTable::TryLookup(uint32_t currentNodeId,
                              uint32_t destinationNodeId,
                              uint32_t destinationPortId,
                              std::vector<uint16_t>& shortestPorts,
                              std::vector<uint16_t>& nonShortestPorts) const
{
    shortestPorts.clear();
    nonShortestPorts.clear();
    const ResolvedRule* rule = FindRule(currentNodeId, destinationNodeId, destinationPortId);
    if (rule == nullptr)
    {
        return TryLookupTopology(currentNodeId,
                                 destinationNodeId,
                                 destinationPortId,
                                 shortestPorts);
    }

    if (rule->shortestPorts != nullptr)
    {
        shortestPorts = *rule->shortestPorts;
    }
    if (rule->nonShortestPorts != nullptr)
    {
        nonShortestPorts = *rule->nonShortestPorts;
    }
    return true;
}

bool
UbRuleRoutingTable::TryLookupExact(uint32_t currentNodeId,
                                   uint32_t destinationNodeId,
                                   uint32_t destinationPortId,
                                   std::vector<uint16_t>& shortestPorts,
                                   std::vector<uint16_t>& nonShortestPorts) const
{
    shortestPorts.clear();
    nonShortestPorts.clear();
    const ResolvedRule* rule = FindExactRule(currentNodeId, destinationNodeId, destinationPortId);
    if (rule == nullptr)
    {
        return TryLookupTopology(currentNodeId,
                                 destinationNodeId,
                                 destinationPortId,
                                 shortestPorts);
    }

    if (rule->shortestPorts != nullptr)
    {
        shortestPorts = *rule->shortestPorts;
    }
    if (rule->nonShortestPorts != nullptr)
    {
        nonShortestPorts = *rule->nonShortestPorts;
    }
    return true;
}

bool
UbRuleRoutingTable::TryLookupWithPrimaryFallback(uint32_t currentNodeId,
                                                 uint32_t destinationNodeId,
                                                 uint32_t destinationPortId,
                                                 std::vector<uint16_t>& shortestPorts,
                                                 std::vector<uint16_t>& nonShortestPorts) const
{
    shortestPorts.clear();
    nonShortestPorts.clear();
    const ResolvedRule* rule = FindRule(currentNodeId, destinationNodeId, destinationPortId);
    if (destinationPortId == UbRuleDestination::ANY_PORT)
    {
        if (rule != nullptr)
        {
            if (rule->shortestPorts != nullptr)
            {
                shortestPorts = *rule->shortestPorts;
            }
            if (rule->nonShortestPorts != nullptr)
            {
                nonShortestPorts = *rule->nonShortestPorts;
            }
            return true;
        }
        if (currentNodeId < m_nodeRules.size() && m_nodeRules[currentNodeId] != nullptr)
        {
            return TryLookupPrimary(currentNodeId,
                                    destinationNodeId,
                                    shortestPorts,
                                    nonShortestPorts);
        }
        return TryLookupTopology(currentNodeId,
                                 destinationNodeId,
                                 destinationPortId,
                                 shortestPorts);
    }
    if (rule == nullptr)
    {
        return TryLookupTopology(currentNodeId,
                                 destinationNodeId,
                                 destinationPortId,
                                 shortestPorts);
    }
    if (rule->shortestPorts != nullptr)
    {
        shortestPorts = *rule->shortestPorts;
    }
    if (rule->nonShortestPorts != nullptr)
    {
        nonShortestPorts = *rule->nonShortestPorts;
    }
    if (!shortestPorts.empty() && !nonShortestPorts.empty())
    {
        return true;
    }

    std::vector<uint16_t> primaryShortestPorts;
    std::vector<uint16_t> primaryNonShortestPorts;
    if (!TryLookupPrimary(currentNodeId,
                          destinationNodeId,
                          primaryShortestPorts,
                          primaryNonShortestPorts))
    {
        return true;
    }
    if (shortestPorts.empty())
    {
        shortestPorts = std::move(primaryShortestPorts);
    }
    if (nonShortestPorts.empty())
    {
        nonShortestPorts = std::move(primaryNonShortestPorts);
    }
    return true;
}

bool
UbRuleRoutingTable::TryLookupPrimary(uint32_t currentNodeId,
                                     uint32_t destinationNodeId,
                                     std::vector<uint16_t>& shortestPorts,
                                     std::vector<uint16_t>& nonShortestPorts) const
{
    shortestPorts.clear();
    nonShortestPorts.clear();
    if (destinationNodeId >= m_physicalAdjacency.size())
    {
        return false;
    }

    bool handled = false;
    std::vector<uint16_t> portShortest;
    std::vector<uint16_t> portNonShortest;
    for (const auto& link : m_physicalAdjacency[destinationNodeId])
    {
        if (!TryLookup(currentNodeId,
                       destinationNodeId,
                       link.localPort,
                       portShortest,
                       portNonShortest))
        {
            continue;
        }
        shortestPorts.insert(shortestPorts.end(), portShortest.begin(), portShortest.end());
        nonShortestPorts.insert(nonShortestPorts.end(),
                                portNonShortest.begin(),
                                portNonShortest.end());
        handled = true;
    }
    NormalizePorts(shortestPorts);
    NormalizePorts(nonShortestPorts);
    return handled && (!shortestPorts.empty() || !nonShortestPorts.empty());
}

} // namespace ns3
