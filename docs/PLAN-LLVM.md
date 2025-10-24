### Where LLVM helps in our NodeFlow ecosystem (with concrete examples)

- Per-graph specialization
  - Example: Generate a single step kernel for the exact `devicetrigger_addition` topology. Replace handle lookups and variant checks with direct locals and typed ops.
  - Effect: The loop becomes raw loads/stores and fadds/fadds; no maps, no strings, no branching on node types.

- Constant folding and dead code elimination
  - Example: If `key2` is configured constant=2.0 at build time, LLVM folds it; if a node is unconnected, its code is removed.
  - Effect: Smaller, faster kernel; less memory traffic.

- Node fusion
  - Example: Fuse `DeviceTrigger -> Add` into one function that updates the sum when the trigger flips, eliminating intermediate writes and adjacency traversals.
  - Effect: Fewer cache misses, fewer instructions.

- Type specialization (erase variant overhead)
  - Example: For graphs known to be float-only, generate a float-only variant with no type switching/casting.
  - Effect: SIMD-friendly code; fewer branches.

- Topology-unrolled scheduling
  - Example: Emit straight-line code in topo order for tiny DAGs, or tight for-loops for repeated patterns (chains/fans).
  - Effect: No ready-queue at runtime; the compiler schedules instructions for the target CPU.

- Temporal blocking (N steps at once)
  - Example: Generate a kernel that computes 8 ticks per call (accumulating inputs), enabling unrolling and better ILP.
  - Effect: Much closer to AOT numbers even for heavier graphs; amortizes call overhead.

- Auto-vectorization and CPU tuning
  - Example: When summing multiple inputs or applying the same op across N steps, LLVM emits NEON/AVX loads and fadds.
  - Effect: 2–8x gains for arithmetic-bound subgraphs.

- Memory layout promotion
  - Example: Promote our SoA `portValues` to fixed-size arrays with constant indices; precompute `outToIn` as compile-time tables.
  - Effect: Pointer-arithmetic with constants; the optimizer propagates indices and removes bounds checks.

- Partial evaluation of parameters
  - Example: If `min_interval/max_interval` are compile-time constants for simulated devices, collapse control logic into a simple counter/branch with predicted behavior.
  - Effect: Fewer branches; improved pipeline predictability.

- Multi-target outputs
  - Example: From one IR, produce an arm64 dylib for macOS and a WASM module for a sandboxed browser-side simulator (for UI demos).
  - Effect: Same logic across environments; easier parity tests.

- PGO and sanitizers
  - Example: Run a representative workload to collect profiles; rebuild with PGO. Also build a “sanitized” kernel for fuzz/CI.
  - Effect: Hot paths get better layout and inlining; safer validation runs.

What LLVM will not solve
- WebSocket/IPC latency or bandwidth limits (orthogonal).
- High-level scheduling policy for dynamic/async sources (algorithmic choice, not codegen).
- Hot-swapping arbitrary graphs at runtime without recompilation (JIT can help, but costs complexity).

Minimal integration path (low risk)
- Keep current C++ AOT generator as baseline.
- Add an optional “LLVM backend” that:
  - Emits LLVM IR/MLIR from our descriptors (handles, topo order, outToIn).
  - Specializes types (float-first).
  - Generates a shared library with a C ABI matching `nodeflow_step`.
  - Loads it instead of the C++ step if present; falls back otherwise.
- Start with one micrograph (the addition example), then scale up to multiple node types.

Good first targets for LLVM wins
- Chains of arithmetic nodes (Add, Multiply).
- Fan-in reducers (sum/avg/max).
- Small fixed topologies where straight-line unrolled code is feasible.
- Multi-step kernels (compute 8–32 ticks per call) to improve throughput in batch tests.

## PLAN: LLVM Backend for NodeFlow

### Why LLVM for NodeFlow
- Specialize per-graph code: remove handle/variant dispatch; inline node logic.
- Constant-folding and DCE: bake graph constants, prune unused nodes/ports.
- Node fusion: collapse short chains into tight kernels to cut memory traffic.
- Target tuning: auto-vectorization (NEON/AVX), unrolling, instruction scheduling.
- Multi-target emission: native (arm64/x86), optional WASM for sandbox sims.
- Closer to theoretical best for static graphs than generic runtime.

### Scope and Goals
- Optional LLVM AOT backend outputting a lib with the same C ABI as `nodeflow_step`.
- Inputs: descriptors (nodes/ports/handles), `outToIn`, topo order, static params.
- Outputs: `nodeflow_init/reset/set_input/step/get_output` implemented in LLVM.
- Non-goals (v1): live graph mutation, JIT hot-swap, distributed scheduling.

### Baseline (for comparison)
- Runtime: SoA handles + ready-queue + generation counters.
- AOT (C++): Typed codegen with descriptors; tight but still C++ call/dispatch.

### Concrete wins (examples)
- Per-graph specialization: `add1 = key1 + key2 + random1;` as straight-line typed code.
- Constant folding: `key2==2.0` baked into adds; unused nodes removed.
- Fusion: `Trigger→Add` fused; fewer loads/stores, better cache use.
- Type specialization: float-only graphs; no runtime type checks.
- Unrolled topo: small DAGs emitted straight-line; loops for motifs.
- Temporal blocking: `nodeflow_step_n` (8–32 ticks) for ILP and lower call overhead.

### Architecture
- Frontend: build a compact IR from descriptors (static at AOT time).
- Lower: emit LLVM IR (or MLIR→LLVM) encoding typed state and a topo-ordered step.
- Optimize: `-O3`, optional LTO/PGO, target `-mcpu=native` where safe.
- Package: produce `.a/.dylib` exporting the same symbols we already use.

### IR shape (sketch)
- Structs: `Inputs`, `Outputs`, `State` with fixed offsets (ABI-stable).
- Globals: compile-time tables for port handles and offsets for schema.
- Functions: `nodeflow_init`, `nodeflow_reset`, `nodeflow_set_input`, `nodeflow_step`, optional `nodeflow_step_n`.
- Add node lowering: typed loads → fadd chain → typed store.

### ABI compatibility
- Preserve current C ABI/layout and descriptor arrays for host/runtime parity.
- LLVM lib is drop-in for `<base>_host` replacing C++ step when available.

### Build & Tooling
- Homebrew `llvm` toolchain (`opt/llc/clang`).
- Paths:
  1) Generate `.ll` → `opt` → `llc` → `clang` link.
  2) Interim: generate high-`-O3` C specialized per-graph.
- CMake: `-DAOT_BACKEND=LLVM`, artifacts in `--out-dir`, target `<base>_step_llvm`.

### Performance tactics
- Auto-vectorization, unrolling, fast-math (configurable), temporal blocking.
- Specialize float-first; emit double/int variants as needed.
- Fuse trivial nodes; elide redundant loads/stores across fused ops.
- Optional PGO on representative workloads.

### Determinism & correctness
- Strict topo order; single-threaded by default.
- Bitwise-deterministic mode (disable fast-math) when required.
- Golden tests: compare runtime vs AOT (C++) vs LLVM for scripted inputs.

### Testing & validation
- Unit tests for handle mapping and parity.
- Perf tests via `--bench-rate/--bench-duration` scripted schedules.
- CI job building with `-DAOT_BACKEND=LLVM` and asserting perf/accuracy thresholds.

### Milestones
1) Prototype: emit typed C, then LLVM IR for the addition graph; export ABI.
2) Coverage: `Value`, `DeviceTrigger`, `Add`, then common arithmetic/logical nodes.
3) Fusion & blocking: simple producer→consumer fusion; add `nodeflow_step_n`.
4) Tooling/docs: CMake flag, `--aot-llvm`, README/PLAN updates.
5) Optional: MLIR frontend, WASM target for sandbox demos.

### Risks & mitigations
- Toolchain mismatch (arm64/x86): pin Homebrew LLVM, set target triple.
- Complexity: keep C++ AOT as fallback/golden reference.
- JIT complexity: start AOT-only; add JIT later if demanded.
- Portability: keep C ABI stable; descriptors remain for hosts.

### Next steps
- Add `--aot-llvm` flag and a skeleton IR generator.
- Emit minimal LLVM kernel for `add1` and compare perf to C++ AOT and runtime.
- Integrate CMake target `<base>_step_llvm` and host linking.


