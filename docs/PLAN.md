## NodeFlow Plan: Headless Runtime and Codegen Options

### Goals
- Headless-first execution for servers/agents.
- Optional UI runs in a separate process (or another machine) and attaches via IPC.
- Keep runtime semantics and generated output consistent.
- Preserve portability and performance.

### Option A — Current Runtime Engine (Headless Core)
- Description: Interpret JSON, build node graph in memory, and execute a scheduler loop.
- IO/Timers: Provided by host (threads, timers) or adapters; UI optional and separate.
- Pros:
  - Simple deploy; one binary reads JSON.
  - Highly dynamic: hot-reload/replace config without rebuild.
  - Easy debugging (introspection, logs).
- Cons:
  - Slight runtime overhead vs. AOT.
  - Requires JSON and parsing at startup.
- Performance notes:
  - Use topological order and preallocated maps/vectors to minimize allocations.
  - Consider lock-free queues for inter-node async messaging if needed.

### Option B — AOT Step-Function Library (Hybrid, Recommended)
- Description: Generate a small static/shared library that exports a single step function:
  - C API: `nodeflow_step(const Inputs* in, Outputs* out, State* state)`
  - Inputs: key flags, random-ready flag, random value, etc.
  - Outputs: mapped node outputs by ID/index (e.g., `add1` result).
  - State: per-node persistent state across ticks.
- IO/Timers: Owned by host. No threads/UI/timers in generated code.
- Pros:
  - Headless and portable; no platform deps in the AOT output.
  - Near-runtime semantics; easy to keep in sync with a small generator.
  - Deterministic and testable; small binary.
  - Host can run UI in separate process talking over IPC to this step API.
- Cons:
  - Requires a codegen step at build-time.
  - Slightly more complex build (need to compile the generated lib).
- Performance notes:
  - Inline simple ops (e.g., Add) and fold constants at codegen time.
  - Expose fixed-layout structs for zero-copy inputs/outputs.

### Option C — Full AOT Interactive Binary (No UI, but self-timed)
- Description: Generate a standalone headless binary that implements timers/threads internally and runs its own main loop.
- IO/Timers: Implemented inside the generated code (e.g., random thread, key polling adapter) but no UI.
- Pros:
  - Single binary; minimal host logic needed.
- Cons:
  - Harder to embed into larger systems (less control over timing/lifecycle).
  - Platform-specific threading/timing surface in generated code.
- Performance notes:
  - Similar to runtime; minimal overhead once running.

### Option D — Kernels-Only AOT + Runtime Loop
- Description: Generate only compute kernels (e.g., `Add`, `Mul`, etc.) and plug them into the existing runtime scheduler.
- IO/Timers: Provided by runtime/host; JSON parsing and graph orchestration remain dynamic.
- Pros:
  - Small generator surface; reuse runtime loop.
  - Gains from compiled kernels while keeping dynamic wiring.
- Cons:
  - Two integration points (runtime + generated kernels).
  - Less end-to-end specialization than Option B.

### UI Strategy (Separate Process)
- The core (runtime or AOT step lib) runs headless.
- UI process (ncurses/GUI/web) communicates via IPC:
  - Options: UNIX domain socket, TCP, or stdio pipes.
  - Protocol: newline-delimited JSON or flat binary with fixed structs.
- Benefits:
  - Crash isolation, portability, and clean layering.
  - Multiple UIs can attach to the same headless core.

### IPC Sketch
- Headless core exposes:
  - subscribe(outputs): stream of output updates
  - publish(inputs): apply external events each tick
- Minimal protocol example (NDJSON):
  - `{ "type": "inputs", "key1": true }`
  - `{ "type": "outputs", "add1": 3.0 }`

### Performance Considerations
- Hot path:
  - Precompute execution order once; avoid per-tick allocations.
  - Use contiguous storage for ports; store indices instead of strings.
  - Batch apply inputs, then run nodes, then batch emit outputs.
- AOT advantages (Options B/D):
  - Inline simple nodes, constant fold value nodes.
  - Remove unused nodes/ports at codegen.

### Recommended Path
- Adopt Option B (AOT Step-Function Library) as primary codegen target:
  - `nodeflow_codegen_lib` CMake target producing `libnodeflow_step.(a|dylib)` and `nodeflow_step.h`.
  - Ship a tiny host runtime (headless) that:
    - loads/initializes State
    - receives inputs (e.g., from device/IPC)
    - calls `nodeflow_step` each tick
    - publishes outputs to logs/IPC
- Keep the current runtime engine for rapid iteration and as reference semantics.
- Optional: add Option D later for compiled kernels if needed.

### Action Items
- Codegen: emit `nodeflow_step.h/.cpp` with:
  - Fixed structs for Inputs/Outputs/State (generated from the graph)
  - Deterministic node order and direct field access (no maps)
  - C API wrapper for C++ symbol stability
- CMake:
  - `-DNODEFLOW_CODEGEN=ON` (default) builds the step library target
  - Install or copy artifacts to `build/` for host linking
- Host example (headless):
  - Minimal loop reading inputs from stdin/IPC
  - Calls `nodeflow_step`, writes outputs to stdout/IPC
- UI example (separate process):
  - Subscribes to outputs over socket; sends input events on keypress

### Notes
- No UI in generated code; UI is always external.
- Keep JSON optional at runtime: embed config at codegen time, or allow external file for flexibility.
- Ensure stable ABI for the generated C API to ease integration across languages.
