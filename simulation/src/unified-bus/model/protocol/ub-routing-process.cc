// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-routing-process.h"
#include "ns3/ub-enum-accessor.h"

#include "ns3/enum.h"
#include "ns3/ipv4-address.h"
#include "ns3/log.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-next-hop-index.h"
#include "ns3/ub-routing-policy.h"
#include "ns3/ub-rule-routing.h"
#include "ns3/hash.h"

#include <algorithm>
#include <sstream>

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED(UbRoutingProcess);
NS_LOG_COMPONENT_DEFINE("UbRoutingProcess");

namespace
{

uint64_t
BuildRoutingPhaseHash(uint32_t seed,
                      uint32_t nodeId,
                      const std::vector<uint16_t>& candidatePorts)
{
    std::ostringstream key;
    key << "ROUTING_RR_PHASE:" << seed << ':' << nodeId;
    for (uint16_t port : candidatePorts)
    {
        key << ':' << port;
    }
    return Hash64(key.str());
}
void
RemoveIngressPort(uint16_t inPort,
                  std::vector<uint16_t>& shortestPorts,
                  std::vector<uint16_t>& nonShortestPorts)
{
    if (inPort == UINT16_MAX)
    {
        return;
    }
    auto remove = [inPort](std::vector<uint16_t>& ports) {
        ports.erase(std::remove(ports.begin(), ports.end(), inPort), ports.end());
    };
    remove(shortestPorts);
    remove(nonShortestPorts);
}

void
RetainAllowedPorts(const std::vector<uint16_t>& allowedPorts,
                   std::vector<uint16_t>& shortestPorts,
                   std::vector<uint16_t>& nonShortestPorts)
{
    const auto isDisallowed = [&](uint16_t port) {
        return std::find(allowedPorts.begin(), allowedPorts.end(), port) == allowedPorts.end();
    };
    shortestPorts.erase(std::remove_if(shortestPorts.begin(), shortestPorts.end(), isDisallowed),
                        shortestPorts.end());
    nonShortestPorts.erase(
        std::remove_if(nonShortestPorts.begin(), nonShortestPorts.end(), isDisallowed),
        nonShortestPorts.end());
}
} // namespace

TypeId
UbRoutingProcess::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::UbRoutingProcess")
            .SetParent<Object>()
            .SetGroupName("UnifiedBus")
            .AddConstructor<UbRoutingProcess>()
            .AddAttribute(
                "MultipathSelector",
                "Selector used to choose one path from the routing candidate set.",
                MakeUbEnumValue(MultipathSelector::HASH64),
                MakeUbEnumAccessor(&UbRoutingProcess::m_multipathSelector),
                MakeUbEnumChecker(MultipathSelector::HASH64,
                                "HASH64",
                                MultipathSelector::CRC32,
                                "CRC32",
                                MultipathSelector::TOEPLITZ,
                                "TOEPLITZ",
                                MultipathSelector::ROUND_ROBIN,
                                "ROUND_ROBIN",
                                MultipathSelector::ADAPTIVE,
                                "ADAPTIVE",
                                MultipathSelector::INGRESS_PORT_STRIPE,
                                "INGRESS_PORT_STRIPE"));
    return tid;
}

UbRoutingProcess::UbRoutingProcess() = default;

void
UbRoutingProcess::SetNextHopIndex(std::shared_ptr<const UbNextHopIndex> nextHopIndex)
{
    m_nextHopIndex = std::move(nextHopIndex);
}

void
UbRoutingProcess::SetRuleRoutingTable(std::shared_ptr<const UbRuleRoutingTable> ruleRoutingTable)
{
    m_ruleRoutingTable = std::move(ruleRoutingTable);
}

void
UbRoutingProcess::ValidateRoutingType(RoutingType routingType, const std::string& owner) const
{
    if (MultipathSelectorIsValidForRoutingType(m_multipathSelector, routingType))
    {
        return;
    }

    const RoutingType compatibleRoutingType =
        MakeRoutingType(m_multipathSelector != MultipathSelector::INGRESS_PORT_STRIPE,
                        RoutingTypeUsesShortestPaths(routingType));
    NS_ABORT_MSG("Invalid routing configuration for "
                 << owner << ": RoutingType \"" << RoutingTypeToString(routingType)
                 << "\" cannot be combined with MultipathSelector \""
                 << MultipathSelectorToString(m_multipathSelector)
                 << "\". Use RoutingType \"" << RoutingTypeToString(compatibleRoutingType)
                 << "\" or MultipathSelector \"HASH64\".");
}


void
UbRoutingProcess::SetNodeId(uint32_t nodeId)
{
    m_nodeId = nodeId;
}

UbRoutingTable&
UbRoutingProcess::GetRoutingTable()
{
    return m_table;
}

const UbRoutingTable&
UbRoutingProcess::GetRoutingTable() const
{
    return m_table;
}

void
UbRoutingProcess::AddShortestRoute(uint32_t destIp, const std::vector<uint16_t>& outPorts)
{
    m_table.AddShortestRoute(destIp, outPorts);
}

void
UbRoutingProcess::AddOtherRoute(uint32_t destIp, const std::vector<uint16_t>& outPorts)
{
    m_table.AddOtherRoute(destIp, outPorts);
}

void
UbRoutingProcess::AddShortestRouteRange(uint32_t startNodeId,
                                        uint32_t endNodeId,
                                        const std::vector<uint16_t>& outPorts)
{
    m_table.AddShortestRouteRange(startNodeId, endNodeId, outPorts);
}

void
UbRoutingProcess::AddOtherRouteRange(uint32_t startNodeId,
                                     uint32_t endNodeId,
                                     const std::vector<uint16_t>& outPorts)
{
    m_table.AddOtherRouteRange(startNodeId, endNodeId, outPorts);
}

void
UbRoutingProcess::AddShortestRouteRange(uint32_t startNodeId,
                                        uint32_t endNodeId,
                                        uint32_t dstPortId,
                                        const std::vector<uint16_t>& outPorts)
{
    m_table.AddShortestRouteRange(startNodeId, endNodeId, dstPortId, outPorts);
}

void
UbRoutingProcess::AddOtherRouteRange(uint32_t startNodeId,
                                     uint32_t endNodeId,
                                     uint32_t dstPortId,
                                     const std::vector<uint16_t>& outPorts)
{
    m_table.AddOtherRouteRange(startNodeId, endNodeId, dstPortId, outPorts);
}

bool
UbRoutingProcess::TryGetIndexedCandidates(
    uint32_t dip,
    std::vector<uint16_t>& shortestPorts,
    std::vector<uint16_t>& nonShortestPorts) const
{
    if (m_nextHopIndex == nullptr)
    {
        return false;
    }
    const uint32_t destinationNodeId = utils::IpToNodeId(Ipv4Address(dip));
    const uint32_t lastByte = dip & 0xff;
    const uint32_t destinationPortId =
        lastByte == 0 ? UbRuleDestination::ANY_PORT : lastByte - 1;
    return m_nextHopIndex->TryLookup(m_nodeId,
                                     destinationNodeId,
                                     destinationPortId,
                                     shortestPorts,
                                     nonShortestPorts);
}

bool
UbRoutingProcess::TryGetRuleCandidates(
    uint32_t dip,
    std::vector<uint16_t>& shortestPorts,
    std::vector<uint16_t>& nonShortestPorts) const
{
    if (m_ruleRoutingTable == nullptr)
    {
        return false;
    }
    const uint32_t destinationNodeId = utils::IpToNodeId(Ipv4Address(dip));
    const uint32_t lastByte = dip & 0xff;
    const uint32_t destinationPortId =
        lastByte == 0 ? UbRuleDestination::ANY_PORT : lastByte - 1;
    return m_ruleRoutingTable->TryLookup(m_nodeId,
                                         destinationNodeId,
                                         destinationPortId,
                                         shortestPorts,
                                         nonShortestPorts);
}

bool
UbRoutingProcess::TryGetRuleCandidatesExact(
    uint32_t dip,
    std::vector<uint16_t>& shortestPorts,
    std::vector<uint16_t>& nonShortestPorts) const
{
    if (m_ruleRoutingTable == nullptr)
    {
        return false;
    }
    const uint32_t destinationNodeId = utils::IpToNodeId(Ipv4Address(dip));
    const uint32_t lastByte = dip & 0xff;
    const uint32_t destinationPortId =
        lastByte == 0 ? UbRuleDestination::ANY_PORT : lastByte - 1;
    return m_ruleRoutingTable->TryLookupExact(m_nodeId,
                                              destinationNodeId,
                                              destinationPortId,
                                              shortestPorts,
                                              nonShortestPorts);
}

bool
UbRoutingProcess::TryGetRuleCandidatesWithPrimaryFallback(
    uint32_t dip,
    uint16_t inPort,
    std::vector<uint16_t>& shortestPorts,
    std::vector<uint16_t>& nonShortestPorts) const
{
    if (m_ruleRoutingTable == nullptr)
    {
        return false;
    }
    const uint32_t destinationNodeId = utils::IpToNodeId(Ipv4Address(dip));
    const uint32_t lastByte = dip & 0xff;
    const uint32_t destinationPortId =
        lastByte == 0 ? UbRuleDestination::ANY_PORT : lastByte - 1;
    if (!m_ruleRoutingTable->TryLookupWithPrimaryFallback(m_nodeId,
                                                          destinationNodeId,
                                                          destinationPortId,
                                                          shortestPorts,
                                                          nonShortestPorts))
    {
        return false;
    }
    RemoveIngressPort(inPort, shortestPorts, nonShortestPorts);
    return true;
}

bool
UbRoutingProcess::TryGetIndexedCandidatesWithPrimaryFallback(
    uint32_t dip,
    uint16_t inPort,
    std::vector<uint16_t>& shortestPorts,
    std::vector<uint16_t>& nonShortestPorts) const
{
    const bool foundExact = TryGetIndexedCandidates(dip, shortestPorts, nonShortestPorts);
    const uint32_t primaryDip = Ipv4Address(dip).CombineMask(Ipv4Mask("255.255.255.0")).Get();
    if (primaryDip == dip || (foundExact && !shortestPorts.empty() && !nonShortestPorts.empty()))
    {
        RemoveIngressPort(inPort, shortestPorts, nonShortestPorts);
        return foundExact;
    }

    std::vector<uint16_t> primaryShortestPorts;
    std::vector<uint16_t> primaryNonShortestPorts;
    const bool foundPrimary = TryGetIndexedCandidates(primaryDip,
                                                       primaryShortestPorts,
                                                       primaryNonShortestPorts);
    if (shortestPorts.empty())
    {
        shortestPorts = std::move(primaryShortestPorts);
    }
    if (nonShortestPorts.empty())
    {
        nonShortestPorts = std::move(primaryNonShortestPorts);
    }
    RemoveIngressPort(inPort, shortestPorts, nonShortestPorts);
    return foundExact || foundPrimary;
}

void
UbRoutingProcess::GetShortestOutPorts(uint32_t destIp, std::vector<uint16_t>& outPorts) const
{
    outPorts.clear();
    std::vector<uint16_t> nonShortestPorts;
    if (TryGetIndexedCandidates(destIp, outPorts, nonShortestPorts))
    {
        return;
    }
    const auto ports = m_table.LookupShortest(destIp);
    outPorts.assign(ports.begin(), ports.end());
}

void
UbRoutingProcess::GetOtherOutPorts(uint32_t destIp, std::vector<uint16_t>& outPorts) const
{
    outPorts.clear();
    std::vector<uint16_t> shortestPorts;
    if (TryGetIndexedCandidates(destIp, shortestPorts, outPorts))
    {
        return;
    }
    const auto ports = m_table.LookupOther(destIp);
    outPorts.assign(ports.begin(), ports.end());
}

void
UbRoutingProcess::GetShortestCandidates(uint32_t& dip,
                                        uint16_t inPort,
                                        std::vector<uint16_t>& outPorts) const
{
    GetShortestOutPorts(dip, outPorts);
    if (outPorts.empty())
    {
        const uint32_t primaryDip = Ipv4Address(dip).CombineMask(Ipv4Mask("255.255.255.0")).Get();
        if (primaryDip != dip)
        {
            GetShortestOutPorts(primaryDip, outPorts);
            dip = primaryDip;
        }
    }
    std::vector<uint16_t> unused;
    RemoveIngressPort(inPort, outPorts, unused);
}

void
UbRoutingProcess::GetNonShortestCandidates(uint32_t& dip,
                                           uint16_t inPort,
                                           std::vector<uint16_t>& outPorts) const
{
    GetOtherOutPorts(dip, outPorts);
    if (outPorts.empty())
    {
        const uint32_t primaryDip = Ipv4Address(dip).CombineMask(Ipv4Mask("255.255.255.0")).Get();
        if (primaryDip != dip)
        {
            GetOtherOutPorts(primaryDip, outPorts);
            dip = primaryDip;
        }
    }
    std::vector<uint16_t> unused;
    RemoveIngressPort(inPort, outPorts, unused);
}

std::vector<uint16_t>
UbRoutingProcess::GetAllOutPorts(uint32_t destIp) const
{
    std::vector<uint16_t> shortestPorts;
    std::vector<uint16_t> nonShortestPorts;
    if (!TryGetRuleCandidates(destIp, shortestPorts, nonShortestPorts) &&
        !TryGetIndexedCandidates(destIp, shortestPorts, nonShortestPorts))
    {
        const auto shortest = m_table.LookupShortest(destIp);
        const auto nonShortest = m_table.LookupOther(destIp);
        shortestPorts.assign(shortest.begin(), shortest.end());
        nonShortestPorts.assign(nonShortest.begin(), nonShortest.end());
    }
    shortestPorts.insert(shortestPorts.end(), nonShortestPorts.begin(), nonShortestPorts.end());
    std::sort(shortestPorts.begin(), shortestPorts.end());
    shortestPorts.erase(std::unique(shortestPorts.begin(), shortestPorts.end()),
                        shortestPorts.end());
    return shortestPorts;
}

bool
UbRoutingProcess::RemoveShortestRoute(uint32_t destIp)
{
    return m_table.RemoveShortestRoute(destIp);
}

bool
UbRoutingProcess::RemoveOtherRoute(uint32_t destIp)
{
    return m_table.RemoveOtherRoute(destIp);
}

UbRoutingTable::PortSetView
UbRoutingProcess::LookupCandidatePorts(uint32_t& dip, bool shortest) const
{
    auto ports = shortest ? m_table.LookupShortest(dip) : m_table.LookupOther(dip);
    if (!ports.empty())
    {
        return ports;
    }

    const uint32_t maskedDip = Ipv4Address(dip).CombineMask(Ipv4Mask("255.255.255.0")).Get();
    if (maskedDip == dip)
    {
        return ports;
    }
    ports = shortest ? m_table.LookupShortest(maskedDip) : m_table.LookupOther(maskedDip);
    dip = maskedDip;
    return ports;
}

int
UbRoutingProcess::GetOutPort(RoutingKey& key,
                             bool& selectedShortestPath,
                             uint16_t inPort)
{
    return GetOutPortInternal(key, nullptr, selectedShortestPath, inPort);
}

int
UbRoutingProcess::GetOutPortRestricted(RoutingKey& key,
                                       const std::vector<uint16_t>& allowedOutPorts,
                                       bool& selectedShortestPath,
                                       uint16_t inPort)
{
    return GetOutPortInternal(key, &allowedOutPorts, selectedShortestPath, inPort);
}

int
UbRoutingProcess::GetOutPortAfterFailure(RoutingKey& key,
                                         bool& selectedShortestPath,
                                         uint16_t inPort,
                                         bool exactEndpoint,
                                         const std::vector<uint16_t>* allowedOutPorts)
{
    const bool failureDetour = !m_unavailableOutputs.empty();
    return exactEndpoint
               ? GetOutPortForEndpointInternal(key, allowedOutPorts, selectedShortestPath,
                                                inPort, failureDetour)
               : GetOutPortInternal(key, allowedOutPorts, selectedShortestPath,
                                      inPort, failureDetour);
}

int
UbRoutingProcess::GetOutPortInternal(RoutingKey& key,
                                     const std::vector<uint16_t>* allowedOutPorts,
                                     bool& selectedShortestPath,
                                     uint16_t inPort,
                                     bool failureDetour)
{
    NS_LOG_DEBUG("[UbRoutingProcess GetOutPort]: sip: " << Ipv4Address(key.sip)
                                                        << " dip: " << Ipv4Address(key.dip)
                                                        << " sport: " << key.sport
                                                        << " dport: " << key.dport
                                                        << " priority: "
                                                        << static_cast<uint16_t>(key.priority)
                                                        << " routingType: "
                                                        << static_cast<uint32_t>(key.routingType));

    ValidateRoutingType(key.routingType, "packet forwarding");
    ++m_hotPathLookups;
    m_lastFailureExcludedAllCandidates = false;

    std::vector<uint16_t> structuredShortest;
    std::vector<uint16_t> structuredNonShortest;
    const bool handledByRule = TryGetRuleCandidatesWithPrimaryFallback(
        key.dip,
        inPort,
        structuredShortest,
        structuredNonShortest);
    const bool handledByIndex =
        !handledByRule && TryGetIndexedCandidatesWithPrimaryFallback(key.dip,
                                                                     inPort,
                                                                     structuredShortest,
                                                                     structuredNonShortest);
    if (handledByRule || handledByIndex)
    {
        if (RoutingTypeUsesShortestPaths(key.routingType) && !failureDetour)
        {
            structuredNonShortest.clear();
        }
        if (allowedOutPorts != nullptr)
        {
            RetainAllowedPorts(*allowedOutPorts, structuredShortest, structuredNonShortest);
        }
        const UbRoutingCandidateView structuredCandidates{structuredShortest,
                                                          structuredNonShortest,
                                                          UINT16_MAX};
        if (UbRoutingPolicy::Count(structuredCandidates) == 0)
        {
            NS_LOG_ERROR("No candidate ports found for dip: " << Ipv4Address(key.dip));
            return -1;
        }
        return SelectCandidate(key, structuredCandidates, selectedShortestPath, inPort, failureDetour);
    }

    uint32_t lookupDip = key.dip;
    const auto shortest = LookupCandidatePorts(lookupDip, true);
    UbRoutingTable::PortSetView nonShortest;
    if (!RoutingTypeUsesShortestPaths(key.routingType) || failureDetour)
    {
        nonShortest = LookupCandidatePorts(lookupDip, false);
    }

    if (allowedOutPorts == nullptr)
    {
        const UbRoutingCandidateView candidates{shortest, nonShortest, inPort};
        if (UbRoutingPolicy::Count(candidates) == 0)
        {
            NS_LOG_ERROR("No candidate ports found for dip: " << Ipv4Address(key.dip));
            return -1;
        }
        return SelectCandidate(key, candidates, selectedShortestPath, inPort, failureDetour);
    }

    m_hotPathTemporaryContainerAllocations += 2;
    std::vector<uint16_t> filteredShortest(shortest.begin(), shortest.end());
    std::vector<uint16_t> filteredNonShortest(nonShortest.begin(), nonShortest.end());
    RetainAllowedPorts(*allowedOutPorts, filteredShortest, filteredNonShortest);

    const UbRoutingCandidateView candidates{filteredShortest, filteredNonShortest, inPort};
    if (UbRoutingPolicy::Count(candidates) == 0)
    {
        NS_LOG_ERROR("No candidate ports found for dip: " << Ipv4Address(key.dip));
        return -1;
    }

    return SelectCandidate(key, candidates, selectedShortestPath, inPort, failureDetour);
}

int
UbRoutingProcess::GetOutPortForEndpoint(RoutingKey& key,
                                        bool& selectedShortestPath,
                                        uint16_t inPort)
{
    return GetOutPortForEndpointInternal(key, nullptr, selectedShortestPath, inPort);
}

int
UbRoutingProcess::GetOutPortForEndpointRestricted(
    RoutingKey& key,
    const std::vector<uint16_t>& allowedOutPorts,
    bool& selectedShortestPath,
    uint16_t inPort)
{
    return GetOutPortForEndpointInternal(key, &allowedOutPorts, selectedShortestPath, inPort);
}

int
UbRoutingProcess::GetOutPortForEndpointInternal(RoutingKey& key,
                                                const std::vector<uint16_t>* allowedOutPorts,
                                                bool& selectedShortestPath,
                                                uint16_t inPort,
                                     bool failureDetour)
{
    ValidateRoutingType(key.routingType, "endpoint packet forwarding");
    m_lastFailureExcludedAllCandidates = false;
    std::vector<uint16_t> shortestPorts;
    std::vector<uint16_t> nonShortestPorts;
    const bool handledByRule = TryGetRuleCandidatesExact(key.dip, shortestPorts, nonShortestPorts);
    const bool handledByIndex =
        !handledByRule && TryGetIndexedCandidates(key.dip, shortestPorts, nonShortestPorts);

    if (!handledByRule && !handledByIndex)
    {
        GetShortestOutPorts(key.dip, shortestPorts);
        if (!RoutingTypeUsesShortestPaths(key.routingType) || failureDetour)
        {
            GetOtherOutPorts(key.dip, nonShortestPorts);
        }
    }
    if (RoutingTypeUsesShortestPaths(key.routingType) && !failureDetour)
    {
        nonShortestPorts.clear();
    }

    if (allowedOutPorts != nullptr)
    {
        RetainAllowedPorts(*allowedOutPorts, shortestPorts, nonShortestPorts);
    }

    const UbRoutingCandidateView candidates{shortestPorts, nonShortestPorts, inPort};
    if (UbRoutingPolicy::Count(candidates) == 0)
    {
        NS_LOG_ERROR("No exact endpoint candidate ports found for dip: " << Ipv4Address(key.dip));
        return -1;
    }
    return SelectCandidate(key, candidates, selectedShortestPath, inPort, failureDetour);
}

int
UbRoutingProcess::SelectOutPort(RoutingKey& key,
                                const std::vector<uint16_t>& shortestPorts,
                                const std::vector<uint16_t>& nonShortestPorts,
                                bool& selectedShortestPath,
                                uint16_t inPort)
{
    ValidateRoutingType(key.routingType, "packet forwarding");
    m_lastFailureExcludedAllCandidates = false;
    const UbRoutingCandidateView candidates{shortestPorts, nonShortestPorts, UINT16_MAX};
    if (UbRoutingPolicy::Count(candidates) == 0)
    {
        return -1;
    }
    return SelectCandidate(key, candidates, selectedShortestPath, inPort);
}

int
UbRoutingProcess::SelectAdaptiveOutPort(RoutingKey& key,
                                        const std::vector<uint16_t>& shortestPorts,
                                        const std::vector<uint16_t>& nonShortestPorts,
                                        bool& selectedShortestPath)
{
    const UbRoutingCandidateView candidates{shortestPorts, nonShortestPorts, UINT16_MAX};
    return UbRoutingPolicy::SelectAdaptive(key, candidates, m_nodeId, selectedShortestPath);
}

int
UbRoutingProcess::SelectCandidate(RoutingKey& key,
                                  const UbRoutingCandidateView& candidates,
                                  bool& selectedShortestPath,
                                  uint16_t inPort,
                                  bool failureDetour)
{
    if (failureDetour && RoutingTypeUsesShortestPaths(key.routingType))
    {
        const UbRoutingCandidateView shortest{candidates.shortest, {}, candidates.excludedPort};
        if (UbRoutingPolicy::Count(shortest) == 0)
        {
            // Missing or forbidden shortest candidates do not confer detour permission.
            m_lastFailureExcludedAllCandidates = false;
            return -1;
        }
        const int preferred = SelectCandidate(key, shortest, selectedShortestPath, inPort);
        if (preferred >= 0 || !m_lastFailureExcludedAllCandidates)
        {
            return preferred;
        }
        const UbRoutingCandidateView detour{{}, candidates.nonShortest, candidates.excludedPort};
        if (UbRoutingPolicy::Count(detour) == 0)
        {
            return -1;
        }
        const int output = SelectCandidate(key, detour, selectedShortestPath, inPort);
        if (output < 0)
        {
            m_lastFailureExcludedAllCandidates = true;
        }
        return output;
    }

    if (m_unavailableOutputs.empty())
    {
        m_lastFailureExcludedAllCandidates = false;
        return SelectCandidateFromView(key, candidates, selectedShortestPath, inPort);
    }

    const auto countLegal = [&candidates](const UbRoutingCandidateView& view) {
        size_t count = 0;
        for (const auto& ports : {view.shortest, view.nonShortest})
        {
            for (uint16_t port : ports)
            {
                if (port != view.excludedPort)
                {
                    ++count;
                }
            }
        }
        return count;
    };
    const bool hadLegalCandidates = countLegal(candidates) != 0;

    // Failure-aware filtering keeps a single candidate-selection path: the failed output is
    // removed before hash, round-robin, adaptive, or ingress-stripe selection can pick it, and no
    // compiled routing artifact changes.
    const auto isUnavailable = [this](uint16_t port) { return IsOutputUnavailable(port); };
    m_filteredShortest.assign(candidates.shortest.begin(), candidates.shortest.end());
    m_filteredNonShortest.assign(candidates.nonShortest.begin(), candidates.nonShortest.end());
    m_filteredShortest.erase(std::remove_if(m_filteredShortest.begin(),
                                            m_filteredShortest.end(),
                                            isUnavailable),
                             m_filteredShortest.end());
    m_filteredNonShortest.erase(std::remove_if(m_filteredNonShortest.begin(),
                                               m_filteredNonShortest.end(),
                                               isUnavailable),
                                m_filteredNonShortest.end());

    const UbRoutingCandidateView filtered{m_filteredShortest,
                                          m_filteredNonShortest,
                                          candidates.excludedPort};
    if (UbRoutingPolicy::Count(filtered) == 0)
    {
        NS_LOG_ERROR("No candidate ports remain after excluding detected failed outputs for dip: "
                     << Ipv4Address(key.dip));
        m_lastFailureExcludedAllCandidates = hadLegalCandidates;
        return -1;
    }
    m_lastFailureExcludedAllCandidates = false;
    return SelectCandidateFromView(key, filtered, selectedShortestPath, inPort);
}

int
UbRoutingProcess::SelectCandidateFromView(RoutingKey& key,
                                          const UbRoutingCandidateView& candidates,
                                          bool& selectedShortestPath,
                                          uint16_t inPort)
{
    if (m_multipathSelector == MultipathSelector::ADAPTIVE)
    {
        const int outPort =
            UbRoutingPolicy::SelectAdaptive(key, candidates, m_nodeId, selectedShortestPath);
        NS_ASSERT_MSG(outPort != -1, "No available output port found");
        return outPort;
    }

    const size_t candidateCount = UbRoutingPolicy::Count(candidates);
    size_t index = 0;
    if (m_multipathSelector == MultipathSelector::ROUND_ROBIN)
    {
        std::vector<uint16_t> candidatePorts;
        candidatePorts.reserve(candidateCount);
        for (const auto& ports : {candidates.shortest, candidates.nonShortest})
        {
            for (uint16_t port : ports)
            {
                if (port != candidates.excludedPort)
                {
                    candidatePorts.push_back(port);
                }
            }
        }
        uint64_t& nextIndex = m_rrNextIndexByCandidates[candidatePorts];
        if (nextIndex == 0)
        {
            BooleanValue enabled;
            UintegerValue seed;
            if (GlobalValue::GetValueByNameFailSafe("UB_ROUTING_RR_PHASE_HASH_ENABLE", enabled) &&
                enabled.Get() &&
                GlobalValue::GetValueByNameFailSafe("UB_ROUTING_RR_PHASE_SEED", seed))
            {
                nextIndex = BuildRoutingPhaseHash(seed.Get(), m_nodeId, candidatePorts) %
                             candidateCount;
            }
        }
        index = nextIndex++ % candidateCount;
    }
    else if (m_multipathSelector == MultipathSelector::INGRESS_PORT_STRIPE &&
             inPort != UINT16_MAX)
    {
        index = inPort % candidateCount;
    }
    else
    {
        const auto bytes = BuildHashBytes(key);
        uint64_t hash = 0;
        switch (m_multipathSelector)
        {
        case MultipathSelector::CRC32:
            hash = CalcCrc32(bytes);
            break;
        case MultipathSelector::TOEPLITZ:
            hash = CalcToeplitzHash(bytes);
            break;
        case MultipathSelector::HASH64:
        case MultipathSelector::INGRESS_PORT_STRIPE:
            hash = Hash64(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            break;
        default:
            NS_ABORT_MSG("Unsupported multipath selector");
        }
        index = hash % candidateCount;
    }

    int outPort = -1;
    for (const auto& ports : {candidates.shortest, candidates.nonShortest})
    {
        for (uint16_t port : ports)
        {
            if (port == candidates.excludedPort)
            {
                continue;
            }
            if (index-- == 0)
            {
                selectedShortestPath = ports.data() == candidates.shortest.data();
                outPort = port;
                break;
            }
        }
        if (outPort != -1)
        {
            break;
        }
    }
    NS_ASSERT_MSG(outPort != -1, "No available output port found");
    return outPort;
}

void
UbRoutingProcess::SetOutputUnavailable(uint16_t outPort)
{
    m_unavailableOutputs.insert(outPort);
}

void
UbRoutingProcess::ClearOutputUnavailable(uint16_t outPort)
{
    m_unavailableOutputs.erase(outPort);
}

bool
UbRoutingProcess::IsOutputUnavailable(uint16_t outPort) const
{
    return m_unavailableOutputs.find(outPort) != m_unavailableOutputs.end();
}

uint64_t
UbRoutingProcess::GetHotPathLookupCountForTest() const
{
    return m_hotPathLookups;
}

uint64_t
UbRoutingProcess::GetHotPathTemporaryContainerAllocationCountForTest() const
{
    return m_hotPathTemporaryContainerAllocations;
}

std::array<uint8_t, 17>
UbRoutingProcess::BuildHashBytes(const RoutingKey& rtKey) const
{
    const uint32_t salt = utils::NodeIdToIp(m_nodeId).Get();
    return {
        static_cast<uint8_t>((rtKey.sip >> 24) & 0xff),
        static_cast<uint8_t>((rtKey.sip >> 16) & 0xff),
        static_cast<uint8_t>((rtKey.sip >> 8) & 0xff),
        static_cast<uint8_t>(rtKey.sip & 0xff),
        static_cast<uint8_t>((rtKey.dip >> 24) & 0xff),
        static_cast<uint8_t>((rtKey.dip >> 16) & 0xff),
        static_cast<uint8_t>((rtKey.dip >> 8) & 0xff),
        static_cast<uint8_t>(rtKey.dip & 0xff),
        static_cast<uint8_t>((rtKey.sport >> 8) & 0xff),
        static_cast<uint8_t>(rtKey.sport & 0xff),
        static_cast<uint8_t>((rtKey.dport >> 8) & 0xff),
        static_cast<uint8_t>(rtKey.dport & 0xff),
        rtKey.priority,
        static_cast<uint8_t>((salt >> 24) & 0xff),
        static_cast<uint8_t>((salt >> 16) & 0xff),
        static_cast<uint8_t>((salt >> 8) & 0xff),
        static_cast<uint8_t>(salt & 0xff),
    };
}

uint32_t
UbRoutingProcess::CalcCrc32(const std::array<uint8_t, 17>& bytes) const
{
    uint32_t crc = 0xffffffffU;
    for (uint8_t byte : bytes)
    {
        crc ^= byte;
        for (uint8_t bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
        }
    }
    return ~crc;
}

uint32_t
UbRoutingProcess::CalcToeplitzHash(const std::array<uint8_t, 17>& bytes) const
{
    static constexpr std::array<uint8_t, 40> key = {
        0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2,
        0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0,
        0xd0, 0xca, 0x2b, 0xcb, 0xae, 0x7b, 0x30, 0xb4,
        0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30, 0xf2, 0x0c,
        0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa,
    };

    auto keyWindow = [](uint32_t bitOffset) {
        uint32_t value = 0;
        for (uint32_t bit = 0; bit < 32; ++bit)
        {
            const uint32_t position = bitOffset + bit;
            value = (value << 1) |
                    ((key[position / 8] >> (7 - (position % 8))) & 1U);
        }
        return value;
    };

    uint32_t hash = 0;
    uint32_t bitOffset = 0;
    for (uint8_t byte : bytes)
    {
        for (uint8_t bit = 0; bit < 8; ++bit, ++bitOffset)
        {
            if (((byte >> (7 - bit)) & 1U) != 0)
            {
                hash ^= keyWindow(bitOffset);
            }
        }
    }
    return hash;
}

} // namespace ns3
