本文档介绍将子项目嵌入到ns3仿真程序中的思路与大致方案。

嵌入位置：`RdmaHw::Receive`在接收到报文后，会根据报文类型进行相应处理。对于udp报文，我们在其触发`RdmaHw::Receive`后需要先送到子项目中`System`的快速路径进行on-path的处理（`System`中的`Slowpath`作为off-path的慢路径，负责处理在快速路径的`StableTable`中未命中的数据包），完成处理后再调用原本会触发的`ReceiveUdp`函数。

`BurstTable`、`BlockQueue`、`StableTable`中还残留有一些recirculate逻辑没删干净，这部分直接忽略，不要嵌入到ns3-rdma的类实现中。

为了降低仿真复杂度，认为`BurstTable`的更新和查找、`BlockQueue`的出入队和更新维护、`StableTable`的更新和查找、`FanInBuffer`、`FlowTracker`的计数和规则生成延迟都为0。PCIe延迟仍需要考虑。`Slowpath`中，仍然保留数据包处理延迟（`ScalableDelay<std::pair<T, bool>, PROCESS_CYCLE + PROCESS_EXTRA_CYCLE> pkt_delay`）与规则生成延迟（`Slowpath::get_rule_latency`）。

为了降低仿真复杂度，`BlockQueue`不能每周期调度一次，必须对于每个数据包调度常数次，以下给出调度算法的一个参考实现思路，可以在此基础上进行改进：

- 若当前无可调度数据包且无清空信号可生成（即非空队列全被阻塞），进入休眠状态；
- 在休眠状态下如果miss队列有数据包到达，调度完这个数据包后再进入休眠状态；
- 在休眠状态下如果收到取消阻塞信号，开始逐个调度取消阻塞的队列，直到该队列调度空，中间不能调度其他队列；
- 清空信号的生成/调度优先级仍然低于所有队列（包括miss队列）

此外，`BlockQueue`和`Slowpath`都会涉及到从一个队列中连续调度若干个数据包的情况，这在ns3中实现时可以用调度一个数据包后，将调度下一个数据包的时间Push到全局调度队列中来实现。

通过以上更改，我们可以将这个子项目逐周期仿真的逻辑迁移到ns3-rdma逐事件仿真的逻辑：数据包在`BurstTable`、`BlockQueue`、`StableTable`、`FlowTracker`、`Slowpath`之间的移动作为一个事件，各模块被数据包触发和被其他模块输出信号触发的内部状态更新作为事件。

子项目中用`packet_id`来帮助判断是否是真实数据包，用`flow_id`来查表，嵌入时`packet_id`就没用了，`flow_id`替换为五元组。`sequence_number`和recirculate相关，也舍弃。





