## Subroutines (Composite Nodes / Modules)

This document proposes a first-class "subroutine" concept for NodeFlow: reusable composite nodes with fixed inputs/outputs, optional parameters, and encapsulated state. Subroutines are compiled efficiently by the AOT generator and run with the same tick/step semantics as the rest of the graph.

### Goals
- Reuse: author once (PID, debounce, low-pass, FSM), instantiate many times.
- Performance: compile to tight code; allow the C/C++ compiler to inline.
- Determinism: fixed I/O shape and state; no dynamic allocations in the hot path.
- Parity: identical semantics in runtime and AOT.

---

## JSON Shapes

There are two complementary ways to define modules. Both produce the same runtime/AOT surface:

### 1) Builtin Module (opaque, codegen-aware)
Compact, no internal nodes are declared; the AOT generator/runtime recognize the `impl` string.

```json
{
  "modules": [
    {
      "id": "PID",
      "impl": "builtin:pid",
      "inputs": [ { "id": "sp", "type": "float" }, { "id": "pv", "type": "float" } ],
      "outputs": [ { "id": "out", "type": "float" } ],
      "defaults": { "kp": 0.3, "ki": 0.1, "kd": 0.01, "min": -5.0, "max": 5.0 }
    }
  ],
  "instances": [ { "id": "pid1", "module": "PID", "params": { "kp": 0.25 } } ]
}
```

### 2) Inline Subgraph Module (explicit internals)
Uses pseudo-nodes `@in:name` and `@out:name` to wire module boundaries to internal nodes.

```json
{
  "modules": [
    {
      "id": "PID",
      "inputs": [ { "id": "sp", "type": "float" }, { "id": "pv", "type": "float" } ],
      "outputs": [ { "id": "out", "type": "float" } ],
      "nodes": [
        { "id": "neg",  "type": "Mul",  "parameters": { "k": -1.0 },
          "inputs": [{"id":"x","type":"float"}], "outputs":[{"id":"y","type":"float"}] },
        { "id": "err",  "type": "Add",
          "inputs": [{"id":"a","type":"float"},{"id":"b","type":"float"}],
          "outputs":[{"id":"y","type":"float"}] },
        { "id": "integ","type": "Integrate","inputs":[{"id":"x","type":"float"}],"outputs":[{"id":"y","type":"float"}] },
        { "id": "deriv","type": "Diff",      "inputs":[{"id":"x","type":"float"}],"outputs":[{"id":"y","type":"float"}] },
        { "id": "p",    "type": "Mul",  "parameters": { "k": 0.3 },
          "inputs": [{"id":"x","type":"float"}], "outputs":[{"id":"y","type":"float"}] },
        { "id": "i",    "type": "Mul",  "parameters": { "k": 0.1 },
          "inputs": [{"id":"x","type":"float"}], "outputs":[{"id":"y","type":"float"}] },
        { "id": "d",    "type": "Mul",  "parameters": { "k": 0.01 },
          "inputs": [{"id":"x","type":"float"}], "outputs":[{"id":"y","type":"float"}] },
        { "id": "sum1", "type": "Add",   "inputs":[{"id":"a","type":"float"},{"id":"b","type":"float"}], "outputs":[{"id":"y","type":"float"}] },
        { "id": "sum2", "type": "Add",   "inputs":[{"id":"a","type":"float"},{"id":"b","type":"float"}], "outputs":[{"id":"y","type":"float"}] },
        { "id": "sat",  "type": "Clamp", "parameters": { "min": -5, "max": 5 },
          "inputs": [{"id":"x","type":"float"}], "outputs":[{"id":"y","type":"float"}] }
      ],
      "connections": [
        { "fromNode": "@in:sp", "fromPort": "out", "toNode": "err",  "toPort": "a" },
        { "fromNode": "@in:pv", "fromPort": "out", "toNode": "neg",  "toPort": "x" },
        { "fromNode": "neg",    "fromPort": "y",   "toNode": "err",  "toPort": "b" },
        { "fromNode": "err",    "fromPort": "y",   "toNode": "p",    "toPort": "x" },
        { "fromNode": "err",    "fromPort": "y",   "toNode": "integ","toPort": "x" },
        { "fromNode": "@in:pv", "fromPort": "out", "toNode": "deriv","toPort": "x" },
        { "fromNode": "integ",  "fromPort": "y",   "toNode": "i",    "toPort": "x" },
        { "fromNode": "deriv",  "fromPort": "y",   "toNode": "d",    "toPort": "x" },
        { "fromNode": "p",      "fromPort": "y",   "toNode": "sum1", "toPort": "a" },
        { "fromNode": "i",      "fromPort": "y",   "toNode": "sum1", "toPort": "b" },
        { "fromNode": "sum1",   "fromPort": "y",   "toNode": "sum2", "toPort": "a" },
        { "fromNode": "d",      "fromPort": "y",   "toNode": "sum2", "toPort": "b" },
        { "fromNode": "sum2",   "fromPort": "y",   "toNode": "sat",  "toPort": "x" },
        { "fromNode": "sat",    "fromPort": "y",   "toNode": "@out:out", "toPort": "in" }
      ],
      "defaults": { "kp": 0.3, "ki": 0.1, "kd": 0.01, "min": -5.0, "max": 5.0 }
    }
  ]
}
```

> Use the builtin form until core/AOT supports the required primitive nodes (Mul, Clamp, Integrate, Diff). The inline form is future-proofed with `@in:*`/`@out:*` boundary pseudo-nodes.

### Instances

```json
{
  "instances": [
    { "id": "pid_motor1", "module": "PID", "params": { "kp": 0.25, "min": -10, "max": 10 } },
    { "id": "pid_motor2", "module": "PID" }
  ],
  "connections": [
    { "fromNode": "setpoint", "fromPort": "out1", "toNode": "pid_motor1", "toPort": "sp" },
    { "fromNode": "sensor",   "fromPort": "out1", "toNode": "pid_motor1", "toPort": "pv" },
    { "fromNode": "pid_motor1","fromPort":"out",  "toNode": "actuator",  "toPort": "in1" }
  ]
}
```

---

## AOT Codegen Strategies

Two approaches (pick one; both can coexist behind a flag):

### A) Function-per-module (recommended first)
- Emit per-module types and functions once, then call per instance.
- C compiler will inline aggressively at -O2/-O3.

Example (PID):

```c
typedef struct { float sp, pv; } PIDInputs;
typedef struct { float out; } PIDOutputs;
typedef struct { double integ; float prevPv; } PIDState;

static inline void pid_tick(PIDState* s, double dt_ms) {
  (void)dt_ms; /* if using time in filters, update here */
}

static inline void pid_step(const PIDInputs* in, PIDOutputs* out, PIDState* s,
                            float kp, float ki, float kd, float minv, float maxv) {
  float err = in->sp - in->pv;
  s->integ += err;             /* clamp if needed */
  float deriv = in->pv - s->prevPv; s->prevPv = in->pv; /* simple D on measurement */
  float u = kp*err + ki*s->integ - kd*deriv;
  if (u < minv) u = minv; else if (u > maxv) u = maxv;
  out->out = u;
}
```

Top-level wiring in generated `<base>_step.cpp`:
- State: `PIDState pid1; PIDState pid2; ...` inside `NodeFlowState`.
- In `nodeflow_tick(dt, in, out, state)`: `pid_tick(&state->pid1, dt); ...`
- In `nodeflow_step(...)`: build `PIDInputs`/`PIDOutputs` views from outer `NodeFlowInputs`/`NodeFlowOutputs`, call `pid_step(...)`, then forward outputs to the instance’s proxy ports.

### B) Flattening (inline expansion)
- Expand module internals into the outer graph with `instance.node` id prefixes.
- Simplest IR-wise; potentially larger generated code for many instances.

---

## Runtime and Descriptors

- Each instance appears as a node exposing ports from its module I/O:
  - Ports `pid1:sp`, `pid1:pv` (inputs) and `pid1:out` (output) are included in `NODEFLOW_PORTS` with proper `dtype`.
- Handles: assigned as usual; no special-casing needed for hosts.
- WebSocket schema: unchanged; UIs can bind to instance ports exactly like normal nodes.

---

## Parameters

- Module `defaults` define baseline constants; `instances[].params` override per-instance.
- Codegen options for passing params to `pid_step`:
  1) Inline constants at call-site (fastest; not reconfigurable without rebuild).
  2) Store per-instance param fields in `NodeFlowState` and pass by value (allows runtime tuning by future control messages).

Recommendation: start with inline constants; later add optional param storage and a `nodeflow_config_instance(instance_handle, key, value)` ABI if runtime reconfiguration is required.

---

## Tick/Step Semantics

- `nodeflow_tick(double dt_ms, ...)` advances time-based state (integrators with time scaling, debouncers, monostables, etc.).
- `nodeflow_step(...)` computes outputs in topological order, reading any state updated by `tick`.
- Modules follow the same contract: `pid_tick` then `pid_step`.

---

## Constraints & Validation

- No recursion; module graphs must be acyclic.
- Fixed I/O types; fan-in/fan-out are static at compile time.
- No blocking I/O in module code; hosts feed inputs via `NodeFlowInputs`.
- Validator checks:
  - Module boundary wiring only uses `@in:*` and `@out:*` pseudo-nodes.
  - All module inputs/outputs are connected exactly once to internal edges.
  - Instance ports match module I/O dtypes.

---

## Example: Demo with Two PID Instances

See `demo3.json` for a concrete example using the builtin PID module with two instances (`pid1`, `pid2`), mocked setpoints/sensors, and a sum of outputs.

---

## Roadmap / Next Steps

1) Loader/IR:
   - Parse `modules[]` and build a module library.
   - Support `instances[]`: either materialize as call nodes (function-per-module) or expand (flattening).
2) AOT Codegen:
   - Emit per-module types and functions; add per-instance state fields and calls.
   - Reflect instance ports into `NODEFLOW_PORTS`.
3) Runtime Parity:
   - Add instance proxy nodes to runtime with identical tick/step semantics.
4) Optional:
   - Runtime instance reconfiguration API; param storage in `NodeFlowState`.
   - Inline subgraph support once primitives (Mul, Clamp, Integrate, Diff) land in core/AOT.


