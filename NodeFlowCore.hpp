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

// FlowEngine manages the execution and compilation of the flow
class FlowEngine {
public:
    FlowEngine() = default;
    void loadFromJson(const nlohmann::json& json);
    void execute();
    std::unordered_map<NodeId, std::vector<Value>> getOutputs() const;
    void compileToExecutable(const std::string& outputFile, bool dslMode = true);

private:
    std::vector<Node> nodes;
    std::vector<Connection> connections;
    std::vector<NodeId> executionOrder;

    void computeExecutionOrder();
};

} // namespace NodeFlow

extern "C" {
    int isKeyPressed(const char* key);
    int isRandomReady(int min_interval, int max_interval);
    float getRandomNumber();
}