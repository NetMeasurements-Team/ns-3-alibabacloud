#include <iostream>
#include <fstream>
#include "ns3/abort.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/object-vector.h"
#include "ns3/uinteger.h"
#include "ns3/log.h"
#include "ns3/assert.h"
#include "ns3/global-value.h"
#include "ns3/boolean.h"
#include "ns3/simulator.h"
#include "ns3/random-variable.h"
#include "switch-mmu.h"

NS_LOG_COMPONENT_DEFINE("SwitchMmu");
namespace ns3 {
	TypeId SwitchMmu::GetTypeId(void){
		static TypeId tid = TypeId("ns3::SwitchMmu")
			.SetParent<Object>()
			.AddConstructor<SwitchMmu>();
		return tid;
	}

	SwitchMmu::SwitchMmu(void){
		buffer_size = 12 * 1024 * 1024;
		reserve = 4 * 1024;
		resume_offset = 3 * 1024;

		// Initialize configuration before the frontend fills the per-port values.
		node_id = 0;
		total_hdrm = total_rsrv = 0;
		memset(headroom, 0, sizeof(headroom));
		memset(pfc_a_shift, 0, sizeof(pfc_a_shift));
		memset(kmin, 0, sizeof(kmin));
		memset(kmax, 0, sizeof(kmax));
		memset(pmax, 0, sizeof(pmax));

		// headroom
		shared_used_bytes = 0;
		memset(hdrm_bytes, 0, sizeof(hdrm_bytes));
		memset(ingress_bytes, 0, sizeof(ingress_bytes));
		memset(paused, 0, sizeof(paused));
		memset(egress_bytes, 0, sizeof(egress_bytes));
	}
	SwitchMmu::IngressRegion SwitchMmu::ClassifyIngress(uint32_t port, uint32_t qIndex, uint32_t psize){
		NS_ABORT_MSG_IF(port == 0 || port >= pCnt || qIndex == 0 || qIndex >= qCnt,
		                "Invalid MMU ingress port or data priority");
		// Validate the partition even when the packet fits in the reservation.
		const uint32_t threshold = GetPfcThreshold(port);
		const uint64_t next = uint64_t{ingress_bytes[port][qIndex]} + psize;
		if (next <= reserve || next - reserve <= threshold)
			return IngressRegion::Normal;
		if (uint64_t{hdrm_bytes[port][qIndex]} + psize <= headroom[port])
			return IngressRegion::Headroom;
		return IngressRegion::Rejected;
	}
	bool SwitchMmu::CheckIngressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize){
		if (ClassifyIngress(port, qIndex, psize) != IngressRegion::Rejected)
			return true;
		std::cout << "node " << node_id << ": dropping lossless packet (port " << port
		          << ", priority " << qIndex << "), headroom " << hdrm_bytes[port][qIndex]
		          << "/" << headroom[port] << ", packet " << psize << std::endl;
		return false;
	}
	bool SwitchMmu::CheckEgressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize){
		return true;
	}
	void SwitchMmu::UpdateIngressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize){
		const IngressRegion region = ClassifyIngress(port, qIndex, psize);
		NS_ABORT_MSG_IF(region == IngressRegion::Rejected,
		    "Accounting for a packet rejected by the MMU");
        if (region == IngressRegion::Headroom)
        {
            hdrm_bytes[port][qIndex] += psize;
        }
        else
        {
            const uint32_t old_shared = GetSharedUsed(port, qIndex);
            ingress_bytes[port][qIndex] += psize;
            shared_used_bytes += GetSharedUsed(port, qIndex) - old_shared;
        }
	}
	void SwitchMmu::UpdateEgressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize){
		egress_bytes[port][qIndex] += psize;
	}
	void SwitchMmu::RemoveFromIngressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize){
		NS_ABORT_MSG_IF(uint64_t{ingress_bytes[port][qIndex]} + hdrm_bytes[port][qIndex] < psize,
		                "Ingress accounting underflow");
		uint32_t from_hdrm = std::min(hdrm_bytes[port][qIndex], psize);
        uint32_t from_shared = std::min(
            psize - from_hdrm,
            ingress_bytes[port][qIndex] > reserve ? ingress_bytes[port][qIndex] - reserve : 0);
		NS_ABORT_MSG_IF(from_shared > shared_used_bytes, "Shared accounting underflow");
		// Aggregate accounting releases headroom first, then shared and reserved bytes.
		hdrm_bytes[port][qIndex] -= from_hdrm;
		ingress_bytes[port][qIndex] -= psize - from_hdrm;
		shared_used_bytes -= from_shared;
	}
	void SwitchMmu::RemoveFromEgressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize){
		NS_ABORT_MSG_IF(psize > egress_bytes[port][qIndex], "Egress accounting underflow");
		egress_bytes[port][qIndex] -= psize;
	}
	bool SwitchMmu::CheckShouldPause(uint32_t port, uint32_t qIndex){
		return !paused[port][qIndex] && (hdrm_bytes[port][qIndex] > 0 || GetSharedUsed(port, qIndex) >= GetPfcThreshold(port));
	}
	bool SwitchMmu::CheckShouldResume(uint32_t port, uint32_t qIndex){
		if (!paused[port][qIndex])
			return false;
		uint32_t shared_used = GetSharedUsed(port, qIndex);
        return hdrm_bytes[port][qIndex] == 0 &&
               (shared_used == 0 || uint64_t{shared_used} + resume_offset <= GetPfcThreshold(port));
	}
	void SwitchMmu::SetPause(uint32_t port, uint32_t qIndex){
		paused[port][qIndex] = true;
	}
	void SwitchMmu::SetResume(uint32_t port, uint32_t qIndex){
		paused[port][qIndex] = false;
	}
    uint32_t SwitchMmu::GetPfcThreshold(uint32_t port){
        NS_ABORT_MSG_IF(port == 0 || port >= pCnt, "Invalid MMU port");

        const uint64_t fixed = total_hdrm + total_rsrv;
        NS_ABORT_MSG_IF(fixed >= buffer_size, "Headroom and reservations leave no shared buffer");

        const uint64_t shared_capacity = uint64_t{buffer_size} - fixed;
        NS_ABORT_MSG_IF(shared_used_bytes > shared_capacity,
                        "Shared-buffer accounting exceeds capacity");
        NS_ABORT_MSG_IF(pfc_a_shift[port] >= 32, "Invalid PFC threshold shift");

        return static_cast<uint32_t>((shared_capacity - shared_used_bytes) >> pfc_a_shift[port]);
    }
	uint32_t SwitchMmu::GetSharedUsed(uint32_t port, uint32_t qIndex){
		uint32_t used = ingress_bytes[port][qIndex];
		return used > reserve ? used - reserve : 0;
	}
	bool SwitchMmu::ShouldSendCN(uint32_t ifindex, uint32_t qIndex){
		if (qIndex == 0)
			return false;
		if (egress_bytes[ifindex][qIndex] > kmax[ifindex])
			return true;
		if (egress_bytes[ifindex][qIndex] > kmin[ifindex])
        {
            const double p = pmax[ifindex] *
                             static_cast<double>(egress_bytes[ifindex][qIndex] - kmin[ifindex]) /
                             (kmax[ifindex] - kmin[ifindex]);
			if (UniformVariable(0, 1).GetValue() < p)
				return true;
		}
		return false;
	}
	void SwitchMmu::ConfigEcn(uint32_t port, uint32_t _kmin, uint32_t _kmax, double _pmax){
		kmin[port] = _kmin * 1000;
		kmax[port] = _kmax * 1000;
		pmax[port] = _pmax;
	}
	void SwitchMmu::ConfigHdrm(uint32_t port, uint32_t size){
		NS_ABORT_MSG_IF(port == 0 || port >= pCnt, "Invalid headroom port");
		headroom[port] = size;
	}
	void SwitchMmu::ConfigNPort(uint32_t n_port){
		NS_ABORT_MSG_IF(n_port >= pCnt, "Too many MMU ports");
		total_hdrm = 0;
		total_rsrv = 0;
		for (uint32_t i = 1; i <= n_port; i++){
			total_hdrm += headroom[i];
			total_rsrv += reserve;
		}
	}
	void SwitchMmu::ConfigBufferSize(uint32_t size){
		NS_ABORT_MSG_IF(total_hdrm + total_rsrv >= size,
		    "Headroom and reservations leave no shared buffer");
		NS_ABORT_MSG_IF(shared_used_bytes > size - total_hdrm - total_rsrv,
		    "Shared-buffer accounting exceeds capacity");
		buffer_size = size;
	}
}
