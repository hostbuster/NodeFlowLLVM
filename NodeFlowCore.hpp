// NodeFlow core types and engine
//
// This header defines the in-memory graph (nodes, ports, connections) and a
// small execution engine that can load a JSON flow, evaluate it, and expose
// descriptors/handles so higher layers (runtime, WS, AOT) can interact in a
// generic way. The engine is written to move toward a SoA (structure-of-arrays)
// layout for performance and determinism.
#pragma once
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <unordered_map>
#include <variant>
#include <memory>

namespace NodeFlow {

using NodeId = std::string;
using PortId = std::string;
// Scalar value that can flow on ports. Extend here to add more types.
using Value = std::variant<int, float, double, std::string>;
using PortHandle = int;
using Generation = unsigned long long;

// Represents a port (input or output) declared on a node
struct Port {
    PortId id;
    std::string type; // "input" or "output"
    std::string dataType; // "int", "float", "double", "string", "async_int", etc.
    Value value;
};

// Represents a connection (wire) between ports
struct Connection {
    NodeId fromNode;
    PortId fromPort;
    NodeId toNode;
    PortId toPort;
};

// Represents a node (operator, device trigger, etc.)
struct Node {
    NodeId id;
    std::string type;
    std::vector<Port> inputs;
    std::vector<Port> outputs;
    std::unordered_map<std::string, Value> parameters;

    void execute(std::unordered_map<PortId, Value>& portValues);
};

// Descriptors and introspection (used by WS/runtime/AOT to be fully generic)
struct PortDesc {
    PortHandle handle;
    NodeId nodeId;
    PortId portId;
    std::string direction; // "input" or "output"
    std::string dataType;  // base type string
};

struct NodeDesc {
    NodeId id;
    std::string type;
    std::vector<PortHandle> inputPorts;
    std::vector<PortHandle> outputPorts;
};

// FlowEngine manages the flow graph lifecycle: load, execute, describe, AOT
class FlowEngine {
public:
    FlowEngine() = default;
    // Load a graph from JSON (nodes, ports, connections)
    void loadFromJson(const nlohmann::json& json);
    // Evaluate the graph once (non-blocking, deterministic)
    void execute();
    // Advance internal time for time-based nodes (e.g., Timer); dt in milliseconds
    void tick(double dtMs);
    // Convenience accessor to current node outputs (kept for compatibility)
    std::unordered_map<NodeId, std::vector<Value>> getOutputs() const;
    // Minimal AOT/demo codegen helpers
    void compileToExecutable(const std::string& outputFile, bool dslMode = true);
    void generateStepLibrary(const std::string& baseName) const;
    // Experimental: prefer LLVM-style specialized generation (current stub
    // routes to the C++ generator; kept for CLI/API parity and future swap-in)
    void generateStepLibraryLLVM(const std::string& baseName) const;

    // Control helpers for runtime/IPC
    // Set a node's current value (commonly DeviceTrigger). Propagates downstream.
    void setNodeValue(const std::string& nodeId, float value);
    // Update per-node timing/config parameters
    void setNodeConfigMinMax(const std::string& nodeId, int minIntervalMs, int maxIntervalMs);

    // Introspection
    const std::vector<NodeDesc>& getNodeDescs() const { return nodeDescs; }
    const std::vector<PortDesc>& getPortDescs() const { return portDescs; }
    int getPortHandle(const std::string& nodeId, const std::string& portId, const std::string& direction) const;
    Value readPort(PortHandle handle) const { return (handle >= 0 && (size_t)handle < portValues.size()) ? portValues[handle] : Value{}; }
    void writePort(PortHandle handle, const Value& v) { if (handle >= 0 && (size_t)handle < portValues.size()) portValues[handle] = v; }

    // Generation counters and deltas
    // Begin a new WS snapshot epoch and return its generation
    Generation beginSnapshot();
    // Compatibility delta helpers for runtime/WS
    std::unordered_map<NodeId, Value> getOutputsChangedSince(Generation lastSnapshotGen) const;
    std::vector<std::tuple<NodeId, PortId, Value>> getPortDeltasChangedSince(Generation lastSnapshotGen) const;
    Generation currentEvalGeneration() const { return evalGeneration; }

    // Performance counters (lightweight; zero-alloc, resettable)
    struct PerfStats {
        unsigned long long evalCount = 0;
        unsigned long long nodesEvaluated = 0;
        unsigned long long dependentsEnqueued = 0;
        unsigned long long readyQueueMax = 0;
        unsigned long long evalTimeNsAccum = 0; // total
        unsigned long long evalTimeNsMin = (unsigned long long)-1;
        unsigned long long evalTimeNsMax = 0;
    };
    PerfStats getAndResetPerfStats() {
        PerfStats out = perf;
        perf = PerfStats{};
        return out;
    }

private:
    std::vector<Node> nodes;
    std::vector<Connection> connections;
    std::vector<NodeId> executionOrder;

    // Interned descriptors and handle maps
    std::vector<NodeDesc> nodeDescs;
    std::vector<PortDesc> portDescs;
    std::unordered_map<std::string, PortHandle> portKeyToHandle; // key: nodeId+":"+portId+":"+direction

    // Generations and change tracking (per-node first output)
    Generation evalGeneration = 1;
    Generation snapshotGeneration = 0;
    std::unordered_map<NodeId, Generation> outputChangedStamp; // nodeId -> last eval gen when its primary output changed
    std::vector<Generation> portChangedStamp; // per port handle
    std::vector<Value> portValues; // current port values by handle (SoA seed)
    std::vector<std::vector<PortHandle>> outToIn; // output handle -> list of input handles
    std::unordered_map<NodeId, std::vector<PortHandle>> nodeOutputHandles; // node -> its output handles

    // Deterministic scheduling scaffolding (ready-queue, topo index)
    std::unordered_map<NodeId, std::vector<NodeId>> dependents; // nodeId -> downstream nodes
    std::unordered_map<NodeId, int> topoIndex; // nodeId -> topological order index
    std::unordered_map<NodeId, size_t> nodeIndex; // nodeId -> index in nodes vector
    std::vector<NodeId> readyQueue;
    std::unordered_map<NodeId, Generation> readyStamp;
    bool coldStart = true;

    // Perf counters (mutated inside execute and enqueue)
    PerfStats perf;

    // Per-node state for time-based/edge-detect nodes
    std::vector<double> timerAccumMs;      // per Timer node accumulator
    std::vector<int> counterLastTick;      // per Counter node last input (>0 => 1, else 0)
    std::vector<double> counterValue;      // per Counter node current value (double for uniformity)

    void enqueueNode(const NodeId& id);
    void enqueueDependents(const NodeId& id);

    void computeExecutionOrder();
};

} // namespace NodeFlow

extern "C" {
    int isKeyPressed(const char* key);
    int isRandomReady(int min_interval, int max_interval);
    float getRandomNumber();
}