#ifndef SWITCH_NODE_H
#define SWITCH_NODE_H

#include <unordered_map>
#include <vector>
#include <ns3/node.h>
#include "qbb-net-device.h"
#include "switch-mmu.h"
#include "pint.h"
#include "ns3/random-variable-stream.h"

namespace ns3 {

class Packet;

class SwitchNode : public Node{
	static const uint32_t pCnt = 1025;	// Number of ports used
	static const uint32_t qCnt = 8;	// Number of queues/priorities used
	uint32_t m_ecmpSeed;
	Ptr<UniformRandomVariable> m_rand;
	std::unordered_map<uint32_t, std::vector<int> > m_rtTable; // map from ip address (u32) to possible ECMP port (index of dev)
	std::unordered_map<uint32_t, uint32_t> m_srNextHop; // map from neighbor node id to local egress port (index of dev), used for source routing
	std::set<uint32_t> active_ports;	// record active ports in switch

	// monitor of PFC
	uint32_t m_bytes[pCnt][pCnt][qCnt]; // m_bytes[inDev][outDev][qidx] is the bytes from inDev enqueued for outDev at qidx
	
	uint64_t m_txBytes[pCnt]; // counter of tx bytes

	uint32_t m_lastPktSize[pCnt];
	uint64_t m_lastPktTs[pCnt]; // ns
	double m_u[pCnt];

protected:
	bool m_ecnEnabled;
    bool m_pfcEnabled;
    bool m_packetSpraying;
    bool m_sourceRouting;
	uint32_t m_ccMode;
	uint64_t m_maxRtt;

	uint32_t m_ackHighPrio; // set high priority for ACK/NACK
    double m_forwardDelay;

	// Telemetry flow-index counters (claude_reports/TELEMETRY_FLOW_INDEX_PLAN.md).
	// The whole point of the per-switch dense index: this switch allocates
	// exactly N_s counters and addresses them directly with the telemetry
	// field for its own position in the path, which is idx[srh.ptr] -- no
	// lookup, no per-flow state, no hashing.
	bool m_telemetryCounting;
	std::vector<uint64_t> m_telemetryCounters;       // one per flow index at this switch
	uint64_t m_telemetryNoIndex;      // header present but NO_INDEX for this hop
	uint64_t m_telemetryOutOfRange;   // index >= N_s: flow list and topology disagree
	uint64_t m_telemetryNoHeader;     // source-routed packet carrying no telemetry header
	uint64_t m_srPtrOutOfRange;       // SRH pointer past the segment list: dropped, not forwarded
	uint64_t m_srSelfMismatch;        // segs[ptr-1] != this switch's own id: header describes a different route

private:
	int GetOutDev(Ptr<const Packet>, CustomHeader &ch);
	void SendToDev(Ptr<Packet>p, CustomHeader &ch);
	void CountTelemetry(CustomHeader &ch);
	static uint32_t EcmpHash(const uint8_t* key, size_t len, uint32_t seed);
	void CheckAndSendPfc(uint32_t inDev, uint32_t qIndex);
	void CheckAndSendResume(uint32_t inDev, uint32_t qIndex);
public:
	Ptr<SwitchMmu> m_mmu;

	static TypeId GetTypeId (void);
	SwitchNode();
	void SetEcmpSeed(uint32_t seed);
	void AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx);
	void AddSrNextHopEntry(uint32_t neighborNodeId, uint32_t intf_idx);
	void ClearTable();
	void SetTelemetryCounterCount(uint32_t n);
	const std::vector<uint64_t>& GetTelemetryCounters() const { return m_telemetryCounters; }
	uint64_t GetTelemetryNoIndex() const { return m_telemetryNoIndex; }
	uint64_t GetTelemetryOutOfRange() const { return m_telemetryOutOfRange; }
	uint64_t GetTelemetryNoHeader() const { return m_telemetryNoHeader; }
	uint64_t GetSrPtrOutOfRange() const { return m_srPtrOutOfRange; }
	uint64_t GetSrSelfMismatch() const { return m_srSelfMismatch; }
	bool SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader &ch);
	void SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p);

	// for approximate calc in PINT
	int logres_shift(int b, int l);
	int log2apprx(int x, int b, int m, int l); // given x of at most b bits, use most significant m bits of x, calc the result in l bits
	
	// for monitor
	uint64_t last_txBytes[pCnt]; // last sampling of the counter of tx bytes
	uint64_t last_port_qlen[pCnt]; // last sampling of the port length
	
	/**
	 * outoput format:
	 * time, sw_id, port_id, q_id, qlen, port_len
	 */
	void PrintSwitchQlen(FILE* qlen_output);
	/**
	 * outoput format:
	 * time, sw_id, port_id, txBytes
	 */
	void PrintSwitchBw(FILE* bw_output, uint32_t bw_mon_interval);
};

} /* namespace ns3 */

#endif /* SWITCH_NODE_H */
