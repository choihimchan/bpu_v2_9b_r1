# BPU v2.9b-r1 — Data Stability Under Pressure (Flow Diagram)

This diagram illustrates the high-level runtime flow of  
**BPU (Batch Processing Unit)** and how it maintains output stability
under **TX backpressure** and **budget pressure**.

---

## Diagram intent

This diagram explains how BPU:

- Explicitly models backpressure paths
- Applies budget-based degradation
- Preserves high-priority data
- Makes all scheduling decisions observable via runtime counters

It represents **runtime behavior**, not static architecture.

---

## High-level flow

```mermaid
flowchart LR
    P[Producers]
    IN[Ingress_Coalesce]
    Q[Queues_by_type_priority]
    S[Scheduler_Tick]
    TX[TX_Flush]
    OUT[UART_Frames_Out]
    BACK[Backpressure]
    DROP[Drop_or_Degrade_low_priority]
    STATS[Runtime_Stats]

    P --> IN
    IN --> Q
    Q --> S

    S -->|budget_ok| TX
    S -->|budget_tight| DROP

    TX -->|blocked| BACK
    BACK --> Q

    TX --> OUT

    S --> STATS
    DROP --> STATS
    BACK --> STATS

```
## How to read this diagram

- Every arrow represents a **runtime decision**
- Budget and TX state are evaluated on each scheduler tick
- Drops and backpressure are **explicitly counted**
- Recovery paths rejoin the main queue flow

Cross-reference

This diagram directly corresponds to:

Runtime counters described in docs/stats.md

Observed execution scenarios in docs/log_samples.md

Together, these documents describe a single validated runtime model.

