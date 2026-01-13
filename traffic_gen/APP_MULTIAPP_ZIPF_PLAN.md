  # Multi-App Zipf Traffic Generation Plan

  ## Requirements
  - Extend flow file format to carry per-flow app ports and keep `pg` available for ECN/priority.
  - Support multiple apps per host; each app has a fixed `src_port`.
  - Each app generates flows as an independent Poisson process; flow size is sampled from the input CDF.
  - For any destination IP, an app may select any `dst_port` (random in a configured range).
  - Enforce per-host bandwidth: if actual send/receive rates exceed `bandwidth`, redistribute the excess to other hosts **proportionally at random** to their remaining capacity.
  - Goal 1: All hosts have the same total send rate; within a host, per-app send rates and per-app receive rates follow independent Zipf distributions.
  - Goal 2: Additionally, host total send rates and host total receive rates follow independent Zipf distributions.

  ## Flow File Format (proposed)

  src dst sport dport pg size start_time

  - `src_port` is fixed per app (e.g., `port_base + src_app_id`).
  - `dst_port` is fixed per app (e.g., `port_base + dst_app_id`).

  ## Generator Parameters (new)
  - `--napps N` (or `-a N`) number of apps per host.
  - `--zaa s_app` Zipf exponent for per-app send/receive rates (zipf_alpha_app).
  - `--zah s_host` Zipf exponent for per-host total send/receive rates (zipf_alpha_host; used in Goal 2).
  - `--port-base`.

  ## Notation
  - `B`: host link bandwidth (bits/s), `L`: load.
  - `R_total = B * L * nhost` total network send rate.
  - `R_send[h]`, `R_recv[h]`: host total send/recv rates.
  - `w_send[h][a]`, `w_recv[h][a]`: per-app Zipf weights (independent).

  ## Goal 1 Algorithm (uniform host totals + Zipf per app)
  1. Set `R_send[h] = B * L` for all hosts; set `R_recv[h] = B * L` for all hosts.
  2. For each host `h`, draw independent Zipf weights over apps: `w_send[h][a]`, `w_recv[h][a]`.
  3. Per-app rates: `R_send[h][a] = R_send[h] * w_send[h][a]`.
  4. Poisson inter-arrival per app:
     - `avg_flow_size = CDF.mean()`
     - `avg_inter_arrival[h][a] = 1 / (R_send[h][a] / 8 / avg_flow_size)`.
  5. Destination selection:
     - Pick `dst_host` uniformly (since all `R_recv[h]` equal), excluding `src`.
     - Pick `dst_app` according to `w_recv[dst_host][a]`.
     - Set `dst_port = port_base + dst_app`.
  6. Merge events with a min-heap keyed by time; emit flows in time order.
  7. Bandwidth guard (if `R_send[h]` or `R_recv[h]` exceeds `B`, apply redistribution below).

  ## Goal 2 Algorithm (Zipf host totals + Zipf per app)
  1. Draw independent Zipf weights over hosts:
     - `W_send[h]` and `W_recv[h]` (independent).
  2. Set host totals:
     - `R_send[h] = R_total * W_send[h]`
     - `R_recv[h] = R_total * W_recv[h]`
  3. Apply bandwidth guard/redistribution to `R_send` and `R_recv` separately.
  4. For each host, draw independent per-app Zipf weights and compute `R_send[h][a]` as in Goal 1.
  5. Poisson arrival + destination selection as in Goal 1, except `dst_host` is drawn by `R_recv[h]` weights.

  ## Bandwidth Guard: Random Proportional Redistribution
  Apply separately to send rates and receive rates.
  1. For each host `h`, if `R[h] > B`, set `excess[h] = R[h] - B` and cap `R[h] = B`.
  2. Compute remaining capacity `cap[h] = B - R[h]` for all hosts.
  3. For each oversubscribed host, redistribute its `excess` to other hosts using random proportional allocation:
     - Probability for host `j`: `p[j] = cap[j] / sum(cap)` (hosts with `cap[j] > 0`).
     - Sample a multinomial allocation of `excess` across hosts by `p[j]` (or iteratively sample chunks).
     - Update `R[j] += allocated[j]`, `cap[j] -= allocated[j]`.
  4. If total capacity is insufficient, scale down all rates proportionally (fallback safeguard).
  5. After redistribution, normalize per-app rates within each host using the host’s Zipf weights.

  ## network-load-balance.cc Parsing Changes (summary)
  - Read two extra fields per flow: `src_port`, `dst_port`.
  - Use these ports directly instead of `portNumber[src]++` and `dportNumber[dst]++`.
  - Keep existing `pg`, `size`, `start_time` semantics unchanged.

  ## Validation Checks (by modifying `traffic_gen.py`)
  - Per-host send/recv rate <= `bandwidth` after redistribution.
  - Total send rate matches total recv rate (up to sampling error).
  - Per-app rate distributions match Zipf targets.
  - Flow sizes match CDF; start times obey Poisson for each app.
