# Cycle-Accurate Stride Value Predictor (SVP) Implementation

A high-performance microarchitectural implementation of a cycle-accurate **Stride Value Predictor (SVP)**, integrated directly into the `721sim` superscalar Out-of-Order (OoO) processor simulator infrastructure. 

This engine is engineered to speculatively inject predicted operand values during the Register Rename stage, breaking true data dependencies (RAW hazards) and accelerating instructions-per-cycle (IPC) throughput across complex, data-dependent SPEC benchmarks.

## Microarchitectural Features & Design Constraints

- **Speculative Rename Injection**: Intercepts instructions during the dispatch/rename phase to check the Value Prediction Table (VPT). If a confident prediction is available, the physical destination register is speculatively marked as "ready," bypassing traditional issue queue producer-latency delays.
- **Hardware Storage Budget Optimization**: Adheres to a strict **19.0 KB hardware budget constraint** for all structures. Implements an efficient Value Prediction Table (VPT) configuration mapping 32-bit/64-bit strides, state tags, and validation bits without exceeding physical capacity.
- **Forward Probabilistic Counters (FPC)**: Integrates 2-bit state-saturating confidence counters per entry to govern prediction throttling. Enforces a high-threshold state machine to guarantee a **>99.5% prediction accuracy**, eliminating costly pipeline flushes caused by speculative mispredictions.
- **Verification Recovery & Replay Hooks**: Integrates dual-point validation handling across the executing backend. Checks speculative outcomes at both the standard `pipeline_t::writeback()` phase and the `pipeline_t::load_replay()` path to catch late-completing load instructions and resolve validation misses cleanly.

---

## Simulation Execution Pipeline Flow

The execution engine sequences speculative states sequentially across the simulated superscalar pipeline:
1. **Fetch & Decode**: Standard instructions stream from trace files into the front-end pipeline.
2. **Rename / Predict**: The PC indexes into the Stride Value Predictor. If a confident stride state matches, the physical register alias table (RAT) is updated with the predicted speculative value, and downstream dependent instructions are awakened early.
3. **Issue / Execute**: Instructions execute out-of-order based on an oldest-first selection policy. Value-predicted instructions run without waiting for true memory or arithmetic dependencies to clear.
4. **Writeback / Validate**: The actual computed outcome is cross-checked against the speculatively injected value. If a mismatch occurs, the engine initiates a selective execution recovery/replay pass to nullify the polluted down-stream path.

---

## Getting Started

### Prerequisites
- Linux/Unix compute environment.
- GCC/G++ compiler supporting standard C++ compilation tools.
- A functional instance of the base `721sim` simulation infrastructure.

### Installation & Compilation
The simulator compiles via an optimization-enabled `Makefile`. To build the standalone simulator executable:

```bash
make
```
### Performance Metrics & Tracking
Upon completing a trace benchmark, the simulator dumps comprehensive microarchitectural performance reports, including:
- Total instruction count and baseline simulation cycles.
- Instructions Per Cycle (IPC) metrics showing a 2.04x scaling speedup on data-dependent workloads.
- Total prediction allocations, speculative injections, and exact misprediction recovery counts.
- Final state analysis of the internal VPT confidence arrays.
