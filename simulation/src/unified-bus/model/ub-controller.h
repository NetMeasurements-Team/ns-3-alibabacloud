// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_CONTROL_H
#define UB_CONTROL_H

#include <ns3/core-module.h>
#include <vector>
#include <map>
#include <unordered_map>
#include "ub-datatype.h"
#include "ns3/ub-switch-allocator.h"
#include "ns3/ub-function.h"
#include "ns3/ub-datalink.h"
#include "ns3/ub-tp-connection-manager.h"


using namespace utils;
namespace ns3 {

class UbLdstApi;
class UbFunction;
class UbTransaction;
class UbTransportChannel;
class UbTransportGroup;
class UbDataLink;
class UbPort;

/**
 * @brief UB Management Unit
 */
class UbController : public Object {
public:
    static TypeId GetTypeId();

    UbController();
    ~UbController();

    // Transport channel management
    /**
     * @brief Create a new transport channel
     * @param src Source identifier
     * @param dest Destination identifier
     * @param sport Source port
     * @param dport Destination port
     * @param tpn Transport identifier
     * @return Pointer to created transport channel
     */
    // bool CreateTp(uint32_t src, uint32_t dest, uint8_t sport, uint8_t dport, UbPriority priority,
    //               uint32_t srcTpn, uint32_t dstTpn, Ptr<UbCongestionControl> congestionCtrl);
    bool CreateTp(const Connection& conn, Ptr<UbCongestionControl> congestionCtrl);

    /**
     * @brief Get tp by tpn
     * @param tpn tpn number
     **/
    Ptr<UbTransportChannel> GetTp(uint32_t tpn);

    /**
     * @brief destroy tp instance
     * @param tpn tpn number
     **/
    void DestroyTp(uint32_t tpn);

    // Transport channel Group management
    /**
     * @brief Create a new transport channel group
     * @param groupId Group identifier
     * @return Pointer to created transport channel group
     */
    Ptr<UbTransportGroup> CreateTpGroup(uint32_t src, uint32_t dest, uint32_t type, uint32_t priority, uint32_t tpgn);

    /**
     * @brief Get transport channel group by group ID
     * @param groupId Group identifier
     * @return Pointer to transport channel group or nullptr if not found
     */
    Ptr<UbTransportGroup> GetTpGroup(TpgTag tpgTag);

    std::vector<Ptr<UbTransportGroup>> GetTpGroup(uint64_t src, uint64_t dest, uint64_t priority);
    /**
     * @brief Generate transport channel group tag from parameters
     * @param src Source identifier
     * @param dest Destination identifier
     * @param sport Source port
     * @param dport Destination port
     * @param groupId Group identifier
     * @return Generated transport channel group tag
     */
    TpgTag GenTpGroupTag(uint32_t src, uint32_t dest, uint32_t type, uint32_t priority, uint32_t tpgn);

    /**
     * @brief Destroy transport channel group
     * @param groupId Group identifier
     */
    void DestroyTpGroup(uint32_t src, uint32_t dest, uint32_t type, uint32_t priority, uint32_t tpgn);

    /**
     * @brief Destroy transport channel group by tag
     * @param tpgTag transport channel group tag
     */
    void DestroyTpGroup(TpgTag tpgTag);

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
     * @brief 1.每个controller创建一个Function
     */
    void CreateUbFunction();

    /**
     * @brief 2.每个controller创建一个Transaction
     */
    void CreateUbTransaction();

    std::unordered_map<uint32_t, Ptr<UbTransportChannel>> m_tpsMapInIngressSource; // 统一存到voq队列中，tps的位置

    bool AddTpMapping(uint32_t key, Ptr<UbTransportChannel> tp);
    Ptr<UbTransportChannel> GetTpByMap(uint32_t key);       // 通过调度计算得到的inport获取对应的TP

    /**
     * @brief Get transport channel by TPN
     * @param tpn Transport number
     * @return Pointer to transport channel or nullptr if not found
     */
    Ptr<UbTransportChannel> GetTpByTpn(uint32_t tpn);

    std::map<uint32_t, Ptr<UbTransportChannel>> GetTpnMap() const;

    void SetTpConnManager(Ptr<TpConnectionManager> conn)
    {
        m_tpnConn = conn;
    }
    Ptr<TpConnectionManager> GetTpConnManager() { return m_tpnConn; }

    /**
     * @brief 判断TP实例是否存在
     */
    bool IsTPExists(uint32_t tpn);

    uint32_t GetActiveSenderFlowCount() const { return m_activeSenderFlowCount; }
    void IncrementActiveSenderFlowCount() { ++m_activeSenderFlowCount; }
    void DecrementActiveSenderFlowCount()
    {
        if (m_activeSenderFlowCount > 0)
        {
            --m_activeSenderFlowCount;
        }
    }
private:
    Ptr<UbFunction> m_function; // 功能层
    Ptr<UbTransaction> m_transaction; // 事务层
    // Resource storage
    std::map<uint32_t, Ptr<UbTransportChannel>> m_numToTp{};   // 协议规定TPH中的TP number，在生成TP时，赋值为m_transports_count
    uint32_t m_transportsCount{0}; // increment only，用于标识tpn
    std::map<TpgTag, Ptr<UbTransportGroup>> m_tpGroups{};
    std::map<uint64_t, Ptr<UbPort>> m_ports{};
    Ptr<UbDataLink> m_datalink = nullptr;

    std::unordered_map<uint32_t, std::vector<Ptr<UbPort>>> m_destinationToPortsMap{};
    std::unordered_map<uint32_t, std::vector<std::pair<uint8_t, uint8_t>>> m_destinationToPortPairMap{};
    // src:0 dst:1
    // ub-config 0->1: p0-p0 p1-p1
    std::map<std::vector<std::pair<uint8_t, uint8_t>>, uint8_t> m_portPairsToIter{};
    std::vector<std::vector<std::vector<uint32_t>>> m_dstPriToTp{}; // level_0 dst_node, level_1 priority, level_2 tpns
    std::vector<std::vector<uint8_t>> m_dstPriToTpRrIndex{}; // level_0 dst_node, level_1 priority, level_2 iteration

    Ptr<TpConnectionManager> m_tpnConn; // 当前节点维护的tpnConn
    uint32_t m_activeSenderFlowCount{0};

};

} // namespace ns3

#endif /* UB_CONTROL_H */
