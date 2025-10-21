## NodeFlow: Asynchronous Dataflow Framework

NodeFlowLLVM is a C++ framework for creating and executing dataflow graphs with asynchronous nodes, designed to simulate real-time systems like device triggers. The core is headless and exposes a WebSocket (WS) interface; a minimal web UI simulates devices. We also support Ahead-of-Time (AOT) generation of small step-function libraries (C++ and LLVM-IR variants) for embedding.

### Key Features

- **Headless core**: No ncurses/UI in the runtime; simulation is via WS from the web client.
- **JSON Configuration**: Flows are defined in `*.json` (nodes, connections, parameters). The example is `devicetrigger_addition.json`.
- **SoA engine & determinism**: Handle-indexed arrays, generation counters, and a deterministic ready-queue scheduler.
- **WebSocket IPC**: Schema/snapshot/delta streaming to a web UI; UI can send `set` commands back.
- **AOT step libraries**: Generate `<base>_step` (typed C++) or `<base>_step_llvm` (LLVM-IR) for embedding.

### Why AOT (and optionally LLVM)

- **Performance**: Typed, per-graph code with direct loads/stores; no string/maps/variant dispatch in the hot path.
- **Embeddability**: A tiny C ABI (`nodeflow_step`) driven by a host; easy to integrate.
- **Specialization**: Optionally compile LLVM IR for target-tuned kernels and multi-step variants.

### Dependencies (macOS)

- Homebrew packages: `nlohmann-json`, `asio`, `openssl@3`, `cmake`, `fmt`, `llvm` (optional for IR toolchain)
- CLI11 is fetched automatically if not installed

### Build and Run (macOS)

#### Install prerequisites

```bash
# Install Homebrew if needed
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Dependencies
brew install nlohmann-json asio openssl@3 cmake fmt llvm
```

Note: On macOS Apple Silicon, prefer Homebrew at `/opt/homebrew`. We link against `openssl@3` and use standalone `asio`.

#### Build (runtime + AOT enabled by default)

```bash
mkdir build && cd build

# Ensure Homebrew's LLVM is used
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"   # Apple Silicon
# or
export PATH="/usr/local/opt/llvm/bin:$PATH"      # Intel

cmake .. -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
make -j
```

#### Run (runtime, headless + WebSockets)

```bash
./NodeFlowCore --flow devicetrigger_addition.json
```

- **Interact** (web UI): open `web/index.html` in your browser; it connects to `ws://127.0.0.1:9002/stream`.
  - Toggle checkboxes for `key1` and `key2` to send values (1.0/2.0) to the core.
  - Use the slider for `random1` (0–100) to send updates.
  - The UI displays values received from the core and the `add1` result.

### Runtime CLI

- Core options
  - `--flow <path>`: flow JSON (default `devicetrigger_addition.json`, fallback search `.` `..` `../..`).
  - `--build-aot`: generate AOT step library and exit.
  - `--aot-llvm`: use LLVM-style backend for AOT generation (emits `<base>_step.ll` + glue).
  - `--out-dir <dir>`: output directory for AOT files.
- WebSocket options
  - `--ws-port <int>` (default 9002)
  - `--ws-path <string>` (default `/stream`)
- Benchmark & perf
  - `--bench`: compute-only mode (no WS); feeds inputs in-process.
  - `--bench-rate <hz>`, `--bench-duration <sec>`
  - `--perf-out <file.ndjson>`, `--perf-interval <ms>`
- Delta aggregation (WS)
  - `--ws-delta-rate-hz <hz>`: 0=immediate (default 60)
  - `--ws-delta-max-batch <n>`: cap keys per delta (default 512)
  - `--ws-delta-epsilon <float>`: drop tiny float diffs (default 0)
  - `--ws-heartbeat-sec <sec>`: idle heartbeat (default 15)
  - `--ws-delta-fast`: send an immediate tiny delta on set (default on)

Example:
```bash
./build/NodeFlowCore --flow devicetrigger_addition.json --ws-delta-rate-hz 60 --ws-delta-fast
```

### AOT step library (Option B)

When `--build-aot` is used, the engine emits a small, JSON-driven step-function interface:
- `<base>_step.h/.cpp` (typed C++):
  - `NodeFlowInputs` (one field per `DeviceTrigger`)
  - `NodeFlowOutputs` (one field per sink)
  - `nodeflow_step(const NodeFlowInputs*, NodeFlowOutputs*, NodeFlowState*)`
- With `--aot-llvm`, also emit `<base>_step.ll` and `<base>_step_desc.cpp`; CMake builds `<base>_step_llvm` and `<base>_host_llvm`.

Build integration:
- CMake auto-builds any `*_step.cpp` into `lib<base>_step.a` and `<base>_host`.
- For LLVM IR, CMake compiles `*_step.ll` and links with `<base>_step_desc.cpp` into `<base>_step_llvm` and `<base>_host_llvm`.

Usage:

```bash
# Build only AOT libs/hosts (no interactive runtime)
cmake -S . -B build -DNODEFLOW_BUILD_RUNTIME=OFF && cmake --build build

# Run host demo (fields depend on your flow; for the demo graph):
./build/devicetrigger_addition_host --key1=1 --key2=2 --random1=3
```

### Runtime details

- Inputs are driven over WS by the UI; no ncurses/keyboard dependency.
- Deterministic ready-queue scheduler; SoA storage; generation counters for O(1) dirty tracking.
- Snapshots/deltas streamed generically from descriptors; UI binds dynamically.

### WebSocket protocol + Web UI

- Run runtime and open the web client:

```bash
./build/NodeFlowCore --flow devicetrigger_addition.json
# Then open web/index.html (connects to ws://127.0.0.1:9002/stream)
```

- Messages are NDJSON (one JSON per line):
  - `{"type":"schema","ports":[{handle,nodeId,portId,direction,dtype},...]}`
  - `{"type":"snapshot", "node:port": value, ...}` plus plain `"node"` alias for single-output nodes
  - `{"type":"delta", "node:port": value, ...}` compact changes since last eval (coalesced)
  - `{"ok":true}` small ACKs to control commands
  - `{"type":"heartbeat"}` idle keepalive
- Client → server controls:
  - `{"type":"set","node":"key1","value":1.0}` or `{"type":"set","handle":0,"value":1.0}`
  - `{"type":"subscribe"}` (optional)
  - `{"type":"config","node":"random1","min_interval":100,"max_interval":300}`

### Build options

- `-DNODEFLOW_CODEGEN=ON|OFF` (default ON): include demo standalone codegen stub.
- `-DNODEFLOW_BUILD_RUNTIME=ON|OFF` (default ON): build the interactive runtime.
- `-DAOT_BACKEND_LLVM=ON|OFF` (default OFF): define `NODEFLOW_AOT_LLVM` for CLI plumbing.

### Files

- `devicetrigger_addition.json`: Defines the dataflow (two keyboard triggers, one random trigger, one add node).
- `NodeFlowCore.hpp`: Core framework structures and interfaces.
- `NodeFlowCore.cpp`: Node execution, SoA scheduler, AOT code generators (C++ & LLVM IR emitter).
- `main.cpp`: CLI (CLI11), JSON load, WS server, generic schema/snapshot/delta, perf & delta aggregation.
- `aot_host_template.cpp`: Minimal AOT host (CLI11); can run timed loops or serve WS. Supports `--help` and `--help-all`.
- `CMakeLists.txt`: Build configuration for nlohmann-json, WebSockets (Asio + OpenSSL@3), and optional LLVM demo codegen.
- `web/index.html`: Minimal WebSocket client to visualize live values.
- `third_party/Simple-WebSocket-Server`: Header-only WebSocket server library (HTTP(S)/WS(S)) used for streaming.

### Notes & Troubleshooting

- **OpenSSL arch**: Prefer `/opt/homebrew/opt/openssl@3` on Apple Silicon; see CMake diagnostics.
- **Asio headers**: `brew install asio`; CMake prints the include dir if found.
- **WS port busy**: `lsof -nP -iTCP:9002 -sTCP:LISTEN` then kill offending PID.
- **Flow file**: Use `--flow /absolute/path.json` or place JSON at project root.


NodeFlow showcases how LLVM and a DSL streamline async, event-driven applications on macOS, with a path to real-world device integration and scalable data processing.