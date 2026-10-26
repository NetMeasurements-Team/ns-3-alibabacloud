#ifndef RDMA_QUEUE_PAIR_H
#define RDMA_QUEUE_PAIR_H

#include <ns3/data-rate.h>
#include <ns3/event-id.h>
#include <ns3/int-header.h>
#include <ns3/ipv4-address.h>
#include <ns3/object.h>
#include <ns3/packet.h>

#include <deque>
#include <map>
#include <mutex>
#include <queue>
#include <vector>

namespace ns3
{
class RdmaCongestionOps;

class RdmaQueuePair : public Object
{
  public:
    Time startTime;
    Ipv4Address sip, dip;
    uint16_t sport, dport;
    uint32_t m_src, m_dest;
    uint64_t snd_nxt, snd_una; // next seq to send, the highest unacked seq
    uint16_t m_pg;
    uint16_t m_ipid;
    uint32_t m_win;      // bound of on-the-fly packets
    uint64_t m_baseRtt;  // base RTT of this qp
    DataRate m_max_rate; // max rate
    bool m_var_win;      // variable window size
    Time m_nextAvail;    //< Soonest time of next send
    uint32_t wp;         // current window of packets
    uint32_t lastPktSize;
    Callback<void> m_notifyAppFinish;
    Callback<void> m_notifyAppSent;
    /******************************
     * runtime states
     *****************************/
    uint32_t nvls_enable;
    DataRate m_rate; //< Current rate
    Ptr<RdmaCongestionOps> m_congestionControl;
    EventId m_rtoEvent;

    /***********
     * methods
     **********/
    static TypeId GetTypeId(void);
    RdmaQueuePair(uint16_t pg,
                  Ipv4Address _sip,
                  Ipv4Address _dip,
                  uint16_t _sport,
                  uint16_t _dport);
    void SetWin(uint32_t win);
    void SetBaseRtt(uint64_t baseRtt);
    void SetVarWin(bool v);
    void SetAppNotifyCallback(Callback<void> notifyAppFinish);
    void SetAppSentCallback(Callback<void> notifyAppSent);

    /**
     * \brief Bytes left to send from snd_nxt to the end of ALL queued messages
     * (pending and in-flight). Spans message boundaries; GetNxtPacket caps it.
     */
    uint64_t GetBytesLeft();
    uint32_t GetSrc();
    uint32_t GetDest();
    void SetSrc(uint32_t src);
    void SetDest(uint32_t dest);
    uint32_t GetHash(void);
    /**
     * \brief Cumulative ACK. Returns true if snd_una advanced (used to re-arm the RTO).
     */
    bool Acknowledge(uint64_t ack);
    uint64_t GetOnTheFly();
    bool IsWinBound();
    /**
     * \brief window size calculated from m_rate
     */
    uint64_t GetWin();
    /**
     * \brief Check if there is no more message to send AND no message in flight
     */
    bool IsFinished();
    void UpdateRate();

    /***********
     * tokens
     **********/
    class TokenBucket {
    public:
        int64_t m_tokens; // bytes
        int64_t m_tokensMax; // bytes
        Time m_lastTokenStamp;
        void Update(Time now, DataRate rate) {
            if (m_tokens < m_tokensMax) {
                int64_t delta = std::round((now - m_lastTokenStamp).GetSeconds() *
                    rate.GetBitRate() / 8); // bytes
                m_tokens = std::min(m_tokens + delta, m_tokensMax);
                m_lastTokenStamp = now;
            }
        }
        void Init(int64_t initToken, Time now, int64_t tokensMax){
            m_tokens = initToken;
            m_lastTokenStamp = now;
            m_tokensMax = tokensMax;
        }
        bool hasEnoughTokens(uint32_t bytes) {
            return m_tokens >= bytes;
        }
        void consumeTokens(uint32_t bytes) {
            m_tokens -= bytes; // token could be negative
            // this bucket will consume tokens only when hasEnoughTokens(min(TOKEN_PER_ROUND, lastPacketSize))
        }
    };
    TokenBucket m_tokenBucket;

    /***********
     * messages - dual queue system for pipelining (continuous flow)
     **********/
    class RdmaMessage {
    public:
        uint64_t m_size;
        uint64_t m_startSeq;
        uint64_t m_flow_id;
        uint64_t m_tag;
        /** true when all packets have been transmitted (reset on recovery) */
        bool m_sent = false;
        /** prevents triggering the SendComplete callback multiple times in case of retransmissions */
        bool m_appNotified = false;
        Callback<void> m_notifyAppFinish;
        Callback<void> m_notifyAppSent;
    };

    /**
     * \brief Messages that are being sent or waiting to be sent.
     * Front message is the one currently being transmitted.
     * Each message has m_startSeq calculated at push time.
     */
    std::deque<RdmaMessage> m_pendingMessages;

    /**
     * \brief Messages that have been fully sent but not yet ACKed.
     * Kept for triggering the finish callback when ACKed and for retransmission
     * on NACK/RTO. Ordered by sequence number (oldest first).
     */
    std::deque<RdmaMessage> m_inFlightMessages;

    std::mutex m_mutex;
    Time m_lastMessageStartTime;

    /**
     * \brief Push a new message to the pending queue. Its start sequence is the
     * end of the last pending/in-flight message, so messages are back to back.
     */
    void PushMessage(uint64_t size,
                     uint64_t flow_id,
                     uint64_t tag,
                     Callback<void> notifyAppFinish,
                     Callback<void> notifyAppSent);

    /**
     * \brief Called when the last packet of the front pending message is sent.
     * Moves the message from the pending to the in-flight queue.
     * \return The message that was moved (for callback purposes)
     */
    RdmaMessage MoveCurrentToInFlight();

    /**
     * \brief Called when the oldest in-flight message is fully ACKed.
     * Removes it from the in-flight queue and triggers its finish callback.
     */
    void FinishMessage();

    /**
     * \brief True if the front pending message has been fully sent
     * (snd_nxt >= m_startSeq + m_size).
     */
    bool IsCurMessageFullySent();

    /**
     * \brief True if the oldest in-flight message is fully ACKed
     * (snd_una >= m_startSeq + m_size).
     */
    bool IsOldestInFlightAcked();

    /**
     * \brief End sequence number over all pending and in-flight messages.
     */
    uint64_t GetTotalEndSeq();
};

class RdmaRxQueuePair : public Object
{ // Rx side queue pair
  public:
    struct ECNAccount
    {
        uint16_t qIndex;
        uint8_t ecnbits;
        uint16_t qfb;
        uint16_t total;

        ECNAccount()
        {
            memset(this, 0, sizeof(ECNAccount));
        }
    };

    ECNAccount m_ecn_source;
    uint32_t sip, dip;
    uint16_t sport, dport;
    uint16_t m_ipid;
    uint32_t m_win;
    bool m_ooo;
    uint64_t ReceiverNextExpectedSeq;
    std::map<uint32_t, size_t> m_outOfSeqPackets;
    Time m_nackTimer;
    int32_t m_milestone_rx;
    uint32_t m_lastNACK;
    EventId QcnTimerEvent; // if destroy this rxQp, remember to cancel this timer

    class RdmaMessage {
    public:
        uint64_t m_size;
        uint64_t m_startSeq;
        uint64_t m_flow_id;
        uint64_t m_tag;
    };
    std::queue<RdmaMessage> m_messages;

    static TypeId GetTypeId(void);
    RdmaRxQueuePair();
    void SetTag(uint64_t tag);
    void PushMessage(uint64_t size, uint64_t flow_id, uint64_t tag);
    uint32_t GetHash(void);
};

class RdmaQueuePairGroup : public Object
{
  public:
    std::vector<Ptr<RdmaQueuePair>> m_qps;
    // std::vector<Ptr<RdmaRxQueuePair> > m_rxQps;

    static TypeId GetTypeId(void);
    RdmaQueuePairGroup(void);
    uint32_t GetN(void);
    Ptr<RdmaQueuePair> Get(uint32_t idx);
    Ptr<RdmaQueuePair> operator[](uint32_t idx);
    void AddQp(Ptr<RdmaQueuePair> qp);
    void RemoveQp(Ptr<RdmaQueuePair> qp);
    // void AddRxQp(Ptr<RdmaRxQueuePair> rxQp);
    /**
     * \warning This function itself is not thread safe. Using it within ExplicitCriticalSection.
     * \param rr_last the last rr index. It will be updated to the correct position after removing.
     */
    void RemoveFinishedQps(uint32_t &rr_last);
    void Clear(void);
};

} // namespace ns3

#endif /* RDMA_QUEUE_PAIR_H */