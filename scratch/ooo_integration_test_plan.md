# OooSystem Integration Test Plan

This document outlines the integration testing strategy for `OooSystem`, ensuring all sub-modules (`BlockQueue`, `Slowpath`, `StableTable`, `FlowTracker`, `BurstTable`) work together correctly.

## Test Strategy

We will use `ns-3` simulation with synthetic traffic generation to cover the following scenarios. We will use a Mock UDP Handler to verify end-to-end packet delivery, ordering, and latency.

## Test Scenarios

### 1. System Initialization & Idling
**Goal**: Verify the system starts up correctly, internal states are initialized, and it handles "no traffic" without errors (e.g., periodic flush events don't crash).
- **Action**: Instantiate `OooSystem`, run for a duration with no packets.
- **Verify**: No crashes, internal counters are zero.

### 2. Steady-State Big Flow
**Goal**: Verify that a known "Big Flow" (in Stable Table) skips the BlocQueue/Slowpath and goes through FastPath.
- **Setup**: Pre-configure Stable Table with a flow.
- **Action**: Send packets for this flow.
- **Verify**: Packets delivered, correct order, low latency (FastPath), BlockQueue/Slowpath counters unchanged.

### 3. Steady-State Small Flow (Cold -> Hot Transition)
**Goal**: Verify a new traffic flow starts as "Cold", goes to Slowpath, gets a Rule allocated, and eventually becomes "Hot" (FastPath).
- **Action**: Send a burst of packets for a new Flow ID.
- **Verify**:
    1. First packets go to BlockQueue/Slowpath (Higher latency).
    2. Rule installed in Stable Table.
    3. Subsequent packets go FastPath.
    4. **Critical**: No Packet Reordering at the output.

### 4. BlockQueue & Scheduler Logic (Mocked Signals)
**Goal**: Test BlockQueue fairness and priority when Slowpath provides explicit signals (Allocate/Unblock).
- **Action**: Manually trigger `BlockUpdate` signals while traffic is flowing.
- **Verify**: Queues block/unblock as expected; Drain priority is respected (if observable).

### 5. New Big Flow & OOO Avoidance
**Goal**: Verify that when a flow upgrades from Small (Slowpath) to Big (FastPath), OOO avoidance mechanisms work (e.g., flushing queues before switching).
- **Action**: Generate traffic that triggers rule creation.
- **Verify**: Sequence of packets at output is strictly monotonic.

### 6. Contention: Stable Table
**Goal**: Verify behavior when Stable Table is full or under contention.
- **Setup**: Small Stable Table size.
- **Action**: Generate many distinct big flows.
- **Verify**: Correct replacement/LRU behavior (if applicable), no crashes, continued delivery (via Slowpath if displaced).

### 7. Contention: Burst Table
**Goal**: Verify behavior when Burst Table (BlockQueue pools) is exhausted.
- **Setup**: Small BlockQueue pool size.
- **Action**: Generate many concurrent cold flows.
- **Verify**: Pool exhaustion handled gracefull (e.g., drop or wait), resources reclaimed when flows finish.

### 8. Mixed Traffic (Integration)
**Goal**: Complex scenario with Background Small Flows + active Big Flows.
- **Action**: Random traffic generator mixing multiple flow IDs.
- **Verify**: Global stability, no memory leaks, strict ordering per flow.

## Verification Tools
- **Packet Tagging**: Add Sequence Number and Flow ID to every packet.
- **Global Receiver**: A specialized `UdpHandler` that tracks:
    - Last received Sequence Number per Flow (for OOO check).
    - Total counts.
    - Latency stats.
