// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-routing-table.h"

#include "ns3/assert.h"
#include "ns3/ipv4-address.h"
#include "ns3/ub-network-address.h"

#include <algorithm>
#include <limits>
#include <set>
#include <string>

namespace ns3 {

namespace
{
constexpr uint32_t kPrimaryRangeRoutePort = std::numeric_limits<uint32_t>::max();
}

size_t
UbRoutingTable::VectorHash::operator()(const std::vector<uint16_t>& ports) const
{
    std::string hashKey;
    for (uint16_t port : ports)
    {
        hashKey += std::to_string(port);
    }
    return Hash64(hashKey);
}

UbRoutingTable::PortSet
UbRoutingTable::GetOrCreatePortSet(const std::vector<uint16_t>& ports)
{
    const std::set<uint16_t> sorted(ports.begin(), ports.end());
    const std::vector<uint16_t> normalized(sorted.begin(), sorted.end());
    const auto existing = m_portSetPool.find(normalized);
    if (existing != m_portSetPool.end())
    {
        return existing->second;
    }

    auto sharedPorts = std::make_shared<std::vector<uint16_t>>(normalized);
    m_portSetPool.emplace(normalized, sharedPorts);
    return sharedPorts;
}

void
UbRoutingTable::AddShortestRoute(uint32_t destIp, const std::vector<uint16_t>& outPorts)
{
    std::vector<uint16_t> mergedOutPorts;
    const auto existing = m_shortest.find(destIp);
    if (existing != m_shortest.end())
    {
        mergedOutPorts.insert(mergedOutPorts.end(),
                              existing->second->begin(),
                              existing->second->end());
    }
    mergedOutPorts.insert(mergedOutPorts.end(), outPorts.begin(), outPorts.end());
    m_shortest[destIp] = GetOrCreatePortSet(mergedOutPorts);
}

void
UbRoutingTable::AddOtherRoute(uint32_t destIp, const std::vector<uint16_t>& outPorts)
{
    std::vector<uint16_t> mergedOutPorts;
    const auto existing = m_other.find(destIp);
    if (existing != m_other.end())
    {
        mergedOutPorts.insert(mergedOutPorts.end(),
                              existing->second->begin(),
                              existing->second->end());
    }
    mergedOutPorts.insert(mergedOutPorts.end(), outPorts.begin(), outPorts.end());
    m_other[destIp] = GetOrCreatePortSet(mergedOutPorts);
}

void
UbRoutingTable::AddRouteRange(RouteRangeByPortMap& routeRangesByPort,
                              uint32_t startNodeId,
                              uint32_t endNodeId,
                              uint32_t dstPortId,
                              const std::vector<uint16_t>& outPorts)
{
    AddRouteRangeToMap(routeRangesByPort[dstPortId], startNodeId, endNodeId, outPorts);
}

void
UbRoutingTable::AddRouteRangeToMap(RouteRangeMap& routeRanges,
                                   uint32_t startNodeId,
                                   uint32_t endNodeId,
                                   const std::vector<uint16_t>& outPorts)
{
    NS_ASSERT_MSG(startNodeId <= endNodeId, "route range start must not exceed end");

    auto valueAt = [&routeRanges](uint32_t nodeId) -> PortSet {
        auto it = routeRanges.upper_bound(nodeId);
        if (it == routeRanges.begin())
        {
            return nullptr;
        }
        return std::prev(it)->second;
    };

    if (routeRanges.find(startNodeId) == routeRanges.end())
    {
        routeRanges[startNodeId] = valueAt(startNodeId);
    }
    if (endNodeId != std::numeric_limits<uint32_t>::max())
    {
        const uint32_t afterEnd = endNodeId + 1;
        if (routeRanges.find(afterEnd) == routeRanges.end())
        {
            routeRanges[afterEnd] = valueAt(afterEnd);
        }
    }

    for (auto it = routeRanges.lower_bound(startNodeId);
         it != routeRanges.end() && it->first <= endNodeId;
         ++it)
    {
        std::vector<uint16_t> mergedOutPorts;
        if (it->second != nullptr)
        {
            mergedOutPorts.insert(mergedOutPorts.end(), it->second->begin(), it->second->end());
        }
        mergedOutPorts.insert(mergedOutPorts.end(), outPorts.begin(), outPorts.end());
        it->second = GetOrCreatePortSet(mergedOutPorts);
    }
}

void
UbRoutingTable::AddShortestRouteRange(uint32_t startNodeId,
                                      uint32_t endNodeId,
                                      const std::vector<uint16_t>& outPorts)
{
    AddShortestRouteRange(startNodeId, endNodeId, 0, outPorts);
}

void
UbRoutingTable::AddOtherRouteRange(uint32_t startNodeId,
                                   uint32_t endNodeId,
                                   const std::vector<uint16_t>& outPorts)
{
    AddOtherRouteRange(startNodeId, endNodeId, 0, outPorts);
}

void
UbRoutingTable::AddShortestRouteRange(uint32_t startNodeId,
                                      uint32_t endNodeId,
                                      uint32_t dstPortId,
                                      const std::vector<uint16_t>& outPorts)
{
    AddRouteRange(m_shortestRanges,
                  startNodeId,
                  endNodeId,
                  kPrimaryRangeRoutePort,
                  outPorts);
    AddRouteRange(m_shortestRanges, startNodeId, endNodeId, dstPortId, outPorts);
}

void
UbRoutingTable::AddOtherRouteRange(uint32_t startNodeId,
                                   uint32_t endNodeId,
                                   uint32_t dstPortId,
                                   const std::vector<uint16_t>& outPorts)
{
    AddRouteRange(m_otherRanges,
                  startNodeId,
                  endNodeId,
                  kPrimaryRangeRoutePort,
                  outPorts);
    AddRouteRange(m_otherRanges, startNodeId, endNodeId, dstPortId, outPorts);
}

UbRoutingTable::PortSet
UbRoutingTable::LookupRange(const RouteRangeByPortMap& routeRangesByPort, uint32_t destIp) const
{
    if (routeRangesByPort.empty())
    {
        return nullptr;
    }

    const uint32_t nodeId = utils::IpToNodeId(Ipv4Address(destIp));
    const uint32_t lastByte = destIp & 0x000000ff;
    const uint32_t portId = lastByte == 0 ? kPrimaryRangeRoutePort : lastByte - 1;
    const auto port = routeRangesByPort.find(portId);
    if (port == routeRangesByPort.end() || port->second.empty())
    {
        return nullptr;
    }

    auto range = port->second.upper_bound(nodeId);
    if (range == port->second.begin())
    {
        return nullptr;
    }
    return std::prev(range)->second;
}

UbRoutingTable::PortSet
UbRoutingTable::Lookup(const RouteMap& exact,
                       const RouteRangeByPortMap& ranges,
                       uint32_t destIp) const
{
    const auto route = exact.find(destIp);
    return route == exact.end() ? LookupRange(ranges, destIp) : route->second;
}

UbRoutingTable::PortSetView
UbRoutingTable::LookupShortest(uint32_t destIp) const
{
    const PortSet ports = Lookup(m_shortest, m_shortestRanges, destIp);
    return ports == nullptr ? PortSetView() : PortSetView(*ports);
}

UbRoutingTable::PortSetView
UbRoutingTable::LookupOther(uint32_t destIp) const
{
    const PortSet ports = Lookup(m_other, m_otherRanges, destIp);
    return ports == nullptr ? PortSetView() : PortSetView(*ports);
}

bool
UbRoutingTable::RemoveShortestRoute(uint32_t destIp)
{
    return m_shortest.erase(destIp) > 0;
}

bool
UbRoutingTable::RemoveOtherRoute(uint32_t destIp)
{
    return m_other.erase(destIp) > 0;
}

} // namespace ns3
