## Strategy: Generation Counters for High-Performance State Tracking

### Why generation counters?

Generation counters replace repeated clearing of large boolean/flag arrays with a single O(1) increment. This saves time and avoids cache thrash when the number of tracked items (nodes, ports, edges) is large and you need to frequently “reset” visited/dirty state.

### Core pattern

- Keep a global `currentGeneration` (monotonic integer).
- Each tracked item stores a `stamp` (the generation when it was last marked).
- An item is considered “marked” in the current pass if `stamp == currentGeneration`.
- To reset all items at once, increment `currentGeneration`.

```cpp
struct Tracked {
    uint32_t stamp = 0; // last generation this item was marked
};

uint32_t currentGeneration = 1;

inline bool isMarked(const Tracked& t) noexcept { return t.stamp == currentGeneration; }
inline void mark(Tracked& t) noexcept { t.stamp = currentGeneration; }
inline void resetAll() noexcept { ++currentGeneration; } // O(1)
```

### Applying to NodeFlow

We track several states per evaluation cycle:
- Dirty ports (inputs/outputs changed)
- Ready nodes (should be evaluated because a dependency changed)
- Visited nodes (already enqueued/evaluated this cycle)
- Snapshot/delta emission (what changed since last broadcast)

Use separate stamps for clarity:
- `dirtyStamp[portHandle]`
- `readyStamp[nodeHandle]`
- `visitedStamp[nodeHandle]`
- `changedStamp[portHandle]`

And separate generation counters for orthogonal concerns:
- `evalGeneration` for each evaluation wave
- `snapshotGeneration` for each broadcast snapshot

This separation allows you to “reset” evaluation without disturbing snapshot logic, and vice versa.

### Ready queue deduplication

When a port becomes dirty, enqueue its dependent node if not already queued:

```cpp
void onPortDirty(int port, int node) {
    if (readyStamp[node] != evalGeneration) {
        readyStamp[node] = evalGeneration;
        readyQueue.push(node); // stable order maintained elsewhere
    }
}
```

### Dirty propagation

When evaluating a node, only propagate downstream updates if the output actually changed. Record `dirtyStamp` and `changedStamp`:

```cpp
bool writePortIfChanged(int port, float newValue) {
    float& cur = values[port];
    if (cur != newValue) {
        cur = newValue;
        dirtyStamp[port] = evalGeneration;
        changedStamp[port] = snapshotGeneration; // for deltas
        return true;
    }
    return false;
}
```

### Snapshot and delta emission

- On connect, send a `schema` and a full `snapshot`.
- For subsequent frames, emit only ports whose `changedStamp >= lastSentSnapshotGeneration`.

```cpp
void beginSnapshot() { ++snapshotGeneration; }

bool shouldEmit(int port) { return changedStamp[port] >= lastSnapshotGeneration; }
```

After emitting a snapshot, set `lastSnapshotGeneration = snapshotGeneration`.

### Ordering and determinism

- Keep the scheduler single-threaded by default to preserve determinism.
- Use stable ordering for the ready queue (by topological index, then node handle) so the same event sequence → same results.
- Generation counters do not affect ordering; they only avoid unnecessary clears.

### Overflow considerations

- Use `uint32_t` or `uint64_t`. 32-bit gives ~4 billion generations; with 1 kHz cycles that’s ~49 days. 64-bit is effectively unbounded.
- On wrap-around, perform a rare slow-path full clear of stamps and reset generations to 1.

### Memory and cache behavior

- Store stamps in contiguous arrays indexed by handles (cache-friendly).
- Prefer SoA layouts; co-locate stamps with the data they guard for locality.
- Avoid maps/sets on hot paths.

### Concurrency notes

- For single-threaded evaluation: no special handling needed.
- For multi-thread stages: you can use per-stage `evalGeneration` to reduce contention; updates to shared stamps should be atomic or guarded by work partitioning.

### Practical checklist

- [ ] Replace all boolean visited/dirty arrays with generation-stamped arrays.
- [ ] Introduce `evalGeneration` and `snapshotGeneration` counters.
- [ ] Use ready-queue dedup with `readyStamp`.
- [ ] Gate delta emission with `changedStamp` vs `lastSnapshotGeneration`.
- [ ] Add a rare wrap-around handler to reinitialize stamps.
