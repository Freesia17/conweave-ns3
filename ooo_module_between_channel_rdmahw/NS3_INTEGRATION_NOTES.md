# NS3-RDMA OOO System 集成说明

## 目标与范围
该集成将 `ooo_module_between_channel_rdmahw/utils/System.hpp` 的逻辑以“逐事件”方式嵌入到
`src/point-to-point/model/ooo-system.cc`。除 `integrate_guideline.md` 指定的延迟简化外，
其余逻辑（BurstTable/FlowTracker/BlockQueue/StableTable/Slowpath/RuleUpdater）保持一致。
显式丢弃 recirculate/psn/epsn 相关逻辑。

## 数据路径与事件
- UDP 报文进入 `OooSystemAdapter::HandleUdp`，解析 FlowIDNUMTag 作为 flow_id。
- BurstTable 立即命中/未命中并给出 index，BlockQueue 负责排队与调度。
- StableTable 命中直接 fast-path 送回 `ReceiveUdp`；未命中进入 FlowTracker。
- FlowTracker 进行 CMS/Bloom 判定与规则分配，生成插入信号并将报文送慢路径。
- Slowpath 处理包、生成规则与 unblock 信号；RuleUpdater 进行门控并更新 StableTable。

## 关键模块行为
- **BurstTable**：维护 `flow_id -> index` 映射，支持插入与按 index 删除。
- **FlowTracker**：CMS 计数 >= 阈值后更新 Bloom；Bloom 命中（count==1）触发规则分配。
- **BlockQueue**：队列阻塞/解除阻塞与清空信号；清空信号为 `flow_id < 0` 且带 `burst_table_index`。
  调度间隔为 1ns，避免 0 时间循环。
- **StableTable**：命中更新计数；周期性 flush 计数到 Slowpath。
- **Slowpath**：保持处理延迟与规则生成延迟；规则就绪后发 update+unblock。
- **RuleUpdater**：按 `pass_check` 门控更新；无论更新是否成功均转发 unblock。

## 延迟与定时
- BurstTable/BlockQueue/StableTable/FlowTracker/FanInBuffer 延迟视为 0。
- PCIe 延迟与 Slowpath 处理/规则延迟保留。
- Slowpath 处理速率：`slowpath_process_cycle_ns`；处理完成延迟为 `slowpath_process_ns`。
- CMS/Bloom 清空周期与 StableTable flush 周期按配置定时触发。

## 已显式移除/忽略
- recirculate 与 psn/epsn 相关状态与行为。
- BlockQueue 的 recirculate 复写逻辑。
