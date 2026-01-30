# BPU v2.9b-r1 — Design Notes

This document describes the design philosophy and internal structure of  
**BPU (Batch Processing Unit)** — a small embedded scheduling engine focused on
**output stability under pressure**, not raw throughput.

BPU was designed and validated on ESP32 (Arduino environment), but the concepts
are hardware-agnostic and apply to constrained output pipelines in general.

---

## 1. Problem Statement

In embedded systems—especially those involving UART, BLE, or SPI outputs—the
system frequently encounters **output pressure**:

- TX buffers temporarily unavailable
- Limited bytes-per-tick budget
- Bursty producers generating more data than can be flushed immediately

Typical approaches (simple queues, ring buffers) fail in these cases by:
- Growing unbounded
- Blocking producers
- Or silently losing important data

BPU was created to address this exact failure mode.

---

## 2. Core Design Principles

### 2.1 Stability over completeness

BPU intentionally prioritizes **system stability** over guaranteeing delivery
of all data.

Losing *low-priority telemetry* is acceptable.  
Blocking the system is not.

This principle drives all scheduling decisions and degradation behavior.

---

### 2.2 Separation of Events and Jobs

BPU separates input into two conceptual layers:

- **Events**  
  Lightweight signals representing *state changes* or *intent*.

- **Jobs**  
  Concrete work units generated from events and scheduled for output.

Why this matters:

- Events can be **coalesced**
- Jobs represent **actual cost** (bytes, time)
- Scheduling decisions happen at the job level, not the event level

This separation avoids redundant work and enables precise pressure control.

---

## 3. Event Coalescing

Not all events are equal.

Some event types (e.g. telemetry updates) only require the **latest state**.

BPU supports **event coalescing**, where:
- Multiple incoming events of the same type
- Collapse into a single, newest event

Benefits:
- Prevents queue growth under bursty conditions
- Reduces useless work
- Keeps output semantically meaningful

This is a key difference from naive FIFO designs.

---

## 4. Job Scheduling and Budget Control

### 4.1 Bytes-per-tick budget

BPU enforces a **bytes-per-tick** limit during output flushing.

Each tick:
- A fixed output budget is assigned
- Jobs are flushed until the budget is exhausted
- Remaining jobs are deferred

This guarantees:
- A deterministic upper bound on output cost per tick
- No single tick monopolizes the system

---

### 4.2 TX Backpressure Handling

When the UART TX buffer is unavailable:
- Jobs are **not discarded immediately**
- They are requeued
- A `skipTX` counter is incremented

Once TX resumes:
- Queued jobs flush automatically
- Recovery is observable via runtime statistics

This behavior is validated using real execution logs.

---

## 5. Degradation Strategy

Under sustained pressure, BPU degrades gracefully:

- High-priority jobs are preserved
- Low-priority jobs (e.g. TELEMETRY) may be dropped
- Drop decisions are explicit and counted (`drop`, `degrade` stats)

This avoids:
- Memory exhaustion
- Producer blocking
- Undefined behavior

Degradation is a **first-class feature**, not a failure mode.

---

## 6. Observability and Validation

BPU exposes explicit runtime statistics to make pressure behavior observable:

- Event in/out/merge/drop
- Job in/out/requeue/drop
- TX skip counters
- Budget exhaustion indicators

Real execution logs and detailed interpretations are provided in:
- `docs/log_samples.md`
- `docs/stats.md`

These logs demonstrate:
- Backpressure handling
- Budget exhaustion
- Graceful degradation and recovery
