## NodeFlow: Asynchronous Dataflow Framework

NodeFlow is a C++ framework for creating and executing dataflow graphs with asynchronous nodes, designed to simulate real-time systems like device triggers. It compiles flows to standalone executables via LLVM for high performance and portability. The demo shows three async inputs (key1, key2, random1) whose values are summed in real-time using ncurses for keyboard input and a background random-number thread.

### Key Features

- **Asynchronous Nodes**: Event-driven inputs (keyboard, timers) implemented with coroutines for non-blocking execution.
- **JSON Configuration**: Flows are defined in `devicetrigger_addition.json` (nodes, connections, parameters like key triggers and random intervals).
- **Real-Time Updates**: Outputs update dynamically on key presses and on periodic random triggers.
- **LLVM Compilation**: Generates optimized native code with a DSL-like `run_flow` entry point for easy integration.
- **ncurses Integration**: Real-time keyboard input for interactive terminal applications.

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
brew install llvm ncurses nlohmann-json
```

Note: On macOS, ncurses is provided by Homebrew. CMake links `${CURSES_LIBRARIES}` (typically under `/opt/homebrew/opt/ncurses` on Apple Silicon or `/usr/local/opt/ncurses` on Intel).

#### Build

```bash
mkdir build && cd build

# Ensure Homebrew's LLVM is used
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"   # Apple Silicon
# or
export PATH="/usr/local/opt/llvm/bin:$PATH"      # Intel

cmake .. -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
make -j
```

#### Run

```bash
./NodeFlowCore
```

- **Interact**: Press `1` or `2` to trigger `key1` (1.0) or `key2` (2.0). `random1` updates every 100–500 ms with a value in `[0, 100]`. Press `q` to quit.
- **Output**: The ncurses UI shows the current sum and node statuses.
- **Generated Executable**: The flow is compiled to `nodeflow_output` in the build directory and can be run independently: `./nodeflow_output`.

### Files

- `devicetrigger_addition.json`: Defines the dataflow (two keyboard triggers, one random trigger, one add node).
- `NodeFlowCore.hpp`: Core framework structures and interfaces.
- `NodeFlowCore.cpp`: Node execution and LLVM compilation.
- `main.cpp`: ncurses UI, random-number thread, JSON loading, and compilation step.
- `CMakeLists.txt`: Build configuration for LLVM, nlohmann-json, and ncurses.

### Notes & Troubleshooting

- **macOS + ncurses**: Uses Homebrew’s ncurses for terminal input.
- **LLVM in PATH**: Ensure Homebrew’s LLVM is first in PATH before running CMake.
- **Linking errors (ncurses)**: Verify Homebrew paths in CMake or run `brew link ncurses`.
- **LLVM errors**: Confirm you are using Homebrew’s LLVM (`brew info llvm` for paths).
- **Config file**: Ensure `devicetrigger_addition.json` is in the project root.

NodeFlow showcases how LLVM and a DSL streamline async, event-driven applications on macOS, with a path to real-world device integration and scalable data processing.