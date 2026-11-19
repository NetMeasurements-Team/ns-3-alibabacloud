// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TP_CONNECTION_MANAGER_H
#define UB_TP_CONNECTION_MANAGER_H

#include "ub-lifecycle.h"
#include "ub-network-address.h"

#include "ns3/log.h"
#include "ns3/random-variable-stream.h"

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace ns3
{
class UbController;
class UbTpnAllocationBoundaryAccessor;
class UbTransportChannelManager;
}

namespace utils {
/**
 * @brief tp-config.csv 中定义的数据结构
 */
struct Connection {
    uint32_t ownerNodeId{UINT32_MAX};
    uint32_t node1;
    uint32_t port1;
    uint32_t tpn1;
    uint32_t node2;
    uint32_t port2;
    uint32_t tpn2;
    uint32_t priority;
    uint32_t metrics;
    uint64_t reservationGeneration{0};
};

enum class EndpointSlotState : uint8_t
{
    UNMATERIALIZED,
    ACTIVE,
    DRAINING,
    RETIRED
};

struct ConnectionLifecycleSnapshot
{
    uint32_t ownerNodeId{UINT32_MAX};
    uint64_t generation{0};
    uint32_t node1{0};
    uint32_t tpn1{0};
    uint32_t node2{0};
    uint32_t tpn2{0};
    EndpointSlotState slot1{EndpointSlotState::UNMATERIALIZED};
    EndpointSlotState slot2{EndpointSlotState::UNMATERIALIZED};
    bool destroyRequested{false};
    UbDestroyReason reason{UbDestroyReason::EXPLICIT};
};

struct ConnectionStorageSnapshot
{
    uint64_t canonicalRecords{0};
    uint64_t authoritativeLocalViews{0};
    uint64_t secondaryIndexEntries{0};
    uint64_t unmaterializedReservationRecords{0};
    uint64_t unmaterializedEndpointSlots{0};
    uint64_t activeLifecycleSidecars{0};
    uint64_t destroyCallbackBundles{0};
    uint64_t destroyCallbackEntries{0};
};

/**
 * @brief 获取TPN规制的枚举类
 */
enum class GetTpnRuleT {
    BY_PEERNODE = 0,
    BY_PEERNODE_PRIORITY,
    BY_PEERNODE_LOCALPORT,
    BY_PEERNODE_BOTHPORTS,
    BY_ALL,
    OTHER
};

/**
 * @brief 连接索引管理器，管理所有连接
 */
class TpConnectionManager : public Object {
public:
    static TypeId GetTypeId();

    TpConnectionManager();
    ~TpConnectionManager();
    // 添加连接（不需要nodeId参数）
    void AddConnection(const Connection& conn);

    // 添加单边连接
    void AddUnilateralConnection(const Connection& conn, const uint32_t src);

    // 获取指定节点的连接管理器视图
    Ptr<TpConnectionManager> GetConnectionManagerByNode(uint32_t nodeId);

    /**
     * @brief Select or create transport connections for a traffic record.
     * @param ruler Connection selection rule used when no endpoint-port constraint is present.
     * @param useShortestPath Whether newly created connections must use shortest routes.
     * @param useMultiPath Whether multiple transport connections may be returned.
     * @param localNodeId Local endpoint node identifier.
     * @param peerNodeId Peer endpoint node identifier.
     * @param localPort Optional local endpoint port; UINT32_MAX means wildcard.
     * @param peerPort Optional peer endpoint port; UINT32_MAX means wildcard.
     * @param priority Traffic priority used for connection matching and creation.
     */
    std::vector<uint32_t> GetTpns(GetTpnRuleT ruler,
                                  bool useShortestPath,
                                  bool useMultiPath,
                                  uint32_t localNodeId,
                                  uint32_t peerNodeId,
                                  uint32_t localPort = UINT32_MAX,
                                  uint32_t peerPort = UINT32_MAX,
                                  uint32_t priority = UINT32_MAX);

    /**
     * @brief Reserve connection records without materializing either TP endpoint.
     * @param useShortestPath Whether reserved paths must use shortest routes.
     * @param localNodeId Local node identifier.
     * @param peerNodeId Peer node identifier.
     * @param priority Traffic priority associated with the reservation.
     * @param localPort Optional local endpoint port; UINT32_MAX means wildcard.
     * @param peerPort Optional peer endpoint port; UINT32_MAX means wildcard.
     */
    void ReserveTpnsForTraffic(bool useShortestPath,
                               uint32_t localNodeId,
                               uint32_t peerNodeId,
                               uint32_t priority,
                               uint32_t localPort = UINT32_MAX,
                               uint32_t peerPort = UINT32_MAX);

    /**
     * @brief Reserve connections owned by the standard driver's planned-destroy policy.
     * @param useShortestPath Whether reserved paths must use shortest routes.
     * @param localNodeId Local node identifier and lifecycle owner of the returned identities.
     * @param peerNodeId Peer node identifier.
     * @param priority Traffic priority associated with the reservation.
     * @param localPort Optional local endpoint port; UINT32_MAX means wildcard.
     * @param peerPort Optional peer endpoint port; UINT32_MAX means wildcard.
     * @return Identities created by this call. Returns empty when a matching reservation already
     *         exists; existing reservations are never claimed.
     */
    std::vector<UbLifecycleResourceIdentity> ReservePlannedTpnsForTraffic(
        bool useShortestPath,
        uint32_t localNodeId,
        uint32_t peerNodeId,
        uint32_t priority,
        uint32_t localPort = UINT32_MAX,
        uint32_t peerPort = UINT32_MAX);

    // 1. 获取节点相关的所有连接
    std::vector<Connection> GetNodeConnections(uint32_t nodeId) const;

    // 2. 通过对端nodeId找到所有可用tpn
    std::vector<std::pair<uint32_t, uint32_t>> GetTpnsByPeerNode(uint32_t localNodeId, uint32_t peerNodeId) const;

    // 3. 通过对端nodeId+priority找到所有可用tpn
    std::vector<std::pair<uint32_t, uint32_t>> GetTpnsByPeerNodePriority(uint32_t localNodeId,
                                                    uint32_t peerNodeId,
                                                    uint32_t priority) const;

    // 4. 通过对端nodeId+本端port找到所有可用tpn
    std::vector<std::pair<uint32_t, uint32_t>> GetTpnsByPeerNodeLocalPort(uint32_t localNodeId,
                                                     uint32_t peerNodeId,
                                                     uint32_t localPort) const;

    // 5. 通过对端nodeId+本端port+对端port找到所有可用tpn
    std::vector<std::pair<uint32_t, uint32_t>> GetTpnsByPeerNodeBothPorts(uint32_t localNodeId,
                                                     uint32_t peerNodeId,
                                                     uint32_t localPort,
                                                     uint32_t peerPort) const;

    // 组合查询：通过完整条件找到所有可用tpn
    std::vector<std::pair<uint32_t, uint32_t>> GetTpnsByFullCriteria(uint32_t localNodeId,
                                               uint32_t peerNodeId,
                                               uint32_t localPort,
                                               uint32_t peerPort,
                                               uint32_t priority) const;

    /**
     * @brief Query local connections using endpoint nodes, optional ports, and priority.
     * @param localNodeId Local endpoint node identifier.
     * @param peerNodeId Peer endpoint node identifier.
     * @param localPort Optional local endpoint port; UINT32_MAX means wildcard.
     * @param peerPort Optional peer endpoint port; UINT32_MAX means wildcard.
     * @param priority Traffic priority to match.
     * @return Matching local TPN and connection-metric pairs.
     */
    std::vector<std::pair<uint32_t, uint32_t>>
    GetTpnsByTrafficCriteria(uint32_t localNodeId,
                             uint32_t peerNodeId,
                             uint32_t localPort,
                             uint32_t peerPort,
                             uint32_t priority) const;

    // 获取某个节点的所有TPN
    std::vector<uint32_t> GetAllTpnsForNode(uint32_t nodeId) const;

    // 获取节点的邻居节点
    std::set<uint32_t> GetNeighborNodes(uint32_t nodeId) const;

    // 获取所有连接
    const std::vector<Connection>& GetAllConnections() const;

    // 获取连接总数
    size_t GetConnectionCount() const;

    // 获取与该tp相关的conn
    Connection GetConnection(uint32_t tpn, uint32_t src, uint32_t dst, uint32_t priority);

    // 获取与该tp相关的conn，所有优先级
    Connection GetConnection(uint32_t tpn, uint32_t src, uint32_t dst);

    /**
     * @brief Find a reservation by the local node and local TPN.
     * @param localNodeId Local node identifier.
     * @param localTpn Locally assigned transport path number.
     * @param connection Output connection arranged from the local node's perspective.
     * @return True when a matching reservation exists.
     */
    bool TryGetLocalConnection(uint32_t localNodeId,
                               uint32_t localTpn,
                               Connection& connection) const;

    /** Notify the canonical owner that one reserved endpoint has materialized. */
    void NotifyEndpointMaterialized(uint32_t localNodeId,
                                    uint32_t localTpn,
                                    bool draining = false);
    bool IsConnectionDestroyRequested(uint32_t localNodeId, uint32_t localTpn) const;
    void DestroyDrainOnlyEndpoint(uint32_t localNodeId, uint32_t localTpn);

    UbDestroyStatus DestroyConnection(uint32_t localNodeId,
                                      uint32_t localTpn,
                                      UbDestroyReason reason = UbDestroyReason::EXPLICIT,
                                      UbDestroyCallback callback = {});

    /**
     * @brief Explicitly destroy the connection identified by an immutable lifecycle identity.
     * @param identity Exact RTP connection identity returned by a reservation manager.
     * @param reason Network-semantic reason for the explicit destroy request.
     * @param callback Callback invoked after destroy completes or fails asynchronously.
     * @return REJECTED when the kind or local ID is invalid, or when an in-process owner rejects
     *         the identity; otherwise the current explicit-destroy status.
     */
    UbDestroyStatus DestroyConnection(const UbLifecycleResourceIdentity& identity,
                                      UbDestroyReason reason = UbDestroyReason::EXPLICIT,
                                      UbDestroyCallback callback = {});

    static bool ApplyLifecycleControl(const UbLifecycleControlRecord& record);

    bool GetConnectionLifecycleSnapshot(uint32_t localTpn,
                                        ConnectionLifecycleSnapshot& snapshot) const;
    size_t GetCanonicalConnectionCount() const;
    size_t GetCanonicalConnectionCountForTest() const
    {
        return GetCanonicalConnectionCount();
    }

    ConnectionStorageSnapshot GetConnectionStorageSnapshot() const;

    // 清空指定节点的连接
    void ClearNodeConnections(uint32_t nodeId);

    // 清空所有连接
    void Clear();

    uint32_t GetNextTpn();
private:
  friend class ns3::UbController;
  friend class ns3::UbTpnAllocationBoundaryAccessor;
  friend class ns3::UbTransportChannelManager;

  static std::mutex g_tpConnectionControlLock;

    uint32_t ReserveNextTpnLocked();

    struct DestroyCallbackBundle
    {
        std::vector<UbDestroyCallback> callbacks;
        std::vector<std::pair<uint32_t, uint64_t>> remoteCallbacks;
    };

    struct ConnectionLifecycleSidecar
    {
        EndpointSlotState slot1{EndpointSlotState::UNMATERIALIZED};
        EndpointSlotState slot2{EndpointSlotState::UNMATERIALIZED};
        bool destroyRequested{false};
        bool slot1Materialized{false};
        bool slot2Materialized{false};
        bool finalizing{false};
        UbDestroyReason reason{UbDestroyReason::EXPLICIT};
        uint8_t erasedViews{0};
        std::unique_ptr<DestroyCallbackBundle> callbackBundle;
    };

    struct ConnectionLifecycleRecord
    {
        Connection connection;
        UbLifecycleResourceIdentity identity;
        std::unique_ptr<ConnectionLifecycleSidecar> lifecycle;
    };

    void RegisterCanonicalConnectionLocked(Connection& conn);
    void AttachConnectionIdentityLocked(Connection& conn);
    static UbLifecycleResourceIdentity GetConnectionIdentity(const Connection& connection);
    bool MatchesCanonicalConnection(const UbLifecycleResourceIdentity& identity) const;
    bool NotifyEndpointMaterializedLocked(uint32_t localNodeId,
                                          uint32_t localTpn,
                                          EndpointSlotState state);
    UbDestroyStatus DestroyConnectionOwned(const UbLifecycleResourceIdentity& identity,
                                           UbDestroyReason reason,
                                           UbDestroyCallback callback,
                                           uint32_t callbackNodeId = UINT32_MAX,
                                           uint64_t requestId = 0);
    UbDestroyStatus DestroyConnectionLocked(
        uint32_t localNodeId,
        uint32_t localTpn,
        const UbLifecycleResourceIdentity* expectedIdentity,
        UbDestroyReason reason,
        UbDestroyCallback callback);
    void RequestDestroyNotice(const Connection& connection,
                              const UbLifecycleResourceIdentity& identity,
                              uint8_t slot,
                              UbDestroyReason reason);
    void RequestFreezeNotice(const Connection& connection,
                             const UbLifecycleResourceIdentity& identity,
                             uint8_t slot,
                             UbDestroyReason reason);
    void OnLocalEndpointDestroyed(UbLifecycleResourceIdentity connectionIdentity,
                                  uint8_t slot,
                                  UbLifecycleResourceIdentity endpointIdentity,
                                  UbDestroyStatus status);
    void OnEndpointDestroyed(const UbLifecycleResourceIdentity& identity,
                             uint8_t slot,
                             UbDestroyStatus status);
    void TryFinalizeConnection(const UbLifecycleResourceIdentity& identity);
    void RequestEraseConnectionView(const Connection& connection,
                                    const UbLifecycleResourceIdentity& identity,
                                    uint8_t slot);
    void OnConnectionViewErased(const UbLifecycleResourceIdentity& identity, uint8_t slot);
    void CompleteFinalizeConnection(const UbLifecycleResourceIdentity& identity);
    void CompleteRemoteDestroy(const UbLifecycleResourceIdentity& identity,
                               uint64_t requestId,
                               UbDestroyStatus status);
    void EraseConnectionView(uint32_t localNodeId, uint32_t localTpn);

    struct LocalConnectionRef
    {
        uint32_t localNodeId{UINT32_MAX};
        uint32_t localTpn{UINT32_MAX};
        uint64_t reservationGeneration{0};

        bool operator==(const LocalConnectionRef& other) const
        {
            return localNodeId == other.localNodeId && localTpn == other.localTpn &&
                   reservationGeneration == other.reservationGeneration;
        }
        bool operator!=(const LocalConnectionRef& other) const { return !(*this == other); }
        bool operator<(const LocalConnectionRef& other) const
        {
            return std::tie(localNodeId, localTpn, reservationGeneration) <
                   std::tie(other.localNodeId, other.localTpn, other.reservationGeneration);
        }
    };

    struct ConnectionOrderRef
    {
        LocalConnectionRef localView;
        bool reverseToOriginalOrientation{false};
    };

    const Connection* ResolveConnectionRefLocked(const LocalConnectionRef& ref) const;
    enum class InboundConnectionState : uint8_t
    {
        LIVE,
        RETIRED_EXACT,
        INVALID,
    };
    InboundConnectionState ResolveInboundConnection(uint32_t localNodeId,
                                                    uint32_t srcTpn,
                                                    uint32_t dstTpn,
                                                    Ipv4Address sourceIp,
                                                    Ipv4Address destinationIp,
                                                    uint32_t priority,
                                                    Connection& connection) const;
    void InvalidateAllConnectionsCacheLocked();

    // 为指定节点建立各种索引
    LocalConnectionRef BuildIndexesForNode(uint32_t localNodeId, Connection conn);

    // 从索引中清理指定节点
    void ClearNodeFromIndexes(uint32_t nodeId);

    // Reserve new TP connection records and return their local TPN/metric pairs.
    std::vector<std::pair<uint32_t, uint32_t>> ReserveNewTpConnections(uint32_t src,
                                                                       uint32_t dst,
                                                                       uint32_t priority,
                                                                       bool useShortestPath,
                                                                       uint32_t localPort,
                                                                       uint32_t peerPort);

    // 重建tp对
    std::vector<uint32_t> ReconstructTPs(std::vector<uint32_t> tpns,
                                         uint32_t src,
                                         uint32_t dst,
                                         uint32_t priority,
                                         bool useShortestPath,
                                         bool useMultiPath);

    uint32_t ReconstructTp(Connection conn);

  private:
    // 每个本地端点视图仅保留一个完整的定向连接值。
    std::map<LocalConnectionRef, Connection> m_localConnectionViews;

    // 公共列表顺序及其按需兼容缓存。
    std::vector<ConnectionOrderRef> m_allConnectionOrder;
    mutable std::vector<Connection> m_allConnectionsCache;
    mutable bool m_allConnectionsCacheValid{false};

    // 每个节点的相关连接
    std::unordered_map<uint32_t, std::vector<LocalConnectionRef>> m_nodeConnections;

    // 索引2: (localNodeId, peerNodeId) -> connections
    std::map<std::pair<uint32_t, uint32_t>, std::vector<LocalConnectionRef>> m_peerNodeIndex;

    // 索引3: (localNodeId, peerNodeId, priority) -> connections
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::vector<LocalConnectionRef>>
        m_peerNodePriorityIndex;

    // 索引4: (localNodeId, peerNodeId, localPort) -> connections
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::vector<LocalConnectionRef>>
        m_peerNodeLocalPortIndex;

    // 索引5: (localNodeId, peerNodeId, localPort, peerPort) -> connections
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, std::vector<LocalConnectionRef>>
        m_bothPortsIndex;

    // 索引6: (localNodeId, localTpn) -> generation-aware local view reference
    std::map<std::pair<uint32_t, uint32_t>, LocalConnectionRef> m_localTpnIndex;
    std::map<std::pair<uint32_t, uint32_t>, Connection> m_retiredLocalConnectionViews;
    std::set<std::pair<uint32_t, uint32_t>> m_destroyRequestedViews;

    std::set<uint32_t> m_allocatedTpns;
    std::set<uint32_t> m_reservedTpns;

    uint32_t m_nextTpn = 0;
    bool m_tpnSpaceExhausted{false};

    Ptr<UniformRandomVariable> m_random;


    mutable std::mutex m_stateLock;
    uint32_t m_ownerNodeId{UINT32_MAX};
    std::map<std::pair<uint32_t, uint32_t>, ConnectionLifecycleRecord>
        m_canonicalConnections;
    struct PendingRemoteDestroy
    {
        UbLifecycleResourceIdentity identity;
        UbDestroyCallback callback;
    };
    uint64_t m_nextRemoteDestroyRequestId{0};
    std::map<uint64_t, PendingRemoteDestroy> m_pendingRemoteDestroyCallbacks;

  public:
    void SetOwnerNodeId(uint32_t nodeId) { m_ownerNodeId = nodeId; }
};

} // namespace utils

#endif
