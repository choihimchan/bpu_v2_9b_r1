# BPU – Data Stability Under Pressure (Diagram)

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

This diagram matches:
- counters in `stats.md`
- scenarios in `log_samples.md`

- ## Diagram intent

This diagram illustrates how BPU maintains data stability
under TX backpressure and budget pressure by:

- Explicitly modeling backpressure paths
- Applying budget-based degradation
- Preserving high-priority data
- Making all decisions observable via runtime counters

