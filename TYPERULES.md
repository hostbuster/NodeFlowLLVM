## NodeFlow Type Rules

### Base types
- Numeric scalar types only: `int`, `float`, `double`.
- No `async_` prefixes (deprecated). Types in JSON are literal.

### Coercion and compatibility
- An output may connect to an input of a different numeric type.
- Numeric coercions are allowed across edges: `int < float < double`.
- Non-numeric or mixed-with-non-numeric types are rejected.

### Runtime semantics (core)
- Storage: values are held in `std::variant<int,float,double,std::string>`; strings are not used in the compute path.
- Propagation (edge write): when writing an output value to a downstream input edge, the value is cast to the destination port’s declared dtype.
- Node execution:
  - Inputs are read and cast to the node’s compute dtype.
  - Outputs are written in each output port’s declared dtype.
- Deltas: comparisons are exact for `int`, and value-sensitive (stringified) for floats/doubles (epsilon filter optional via WS flags).

### Node-specific rules
- DeviceTrigger
  - Host/runtime writes the node’s output in the declared dtype.
  - Typical shapes: `int`, `float`, `double`.
- Value
  - Emits the `parameters.value` cast to the declared output dtype.
- Timer
  - Internal accumulator is `double` milliseconds.
  - Emits pulses (0→1→0) at `parameters.interval_ms` in the declared output dtype.
  - Runtime: 1 is set in `tick(dt)` when firing; reset to 0 between pulses.
- Counter
  - Rising-edge detector on its first input.
  - Internal count held as `double` for uniformity; output is cast to the declared dtype on write.
- Add
  - Compute dtype defaults to the output port’s declared dtype.
  - Each input is cast to compute dtype before summation.
  - Result is written in the declared output dtype.

### AOT C++ generator
- NodeFlowState
  - Timers: `double acc_<id>; <dtype> tout_<id>;`
  - Counters: `int last_<id>; double cnt_<id>;`
- `nodeflow_tick(double dt_ms, ...)`
  - Timers: update `acc_`; when firing, set `tout_<id>` to 1 cast to `<dtype>`, else 0 cast to `<dtype>`.
  - Counters: `tick = (tout_src > 0.5)`; on rising edge increment `cnt_<id>`; update `last_<id>`.
- `nodeflow_step(...)`
  - DeviceTrigger: `out = in->nodeId` (declared dtype).
  - Timer: `out = s->tout_<id>`.
  - Counter: `out = (<dtype>)s->cnt_<id>`.
  - Add: for each upstream source temp `_src`, cast to output dtype before adding; write in output dtype.
- `nodeflow_get_output(handle, ...)`
  - Returns Timer and Counter outputs via state; sinks via `out`.
  - DeviceTrigger outputs are not returned here (not available in this ABI).

### JSON expectations
- Port `type` values must be one of: `int`, `float`, `double`.
- Mixed numeric connections are allowed; the core casts at edges and nodes as needed.
- Non-numeric ports are not supported in compute paths.

### Error handling
- On load: non-numeric mismatches (`string` or unknown types) across a connection cause `Type mismatch in connection`.
- During execution: failed casts are not expected for supported numeric types; inputs absent (unconnected) default to 0 in Add.

### Best practices
- Prefer `float` for most compute unless you need `double` precision or `int` semantics.
- For timers/counters, choose output dtype based on downstream expectations (e.g., `int` for counters).
- When summing heterogeneous inputs, set the Add output dtype explicitly to the desired numeric rank.
- Keep UI bindings generic; use schema (`dtype`) to render and format values correctly.


