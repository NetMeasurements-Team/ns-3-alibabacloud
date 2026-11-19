// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_RELIABLE_REROUTING_H
#define UB_RELIABLE_REROUTING_H

#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/traced-callback.h"
#include "ns3/tag.h"

#include <array>
#include <cstdint>
#include <deque>
#include <ostream>
#include <string>

namespace ns3 {

/** Strict whole-message codec for CFG5/NLP6 packet-level LastAck. */
struct UbLastAckMessage
{
    uint32_t sourceNode{0};
    uint32_t destinationNode{0};
    uint16_t destinationPort{0};
    uint16_t receiveNext{0};
};

Ptr<Packet> BuildReliableReroutingLastAck(uint32_t sourceNode,
                                         uint32_t destinationNode,
                                         uint32_t destinationPort,
                                         uint16_t receiveNext);
bool ParseReliableReroutingLastAck(Ptr<const Packet> packet, UbLastAckMessage& message);

/** Simulator-only source-copy identity, never a substitute for wire validation. */
class UbLastAckTag : public Tag
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::UbLastAckTag").SetParent<Tag>().AddConstructor<UbLastAckTag>();
        return tid;
    }
    TypeId GetInstanceTypeId() const override { return GetTypeId(); }
    uint32_t GetSerializedSize() const override { return 9; }
    void Serialize(TagBuffer b) const override
    {
        b.WriteU32(sourceNode); b.WriteU32(sourcePort); b.WriteU8(copy);
    }
    void Deserialize(TagBuffer b) override
    {
        sourceNode = b.ReadU32(); sourcePort = b.ReadU32(); copy = b.ReadU8();
    }
    void Print(std::ostream& os) const override { os << sourceNode << ':' << sourcePort << ':' << +copy; }
    uint32_t sourceNode{0};
    uint32_t sourcePort{0};
    uint8_t copy{0};
};

class UbLink;
class UbPort;

/**
 * @brief One reliable-rerouting observability record.
 *
 * Fields that do not apply to an event keep their documented sentinel: `attemptId == 0` means "no
 * local attempt identity", and `oldOutput`/`newOutput` use `UB_REROUTING_NO_OUTPUT`.
 */
constexpr uint32_t UB_REROUTING_NO_OUTPUT = 0xffffffffu;

/** Result of offering one retained packet to the switch recovery path. */
enum class UbRecoverySubmitResult : uint8_t
{
    TRANSFERRED,  //!< Packet entered a healthy VOQ; the caller may release ownership.
    DISCARDED,    //!< Link-local control frame disposed locally instead of rerouted.
};

enum class UbRecoveryStage : uint8_t
{
    IN_FLIGHT,
    RETRY,
    FAILED_EGRESS,
    RECOVERY_VOQ,
    COUNT
};

struct UbRecoveryOccupancy
{
    uint64_t packets{0};
    uint64_t bytes{0};
    uint64_t peakPackets{0};
    uint64_t peakBytes{0};
};

struct UbRecoveryOccupancyEvent
{
    uint32_t nodeId{0};
    uint32_t portId{0};
    std::string stage;
    std::string state;
    std::string event;
    UbRecoveryOccupancy occupancy;
    std::string source{"none"};
};

struct UbReliableReroutingEvent
{
    uint32_t nodeId{0};
    uint32_t portId{0};
    std::string event;
    uint64_t attemptId{0};
    std::string owner{"none"};
    uint32_t oldOutput{UB_REROUTING_NO_OUTPUT};
    uint32_t newOutput{UB_REROUTING_NO_OUTPUT};
    uint64_t retryPackets{0};
    uint64_t retryBytes{0};
    uint64_t pendingPackets{0};
    uint64_t pendingBytes{0};
    uint64_t processingPackets{0};
    uint64_t inFlightPackets{0};
    uint64_t inFlightBytes{0};
    uint64_t failedEgressPackets{0};
    uint64_t failedEgressBytes{0};
    uint64_t recoveryVoqPackets{0};
    uint64_t recoveryVoqBytes{0};
    uint32_t sourceNode{UB_REROUTING_NO_OUTPUT};
    uint32_t sourcePort{UB_REROUTING_NO_OUTPUT};
    uint32_t destinationNode{UB_REROUTING_NO_OUTPUT};
    uint32_t destinationPort{UB_REROUTING_NO_OUTPUT};
    uint32_t receiveNext{UB_REROUTING_NO_OUTPUT};
    uint32_t copy{0};
};

void TraceReliableReroutingLastAck(Ptr<const Packet> packet,
                                   uint32_t node,
                                   uint32_t port,
                                   const std::string& event);

/**
 * @brief Direction-local reliable rerouting state for one UB port.
 *
 * The component retains original whole packets for one link direction until complete peer receipt,
 * and turns a physical link failure into the model's
 * detect / wait-for-LastAck / recover / drained progression. It owns the modeled timers and asks
 * the owning switch to migrate retained work; it never mutates switch queues itself.
 */
class UbReliableRerouting : public Object
{
  public:
    enum class State
    {
        DISABLED,           //!< Feature not armed on this port.
        HEALTHY,            //!< Link healthy, in-flight references released by peer receipt.
        DETECTION_PENDING,  //!< Physical failure seen, endpoint has not detected it yet.
        WAIT_LAST_ACK,      //!< Output detected as failed; backlog frozen until LastAck.
        RECOVERING,         //!< LastAck received; retained work can move to healthy outputs.
        DRAINED             //!< Local recovery work finished; the failed output stays down.
    };

    static TypeId GetTypeId();

    UbReliableRerouting();
    ~UbReliableRerouting() override;

    /**
     * @brief Arm this port for reliable rerouting before traffic starts.
     * @param port Owning port.
     * @param link Attached link.
     * @param detectionDelay Physical failure to endpoint detection.
     * @param lastAckTimeout Maximum wait for an actual LastAck.
     */
    void Arm(Ptr<UbPort> port, Ptr<UbLink> link, Time detectionDelay, Time lastAckTimeout);

    bool IsArmed() const { return m_state != State::DISABLED; }
    State GetState() const { return m_state; }
    static const char* GetStateName(State state);

    /** The detected failed output must not be used for routing or transmission. */
    bool IsOutputBlocked() const
    {
        return m_state == State::WAIT_LAST_ACK || m_state == State::RECOVERING ||
               m_state == State::DRAINED;
    }

    Time GetDetectionDelay() const { return m_detectionDelay; }
    Time GetLastAckTimeout() const { return m_lastAckTimeout; }

    // ---- Original packet lifecycle ----------------------------------------

    /**
     * @brief Retain the original packet of an eligible transmit attempt.
     * @param packet Packet handed to the link.
     * @param inPort Original ingress port identity.
     * @param priority Original VL/priority.
     * @return Local monotonic attempt token; never zero.
     */
    uint64_t CreateAttempt(Ptr<Packet> packet, uint32_t inPort, uint32_t priority);

    /**
     * @brief Release the attempt that the peer completely received.
     * @param token Attempt token; zero means "not a retained attempt".
     */
    void ConfirmAttempt(uint64_t token);
    /** Advance only at complete link delivery; token-1 is the zero-based packet sequence. */
    void ReceiveAttempt(uint64_t token);
    uint64_t GetConfirmedNext() const { return m_confirmedNext; }
    uint64_t GetReceiveNext() const { return m_receiveNext; }

    const UbRecoveryOccupancy& GetOccupancy(UbRecoveryStage stage) const;
    static const char* GetStageName(UbRecoveryStage stage);
    uint64_t GetRetryPacketCount() const { return GetOccupancy(UbRecoveryStage::RETRY).packets; }
    uint64_t GetRetryByteCount() const { return GetOccupancy(UbRecoveryStage::RETRY).bytes; }
    uint64_t GetPeakRetryPacketCount() const { return GetOccupancy(UbRecoveryStage::RETRY).peakPackets; }
    uint64_t GetInFlightPacketCount() const { return GetOccupancy(UbRecoveryStage::IN_FLIGHT).packets; }
    uint64_t GetInFlightByteCount() const { return GetOccupancy(UbRecoveryStage::IN_FLIGHT).bytes; }
    uint64_t GetPeakInFlightPacketCount() const { return GetOccupancy(UbRecoveryStage::IN_FLIGHT).peakPackets; }
    uint64_t GetRecoveryVoqPacketCount() const { return GetOccupancy(UbRecoveryStage::RECOVERY_VOQ).packets; }
    uint64_t GetRecoveryVoqByteCount() const { return GetOccupancy(UbRecoveryStage::RECOVERY_VOQ).bytes; }
    void EnqueueRecoveryVoq(uint32_t bytes, UbRecoveryStage source);
    void DequeueRecoveryVoq(uint32_t bytes);
    void FailedEgressEnqueued(uint32_t bytes);
    void FailedEgressDequeued(uint32_t bytes);
    void DiscardFailedEgress(uint32_t bytes);
    void WriteOccupancySummary();

    // ---- Failure progression ---------------------------------------------

    /** Physical failure action (t0); arms the detection timer. */
    void OnPhysicalFailure();

    /** Detection completion (td); freezes the output and stops new attempts. */
    void OnDetection();

    /** Validate an actual network-delivered LastAck before releasing retained ownership. */
    void ReceiveLastAck(Ptr<const Packet> packet, const UbLastAckMessage& message);
    void NotifyLastAckSent(uint8_t copy);

    /** Switch reports that retained backlog for this output finished recovery. */
    void NotifyBacklogFinished();

    /** Switch reports how much retained work it currently owns for this output. */
    void SetPendingRecoveryCounters(uint64_t packets, uint64_t bytes, uint64_t processing);

    /**
     * @brief Publish an evidence record for switch-owned retained work.
     *
     * VOQ, egress, and delayed-processing backlog entries are owned by the switch, but the
     * observability path stays single-sourced on this component so the CSV schema has one writer.
     */
    void PublishBacklogEvent(const std::string& event,
                             uint64_t entryId,
                             uint32_t oldOutput,
                             uint32_t newOutput = UB_REROUTING_NO_OUTPUT);

    void DoDispose() override;

    TracedCallback<const UbReliableReroutingEvent&> m_traceRerouting;

  private:
    struct Attempt
    {
        uint64_t token{0};
        Ptr<Packet> packet;
        uint32_t inPort{0};
        uint32_t priority{0};
    };

    void AddOccupancy(UbRecoveryStage stage, uint64_t packets, uint64_t bytes);
    void RemoveOccupancy(UbRecoveryStage stage, uint64_t packets, uint64_t bytes);
    void PublishOccupancy(const std::string& event, bool summary = false,
                          UbRecoveryStage source = UbRecoveryStage::COUNT);
    void Publish(const std::string& event,
                 uint64_t attemptId = 0,
                 uint32_t oldOutput = UB_REROUTING_NO_OUTPUT,
                 uint32_t newOutput = UB_REROUTING_NO_OUTPUT,
                 Ptr<const Packet> message = nullptr,
                 bool switchOwned = false);
    void TransitionTo(State state);
    void TransferRetryBacklog();
    void OnLastAck();
    void OnLastAckTimeout();
    void OnLastAckSendTimeout();
    void UpdateDrainState();

    Ptr<UbPort> m_port;
    Ptr<UbLink> m_link;
    State m_state{State::DISABLED};
    Time m_detectionDelay{Time(0)};
    Time m_lastAckTimeout{Time(0)};
    Time m_detectionTime{Time(0)};
    EventId m_detectionEvent;
    EventId m_lastAckEvent;
    EventId m_lastAckSendEvent;
    uint32_t m_peerNode{0};
    uint32_t m_peerPort{0};
    uint8_t m_sentCopies{0};
    uint64_t m_nextAttemptToken{1};
    uint64_t m_confirmedNext{0};
    uint64_t m_receiveNext{0};
    std::deque<Attempt> m_inFlight;
    std::deque<Attempt> m_attempts;
    std::array<UbRecoveryOccupancy, static_cast<size_t>(UbRecoveryStage::COUNT)> m_occupancy{};
    bool m_traceOccupancyEvents{false};
    uint64_t m_pendingPackets{0};
    uint64_t m_pendingBytes{0};
    uint64_t m_processingPackets{0};
    bool m_backlogFinished{false};
};

} // namespace ns3

namespace ns3 {

inline std::ostream&
operator<<(std::ostream& os, UbReliableRerouting::State state)
{
    return os << UbReliableRerouting::GetStateName(state);
}

} // namespace ns3

#endif // UB_RELIABLE_REROUTING_H
