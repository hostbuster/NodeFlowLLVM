#include "NodeFlowCore.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <iostream>
#include <CLI/CLI.hpp>
#include <fmt/core.h>
#include "third_party/Simple-WebSocket-Server/server_ws.hpp"
#include <mutex>

// Global state
std::atomic<bool> running(true);


int main(int argc, char** argv) {
    // Initialize random seed
    std::srand(std::time(nullptr));

    // Load JSON flow
    NodeFlow::FlowEngine engine;
    // Resolve flow file path
    std::string flowPath = "devicetrigger_addition.json";
    bool buildAOT = false;
    std::string outDir;
    int wsPort = 9002;
    std::string wsPath = "/stream";
    CLI::App app{"NodeFlowCore"};
    try {
        app.add_option("--flow", flowPath, "Path to flow JSON file");
        app.add_flag("--build-aot", buildAOT, "Generate AOT step library using flow basename");
        app.add_option("--out-dir", outDir, "Directory to write generated AOT files");
        app.add_option("--ws-port", wsPort, "WebSocket port");
        app.add_option("--ws-path", wsPath, "WebSocket path (e.g., /stream)");
        
        app.allow_extras(false);
        app.validate_positionals();
        app.set_config();
        app.set_help_all_flag("--help-all", "Show all help");
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        return app.exit(e);
    }

    nlohmann::json json;
    {
        std::ifstream f(flowPath);
        if (f.good()) {
            f >> json;
        } else {
            std::ifstream f2("../" + flowPath);
            if (f2.good()) {
                f2 >> json;
            } else {
                std::ifstream f3("../../" + flowPath);
                if (f3.good()) {
                    f3 >> json;
                } else {
                    throw std::runtime_error("Could not find flow file: " + flowPath);
                }
            }
        }
    }
    engine.loadFromJson(json);

    // No random interval parsing here; inputs are driven externally via IPC

    // AOT generation (Option B) using flow basename
    if (buildAOT) {
        auto slash = flowPath.find_last_of("/\\");
        std::string base = (slash == std::string::npos) ? flowPath : flowPath.substr(slash + 1);
        auto dot = base.rfind('.');
        if (dot != std::string::npos) base = base.substr(0, dot);
        if (!outDir.empty()) {
            // chdir to outDir for generation, then return
            std::error_code ec;
            // POSIX chdir is fine; use std::filesystem in C++17 for path ops
            // We'll just prefix base with outDir when writing files by temporarily changing cwd
            // Simpler: prepend outDir to base
            base = outDir + "/" + base;
        }
        engine.generateStepLibrary(base);
        // Do not start runtime when building AOT artifacts
        return 0;
    }

    // Generate standalone binary from current flow (no interaction needed)
#if NODEFLOW_CODEGEN
    engine.compileToExecutable("nodeflow_output");
#endif

    // No random thread needed; runtime is fully headless

    // Initialize ncurses (optional)
    // Startup message
    fmt::print("NodeFlowCore started. WS=on, flow='{}'\n", flowPath);

    // WebSocket server (optional)
    using WsServer = SimpleWeb::SocketServer<SimpleWeb::WS>;
    std::unique_ptr<WsServer> wsServer;
    std::thread wsThread;
    std::mutex wsMutex;
    std::mutex engineMutex;
    std::string latestJson; // last snapshot/delta, for simple demo
    {
        wsServer = std::make_unique<WsServer>();
        wsServer->config.port = wsPort;

        // Simple-WebSocket-Server expects a regex endpoint; wrap /path as ^/path$
        std::string wsPattern = wsPath;
        if (wsPattern.empty() || wsPattern.front() != '^') wsPattern = "^" + wsPattern + "$";

        auto &ep = wsServer->endpoint[wsPattern];

        auto buildSchema = [&]() {
            const auto &nodes = engine.getNodeDescs();
            const auto &ports = engine.getPortDescs();
            std::string s;
            s += "{\"type\":\"schema\",";
            s += "\"nodes\":[";
            for (size_t i = 0; i < nodes.size(); ++i) {
                const auto &n = nodes[i];
                if (i) s += ",";
                s += "{\"id\":\"" + n.id + "\",\"type\":\"" + n.type + "\"}";
            }
            s += "],";
            s += "\"ports\":[";
            for (size_t i = 0; i < ports.size(); ++i) {
                const auto &p = ports[i];
                if (i) s += ",";
                s += "{\"handle\":" + std::to_string(p.handle)
                   + ",\"nodeId\":\"" + p.nodeId + "\""
                   + ",\"portId\":\"" + p.portId + "\""
                   + ",\"direction\":\"" + p.direction + "\""
                   + ",\"dtype\":\"" + p.dataType + "\"}";
            }
            s += "]}\n";
            return s;
        };

        auto valueToJson = [](const NodeFlow::Value &v) -> std::string {
            if (std::holds_alternative<float>(v)) return fmt::format("{:.3f}", std::get<float>(v));
            if (std::holds_alternative<double>(v)) return fmt::format("{:.3f}", std::get<double>(v));
            if (std::holds_alternative<int>(v)) return fmt::format("{}", std::get<int>(v));
            if (std::holds_alternative<std::string>(v)) {
                const auto &s = std::get<std::string>(v);
                std::string esc; esc.reserve(s.size()+2);
                esc.push_back('"');
                for (char c : s) { if (c=='"' || c=='\\') esc.push_back('\\'); esc.push_back(c);} 
                esc.push_back('"');
                return esc;
            }
            return "null";
        };

        auto buildSnapshot = [&]() {
            std::lock_guard<std::mutex> engLock(engineMutex);
            engine.execute();
            std::string js = "{\"type\":\"snapshot\"";
            const auto &ports = engine.getPortDescs();
            // count outputs per node
            std::unordered_map<std::string,int> outCount;
            for (const auto &p : ports) if (p.direction == "output") ++outCount[p.nodeId];
            for (const auto &p : ports) {
                if (p.direction != "output") continue;
                auto val = engine.readPort(p.handle);
                // canonical key
                js += ",\""; js += p.nodeId; js += ":"; js += p.portId; js += "\":";
                js += valueToJson(val);
                // alias for single-output nodes to keep existing UI working (nodeId only)
                if (outCount[p.nodeId] == 1) {
                    js += ",\""; js += p.nodeId; js += "\":"; js += valueToJson(val);
                }
            }
            js += "}\n";
            return js;
        };

        ep.on_message = [&, wsPattern](auto conn, auto msg){
            try {
                auto data = msg->string();
                // Expect small JSON commands like: {"type":"set","node":"key1","value":1.0}
                // Minimal parser: look for substrings (to avoid dep)
                auto has = [&](const char* s){ return data.find(s) != std::string::npos; };
                auto getStr = [&](const char* key)->std::string{
                    auto p = data.find(std::string("\"") + key + "\"");
                    if (p == std::string::npos) return {};
                    p = data.find(':', p);
                    if (p == std::string::npos) return {};
                    auto q1 = data.find('"', p+1);
                    auto q2 = data.find('"', q1+1);
                    if (q1 == std::string::npos || q2 == std::string::npos) return {};
                    return data.substr(q1+1, q2-q1-1);
                };
                auto getNum = [&](const char* key)->double{
                    auto p = data.find(std::string("\"") + key + "\"");
                    if (p == std::string::npos) return 0.0;
                    p = data.find(':', p);
                    if (p == std::string::npos) return 0.0;
                    auto q = data.find_first_of(",}\n ", p+1);
                    auto s = data.substr(p+1, (q==std::string::npos?data.size():q) - (p+1));
                    return std::atof(s.c_str());
                };
                auto hasKey = [&](const char* key){
                    auto p = data.find(std::string("\"") + key + "\"");
                    if (p == std::string::npos) return false;
                    p = data.find(':', p);
                    return p != std::string::npos;
                };
                auto type = getStr("type");
                auto broadcastSnapshot = [&](){
                    std::string snap = buildSnapshot();
                    {
                        std::lock_guard<std::mutex> lock(wsMutex);
                        latestJson = snap;
                    }
                    auto it_ep = wsServer->endpoint.find(wsPattern);
                    if (it_ep != wsServer->endpoint.end()) {
                        for (auto &c2 : it_ep->second.get_connections()) {
                            c2->send(snap);
                        }
                    }
                };
                if (type == "set") {
                    float value = static_cast<float>(getNum("value"));
                    if (hasKey("handle")) {
                        int handle = static_cast<int>(getNum("handle"));
                        const auto &ports = engine.getPortDescs();
                        if (handle >= 0 && static_cast<size_t>(handle) < ports.size()) {
                            const auto &pd = ports[handle];
                            // For device inputs/outputs, set node value
                            {
                                std::lock_guard<std::mutex> engLock2(engineMutex);
                                engine.setNodeValue(pd.nodeId, value);
                            }
                        }
                    } else {
                        auto node = getStr("node");
                        {
                            std::lock_guard<std::mutex> engLock3(engineMutex);
                            engine.setNodeValue(node, value);
                        }
                    }
                    conn->send("{\"ok\":true}\n");
                    broadcastSnapshot();
                } else if (type == "config") {
                    auto node = getStr("node");
                    int minI = static_cast<int>(getNum("min_interval"));
                    int maxI = static_cast<int>(getNum("max_interval"));
                    {
                        std::lock_guard<std::mutex> engLock4(engineMutex);
                        engine.setNodeConfigMinMax(node, minI, maxI);
                    }
                    conn->send("{\"ok\":true}\n");
                    broadcastSnapshot();
                } else if (type == "reload") {
                    auto path = getStr("flow");
                    std::ifstream f(path);
                    if (f.good()) { nlohmann::json j; f >> j; { std::lock_guard<std::mutex> engLock5(engineMutex); engine.loadFromJson(j); } conn->send("{\"ok\":true}\n"); broadcastSnapshot(); }
                    else conn->send("{\"ok\":false}\n");
                } else if (type == "subscribe") {
                    conn->send("{\"ok\":true}\n");
                } else {
                    conn->send("{\"ok\":false,\"err\":\"unknown type\"}\n");
                }
            } catch(...) {
                try { conn->send("{\"ok\":false}\n"); } catch(...) {}
            }
        };

        ep.on_open = [&](auto conn){
            std::lock_guard<std::mutex> lock(wsMutex);
            try {
                auto schema = buildSchema();
                conn->send(schema);
            } catch(...) {}
            if (!latestJson.empty()) conn->send(latestJson);
            fmt::print("client connected\n");
        };
        ep.on_close = [&](auto /*conn*/, int /*status*/, const std::string& /*reason*/){ fmt::print("client disconnected\n"); };

        wsThread = std::thread([&]{ wsServer->start(); });
    }

    // Main loop: Run flow and broadcast generic snapshots and deltas
    NodeFlow::Generation lastSnapshotGen = 0;
    while (running) {
        engine.execute();
        auto valueToJsonLoop = [](const NodeFlow::Value &v) -> std::string {
            if (std::holds_alternative<float>(v)) return fmt::format("{:.3f}", std::get<float>(v));
            if (std::holds_alternative<double>(v)) return fmt::format("{:.3f}", std::get<double>(v));
            if (std::holds_alternative<int>(v)) return fmt::format("{}", std::get<int>(v));
            if (std::holds_alternative<std::string>(v)) {
                const auto &s = std::get<std::string>(v);
                std::string esc; esc.reserve(s.size()+2);
                esc.push_back('"');
                for (char c : s) { if (c=='"' || c=='\\') esc.push_back('\\'); esc.push_back(c);} 
                esc.push_back('"');
                return esc;
            }
            return "null";
        };

        std::string jsonOut = "{\"type\":\"snapshot\"";
        const auto &portsOut = engine.getPortDescs();
        std::unordered_map<std::string,int> outCount2;
        for (const auto &p : portsOut) if (p.direction == "output") ++outCount2[p.nodeId];
        for (const auto &p : portsOut) {
            if (p.direction != "output") continue;
            auto v = engine.readPort(p.handle);
            jsonOut += ",\""; jsonOut += p.nodeId; jsonOut += ":"; jsonOut += p.portId; jsonOut += "\":";
            jsonOut += valueToJsonLoop(v);
            if (outCount2[p.nodeId] == 1) {
                jsonOut += ",\""; jsonOut += p.nodeId; jsonOut += "\":"; jsonOut += valueToJsonLoop(v);
            }
        }
        jsonOut += "}\n";

        {
            std::lock_guard<std::mutex> lock(wsMutex);
            latestJson = jsonOut;
        }
        if (wsServer) {
            auto endpoint_it = wsServer->endpoint.find(wsPath);
            if (endpoint_it != wsServer->endpoint.end()) {
                auto &endpoint = endpoint_it->second;
                for (auto &conn : endpoint.get_connections()) {
                    conn->send(jsonOut);
                }
            }
        }

        // Delta emission using generation counters (per-port)
        auto snapGen = engine.beginSnapshot();
        auto deltas = engine.getPortDeltasChangedSince(lastSnapshotGen);
        if (!deltas.empty()) {
            std::string delta = "{\"type\":\"delta\"";
            for (const auto &t : deltas) {
                const auto &nodeId = std::get<0>(t);
                const auto &portId = std::get<1>(t);
                const auto &val = std::get<2>(t);
                delta += ",\""; delta += nodeId; delta += ":"; delta += portId; delta += "\":";
                delta += valueToJsonLoop(val);
            }
            delta += "}\n";
            if (wsServer) {
                auto endpoint_it2 = wsServer->endpoint.find(wsPath);
                if (endpoint_it2 != wsServer->endpoint.end()) {
                    auto &endpoint2 = endpoint_it2->second;
                    for (auto &conn : endpoint2.get_connections()) {
                        conn->send(delta);
                    }
                }
            }
        }
        lastSnapshotGen = snapGen;

        // Small delay to prevent CPU overuse
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Cleanup
    
    running = false;

    return 0;
}