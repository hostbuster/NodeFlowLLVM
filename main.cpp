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

    // Resolve random trigger intervals from JSON (fallback to defaults)
    int rand_min_ms = 100;
    int rand_max_ms = 500;
    try {
        if (json.contains("nodes") && json["nodes"].is_array()) {
            for (const auto& n : json["nodes"]) {
                if (n.contains("type") && n["type"].is_string() && n["type"].get<std::string>() == "DeviceTrigger") {
                    if (n.contains("parameters") && n["parameters"].is_object()) {
                        const auto& p = n["parameters"];
                        if (p.contains("min_interval") && p["min_interval"].is_number_integer()) {
                            rand_min_ms = p["min_interval"].get<int>();
                        }
                        if (p.contains("max_interval") && p["max_interval"].is_number_integer()) {
                            rand_max_ms = p["max_interval"].get<int>();
                        }
                    }
                }
            }
        }
    } catch (...) {
        // keep defaults
    }

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
    std::string latestJson; // last snapshot/delta, for simple demo
    {
        wsServer = std::make_unique<WsServer>();
        wsServer->config.port = wsPort;

        // Simple-WebSocket-Server expects a regex endpoint; wrap /path as ^/path$
        std::string wsPattern = wsPath;
        if (wsPattern.empty() || wsPattern.front() != '^') wsPattern = "^" + wsPattern + "$";

        auto &ep = wsServer->endpoint[wsPattern];

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
                auto type = getStr("type");
                auto broadcastSnapshot = [&](){
                    engine.execute();
                    auto outs = engine.getOutputs();
                    auto getF = [&](const char* id){
                        auto it = outs.find(id);
                        if (it == outs.end() || it->second.empty()) return 0.0f;
                        const auto &v = it->second[0];
                        if (std::holds_alternative<float>(v)) return std::get<float>(v);
                        if (std::holds_alternative<double>(v)) return static_cast<float>(std::get<double>(v));
                        if (std::holds_alternative<int>(v)) return static_cast<float>(std::get<int>(v));
                        return 0.0f;
                    };
                    char buf[256];
                    int n = std::snprintf(buf, sizeof(buf),
                        "{\"type\":\"snapshot\",\"key1\":%.3f,\"key2\":%.3f,\"random1\":%.3f,\"add1\":%.3f}\n",
                        getF("key1"), getF("key2"), getF("random1"), getF("add1"));
                    std::string json(buf, n > 0 ? (size_t)n : 0);
                    {
                        std::lock_guard<std::mutex> lock(wsMutex);
                        latestJson = json;
                    }
                    auto it_ep = wsServer->endpoint.find(wsPattern);
                    if (it_ep != wsServer->endpoint.end()) {
                        for (auto &c2 : it_ep->second.get_connections()) {
                            c2->send(json);
                        }
                    }
                };
                if (type == "set") {
                    auto node = getStr("node");
                    float value = static_cast<float>(getNum("value"));
                    engine.setNodeValue(node, value);
                    conn->send("{\"ok\":true}\n");
                    broadcastSnapshot();
                } else if (type == "config") {
                    auto node = getStr("node");
                    int minI = static_cast<int>(getNum("min_interval"));
                    int maxI = static_cast<int>(getNum("max_interval"));
                    engine.setNodeConfigMinMax(node, minI, maxI);
                    conn->send("{\"ok\":true}\n");
                    broadcastSnapshot();
                } else if (type == "reload") {
                    auto path = getStr("flow");
                    std::ifstream f(path);
                    if (f.good()) { nlohmann::json j; f >> j; engine.loadFromJson(j); conn->send("{\"ok\":true}\n"); broadcastSnapshot(); }
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
            if (!latestJson.empty()) conn->send(latestJson);
            fmt::print("client connected\n");
        };
        ep.on_close = [&](auto /*conn*/, int /*status*/, const std::string& /*reason*/){ fmt::print("client disconnected\n"); };

        wsThread = std::thread([&]{ wsServer->start(); });
    }

    // Main loop: Run flow and update display
    float last_sum = -9999.0f;
    while (running) {
        engine.execute();
        auto outputs = engine.getOutputs();
        auto getFloat = [&](const std::string& nodeId) -> float {
            if (!outputs.count(nodeId) || outputs[nodeId].empty()) return 0.0f;
            const auto& v = outputs[nodeId][0];
            if (std::holds_alternative<float>(v)) return std::get<float>(v);
            if (std::holds_alternative<double>(v)) return static_cast<float>(std::get<double>(v));
            if (std::holds_alternative<int>(v)) return static_cast<float>(std::get<int>(v));
            return 0.0f;
        };

        float key1_val = getFloat("key1");
        float key2_val = getFloat("key2");
        float random_val = getFloat("random1");
        float engine_sum = 0.0f;
        if (outputs.count("add1") && !outputs["add1"].empty()) {
            const auto& v = outputs["add1"][0];
            engine_sum = std::holds_alternative<float>(v) ? std::get<float>(v)
                        : std::holds_alternative<double>(v) ? static_cast<float>(std::get<double>(v))
                        : std::holds_alternative<int>(v) ? static_cast<float>(std::get<int>(v))
                        : 0.0f;
        }
        float calc_sum = key1_val + key2_val + random_val;

        // Update display only if sum changes
        if (calc_sum != last_sum) {
            last_sum = calc_sum;
        }

        {
            // Emit a compact JSON snapshot for the demo
            std::string json = fmt::format(
                "{{\"type\":\"snapshot\",\"key1\":{:.3f},\"key2\":{:.3f},\"random1\":{:.3f},\"add1\":{:.3f}}}\n",
                key1_val, key2_val, random_val, engine_sum);
            {
                std::lock_guard<std::mutex> lock(wsMutex);
                latestJson = json;
            }
            if (wsServer) {
                auto endpoint_it = wsServer->endpoint.find(wsPath);
                if (endpoint_it != wsServer->endpoint.end()) {
                    auto &endpoint = endpoint_it->second;
                    for (auto &conn : endpoint.get_connections()) {
                        conn->send(json);
                    }
                }
            }
        }

        // Small delay to prevent CPU overuse
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Cleanup
    
    running = false;

    return 0;
}