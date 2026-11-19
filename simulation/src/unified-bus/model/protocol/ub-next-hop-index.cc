// SPDX-License-Identifier: GPL-2.0-only
#include "ub-next-hop-index.h"

#include "ns3/abort.h"
#include "ns3/ub-utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <tuple>

namespace ns3
{

namespace
{

constexpr std::array<uint8_t, 8> COMPRESSED_ROUTE_MAGIC = {'U', 'B', 'C', 'R', 'I', 'D', 'X', 0};
constexpr uint16_t COMPRESSED_ROUTE_MAJOR = 1;
constexpr uint16_t COMPRESSED_ROUTE_MINOR = 0;
constexpr uint32_t COMPRESSED_ROUTE_ENDIAN_MARKER = 0x01020304;
constexpr uint32_t COMPRESSED_ROUTE_HEADER_SIZE = 88;

uint16_t
ReadLittleUint16(const std::vector<uint8_t>& bytes, std::size_t& offset)
{
    NS_ABORT_MSG_IF(offset + 2 > bytes.size(), "truncated compressed routing artifact");
    const uint16_t value =
        static_cast<uint16_t>(bytes[offset]) | static_cast<uint16_t>(bytes[offset + 1]) << 8;
    offset += 2;
    return value;
}

uint32_t
ReadLittleUint32(const std::vector<uint8_t>& bytes, std::size_t& offset)
{
    NS_ABORT_MSG_IF(offset + 4 > bytes.size(), "truncated compressed routing artifact");
    uint32_t value = 0;
    for (uint32_t index = 0; index < 4; ++index)
    {
        value |= static_cast<uint32_t>(bytes[offset + index]) << (index * 8);
    }
    offset += 4;
    return value;
}

uint64_t
ReadLittleUint64(const std::vector<uint8_t>& bytes, std::size_t& offset)
{
    NS_ABORT_MSG_IF(offset + 8 > bytes.size(), "truncated compressed routing artifact");
    uint64_t value = 0;
    for (uint32_t index = 0; index < 8; ++index)
    {
        value |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8);
    }
    offset += 8;
    return value;
}

uint32_t
CalculateCrc32(const std::vector<uint8_t>& bytes)
{
    uint32_t crc = 0xffffffffU;
    for (uint8_t byte : bytes)
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

std::array<uint8_t, 32>
ParseFingerprint(const std::string& value)
{
    NS_ABORT_MSG_IF(value.size() != 64,
                    "compressed routing fingerprint must contain 64 hexadecimal characters");
    std::array<uint8_t, 32> fingerprint{};
    auto nibble = [](char character) -> uint8_t {
        if (character >= '0' && character <= '9')
        {
            return static_cast<uint8_t>(character - '0');
        }
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        NS_ABORT_MSG_IF(character < 'a' || character > 'f',
                        "compressed routing fingerprint contains a non-hexadecimal character");
        return static_cast<uint8_t>(character - 'a' + 10);
    };
    for (std::size_t index = 0; index < fingerprint.size(); ++index)
    {
        fingerprint[index] =
            static_cast<uint8_t>((nibble(value[index * 2]) << 4) | nibble(value[index * 2 + 1]));
    }
    return fingerprint;
}

std::size_t
CheckedProduct(std::size_t left, std::size_t right, const char* description)
{
    NS_ABORT_MSG_IF(right != 0 && left > std::numeric_limits<std::size_t>::max() / right,
                    "compressed routing " << description << " size overflows");
    return left * right;
}

std::size_t
CheckedAdd(std::size_t left, std::size_t right, const char* description)
{
    NS_ABORT_MSG_IF(left > std::numeric_limits<std::size_t>::max() - right,
                    "compressed routing " << description << " size overflows");
    return left + right;
}

void
NormalizePorts(std::vector<uint16_t>& ports)
{
    std::sort(ports.begin(), ports.end());
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
}

} // namespace

bool
UbNextHopIndex::RouteCandidatesLess::operator()(const RouteCandidates& left,
                                                const RouteCandidates& right) const
{
    return std::tie(left.shortestPorts, left.nonShortestPorts) <
           std::tie(right.shortestPorts, right.nonShortestPorts);
}

UbNextHopIndex::PackedIds::PackedIds(std::size_t size, uint32_t largestValue)
{
    if (largestValue <= std::numeric_limits<uint8_t>::max())
    {
        m_values = std::vector<uint8_t>(size, 0);
    }
    else if (largestValue <= std::numeric_limits<uint16_t>::max())
    {
        m_values = std::vector<uint16_t>(size, 0);
    }
    else
    {
        m_values = std::vector<uint32_t>(size, 0);
    }
}

void
UbNextHopIndex::PackedIds::Set(std::size_t index, uint32_t value)
{
    EnsureWidth(value);
    std::visit(
        [&](auto& values) {
            using Value = typename std::decay_t<decltype(values)>::value_type;
            values[index] = static_cast<Value>(value);
        },
        m_values);
}

uint32_t
UbNextHopIndex::PackedIds::Get(std::size_t index) const
{
    return std::visit([&](const auto& values) { return static_cast<uint32_t>(values[index]); },
                      m_values);
}

uint32_t
UbNextHopIndex::PackedIds::GetWidthBits() const
{
    return std::visit(
        [](const auto& values) {
            using Value = typename std::decay_t<decltype(values)>::value_type;
            return static_cast<uint32_t>(sizeof(Value) * 8);
        },
        m_values);
}

UbNextHopIndex::PackedIds
UbNextHopIndex::PackedIds::Read(const std::vector<uint8_t>& payload,
                                std::size_t& offset,
                                std::size_t count,
                                uint32_t widthBytes)
{
    NS_ABORT_MSG_IF(widthBytes != 1 && widthBytes != 2 && widthBytes != 4,
                    "compressed routing class ID width must be 1, 2, or 4 bytes");
    const std::size_t byteCount = CheckedProduct(count, widthBytes, "class ID array");
    NS_ABORT_MSG_IF(offset + byteCount < offset || offset + byteCount > payload.size(),
                    "truncated compressed routing class ID array");

    PackedIds ids;
    if (widthBytes == 1)
    {
        ids.m_values =
            std::vector<uint8_t>(payload.begin() + offset, payload.begin() + offset + byteCount);
        offset += byteCount;
        return ids;
    }
    if (widthBytes == 2)
    {
        std::vector<uint16_t> values(count);
        for (auto& value : values)
        {
            value = ReadLittleUint16(payload, offset);
        }
        ids.m_values = std::move(values);
        return ids;
    }
    std::vector<uint32_t> values(count);
    for (auto& value : values)
    {
        value = ReadLittleUint32(payload, offset);
    }
    ids.m_values = std::move(values);
    return ids;
}

void
UbNextHopIndex::PackedIds::EnsureWidth(uint32_t value)
{
    if (value <= std::numeric_limits<uint8_t>::max())
    {
        return;
    }
    if (std::holds_alternative<std::vector<uint8_t>>(m_values))
    {
        m_values = Widen<uint16_t>(std::get<std::vector<uint8_t>>(m_values));
    }
    if (value <= std::numeric_limits<uint16_t>::max())
    {
        return;
    }
    if (std::holds_alternative<std::vector<uint16_t>>(m_values))
    {
        m_values = Widen<uint32_t>(std::get<std::vector<uint16_t>>(m_values));
    }
}

std::shared_ptr<const UbNextHopIndex>
UbNextHopIndex::Build(const std::vector<utils::UbStructuredTableRoute>& routes, uint32_t nodeCount)
{
    UbNextHopIndexBuilder builder(nodeCount, routes.size());
    for (const auto& route : routes)
    {
        builder.AddRoute(route);
    }
    return builder.Build();
}

std::shared_ptr<const UbNextHopIndex>
UbNextHopIndex::Read(std::istream& input,
                     const std::vector<std::vector<bool>>& connectedPorts,
                     const std::string& expectedFingerprint)
{
    std::array<uint8_t, COMPRESSED_ROUTE_HEADER_SIZE> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    NS_ABORT_MSG_IF(static_cast<std::size_t>(input.gcount()) != header.size(),
                    "truncated compressed routing header");
    NS_ABORT_MSG_IF(
        !std::equal(COMPRESSED_ROUTE_MAGIC.begin(), COMPRESSED_ROUTE_MAGIC.end(), header.begin()),
        "invalid compressed routing magic");

    const std::vector<uint8_t> headerBytes(header.begin(), header.end());
    std::size_t offset = COMPRESSED_ROUTE_MAGIC.size();
    const uint16_t major = ReadLittleUint16(headerBytes, offset);
    const uint16_t minor = ReadLittleUint16(headerBytes, offset);
    NS_ABORT_MSG_IF(major != COMPRESSED_ROUTE_MAJOR || minor > COMPRESSED_ROUTE_MINOR,
                    "unsupported compressed routing format version " << major << "." << minor);
    NS_ABORT_MSG_IF(ReadLittleUint32(headerBytes, offset) != COMPRESSED_ROUTE_ENDIAN_MARKER,
                    "invalid compressed routing byte-order marker");
    NS_ABORT_MSG_IF(ReadLittleUint32(headerBytes, offset) != COMPRESSED_ROUTE_HEADER_SIZE,
                    "invalid compressed routing header size");
    const uint32_t nodeCount = ReadLittleUint32(headerBytes, offset);
    const uint32_t rowCount = ReadLittleUint32(headerBytes, offset);
    const uint32_t destinationCount = ReadLittleUint32(headerBytes, offset);
    const uint32_t endpointCount = ReadLittleUint32(headerBytes, offset);
    const uint32_t classCount = ReadLittleUint32(headerBytes, offset);
    NS_ABORT_MSG_IF(connectedPorts.empty(), "compressed routing requires at least one node");
    NS_ABORT_MSG_IF(nodeCount != connectedPorts.size(),
                    "compressed routing node count differs from the prepared topology");
    NS_ABORT_MSG_IF(rowCount > nodeCount || destinationCount > nodeCount || endpointCount == 0 ||
                        classCount == 0,
                    "invalid compressed routing dimensions");
    const uint32_t portWidth = headerBytes[offset++];
    const uint32_t nodeWidth = headerBytes[offset++];
    NS_ABORT_MSG_IF((portWidth != 1 && portWidth != 2 && portWidth != 4) ||
                        (nodeWidth != 1 && nodeWidth != 2 && nodeWidth != 4),
                    "compressed routing class ID width must be 1, 2, or 4 bytes");
    NS_ABORT_MSG_IF(ReadLittleUint16(headerBytes, offset) != 0,
                    "compressed routing reserved header bits must be zero");

    const auto expected = ParseFingerprint(expectedFingerprint);
    NS_ABORT_MSG_IF(offset + expected.size() > headerBytes.size(),
                    "truncated compressed routing fingerprint");
    NS_ABORT_MSG_IF(!std::equal(expected.begin(), expected.end(), headerBytes.begin() + offset),
                    "compressed routing artifact does not match the prepared topology");
    offset += expected.size();
    const uint64_t payloadSize = ReadLittleUint64(headerBytes, offset);
    const uint32_t expectedCrc = ReadLittleUint32(headerBytes, offset);
    NS_ABORT_MSG_IF(offset != headerBytes.size(), "invalid compressed routing header layout");
    NS_ABORT_MSG_IF(payloadSize > std::numeric_limits<std::size_t>::max(),
                    "compressed routing payload is too large for this platform");

    const std::size_t portCellCount = CheckedProduct(rowCount, endpointCount, "port lookup");
    const std::size_t nodeCellCount = CheckedProduct(rowCount, destinationCount, "node lookup");
    std::size_t minimumPayloadSize =
        CheckedProduct(nodeCount, sizeof(uint32_t) * 2, "node mappings");
    minimumPayloadSize = CheckedAdd(
        minimumPayloadSize,
        CheckedProduct(CheckedProduct(nodeCount, PORTS_PER_ADDRESS_BLOCK, "endpoint mapping"),
                       sizeof(uint32_t),
                       "endpoint mapping"),
        "fixed fields");
    minimumPayloadSize =
        CheckedAdd(minimumPayloadSize,
                   CheckedProduct(classCount, sizeof(uint32_t) * 2, "route class headers"),
                   "route classes");
    minimumPayloadSize = CheckedAdd(minimumPayloadSize,
                                    CheckedProduct(portCellCount, portWidth, "port class IDs"),
                                    "port class IDs");
    minimumPayloadSize = CheckedAdd(minimumPayloadSize,
                                    CheckedProduct(nodeCellCount, nodeWidth, "node class IDs"),
                                    "node class IDs");
    NS_ABORT_MSG_IF(payloadSize < minimumPayloadSize,
                    "compressed routing payload is smaller than its declared dimensions");

    constexpr std::size_t READ_CHUNK_SIZE = 64 * 1024;
    std::vector<uint8_t> payload;
    const std::streampos payloadStart = input.tellg();
    if (payloadStart != std::streampos(-1))
    {
        input.seekg(0, std::ios::end);
        const std::streampos fileEnd = input.tellg();
        NS_ABORT_MSG_IF(fileEnd == std::streampos(-1) || fileEnd < payloadStart ||
                            static_cast<uint64_t>(fileEnd - payloadStart) != payloadSize,
                        "compressed routing payload length differs from the file");
        input.seekg(payloadStart);
        payload.resize(static_cast<std::size_t>(payloadSize));
        input.read(reinterpret_cast<char*>(payload.data()), payload.size());
        NS_ABORT_MSG_IF(static_cast<std::size_t>(input.gcount()) != payload.size(),
                        "truncated compressed routing payload");
    }
    else
    {
        input.clear();
        payload.reserve(
            std::min<std::size_t>(static_cast<std::size_t>(payloadSize), READ_CHUNK_SIZE));
        std::array<char, READ_CHUNK_SIZE> chunk{};
        std::size_t remaining = static_cast<std::size_t>(payloadSize);
        while (remaining != 0)
        {
            const std::size_t requested = std::min(remaining, chunk.size());
            input.read(chunk.data(), requested);
            const std::size_t received = static_cast<std::size_t>(input.gcount());
            payload.insert(payload.end(), chunk.begin(), chunk.begin() + received);
            NS_ABORT_MSG_IF(received != requested, "truncated compressed routing payload");
            remaining -= received;
        }
    }
    char trailing = 0;
    NS_ABORT_MSG_IF(input.get(trailing), "compressed routing artifact contains trailing bytes");
    NS_ABORT_MSG_IF(CalculateCrc32(payload) != expectedCrc,
                    "compressed routing payload checksum mismatch");

    auto index = std::shared_ptr<UbNextHopIndex>(new UbNextHopIndex());
    index->m_rowCount = rowCount;
    index->m_destinationNodeCount = destinationCount;
    index->m_endpointCount = endpointCount;
    offset = 0;
    index->m_rowByNodeId.resize(nodeCount);
    for (auto& value : index->m_rowByNodeId)
    {
        value = ReadLittleUint32(payload, offset);
        NS_ABORT_MSG_IF(value != INVALID_INDEX && value >= rowCount,
                        "compressed routing row mapping is out of range");
    }
    std::vector<const std::vector<bool>*> connectedPortsByRow(rowCount, nullptr);
    std::vector<bool> mappedRows(rowCount, false);
    for (uint32_t nodeId = 0; nodeId < nodeCount; ++nodeId)
    {
        const uint32_t row = index->m_rowByNodeId[nodeId];
        if (row == INVALID_INDEX)
        {
            continue;
        }
        NS_ABORT_MSG_IF(mappedRows[row], "compressed routing maps multiple nodes to one row");
        mappedRows[row] = true;
        connectedPortsByRow[row] = &connectedPorts[nodeId];
    }
    NS_ABORT_MSG_IF(std::find(mappedRows.begin(), mappedRows.end(), false) != mappedRows.end(),
                    "compressed routing contains an unmapped row");
    index->m_destinationIndexByNodeId.resize(nodeCount);
    std::vector<bool> mappedDestinations(destinationCount, false);
    for (auto& value : index->m_destinationIndexByNodeId)
    {
        value = ReadLittleUint32(payload, offset);
        NS_ABORT_MSG_IF(value != INVALID_INDEX && value >= destinationCount,
                        "compressed routing destination mapping is out of range");
        if (value != INVALID_INDEX)
        {
            NS_ABORT_MSG_IF(mappedDestinations[value],
                            "compressed routing maps multiple nodes to one destination");
            mappedDestinations[value] = true;
        }
    }
    NS_ABORT_MSG_IF(std::find(mappedDestinations.begin(), mappedDestinations.end(), false) !=
                        mappedDestinations.end(),
                    "compressed routing contains an unmapped destination");
    index->m_endpointByAddress.resize(
        CheckedProduct(nodeCount, PORTS_PER_ADDRESS_BLOCK, "endpoint mapping"));
    std::vector<bool> mappedEndpoints(endpointCount, false);
    for (auto& value : index->m_endpointByAddress)
    {
        value = ReadLittleUint32(payload, offset);
        NS_ABORT_MSG_IF(value != INVALID_INDEX && value >= endpointCount,
                        "compressed routing endpoint mapping is out of range");
        if (value != INVALID_INDEX)
        {
            NS_ABORT_MSG_IF(mappedEndpoints[value],
                            "compressed routing maps multiple addresses to one endpoint");
            mappedEndpoints[value] = true;
        }
    }
    NS_ABORT_MSG_IF(std::find(mappedEndpoints.begin(), mappedEndpoints.end(), false) !=
                        mappedEndpoints.end(),
                    "compressed routing contains an unmapped endpoint");

    index->m_classes.reserve(classCount);
    const std::size_t largestNodePortCount =
        std::max_element(
            connectedPorts.begin(),
            connectedPorts.end(),
            [](const auto& left, const auto& right) { return left.size() < right.size(); })
            ->size();
    for (uint32_t classId = 0; classId < classCount; ++classId)
    {
        const uint32_t shortestCount = ReadLittleUint32(payload, offset);
        const uint32_t nonShortestCount = ReadLittleUint32(payload, offset);
        NS_ABORT_MSG_IF(shortestCount > largestNodePortCount ||
                            nonShortestCount > largestNodePortCount,
                        "compressed routing candidate count exceeds every node port count");
        const std::size_t candidateCount =
            CheckedAdd(shortestCount, nonShortestCount, "route candidates");
        NS_ABORT_MSG_IF(candidateCount > (payload.size() - offset) / sizeof(uint16_t),
                        "truncated compressed routing candidates");
        RouteCandidates candidates;
        candidates.shortestPorts.resize(shortestCount);
        candidates.nonShortestPorts.resize(nonShortestCount);
        for (auto& port : candidates.shortestPorts)
        {
            port = ReadLittleUint16(payload, offset);
        }
        for (auto& port : candidates.nonShortestPorts)
        {
            port = ReadLittleUint16(payload, offset);
        }
        NS_ABORT_MSG_IF(classId == 0 && (!candidates.shortestPorts.empty() ||
                                         !candidates.nonShortestPorts.empty()),
                        "compressed routing class zero must be empty");
        NS_ABORT_MSG_IF(classId != 0 && candidates.shortestPorts.empty() &&
                            candidates.nonShortestPorts.empty(),
                        "compressed routing nonzero class must contain a candidate");
        NS_ABORT_MSG_IF(
            !std::is_sorted(candidates.shortestPorts.begin(), candidates.shortestPorts.end()) ||
                std::adjacent_find(candidates.shortestPorts.begin(),
                                   candidates.shortestPorts.end()) !=
                    candidates.shortestPorts.end() ||
                !std::is_sorted(candidates.nonShortestPorts.begin(),
                                candidates.nonShortestPorts.end()) ||
                std::adjacent_find(candidates.nonShortestPorts.begin(),
                                   candidates.nonShortestPorts.end()) !=
                    candidates.nonShortestPorts.end(),
            "compressed routing candidate ports must be sorted and unique");
        std::vector<uint16_t> overlap;
        std::set_intersection(candidates.shortestPorts.begin(),
                              candidates.shortestPorts.end(),
                              candidates.nonShortestPorts.begin(),
                              candidates.nonShortestPorts.end(),
                              std::back_inserter(overlap));
        NS_ABORT_MSG_IF(!overlap.empty(),
                        "compressed routing shortest and non-shortest candidates overlap");
        index->m_classes.push_back(std::move(candidates));
    }

    index->m_portRoutes = PackedIds::Read(payload, offset, portCellCount, portWidth);
    index->m_nodeRoutes = PackedIds::Read(payload, offset, nodeCellCount, nodeWidth);
    NS_ABORT_MSG_IF(offset != payload.size(), "compressed routing payload has unused bytes");
    auto validateClassForRow = [&](uint32_t row, uint32_t classId) {
        NS_ABORT_MSG_IF(classId >= classCount, "compressed routing class ID is out of range");
        if (classId == 0)
        {
            return;
        }
        const auto& ports = *connectedPortsByRow[row];
        const auto& candidates = index->m_classes[classId];
        auto validatePorts = [&](const std::vector<uint16_t>& candidatePorts) {
            for (uint16_t port : candidatePorts)
            {
                NS_ABORT_MSG_IF(
                    port >= ports.size() || !ports[port],
                    "compressed routing candidate port is not connected on the source node");
            }
        };
        validatePorts(candidates.shortestPorts);
        validatePorts(candidates.nonShortestPorts);
    };
    for (uint32_t row = 0; row < rowCount; ++row)
    {
        std::vector<uint32_t> usedClassIds;
        usedClassIds.reserve(CheckedAdd(endpointCount, destinationCount, "row class IDs"));
        for (uint32_t endpoint = 0; endpoint < endpointCount; ++endpoint)
        {
            usedClassIds.push_back(
                index->m_portRoutes.Get(static_cast<std::size_t>(row) * endpointCount + endpoint));
        }
        for (uint32_t destination = 0; destination < destinationCount; ++destination)
        {
            usedClassIds.push_back(index->m_nodeRoutes.Get(
                static_cast<std::size_t>(row) * destinationCount + destination));
        }
        std::sort(usedClassIds.begin(), usedClassIds.end());
        usedClassIds.erase(std::unique(usedClassIds.begin(), usedClassIds.end()),
                           usedClassIds.end());
        for (uint32_t classId : usedClassIds)
        {
            validateClassForRow(row, classId);
        }
    }
    return index;
}

uint32_t
UbNextHopIndex::Intern(RouteCandidates candidates)
{
    NormalizePorts(candidates.shortestPorts);
    NormalizePorts(candidates.nonShortestPorts);
    std::vector<uint16_t> overlap;
    std::set_intersection(candidates.shortestPorts.begin(),
                          candidates.shortestPorts.end(),
                          candidates.nonShortestPorts.begin(),
                          candidates.nonShortestPorts.end(),
                          std::back_inserter(overlap));
    NS_ABORT_MSG_IF(!overlap.empty(),
                    "next-hop shortest and non-shortest candidates overlap");
    const auto existing = m_classIds.find(candidates);
    if (existing != m_classIds.end())
    {
        return existing->second;
    }
    const uint32_t classId = static_cast<uint32_t>(m_classes.size());
    m_classes.push_back(std::move(candidates));
    m_classIds.emplace(m_classes.back(), classId);
    return classId;
}

UbNextHopIndexBuilder::UbNextHopIndexBuilder(uint32_t nodeCount, std::size_t expectedRouteCount)
    : m_nodeCount(nodeCount),
      m_index(std::shared_ptr<UbNextHopIndex>(new UbNextHopIndex()))
{
    NS_ABORT_MSG_IF(nodeCount == 0, "next-hop index requires at least one node");
    m_index->m_classes.push_back(UbNextHopIndex::RouteCandidates{});
    m_routes.reserve(expectedRouteCount);
}

void
UbNextHopIndexBuilder::AddRoute(const utils::UbStructuredTableRoute& route)
{
    NS_ABORT_MSG_IF(m_built, "cannot add a route after building the next-hop index");
    NS_ABORT_MSG_IF(route.currentNodeId >= m_nodeCount || route.destinationNodeId >= m_nodeCount,
                    "next-hop route references an unknown node");
    NS_ABORT_MSG_IF(route.destinationPortId >= UbNextHopIndex::PORTS_PER_ADDRESS_BLOCK - 1,
                    "next-hop destination port cannot be encoded in a UB address");
    NS_ABORT_MSG_IF(route.shortestPorts.empty() && route.nonShortestPorts.empty(),
                    "next-hop route must contain at least one output port");

    m_currentNodeIds.insert(route.currentNodeId);
    m_destinationNodeIds.insert(route.destinationNodeId);
    m_endpointKeys.emplace(route.destinationNodeId, route.destinationPortId);
    const uint32_t classId = m_index->Intern({route.shortestPorts, route.nonShortestPorts});
    m_routes.push_back(
        {route.currentNodeId, route.destinationNodeId, route.destinationPortId, classId});
}

void
UbNextHopIndexBuilder::AddPrimaryRoute(uint32_t currentNodeId,
                                       uint32_t destinationNodeId,
                                       const std::vector<uint16_t>& shortestPorts,
                                       const std::vector<uint16_t>& nonShortestPorts)
{
    NS_ABORT_MSG_IF(m_built, "cannot add a route after building the next-hop index");
    NS_ABORT_MSG_IF(currentNodeId >= m_nodeCount || destinationNodeId >= m_nodeCount,
                    "next-hop primary route references an unknown node");
    NS_ABORT_MSG_IF(shortestPorts.empty() && nonShortestPorts.empty(),
                    "next-hop primary route must contain at least one output port");
    m_currentNodeIds.insert(currentNodeId);
    m_destinationNodeIds.insert(destinationNodeId);
    const uint32_t classId = m_index->Intern({shortestPorts, nonShortestPorts});
    m_primaryRoutes.push_back({currentNodeId, destinationNodeId, classId});
}

std::shared_ptr<const UbNextHopIndex>
UbNextHopIndexBuilder::Build()
{
    NS_ABORT_MSG_IF(m_built, "next-hop index builder can only build once");
    NS_ABORT_MSG_IF(m_routes.empty(), "next-hop index requires at least one route");
    m_built = true;
    auto& index = *m_index;

    index.m_rowCount = static_cast<uint32_t>(m_currentNodeIds.size());
    index.m_endpointCount = static_cast<uint32_t>(m_endpointKeys.size());
    index.m_destinationNodeCount = static_cast<uint32_t>(m_destinationNodeIds.size());
    index.m_rowByNodeId.assign(m_nodeCount, UbNextHopIndex::INVALID_INDEX);
    index.m_destinationIndexByNodeId.assign(m_nodeCount, UbNextHopIndex::INVALID_INDEX);
    index.m_endpointByAddress.assign(static_cast<std::size_t>(m_nodeCount) *
                                         UbNextHopIndex::PORTS_PER_ADDRESS_BLOCK,
                                     UbNextHopIndex::INVALID_INDEX);
    index.m_endpointsByDestination.resize(index.m_destinationNodeCount);

    uint32_t row = 0;
    for (uint32_t nodeId : m_currentNodeIds)
    {
        index.m_rowByNodeId[nodeId] = row++;
    }
    uint32_t destinationIndex = 0;
    for (uint32_t nodeId : m_destinationNodeIds)
    {
        index.m_destinationIndexByNodeId[nodeId] = destinationIndex++;
    }
    uint32_t endpointIndex = 0;
    for (const auto& [nodeId, portId] : m_endpointKeys)
    {
        index.m_endpointByAddress[static_cast<std::size_t>(nodeId) *
                                      UbNextHopIndex::PORTS_PER_ADDRESS_BLOCK +
                                  portId] = endpointIndex;
        index.m_endpointsByDestination[index.m_destinationIndexByNodeId[nodeId]].push_back(
            endpointIndex);
        ++endpointIndex;
    }

    index.m_portRoutes = UbNextHopIndex::PackedIds(
        static_cast<std::size_t>(index.m_rowCount) * index.m_endpointCount,
        static_cast<uint32_t>(index.m_classes.size() - 1));
    for (const auto& route : m_routes)
    {
        const uint32_t routeRow = index.m_rowByNodeId[route.currentNodeId];
        const uint32_t routeEndpoint =
            index.m_endpointByAddress[static_cast<std::size_t>(route.destinationNodeId) *
                                          UbNextHopIndex::PORTS_PER_ADDRESS_BLOCK +
                                      route.destinationPortId];
        const std::size_t cell =
            static_cast<std::size_t>(routeRow) * index.m_endpointCount + routeEndpoint;
        uint32_t classId = route.classId;
        const uint32_t existingClassId = index.m_portRoutes.Get(cell);
        if (existingClassId != 0)
        {
            UbNextHopIndex::RouteCandidates candidates = index.m_classes[classId];
            const auto& existing = index.m_classes[existingClassId];
            candidates.shortestPorts.insert(candidates.shortestPorts.end(),
                                            existing.shortestPorts.begin(),
                                            existing.shortestPorts.end());
            candidates.nonShortestPorts.insert(candidates.nonShortestPorts.end(),
                                               existing.nonShortestPorts.begin(),
                                               existing.nonShortestPorts.end());
            classId = index.Intern(std::move(candidates));
        }
        index.m_portRoutes.Set(cell, classId);
    }

    index.m_nodeRoutes = UbNextHopIndex::PackedIds(
        static_cast<std::size_t>(index.m_rowCount) * index.m_destinationNodeCount,
        static_cast<uint32_t>(index.m_classes.size() - 1));
    for (uint32_t routeRow = 0; routeRow < index.m_rowCount; ++routeRow)
    {
        for (uint32_t destination = 0; destination < index.m_destinationNodeCount; ++destination)
        {
            UbNextHopIndex::RouteCandidates candidates;
            for (uint32_t endpoint : index.m_endpointsByDestination[destination])
            {
                const uint32_t classId = index.m_portRoutes.Get(
                    static_cast<std::size_t>(routeRow) * index.m_endpointCount + endpoint);
                if (classId == 0)
                {
                    continue;
                }
                const auto& endpointCandidates = index.m_classes[classId];
                candidates.shortestPorts.insert(candidates.shortestPorts.end(),
                                                endpointCandidates.shortestPorts.begin(),
                                                endpointCandidates.shortestPorts.end());
                candidates.nonShortestPorts.insert(candidates.nonShortestPorts.end(),
                                                   endpointCandidates.nonShortestPorts.begin(),
                                                   endpointCandidates.nonShortestPorts.end());
            }
            if (!candidates.shortestPorts.empty() || !candidates.nonShortestPorts.empty())
            {
                index.m_nodeRoutes.Set(
                    static_cast<std::size_t>(routeRow) * index.m_destinationNodeCount + destination,
                    index.Intern(std::move(candidates)));
            }
        }
    }
    for (const auto& route : m_primaryRoutes)
    {
        const uint32_t routeRow = index.m_rowByNodeId[route.currentNodeId];
        const uint32_t destination = index.m_destinationIndexByNodeId[route.destinationNodeId];
        index.m_nodeRoutes.Set(static_cast<std::size_t>(routeRow) * index.m_destinationNodeCount +
                                   destination,
                               route.classId);
    }
    index.m_classIds.clear();
    std::vector<PendingRoute>().swap(m_routes);
    std::vector<PendingPrimaryRoute>().swap(m_primaryRoutes);
    return m_index;
}

bool
UbNextHopIndex::TryLookup(uint32_t currentNodeId,
                          uint32_t destinationNodeId,
                          uint32_t destinationPortId,
                          std::vector<uint16_t>& shortestPorts,
                          std::vector<uint16_t>& nonShortestPorts) const
{
    shortestPorts.clear();
    nonShortestPorts.clear();
    if (currentNodeId >= m_rowByNodeId.size() ||
        destinationNodeId >= m_destinationIndexByNodeId.size())
    {
        return false;
    }
    const uint32_t row = m_rowByNodeId[currentNodeId];
    const uint32_t destination = m_destinationIndexByNodeId[destinationNodeId];
    if (row == INVALID_INDEX || destination == INVALID_INDEX)
    {
        return false;
    }

    uint32_t classId = 0;
    if (destinationPortId != ANY_PORT)
    {
        if (destinationPortId >= PORTS_PER_ADDRESS_BLOCK - 1)
        {
            return false;
        }
        const uint32_t endpoint = m_endpointByAddress[static_cast<std::size_t>(destinationNodeId) *
                                                          PORTS_PER_ADDRESS_BLOCK +
                                                      destinationPortId];
        if (endpoint != INVALID_INDEX)
        {
            classId = m_portRoutes.Get(static_cast<std::size_t>(row) * m_endpointCount + endpoint);
        }
    }
    else
    {
        classId =
            m_nodeRoutes.Get(static_cast<std::size_t>(row) * m_destinationNodeCount + destination);
    }
    if (classId == 0 || classId >= m_classes.size())
    {
        return false;
    }

    shortestPorts = m_classes[classId].shortestPorts;
    nonShortestPorts = m_classes[classId].nonShortestPorts;
    return true;
}

uint32_t
UbNextHopIndex::GetRouteClassCount() const
{
    return static_cast<uint32_t>(m_classes.size() - 1);
}

uint32_t
UbNextHopIndex::GetPortIdWidthBits() const
{
    return m_portRoutes.GetWidthBits();
}

uint32_t
UbNextHopIndex::GetNodeIdWidthBits() const
{
    return m_nodeRoutes.GetWidthBits();
}

} // namespace ns3
