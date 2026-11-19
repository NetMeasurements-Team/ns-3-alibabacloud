// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_RETRANS_H
#define UB_RETRANS_H

#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/ub-datatype.h"
#include "ns3/ub-header.h"
#include "ns3/ub-lifecycle.h"
#include "ns3/packet.h"

#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace ns3 {

class UbTransportChannel;
class UbRetransController;

struct UbRetransAckResult
{
    bool ackAdvanced{false};
    bool ignoreResponse{false};
    uint64_t previousSndUna{0};
    uint64_t newSndUna{0};
    bool triggerTransmit{false};
    uint32_t retransmitBytes{0};
};

struct UbRetransReceiveDecision
{
    bool dropPacket{false};
    bool shouldAck{false};
    bool shouldNak{false};
    bool selectiveAck{false};
    bool suppressResponse{false};
    uint64_t responsePsn{0};
    TpOpcode responseOpcode{TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH};
    std::optional<UbSelectiveAckExtTph> selectiveAckHeader;
};

struct UbRetransTimeoutResult
{
    bool triggerTransmit{false};
    uint32_t retransmitBytes{0};
};

struct UbGbnPacketDescriptor
{
    uint64_t logicalPsn{0};
    UbTransactionCorrelationIdentity correlation;
    bool hasCorrelation{false};
    uint32_t packetOffsetBytes{0};
    uint32_t progressBytes{0};
    uint32_t payloadBytes{0};
    uint32_t wireLengthBytes{0};
    uint64_t remoteAddress{0};
    uint32_t remoteTokenId{0};
    UbTransactionFields transactionFields;
    Ptr<const Packet> explicitPayload;
    uint32_t taskId{0};
    uint32_t wqeSize{0};
    UbTransactionOpcode taOpcode{UbTransactionOpcode::SEND};
    UbTransactionOpcode requestOpcode{UbTransactionOpcode::SEND};
    UbTransactionSegmentKind segmentKind{UbTransactionSegmentKind::REQUEST};
    uint32_t taSsn{0};
    uint32_t requestTaSsn{0};
    OrderType order{OrderType::ORDER_NO};
    uint32_t originJettyNum{0};
    uint64_t tpMsn{0};
    bool firstPacket{false};
    bool lastPacket{false};
    uint32_t serializedSizeBytes{0};
};

struct UbRetransTimerTestStats
{
    uint64_t startCalls{0};
    uint64_t selectiveOutstandingScanCalls{0};
    uint64_t selectiveOutstandingEntriesVisited{0};
    uint64_t scheduleCalls{0};
    uint64_t cancelCalls{0};
    uint64_t restartCalls{0};
};

class UbRetransStrategy
{
public:
    virtual ~UbRetransStrategy() = default;
};

class UbGbnRetransStrategy : public UbRetransStrategy
{
public:
    explicit UbGbnRetransStrategy(UbRetransController& controller);

    void RetainDescriptor(const UbGbnPacketDescriptor& descriptor);
    bool PrepareRetransmissionFromPsn(uint64_t psn);
    bool HandleTpNak(uint64_t nakPsn);
    Ptr<Packet> TryGetNextRetransmissionPacket();
    bool HasPendingRetransmission() const;
    uint32_t GetNextRetransmissionSize() const;
    uint32_t GetNextRetransmissionProgressBytes() const;
    uint32_t GetNextRetransmissionPayloadBytes() const;
    /** Descriptor of the packet the next GBN retransmission will rebuild, or nullptr. */
    const UbGbnPacketDescriptor* GetNextRetransmissionDescriptor() const;
    uint32_t GetRetransmissionPayloadBytesFrom(uint64_t psn) const;
    bool HasOutstandingDescriptors() const;
    void AcknowledgePrefix(uint64_t newSndUna);
    void RetireSegment(const UbTransactionCorrelationIdentity& correlation,
                       uint64_t psnStart,
                       uint64_t psnEnd);
    UbRetransReceiveDecision OnDataPacketReceived(uint64_t psn);
    UbRetransTimeoutResult OnTimeout();
    void ClearNakSuppressionIfGapClosed(uint64_t recvNext);
    uint32_t GetDescriptorCountForTest() const;
    bool HasDescriptorForTest(uint64_t psn) const;
    uint64_t GetRetransmissionCursorForTest() const;
    void Clear();

private:
    void AdvanceRetransmissionCursor(const UbGbnPacketDescriptor& sentDescriptor);

    UbRetransController& m_controller;
    std::map<uint64_t, UbGbnPacketDescriptor> m_descriptors;
    std::optional<uint64_t> m_retransmitCursor;
    uint64_t m_retransmitEnd{0};
    uint64_t m_lastNakPsn{std::numeric_limits<uint64_t>::max()};
};

class UbSelectiveRetransStrategy : public UbRetransStrategy
{
public:
    explicit UbSelectiveRetransStrategy(UbRetransController& controller);

    void RetainSentPsn(const UbGbnPacketDescriptor& descriptor);
    void MarkPsnAcked(uint64_t psn);
    void AcknowledgeCumulativePsn(uint64_t ackPsn);
    void OnCumulativeAckProgress(uint64_t previousSndUna, uint64_t newSndUna);
    void AdvanceSendUnaFromAckState();
    std::vector<uint64_t> CollectMissingPsnsFromSelectiveAck(uint64_t ackBase,
                                                             const UbSelectiveAckExtTph& saetph);
    std::vector<uint64_t> GetMissingPsnsFromSelectiveAck(uint64_t ackBase,
                                                         const UbSelectiveAckExtTph& saetph) const;
    bool QueueRetransmission(uint64_t psn);
    void CompactRetransmissionQueue();
    Ptr<Packet> TryGetNextRetransmissionPacket();
    UbRetransTimeoutResult OnTimeout();
    UbRetransAckResult OnTransportResponse(const UbTransportHeader& tpHeader,
                                           TpOpcode opcode,
                                           const UbSelectiveAckExtTph& saetph,
                                           const UbCongestionExtTph* cetph);
    UbRetransAckResult OnCumulativeAck(const UbTransportHeader& tpHeader);
    bool ResolveSelectiveAckBitmapBits(uint32_t& bits) const;
    uint32_t GetSelectiveAckBitmapBits() const;
    uint64_t GetSelectiveAckBase() const;
    UbSelectiveAckExtTph BuildSelectiveAckHeader(uint64_t ackBase) const;
    UbRetransReceiveDecision BuildReceiveDecisionForCurrentState() const;
    void OnNewDataPacketSent(const UbGbnPacketDescriptor& descriptor);
    void RetireSegment(const UbTransactionCorrelationIdentity& correlation,
                       uint64_t psnStart,
                       uint64_t psnEnd);
    bool HasPendingRetransmission() const;
    bool CanSendRetransmission() const;
    uint32_t GetNextRetransmissionSize() const;
    uint32_t GetNextRetransmissionPayloadBytes() const;
    uint32_t GetRetainedPayloadBytes(uint64_t psn) const;
    bool HasOutstandingPackets() const;

    uint32_t GetPendingRetransmissionCountForTest() const;
    uint32_t GetRawRetransmissionQueueCountForTest() const;
    bool WasPsnSelectivelyReportedMissingForTest(uint64_t psn) const;
    bool HasRetainedPsnForTest(uint64_t psn) const;
    uint32_t GetPsnRetransmitCountForTest(uint64_t psn) const;
    void Clear();

private:
    struct SentPsnState
    {
        UbGbnPacketDescriptor descriptor;
        bool acknowledged{false};
        bool selectivelyReportedMissing{false};
        bool retransmitPending{false};
        uint32_t retransmitCount{0};
    };

    void RetireAckedStateBeforeSendUna();
    bool IsMarkPsnEnabled() const;
    bool SelectiveAckReportsReceivedAtOrAboveMarkPsn(uint64_t ackBase,
                                                     const UbSelectiveAckExtTph& saetph) const;
    void EnterMarkPsnRetransPhase();
    void FinishMarkPsnRetransPhaseIfDone();
    void MaybeMarkFirstNewSelectivePacket(uint64_t psn);

    UbRetransController& m_controller;
    std::map<uint64_t, SentPsnState> m_sentPsnState;
    std::deque<uint64_t> m_selectiveRetransmitQ;
    bool m_markPsnRetransPhase{false};
    bool m_markPsnAwaitingFirstNew{true};
    bool m_markPsnValid{false};
    uint64_t m_markPsn{0};
    bool m_lastFirstRtxPsnValid{false};
    uint64_t m_lastFirstRtxPsn{0};
};

class UbRetransController
{
public:
    explicit UbRetransController(UbTransportChannel& transport);
    ~UbRetransController();

    void SetBaseRto(Time rto);
    Time GetBaseRto() const;

    void SetCurrentRto(Time rto);
    Time GetCurrentRto() const;

    void SetMaxRetransAttempts(uint16_t attempts);
    uint16_t GetMaxRetransAttempts() const;

    void SetRetransAttemptsLeft(uint16_t attempts);
    uint16_t GetRetransAttemptsLeft() const;

    void SetRetransExponentFactor(uint16_t factor);
    uint16_t GetRetransExponentFactor() const;

    void SetRetransTimeoutMode(UbRetransTimeoutMode mode);
    UbRetransTimeoutMode GetRetransTimeoutMode() const;

    void SetRetransmissionMode(UbRetransmissionMode mode);
    UbRetransmissionMode GetRetransmissionMode() const;

    void SetSelectiveAckBitmapBits(uint32_t bits);
    uint32_t GetSelectiveAckBitmapBitsConfig() const;

    void SetFastRetransEnable(bool enable);
    bool GetFastRetransEnable() const;

    void SetSelectiveMarkPsnEnable(bool enable);
    bool GetSelectiveMarkPsnEnable() const;

    void SetRetransEnable(bool enable);
    bool GetRetransEnable() const;
    void StartTimerIfNeeded();
    void RestartTimerAfterAckProgress();
    void CancelTimer();
    UbRetransTimeoutResult OnTimeout();
    bool HasTimerRunning() const;
    bool HasOutstandingRetransmissionState() const;
    UbRetransAckResult OnTransportNak(const UbTransportHeader& tph);
    UbRetransAckResult OnTransportSelectiveAck(const UbTransportHeader& tph,
                                               TpOpcode opcode,
                                               const UbSelectiveAckExtTph& saetph,
                                               const UbCongestionExtTph* cetph);
    void OnSenderCumulativeAckProgress(uint64_t previousSndUna, uint64_t newSndUna);
    UbRetransReceiveDecision OnDataPacketReceived(uint64_t psn);
    bool ResolveSelectiveAckBitmapBits(uint32_t& bits) const;
    UbRetransReceiveDecision BuildReceiveDecisionForCurrentState() const;
    void OnNewDataPacketSent(const UbGbnPacketDescriptor& descriptor);
    Ptr<Packet> TryGetNextRetransmissionPacket();
    bool HasPendingGbnRetransmission() const;
    uint32_t GetNextGbnRetransmissionSize() const;
    uint32_t GetNextGbnRetransmissionProgressBytes() const;
    uint32_t GetNextGbnRetransmissionPayloadBytes() const;
    /** Descriptor of the packet the next GBN retransmission will rebuild, or nullptr. */
    const UbGbnPacketDescriptor* GetNextGbnRetransmissionDescriptor() const;
    void RetireGbnSegment(const UbTransactionCorrelationIdentity& correlation,
                          uint64_t psnStart,
                          uint64_t psnEnd);
    void RetireSegmentDescriptors(const UbTransactionCorrelationIdentity& correlation,
                                  uint64_t psnStart,
                                  uint64_t psnEnd);
    bool HasPendingSelectiveRetransmission() const;
    bool CanSendSelectiveRetransmission() const;
    uint32_t GetNextSelectiveRetransmissionSize() const;
    uint32_t GetNextSelectiveRetransmissionPayloadBytes() const;
    uint32_t GetPendingSelectiveRetransmissionCountForTest() const;
    uint32_t GetRawSelectiveRetransmissionQueueCountForTest() const;
    bool WasPsnSelectivelyReportedMissingForTest(uint64_t psn) const;
    bool HasRetainedPsnForTest(uint64_t psn) const;
    uint32_t GetPsnRetransmitCountForTest(uint64_t psn) const;
    uint32_t GetGbnDescriptorCountForTest() const;
    bool HasGbnDescriptorForTest(uint64_t psn) const;
    uint64_t GetGbnRetransmissionCursorForTest() const;
    void RetainGbnDescriptorForTest(const UbGbnPacketDescriptor& descriptor);
    void ResetTimerStatsForTest();
    UbRetransTimerTestStats GetTimerStatsForTest() const;
    void ClearRetainedState();
    void ClearNakSuppressionIfGapClosed(uint64_t recvNext);
    UbTransportChannel& GetTransport();
    const UbTransportChannel& GetTransport() const;

private:
    friend class UbSelectiveRetransStrategy;

    void ScheduleTimeout();
    void HandleTrackedTimeout(uint64_t token);
    bool IsCollectingTimerStatsForTest() const;
    void RecordSelectiveOutstandingScanForTest(uint64_t entriesVisited) const;

    UbTransportChannel& m_transport;
    std::unique_ptr<UbGbnRetransStrategy> m_gbn;
    std::unique_ptr<UbSelectiveRetransStrategy> m_selective;
    EventId m_retransEvent{};
    UbTrackedEventSet m_trackedEvents;
    bool m_trackedEventsInitialized{false};
    Time m_baseRto{NanoSeconds(25600)};
    Time m_rto{NanoSeconds(25600)};
    uint16_t m_maxRetransAttempts{7};
    uint16_t m_retransAttemptsLeft{7};
    uint16_t m_retransExponentFactor{1};
    UbRetransTimeoutMode m_retransTimeoutMode{UbRetransTimeoutMode::DYNAMIC};
    UbRetransmissionMode m_retransmissionMode{UbRetransmissionMode::GBN};
    uint32_t m_selectiveAckBitmapBits{0};
    bool m_enableRetrans{false};
    bool m_enableFastRetrans{false};
    bool m_enableSelectiveMarkPsn{false};
    bool m_collectTimerTestStats{false};
    mutable UbRetransTimerTestStats m_timerTestStats;
};

} // namespace ns3

#endif // UB_RETRANS_H
