// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_CONTROL_H
#define UB_CONTROL_H

#include <ns3/core-module.h>
#include <vector>
#include <map>
#include <unordered_map>
#include "ub-datatype.h"
#include "ns3/callback.h"
#include "ns3/ub-port.h"
#include "ns3/ub-switch-allocator.h"
#include "ns3/ub-function.h"
#include "ns3/ub-ctp.h"
#include "ns3/ub-tp-connection-manager.h"
#include "ns3/ub-transport-channel-manager.h"

namespace utils
{
class UbUtils;
}

using namespace utils;
namespace ns3 {

class UbLdstApi;
class UbLdstEndpoint;
class UbFunction;
class UbTransaction;
class UbTransportChannel;
class UbTransportGroup;
class UbPort;

/**
 * @brief UB Management Unit
 */
class UbController : public Object {
public:
    static TypeId GetTypeId();

    UbController();
    ~UbController();

    Ptr<UbTransportChannelManager> GetTransportChannelManager() const;

    Ptr<UbTransportGroup> CreateRtpTpGroup(
        const std::vector<Ptr<UbTransportChannel>>& members,
        uint32_t initialSelectionCursor);
    Ptr<UbTransportGroup> ResolveRtpTpGroup(
        const UbLifecycleResourceIdentity& identity) const;
    Ptr<UbTransportGroup> ResolveRtpTpGroupForMember(
        const UbLifecycleResourceIdentity& tpIdentity) const;
    UbTpgMemberMutationStatus AddRtpTpGroupMember(
        const UbLifecycleResourceIdentity& groupIdentity,
        const UbLifecycleResourceIdentity& tpIdentity);
    UbTpgMemberMutationStatus RemoveRtpTpGroupMember(
        const UbLifecycleResourceIdentity& groupIdentity,
        const UbLifecycleResourceIdentity& tpIdentity,
        UbDestroyReason reason);
    UbDestroyStatus DestroyRtpTpGroup(
        const UbLifecycleResourceIdentity& groupIdentity,
        UbDestroyReason reason = UbDestroyReason::EXPLICIT);
    void NotifyRtpTpGroupWorkChanged(
        const UbLifecycleResourceIdentity& groupIdentity);

    // Port management
    /**
     * @brief Associates a port with a reachable destination.
     * @param port The source port.
     * @param destination The reachable destination address.
     */
    void AddPortDestination(Ptr<UbPort> port, uint32_t destination);

    /**
     * @brief Removes an association between a port and a destination.
     * @param port The source port.
     * @param destination The destination address to remove.
     */
    void RemovePortDestination(Ptr<UbPort> port, uint32_t destination);

    /**
     * @brief Gets all available ports for a given destination.
     * @param destination The destination address to look up.
     * @return A vector of ports that can reach the destination. Returns an empty vector if none found.
     */
    std::vector<Ptr<UbPort>> GetAvailablePorts(uint32_t destination) const;

    Ptr<UbFunction> GetUbFunction();

    Ptr<UbTransaction> GetUbTransaction();

    /**
     * @brief Submit a WQE to a prepared Jetty.
     *
     * The optional observer is installed only after all synchronous admission checks pass and
     * before the WQE is enqueued.  It observes the completed request ID without replacing the
     * Jetty's caller-facing completion callback.
     *
     * @param jettyNum Prepared Jetty receiving the WQE.
     * @param wqe WQE to submit.
     * @param completionObserver Optional internal completion observer.
     * @return True when the WQE was accepted for submission.
     */
    bool SubmitUrmaWqe(uint32_t jettyNum,
                       Ptr<UbWqe> wqe,
                       Callback<void, uint32_t, uint32_t> completionObserver =
                           Callback<void, uint32_t, uint32_t>());

    Ptr<UbLdstEndpoint> GetLdstEndpoint();

    Ptr<UbCtpTransportService> GetCtpTransportService();
    Ptr<UbCtpTransportService> PeekCtpTransportService() const;

    /**
     * @brief Create one CTP Entity from local UB port identifiers.
     * @param entityId Non-zero compact Entity identifier.
     * @param memberPorts Local CNA16-encodable UB port identifiers.
     */
    void CreateCtpEntity(uint32_t entityId, const std::vector<uint32_t>& memberPorts);

    /**
     * @brief Return whether the local registry contains an Entity.
     * @param entityId Entity identifier to look up.
     * @return True when the Entity exists.
     */
    bool HasCtpEntity(uint32_t entityId) const;

    /**
     * @brief Return whether a local port belongs to an Entity.
     * @param entityId Entity identifier to look up.
     * @param portId Local UB port identifier.
     * @return True when the port is a member.
     */
    bool IsCtpEntityPortMember(uint32_t entityId, uint32_t portId) const;

    /**
     * @brief Return the sorted, unique member ports for an Entity.
     * @param entityId Entity identifier to look up.
     * @return Immutable member-port collection.
     */
    const std::vector<uint16_t>& GetCtpEntityPorts(uint32_t entityId) const;

    /**
     * @brief Select a member port using the packet load-balance field.
     * @param entityId Entity identifier to select from.
     * @param loadBalanceField Packet load-balance field.
     * @return Selected local UB port identifier.
     */
    uint16_t SelectCtpEntityMemberPort(uint32_t entityId, uint8_t loadBalanceField) const;

    /**
     * @brief Create an Entity containing every local UB port.
     * @param entityId Non-zero compact Entity identifier.
     */
    void CreateDefaultCtpEntity(uint32_t entityId);

    /** @brief Make the CTP Entity registry read-only. */
    void FreezeCtpEntities();

    /**
     * @brief Return whether the CTP Entity registry is read-only.
     * @return True after the registry is frozen.
     */
    bool AreCtpEntitiesFrozen() const;
    using CtpPacketTimingCallback = Callback<void, UbCtpPacketTimingTraceData>;
    void SetCtpPacketTimingTraceCallbacks(CtpPacketTimingCallback firstPacketSendsCallback,
                                          CtpPacketTimingCallback lastPacketAcksCallback);

    /**
     * @brief 1.每个controller创建一个Function
     */
    void CreateUbFunction();

    /**
     * @brief 2.每个controller创建一个Transaction
     */
    void CreateUbTransaction();

    void SetTpConnManager(Ptr<TpConnectionManager> conn)
    {
        m_tpConnectionManager = conn;
    }

    Ptr<TpConnectionManager> GetTpConnManager()
    {
        return m_tpConnectionManager;
    }

    uint32_t GetActiveSenderFlowCount() const
    {
        return m_activeSenderFlowCount;
    }

    bool IsDisposing() const
    {
        return m_disposing;
    }

    Time GetInitiatorProtocolDispatchDelay() const
    {
        return m_initiatorProtocolDispatchDelay;
    }

    Time GetInitiatorProtocolCompletionVisibilityDelay() const
    {
        return m_initiatorProtocolCompletionVisibilityDelay;
    }
    void IncrementActiveSenderFlowCount() { ++m_activeSenderFlowCount; }
    void DecrementActiveSenderFlowCount()
    {
        if (m_activeSenderFlowCount > 0)
        {
            --m_activeSenderFlowCount;
        }
    }

  private:
    friend class ::utils::UbUtils;

    void DoDispose() override;
    void FinalizeRtpTpGroupMemberRemoval(Ptr<UbTransportGroup> group, uint32_t index);
    void FinalizeRtpTpGroupDestroy(Ptr<UbTransportGroup> group);
    std::vector<uint16_t> ValidateCtpEntityDefinition(
        uint32_t entityId,
        const std::vector<uint32_t>& memberPorts) const;
    std::vector<uint32_t> CollectDefaultCtpEntityPorts() const;

    Ptr<UbFunction> m_function;       // 功能层
    Ptr<UbTransaction> m_transaction; // 事务层
    Ptr<UbLdstEndpoint> m_ldstEndpoint;
    Ptr<UbCtpTransportService> m_ctpTransportService;
    CtpPacketTimingCallback m_ctpFirstPacketSendsCallback;
    CtpPacketTimingCallback m_ctpLastPacketAcksCallback;
    // Resource storage
    Ptr<UbTransportChannelManager> m_transportChannelManager;
    uint64_t m_nextRtpTpGroupId{0};
    std::map<uint64_t, Ptr<UbTransportGroup>> m_rtpTpGroups{};
    std::map<uint32_t, uint64_t> m_rtpTpGroupByTpn{};
    std::unordered_map<uint32_t, std::vector<Ptr<UbPort>>> m_portsByDestination{};
    std::map<uint32_t, std::vector<uint16_t>> m_ctpEntityPorts{};
    bool m_ctpEntitiesFrozen{false};

    Ptr<TpConnectionManager> m_tpConnectionManager;
    uint32_t m_activeSenderFlowCount{0};
    bool m_disposing{false};
    Time m_initiatorProtocolDispatchDelay{NanoSeconds(0)};
    Time m_initiatorProtocolCompletionVisibilityDelay{NanoSeconds(0)};
};

} // namespace ns3

#endif /* UB_CONTROL_H */
