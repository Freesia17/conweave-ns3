import sys
import random
import math
import heapq
import numpy as np
from optparse import OptionParser
from custom_rand import CustomRand

class Flow:
    def __init__(self, src, dst, sport, dport, size, t):
        self.src, self.dst, self.sport, self.dport, self.size, self.t = src, dst, sport, dport, size, t
    def __str__(self):
        return "%d %d %d %d 3 %d %.9f"%(self.src, self.dst, self.sport, self.dport, self.size, self.t)

def translate_bandwidth(b):
    if b == None:
        return None
    if type(b)!=str:
        return None
    if b[-1] == 'G':
        return float(b[:-1])*1e9
    if b[-1] == 'M':
        return float(b[:-1])*1e6
    if b[-1] == 'K':
        return float(b[:-1])*1e3
    return float(b)

def poisson(lam):
    if lam <= 0: return float('inf')
    return -math.log(1-random.random())*lam

class ZipfSampler:
    def __init__(self, n, alpha):
        self.n = n
        self.alpha = alpha
        ranks = np.arange(1, n + 1)
        weights = 1.0 / np.power(ranks, alpha)
        self.probs = weights / np.sum(weights)
        self.indices = np.arange(n)

    def get_probs(self):
        p = np.copy(self.probs)
        np.random.shuffle(p)
        return p

def redistribute_capacity(rates, max_cap):
    """
    Random Proportional Redistribution Logic:
    1. If rate[h] > max_cap, set excess = rate[h] - max_cap, rate[h] = max_cap.
    2. Collect total excess.
    3. Redistribute excess to hosts with remaining capacity (rate < max_cap).
       Probability of receiving excess is proportional to remaining capacity.
    4. Repeat until no excess or all hosts full.
    """
    rates = np.array(rates, dtype=float)
    
    while True:
        excess = 0.0
        
        # 1. Cap and collect excess
        over_indices = np.where(rates > max_cap)[0]
        if len(over_indices) == 0:
            break # No one is over limit
            
        for idx in over_indices:
            excess += rates[idx] - max_cap
            rates[idx] = max_cap
            
        if excess < 1e-9:
            break

        # 2. Find candidates for redistribution
        under_indices = np.where(rates < max_cap)[0]
        if len(under_indices) == 0:
            # All hosts are full (or over), cannot redistribute remaining excess
            # Scale down everything? Or just drop excess (effectively lowering total load)
            # Strategy: Just drop excess as we hit physical limits
            print("Warning: Network fully saturated, dropping excess traffic: %f" % excess)
            break
            
        # 3. Calculate weights based on remaining capacity
        # remaining_cap[j] = max_cap - rates[j]
        remaining = max_cap - rates[under_indices]
        total_remaining = np.sum(remaining)
        
        if total_remaining <= 0:
            break

        probs = remaining / total_remaining
        
        # 4. Redistribute "Randomly"
        # We can simulate random chunks allocation, or just fluid allocation 
        # "Distribute ... proportionally at random" implies randomness.
        # Let's chunk the excess into small units to simulate randomness or just use weighted assignment.
        # Since 'excess' is rate (continuous), 'random proportional' is statistically 'proportional'.
        # For simplicity and exactness to the distribution, we add proportionally here.
        # If strict "random chunk" simulation is needed, it would be very slow.
        # Interpret "proportionally at random" as: target distribution is proportional.
        
        # Add excess to under-utilized hosts
        # If we add all excess at once, some might overflow again in next iteration (which is handled by loop)
        rates[under_indices] += excess * probs

    return rates

if __name__ == "__main__":
    port = 80
    parser = OptionParser()
    parser.add_option("-c", "--cdf", dest = "cdf_file", help = "cdf file", default = "uniform_distribution.txt")
    parser.add_option("-n", "--nhost", dest = "nhost", help = "number of hosts")
    parser.add_option("-a", "--napps", dest = "napps", default="1", help = "apps per host")
    parser.add_option("-p", "--base-port", dest = "base_port", default="10000", help = "base port for app mapping")
    parser.add_option("-l", "--load", dest = "load", default="0.3", help = "load")
    parser.add_option("-b", "--bandwidth", dest = "bandwidth", default="10G", help = "bandwidth")
    parser.add_option("-t", "--time", dest = "time", default="10", help = "time")
    parser.add_option("-o", "--output", dest = "output", default="tmp_traffic.txt", help = "output")
    # Zipf parameters
    parser.add_option("--zah", dest = "zipf_alpha_host", default="0.001", help = "Zipf alpha Host")
    parser.add_option("--zaa", dest = "zipf_alpha_app", default="0.001", help = "Zipf alpha App")
    
    options,args = parser.parse_args()

    base_t = 2000000000
    base_port = int(options.base_port) if options.base_port else 10000

    if not options.nhost:
        print("Missing required argument: number of hosts. Please specify it using -n or --nhost.")
        sys.exit(0)
        
    nhost = int(options.nhost)
    napps = int(options.napps) if options.napps else 1
    if napps <= 0:
        napps = 1
    if base_port + napps - 1 > 65535:
        print("Invalid base_port/napps: port range exceeds 65535")
        sys.exit(0)
    load = float(options.load)
    bandwidth = translate_bandwidth(options.bandwidth)
    # Bandwidth here is physical link speed (e.g., 10Gbps)
    cap_rate = bandwidth
    
    time_dur = float(options.time)*1e9
    output = options.output
    zipf_alpha_host = float(options.zipf_alpha_host) if options.zipf_alpha_host else 0.001
    zipf_alpha_app = float(options.zipf_alpha_app) if options.zipf_alpha_app else 0.001

    if bandwidth == None: sys.exit(0)

    # Read CDF
    fileName = options.cdf_file
    file = open(fileName,"r")
    lines = file.readlines()
    cdf = []
    for line in lines:
        x,y = map(float, line.strip().split(' '))
        cdf.append([x,y])
    customRand = CustomRand()
    if not customRand.setCdf(cdf): sys.exit(0)
    avg_flow_size = customRand.getAvg() # bytes

    # ----------------------------------------------------------------
    # 1. Total Network Throughput = nhost * (bandwidth * load)
    #    (Assuming load is average load)
    # ----------------------------------------------------------------
    total_target_rate = nhost * (bandwidth * load) # bits per second
    
    # 2. Generate Host Raw Weights
    host_sampler = ZipfSampler(nhost, zipf_alpha_host)
    raw_tx_weights = host_sampler.get_probs()
    raw_rx_weights = host_sampler.get_probs()
    
    # 3. Assign Raw Rates
    tx_rates = raw_tx_weights * total_target_rate
    rx_rates = raw_rx_weights * total_target_rate
    
    # 4. Apply Bandwidth Guard (Redistribution)
    print("Redistributing TX Rates (Cap: %e)..." % cap_rate)
    tx_rates = redistribute_capacity(tx_rates, cap_rate)
    print("Redistributing RX Rates (Cap: %e)..." % cap_rate)
    rx_rates = redistribute_capacity(rx_rates, cap_rate)
    
    # Re-normalize to get final probabilities for Destination Selection
    # (For Tx, we use absolute rates to determine inter-arrival)
    P_Rx_Host = rx_rates / np.sum(rx_rates)
    
    # 5. App Weights
    app_sampler = ZipfSampler(napps, zipf_alpha_app)
    P_Tx_App = [app_sampler.get_probs() for _ in range(nhost)]
    P_Rx_App = [app_sampler.get_probs() for _ in range(nhost)]
    
    # 6. Event Loop Setup
    ofile = open(output, "w")
    
    # Calculate Lambda for each host
    # Rate (bits/s) / AvgFlowBits = Flows/s
    avg_flow_bits = avg_flow_size * 8.0
    lambda_per_host = tx_rates / avg_flow_bits
    
    avg_inter_arrival_per_host = np.zeros(nhost)
    host_event_list = []
    
    for i in range(nhost):
        if lambda_per_host[i] > 1e-9:
            avg_inter_arrival_per_host[i] = 1e9 / lambda_per_host[i] # ns
            t_next = base_t + int(poisson(avg_inter_arrival_per_host[i]))
            host_event_list.append((t_next, i))
        else:
            avg_inter_arrival_per_host[i] = -1
            
    heapq.heapify(host_event_list)
    
    # Estimate total flows
    total_lambda = np.sum(lambda_per_host)
    n_flow_estimate = int(total_lambda * (time_dur / 1e9))
    ofile.write("%d \n"%n_flow_estimate)
    
    n_flow = 0
    
    while len(host_event_list) > 0:
        t, src = host_event_list[0]
        
        # Schedule next
        if avg_inter_arrival_per_host[src] > 0:
            inter_t = int(poisson(avg_inter_arrival_per_host[src]))
        else:
            heapq.heappop(host_event_list)
            continue
            
        if t > time_dur + base_t:
            heapq.heappop(host_event_list)
            continue
            
        # --- PROPERTIES ---
        # Src Port
        src_app_idx = np.random.choice(napps, p=P_Tx_App[src])
        sport = base_port + src_app_idx
        
        # Dst Host (Weighted by Rx Rate)
        while True:
            dst = np.random.choice(nhost, p=P_Rx_Host)
            if dst != src: break
            
        # Dst Port
        dst_app_idx = np.random.choice(napps, p=P_Rx_App[dst])
        dport = base_port + dst_app_idx
        
        # Size
        size = int(customRand.rand())
        if size <= 0: size = 1
        
        n_flow += 1
        ofile.write("%d %d %d %d 3 %d %.9f\n"%(src, dst, sport, dport, size, t * 1e-9))
        
        heapq.heapreplace(host_event_list, (t + inter_t, src))
        
        if n_flow % 100000 == 0:
            print("Generated %d flows" % n_flow)
            
    ofile.seek(0)
    ofile.write("%d"%n_flow)
    ofile.close()
    print("Done: %d flows." % n_flow)
