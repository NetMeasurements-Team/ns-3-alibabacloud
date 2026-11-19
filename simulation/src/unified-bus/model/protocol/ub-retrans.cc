// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-retrans.h"

#include "ns3/assert.h"
#include "ns3/log.h"
#include "ns3/node-list.h"
#include "ns3/simulator.h"
#include "ns3/ub-modulo-sequence.h"
#include "ns3/ub-transport.h"

#include <algorithm>
#include <array>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbRetransController");

namespace {

using UbTpPsnSequence = UbModuloSequence<24, uint64_t>;

} // namespace

UbGbnRetransStrategy::UbGbnRetransStrategy(UbRetransController& controller)
    : m_controller(controller)
{
}

void
UbGbnRetransStrategy::RetainDescriptor(const UbGbnPacketDescriptor& descriptor)
{
    NS_ABORT_MSG_IF(descriptor.logicalPsn < m_controller.GetTransport().GetPsnSndUna(),
                    "Cannot retain an already acknowledged GBN descriptor");
    NS_ABORT_MSG_IF(!m_descriptors.emplace(descriptor.logicalPsn, descriptor).second,
                    "Cannot retain the same GBN PSN descriptor twice");
}

bool
UbGbnRetransStrategy::PrepareRetransmissionFromPsn(uint64_t psn)
{
    const UbTransportChannel& transport = m_controller.GetTransport();
    if (psn < transport.GetPsnSndUna() || psn >= transport.GetPsnSndNxt()) {
        return false;
    }
    const auto descriptor = m_descriptors.lower_bound(psn);
    if (descriptor == m_descriptors.end() || descriptor->first >= transport.GetPsnSndNxt()) {
        if (!HasPendingRetransmission()) {
            m_retransmitCursor.reset();
            m_retransmitEnd = 0;
        }
        return false;
    }
    m_retransmitEnd = transport.GetPsnSndNxt();
    m_retransmitCursor = descriptor->first;
    return true;
}

void
UbGbnRetransStrategy::AdvanceRetransmissionCursor(
    const UbGbnPacketDescriptor& sentDescriptor)
{
    const auto sent = m_descriptors.find(sentDescriptor.logicalPsn);
    if (sent == m_descriptors.end() ||
        sent->second.hasCorrelation != sentDescriptor.hasCorrelation ||
        (sentDescriptor.hasCorrelation &&
         sent->second.correlation != sentDescriptor.correlation)) {
        return;
    }
    const auto next = m_descriptors.upper_bound(sentDescriptor.logicalPsn);
    if (next == m_descriptors.end() || next->first >= m_retransmitEnd) {
        m_retransmitCursor.reset();
        m_retransmitEnd = 0;
        return;
    }
    m_retransmitCursor = next->first;
}

Ptr<Packet>
UbGbnRetransStrategy::TryGetNextRetransmissionPacket()
{
    if (!HasPendingRetransmission()) {
        return nullptr;
    }
    const auto descriptorIt = m_descriptors.find(*m_retransmitCursor);
    NS_ABORT_MSG_IF(descriptorIt == m_descriptors.end(),
                    "GBN retransmission cursor lost its descriptor");
    const UbGbnPacketDescriptor descriptor = descriptorIt->second;
    Ptr<Packet> packet = m_controller.GetTransport().SendGbnRetransmission(descriptor);
    if (packet != nullptr) {
        AdvanceRetransmissionCursor(descriptor);
    }
    return packet;
}

bool
UbGbnRetransStrategy::HasPendingRetransmission() const
{
    return m_retransmitCursor.has_value() && *m_retransmitCursor < m_retransmitEnd &&
           m_descriptors.find(*m_retransmitCursor) != m_descriptors.end();
}

uint32_t
UbGbnRetransStrategy::GetNextRetransmissionSize() const
{
    if (!HasPendingRetransmission()) {
        return 0;
    }
    return m_descriptors.at(*m_retransmitCursor).wireLengthBytes;
}

uint32_t
UbGbnRetransStrategy::GetNextRetransmissionProgressBytes() const
{
    if (!HasPendingRetransmission()) {
        return 0;
    }
    return m_descriptors.at(*m_retransmitCursor).progressBytes;
}

uint32_t
UbGbnRetransStrategy::GetNextRetransmissionPayloadBytes() const
{
    if (!HasPendingRetransmission()) {
        return 0;
    }
    return m_descriptors.at(*m_retransmitCursor).payloadBytes;
}

const UbGbnPacketDescriptor*
UbGbnRetransStrategy::GetNextRetransmissionDescriptor() const
{
    if (!HasPendingRetransmission())
    {
        return nullptr;
    }
    return &m_descriptors.at(*m_retransmitCursor);
}

uint32_t
UbGbnRetransStrategy::GetRetransmissionPayloadBytesFrom(uint64_t psn) const
{
    uint32_t payloadBytes = 0;
    for (auto descriptor = m_descriptors.lower_bound(psn);
         descriptor != m_descriptors.end() &&
         descriptor->first < m_controller.GetTransport().GetPsnSndNxt();
         ++descriptor) {
        payloadBytes += descriptor->second.payloadBytes;
    }
    return payloadBytes;
}

bool
UbGbnRetransStrategy::HasOutstandingDescriptors() const
{
    return !m_descriptors.empty();
}

void
UbGbnRetransStrategy::AcknowledgePrefix(uint64_t newSndUna)
{
    m_descriptors.erase(m_descriptors.begin(), m_descriptors.lower_bound(newSndUna));
    if (!m_retransmitCursor.has_value()) {
        return;
    }
    const auto next = m_descriptors.lower_bound(*m_retransmitCursor);
    if (next == m_descriptors.end() || next->first >= m_retransmitEnd) {
        m_retransmitCursor.reset();
        m_retransmitEnd = 0;
    } else {
        m_retransmitCursor = next->first;
    }
}

void
UbGbnRetransStrategy::RetireSegment(const UbTransactionCorrelationIdentity& correlation,
                                    uint64_t psnStart,
                                    uint64_t psnEnd)
{
    for (auto descriptor = m_descriptors.lower_bound(psnStart);
         descriptor != m_descriptors.end() && descriptor->first < psnEnd;) {
        if (descriptor->second.hasCorrelation && descriptor->second.correlation == correlation) {
            descriptor = m_descriptors.erase(descriptor);
        } else {
            ++descriptor;
        }
    }
    if (m_retransmitCursor.has_value() && *m_retransmitCursor >= psnStart &&
        *m_retransmitCursor < psnEnd) {
        const auto next = m_descriptors.lower_bound(*m_retransmitCursor);
        if (next == m_descriptors.end() || next->first >= m_retransmitEnd) {
            m_retransmitCursor.reset();
            m_retransmitEnd = 0;
        } else {
            m_retransmitCursor = next->first;
        }
    }
}

bool
UbGbnRetransStrategy::HandleTpNak(uint64_t nakPsn)
{
    if (m_controller.GetRetransmissionMode() != UbRetransmissionMode::GBN ||
        !m_controller.GetFastRetransEnable()) {
        return false;
    }

    const UbTransportChannel& transport = m_controller.GetTransport();
    if (nakPsn < transport.GetPsnSndUna() || nakPsn >= transport.GetPsnSndNxt()) {
        return false;
    }

    return PrepareRetransmissionFromPsn(nakPsn);
}

UbRetransReceiveDecision
UbGbnRetransStrategy::OnDataPacketReceived(uint64_t psn)
{
    UbRetransReceiveDecision decision;
    if (m_controller.GetRetransmissionMode() != UbRetransmissionMode::GBN) {
        return decision;
    }

    const uint64_t nakPsn = m_controller.GetTransport().GetPsnRecvNxt();
    if (psn <= nakPsn) {
        return decision;
    }
    decision.dropPacket = true;
    if (!m_controller.GetFastRetransEnable()) {
        return decision;
    }
    if (m_lastNakPsn == nakPsn) {
        decision.suppressResponse = true;
        decision.responsePsn = nakPsn;
        return decision;
    }

    m_lastNakPsn = nakPsn;
    decision.shouldNak = true;
    decision.responsePsn = nakPsn;
    decision.responseOpcode = m_controller.GetTransport().GetResponseOpcodeForRetrans(false);
    return decision;
}

UbRetransTimeoutResult
UbGbnRetransStrategy::OnTimeout()
{
    UbRetransTimeoutResult result;
    if (m_controller.GetRetransmissionMode() != UbRetransmissionMode::GBN) {
        return result;
    }

    const uint64_t sndUna = m_controller.GetTransport().GetPsnSndUna();
    result.triggerTransmit = PrepareRetransmissionFromPsn(sndUna);
    if (result.triggerTransmit) {
        result.retransmitBytes = GetRetransmissionPayloadBytesFrom(sndUna);
    }
    return result;
}

void
UbGbnRetransStrategy::ClearNakSuppressionIfGapClosed(uint64_t recvNext)
{
    if (m_lastNakPsn != std::numeric_limits<uint64_t>::max() && recvNext > m_lastNakPsn) {
        m_lastNakPsn = std::numeric_limits<uint64_t>::max();
    }
}

uint32_t
UbGbnRetransStrategy::GetDescriptorCountForTest() const
{
    return static_cast<uint32_t>(m_descriptors.size());
}

bool
UbGbnRetransStrategy::HasDescriptorForTest(uint64_t psn) const
{
    return m_descriptors.find(psn) != m_descriptors.end();
}

uint64_t
UbGbnRetransStrategy::GetRetransmissionCursorForTest() const
{
    return m_retransmitCursor.value_or(std::numeric_limits<uint64_t>::max());
}

void
UbGbnRetransStrategy::Clear()
{
    m_descriptors.clear();
    m_retransmitCursor.reset();
    m_retransmitEnd = 0;
    m_lastNakPsn = std::numeric_limits<uint64_t>::max();
}

UbSelectiveRetransStrategy::UbSelectiveRetransStrategy(UbRetransController& controller)
    : m_controller(controller)
{
}

void
UbSelectiveRetransStrategy::RetainSentPsn(const UbGbnPacketDescriptor& descriptor)
{
    SentPsnState& state = m_sentPsnState[descriptor.logicalPsn];
    state.descriptor = descriptor;
    state.acknowledged = false;
    state.selectivelyReportedMissing = false;
    state.retransmitPending = false;
    state.retransmitCount = 0;
}

void
UbSelectiveRetransStrategy::MarkPsnAcked(uint64_t psn)
{
    auto it = m_sentPsnState.find(psn);
    if (it == m_sentPsnState.end()) {
        return;
    }
    it->second.acknowledged = true;
    it->second.selectivelyReportedMissing = false;
    it->second.retransmitPending = false;
}

void
UbSelectiveRetransStrategy::AcknowledgeCumulativePsn(uint64_t ackPsn)
{
    UbTransportChannel& transport = m_controller.GetTransport();
    for (uint64_t psn = transport.GetPsnSndUna(); psn <= ackPsn; ++psn) {
        MarkPsnAcked(psn);
        if (psn == UINT64_MAX) {
            break;
        }
    }
    if (ackPsn + 1 > transport.GetPsnSndUna()) {
        transport.SetPsnSndUna(ackPsn + 1);
    }
    RetireAckedStateBeforeSendUna();
}

void
UbSelectiveRetransStrategy::OnCumulativeAckProgress(uint64_t previousSndUna,
                                                    uint64_t newSndUna)
{
    if (newSndUna <= previousSndUna) {
        return;
    }

    for (uint64_t psn = previousSndUna; psn < newSndUna; ++psn) {
        MarkPsnAcked(psn);
        if (psn == UINT64_MAX) {
            break;
        }
    }
    RetireAckedStateBeforeSendUna();
    CompactRetransmissionQueue();
}

void
UbSelectiveRetransStrategy::AdvanceSendUnaFromAckState()
{
    UbTransportChannel& transport = m_controller.GetTransport();
    while (true) {
        auto it = m_sentPsnState.find(transport.GetPsnSndUna());
        if (it == m_sentPsnState.end() || !it->second.acknowledged) {
            break;
        }
        m_sentPsnState.erase(it);
        transport.SetPsnSndUna(transport.GetPsnSndUna() + 1);
    }
}

std::vector<uint64_t>
UbSelectiveRetransStrategy::CollectMissingPsnsFromSelectiveAck(uint64_t ackBase,
                                                               const UbSelectiveAckExtTph& saetph)
{
    const uint32_t bitmapBits = saetph.GetBitmapBitCount();
    const uint64_t representedEnd = ackBase + bitmapBits - 1;
    const uint64_t maxRcvPsn =
        UbTpPsnSequence::UnwrapAtOrAfter(saetph.GetMaxRcvPsn(), ackBase);
    const uint64_t lastCandidate = std::min<uint64_t>(maxRcvPsn, representedEnd);
    const bool baseIsMissingEvidence = ackBase == 0 && !saetph.GetBitmapBit(0);
    uint64_t firstCandidate = baseIsMissingEvidence ? 0 : ackBase + 1;
    std::vector<uint64_t> missingPsns;

    for (uint64_t psn = ackBase; psn <= lastCandidate; ++psn) {
        const uint32_t offset = static_cast<uint32_t>(psn - ackBase);
        if (saetph.GetBitmapBit(offset)) {
            MarkPsnAcked(psn);
        }
        if (psn == UINT64_MAX) {
            break;
        }
    }

    for (uint64_t psn = firstCandidate; psn <= lastCandidate; ++psn) {
        const uint32_t offset = static_cast<uint32_t>(psn - ackBase);
        if (saetph.GetBitmapBit(offset)) {
            if (psn == UINT64_MAX) {
                break;
            }
            continue;
        }

        auto it = m_sentPsnState.find(psn);
        if (it != m_sentPsnState.end() && !it->second.acknowledged &&
            !it->second.selectivelyReportedMissing)
        {
            it->second.selectivelyReportedMissing = true;
            missingPsns.push_back(psn);
        }
        if (psn == UINT64_MAX) {
            break;
        }
    }
    return missingPsns;
}

std::vector<uint64_t>
UbSelectiveRetransStrategy::GetMissingPsnsFromSelectiveAck(uint64_t ackBase,
                                                           const UbSelectiveAckExtTph& saetph) const
{
    const uint32_t bitmapBits = saetph.GetBitmapBitCount();
    const uint64_t representedEnd = ackBase + bitmapBits - 1;
    const uint64_t maxRcvPsn =
        UbTpPsnSequence::UnwrapAtOrAfter(saetph.GetMaxRcvPsn(), ackBase);
    const uint64_t lastCandidate = std::min<uint64_t>(maxRcvPsn, representedEnd);
    const bool baseIsMissingEvidence = ackBase == 0 && !saetph.GetBitmapBit(0);
    const uint64_t firstCandidate = baseIsMissingEvidence ? 0 : ackBase + 1;
    std::vector<uint64_t> missingPsns;

    for (uint64_t psn = firstCandidate; psn <= lastCandidate; ++psn) {
        const uint32_t offset = static_cast<uint32_t>(psn - ackBase);
        if (!saetph.GetBitmapBit(offset)) {
            missingPsns.push_back(psn);
        }
        if (psn == UINT64_MAX) {
            break;
        }
    }
    return missingPsns;
}

bool
UbSelectiveRetransStrategy::QueueRetransmission(uint64_t psn)
{
    auto it = m_sentPsnState.find(psn);
    if (it == m_sentPsnState.end() || it->second.acknowledged || it->second.retransmitPending) {
        return false;
    }
    it->second.retransmitPending = true;
    m_selectiveRetransmitQ.push_back(psn);
    return true;
}

void
UbSelectiveRetransStrategy::CompactRetransmissionQueue()
{
    std::deque<uint64_t> liveQueue;
    for (uint64_t psn : m_selectiveRetransmitQ) {
        auto it = m_sentPsnState.find(psn);
        if (it != m_sentPsnState.end() && !it->second.acknowledged &&
            it->second.retransmitPending)
        {
            liveQueue.push_back(psn);
        }
    }
    m_selectiveRetransmitQ.swap(liveQueue);
}

bool
UbSelectiveRetransStrategy::HasPendingRetransmission() const
{
    for (uint64_t psn : m_selectiveRetransmitQ) {
        auto it = m_sentPsnState.find(psn);
        if (it != m_sentPsnState.end() && !it->second.acknowledged) {
            return true;
        }
    }
    return false;
}

bool
UbSelectiveRetransStrategy::CanSendRetransmission() const
{
    if (m_controller.GetRetransmissionMode() != UbRetransmissionMode::SELECTIVE) {
        return false;
    }
    if (!HasPendingRetransmission()) {
        return false;
    }
    return !IsMarkPsnEnabled() || m_markPsnRetransPhase;
}

uint32_t
UbSelectiveRetransStrategy::GetNextRetransmissionSize() const
{
    for (uint64_t psn : m_selectiveRetransmitQ) {
        auto it = m_sentPsnState.find(psn);
        if (it != m_sentPsnState.end() && !it->second.acknowledged) {
            return it->second.descriptor.serializedSizeBytes;
        }
    }
    return 0;
}

uint32_t
UbSelectiveRetransStrategy::GetNextRetransmissionPayloadBytes() const
{
    for (uint64_t psn : m_selectiveRetransmitQ) {
        auto it = m_sentPsnState.find(psn);
        if (it != m_sentPsnState.end() && !it->second.acknowledged) {
            return it->second.descriptor.payloadBytes;
        }
    }
    return 0;
}

uint32_t
UbSelectiveRetransStrategy::GetRetainedPayloadBytes(uint64_t psn) const
{
    auto it = m_sentPsnState.find(psn);
    return it == m_sentPsnState.end() ? 0 : it->second.descriptor.payloadBytes;
}

bool
UbSelectiveRetransStrategy::HasOutstandingPackets() const
{
    if (m_controller.IsCollectingTimerStatsForTest())
    {
        uint64_t entriesVisited = 0;
        for (const auto& [psn, state] : m_sentPsnState)
        {
            (void)psn;
            ++entriesVisited;
            if (!state.acknowledged)
            {
                m_controller.RecordSelectiveOutstandingScanForTest(entriesVisited);
                return true;
            }
        }
        m_controller.RecordSelectiveOutstandingScanForTest(entriesVisited);
        return false;
    }
    return std::any_of(m_sentPsnState.begin(),
                       m_sentPsnState.end(),
                       [](const auto& entry) { return !entry.second.acknowledged; });
}

Ptr<Packet>
UbSelectiveRetransStrategy::TryGetNextRetransmissionPacket()
{
    UbTransportChannel& transport = m_controller.GetTransport();
    while (CanSendRetransmission()) {
        const uint64_t psn = m_selectiveRetransmitQ.front();
        auto it = m_sentPsnState.find(psn);
        if (it == m_sentPsnState.end() || it->second.acknowledged) {
            m_selectiveRetransmitQ.pop_front();
            if (IsMarkPsnEnabled()) {
                FinishMarkPsnRetransPhaseIfDone();
            }
            continue;
        }
        const UbGbnPacketDescriptor descriptor = it->second.descriptor;
        const uint32_t payloadBytes = descriptor.payloadBytes;
        if (transport.IsCcLimitedForRetransmission(payloadBytes)) {
            transport.SetSendWindowLimited(true);
            return nullptr;
        }
        m_selectiveRetransmitQ.pop_front();
        it->second.retransmitPending = false;
        if (IsMarkPsnEnabled() && it->second.retransmitCount == 0) {
            m_lastFirstRtxPsn = psn;
            m_lastFirstRtxPsnValid = true;
        }
        it->second.retransmitCount++;
        Ptr<Packet> retransmission = transport.SendSelectiveRetransmission(descriptor);
        if (IsMarkPsnEnabled()) {
            FinishMarkPsnRetransPhaseIfDone();
        }
        return retransmission;
    }
    if (IsMarkPsnEnabled()) {
        FinishMarkPsnRetransPhaseIfDone();
    }
    return nullptr;
}

UbRetransTimeoutResult
UbSelectiveRetransStrategy::OnTimeout()
{
    UbRetransTimeoutResult result;
    if (m_controller.GetRetransmissionMode() != UbRetransmissionMode::SELECTIVE) {
        return result;
    }

    EnterMarkPsnRetransPhase();
    for (auto& [psn, state] : m_sentPsnState) {
        if (!state.acknowledged && !state.retransmitPending) {
            state.retransmitPending = true;
            m_selectiveRetransmitQ.push_back(psn);
        }
    }
    result.triggerTransmit = true;
    return result;
}

UbRetransAckResult
UbSelectiveRetransStrategy::OnTransportResponse(const UbTransportHeader& tpHeader,
                                                TpOpcode opcode,
                                                const UbSelectiveAckExtTph& saetph,
                                                const UbCongestionExtTph* cetph)
{
    (void)opcode;
    UbRetransAckResult result;
    if (m_controller.GetRetransmissionMode() != UbRetransmissionMode::SELECTIVE) {
        return result;
    }

    UbTransportChannel& transport = m_controller.GetTransport();
    result.previousSndUna = transport.GetPsnSndUna();
    const uint64_t ackBaseLowerBound =
        result.previousSndUna == 0 ? 0 : result.previousSndUna - 1;
    const auto logicalAckBase =
        UbTpPsnSequence::UnwrapInWindow(tpHeader.GetPsn(),
                                        ackBaseLowerBound,
                                        transport.GetPsnSndNxt());
    if (!logicalAckBase.has_value())
    {
        result.newSndUna = result.previousSndUna;
        result.ignoreResponse = true;
        return result;
    }
    const uint64_t ackBase = *logicalAckBase;
    const std::vector<uint64_t> allMissingPsns = GetMissingPsnsFromSelectiveAck(ackBase, saetph);
    if (SelectiveAckReportsReceivedAtOrAboveMarkPsn(ackBase, saetph)) {
        EnterMarkPsnRetransPhase();
    }
    if (saetph.GetBitmapBit(0)) {
        AcknowledgeCumulativePsn(ackBase);
    }
    std::vector<uint64_t> missingPsns = CollectMissingPsnsFromSelectiveAck(ackBase, saetph);
    uint32_t retransmitBytes = 0;
    for (uint64_t psn : missingPsns) {
        retransmitBytes += GetRetainedPayloadBytes(psn);
    }
    if (cetph != nullptr) {
        transport.OnSenderReceivesTpsackCongestionFeedback(ackBase,
                                                           *cetph,
                                                           retransmitBytes);
    }
    bool queuedFastRetransmission = false;
    if (m_controller.GetFastRetransEnable()) {
        const std::vector<uint64_t>& candidatePsns = IsMarkPsnEnabled() ? allMissingPsns : missingPsns;
        for (uint64_t psn : candidatePsns) {
            if (IsMarkPsnEnabled() && !m_markPsnRetransPhase) {
                continue;
            }
            queuedFastRetransmission = QueueRetransmission(psn) || queuedFastRetransmission;
        }
    }
    AdvanceSendUnaFromAckState();
    RetireAckedStateBeforeSendUna();
    CompactRetransmissionQueue();
    result.newSndUna = transport.GetPsnSndUna();
    result.ackAdvanced = result.newSndUna > result.previousSndUna;
    result.triggerTransmit = queuedFastRetransmission && CanSendRetransmission() &&
                             !transport.IsCcLimitedForRetransmission(GetNextRetransmissionPayloadBytes());
    return result;
}

UbRetransAckResult
UbSelectiveRetransStrategy::OnCumulativeAck(const UbTransportHeader& tpHeader)
{
    UbRetransAckResult result;
    if (m_controller.GetRetransmissionMode() != UbRetransmissionMode::SELECTIVE) {
        return result;
    }

    UbTransportChannel& transport = m_controller.GetTransport();
    result.previousSndUna = transport.GetPsnSndUna();
    const uint64_t ackPsn = UbTpPsnSequence::Unwrap(tpHeader.GetPsn(), result.previousSndUna);
    AcknowledgeCumulativePsn(ackPsn);
    AdvanceSendUnaFromAckState();
    CompactRetransmissionQueue();
    result.newSndUna = transport.GetPsnSndUna();
    result.ackAdvanced = result.newSndUna > result.previousSndUna;
    return result;
}

bool
UbSelectiveRetransStrategy::ResolveSelectiveAckBitmapBits(uint32_t& bits) const
{
    const uint32_t configuredBits = m_controller.GetSelectiveAckBitmapBitsConfig();
    if (configuredBits != 0)
    {
        if (!UbSelectiveAckExtTph::IsSupportedBitmapBitCount(configuredBits))
        {
            return false;
        }
        bits = configuredBits;
        return true;
    }

    const uint32_t usefulWindow =
        std::min<uint32_t>(m_controller.GetTransport().GetPsnOooThresholdForRetrans(), 1024);
    if (usefulWindow == 0)
    {
        return false;
    }

    const std::array<uint32_t, 5> supportedBits = {64, 128, 256, 512, 1024};
    for (uint32_t candidate : supportedBits)
    {
        if (candidate >= usefulWindow)
        {
            bits = candidate;
            return true;
        }
    }

    return false;
}

uint32_t
UbSelectiveRetransStrategy::GetSelectiveAckBitmapBits() const
{
    uint32_t bits = 0;
    const bool resolved = ResolveSelectiveAckBitmapBits(bits);
    NS_ASSERT_MSG(resolved, "Invalid selective ACK bitmap configuration");
    return bits;
}

uint64_t
UbSelectiveRetransStrategy::GetSelectiveAckBase() const
{
    return m_controller.GetTransport().GetCumulativeAckPsnForRetrans();
}

UbSelectiveAckExtTph
UbSelectiveRetransStrategy::BuildSelectiveAckHeader(uint64_t ackBase) const
{
    const UbTransportChannel& transport = m_controller.GetTransport();
    UbSelectiveAckExtTph header;
    header.SetBitmapBitCount(GetSelectiveAckBitmapBits());
    header.SetMaxRcvPsn(UbTpPsnSequence::ToWire(transport.GetMaxRcvPsnForRetrans()));

    const uint32_t bitmapBits = header.GetBitmapBitCount();
    const uint64_t maxEvidencePsn =
        std::min<uint64_t>(transport.GetMaxRcvPsnForRetrans(), ackBase + bitmapBits - 1);
    for (uint64_t psn = ackBase; psn <= maxEvidencePsn; ++psn)
    {
        if (psn < transport.GetPsnRecvNxt() || transport.ReceiveWindowContainsForRetrans(psn))
        {
            header.SetBitmapBit(static_cast<uint32_t>(psn - ackBase), true);
        }
        if (psn == UINT64_MAX)
        {
            break;
        }
    }
    return header;
}

UbRetransReceiveDecision
UbSelectiveRetransStrategy::BuildReceiveDecisionForCurrentState() const
{
    const UbTransportChannel& transport = m_controller.GetTransport();
    UbRetransReceiveDecision decision;
    decision.shouldAck = true;

    if (!transport.HasReceiveGapForRetrans())
    {
        decision.responsePsn = transport.GetCumulativeAckPsnForRetrans();
        decision.responseOpcode = transport.GetResponseOpcodeForRetrans(false);
        return decision;
    }

    uint32_t selectiveAckBits = 0;
    decision.selectiveAck = true;
    decision.responsePsn = GetSelectiveAckBase();
    if (!ResolveSelectiveAckBitmapBits(selectiveAckBits))
    {
        decision.suppressResponse = true;
        return decision;
    }

    decision.responseOpcode = transport.GetResponseOpcodeForRetrans(true);
    decision.selectiveAckHeader = BuildSelectiveAckHeader(decision.responsePsn);
    return decision;
}

void
UbSelectiveRetransStrategy::OnNewDataPacketSent(const UbGbnPacketDescriptor& descriptor)
{
    if (IsMarkPsnEnabled()) {
        MaybeMarkFirstNewSelectivePacket(descriptor.logicalPsn);
    }
    RetainSentPsn(descriptor);
}

void
UbSelectiveRetransStrategy::RetireSegment(
    const UbTransactionCorrelationIdentity& correlation,
    uint64_t psnStart,
    uint64_t psnEnd)
{
    for (auto descriptor = m_sentPsnState.lower_bound(psnStart);
         descriptor != m_sentPsnState.end() && descriptor->first < psnEnd;)
    {
        if (descriptor->second.descriptor.hasCorrelation &&
            descriptor->second.descriptor.correlation == correlation)
        {
            descriptor = m_sentPsnState.erase(descriptor);
        }
        else
        {
            ++descriptor;
        }
    }
    CompactRetransmissionQueue();
}

void
UbSelectiveRetransStrategy::RetireAckedStateBeforeSendUna()
{
    const uint64_t sndUna = m_controller.GetTransport().GetPsnSndUna();
    for (auto it = m_sentPsnState.begin(); it != m_sentPsnState.end();) {
        if (it->first < sndUna && it->second.acknowledged) {
            it = m_sentPsnState.erase(it);
        } else {
            ++it;
        }
    }
}

bool
UbSelectiveRetransStrategy::IsMarkPsnEnabled() const
{
    return m_controller.GetSelectiveMarkPsnEnable() &&
           m_controller.GetRetransmissionMode() == UbRetransmissionMode::SELECTIVE &&
           m_controller.GetFastRetransEnable();
}

bool
UbSelectiveRetransStrategy::SelectiveAckReportsReceivedAtOrAboveMarkPsn(
    uint64_t ackBase,
    const UbSelectiveAckExtTph& saetph) const
{
    if (!IsMarkPsnEnabled() || !m_markPsnValid) {
        return false;
    }

    const uint32_t bitmapBits = saetph.GetBitmapBitCount();
    const uint64_t representedEnd = ackBase + bitmapBits - 1;
    const uint64_t maxRcvPsn =
        UbTpPsnSequence::UnwrapAtOrAfter(saetph.GetMaxRcvPsn(), ackBase);
    const uint64_t visibleEnd = std::min<uint64_t>(maxRcvPsn, representedEnd);

    for (uint64_t psn = ackBase; psn <= visibleEnd; ++psn) {
        const uint32_t offset = static_cast<uint32_t>(psn - ackBase);
        if (psn >= m_markPsn && saetph.GetBitmapBit(offset)) {
            return true;
        }
        if (psn == UINT64_MAX) {
            break;
        }
    }
    return false;
}

void
UbSelectiveRetransStrategy::EnterMarkPsnRetransPhase()
{
    if (!IsMarkPsnEnabled()) {
        return;
    }
    m_markPsnRetransPhase = true;
}

void
UbSelectiveRetransStrategy::FinishMarkPsnRetransPhaseIfDone()
{
    if (!IsMarkPsnEnabled() || !m_markPsnRetransPhase) {
        return;
    }
    if (HasPendingRetransmission()) {
        return;
    }
    m_markPsnRetransPhase = false;
    m_markPsnAwaitingFirstNew = true;
    m_markPsnValid = false;
}

void
UbSelectiveRetransStrategy::MaybeMarkFirstNewSelectivePacket(uint64_t psn)
{
    if (!IsMarkPsnEnabled() || !m_markPsnAwaitingFirstNew) {
        return;
    }
    m_markPsn = psn;
    m_markPsnValid = true;
    m_markPsnAwaitingFirstNew = false;
}

uint32_t
UbSelectiveRetransStrategy::GetPendingRetransmissionCountForTest() const
{
    uint32_t count = 0;
    for (uint64_t psn : m_selectiveRetransmitQ) {
        auto it = m_sentPsnState.find(psn);
        if (it != m_sentPsnState.end() && !it->second.acknowledged) {
            ++count;
        }
    }
    return count;
}

uint32_t
UbSelectiveRetransStrategy::GetRawRetransmissionQueueCountForTest() const
{
    return static_cast<uint32_t>(m_selectiveRetransmitQ.size());
}

bool
UbSelectiveRetransStrategy::WasPsnSelectivelyReportedMissingForTest(uint64_t psn) const
{
    auto it = m_sentPsnState.find(psn);
    return it != m_sentPsnState.end() && it->second.selectivelyReportedMissing;
}

bool
UbSelectiveRetransStrategy::HasRetainedPsnForTest(uint64_t psn) const
{
    return m_sentPsnState.find(psn) != m_sentPsnState.end();
}

uint32_t
UbSelectiveRetransStrategy::GetPsnRetransmitCountForTest(uint64_t psn) const
{
    auto it = m_sentPsnState.find(psn);
    return it == m_sentPsnState.end() ? 0 : it->second.retransmitCount;
}

void
UbSelectiveRetransStrategy::Clear()
{
    m_sentPsnState.clear();
    m_selectiveRetransmitQ.clear();
    m_markPsnRetransPhase = false;
    m_markPsnAwaitingFirstNew = true;
    m_markPsnValid = false;
    m_markPsn = 0;
    m_lastFirstRtxPsnValid = false;
    m_lastFirstRtxPsn = 0;
}

UbRetransController::UbRetransController(UbTransportChannel& transport)
    : m_transport(transport)
{
    m_gbn = std::make_unique<UbGbnRetransStrategy>(*this);
    m_selective = std::make_unique<UbSelectiveRetransStrategy>(*this);
}

UbRetransController::~UbRetransController()
{
    CancelTimer();
    ClearRetainedState();
}

void
UbRetransController::SetBaseRto(Time rto)
{
    m_baseRto = rto;
    m_rto = rto;
}

Time
UbRetransController::GetBaseRto() const
{
    return m_baseRto;
}

void
UbRetransController::SetCurrentRto(Time rto)
{
    m_rto = rto;
}

Time
UbRetransController::GetCurrentRto() const
{
    return m_rto;
}

void
UbRetransController::SetMaxRetransAttempts(uint16_t attempts)
{
    m_maxRetransAttempts = attempts;
    m_retransAttemptsLeft = attempts;
}

uint16_t
UbRetransController::GetMaxRetransAttempts() const
{
    return m_maxRetransAttempts;
}

void
UbRetransController::SetRetransAttemptsLeft(uint16_t attempts)
{
    m_retransAttemptsLeft = attempts;
}

uint16_t
UbRetransController::GetRetransAttemptsLeft() const
{
    return m_retransAttemptsLeft;
}

void
UbRetransController::SetRetransExponentFactor(uint16_t factor)
{
    m_retransExponentFactor = factor;
}

uint16_t
UbRetransController::GetRetransExponentFactor() const
{
    return m_retransExponentFactor;
}

void
UbRetransController::SetRetransTimeoutMode(UbRetransTimeoutMode mode)
{
    m_retransTimeoutMode = mode;
}

UbRetransTimeoutMode
UbRetransController::GetRetransTimeoutMode() const
{
    return m_retransTimeoutMode;
}

void
UbRetransController::SetRetransmissionMode(UbRetransmissionMode mode)
{
    m_retransmissionMode = mode;
}

UbRetransmissionMode
UbRetransController::GetRetransmissionMode() const
{
    return m_retransmissionMode;
}

void
UbRetransController::SetSelectiveAckBitmapBits(uint32_t bits)
{
    m_selectiveAckBitmapBits = bits;
}

uint32_t
UbRetransController::GetSelectiveAckBitmapBitsConfig() const
{
    return m_selectiveAckBitmapBits;
}

void
UbRetransController::SetFastRetransEnable(bool enable)
{
    m_enableFastRetrans = enable;
}

bool
UbRetransController::GetFastRetransEnable() const
{
    return m_enableFastRetrans;
}

void
UbRetransController::SetSelectiveMarkPsnEnable(bool enable)
{
    m_enableSelectiveMarkPsn = enable;
}

bool
UbRetransController::GetSelectiveMarkPsnEnable() const
{
    return m_enableSelectiveMarkPsn;
}

void
UbRetransController::SetRetransEnable(bool enable)
{
    m_enableRetrans = enable;
    if (!m_enableRetrans) {
        CancelTimer();
        ClearRetainedState();
    }
}

bool
UbRetransController::GetRetransEnable() const
{
    return m_enableRetrans;
}

void
UbRetransController::ScheduleTimeout()
{
    if (m_collectTimerTestStats)
    {
        ++m_timerTestStats.scheduleCalls;
    }
    const UbLifecycleResourceIdentity& owner = m_transport.GetLifecycleOwner();
    if (owner.generation == 0)
    {
        m_retransEvent =
            Simulator::Schedule(m_rto, &UbTransportChannel::ReTxTimeout, &m_transport);
        return;
    }
    if (!m_trackedEventsInitialized)
    {
        m_trackedEvents.Init(
            UbLifecycleCoordinator::GetOrCreate(NodeList::GetNode(m_transport.GetSrc())));
        m_trackedEventsInitialized = true;
    }
    const uint64_t token = m_trackedEvents.Track(owner);
    m_retransEvent =
        Simulator::Schedule(m_rto, &UbRetransController::HandleTrackedTimeout, this, token);
    m_trackedEvents.Attach(owner, token, m_retransEvent, {});
}

void
UbRetransController::HandleTrackedTimeout(uint64_t token)
{
    const UbLifecycleResourceIdentity& owner = m_transport.GetLifecycleOwner();
    if (!m_trackedEvents.Begin(owner, token))
    {
        return;
    }
    m_transport.ReTxTimeout();
    m_trackedEvents.Resolve(owner, token);
}

void
UbRetransController::StartTimerIfNeeded()
{
    if (m_collectTimerTestStats)
    {
        ++m_timerTestStats.startCalls;
    }
    if (!m_enableRetrans || !m_retransEvent.IsExpired())
    {
        return;
    }

    if (!HasOutstandingRetransmissionState())
    {
        return;
    }

    m_rto = m_baseRto;
    ScheduleTimeout();
}

void
UbRetransController::RestartTimerAfterAckProgress()
{
    if (m_collectTimerTestStats)
    {
        ++m_timerTestStats.restartCalls;
    }
    if (!m_enableRetrans)
    {
        return;
    }

    m_rto = m_baseRto;
    m_retransAttemptsLeft = m_maxRetransAttempts;
    CancelTimer();
    if (HasOutstandingRetransmissionState()) {
        ScheduleTimeout();
    }
}

void
UbRetransController::CancelTimer()
{
    if (m_collectTimerTestStats)
    {
        ++m_timerTestStats.cancelCalls;
    }
    m_retransEvent.Cancel();
    if (m_trackedEventsInitialized)
    {
        m_trackedEvents.CancelPending(m_transport.GetLifecycleOwner());
    }
}

UbRetransTimeoutResult
UbRetransController::OnTimeout()
{
    if (!m_enableRetrans)
    {
        return {};
    }

    m_retransAttemptsLeft--;
    if (m_retransTimeoutMode == UbRetransTimeoutMode::DYNAMIC) {
        uint64_t rto = m_rto.GetNanoSeconds();
        rto = rto << m_retransExponentFactor;
        m_rto = NanoSeconds(rto);
    } else {
        m_rto = m_baseRto;
    }
    NS_ASSERT_MSG(m_retransAttemptsLeft > 0, "Avaliable retransmission attempts exhausted.");
    ScheduleTimeout();
    if (m_retransmissionMode == UbRetransmissionMode::GBN) {
        return m_gbn->OnTimeout();
    }
    if (m_retransmissionMode == UbRetransmissionMode::SELECTIVE) {
        return m_selective->OnTimeout();
    }
    NS_ASSERT_MSG(false, "Unknown retransmission mode.");
    return {};
}

bool
UbRetransController::HasTimerRunning() const
{
    return !m_retransEvent.IsExpired();
}

bool
UbRetransController::HasOutstandingRetransmissionState() const
{
    if (!m_enableRetrans) {
        return false;
    }
    if (m_retransmissionMode == UbRetransmissionMode::GBN) {
        return m_gbn->HasOutstandingDescriptors();
    }
    if (m_retransmissionMode == UbRetransmissionMode::SELECTIVE) {
        return m_selective->HasOutstandingPackets();
    }
    return false;
}

UbRetransAckResult
UbRetransController::OnTransportNak(const UbTransportHeader& tph)
{
    UbRetransAckResult result;
    if (!m_enableRetrans) {
        result.ignoreResponse = true;
        return result;
    }
    if (m_retransmissionMode == UbRetransmissionMode::GBN) {
        const auto logicalNakPsn =
            UbTpPsnSequence::UnwrapInWindow(tph.GetPsn(),
                                            m_transport.GetPsnSndUna(),
                                            m_transport.GetPsnSndNxt());
        if (!logicalNakPsn.has_value()) {
            result.ignoreResponse = true;
            return result;
        }
        result.triggerTransmit = m_gbn->HandleTpNak(*logicalNakPsn);
        if (result.triggerTransmit) {
            result.retransmitBytes = m_gbn->GetRetransmissionPayloadBytesFrom(*logicalNakPsn);
        }
    }
    return result;
}

UbRetransAckResult
UbRetransController::OnTransportSelectiveAck(const UbTransportHeader& tph,
                                             TpOpcode opcode,
                                             const UbSelectiveAckExtTph& saetph,
                                             const UbCongestionExtTph* cetph)
{
    UbRetransAckResult result;
    if (!m_enableRetrans) {
        result.ignoreResponse = true;
        return result;
    }
    if (m_retransmissionMode == UbRetransmissionMode::SELECTIVE) {
        return m_selective->OnTransportResponse(tph, opcode, saetph, cetph);
    }
    if (m_retransmissionMode == UbRetransmissionMode::GBN) {
        NS_LOG_WARN("Dropping TPSACK in GBN retransmission mode.");
    }
    result.ignoreResponse = true;
    return result;
}

void
UbRetransController::OnSenderCumulativeAckProgress(uint64_t previousSndUna,
                                                   uint64_t newSndUna)
{
    if (!m_enableRetrans || newSndUna <= previousSndUna) {
        return;
    }
    if (m_retransmissionMode == UbRetransmissionMode::SELECTIVE) {
        m_selective->OnCumulativeAckProgress(previousSndUna, newSndUna);
    } else if (m_retransmissionMode == UbRetransmissionMode::GBN) {
        m_gbn->AcknowledgePrefix(newSndUna);
    }
}

UbRetransReceiveDecision
UbRetransController::OnDataPacketReceived(uint64_t psn)
{
    if (!m_enableRetrans)
    {
        return {};
    }

    if (m_retransmissionMode == UbRetransmissionMode::GBN) {
        return m_gbn->OnDataPacketReceived(psn);
    }
    return {};
}

bool
UbRetransController::ResolveSelectiveAckBitmapBits(uint32_t& bits) const
{
    return m_selective->ResolveSelectiveAckBitmapBits(bits);
}

UbRetransReceiveDecision
UbRetransController::BuildReceiveDecisionForCurrentState() const
{
    if (!m_enableRetrans)
    {
        UbRetransReceiveDecision decision;
        decision.shouldAck = true;
        decision.responsePsn = m_transport.GetCumulativeAckPsnForRetrans();
        decision.responseOpcode = m_transport.GetResponseOpcodeForRetrans(false);
        return decision;
    }

    if (m_retransmissionMode == UbRetransmissionMode::SELECTIVE)
    {
        return m_selective->BuildReceiveDecisionForCurrentState();
    }

    UbRetransReceiveDecision decision;
    decision.shouldAck = true;
    decision.responsePsn = m_transport.GetCumulativeAckPsnForRetrans();
    decision.responseOpcode = m_transport.GetResponseOpcodeForRetrans(false);
    return decision;
}

void
UbRetransController::OnNewDataPacketSent(const UbGbnPacketDescriptor& descriptor)
{
    if (!m_enableRetrans)
    {
        return;
    }

    if (m_retransmissionMode == UbRetransmissionMode::GBN) {
        m_gbn->RetainDescriptor(descriptor);
    } else if (m_retransmissionMode == UbRetransmissionMode::SELECTIVE) {
        m_selective->OnNewDataPacketSent(descriptor);
    }
}

Ptr<Packet>
UbRetransController::TryGetNextRetransmissionPacket()
{
    if (!m_enableRetrans)
    {
        return nullptr;
    }

    if (m_retransmissionMode == UbRetransmissionMode::GBN) {
        return m_gbn->TryGetNextRetransmissionPacket();
    }
    if (m_retransmissionMode == UbRetransmissionMode::SELECTIVE) {
        return m_selective->TryGetNextRetransmissionPacket();
    }
    return nullptr;
}

bool
UbRetransController::HasPendingGbnRetransmission() const
{
    return m_enableRetrans && m_retransmissionMode == UbRetransmissionMode::GBN &&
           m_gbn->HasPendingRetransmission();
}

uint32_t
UbRetransController::GetNextGbnRetransmissionSize() const
{
    return HasPendingGbnRetransmission() ? m_gbn->GetNextRetransmissionSize() : 0;
}

uint32_t
UbRetransController::GetNextGbnRetransmissionProgressBytes() const
{
    return HasPendingGbnRetransmission() ? m_gbn->GetNextRetransmissionProgressBytes() : 0;
}

uint32_t
UbRetransController::GetNextGbnRetransmissionPayloadBytes() const
{
    return HasPendingGbnRetransmission() ? m_gbn->GetNextRetransmissionPayloadBytes() : 0;
}

const UbGbnPacketDescriptor*
UbRetransController::GetNextGbnRetransmissionDescriptor() const
{
    return HasPendingGbnRetransmission() ? m_gbn->GetNextRetransmissionDescriptor() : nullptr;
}

void
UbRetransController::RetireGbnSegment(const UbTransactionCorrelationIdentity& correlation,
                                      uint64_t psnStart,
                                      uint64_t psnEnd)
{
    if (m_retransmissionMode != UbRetransmissionMode::GBN) {
        return;
    }
    m_gbn->RetireSegment(correlation, psnStart, psnEnd);
    if (!m_gbn->HasOutstandingDescriptors()) {
        CancelTimer();
    }
}

void
UbRetransController::RetireSegmentDescriptors(
    const UbTransactionCorrelationIdentity& correlation,
    uint64_t psnStart,
    uint64_t psnEnd)
{
    if (m_retransmissionMode == UbRetransmissionMode::GBN) {
        m_gbn->RetireSegment(correlation, psnStart, psnEnd);
    } else if (m_retransmissionMode == UbRetransmissionMode::SELECTIVE) {
        m_selective->RetireSegment(correlation, psnStart, psnEnd);
    }
    if (!HasOutstandingRetransmissionState()) {
        CancelTimer();
    }
}

bool
UbRetransController::HasPendingSelectiveRetransmission() const
{
    if (!m_enableRetrans)
    {
        return false;
    }

    return m_retransmissionMode == UbRetransmissionMode::SELECTIVE &&
           m_selective->HasPendingRetransmission();
}

bool
UbRetransController::CanSendSelectiveRetransmission() const
{
    if (!m_enableRetrans)
    {
        return false;
    }

    return m_retransmissionMode == UbRetransmissionMode::SELECTIVE &&
           m_selective->CanSendRetransmission();
}

uint32_t
UbRetransController::GetNextSelectiveRetransmissionSize() const
{
    if (!m_enableRetrans || m_retransmissionMode != UbRetransmissionMode::SELECTIVE) {
        return 0;
    }
    return m_selective->GetNextRetransmissionSize();
}

uint32_t
UbRetransController::GetNextSelectiveRetransmissionPayloadBytes() const
{
    if (!m_enableRetrans || m_retransmissionMode != UbRetransmissionMode::SELECTIVE) {
        return 0;
    }
    return m_selective->GetNextRetransmissionPayloadBytes();
}

uint32_t
UbRetransController::GetPendingSelectiveRetransmissionCountForTest() const
{
    return m_selective->GetPendingRetransmissionCountForTest();
}

uint32_t
UbRetransController::GetRawSelectiveRetransmissionQueueCountForTest() const
{
    return m_selective->GetRawRetransmissionQueueCountForTest();
}

bool
UbRetransController::WasPsnSelectivelyReportedMissingForTest(uint64_t psn) const
{
    return m_selective->WasPsnSelectivelyReportedMissingForTest(psn);
}

bool
UbRetransController::HasRetainedPsnForTest(uint64_t psn) const
{
    return m_selective->HasRetainedPsnForTest(psn);
}

uint32_t
UbRetransController::GetPsnRetransmitCountForTest(uint64_t psn) const
{
    return m_selective->GetPsnRetransmitCountForTest(psn);
}

uint32_t
UbRetransController::GetGbnDescriptorCountForTest() const
{
    return m_gbn->GetDescriptorCountForTest();
}

bool
UbRetransController::HasGbnDescriptorForTest(uint64_t psn) const
{
    return m_gbn->HasDescriptorForTest(psn);
}

uint64_t
UbRetransController::GetGbnRetransmissionCursorForTest() const
{
    return m_gbn->GetRetransmissionCursorForTest();
}

void
UbRetransController::RetainGbnDescriptorForTest(const UbGbnPacketDescriptor& descriptor)
{
    m_gbn->RetainDescriptor(descriptor);
}

void
UbRetransController::ResetTimerStatsForTest()
{
    m_collectTimerTestStats = true;
    m_timerTestStats = {};
}

UbRetransTimerTestStats
UbRetransController::GetTimerStatsForTest() const
{
    return m_timerTestStats;
}

bool
UbRetransController::IsCollectingTimerStatsForTest() const
{
    return m_collectTimerTestStats;
}

void
UbRetransController::RecordSelectiveOutstandingScanForTest(uint64_t entriesVisited) const
{
    ++m_timerTestStats.selectiveOutstandingScanCalls;
    m_timerTestStats.selectiveOutstandingEntriesVisited += entriesVisited;
}

void
UbRetransController::ClearRetainedState()
{
    if (m_gbn != nullptr) {
        m_gbn->Clear();
    }
    if (m_selective != nullptr) {
        m_selective->Clear();
    }
}

void
UbRetransController::ClearNakSuppressionIfGapClosed(uint64_t recvNext)
{
    m_gbn->ClearNakSuppressionIfGapClosed(recvNext);
}

UbTransportChannel&
UbRetransController::GetTransport()
{
    return m_transport;
}

const UbTransportChannel&
UbRetransController::GetTransport() const
{
    return m_transport;
}

} // namespace ns3
