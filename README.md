## NodeFlow: Asynchronous Dataflow Framework

NodeFlowLLVM is a C++ framework for creating and executing dataflow graphs with asynchronous nodes, designed to simulate real-time systems like device triggers. It compiles flows to standalone executables via LLVM for high performance and portability. The demo shows three async inputs (key1, key2, random1) whose values are summed in real-time; devices are simulated via a WebSocket-powered web UI.

### Key Features

- **Asynchronous Nodes**: Event-driven inputs (keyboard, timers) implemented with coroutines for non-blocking execution.
- **JSON Configuration**: Flows are defined in `devicetrigger_addition.json` (nodes, connections, parameters like key triggers and random intervals).
- **Real-Time Updates**: Outputs update dynamically on key presses and on periodic random triggers.
- **LLVM Compilation**: Generates optimized native code with a DSL-like `run_flow` entry point for easy integration.
- **WebSocket Streaming (optional)**: Stream node updates and sums to a web UI via Simple-WebSocket-Server.
  - The web UI also sends input events (simulate devices) to the core.

### Why LLVM + a DSL

- **Performance**: LLVM optimizations (e.g., `fadd` for float addition) approach hand-written performance.
- **Portability**: Targets any LLVM-supported platform (x86, ARM, etc.) without rewriting code.
- **DSL Simplicity**: A generated `run_flow` function abstracts graphs, coroutines, and event loops behind a simple call interface.
- **Flexibility**: Coroutine support enables async nodes that suspend/resume efficiently.

This eliminates manual event-loop coding and enables rapid prototyping of complex async systems.

### Potential Enhancements

- **GUI Integration**: Replace ncurses with a Cinder-based macOS UI to visualize node states.
- **Extended Types**: Add `async_int`, `async_double`, `async_string` for broader use-cases.
- **Persistent State**: Keep node values across runs for stateful behavior (e.g., cumulative sums).
- **Real Devices**: Swap keyboard/random inputs for actual device signals (e.g., sensors over TCP).
- **Optimizations**: Apply LLVM passes (inlining, unrolling) for further gains.
- **Parallelism**: Explore multi-threaded or distributed node processing.

### Build and Run (macOS)

#### Install prerequisites

```bash
# Install Homebrew if needed
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Dependencies
brew install llvm nlohmann-json asio openssl@3 cmake fmt
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

### New: Command-line flags

- `--flow <path>`: specify the flow JSON file. If omitted, defaults to `devicetrigger_addition.json` with fallback search in `.`/`..`/`../..`.
- `--build-aot`: generate an AOT step-function library and exit. Files are named `<basename>_step.h/.cpp` (basename from the JSON filename).
- `--out-dir <dir>`: when used with `--build-aot`, write generated files into the directory.
  
WebSockets are always enabled; the server runs by default. You can configure:
- `--ws-port <int>`: port (default 9002)
- `--ws-path <string>`: path (default `/stream`)

Examples:

```bash
# Generate AOT-only artifacts (no runtime launch), writing to build/aot
./NodeFlowCore --flow devicetrigger_addition.json --build-aot --out-dir build/aot
```

### AOT step library (Option B)

When `--build-aot` is used, the engine emits a small, JSON-driven step-function interface:
- `<basename>_step.h` defines:
  - `NodeFlowInputs` (fields for each `DeviceTrigger` node)
  - `NodeFlowOutputs` (fields for sink nodes)
  - `nodeflow_step(const NodeFlowInputs*, NodeFlowOutputs*, NodeFlowState*)`
- `<basename>_step.cpp` implements evaluation in topological order.

Build integration:
- CMake auto-builds any `*_step.cpp` in the source dir into a static library `build/lib<basename>_step.a`.
- A minimal host example `<basename>_host` is built to demonstrate calling `nodeflow_step`.

Usage:

```bash
# Build only AOT libs/hosts (no interactive runtime)
cmake -S . -B build -DNODEFLOW_BUILD_RUNTIME=OFF && cmake --build build

# Run host demo (fields depend on your flow; for the demo graph):
./build/devicetrigger_addition_host --key1=1 --key2=2 --random1=3
```

### Runtime details

- Non-blocking `DeviceTrigger` nodes:
  - Inputs are driven by the web UI over WebSockets (no ncurses / keyboard dependency).
  - Random can be driven from the UI; internal timers can be enabled per flow if needed.
- Values persist between ticks; the sum updates when any input changes.
- UI displays per-node values and both a calculated sum and the engine sum for verification.

### WebSocket streaming + Web UI

- Run runtime and open the web client:

```bash
./build/NodeFlowCore --flow devicetrigger_addition.json
# Then open web/index.html (connects to ws://127.0.0.1:9002/stream)
```

- The runtime broadcasts compact NDJSON-style JSON snapshots with current node values (key1, key2, random1) and `add1`.

### Build options

- `-DNODEFLOW_CODEGEN=ON|OFF` (default ON): include the demo standalone codegen (`nodeflow_output`). We will iterate on LLVM/codegen next.
- `-DNODEFLOW_BUILD_RUNTIME=ON|OFF` (default ON): build the interactive runtime `NodeFlowCore`.

### Files

- `devicetrigger_addition.json`: Defines the dataflow (two keyboard triggers, one random trigger, one add node).
- `NodeFlowCore.hpp`: Core framework structures and interfaces.
- `NodeFlowCore.cpp`: Node execution and LLVM compilation.
- `main.cpp`: JSON loading, WebSocket server, and codegen hooks.
- `CMakeLists.txt`: Build configuration for nlohmann-json, WebSockets (Asio + OpenSSL@3), and optional LLVM demo codegen.
- `web/index.html`: Minimal WebSocket client to visualize live values.
- `third_party/Simple-WebSocket-Server`: Header-only WebSocket server library (HTTP(S)/WS(S)) used for streaming.

### Notes & Troubleshooting

- **LLVM in PATH**: Ensure Homebrew’s LLVM is first in PATH before running CMake.
- **LLVM errors**: Confirm you are using Homebrew’s LLVM (`brew info llvm` for paths).
- **Config file**: Ensure `devicetrigger_addition.json` is in the project root.

NodeFlow showcases how LLVM and a DSL streamline async, event-driven applications on macOS, with a path to real-world device integration and scalable data processing.