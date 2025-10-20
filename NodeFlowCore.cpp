// NodeFlowCore.cpp
//
// Implements the flow engine: JSON load, graph descriptors/handles, execution,
// and minimal AOT demo generation. The engine is moving toward a SoA layout
// for high-performance deterministic evaluation.
#include "NodeFlowCore.hpp"
#include <fstream>
#include <algorithm>
#include <stdexcept>
#include <variant>
#include <chrono>
#include <thread>
#include <random>
#include <ctime>
#include <iostream>
// headless-only; remove legacy TUI includes

namespace NodeFlow {

// Declarations are provided in header; definitions are implemented in main.cpp

void Node::execute(std::unordered_map<PortId, Value>& portValues) {
    auto makeKey = [&](const std::string& portId) { return id + ":" + portId; };
    if (type == "Value") {
        if (parameters.count("value")) {
            for (auto& output : outputs) {
                portValues[makeKey(output.id)] = parameters.at("value");
                output.value = parameters.at("value");
            }
        }
    } else if (type == "DeviceTrigger") {
        // Headless: treat externally-set 'value' as the current output; no key polling
        if (parameters.count("key")) {
            for (auto& output : outputs) {
                Value newVal = output.value;
                if (parameters.count("value")) {
                    const Value& p = parameters.at("value");
                    if (std::holds_alternative<float>(p)) newVal = std::get<float>(p);
                    else if (std::holds_alternative<double>(p)) newVal = static_cast<float>(std::get<double>(p));
                    else if (std::holds_alternative<int>(p)) newVal = static_cast<float>(std::get<int>(p));
                }
                output.value = newVal;
                portValues[makeKey(output.id)] = newVal;
            }
        } else if (parameters.count("min_interval")) {
            // Internal per-node random timer: non-blocking, updates when due
            const int minMs = std::get<int>(parameters.at("min_interval"));
            const int maxMs = std::get<int>(parameters.at("max_interval"));
            using Clock = std::chrono::steady_clock;
            static std::unordered_map<std::string, Clock::time_point> nodeIdToNextDue;
            static std::mt19937 rng(std::random_device{}());
            static std::unordered_map<std::string, std::uniform_int_distribution<int>> nodeIdToInterval;
            static std::uniform_real_distribution<float> valueDist(0.0f, 100.0f);

            auto now = Clock::now();
            auto itDue = nodeIdToNextDue.find(id);
            if (itDue == nodeIdToNextDue.end()) {
                nodeIdToInterval.emplace(id, std::uniform_int_distribution<int>(minMs, maxMs));
                auto delta = std::chrono::milliseconds(nodeIdToInterval[id](rng));
                nodeIdToNextDue[id] = now + delta;
            }

            bool ready = now >= nodeIdToNextDue[id];
            if (ready) {
                auto delta = std::chrono::milliseconds(nodeIdToInterval[id](rng));
                nodeIdToNextDue[id] = now + delta;
            }

            for (auto& output : outputs) {
                Value newVal = output.value;
                if (ready) newVal = valueDist(rng);
                output.value = newVal;
                portValues[makeKey(output.id)] = newVal;
            }
        } else {
            // No parameters; just reuse last
            for (auto& output : outputs) {
                portValues[makeKey(output.id)] = output.value;
            }
        }
    } else if (type == "Add") {
        if (inputs.empty() || outputs.empty()) {
            throw std::runtime_error("Invalid Add node configuration");
        }
        std::string dataType = outputs[0].dataType;
        auto getBaseType = [](const std::string& t) {
            return t.find("async_") == 0 ? t.substr(6) : t;
        };
        std::string baseType = getBaseType(inputs[0].dataType);
        for (const auto& input : inputs) {
            if (getBaseType(input.dataType) != baseType) {
                throw std::runtime_error("Type mismatch in Add node");
            }
        }
        if (baseType != dataType) {
            throw std::runtime_error("Output type mismatch in Add node");
        }
        if (dataType == "int") {
            int sum = 0;
            for (const auto& input : inputs) {
                auto key = makeKey(input.id);
                if (portValues.count(key) && std::holds_alternative<int>(portValues[key])) {
                    sum += std::get<int>(portValues[key]);
                }
            }
            for (auto& output : outputs) {
                output.value = sum;
                portValues[makeKey(output.id)] = sum;
            }
        } else if (dataType == "float") {
            float sum = 0.0f;
            for (const auto& input : inputs) {
                auto key = makeKey(input.id);
                if (portValues.count(key) && std::holds_alternative<float>(portValues[key])) {
                    sum += std::get<float>(portValues[key]);
                }
            }
            for (auto& output : outputs) {
                output.value = sum;
                portValues[makeKey(output.id)] = sum;
            }
        } else if (dataType == "double") {
            double sum = 0.0;
            for (const auto& input : inputs) {
                auto key = makeKey(input.id);
                if (portValues.count(key) && std::holds_alternative<double>(portValues[key])) {
                    sum += std::get<double>(portValues[key]);
                }
            }
            for (auto& output : outputs) {
                output.value = sum;
                portValues[makeKey(output.id)] = sum;
            }
        } else if (dataType == "string") {
            std::string result;
            for (const auto& input : inputs) {
                auto key = makeKey(input.id);
                if (portValues.count(key) && std::holds_alternative<std::string>(portValues[key])) {
                    result += std::get<std::string>(portValues[key]);
                }
            }
            for (auto& output : outputs) {
                output.value = result;
                portValues[makeKey(output.id)] = result;
            }
        }
    }
}

// Load a graph from JSON and (re)build descriptors, topology, and adjacency
void FlowEngine::loadFromJson(const nlohmann::json& json) {
    srand(time(NULL));
    nodes.clear();
    connections.clear();
    nodeDescs.clear();
    portDescs.clear();
    portKeyToHandle.clear();
    portChangedStamp.clear();
    portValues.clear();
    outToIn.clear();
    nodeOutputHandles.clear();

    for (const auto& nodeJson : json["nodes"]) {
        Node node;
        node.id = nodeJson["id"].get<std::string>();
        node.type = nodeJson["type"].get<std::string>();

        for (const auto& input : nodeJson["inputs"]) {
            node.inputs.push_back({input["id"].get<std::string>(), "input", input["type"].get<std::string>(), Value{0.0f}});
        }
        for (const auto& output : nodeJson["outputs"]) {
            node.outputs.push_back({output["id"].get<std::string>(), "output", output["type"].get<std::string>(), Value{0.0f}});
        }
        if (nodeJson.contains("parameters") && nodeJson["parameters"].is_object()) {
        for (const auto& param : nodeJson["parameters"].items()) {
                // Use the actual JSON type for parameters (keys may be strings even if outputs are numeric)
                if (param.value().is_string()) {
                    node.parameters[param.key()] = param.value().get<std::string>();
                } else if (param.value().is_number_integer()) {
                    node.parameters[param.key()] = param.value().get<int>();
                } else if (param.value().is_number_float()) {
                    // store as double to preserve precision; execution code handles double and float
                    node.parameters[param.key()] = param.value().get<double>();
                } else if (param.value().is_boolean()) {
                    node.parameters[param.key()] = param.value().get<bool>() ? 1 : 0;
                }
            }
        }
        // Build descriptors and handles as we go
        NodeDesc nd; nd.id = node.id; nd.type = node.type;
        for (const auto& ip : node.inputs) {
            std::string key = node.id + ":" + ip.id + ":input";
            PortHandle h = static_cast<PortHandle>(portDescs.size());
            portKeyToHandle[key] = h;
            portDescs.push_back({h, node.id, ip.id, "input", ip.dataType});
            portChangedStamp.push_back(0);
            nd.inputPorts.push_back(h);
        }
        for (const auto& op : node.outputs) {
            std::string key = node.id + ":" + op.id + ":output";
            PortHandle h = static_cast<PortHandle>(portDescs.size());
            portKeyToHandle[key] = h;
            portDescs.push_back({h, node.id, op.id, "output", op.dataType});
            portChangedStamp.push_back(0);
            nd.outputPorts.push_back(h);
        }
        nodeDescs.push_back(std::move(nd));
        nodes.push_back(node);
    }
    portValues.resize(portDescs.size());
    outToIn.resize(portDescs.size());

    // Build handle adjacency and node->output handles map
    for (const auto &n : nodes) {
        std::vector<PortHandle> outs;
        for (const auto &op : n.outputs) {
            int h = getPortHandle(n.id, op.id, "output");
            if (h >= 0) outs.push_back(h);
        }
        nodeOutputHandles[n.id] = std::move(outs);
    }
    for (const auto &c : connections) {
        int hOut = getPortHandle(c.fromNode, c.fromPort, "output");
        int hIn  = getPortHandle(c.toNode, c.toPort, "input");
        if (hOut >= 0 && hIn >= 0) outToIn[hOut].push_back(hIn);
    }

    for (const auto& connJson : json["connections"]) {
        Connection conn = {
            connJson["fromNode"].get<std::string>(),
            connJson["fromPort"].get<std::string>(),
            connJson["toNode"].get<std::string>(),
            connJson["toPort"].get<std::string>()
        };
        auto fromNode = std::find_if(nodes.begin(), nodes.end(), [&](const Node& n) { return n.id == conn.fromNode; });
        auto toNode = std::find_if(nodes.begin(), nodes.end(), [&](const Node& n) { return n.id == conn.toNode; });
        auto fromPort = std::find_if(fromNode->outputs.begin(), fromNode->outputs.end(),
                                     [&](const Port& p) { return p.id == conn.fromPort; });
        auto toPort = std::find_if(toNode->inputs.begin(), toNode->inputs.end(),
                                   [&](const Port& p) { return p.id == conn.toPort; });
        auto getBaseType = [](const std::string& t) { return t.find("async_") == 0 ? t.substr(6) : t; };
        if (getBaseType(fromPort->dataType) != getBaseType(toPort->dataType)) {
            throw std::runtime_error("Type mismatch in connection");
        }
        connections.push_back(conn);
    }

    computeExecutionOrder();

    // Rebuild handle adjacency now that connections are populated
    outToIn.clear();
    outToIn.resize(portDescs.size());
    nodeOutputHandles.clear();
    for (const auto &n : nodes) {
        std::vector<PortHandle> outs;
        for (const auto &op : n.outputs) {
            int h = getPortHandle(n.id, op.id, "output");
            if (h >= 0) outs.push_back(h);
        }
        nodeOutputHandles[n.id] = std::move(outs);
    }
    for (const auto &c : connections) {
        int hOut = getPortHandle(c.fromNode, c.fromPort, "output");
        int hIn  = getPortHandle(c.toNode, c.toPort, "input");
        if (hOut >= 0 && hIn >= 0) outToIn[hOut].push_back(hIn);
        std::cout << "[DEBUG] connect " << c.fromNode << ":" << c.fromPort << "(hOut=" << hOut << ") -> "
                  << c.toNode << ":" << c.toPort << "(hIn=" << hIn << ")\n";
    }
}
int FlowEngine::getPortHandle(const std::string& nodeId, const std::string& portId, const std::string& direction) const {
    auto it = portKeyToHandle.find(nodeId + ":" + portId + ":" + direction);
    if (it == portKeyToHandle.end()) return -1;
    return it->second;
}

void FlowEngine::computeExecutionOrder() {
    executionOrder.clear();
    std::unordered_map<NodeId, std::vector<NodeId>> graph;
    std::unordered_map<NodeId, int> inDegree;

    for (const auto& node : nodes) {
        inDegree[node.id] = 0;
    }
    for (const auto& conn : connections) {
        graph[conn.fromNode].push_back(conn.toNode);
        inDegree[conn.toNode]++;
    }

    std::vector<NodeId> queue;
    for (const auto& node : nodes) {
        if (inDegree[node.id] == 0) {
            queue.push_back(node.id);
        }
    }

    while (!queue.empty()) {
        auto current = queue.front();
        queue.erase(queue.begin());
        executionOrder.push_back(current);

        for (const auto& next : graph[current]) {
            if (--inDegree[next] == 0) {
                queue.push_back(next);
            }
        }
    }

    if (executionOrder.size() != nodes.size()) {
        throw std::runtime_error("Cycle detected in flow graph");
    }

    // Build topo index and dependents
    topoIndex.clear();
    dependents = graph;
    for (size_t i = 0; i < executionOrder.size(); ++i) topoIndex[executionOrder[i]] = static_cast<int>(i);
}

// Evaluate the graph once (non-blocking). Seeds previous outputs, performs
// handle-based propagation, and executes nodes in topological order.
void FlowEngine::execute() {
    // bump evaluation generation
    ++evalGeneration;

    auto makeKey = [](const std::string& nodeId, const std::string& portId) { return nodeId + ":" + portId; };
    // Seed SoA with last outputs so downstream consumers see previous values if no new events
    for (const auto& node : nodes) {
        for (const auto& output : node.outputs) {
            int hOut = getPortHandle(node.id, output.id, "output");
            if (hOut >= 0 && static_cast<size_t>(hOut) < portValues.size()) this->portValues[hOut] = output.value;
        }
    }
    // Initial propagation via handle adjacency
    for (const auto &pd : portDescs) {
        if (pd.direction != "output") continue;
        int hOut = pd.handle;
        for (int hIn : outToIn[hOut]) {
            if (hIn >= 0 && static_cast<size_t>(hIn) < portValues.size()) {
                this->portValues[hIn] = this->portValues[hOut];
                (void)portDescs; // SoA propagation only; map no longer used
            }
        }
    }
    // Helper to process a single node: execute, propagate, mark changes and enqueue dependents
    auto processNode = [&](const NodeId& nodeId) {
        auto it = std::find_if(nodes.begin(), nodes.end(), [&](const Node& n) { return n.id == nodeId; });
        if (it == nodes.end()) return;
        // Capture previous first-output value for change detection
        Value prevOut0;
        bool hasPrev0 = false;
        if (!it->outputs.empty()) { prevOut0 = it->outputs[0].value; hasPrev0 = true; }
        // Handle-based execution for common node types (Value, DeviceTrigger, Add)
        bool handled = false;
        if (it->type == "Value") {
            Value v = 0.0f;
            auto p = it->parameters.find("value");
            if (p != it->parameters.end()) v = p->second;
            for (auto &op : it->outputs) {
                op.value = v;
                int hOut = getPortHandle(it->id, op.id, "output");
                if (hOut >= 0 && (size_t)hOut < portValues.size()) {
                    this->portValues[hOut] = v;
                    if ((size_t)hOut < portChangedStamp.size()) portChangedStamp[hOut] = evalGeneration;
                    for (int hIn : outToIn[hOut]) {
                        if (hIn >= 0 && (size_t)hIn < portValues.size()) {
                            this->portValues[hIn] = v;
                            (void)portDescs;
                        }
                    }
                }
            }
            handled = true;
        } else if (it->type == "DeviceTrigger") {
            // Use last set value (parameters["value"]) or keep current outputs
            Value vOut;
            bool have = false;
            auto p = it->parameters.find("value");
            if (p != it->parameters.end()) { vOut = p->second; have = true; }
            for (auto &op : it->outputs) {
                if (have) op.value = vOut;
                int hOut = getPortHandle(it->id, op.id, "output");
                if (hOut >= 0 && (size_t)hOut < portValues.size()) {
                    this->portValues[hOut] = op.value;
                    if ((size_t)hOut < portChangedStamp.size()) portChangedStamp[hOut] = evalGeneration;
                    for (int hIn : outToIn[hOut]) {
                        if (hIn >= 0 && (size_t)hIn < portValues.size()) {
                            this->portValues[hIn] = op.value;
                            (void)portDescs;
                        }
                    }
                }
            }
            handled = true;
        } else if (it->type == "Add") {
            if (!it->outputs.empty()) {
                const std::string &dtype = it->outputs[0].dataType;
                auto baseType = [&](const std::string &t){ return t.rfind("async_", 0) == 0 ? t.substr(6) : t; };
                std::string bt = baseType(dtype);
                if (bt == "int") {
                    int sum = 0;
                    std::string dbg = "inputs:";
                    for (const auto &ip : it->inputs) {
                        int v = 0;
                        int hIn = getPortHandle(it->id, ip.id, "input");
                        if (hIn >= 0 && (size_t)hIn < portValues.size()) {
                            const Value &vv = this->portValues[hIn];
                            if (std::holds_alternative<int>(vv)) v = std::get<int>(vv);
                            else if (std::holds_alternative<float>(vv)) v = (int)std::get<float>(vv);
                            else if (std::holds_alternative<double>(vv)) v = (int)std::get<double>(vv);
                        }
                        sum += v;
                        dbg += " " + std::to_string(v);
                    }
                    
                    for (auto &op : it->outputs) {
                        op.value = sum;
                        int hOut = getPortHandle(it->id, op.id, "output");
                        if (hOut >= 0 && (size_t)hOut < portValues.size()) {
                            this->portValues[hOut] = sum;
                            if ((size_t)hOut < portChangedStamp.size()) portChangedStamp[hOut] = evalGeneration;
                            for (int hIn : outToIn[hOut]) {
                                if (hIn >= 0 && (size_t)hIn < portValues.size()) {
                                    this->portValues[hIn] = sum;
                                    (void)portDescs;
                                }
                            }
                        }
                    }
                } else if (bt == "double") {
                    double sum = 0.0;
                    std::string dbg = "inputs:";
                    for (const auto &ip : it->inputs) {
                        double v = 0.0;
                        int hIn = getPortHandle(it->id, ip.id, "input");
                        if (hIn >= 0 && (size_t)hIn < portValues.size()) {
                            const Value &vv = this->portValues[hIn];
                            if (std::holds_alternative<double>(vv)) v = std::get<double>(vv);
                            else if (std::holds_alternative<float>(vv)) v = (double)std::get<float>(vv);
                            else if (std::holds_alternative<int>(vv)) v = (double)std::get<int>(vv);
                        }
                        sum += v;
                        dbg += " " + std::to_string(v);
                    }
                    
                    for (auto &op : it->outputs) {
                        op.value = sum;
                        int hOut = getPortHandle(it->id, op.id, "output");
                        if (hOut >= 0 && (size_t)hOut < portValues.size()) {
                            this->portValues[hOut] = sum;
                            if ((size_t)hOut < portChangedStamp.size()) portChangedStamp[hOut] = evalGeneration;
                            for (int hIn : outToIn[hOut]) {
                                if (hIn >= 0 && (size_t)hIn < portValues.size()) {
                                    this->portValues[hIn] = sum;
                                    (void)portDescs;
                                }
                            }
                        }
                    }
                } else { // float/default
                    float sum = 0.0f;
                    std::string dbg = "inputs:";
                    for (const auto &ip : it->inputs) {
                        float v = 0.0f;
                        int hIn = getPortHandle(it->id, ip.id, "input");
                        if (hIn >= 0 && (size_t)hIn < portValues.size()) {
                            const Value &vv = this->portValues[hIn];
                            if (std::holds_alternative<float>(vv)) v = std::get<float>(vv);
                            else if (std::holds_alternative<double>(vv)) v = (float)std::get<double>(vv);
                            else if (std::holds_alternative<int>(vv)) v = (float)std::get<int>(vv);
                        }
                        sum += v;
                        dbg += " " + std::to_string(v);
                    }
                    
                    for (auto &op : it->outputs) {
                        op.value = sum;
                        int hOut = getPortHandle(it->id, op.id, "output");
                        if (hOut >= 0 && (size_t)hOut < portValues.size()) {
                            this->portValues[hOut] = sum;
                            if ((size_t)hOut < portChangedStamp.size()) portChangedStamp[hOut] = evalGeneration;
                            for (int hIn : outToIn[hOut]) {
                                if (hIn >= 0 && (size_t)hIn < portValues.size()) {
                                    this->portValues[hIn] = sum;
                                    (void)portDescs;
                                }
                            }
                        }
                    }
                }
            }
            handled = true;
        } else {
            // Unknown node type: leave outputs unchanged (no-op)
        }
        enqueueDependents(it->id);
        // Mark node changed if primary output changed compared to previous
        if (!it->outputs.empty() && hasPrev0) {
            const auto &new0 = it->outputs[0].value;
            bool changed = prevOut0.index() != new0.index();
            if (!changed) {
                if (std::holds_alternative<float>(new0)) changed = std::get<float>(prevOut0) != std::get<float>(new0);
                else if (std::holds_alternative<double>(new0)) changed = std::get<double>(prevOut0) != std::get<double>(new0);
                else if (std::holds_alternative<int>(new0)) changed = std::get<int>(prevOut0) != std::get<int>(new0);
                else if (std::holds_alternative<std::string>(new0)) changed = std::get<std::string>(prevOut0) != std::get<std::string>(new0);
            }
            if (changed) outputChangedStamp[it->id] = evalGeneration;
        }
    };

    // Deterministic scheduling: first time run full topo, then ready-queue
    if (coldStart) {
        for (const auto& nodeId : executionOrder) processNode(nodeId);
        readyQueue.clear();
        coldStart = false;
    } else {
        while (!readyQueue.empty()) {
            auto nodeId = readyQueue.front();
            readyQueue.erase(readyQueue.begin());
            processNode(nodeId);
        }
    }
}

void FlowEngine::enqueueNode(const NodeId& id) {
    // Dedup by generation and stable order by topo index
    if (readyStamp[id] == evalGeneration) return;
    readyStamp[id] = evalGeneration;
    readyQueue.push_back(id);
    std::stable_sort(readyQueue.begin(), readyQueue.end(), [&](const NodeId& a, const NodeId& b){
        int ia = topoIndex.count(a) ? topoIndex[a] : 0;
        int ib = topoIndex.count(b) ? topoIndex[b] : 0;
        if (ia != ib) return ia < ib;
        return a < b;
    });
}

void FlowEngine::enqueueDependents(const NodeId& id) {
    auto it = dependents.find(id);
    if (it == dependents.end()) return;
    for (const auto &dn : it->second) enqueueNode(dn);
}

std::unordered_map<NodeId, std::vector<Value>> FlowEngine::getOutputs() const {
    std::unordered_map<NodeId, std::vector<Value>> outputs;
    for (const auto& node : nodes) {
        for (const auto& output : node.outputs) {
            outputs[node.id].push_back(output.value);
        }
    }
    return outputs;
}

Generation FlowEngine::beginSnapshot() {
    return ++snapshotGeneration;
}

std::unordered_map<NodeId, Value> FlowEngine::getOutputsChangedSince(Generation lastSnapshotGen) const {
    std::unordered_map<NodeId, Value> out;
    for (const auto& n : nodes) {
        if (n.outputs.empty()) continue;
        auto it = outputChangedStamp.find(n.id);
        if (it != outputChangedStamp.end() && it->second > lastSnapshotGen) {
            out.emplace(n.id, n.outputs[0].value);
        }
    }
    return out;
}

std::vector<std::tuple<NodeId, PortId, Value>> FlowEngine::getPortDeltasChangedSince(Generation lastSnapshotGen) const {
    std::vector<std::tuple<NodeId, PortId, Value>> deltas;
    for (const auto &pd : portDescs) {
        if (pd.direction != "output") continue;
        if (static_cast<size_t>(pd.handle) >= portChangedStamp.size()) continue;
        if (portChangedStamp[pd.handle] > lastSnapshotGen) {
            // find node and port current value
            for (const auto &n : nodes) {
                if (n.id == pd.nodeId) {
                    for (const auto &op : n.outputs) {
                        if (op.id == pd.portId) {
                            deltas.emplace_back(pd.nodeId, pd.portId, op.value);
                            break;
                        }
                    }
                    break;
                }
            }
        }
    }
    return deltas;
}

void FlowEngine::compileToExecutable(const std::string& outputFile, bool /*dslMode*/) {
    // Minimal C++ codegen: emit a small standalone program computing the Add flow
    // from the current in-memory graph (assumes three inputs to 'add1').
    std::string sourcePath = outputFile + ".cpp";
    std::ofstream out(sourcePath);
    if (!out.is_open()) return;

    // Determine types and parameters
    auto findNode = [&](const std::string& id) {
        return std::find_if(nodes.begin(), nodes.end(), [&](const Node& n){ return n.id == id; });
    };
    auto addIt = findNode("add1");
    std::string dtype = (addIt != nodes.end() && !addIt->outputs.empty()) ? addIt->outputs[0].dataType : std::string("float");
    auto getBaseType = [](const std::string& t){ return t.rfind("async_", 0) == 0 ? t.substr(6) : t; };
    dtype = getBaseType(dtype);

    out << "#include <cstdio>\n";
    out << "int main(){\n";
    auto emitVal = [&](const std::string& nodeId){
        auto it = findNode(nodeId);
        if (it == nodes.end()) return std::string("0");
        if (!it->parameters.count("value")) return std::string("0");
        const Value& val = it->parameters.at("value");
        if (dtype == "int") {
            int v = 0;
            if (std::holds_alternative<int>(val)) v = std::get<int>(val);
            else if (std::holds_alternative<float>(val)) v = static_cast<int>(std::get<float>(val));
            else if (std::holds_alternative<double>(val)) v = static_cast<int>(std::get<double>(val));
            return std::to_string(v);
        } else if (dtype == "double") {
            double v = 0;
            if (std::holds_alternative<double>(val)) v = std::get<double>(val);
            else if (std::holds_alternative<float>(val)) v = static_cast<double>(std::get<float>(val));
            else if (std::holds_alternative<int>(val)) v = static_cast<double>(std::get<int>(val));
            char buf[64]; std::snprintf(buf, sizeof(buf), "%.*g", 17, v);
            return std::string(buf);
        } else { // float or default
            float v = 0;
            if (std::holds_alternative<float>(val)) v = std::get<float>(val);
            else if (std::holds_alternative<double>(val)) v = static_cast<float>(std::get<double>(val));
            else if (std::holds_alternative<int>(val)) v = static_cast<float>(std::get<int>(val));
            char buf[64]; std::snprintf(buf, sizeof(buf), "%.*g", 9, v);
            return std::string(buf);
        }
    };

    // Heuristically pick three upstream nodes by scanning connections to add1 inputs order
    std::vector<std::string> inputs;
    if (addIt != nodes.end()) {
        for (const auto& in : addIt->inputs) {
            auto c = std::find_if(connections.begin(), connections.end(), [&](const Connection& cc){ return cc.toPort == in.id; });
            if (c != connections.end()) inputs.push_back(c->fromNode);
        }
    }
    while (inputs.size() < 3) inputs.push_back(inputs.empty() ? "" : inputs.back());

    std::string a = emitVal(inputs[0]);
    std::string b = emitVal(inputs[1]);
    std::string c = emitVal(inputs[2]);

    if (dtype == "int") {
        out << "  int s = (" << a << ") + (" << b << ") + (" << c << ");\n";
        out << "  std::printf(\"%d\\n\", s);\n";
    } else if (dtype == "double") {
        out << "  double s = (" << a << ") + (" << b << ") + (" << c << ");\n";
        out << "  std::printf(\"%f\\n\", s);\n";
    } else {
        out << "  float s = (" << a << ") + (" << b << ") + (" << c << ");\n";
        out << "  std::printf(\"%f\\n\", s);\n";
    }
    out << "  return 0;\n";
    out << "}\n";
    out.close();

    // Compile with Apple Clang
    std::string cmd = std::string("/usr/bin/clang++ ") + sourcePath + " -O2 -std=c++17 -o " + outputFile;
    (void)system(cmd.c_str());
}

} // namespace NodeFlow
 
void NodeFlow::FlowEngine::setNodeValue(const std::string& nodeId, float value) {
    auto it = std::find_if(nodes.begin(), nodes.end(), [&](const Node& n){ return n.id == nodeId; });
    if (it == nodes.end()) return;
    float prev = 0.0f;
    if (!it->outputs.empty()) {
        const auto &v = it->outputs[0].value;
        if (std::holds_alternative<float>(v)) prev = std::get<float>(v);
        else if (std::holds_alternative<double>(v)) prev = static_cast<float>(std::get<double>(v));
        else if (std::holds_alternative<int>(v)) prev = static_cast<float>(std::get<int>(v));
    }
    it->parameters["value"] = static_cast<float>(value);
    bool changed = (prev != value);
    for (auto &out : it->outputs) out.value = static_cast<float>(value);
    // Update SoA values immediately for this node's outputs and propagate to downstream inputs
    for (const auto &op : it->outputs) {
        int hOut = getPortHandle(it->id, op.id, "output");
        if (hOut >= 0 && static_cast<size_t>(hOut) < portValues.size()) {
            portValues[hOut] = static_cast<float>(value);
            if (static_cast<size_t>(hOut) < portChangedStamp.size()) portChangedStamp[hOut] = evalGeneration;
            for (int hIn : outToIn[hOut]) {
                if (hIn >= 0 && static_cast<size_t>(hIn) < portValues.size()) {
                    portValues[hIn] = static_cast<float>(value);
                }
            }
        }
    }
    if (changed) {
        // mark node output changed this eval generation
        outputChangedStamp[it->id] = evalGeneration;
        // and the port handle
        for (const auto &op : it->outputs) {
            int ph = getPortHandle(it->id, op.id, "output");
            if (ph >= 0 && static_cast<size_t>(ph) < portChangedStamp.size()) portChangedStamp[ph] = evalGeneration;
        }
        // enqueue dependents for re-evaluation
        enqueueDependents(it->id);
    }
}

void NodeFlow::FlowEngine::setNodeConfigMinMax(const std::string& nodeId, int minIntervalMs, int maxIntervalMs) {
    auto it = std::find_if(nodes.begin(), nodes.end(), [&](const Node& n){ return n.id == nodeId; });
    if (it == nodes.end()) return;
    it->parameters["min_interval"] = minIntervalMs;
    it->parameters["max_interval"] = maxIntervalMs;
}

// Generate a small step-function library: <baseName>_step.h/.cpp
void NodeFlow::FlowEngine::generateStepLibrary(const std::string& baseName) const {
    const std::string headerPath = baseName + "_step.h";
    const std::string sourcePath = baseName + "_step.cpp";
    std::ofstream h(headerPath), c(sourcePath);
    if (!h.is_open() || !c.is_open()) return;

    auto getBaseType = [](const std::string& t) {
        return t.rfind("async_", 0) == 0 ? t.substr(6) : t;
    };
    auto toCType = [&](const std::string& t) -> std::string {
        const auto bt = getBaseType(t);
        if (bt == "int") return "int";
        if (bt == "double") return "double";
        return "float";
    };

    // Inputs: all DeviceTrigger nodes
    std::vector<const Node*> inputNodes;
    for (const auto& n : nodes) {
        if (n.type == "DeviceTrigger" && !n.outputs.empty()) inputNodes.push_back(&n);
    }
    // Sinks: nodes with no outgoing edges
    std::vector<const Node*> sinkNodes;
    for (const auto& n : nodes) {
        bool hasOut = false;
        for (const auto& cc : connections) { if (cc.fromNode == n.id) { hasOut = true; break; } }
        if (!hasOut && !n.outputs.empty()) sinkNodes.push_back(&n);
    }
    if (sinkNodes.empty()) for (const auto& n : nodes) if (!n.outputs.empty()) sinkNodes.push_back(&n);

    // Header
    h << "#pragma once\n";
    h << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n";
    h << "typedef struct {\n";
    for (const auto* n : inputNodes) h << "  " << toCType(n->outputs[0].dataType) << " " << n->id << ";\n";
    h << "} NodeFlowInputs;\n";
    h << "typedef struct {\n";
    for (const auto* n : sinkNodes) h << "  " << toCType(n->outputs[0].dataType) << " " << n->id << ";\n";
    h << "} NodeFlowOutputs;\n";
    h << "typedef struct {\n";
    h << "} NodeFlowState;\n";
    h << "void nodeflow_step(const NodeFlowInputs* in, NodeFlowOutputs* out, NodeFlowState* state);\n";
    h << "#ifdef __cplusplus\n}\n#endif\n";
    h.close();

    auto findNodeById = [&](const std::string& id) -> const Node* {
        for (const auto& n : nodes) if (n.id == id) return &n; return nullptr;
    };

    c << "#include \"" << headerPath << "\"\n";
    c << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n";
    c << "void nodeflow_step(const NodeFlowInputs* in, NodeFlowOutputs* out, NodeFlowState*) {\n";
    // Temp vars for node outputs
    for (const auto& n : nodes) if (!n.outputs.empty()) c << "  " << toCType(n.outputs[0].dataType) << " _" << n.id << " = 0;\n";
    c << "\n";
    // Execute in topo order
    for (const auto& nodeId : executionOrder) {
        const Node* n = findNodeById(nodeId);
        if (!n || n->outputs.empty()) continue;
        std::string outVar = std::string("_") + n->id;
        if (n->type == "DeviceTrigger") {
            c << "  " << outVar << " = in->" << n->id << ";\n";
        } else if (n->type == "Value") {
            double dv = 0.0;
            auto it = n->parameters.find("value");
            if (it != n->parameters.end()) {
                if (std::holds_alternative<int>(it->second)) dv = static_cast<double>(std::get<int>(it->second));
                else if (std::holds_alternative<float>(it->second)) dv = static_cast<double>(std::get<float>(it->second));
                else if (std::holds_alternative<double>(it->second)) dv = std::get<double>(it->second);
            }
            c << "  " << outVar << " = (" << dv << ");\n";
        } else if (n->type == "Add") {
            std::vector<std::string> src;
            for (const auto& inP : n->inputs) {
                for (const auto& cc : connections) if (cc.toNode == n->id && cc.toPort == inP.id) { src.push_back(std::string("_") + cc.fromNode); break; }
            }
            if (src.empty()) src.push_back("0");
            c << "  " << outVar << " = ";
            for (size_t i = 0; i < src.size(); ++i) { if (i) c << " + "; c << src[i]; }
            c << ";\n";
        }
    }
    c << "\n";
    // Write sinks
    for (const auto* sn : sinkNodes) c << "  out->" << sn->id << " = _" << sn->id << ";\n";
    c << "}\n";
    c << "#ifdef __cplusplus\n}\n#endif\n";
    c.close();
}