// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_PORT_H
#define UB_PORT_H

#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include "ns3/ipv4-header.h"
#include "ns3/ipv4.h"
#include "ns3/udp-header.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/ub-transport.h"
#include "ns3/ub-link.h"
#include "ns3/packet.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/enum.h"
#include "ns3/ub-datalink.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-flow-control.h"
#include "ns3/ub-reliable-rerouting.h"
#include "ns3/ub-small-fifo-queue.h"

#define TIME_TO_LIVE   64


namespace ns3 {
class UbTransportChannel;
class UbEgressQueue;
class UbSwitch;
class UbDataLink;
class UbIngressQueue;
class UbLink;
class UbFlowControl;
class UbCbfc;
class UbCbfcSharedCredit;
class UbPfc;

// Egress queue enqueue item: (inPortId, priority, packet)
using PacketEntry = std::tuple<uint32_t, uint32_t, Ptr<Packet>>;

/**
 * \class UbEgressQueue
 * \brief Port Egress Queue Management (物理输出队列，用于传输调度)
 */
class UbEgressQueue : public Object {
public:
    UbSmallFifoQueue<PacketEntry, 4> m_egressQ; // 通过算法分配到的包, inPortId, priority, packet

    uint64_t m_maxEgressBytes;     // Max bytes accepted by egress queue
    uint64_t m_currentBytes = 0;   // Current bytes in egress queue (用于拥塞控制等)
    uint32_t m_ownerNodeId{Simulator::NO_CONTEXT};

    static TypeId GetTypeId(void);
    explicit UbEgressQueue();
    void SetOwnerNodeId(uint32_t nodeId) { m_ownerNodeId = nodeId; }

    bool DoEnqueue(PacketEntry packetEntry);  // 向端口eq塞入包
    bool CanEnqueue(uint32_t packetBytes) const;
    PacketEntry Peekqueue(void);
    PacketEntry DoDequeue(void);
    // 为报文添加UDP、IPV4、DL packet头
    void AddPacketHeader(Ptr<UbTransportChannel> tp, Ptr<Packet> p, bool credit, bool ack);

    bool IsEmpty();

    /**
     * @brief 获取EgressQueue当前字节占用（用于拥塞控制等）
     */
    uint64_t GetCurrentBytes() const { return m_currentBytes; }

    TracedCallback<Ptr<const Packet>, uint32_t> m_traceUbEnqueue;
    TracedCallback<Ptr<const Packet>, uint32_t> m_traceUbDequeue;
};

/**
 * @brief Port transmission state machine
 */
enum class SendState {
    READY,  // Port is idle and ready to transmit next packet
    BUSY    // Port is actively transmitting a packet
};

/**
 * @brief Packet rate control mode (UB Base Specification 2.1, §4.7).
 *
 * The sender fills No_Operation Blocks (NOBs) after small DLLDPs so that the
 * interval between consecutive DLLDPs meets PACKET_MIN_INTERVAL, lowering the
 * packet reception rate required of the peer receiver.
 *
 * \note Specification basis. Quoted from UB Base Specification 2.1.0 §4.7.1,
 * "Packet Rate Control Modes":
 *
 *   "The data link layer supports a packet rate control mechanism, which
 *    controls the packet transmission rate of DLLDPs by inserting a specified
 *    number of NOBs between DLLDPs at the sender. This reduces the requirement
 *    on the receiver's packet reception rate capability. Due to the limited
 *    small-packet processing rate of the receiver, the sender must reduce the
 *    transmission rate of small packets; otherwise, traffic surges may occur,
 *    rendering the receiver unable to process them."
 *
 *   "The data link layer supports two modes: static packet rate control and
 *    dynamic packet rate control. When the sender operates in static packet
 *    rate control mode, small packets refer to DLLDPs whose length is less than
 *    PACKET_MIN_INTERVAL. When the sender operates in dynamic packet rate
 *    control mode, small packets refer to DLLDPs whose length is less than
 *    AVERAGE_PACKET_LENGTH."
 *
 *   "The packet control mode can be configured independently on both sides of
 *    the link. The local end advertises its packet rate control mode to the
 *    peer end via the PACKET_RATE_CONTROL_MODE field of the Init Block during
 *    the DLL_Param_Init state."
 *
 * The mode is therefore what the peer receiver advertised, not a local choice
 * of this port; the parameters it implies travel in the same Init Block.
 *
 * \note NONE is a simulator-local switch. Table 4-9 defines
 * PACKET_RATE_CONTROL_MODE as a single bit (1'b0 = static, 1'b1 = dynamic) and
 * provides no "disabled" value; NONE exists so existing cases keep their current
 * behavior unless the feature is explicitly enabled.
 *
 * \note Negotiation is not modelled. §4.7.1 and §4.7.2 fix the parameters in
 * DLL_Param_Init and state that they "cannot be dynamically changed thereafter";
 * Table 4-9 adds that "It is not allowed to send only part of the initialization
 * parameters." This port takes all of them as static TypeId attributes that may
 * be changed at any time, and does not model the Init Block exchange. Every
 * parameter describes the peer receiver and constrains this port's egress
 * direction only; the two link directions are configured independently.
 *
 * \note Model limits. A padded DLLDP only keeps this port occupied for longer:
 * the NOBs are not written to the retry buffer, are not counted in the packet
 * byte counters, and do not delay the padded DLLDP's own arrival at the peer.
 * UbInterframeGap is a simulator-level idle time rather than a specification
 * parameter, and the two modes treat it differently. STATIC counts it towards
 * PACKET_INTERVAL: §4.7.3.1 rule 2 defines that interval as "the interval
 * between the SOPs of the previous DLLDP and the next DLLDP to be sent", so
 * any on-wire idle is part of it. DYNAMIC adds it on top of the NOB_NUM the
 * §4.7.3.2 rule 5 formulas produce: those formulas count flits only, so the
 * gap can only make the interval longer than the specification requires.
 * Packets sent through the delayed-fault detached path bypass the port
 * transmit arbitration, so they are not rate limited.
 */
enum class PacketRateControlMode : uint8_t {
    NONE,     //!< Disabled (simulator default; not a specification-defined state)
    STATIC,   //!< §4.7.3.1
    DYNAMIC   //!< §4.7.3.2 (introduced in 2.1)
};

/**
 * \class UbPort
 * \brief A Device for Unified-bus
 */
class UbPort : public PointToPointNetDevice {
public:

    static TypeId GetTypeId(void);

    UbPort();

    virtual ~UbPort();

    using ReceiveHandler = Callback<void, Ptr<UbPort>, Ptr<Packet>>;

    void Receive(Ptr<Packet> p);

    void SetReceiveHandler(ReceiveHandler handler);

    void ClearReceiveHandler();

    bool HasReceiveHandler() const;

    void EnableMpiReceive();

    bool HasMpiReceive() const;

    bool Attach(Ptr<UbLink> ch);

    void NotifyLinkUp(void);

    void TriggerTransmit();
    void NotifyAllocationFinish();

    // 判断
    bool IsUb(void) const;

    bool IsReady();

    bool IsBusy();

    void SetCredits(int index, uint8_t value); // 设置用于恢复的信用证值
    void ResetCredits();
    uint8_t GetCredits(int index);
    void SetDataRate(DataRate bps);

    // 获取
    DataRate GetDataRate();

    Time GetInterframeGap();

    Ptr<Channel> GetChannel(void) const override;

    Ptr<UbEgressQueue> GetUbQueue();
    bool EnqueueToEgress(PacketEntry packetEntry);

    uint64_t GetTxBytes();

    void SetIfIndex(const uint32_t index) override;

    void SetSendState(SendState state);

    uint32_t GetIfIndex() const override;

    /**
     * @brief Access this port's direction-local reliable-rerouting component.
     *
     * The component exists on every port and is armed only after an eligible switch-to-switch
     * link is fully attached.
     */
    Ptr<UbReliableRerouting> GetReliableRerouting() const { return m_rerouting; }

    /** Confirm a retained transmit attempt at the peer receipt boundary. */
    void ConfirmTransmitAttempt(uint64_t token);

    /** Block or unblock this port's output. A detected failed output never unblocks. */
    void SetOutputBlocked(bool blocked) { m_outputBlocked = blocked; }
    bool IsOutputBlocked() const { return m_outputBlocked; }

    void IncreaseRcvQueueSize(Ptr<Packet> p, Ptr<UbPort> port);
    void DecreaseRcvQueueSize(Ptr<Packet> p, uint32_t portId);

    void CreateAndInitFc(FcType type);

    Ptr<UbFlowControl> GetFlowControl()
    {
        return m_flowControl;
    }

    static const uint32_t qCnt = 16;

    static std::unordered_map<UbNodeType_t, std::string> g_node_type_map;

    static uint32_t ParseHeader(Ptr<Packet> p, Header& h);

    static void AddUdpHeader(Ptr<Packet> p, Ptr<UbTransportChannel> tp);

    static void AddUdpHeader(Ptr<Packet> p, uint16_t sPort, uint16_t dPort);

    static void AddIpv4Header(Ptr<Packet> p, Ptr<UbTransportChannel> tp);

    static void AddIpv4Header(Ptr<Packet> p, Ipv4Address sIp, Ipv4Address dIp);

    static void AddNetHeader(Ptr<Packet> p);

    uint8_t m_credits[qCnt]; // 需要返回的信用证

    uint64_t GetRcvQueueSize(uint8_t vlId);
    void SetRcvQueueSize(Ptr<Packet> p);
    Ptr<UbFlowControl> m_flowControl; // 使用的流量算法

    uint32_t GetRcvVlQueueSize(uint8_t vlId);
    std::vector<uint32_t> GetRcvQueueSize();

    void SetFaultCallBack(Callback<int, Ptr<Packet>, uint32_t, uint32_t, Ptr<UbPort>> cb)
    {
        m_faultCallBack = cb;
    }
    
    void TransmitPacket(Ptr<Packet>, Time delay);

    void TransmitPacketDetached(Ptr<Packet>);

    /// Reset the channel into READY state and try transmit again
    void TransmitComplete();

    /// Test accessor for the packet rate control algorithm (UB Base Spec 2.1 §4.7).
    Time GetPacketRateNobTimeForTest(Ptr<Packet> packet) const
    {
        return ComputePacketRateNobTime(packet);
    }

    /// Test accessor: advance the deficit counters the way a completed send would.
    void CommitPacketRateCountersForTest(Ptr<Packet> packet)
    {
        CommitPacketRateControlCounters(packet);
    }

    /// Test accessor for the dynamic-mode deficit counters, in flits.
    uint64_t GetPacketRateCounter1ForTest() const
    {
        return m_packetRateCnt1;
    }

    uint64_t GetPacketRateCounter2ForTest() const
    {
        return m_packetRateCnt2;
    }

private:
    void LastAckSerialized(Ptr<Packet> packet);
    EventId m_lastAckSerialization;

    TracedCallback<uint32_t, uint32_t, uint32_t> m_tracePortTxNotify;
    TracedCallback<uint32_t, uint32_t, uint32_t> m_tracePortRxNotify;

    void PortTxNotify(uint32_t nodeId, uint32_t mPortId, uint32_t size);
    void PortRxNotify(uint32_t nodeId, uint32_t mPortId, uint32_t size);

    bool TransmitStart(Ptr<Packet> p);

    /**
     * @brief Equivalent NOB insertion time for this packet (UB Base Spec 2.1 §4.7).
     *
     * Returns zero when packet rate control is disabled, when the packet is not a
     * DLLDP (control frames are never padded), or when the computed insertion is
     * empty. The returned time extends the port occupancy only; it is never added
     * to the packet's serialization time.
     */
    Time ComputePacketRateNobTime(Ptr<Packet> packet) const;

    /**
     * @brief Advance the dynamic-mode deficit counters after a successful send.
     *
     * MUST be called only after UbLink::TransmitStart succeeded -- a failed
     * attempt must not consume burst budget.
     *
     * The transmit path always runs ComputePacketRateNobTime, and with it
     * ValidatePacketRateControlConfig, on the same packet first; this function
     * relies on that validation instead of repeating it (the PKT_DU_LENGTH
     * division would fault on a zero PacketRateDeviceDuLengthFlits).
     * CommitPacketRateCountersForTest inherits the contract: configure the
     * dynamic parameters before committing.
     */
    void CommitPacketRateControlCounters(Ptr<Packet> packet);

    /**
     * @brief True when the packet carries a data link layer data packet (DLLDP).
     *
     * Packet rate control pads DLLDPs only. Link control frames (credit/retry
     * acknowledgements) travel through the same egress queue and must never be
     * padded, so the type is taken from the existing data link header rather
     * than guessed from packet size, VL or the enclosing protocol.
     */
    bool IsDllDataPacket(Ptr<Packet> packet) const;

    /// Abort with a specific error when an enabled mode is missing parameters.
    void ValidatePacketRateControlConfig() const;

    /// Look for an available packet and send it using TransmitStart(p)
    void DequeuePacket(void);

    void PktRcvNotify(Ptr<Packet> p);

    void DispatchReceive(Ptr<Packet> packet);

    void HandleWithUbSwitch(Ptr<Packet> packet);

    void TraComEventNotify(Ptr<Packet> p, Time t);

    void UpdateTxBytes(uint64_t bytes);

    void DoDispose() override;

    uint32_t m_portId;

    Ptr<UbEgressQueue> m_ubEQ;

    Ptr<UbLink> m_channel;

    Ptr<UbReliableRerouting> m_rerouting;

    bool m_outputBlocked{false};

    uint64_t m_txBytes;

    DataRate m_bps;

    SendState m_sendState;  // Current transmission state

    bool m_linkUp;

    bool m_mpiReceiveEnabled;

    Time m_tInterframeGap;

    Ptr<Packet> m_currentPkt;
    uint32_t m_currentInPortId;
    uint32_t m_currentPriority;

    Ptr<UbDataLink> m_datalink;

    TracedCallback<Ptr<Packet>> m_tracePktRcvNotify;
    ReceiveHandler m_receiveHandler;
    Callback<int, Ptr<Packet>, uint32_t, uint32_t, Ptr<UbPort>> m_faultCallBack;

    TracedCallback<Ptr<Packet>, Time> m_traceTraComEventNotify;
    std::vector<uint32_t> m_revQueueSize;  // 接收缓存，用于pfc

    // cbfc
    uint8_t m_cbfcFlitLen;                      // flit长度，默认 20 Bytes
    uint8_t m_cbfcFlitsPerCell;                 // 每个Cell包含的flit数量 取值范围：{1, 2, 4, 8, 16, 32}
    uint8_t m_cbfcRetCellGrainDataPacket;       // 数据包返回的CRD的粒度，通常从以下选项中选择 {1, 2, 4, 8, 16, 32, 64, 128}
    uint8_t m_cbfcRetCellGrainControlPacket;    // 控制报文返回的CRD的粒度，通常从以下选项中选择 {1, 2, 4, 8, 16, 32, 64, 128}
    int32_t m_cbfcPortTxfree;
    int32_t m_cbfcSharedInitCells;
    int32_t m_cbfcCtrlCrdRtrThldCells;

    // pfc
    int32_t m_pfcUpThld;
    int32_t m_pfcLowThld;

    // Packet rate control (UB Base Spec 2.1 §4.7); effective only when enabled.
    // All *_Flits parameters count flits sized by CbfcFlitLenByte (spec §1.6: 20 bytes).
    // Each one carries the peer receiver's advertised value; the Table 4-9 field
    // definitions are quoted in the attribute help strings in ub-port.cc.
    // PacketRateMinIntervalFlits is the only one with a default: §4.7 needs it for both
    // modes, so an unconfigured port still has something to use, and it is labelled a
    // simulator choice rather than a spec one. The DYNAMIC-only parameters below
    // deliberately have none. §4.7.1 derives them from the peer receiver's Init Block,
    // and the Table 4-9 note says "It is not allowed to send only part of the
    // initialization parameters", so validation aborts on an unset one instead of
    // letting the simulator assume a peer capability. Their 0 initialisers are the
    // unset state, not values.
    PacketRateControlMode m_packetRateControl{PacketRateControlMode::NONE};
    /// Table 4-9 Flit 3 bits 55:48, 8 bits. 32 is a simulator default, not a spec value.
    uint8_t m_packetRateMinIntervalFlits{32};         // PACKET_MIN_INTERVAL
    /// Table 4-9 Flit 4 bits 55:48, 8 bits. No default; 0 means unset and aborts.
    uint8_t m_packetRateAveragePacketLengthFlits{0};  // AVERAGE_PACKET_LENGTH
    /// Table 4-9 Flit 4 bits 71:64, 8 bits. No default; 0 means unset and aborts.
    uint8_t m_packetRateAverageDuLengthFlits{0};      // AVERAGE_DU_LENGTH
    /// Table 4-9 Flit 4 bits 79:72, 8 bits. No default; 0 means unset and aborts.
    uint8_t m_packetRateDeviceDuLengthFlits{0};       // DEVICE_DU_LENGTH
    /// Table 4-9 Flit 4 bits 31:16, 16 bits. 0 is the field value "no burst".
    uint16_t m_packetRateBurstSizeFlits{0};           // PACKET_BURST_SIZE
    uint64_t m_packetRateCnt1{0};  // dynamic mode deficit vs AVERAGE_PACKET_LENGTH, in flits
    uint64_t m_packetRateCnt2{0};  // dynamic mode deficit vs the current PKT_DU_LENGTH, in flits

    bool m_pktTraceEnabled = false;
};


} // namespace ns3

#endif // UB_PORT_H
