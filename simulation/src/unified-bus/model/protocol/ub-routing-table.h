// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_ROUTING_TABLE_H
#define UB_ROUTING_TABLE_H

#include "ns3/hash.h"

#include <map>
#include <memory>
#include "ns3/ub-span.h"
#include <unordered_map>
#include <vector>

namespace ns3 {

class UbRoutingTable
{
  public:
    using PortSetView = UbSpan<const uint16_t>;

    void AddShortestRoute(uint32_t destIp, const std::vector<uint16_t>& outPorts);
    void AddOtherRoute(uint32_t destIp, const std::vector<uint16_t>& outPorts);
    void AddShortestRouteRange(uint32_t startNodeId,
                               uint32_t endNodeId,
                               const std::vector<uint16_t>& outPorts);
    void AddOtherRouteRange(uint32_t startNodeId,
                            uint32_t endNodeId,
                            const std::vector<uint16_t>& outPorts);
    void AddShortestRouteRange(uint32_t startNodeId,
                               uint32_t endNodeId,
                               uint32_t dstPortId,
                               const std::vector<uint16_t>& outPorts);
    void AddOtherRouteRange(uint32_t startNodeId,
                            uint32_t endNodeId,
                            uint32_t dstPortId,
                            const std::vector<uint16_t>& outPorts);

    PortSetView LookupShortest(uint32_t destIp) const;
    PortSetView LookupOther(uint32_t destIp) const;

    bool RemoveShortestRoute(uint32_t destIp);
    bool RemoveOtherRoute(uint32_t destIp);

  private:
    using PortSet = std::shared_ptr<std::vector<uint16_t>>;
    using RouteMap = std::unordered_map<uint32_t, PortSet>;
    using RouteRangeMap = std::map<uint32_t, PortSet>;
    using RouteRangeByPortMap = std::map<uint32_t, RouteRangeMap>;

    struct VectorHash
    {
        size_t operator()(const std::vector<uint16_t>& ports) const;
    };

    PortSet GetOrCreatePortSet(const std::vector<uint16_t>& ports);
    void AddRouteRange(RouteRangeByPortMap& routeRangesByPort,
                       uint32_t startNodeId,
                       uint32_t endNodeId,
                       uint32_t dstPortId,
                       const std::vector<uint16_t>& outPorts);
    void AddRouteRangeToMap(RouteRangeMap& routeRanges,
                            uint32_t startNodeId,
                            uint32_t endNodeId,
                            const std::vector<uint16_t>& outPorts);
    PortSet LookupRange(const RouteRangeByPortMap& routeRangesByPort, uint32_t destIp) const;
    PortSet Lookup(const RouteMap& exact,
                   const RouteRangeByPortMap& ranges,
                   uint32_t destIp) const;

    std::unordered_map<std::vector<uint16_t>, PortSet, VectorHash> m_portSetPool;
    RouteMap m_shortest;
    RouteMap m_other;
    RouteRangeByPortMap m_shortestRanges;
    RouteRangeByPortMap m_otherRanges;
};

} // namespace ns3

#endif // UB_ROUTING_TABLE_H
