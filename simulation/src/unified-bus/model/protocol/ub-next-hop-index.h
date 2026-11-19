// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_NEXT_HOP_INDEX_H
#define UB_NEXT_HOP_INDEX_H

#include <cstdint>
#include <istream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace utils
{
struct UbStructuredTableRoute;
}

namespace ns3
{

class UbNextHopIndexBuilder;

/**
 * Shared exact next-hop index for structured table routes.
 *
 * Candidate sets are interned globally. Each route lookup stores only an adaptive-width class ID.
 */
class UbNextHopIndex
{
  public:
    static std::shared_ptr<const UbNextHopIndex> Build(
        const std::vector<utils::UbStructuredTableRoute>& routes,
        uint32_t nodeCount);
    static std::shared_ptr<const UbNextHopIndex> Read(
        std::istream& input,
        const std::vector<std::vector<bool>>& connectedPorts,
        const std::string& expectedFingerprint);

    bool TryLookup(uint32_t currentNodeId,
                   uint32_t destinationNodeId,
                   uint32_t destinationPortId,
                   std::vector<uint16_t>& shortestPorts,
                   std::vector<uint16_t>& nonShortestPorts) const;

    uint32_t GetRouteClassCount() const;
    uint32_t GetPortIdWidthBits() const;
    uint32_t GetNodeIdWidthBits() const;

  private:
    friend class UbNextHopIndexBuilder;

    static constexpr uint32_t INVALID_INDEX = UINT32_MAX;
    static constexpr uint32_t ANY_PORT = UINT32_MAX;
    static constexpr uint32_t PORTS_PER_ADDRESS_BLOCK = 256;

    struct RouteCandidates
    {
        std::vector<uint16_t> shortestPorts;
        std::vector<uint16_t> nonShortestPorts;
    };

    struct RouteCandidatesLess
    {
        bool operator()(const RouteCandidates& left, const RouteCandidates& right) const;
    };

    class PackedIds
    {
      public:
        PackedIds() = default;
        PackedIds(std::size_t size, uint32_t largestValue);

        void Set(std::size_t index, uint32_t value);
        uint32_t Get(std::size_t index) const;
        uint32_t GetWidthBits() const;
        static PackedIds Read(const std::vector<uint8_t>& payload,
                              std::size_t& offset,
                              std::size_t count,
                              uint32_t widthBytes);

      private:
        using Storage =
            std::variant<std::vector<uint8_t>, std::vector<uint16_t>, std::vector<uint32_t>>;

        template <typename Destination, typename Source>
        static std::vector<Destination> Widen(const std::vector<Source>& source)
        {
            return std::vector<Destination>(source.begin(), source.end());
        }

        void EnsureWidth(uint32_t value);

        Storage m_values{std::vector<uint8_t>{}};
    };

    UbNextHopIndex() = default;

    uint32_t Intern(RouteCandidates candidates);

    uint32_t m_rowCount{};
    uint32_t m_endpointCount{};
    uint32_t m_destinationNodeCount{};
    std::vector<uint32_t> m_rowByNodeId;
    std::vector<uint32_t> m_destinationIndexByNodeId;
    std::vector<uint32_t> m_endpointByAddress;
    std::vector<std::vector<uint32_t>> m_endpointsByDestination;
    std::vector<RouteCandidates> m_classes;
    std::map<RouteCandidates, uint32_t, RouteCandidatesLess> m_classIds;
    PackedIds m_portRoutes;
    PackedIds m_nodeRoutes;
};

/** Builds an immutable next-hop index without retaining per-route port vectors. */
class UbNextHopIndexBuilder
{
  public:
    explicit UbNextHopIndexBuilder(uint32_t nodeCount, std::size_t expectedRouteCount = 0);

    void AddRoute(const utils::UbStructuredTableRoute& route);
    void AddPrimaryRoute(uint32_t currentNodeId,
                         uint32_t destinationNodeId,
                         const std::vector<uint16_t>& shortestPorts,
                         const std::vector<uint16_t>& nonShortestPorts);
    std::shared_ptr<const UbNextHopIndex> Build();

  private:
    struct PendingRoute
    {
        uint32_t currentNodeId;
        uint32_t destinationNodeId;
        uint32_t destinationPortId;
        uint32_t classId;
    };

    struct PendingPrimaryRoute
    {
        uint32_t currentNodeId;
        uint32_t destinationNodeId;
        uint32_t classId;
    };

    uint32_t m_nodeCount;
    bool m_built{false};
    std::shared_ptr<UbNextHopIndex> m_index;
    std::set<uint32_t> m_currentNodeIds;
    std::set<uint32_t> m_destinationNodeIds;
    std::set<std::pair<uint32_t, uint32_t>> m_endpointKeys;
    std::vector<PendingRoute> m_routes;
    std::vector<PendingPrimaryRoute> m_primaryRoutes;
};

} // namespace ns3

#endif // UB_NEXT_HOP_INDEX_H
