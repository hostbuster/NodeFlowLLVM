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
using Value = std::variant<int, float, double, std::string>;
using PortHandle = int;
using Generation = unsigned long long;

// Represents a port (input or output)
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

// Represents a node
struct Node {
    NodeId id;
    std::string type;
    std::vector<Port> inputs;
    std::vector<Port> outputs;
    std::unordered_map<std::string, Value> parameters;

    void execute(std::unordered_map<PortId, Value>& portValues);
};

// Descriptors and introspection
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

// FlowEngine manages the execution and compilation of the flow
class FlowEngine {
public:
    FlowEngine() = default;
    void loadFromJson(const nlohmann::json& json);
    void execute();
    std::unordered_map<NodeId, std::vector<Value>> getOutputs() const;
    void compileToExecutable(const std::string& outputFile, bool dslMode = true);
    void generateStepLibrary(const std::string& baseName) const;

    // Control helpers for runtime/IPC
    void setNodeValue(const std::string& nodeId, float value);
    void setNodeConfigMinMax(const std::string& nodeId, int minIntervalMs, int maxIntervalMs);

    // Introspection
    const std::vector<NodeDesc>& getNodeDescs() const { return nodeDescs; }
    const std::vector<PortDesc>& getPortDescs() const { return portDescs; }
    int getPortHandle(const std::string& nodeId, const std::string& portId, const std::string& direction) const;
    Value readPort(PortHandle handle) const { return (handle >= 0 && (size_t)handle < portValues.size()) ? portValues[handle] : Value{}; }
    void writePort(PortHandle handle, const Value& v) { if (handle >= 0 && (size_t)handle < portValues.size()) portValues[handle] = v; }

    // Generation counters and deltas
    Generation beginSnapshot();
    std::unordered_map<NodeId, Value> getOutputsChangedSince(Generation lastSnapshotGen) const;
    std::vector<std::tuple<NodeId, PortId, Value>> getPortDeltasChangedSince(Generation lastSnapshotGen) const;
    Generation currentEvalGeneration() const { return evalGeneration; }

private:
    std::vector<Node> nodes;
    std::vector<Connection> connections;
    std::vector<NodeId> executionOrder;

    // Interned descriptors
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

    // Dirty-driven scheduling (interim implementation)
    std::unordered_map<NodeId, std::vector<NodeId>> dependents; // nodeId -> downstream nodes
    std::unordered_map<NodeId, int> topoIndex; // nodeId -> topological order index
    std::vector<NodeId> readyQueue;
    std::unordered_map<NodeId, Generation> readyStamp;
    bool coldStart = true;

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