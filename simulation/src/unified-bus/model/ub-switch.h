// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_SWITCH_H
#define UB_SWITCH_H
#include "ns3/ub-ldst-packet-utils.h"

#include <optional>
#include <unordered_map>
#include <vector>
#include "ns3/nstime.h"
#include "ns3/ub-routing-process.h"
#include "ns3/ub-queue-manager.h"
#include "ns3/node.h"
#include "ns3/traced-callback.h"
#include "ns3/ub-header.h"
#include "ns3/ipv4-header.h"
#include "ns3/udp-header.h"

namespace ns3 {
class UbSwitchCaqm;
class UbPort;
class UbSwitchAllocator;
class UbCongestionControl;
class UbReliableRerouting;
enum class UbRecoverySubmitResult : uint8_t;

enum class FcType {
    CBFC,
    CBFC_SHARED,
    PFC_FIXED,
    PFC_DYNAMIC,
    PFC_DYNAMIC_PAPER,
    NONE  // No flow control
};

typedef enum {
    UB_SWITCH,
    UB_DEVICE
} UbNodeType_t;

typedef enum {
    UB_CONTROL_FRAME = 1,
    UB_URMA_DATA_PACKET,
    UB_LDST_DATA_PACKET,
    UB_CTP_DATA_PACKET,
    UB_LAST_ACK_PACKET,
    UNKOWN_TYPE
} UbPacketType_t;

/**
 * @brief Parsed URMA packet headers (for efficient single-pass parsing)
 */
struct ParsedURMAHeaders {
    UbDatalinkPacketHeader datalinkPacketHeader;
    UbIpBasedNetworkHeader networkHeader;  // Must remove to access inner headers
    Ipv4Header ipv4Header;
    UdpHeader udpHeader;
    UbTransportHeader transportHeader;
};

/**
 * @brief Parsed LDST packet headers (for efficient single-pass parsing)
 */
struct ParsedLdstHeaders {
    UbDatalinkPacketHeader datalinkPacketHeader;
    UbLdstNetworkFields network;
    UbDummyTransactionHeader dummyTransactionHeader;  // Compatible with both Compact and CompactAck
};

/**
 * @brief Parsed CTP packet headers used by switch dispatch/routing.
 */
struct ParsedCtpHeaders {
    UbDatalinkPacketHeader datalinkPacketHeader;
    UbCna16NetworkHeader cna16NetworkHeader;
    UbCna24NetworkHeader cna24NetworkHeader;
    UbCtpHeader ctpHeader;
    UbCompactEidHeader compactEidHeader;
    bool isCna24{false};
};

/**
 * @brief 交换机
 */
class UbSwitch : public Object {
public:
    UbSwitch();
    ~UbSwitch();
    void DoDispose() override;
    static TypeId GetTypeId (void);

    void SwitchHandlePacket(Ptr<UbPort> port, Ptr<Packet> packet);
    // 端口发送完毕，通知交换机出队列
    void NotifySwitchDequeue(uint16_t inPortId, uint32_t outPort, uint32_t priority, Ptr<Packet> p);

    void Init();
    void InitNodePortsFlowControl();
    uint32_t GetVLNum()
    {
        return m_vlNum;
    }
    void SetVLNum(uint32_t vlNum)
    {
        m_vlNum = vlNum;
    }
    UbNodeType_t GetNodeType() {return m_nodeType;}
    void SetNodeType(UbNodeType_t type) {m_nodeType = type;}
    uint32_t GetPortsNum() {return m_portsNum;}
    void SetPortsNum(uint32_t portsNum) {m_portsNum = portsNum;}
    void RegisterTpWithAllocator(Ptr<UbIngressQueue> tp, uint32_t outPort, uint32_t priority);
    void PushPacketToVoq(Ptr<Packet> p, uint32_t outPort, uint32_t priority, uint32_t inPort);
    static bool IsValidVoqIndices(uint32_t outPort, uint32_t priority, uint32_t inPort, uint32_t portsNum, uint32_t vlNum);
    uint64_t GetAllocatedVoqCountForTest() const;
    bool HasVoqForTest(uint32_t outPort, uint32_t priority, uint32_t inPort) const;
    bool TryDestroyVoqForTest(uint32_t outPort, uint32_t priority, uint32_t inPort);
    uint32_t GetPendingInPortProcessingEventCountForTest() const;
    uint32_t GetForcedInPortEventResidueForTest() const { return m_forcedInPortEventResidue; }
    uint64_t GetForcedInPortByteResidueForTest() const { return m_forcedInPortByteResidue; }
    uint64_t GetForcedVoqPacketResidueForTest() const { return m_forcedVoqPacketResidue; }
    uint64_t GetForcedBufferedByteResidueForTest() const { return m_forcedBufferedByteResidue; }
    void RemoveTpFromAllocator(Ptr<UbIngressQueue> tp);
    Ptr<UbSwitchAllocator> GetAllocator();
    Ipv4Address GetNodeIpv4Addr(){return m_Ipv4Addr;}
    Ptr<UbRoutingProcess> GetRoutingProcess() {return m_routingProcess;}
    bool IsCBFCEnable();
    bool IsCBFCSharedEnable();
    bool IsPFCEnable();

    // Internal/runtime override entry. Current user-facing configuration still comes from
    // network_attribute.txt via ConfigStore; these setters exist for tests and future bridge code.
    void SetReservePerQueueBytes(uint32_t bytes);
    void SetSharedPoolBytes(uint64_t bytes);
    void SetHeadroomPerPortBytes(uint32_t bytes);
    void SetDynamicPfcResumeGapBytes(uint32_t bytes);
    void SetDynamicThresholdAlphaShift(uint32_t shift);
    void SetPaperDynamicPfcBeta(uint32_t beta);
    void SetPfcThresholds(int32_t xoffBytes, int32_t xonBytes);
    void SetCbfcCellGeometry(uint8_t flitLenBytes, uint8_t flitsPerCell);
    void SetCbfcReturnCellGrain(uint8_t dataPacketCells, uint8_t controlPacketCells);
    void SetCbfcCredits(int32_t initCreditCells, int32_t sharedInitCreditCells);

    void SetCongestionCtrl(Ptr<UbCongestionControl> congestionCtrl);
    Ptr<UbCongestionControl> GetCongestionCtrl();
    Ptr<UbQueueManager> GetQueueManager();    // Queue Manage Unit
    void SendPacket(Ptr<Packet> p, uint32_t inPort, uint32_t outPort, uint32_t priority);
    void SendLastAck(Ptr<Packet> packet);
    void HandleLastAck(Ptr<UbPort> port, Ptr<Packet> packet);
    void SendControlFrame(Ptr<Packet> packet, uint32_t portId);

    /**
     * @brief Detection completion for a failed output.
     *
     * Excludes the output from route selection, freezes its pending allocator work, and blocks the
     * port from starting new transmissions. Called by the endpoint recovery component at the
     * configured detection time.
     */
    void NotifyOutputDetectedFailed(uint32_t outPort);

    /**
     * @brief Admit one retained recovery packet into a healthy VOQ.
     * @param packet Retained original packet with its old link delivery invalidated.
     * @param inPort Original ingress port identity.
     * @param priority Original VL/priority.
     * @param oldOutput The detected failed output the packet was retained for.
     * @param entryId Local attempt or backlog identity, for evidence only.
     * @return TRANSFERRED after VOQ ownership, or DISCARDED for link-local control frames.
     */
    UbRecoverySubmitResult SubmitRecoveryPacket(Ptr<Packet> packet,
                                                uint32_t inPort,
                                                uint32_t priority,
                                                uint32_t oldOutput,
                                                uint64_t entryId);

    /**
     * @brief Modeled LastAck arrival for a detected failed output.
     *
     * Migrates the switch-owned backlog for that output (VOQ membership and egress-queue
     * ownership) into retained recovery storage and lets the endpoint retry its own attempts.
     */
    void StartOutputRecovery(uint32_t outPort);

    /** True while this switch still owns retained recovery packets for the given output. */
    bool HasPendingRecoveryFor(uint32_t outPort) const;

    /** Output selected by the most recent successful recovery transfer. */
    uint32_t GetLastRecoveryOutput() const { return m_lastRecoveryOutput; }

    /** Retained work currently owned by this switch for one detected failed output. */
    uint64_t GetPendingRecoveryPacketCount(uint32_t outPort) const;
    uint64_t GetPendingRecoveryByteCount(uint32_t outPort) const;

    bool HasDetectedFailedOutput() const { return m_detectedFailedOutput != UINT32_MAX; }
    uint32_t GetDetectedFailedOutput() const { return m_detectedFailedOutput; }
    uint32_t GetRecoveryTransferCountForTest() const { return m_recoveryTransferCount; }
    uint32_t GetRecoveryResolutionCountForTest() const { return m_recoveryResolutionCount; }

    UbPacketType_t GetPacketTypeForTest(Ptr<Packet> packet) { return GetPacketType(packet); }
    RoutingKey GetLdstRoutingKeyForTest(Ptr<Packet> packet);
    RoutingKey GetCtpRoutingKeyForTest(Ptr<Packet> packet);
    uint32_t GetVoqPacketCountForTest(uint32_t outPort, uint32_t priority, uint32_t inPort) const;

protected:
    UbPacketType_t GetPacketType(Ptr<Packet> packet);

private:
    struct BufferOverrideConfig {
        std::optional<uint32_t> reservePerQueueBytes;
        std::optional<uint64_t> sharedPoolBytes;
        std::optional<uint32_t> headroomPerPortBytes;
        std::optional<uint32_t> dynamicPfcResumeGapBytes;
        std::optional<uint32_t> dynamicThresholdAlphaShift;
        std::optional<uint32_t> paperDynamicPfcBeta;
    };

    struct FlowControlOverrideConfig {
        std::optional<int32_t> pfcXoffBytes;
        std::optional<int32_t> pfcXonBytes;
        std::optional<uint8_t> cbfcFlitLenBytes;
        std::optional<uint8_t> cbfcFlitsPerCell;
        std::optional<uint8_t> cbfcReturnGrainDataCells;
        std::optional<uint8_t> cbfcReturnGrainControlCells;
        std::optional<int32_t> cbfcInitCreditCells;
        std::optional<int32_t> cbfcSharedInitCreditCells;
    };

    TracedCallback<uint32_t, UbTransportHeader> m_traceLastPacketTraversesNotify;

    using VirtualOutputQueueGroupKey = uint64_t;

    struct VirtualOutputQueueEntry
    {
        uint32_t inPort;
        Ptr<UbPacketQueue> queue;
    };

    struct VirtualOutputQueueStorage
    {
        std::vector<std::vector<VirtualOutputQueueEntry>> groups;
    };

    struct InPortProcessingEvent
    {
        EventId event;
        uint32_t inPort;
        uint32_t outPort;
        uint32_t priority;
        uint32_t packetBytes;
    };

    struct PendingRecoveryEntry
    {
        Ptr<Packet> packet;
        uint32_t inPort;
        uint32_t priority;
        uint32_t oldOutput;
        uint64_t entryId;
        bool ingressHeld{false};
        bool outputResolved{false};
        uint16_t selectedOutput{UINT16_MAX};
    };

    void LastPacketTraversesNotify(uint32_t nodeId, UbTransportHeader ubTpHeader);

    void VoqInit();
    Ptr<UbPacketQueue> GetOrCreateVoq(uint32_t outPort, uint32_t priority, uint32_t inPort);
    VirtualOutputQueueGroupKey BuildVoqGroupKey(uint32_t outPort, uint32_t priority) const;
    const Ptr<UbPacketQueue>* FindVoqSlot(VirtualOutputQueueGroupKey groupKey,
                                          uint32_t inPort) const;
    Ptr<UbPacketQueue>* GetOrCreateVoqSlot(VirtualOutputQueueGroupKey groupKey,
                                           uint32_t inPort);
    bool TryDestroyEmptyVoq(uint32_t outPort, uint32_t priority, uint32_t inPort);
    bool HasPendingInPortWork(uint32_t outPort, uint32_t priority, uint32_t inPort) const;
    void PruneResolvedInPortEvents();
    void ReceivePacket(Ptr<UbPort> port, Ptr<Packet> p);

    void HandleURMADataPacket(Ptr<UbPort> port, Ptr<Packet> packet);
    void HandleLdstDataPacket(Ptr<UbPort> port, Ptr<Packet> packet);
    void HandleCtpDataPacket(Ptr<UbPort> port, Ptr<Packet> packet);
    bool SinkTpDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedURMAHeaders &headers);
    bool SinkLdstDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedLdstHeaders &headers);
    bool SinkCtpDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedCtpHeaders &headers);
    void ParseURMAPacketHeader(Ptr<Packet> packet, ParsedURMAHeaders &headers);
    void ParseLdstPacketHeader(Ptr<Packet> packet, ParsedLdstHeaders &headers);
    void ParseCtpPacketHeader(Ptr<Packet> packet, ParsedCtpHeaders &headers);
    void GetURMARoutingKey(const ParsedURMAHeaders &headers, RoutingKey &rtKey);
    void GetLdstRoutingKey(const ParsedLdstHeaders &headers, RoutingKey &rtKey);
    void GetCtpRoutingKey(const ParsedCtpHeaders &headers, RoutingKey &rtKey);
    void ForwardDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedURMAHeaders &headers);
    void ForwardDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedLdstHeaders &headers);
    void ForwardDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedCtpHeaders &headers);
    void ForceShortestPathRouting(Ptr<Packet> packet, const UbDatalinkPacketHeader &parsedHeader);
    void ForwardDataPacketAfterAdmission(Ptr<Packet> packet,
                                         uint32_t inPort,
                                         uint32_t outPort,
                                         uint32_t priority);
    void CompleteInPortProcessing(Ptr<Packet> packet,
                                  uint32_t inPort,
                                  uint32_t outPort,
                                  uint32_t priority);
    void MoveInPortProcessingToVoq(Ptr<Packet> packet,
                                   uint32_t inPort,
                                   uint32_t outPort,
                                   uint32_t priority);
    void FinalizeForwardedPacketEnqueue(Ptr<Packet> packet,
                                        uint32_t inPort,
                                        uint32_t outPort,
                                        uint32_t priority,
                                        bool recoveryTransfer = false);
    void AbortNoRecoveryRoute(const RoutingKey& key,
                              uint32_t inPort,
                              uint32_t outPort,
                              const std::string& packetFamily) const;
    void PublishRecoveryBacklogCounters(uint32_t outPort);
    void ReportForwardingRouteFailure(const RoutingKey& key, uint32_t inPort);
    bool ResolveRecoveryOutput(Ptr<Packet> packet,
                               uint32_t inPort,
                               uint32_t oldOutput,
                               uint16_t& newOutput);
    void ParkRecoveryPacket(Ptr<Packet> packet,
                            uint32_t inPort,
                            uint32_t priority,
                            bool ingressHeld);
    void TryTransferPendingRecovery();
    void ClearRecoveryBacklogForOutput(uint32_t outPort);
    void TransferOnePendingEntry(PendingRecoveryEntry& entry);
    void ClearOldLinkPiggybackMetadata(Ptr<Packet> packet);
    Ptr<UbReliableRerouting> FailedOutputRerouting();
    int ResolveURMAOutput(Ptr<Packet> packet,
                          const ParsedURMAHeaders& headers,
                          uint32_t inPort,
                          RoutingKey& key,
                          bool& selectedShortestPath);
    int ResolveLdstOutput(Ptr<Packet> packet,
                          const ParsedLdstHeaders& headers,
                          uint32_t inPort,
                          RoutingKey& key,
                          bool& selectedShortestPath);
    int ResolveCtpOutput(Ptr<Packet> packet,
                         const ParsedCtpHeaders& headers,
                         uint32_t inPort,
                         RoutingKey& key,
                         bool& selectedShortestPath);
    void InitAllocator(Ptr<Node> node);
    void InitQueueManager(Ptr<Node> node);
    void InitRoutingProcess(Ptr<Node> node);
    void ApplyLocalQueueManagerConfig();
    void ApplyLocalPortFlowControlConfig(Ptr<UbPort> port);

    Ptr<UbQueueManager> m_queueManager;   // Memory Management Unit
    Ptr<UbCongestionControl> m_congestionCtrl;
    UbNodeType_t m_nodeType;
    uint32_t m_portsNum = 1025;
    Ptr<UbSwitchAllocator> m_allocator;
    uint32_t m_vlNum = 16;
    Time m_inPortProcessingDelay {NanoSeconds(0)};
    std::vector<InPortProcessingEvent> m_inPortProcessingEvents;
    uint32_t m_forcedInPortEventResidue = 0;
    uint64_t m_forcedInPortByteResidue = 0;
    uint64_t m_forcedVoqPacketResidue = 0;
    uint64_t m_forcedBufferedByteResidue = 0;
    VirtualOutputQueueStorage m_voq; // on-demand VOQ groups keyed by outPort/priority
    Ptr<UbRoutingProcess> m_routingProcess;   // Router Model

    uint32_t m_detectedFailedOutput{UINT32_MAX};
    uint32_t m_lastRecoveryOutput{UINT32_MAX};
    uint32_t m_recoveryTransferCount{0};
    uint32_t m_recoveryResolutionCount{0};

    // Retained work that the switch itself owns during recovery (never a live transmission).
    std::vector<PendingRecoveryEntry> m_pendingRecovery;
    uint64_t m_pendingRecoveryPackets{0};
    uint64_t m_pendingRecoveryBytes{0};
    uint64_t m_pendingFailedProcessing{0};
    bool m_recoveryBacklogMigrated{false};
    bool m_recoveryDisposed{false};
    uint64_t m_nextRecoveryEntryId{0};

    Ipv4Address m_Ipv4Addr;
    bool m_isECNEnable;
    FcType m_flowControlType { FcType::NONE };
    BufferOverrideConfig m_bufferOverrides;
    FlowControlOverrideConfig m_flowControlOverrides;
    enum VlScheduler {
        SP = 0,
        DWRR = 1
    };
    VlScheduler m_vlScheduler {SP};
};

} // namespace ns3

#endif /* UB_SWITCH_H */
