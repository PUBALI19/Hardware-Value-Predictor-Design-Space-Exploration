# High-Performance Stride Value Predictor (SVP) with Forward Probabilistic Counters

An advanced microarchitectural implementation and design space exploration of a cycle-accurate **Value Prediction Engine**, integrated into the `721sim` superscalar Out-of-Order (OoO) processor simulator. 

This project explores the limits of speculative execution by injecting predicted operand values directly into the physical register file (PRF) during the **Register Rename stage**. By breaking true data dependencies (RAW hazards) early, the engine accelerates instructions-per-cycle (IPC) throughput across data-dependent SPEC benchmarks before the actual calculations execute.

---

## Microarchitectural Exploration & Strategies

To maximize performance under strict constraints, the engine implements and compares four distinct value-prediction methodologies:

1. **Stride Value Predictor (SVP - Baseline)**: Indexes a 1024-entry table via a 10-bit PC hash to fetch `retired_value` and `stride`. Calculates the speculative target using `predicted = retired_value + (instance × stride)`, where an in-flight `instance` counter tracks sequential instructions in the Value Prediction Queue (VPQ).
2. **Last Value Predictor (LVP)**: A reduced-complexity strategy capturing constant-value streams. Exploration confirmed that constant-value coverage alone was insufficient to justify the structural overhead compared to stride tracking for this SPEC mix.
3. **Dual-Stride Predictor**: Designed to track alternating or nested stride behaviors. However, evaluation showed that its slower confidence warm-up latency underperformed on highly dynamic workloads.
4. **Forward Probabilistic Counters (FPC - Champion Configuration)**: Our winning design. Instead of standard linear increments, confidence counters scale probabilistically (e.g., matching a 7-bit counter's strict behavior within a 3-bit physical footprint). This forces the engine to reach a high-saturation threshold (~97 consecutive correct predictions) before predicting, achieving **>99.5% prediction accuracy** and virtually eliminating costly commit-time squashes.

---

## Strict 19.0 KB Hardware Cost Accounting

Adhering to a rigid microarchitectural storage constraint, every field within the **1024-entry** table is optimized down to the bit level. Per-entry structures map as follows:

| Hardware Structure Field | Size (Bits) | Functional Description |
| :--- | :---: | :--- |
| **Tag** | 10 bits | Upper PC bit slice mapping for conflict reduction |
| **Confidence (Conf)** | 3 bits | Encodes a `confmax=6` Forward Probabilistic Counter state machine |
| **Retired Value** | 64 bits | Base 64-bit RISC-V register contents |
| **Stride** | 64 bits | Computed distance between sequential instruction outputs |
| **Instance Counter** | 9 bits | Tracks in-flight queue references based on a 300-entry VPQ depth |
| **Total per Entry** | **150 bits** | Structural size per table index |

- **Total Storage Cost (Bits)**: $1024 \text{ entries} \times 150 \text{ bits/entry} = 153,600 \text{ bits}$
- **Total Storage Cost (Bytes)**: $19,456 \text{ Bytes} \rightarrow$ **Exactly 19.00 KB**

*Note: In-flight instruction pipelines, such as the 300-entry Value Prediction Queue (VPQ), are classified as speculative execution tracking states and do not count against the hard architected state storage budget.*

---

## Simulation Execution Framework

The `721sim` compilation targets instruction classifications spanning `INTALU`, `FPUALU`, and `LOAD` operations across four discrete pipeline boundaries:
- **Rename**: Queries the value predictor table, estimates state machine confidence, speculatively updates the Register Alias Table (RAT), and awakens dependent instructions ahead of schedule.
- **Dispatch**: Injects the predicted value directly into the Physical Register File (PRF), changing its state to "ready" to clear downstream issue queue roadblocks.
- **Execute / Writeback**: Validates speculative assumptions. Mispredictions trigger an automated value-misprediction recovery sequence (**VR-1 commit-time squash** mechanism). Dual-point checking is implemented within both `pipeline_t::writeback()` and `pipeline_t::load_replay()` to catch late-completing load instructions cleanly.
- **Retire**: The VPQ pops transactions in-order and trains the table state arrays non-speculatively.

---

## Performance Results & Benchmarks

The champion FPC configuration was evaluated against standard SPEC benchmark reference traces. By optimizing for accuracy over sheer coverage, the architecture successfully mitigated misprediction recovery penalties, yielding dramatic IPC scaling across several critical workloads:

- **429.mcf**: **2.04x Speedup** (Baseline IPC: 0.82 $\rightarrow$ Final Entry IPC: 1.68)
- **623.xalancbmk**: **1.75x Speedup** (Baseline IPC: 1.05 $\rightarrow$ Final Entry IPC: 1.84)
- **649.fotonik3d**: **1.46x Speedup** (Baseline IPC: 0.73 $\rightarrow$ Final Entry IPC: 1.07)
- **434.zeusmp**: **1.11x Speedup** (Baseline IPC: 2.04 $\rightarrow$ Final Entry IPC: 2.28)

---

## Getting Started

### Prerequisites
- Linux/Unix cluster environment.
- GCC/G++ Compiler supporting advanced microarchitectural build tools.

### Compilation & Execution Syntax
To wipe old objects and execute a clean run with our optimal competition flag parameters, execute:

```bash
make cleanrun SIM_FLAGS_EXTRA='--vp-eligible=1,0,1 --vp-svp=300,0,10,10,6,1 --mdp=5,0 --perf=0,0,0,1 -t --cbpALG=0 --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=8 --dw=8 --iw=16 --rw=8 -e10000000'
```
### Key Parameter Definition:
- --vp-eligible=1,0,1: Enables value prediction eligibility for INTALU and LOAD instructions.

- --vp-svp=300,0,10,10,6,1: Configures a 300-entry VPQ, 10-bit indexing, 10-bit tagging, a maximum confidence state of 6, and explicitly enables Forward Probabilistic Counters (fpc=1).
