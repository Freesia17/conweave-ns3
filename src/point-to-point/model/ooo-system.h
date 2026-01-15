#ifndef OOO_SYSTEM_H
#define OOO_SYSTEM_H

#include "ns3/packet.h"
#include "ns3/ptr.h"
#include "ns3/custom-header.h"
#include "ns3/callback.h"

#include <memory>

namespace ns3 {

class OooSystemAdapter {
public:
  struct Config {
    // Table/queue sizing (match ooo_module defaults)
    uint32_t burst_table_volume = 30;
    uint32_t stable_table_volume = 32768;
    uint32_t blockqueue_pool_size = 4096;
    uint32_t big_flow_threshold = 16;

    // Latency parameters (ns)
    uint64_t pcie_latency_ns = 500;
    uint64_t slowpath_process_ns = 20018;  // 18 + 20000
    uint64_t slowpath_process_cycle_ns = 18;
    uint64_t slowpath_rule_latency_ns = 5000;

    // Periodic maintenance (ns)
    uint64_t stable_flush_interval = 10000000;
    uint64_t cms_clear_interval = 2500000;
    uint64_t bloom_clear_interval = 2500000;

    // FlowTracker data structure sizing
    uint32_t flowtracker_cms_width = 131072;
    uint32_t flowtracker_cms_depth = 4;
    uint32_t flowtracker_bloom_size = 131072;
    uint32_t flowtracker_bloom_hash = 4;

    // Slowpath buffer sizing and thresholds
    uint32_t slowpath_buffer_size = 4096;
    uint32_t slowpath_cold_threshold = 16;
    uint32_t slowpath_not_hot_threshold = 40;
  };

  OooSystemAdapter();
  explicit OooSystemAdapter(const Config &cfg);

  void SetConfig(const Config &cfg);
  void SetUdpHandler(Callback<void, Ptr<Packet>, const CustomHeader &> cb);

  // Entry point for UDP packets from RdmaHw::Receive.
  void HandleUdp(Ptr<Packet> p, const CustomHeader &ch);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace ns3

#endif // OOO_SYSTEM_H
