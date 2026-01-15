#include "ooo-system.h"

#include "ns3/assert.h"
#include "ns3/flow-id-num-tag.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("OooSystemAdapter");

namespace {

// Internal enums mirror ooo_module utilities but keep this file self-contained.
enum class UpdateState { IDLE, SUCCESS, FAIL };
enum class UpdateType { IDLE, INSERT, DELETE, REPLACE };
enum class BlockUpdate { ALLOCATE, UNBLOCK };

// Hash UDP five-tuple into a stable, non-negative flow_id.
int32_t FiveTupleFlowId(const CustomHeader &ch) {
  uint64_t hash = 1469598103934665603ULL;  // FNV-1a 64-bit offset basis
  auto mix = [&hash](uint64_t v) {
    hash ^= v;
    hash *= 1099511628211ULL;  // FNV-1a prime
  };

  mix(static_cast<uint64_t>(ch.sip));
  mix(static_cast<uint64_t>(ch.dip));
  mix(static_cast<uint64_t>(ch.udp.sport));
  mix(static_cast<uint64_t>(ch.udp.dport));
  mix(static_cast<uint64_t>(ch.l3Prot));

  uint32_t folded = static_cast<uint32_t>(hash ^ (hash >> 32));
  return static_cast<int32_t>(folded & 0x7fffffff);
}

struct PacketCtx {
  Ptr<Packet> pkt;
  CustomHeader ch;
  int32_t flow_id = -1;
  int32_t burst_table_index = -1;
  int64_t packet_id = -1;
};

class CountMinSketch {
 public:
  CountMinSketch() : d_(1), w_(1), table_(1, 0), seeds_(1, 0) {}

  // Count-Min Sketch used for CMS and Bloom-filter-like counting.
  CountMinSketch(std::size_t d, std::size_t w, uint64_t global_seed = 0)
      : d_(d), w_(w), table_(d * w, 0), seeds_(d) {
    NS_ABORT_MSG_IF(d_ == 0 || w_ == 0, "CountMinSketch size must be > 0");
    if (global_seed == 0) {
      std::random_device rd;
      global_seed = (static_cast<uint64_t>(rd()) << 32) ^ rd();
    }
    std::mt19937_64 gen(global_seed);
    for (std::size_t i = 0; i < d_; ++i) {
      seeds_[i] = gen();
    }
  }

  void Reset() { std::fill(table_.begin(), table_.end(), 0); }

  uint64_t UpdateAndQuery(uint64_t key, uint64_t count = 1) {
    uint64_t res = UINT64_MAX;
    for (std::size_t i = 0; i < d_; ++i) {
      std::size_t col = std::hash<uint64_t>{}(key ^ seeds_[i]) % w_;
      std::size_t idx = i * w_ + col;
      if (UINT64_MAX - table_[idx] < count) {
        table_[idx] = UINT64_MAX;
      } else {
        table_[idx] += count;
      }
      res = std::min(res, table_[idx]);
    }
    return res;
  }

 private:
  std::size_t d_;
  std::size_t w_;
  std::vector<uint64_t> table_;
  std::vector<uint64_t> seeds_;
};

class RuleAllocation {
 public:
  RuleAllocation() : slots_() {}
  explicit RuleAllocation(std::size_t total) : slots_(total, false) {}

  // Simple free-list allocator for BurstTable indices.
  void Reset(std::size_t total) { slots_.assign(total, false); }

  int Allocate() {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (!slots_[i]) {
        slots_[i] = true;
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  void Release(int index) {
    if (index >= 0 && static_cast<std::size_t>(index) < slots_.size()) {
      slots_[index] = false;
    }
  }

  std::size_t Size() const { return slots_.size(); }

 private:
  std::vector<bool> slots_;
};

struct Rule {
  int update_entry = -1;
  int burst_table_idx = -1;
  int replace_entry = -1;
  bool ready = false;
  uint64_t ready_time_ns = 0;
};

struct RuleUpdate {
  UpdateType type = UpdateType::IDLE;
  int update_entry = -1;
  int replace_entry = -1;
  int unblock_index = -1;
};

}  // namespace

struct OooSystemAdapter::Impl {
  explicit Impl(const Config &cfg_in)
      : cfg(cfg_in),
        burst(this),
        blockq(this),
        stable(this),
        flow(this),
        rule_updater(this),
        slow(this) {
    ResetAll();
  }

  void SetConfig(const Config &cfg_in) {
    cfg = cfg_in;
    ResetAll();
  }

  void ResetAll() {
    burst.Reset();
    blockq.Reset();
    stable.Reset();
    flow.Reset();
    rule_updater.Reset();
    slow.Reset();

    if (stable_flush_event.IsRunning()) {
      stable_flush_event.Cancel();
    }
    if (cms_clear_event.IsRunning()) {
      cms_clear_event.Cancel();
    }
    if (bloom_clear_event.IsRunning()) {
      bloom_clear_event.Cancel();
    }

    ScheduleStableFlush();
    ScheduleCmsClear();
    ScheduleBloomClear();
  }

  void HandleUdp(Ptr<Packet> p, const CustomHeader &ch) {
    if (udpHandler.IsNull()) {
      return;
    }

    // Derive flow_id from five-tuple instead of relying on tags.
    int32_t flow_id = FiveTupleFlowId(ch);

    if (flow_id < 0) {
      Simulator::ScheduleNow(udpHandler, p, ch);
      return;
    }

    PacketCtx ctx;
    ctx.pkt = p;
    ctx.ch = ch;
    ctx.flow_id = flow_id;
    ctx.packet_id = static_cast<int64_t>(p->GetUid());
    ctx.burst_table_index = -1;

    int idx = burst.Lookup(flow_id);
    blockq.OnPacket(ctx, idx);
  }

  // Fast path: direct delivery.
  void ForwardFast(const PacketCtx &ctx) {
    if (udpHandler.IsNull() || !ctx.pkt) {
      return;
    }
    Simulator::ScheduleNow(udpHandler, ctx.pkt, ctx.ch);
  }

  // Slow path: PCIe latency before delivery.
  void ForwardSlow(const PacketCtx &ctx) {
    if (udpHandler.IsNull() || !ctx.pkt) {
      return;
    }
    Simulator::Schedule(NanoSeconds(cfg.pcie_latency_ns), udpHandler, ctx.pkt, ctx.ch);
  }

  void SendToSlowpath(const PacketCtx &ctx, bool big_flow) {
    Simulator::Schedule(NanoSeconds(cfg.pcie_latency_ns), &Impl::SlowpathInput, this, ctx,
                        big_flow);
  }

  void SlowpathInput(PacketCtx ctx, bool big_flow) { slow.OnPacket(ctx, big_flow); }

  void ScheduleStableFlush() {
    if (cfg.stable_flush_interval == 0) {
      return;
    }
    stable_flush_event =
        Simulator::Schedule(NanoSeconds(cfg.stable_flush_interval), &Impl::StableFlush, this);
  }

  void StableFlush() {
    std::map<int32_t, int32_t> snapshot = stable.FlushCounters();
    if (!snapshot.empty()) {
      Simulator::Schedule(NanoSeconds(cfg.pcie_latency_ns), &Impl::SlowpathCounters, this,
                          snapshot);
    }
    ScheduleStableFlush();
  }

  void SlowpathCounters(const std::map<int32_t, int32_t> &counters) {
    slow.OnCounters(counters);
  }

  void ScheduleCmsClear() {
    if (cfg.cms_clear_interval == 0) {
      return;
    }
    cms_clear_event =
        Simulator::Schedule(NanoSeconds(cfg.cms_clear_interval), &Impl::ClearCms, this);
  }

  void ClearCms() {
    flow.ClearCms();
    ScheduleCmsClear();
  }

  void ScheduleBloomClear() {
    if (cfg.bloom_clear_interval == 0) {
      return;
    }
    bloom_clear_event =
        Simulator::Schedule(NanoSeconds(cfg.bloom_clear_interval), &Impl::ClearBloom, this);
  }

  void ClearBloom() {
    flow.ClearBloom();
    ScheduleBloomClear();
  }

  void ScheduleUpdateState(UpdateState state) {
    Simulator::Schedule(NanoSeconds(cfg.pcie_latency_ns), &Impl::SlowpathUpdateState, this,
                        state);
  }

  void SlowpathUpdateState(UpdateState state) { slow.OnUpdateState(state); }

  void ScheduleRuleUpdate(const RuleUpdate &ru) {
    Simulator::Schedule(NanoSeconds(cfg.pcie_latency_ns), &Impl::RuleUpdaterInput, this, ru);
  }

  void RuleUpdaterInput(const RuleUpdate &ru) { rule_updater.OnRuleUpdate(ru); }

  struct BurstTableState {
    explicit BurstTableState(Impl *owner_in) : owner(owner_in) {}

    void Reset() {
      index_to_flow.assign(owner->cfg.burst_table_volume, -1);
      flow_to_index.clear();
    }

    int Lookup(int32_t flow_id) const {
      auto it = flow_to_index.find(flow_id);
      return it == flow_to_index.end() ? -1 : it->second;
    }

    void Insert(int32_t flow_id, int32_t index) {
      if (index < 0 ||
          static_cast<std::size_t>(index) >= index_to_flow.size() ||
          index_to_flow.empty()) {
        return;
      }
      if (flow_to_index.size() >= index_to_flow.size() &&
          flow_to_index.find(flow_id) == flow_to_index.end()) {
        return;
      }
      int prev_flow = index_to_flow[index];
      if (prev_flow >= 0) {
        flow_to_index.erase(prev_flow);
      }
      auto it = flow_to_index.find(flow_id);
      if (it != flow_to_index.end()) {
        index_to_flow[it->second] = -1;
      }
      flow_to_index[flow_id] = index;
      index_to_flow[index] = flow_id;
    }

    void DeleteByIndex(int32_t index) {
      if (index < 0 || static_cast<std::size_t>(index) >= index_to_flow.size()) {
        return;
      }
      int flow_id = index_to_flow[index];
      if (flow_id >= 0) {
        flow_to_index.erase(flow_id);
        index_to_flow[index] = -1;
      }
    }

    void ResetAll() { Reset(); }

    Impl *owner;
    std::vector<int32_t> index_to_flow;
    std::unordered_map<int32_t, int32_t> flow_to_index;
  };

  struct BlockQueueState {
    explicit BlockQueueState(Impl *owner_in) : owner(owner_in) {}

    void Reset() {
      queues.assign(owner->cfg.burst_table_volume, std::deque<PacketCtx>());
      blocking.assign(owner->cfg.burst_table_volume, false);
      released.assign(owner->cfg.burst_table_volume, false);
      active_indices.clear();
      clear_cursor = 0;
      rr_pointer = 0;
      drain_index = -1;
      pool_remaining = static_cast<int>(owner->cfg.blockqueue_pool_size);
      miss_queue.clear();
      pending_alloc.clear();
      pending_unblock.clear();
      process_scheduled = false;
      update_scheduled = false;
    }

    void OnPacket(const PacketCtx &ctx, int index) {
      if (index >= 0 && static_cast<std::size_t>(index) < queues.size()) {
        queues[index].push_back(ctx);
        if (queues[index].size() == owner->cfg.big_flow_threshold) {
          // Notify RuleUpdater when a queue reaches the big-flow threshold.
          owner->rule_updater.OnQueueExceed(index);
        }
      } else {
        miss_queue.push_back(ctx);
      }
      if (ctx.flow_id >= 0) {
        pool_remaining--;
      }
      MaybeOverflowReset();
      ScheduleProcess(0);
    }

    void OnUpdate(BlockUpdate type, int index) {
      if (type == BlockUpdate::ALLOCATE) {
        pending_alloc.push_back(index);
      } else {
        pending_unblock.push_back(index);
      }
      ScheduleUpdate();
    }

   private:
    void MaybeOverflowReset() {
      if (pool_remaining <= 1) {
        owner->burst.ResetAll();
        std::fill(blocking.begin(), blocking.end(), false);
      }
    }

    void ScheduleUpdate() {
      if (update_scheduled) {
        return;
      }
      update_scheduled = true;
      update_event = Simulator::ScheduleNow(&BlockQueueState::ProcessUpdate, this);
    }

    void ProcessUpdate() {
      update_scheduled = false;
      if (!pending_alloc.empty()) {
        int index = pending_alloc.front();
        pending_alloc.pop_front();
        ApplyAllocate(index);
      } else if (!pending_unblock.empty()) {
        int index = pending_unblock.front();
        pending_unblock.pop_front();
        ApplyUnblock(index);
      }
      if (!pending_alloc.empty() || !pending_unblock.empty()) {
        ScheduleUpdate();
      }
    }

    void ApplyAllocate(int index) {
      if (index < 0 || static_cast<std::size_t>(index) >= queues.size()) {
        return;
      }
      // Allocate -> block queue, and track for clear-signal emission.
      blocking[index] = true;
      released[index] = false;
      if (std::find(active_indices.begin(), active_indices.end(), index) == active_indices.end()) {
        active_indices.push_back(index);
        if (active_indices.size() == 1) {
          clear_cursor = 0;
        }
      }
    }

    void ApplyUnblock(int index) {
      if (index < 0 || static_cast<std::size_t>(index) >= queues.size()) {
        return;
      }
      // Unblock -> drain this queue exclusively until empty.
      blocking[index] = false;
      if (!process_scheduled) {
        drain_index = index;
      }
      if (queues[index].empty() && !released[index]) {
        ReleaseQueue(index);
      }
      ScheduleProcess(0);
    }

    void ReleaseQueue(int index) {
      if (index < 0 || static_cast<std::size_t>(index) >= queues.size()) {
        return;
      }
      if (released[index]) {
        return;
      }
      // Release frees a rule slot in FlowTracker.
      released[index] = true;
      owner->flow.Release(index);
      auto it = std::find(active_indices.begin(), active_indices.end(), index);
      if (it != active_indices.end()) {
        std::size_t pos = static_cast<std::size_t>(std::distance(active_indices.begin(), it));
        active_indices.erase(it);
        if (active_indices.empty()) {
          clear_cursor = 0;
        } else {
          if (pos < clear_cursor && clear_cursor > 0) {
            clear_cursor--;
          }
          if (clear_cursor >= active_indices.size()) {
            clear_cursor = 0;
          }
        }
      }
    }

    void ScheduleProcess(uint64_t delay_ns) {
      if (process_scheduled) {
        return;
      }
      process_scheduled = true;
      process_event =
          Simulator::Schedule(NanoSeconds(delay_ns), &BlockQueueState::ProcessOne, this);
    }

    bool HasUnblockedQueue() const {
      for (std::size_t i = 0; i < queues.size(); ++i) {
        if (!blocking[i] && !queues[i].empty()) {
          return true;
        }
      }
      return false;
    }

    void ProcessOne() {
      process_scheduled = false;

      PacketCtx out;
      bool have = false;
      int out_index = -2;  // >=0 queue, -1 miss, -2 clear

      if (drain_index >= 0 &&
          static_cast<std::size_t>(drain_index) < queues.size()) {
        if (!queues[drain_index].empty()) {
          out = queues[drain_index].front();
          queues[drain_index].pop_front();
          out_index = drain_index;
          have = true;
          if (queues[drain_index].empty()) {
            int finished = drain_index;
            drain_index = -1;
            if (!blocking[finished]) {
              ReleaseQueue(finished);
            }
          }
        } else {
          drain_index = -1;
        }
      }

      if (!have && !queues.empty()) {
        std::size_t n = queues.size();
        for (std::size_t i = 0; i < n; ++i) {
          std::size_t idx = (static_cast<std::size_t>(rr_pointer) + i) % n;
          if (!blocking[idx] && !queues[idx].empty()) {
            out = queues[idx].front();
            queues[idx].pop_front();
            out_index = static_cast<int>(idx);
            have = true;
            rr_pointer = static_cast<int>((idx + 1) % n);
            if (queues[idx].empty() && !blocking[idx]) {
              ReleaseQueue(static_cast<int>(idx));
            }
            break;
          }
        }
      }

      if (!have && !miss_queue.empty()) {
        out = miss_queue.front();
        miss_queue.pop_front();
        out_index = -1;
        have = true;
      }

      if (!have && !active_indices.empty()) {
        // Emit clear signal when all queues are blocked or miss queue empty.
        out.flow_id = -1;
        out.packet_id = -1;
        out.burst_table_index = active_indices[clear_cursor];
        clear_cursor = (clear_cursor + 1) % active_indices.size();
        out_index = -2;
        have = true;
      }

      if (!have) {
        return;
      }

      if (out_index >= -1 && out.flow_id >= 0) {
        pool_remaining++;
      }

      owner->stable.OnPacket(out);

      if (!have) {
        return;
      }

      if (drain_index >= 0 && static_cast<std::size_t>(drain_index) < queues.size() &&
          !queues[drain_index].empty()) {
        ScheduleProcess(1);
        return;
      }

      if (HasUnblockedQueue() || !miss_queue.empty() || !active_indices.empty()) {
        // Pace block-queue output to avoid zero-time loops.
        ScheduleProcess(1);
      }
    }

    Impl *owner;
    std::deque<PacketCtx> miss_queue;
    std::vector<std::deque<PacketCtx>> queues;
    std::vector<bool> blocking;
    std::vector<bool> released;
    std::deque<int> active_indices;
    std::size_t clear_cursor = 0;
    int rr_pointer = 0;
    int drain_index = -1;
    int pool_remaining = 0;

    std::deque<int> pending_alloc;
    std::deque<int> pending_unblock;
    bool update_scheduled = false;
    EventId update_event;

    bool process_scheduled = false;
    EventId process_event;
  };

  struct StableTableState {
    explicit StableTableState(Impl *owner_in) : owner(owner_in) {}

    void Reset() {
      table.clear();
      counters.clear();
    }

    void OnPacket(const PacketCtx &ctx) {
      if (ctx.flow_id < 0 && ctx.burst_table_index >= 0) {
        // Clear signals pass through as "miss" to FlowTracker.
        owner->flow.OnPacket(ctx);
        return;
      }
      if (ctx.flow_id >= 0 && table.find(ctx.flow_id) != table.end()) {
        counters[ctx.flow_id]++;
        owner->ForwardFast(ctx);
      } else {
        owner->flow.OnPacket(ctx);
      }
    }

    void ApplyUpdate(UpdateType type, int update_entry, int replace_entry) {
      // Update table/counters immediately; update_state is handled by RuleUpdater.
      switch (type) {
        case UpdateType::INSERT:
          if (table.size() < owner->cfg.stable_table_volume) {
            table.insert(update_entry);
            counters[update_entry] = 0;
          }
          break;
        case UpdateType::DELETE:
          table.erase(update_entry);
          counters.erase(update_entry);
          break;
        case UpdateType::REPLACE:
          table.erase(replace_entry);
          counters.erase(replace_entry);
          if (table.size() < owner->cfg.stable_table_volume) {
            table.insert(update_entry);
            counters[update_entry] = 0;
          }
          break;
        case UpdateType::IDLE:
        default:
          break;
      }
    }

    std::map<int32_t, int32_t> FlushCounters() {
      // Periodic counter snapshot to Slowpath.
      std::map<int32_t, int32_t> snapshot = counters;
      counters.clear();
      for (auto flow_id : table) {
        counters[flow_id] = 0;
      }
      return snapshot;
    }

    Impl *owner;
    std::set<int32_t> table;
    std::map<int32_t, int32_t> counters;
  };

  struct FlowTrackerState {
    explicit FlowTrackerState(Impl *owner_in) : owner(owner_in) {}

    void Reset() {
      cms = CountMinSketch(owner->cfg.flowtracker_cms_depth,
                           owner->cfg.flowtracker_cms_width);
      bloom = CountMinSketch(owner->cfg.flowtracker_bloom_hash,
                             owner->cfg.flowtracker_bloom_size);
      rule_alloc.Reset(owner->cfg.burst_table_volume);
    }

    void ClearCms() { cms.Reset(); }
    void ClearBloom() { bloom.Reset(); }

    void Release(int index) { rule_alloc.Release(index); }

    void OnPacket(PacketCtx ctx) {
      if (ctx.flow_id < 0) {
        owner->SendToSlowpath(ctx, false);
        return;
      }

      bool big_flow = false;
      bool insertion_valid = false;
      int insert_index = -1;

      uint64_t count = cms.UpdateAndQuery(static_cast<uint64_t>(ctx.flow_id));
      if (count >= owner->cfg.big_flow_threshold) {
        uint64_t bloom_count = bloom.UpdateAndQuery(static_cast<uint64_t>(ctx.flow_id));
        if (bloom_count == 1) {
          // First time Bloom says heavy -> allocate rule index.
          big_flow = true;
          insert_index = rule_alloc.Allocate();
          if (insert_index >= 0 &&
              static_cast<std::size_t>(insert_index) < rule_alloc.Size()) {
            insertion_valid = true;
            ctx.burst_table_index = insert_index;
          }
        }
      }

      if (insertion_valid) {
        // Insert into BurstTable and block the queue.
        owner->burst.Insert(ctx.flow_id, insert_index);
        owner->blockq.OnUpdate(BlockUpdate::ALLOCATE, insert_index);
      }

      owner->SendToSlowpath(ctx, big_flow);
    }

    Impl *owner;
    CountMinSketch cms;
    CountMinSketch bloom;
    RuleAllocation rule_alloc;
  };

  struct RuleUpdaterState {
    explicit RuleUpdaterState(Impl *owner_in) : owner(owner_in) {}

    void Reset() { pass_check.assign(owner->cfg.burst_table_volume, false); }

    void OnQueueExceed(int index) {
      if (index >= 0 && static_cast<std::size_t>(index) < pass_check.size()) {
        pass_check[index] = true;
      }
    }

    void OnRuleUpdate(const RuleUpdate &ru) {
      // Gate updates using pass_check (queue exceeded signal).
      bool allowed = false;
      if (ru.type != UpdateType::IDLE) {
        if (ru.unblock_index < 0) {
          allowed = true;
        } else if (ru.unblock_index >= 0 &&
                   static_cast<std::size_t>(ru.unblock_index) < pass_check.size() &&
                   pass_check[ru.unblock_index]) {
          allowed = true;
          pass_check[ru.unblock_index] = false;
        }
      }

      if (allowed) {
        owner->stable.ApplyUpdate(ru.type, ru.update_entry, ru.replace_entry);
        owner->ScheduleUpdateState(UpdateState::SUCCESS);
      } else if (ru.type != UpdateType::IDLE) {
        owner->ScheduleUpdateState(UpdateState::FAIL);
      }

      if (ru.unblock_index >= 0) {
        // Unblock always clears BurstTable index and unblocks queue.
        owner->burst.DeleteByIndex(ru.unblock_index);
        owner->blockq.OnUpdate(BlockUpdate::UNBLOCK, ru.unblock_index);
      }
    }

    Impl *owner;
    std::vector<bool> pass_check;
  };

  struct SlowpathState {
    explicit SlowpathState(Impl *owner_in) : owner(owner_in) {}

    void Reset() {
      buffer.clear();
      next_service_time_ns = 0;
      start_scheduled = false;
      burst_table.assign(owner->cfg.burst_table_volume, -1);
      rule_delay.clear();
      rules_on_the_way.clear();
      counters.clear();
      replaced_rules.clear();
      emit_scheduled = false;
      emit_time_ns = 0;
    }

    void OnPacket(const PacketCtx &ctx, bool big_flow) {
      if ((ctx.flow_id >= 0 &&
           buffer.size() < owner->cfg.slowpath_buffer_size) ||
          ctx.burst_table_index >= 0 || big_flow) {
        // Queue packet for processing.
        buffer.push_back(std::make_pair(ctx, big_flow));
        ScheduleStart();
      }
    }

    void OnCounters(const std::map<int32_t, int32_t> &incoming) {
      if (!incoming.empty()) {
        // Refresh counters and protect rules on the way.
        counters = incoming;
        for (const auto &rule : rules_on_the_way) {
          counters[rule.update_entry] = kLargeCount;
          if (rule.replace_entry >= 0) {
            auto it = counters.find(rule.replace_entry);
            if (it != counters.end()) {
              replaced_rules[rule.replace_entry] = it->second;
              counters.erase(it);
            }
          }
        }
      }
    }

    void OnUpdateState(UpdateState state) {
      if (rules_on_the_way.empty()) {
        return;
      }
      const Rule &front = rules_on_the_way.front();
      switch (state) {
        case UpdateState::SUCCESS:
          if (front.replace_entry >= 0) {
            replaced_rules.erase(front.replace_entry);
          }
          rules_on_the_way.pop_front();
          break;
        case UpdateState::FAIL:
          counters.erase(front.update_entry);
          if (front.replace_entry >= 0) {
            auto it = replaced_rules.find(front.replace_entry);
            if (it != replaced_rules.end()) {
              counters[front.replace_entry] = it->second;
              replaced_rules.erase(it);
            }
          }
          rules_on_the_way.pop_front();
          break;
        case UpdateState::IDLE:
        default:
          break;
      }
    }

   private:
    void ScheduleStart() {
      if (start_scheduled || buffer.empty()) {
        return;
      }
      uint64_t now = Simulator::Now().GetNanoSeconds();
      uint64_t start_time = std::max(now, next_service_time_ns);
      start_scheduled = true;
      start_event = Simulator::Schedule(NanoSeconds(start_time - now),
                                        &SlowpathState::StartProcess, this);
    }

    void StartProcess() {
      start_scheduled = false;
      if (buffer.empty()) {
        return;
      }
      auto item = buffer.front();
      buffer.pop_front();

      uint64_t now = Simulator::Now().GetNanoSeconds();
      uint64_t finish_time = now + owner->cfg.slowpath_process_ns;
      next_service_time_ns = now + owner->cfg.slowpath_process_cycle_ns;
      Simulator::Schedule(NanoSeconds(finish_time - now), &SlowpathState::FinishProcess, this,
                          item);
      ScheduleStart();
    }

    void FinishProcess(std::pair<PacketCtx, bool> item) {
      PacketCtx pkt = item.first;
      bool big_flow = item.second;

      if (pkt.flow_id >= 0) {
        if (big_flow &&
            pkt.burst_table_index >= 0 &&
            static_cast<std::size_t>(pkt.burst_table_index) < burst_table.size()) {
          if (burst_table[pkt.burst_table_index] < 0) {
            burst_table[pkt.burst_table_index] = pkt.flow_id;
          }
        }

        if (big_flow) {
          // Generate rule with delayed availability.
          Rule rule;
          rule.update_entry = pkt.flow_id;
          rule.burst_table_idx = pkt.burst_table_index;
          rule.replace_entry = -1;
          rule.ready = (pkt.burst_table_index < 0 ||
                        static_cast<std::size_t>(pkt.burst_table_index) >= burst_table.size());
          rule.ready_time_ns =
              Simulator::Now().GetNanoSeconds() + owner->cfg.slowpath_rule_latency_ns;
          rule_delay.push_back(rule);
          MaybeScheduleEmit();
        }

        owner->ForwardSlow(pkt);
        return;
      }

      if (pkt.burst_table_index >= 0 &&
          static_cast<std::size_t>(pkt.burst_table_index) < burst_table.size()) {
        // Clear signal: mark pending rules ready.
        burst_table[pkt.burst_table_index] = -1;
        int count = 0;
        uint64_t now = Simulator::Now().GetNanoSeconds();
        for (auto &rule : rule_delay) {
          if (rule.burst_table_idx == pkt.burst_table_index) {
            rule.ready = true;
            count++;
          }
        }
        NS_ABORT_MSG_IF(count == 0, "Clear signal before rule ready");
        MaybeScheduleEmit();
      }
    }

    void MaybeScheduleEmit() {
      // Emit rule updates when ready and latency elapsed.
      uint64_t now = Simulator::Now().GetNanoSeconds();
      uint64_t earliest = UINT64_MAX;
      for (const auto &rule : rule_delay) {
        if (!rule.ready) {
          continue;
        }
        earliest = std::min(earliest, rule.ready_time_ns);
      }
      if (earliest == UINT64_MAX) {
        return;
      }
      uint64_t target = std::max(now, earliest);
      if (!emit_scheduled || target < emit_time_ns) {
        if (emit_scheduled) {
          emit_event.Cancel();
        }
        emit_scheduled = true;
        emit_time_ns = target;
        emit_event = Simulator::Schedule(NanoSeconds(target - now),
                                          &SlowpathState::EmitOneRule, this);
      }
    }

    void EmitOneRule() {
      emit_scheduled = false;
      emit_time_ns = 0;
      uint64_t now = Simulator::Now().GetNanoSeconds();

      for (auto it = rule_delay.begin(); it != rule_delay.end(); ++it) {
        if (!it->ready || it->ready_time_ns > now) {
          continue;
        }
        Rule rule = *it;
        rule_delay.erase(it);

        if (counters.find(rule.update_entry) != counters.end()) {
          // Already present: only unblock.
          RuleUpdate ru;
          ru.type = UpdateType::IDLE;
          ru.update_entry = -1;
          ru.replace_entry = -1;
          ru.unblock_index = rule.burst_table_idx;
          owner->ScheduleRuleUpdate(ru);
          MaybeScheduleEmit();
          return;
        }

        int replace_entry = PlaceRule(rule.update_entry);
        // Always unblock, update if placement succeeds.
        RuleUpdate ru;
        ru.unblock_index = rule.burst_table_idx;
        if (replace_entry >= 0) {
          ru.update_entry = rule.update_entry;
          if (replace_entry == rule.update_entry) {
            ru.type = UpdateType::INSERT;
            ru.replace_entry = -1;
          } else {
            ru.type = UpdateType::REPLACE;
            ru.replace_entry = replace_entry;
          }
          Rule pending = rule;
          pending.replace_entry = ru.replace_entry;
          rules_on_the_way.push_back(pending);
        } else {
          ru.type = UpdateType::IDLE;
          ru.update_entry = -1;
          ru.replace_entry = -1;
        }
        owner->ScheduleRuleUpdate(ru);
        MaybeScheduleEmit();
        return;
      }
    }

    int PlaceRule(int flow_id) {
      // Slowpath placement policy mirrors the original module logic.
      if (counters.size() < owner->cfg.stable_table_volume) {
        counters[flow_id] = kLargeCount;
        return flow_id;
      }

      for (auto it = counters.begin(); it != counters.end(); ++it) {
        int rule_flow_id = it->first;
        int count = it->second;
        if (rule_flow_id == flow_id) {
          return -1;
        }
        if (count < static_cast<int>(owner->cfg.slowpath_cold_threshold)) {
          replaced_rules[rule_flow_id] = count;
          counters[flow_id] = kLargeCount;
          counters.erase(it);
          return rule_flow_id;
        }
      }

      int replace_flow_id = -1;
      int min_count = kLargeCount;
      for (const auto &item : counters) {
        if (item.second < min_count) {
          min_count = item.second;
          replace_flow_id = item.first;
        }
      }
      if (min_count < static_cast<int>(owner->cfg.slowpath_not_hot_threshold)) {
        replaced_rules[replace_flow_id] = min_count;
        counters[flow_id] = kLargeCount;
        counters.erase(replace_flow_id);
        return replace_flow_id;
      }
      return -1;
    }

    Impl *owner;
    std::deque<std::pair<PacketCtx, bool>> buffer;
    uint64_t next_service_time_ns = 0;
    bool start_scheduled = false;
    EventId start_event;

    std::vector<int32_t> burst_table;
    std::deque<Rule> rule_delay;
    std::deque<Rule> rules_on_the_way;
    std::map<int32_t, int32_t> counters;
    std::map<int32_t, int32_t> replaced_rules;

    bool emit_scheduled = false;
    uint64_t emit_time_ns = 0;
    EventId emit_event;

    static const int kLargeCount = 0x3f3f3f3f;
  };

  Config cfg;
  Callback<void, Ptr<Packet>, const CustomHeader &> udpHandler;

  BurstTableState burst;
  BlockQueueState blockq;
  StableTableState stable;
  FlowTrackerState flow;
  RuleUpdaterState rule_updater;
  SlowpathState slow;

  EventId stable_flush_event;
  EventId cms_clear_event;
  EventId bloom_clear_event;
};

OooSystemAdapter::OooSystemAdapter() : m_impl(new Impl(Config())) {}

OooSystemAdapter::OooSystemAdapter(const Config &cfg) : m_impl(new Impl(cfg)) {}

void OooSystemAdapter::SetConfig(const Config &cfg) { m_impl->SetConfig(cfg); }

void OooSystemAdapter::SetUdpHandler(Callback<void, Ptr<Packet>, const CustomHeader &> cb) {
  m_impl->udpHandler = cb;
}

void OooSystemAdapter::HandleUdp(Ptr<Packet> p, const CustomHeader &ch) {
  m_impl->HandleUdp(p, ch);
}

}  // namespace ns3
