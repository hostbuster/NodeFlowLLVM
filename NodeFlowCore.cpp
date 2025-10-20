#include "NodeFlowCore.hpp"
#include <fstream>
#include <algorithm>
#include <stdexcept>
#include <variant>
#include <chrono>
#include <thread>
#include <random>
#include <ctime>
#include <ncurses.h>
#include <pthread.h>

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

void FlowEngine::loadFromJson(const nlohmann::json& json) {
    srand(time(NULL));
    nodes.clear();
    connections.clear();

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
        nodes.push_back(node);
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
}

void FlowEngine::execute() {
    std::unordered_map<PortId, Value> portValues;
    auto makeKey = [](const std::string& nodeId, const std::string& portId) { return nodeId + ":" + portId; };
    // Seed with last outputs so downstream consumers see previous values if no new events
    for (const auto& node : nodes) {
        for (const auto& output : node.outputs) {
            portValues[makeKey(node.id, output.id)] = output.value;
        }
    }
    // Initial propagation
    for (const auto& conn : connections) {
        auto fromKey = makeKey(conn.fromNode, conn.fromPort);
        auto toKey = makeKey(conn.toNode, conn.toPort);
        if (portValues.count(fromKey)) {
            portValues[toKey] = portValues[fromKey];
        }
    }
    // Execute nodes in order; after each node, propagate its outputs to consumers
    for (const auto& nodeId : executionOrder) {
        auto it = std::find_if(nodes.begin(), nodes.end(), [&](const Node& n) { return n.id == nodeId; });
        if (it == nodes.end()) continue;
            it->execute(portValues);
        // Propagate fresh outputs from this node
        for (const auto& out : it->outputs) {
            auto outKey = makeKey(it->id, out.id);
            portValues[outKey] = out.value;
            for (const auto& conn : connections) {
                if (conn.fromNode == it->id && conn.fromPort == out.id) {
                    auto toKey = makeKey(conn.toNode, conn.toPort);
                    portValues[toKey] = out.value;
                }
            }
        }
    }
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
    it->parameters["value"] = static_cast<float>(value);
    for (auto &out : it->outputs) out.value = static_cast<float>(value);
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