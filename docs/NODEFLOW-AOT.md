## NodeFlow AOT (Ahead-of-Time) Compilation

### What and Why
- **Purpose**: Convert a JSON-defined NodeFlow graph into a small, typed, optimized step-function library that can run headless in a host process without the full runtime engine.
- **Benefits**:
  - **Performance**: Removes maps/strings/dispatch from the hot path; uses typed arrays and direct calls. In compute-only benchmarks the AOT step is ~100× faster per eval than the generic runtime loop for the demo graph.
  - **Determinism**: Fixed data layout, fixed order; no dynamic parsing or allocation at runtime.
  - **Embeddability**: Host links the static lib and drives it via a tiny C ABI; easy to embed in daemons, services, or other languages via FFI.
  - **Separation of concerns**: Keep I/O, IPC, and UI outside of the kernel; the step library only computes.

### What We Implemented
- **Descriptors and handles** (shared across runtime and AOT):
  - `NodeDesc`, `PortDesc` with integer `handle` per port, `direction`, `dtype`.
  - `outToIn` adjacency, topological order, and per-node output handle lists.
- **AOT generator** (C++):
  - Emits `<base>_step.h/.cpp` and builds `<base>_step` static library automatically via CMake.
  - Exposes descriptor arrays in the header for reflection:
    - `NODEFLOW_NUM_PORTS`, `NODEFLOW_PORTS[]` (nodeId, portId, handle, direction, dtype)
    - `NODEFLOW_NUM_TOPO`, `NODEFLOW_TOPO_ORDER[]`
    - `NODEFLOW_NUM_INPUT_FIELDS`, `NODEFLOW_INPUT_FIELDS[]` (nodeId, offset, dtype)
  - C ABI (drop-in, stable):
    - `nodeflow_init(NodeFlowInputs*, NodeFlowOutputs*, NodeFlowState*)`
    - `nodeflow_reset(NodeFlowInputs*, NodeFlowOutputs*, NodeFlowState*)`
    - `nodeflow_set_input(int handle, double value, NodeFlowInputs*, NodeFlowState*)`
    - `nodeflow_tick(double dt_ms, const NodeFlowInputs*, NodeFlowOutputs*, NodeFlowState*)`  ← advances time
    - `nodeflow_step(const NodeFlowInputs*, NodeFlowOutputs*, NodeFlowState*)`
    - `double nodeflow_get_output(int handle, const NodeFlowOutputs*, const NodeFlowState*)`
- **Host example** (`aot_host_template.cpp`):
  - CLI11 options: `--rate`, `--duration`, `--ws-enable`, `--ws-port`, `--ws-path`, `--set node=value`, `--list`.
  - Optional WebSocket server (Simple-WebSocket-Server, ASIO, OpenSSL):
    - Sends `schema` (ports list), `snapshot` (all outputs), and `delta` (changed outputs).
    - Accepts `set` (by `node` or `handle`) and `subscribe`.
  - Compute-only benchmark mode: `--bench`, `--bench-rate`, `--bench-duration`, `--perf-out`, `--perf-interval`.
  - Thread-safe broadcasting and snapshot generation; persistent endpoint regex key.
- **Runtime parity**:
  - Runtime also emits `schema`, `snapshot`, `delta` via WS, and now includes a compute-only benchmark mode for apples-to-apples comparisons.
  - Time support in runtime: `FlowEngine::tick(double dtMs)` mirrors AOT `nodeflow_tick` and is called each loop with the real elapsed milliseconds.

### Time and Scheduling (Timers/Counters)
- `nodeflow_tick(dt_ms, in, out, state)` advances time-based nodes using the elapsed time in milliseconds since the previous call.
  - Timer nodes accumulate `dt_ms` and emit a one-tick pulse (value 1 in their declared dtype) when their `interval_ms` is reached; otherwise 0.
  - Counter nodes increment on rising edges of their configured input (pulse 0→1), maintaining count in state.
- Typical loop order in host/runtime:
  1) compute `dt_ms` since last iteration
  2) call `nodeflow_tick(dt_ms, ...)`
  3) call `nodeflow_step(in, out, state)` to evaluate the graph
- Pulses are transient per tick: they reset to 0 on the next `nodeflow_tick` unless re-emitted by the timer.
- The runtime uses the same pattern: `engine.tick(dtMs); engine.execute();`

### How-To (Generate, Build, Run)
- **Prerequisites (macOS/Homebrew)**:
  - `brew install nlohmann-json asio openssl@3 cmake fmt cli11`
  - Ensure Homebrew OpenSSL (arm64) is used; see `CMakeLists.txt` diagnostics if linking fails.

- **Build project**:
```bash
cmake -S . -B build && cmake --build build
```

- **Generate AOT from a flow**:
```bash
# Option A: default out dir (project root)
./build/NodeFlowCore --flow devicetrigger_addition.json --build-aot

# Option B: choose an out dir
./build/NodeFlowCore --flow devicetrigger_addition.json --build-aot --out-dir build
```
This produces `<base>_step.h/.cpp` (e.g., `devicetrigger_addition_step.*`). CMake auto-detects and builds a static library `<base>_step` and a matching host `<base>_host`.

- **Run the AOT host**:
```bash
# Print schema and run a timed loop
./build/devicetrigger_addition_host --rate 60 --duration 5

# Enable WS for web UI parity
./build/devicetrigger_addition_host --ws-enable --ws-port 9002 --ws-path /stream

# Set inputs on the command line
./build/devicetrigger_addition_host --set key1=1 --set key2=2 --set random1=42
```
- The host drives time by computing `dt_ms` each iteration and calling `nodeflow_tick(dt_ms, ...)` before `nodeflow_step(...)`. The runtime (`NodeFlowCore`) does the same via `FlowEngine::tick(dtMs)` followed by `execute()`.

- **Web UI**:
  - `web/index.html`: original controls (checkboxes/slider), improved colored log.
  - `web/generic.html`: schema-driven generic viewer; connects to `ws://localhost:9002/stream`.

### Benchmarks (Compute-Only, No WS/UI)
- **Runtime** (engine) vs **AOT host** (step) with the same feeder:
```bash
# Runtime compute-only
./build/NodeFlowCore --flow devicetrigger_addition.json \
  --bench --bench-rate 1000 --bench-duration 10 \
  --perf-out build/runtime.ndjson

# AOT compute-only
./build/devicetrigger_addition_host \
  --bench --bench-rate 1000 --bench-duration 10 \
  --perf-out build/aot.ndjson
```
Each writes NDJSON perf summaries (every second by default). Example lines:
```json
{"type":"perf","evalCount":774,"evalTimeNsAccum":21845512, ...}
{"type":"perf","evalCount":792,"evalTimeNsAccum":170217, ...}
```
- **Interpretation**:
  - Runtime avg per eval ≈ 21,845,512 ns / 774 ≈ 28.2 µs
  - AOT avg per eval ≈ 170,217 ns / 792 ≈ 0.215 µs
  - AOT is roughly two orders of magnitude faster in compute-only mode for the sample graph.

### WS Protocol (Runtime & AOT Host)
- **schema**: ports array with `{handle,nodeId,portId,direction,dtype}`
- **snapshot**: full values map using both `nodeId:portId` and single-output aliases `nodeId`.
- **delta**: only changed outputs since the last snapshot/delta generation.
- **ok**: small `{"ok":true}` responses to control messages.

### ABI and Introspection
- **C ABI (step lib)**:
  - `nodeflow_init`, `nodeflow_reset`, `nodeflow_set_input`, `nodeflow_step`, `nodeflow_get_output`.
- **Descriptors** (in `<base>_step.h`):
  - `NODEFLOW_PORTS[]`: list for schema; hosts can reflect and build UIs or bindings dynamically.
  - `NODEFLOW_INPUT_FIELDS[]`: tells the host where to write inputs into `NodeFlowInputs`.
- **Data layout**:
  - Inputs, outputs, and state are POD structs with fixed offsets; generated code reads/writes by offset directly.

### Limitations & Notes
- **Graph updates**: Changing the flow requires regenerating the step library (like current AOT). No dynamic node additions/removals at runtime.
- **Node coverage**: Typed implementations exist for `Value`, `DeviceTrigger`, `Add` in the fast path; other node types may use a generic path until specialized.
- **Determinism**: Single-threaded step; stable topo order. For the runtime, ready-queue preserves deterministic ordering.
- **Types**: Current focus is numeric (`int`, `float`, `double`); strings supported in runtime snapshot paths but not optimized in AOT step.

### Troubleshooting
- **WS shows connected but no updates**: Ensure the correct endpoint regex key is used for broadcasting (both runtime and host store the compiled `^/stream$` key and use it for lookups).
- **Port 9002 in use**: Find and kill the process:
```bash
lsof -nP -iTCP:9002 -sTCP:LISTEN
for pid in $(lsof -tiTCP:9002 -sTCP:LISTEN); do kill -TERM "$pid"; done
```
- **OpenSSL arch mismatch** (x86_64 vs arm64):
  - Use Homebrew arm64 OpenSSL (`/opt/homebrew/opt/openssl@3`).
  - Set `OPENSSL_ROOT_DIR` if `find_package(OpenSSL)` fails; see CMake diagnostics.
- **Asio headers**: `brew install asio`; CMake shows `Using Asio include dir: ...` if found.
- **Flow not found**: Use `--flow /absolute/path/...json` or place the JSON at project root.

### Future Work (Planned)
- **LLVM backend**: See `PLAN-LLVM.md`. Generate LLVM IR or high-`-O3` C for per-graph specialization, fusion, and temporal blocking; produce a drop-in step lib with the same ABI.
- **Delta coalescing**: Rate-limit and coalesce WS deltas for lower bandwidth.
- **Quantiles**: Add p50/p95/p99 reporting to perf summaries.
- **Step blocking**: `nodeflow_step_n(in,out,state,n)` to amortize call overhead and expose more ILP.
- **Unit tests**: Parity tests (runtime vs AOT), handle mapping, determinism checks.

### File & Target Overview
- **Generated**: `<base>_step.h`, `<base>_step.cpp`, static lib `<base>_step`.
- **Host**: `<base>_host` binary linking the step lib.
- **Docs**: `PLAN.md`, `STRATEGY.md`, `STRATEGY-GenerationCounters.md`, `PLAN-LLVM.md`, this `NODEFLOW-AOT.md`.
- **Web**: `web/index.html` (original controls), `web/generic.html` (schema-driven).

### Benchmark

- AOT vs runtime throughput:
  - Runtime avg per eval ≈ 21,845,512 ns / 774 ≈ 28.2 µs (example L5).
  - AOT avg per eval ≈ 170,217 ns / 792 ≈ 0.215 µs (example L3).
  - AOT is roughly 100–150x faster per eval in compute-only mode.

- Variability:
  - Runtime min/max per-interval span ≈ 10–80 µs min; up to ≈ 0.30 ms max.
  - AOT min as low as 42 ns; typical max < 1 µs, rare spike to 13.5 µs (L5).

- Scheduler counters (runtime):
  - nodesEvaluated ≈ evalCount each second → consistent per-tick work.
  - dependentsEnqueued ≈ evalCount; readyQueueMax=0 suggests small transient queue or immediate drain each tick (expected for this tiny graph).

- What this shows:
  - The SoA runtime is performant but has overhead (graph bookkeeping, C++ variants).
  - The AOT step is a very tight function call with near-zero overhead, hence ~ns-level evals.

Suggestions to refine comparison (optional next):
- Add quantiles: keep a ring buffer of per-eval times and report p50/p95/p99 (not just min/max/mean).
- Normalize by work: also report avg nodesEvaluated per eval to get per-node time.
- IPC-inclusive runs: repeat with --bench-ws to measure WS send time and bytes (separates compute vs IPC).
