#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/core.h>
#include <vector>
#include <string>
#include <utility>
#include <thread>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <CLI/CLI.hpp>
#include "third_party/Simple-WebSocket-Server/server_ws.hpp"

#ifndef STEP_HEADER
#error "STEP_HEADER must be defined to the generated header token, e.g., devicetrigger_addition_step.h"
#endif

// Use pragma include_next-like trick by stringizing the macro
#define STR1(x) #x
#define STR(x) STR1(x)
#include STR(STEP_HEADER)

// If descriptors are available, print a short schema so hosts can bind
// dynamically without hardcoding node/port ids.
extern "C" {
  extern const int NODEFLOW_NUM_PORTS;
  extern const NodeFlowPortDesc NODEFLOW_PORTS[];
  extern const int NODEFLOW_NUM_TOPO;
  extern const int NODEFLOW_TOPO_ORDER[];
  extern const int NODEFLOW_NUM_INPUT_FIELDS;
  extern const NodeFlowInputField NODEFLOW_INPUT_FIELDS[];
}

static float parseFloatArg(int argc, char** argv, const char* name, float defVal) {
    const size_t nlen = std::strlen(name);
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], name, nlen) == 0) {
            const char* eq = argv[i] + nlen;
            if (*eq == '=') return std::strtof(eq + 1, nullptr);
            if (*eq == '\0' && i + 1 < argc) return std::strtof(argv[i + 1], nullptr);
        }
    }
    return defVal;
}

static std::vector<std::pair<std::string, double>> parseSets(int argc, char** argv) {
    std::vector<std::pair<std::string, double>> out;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--set=", 6) == 0) {
            const char* s = argv[i] + 6; // node=value
            const char* eq = std::strchr(s, '=');
            if (!eq) continue;
            std::string key(s, (size_t)(eq - s));
            double val = std::strtod(eq + 1, nullptr);
            out.emplace_back(key, val);
        }
    }
    return out;
}

static int parseIntArg(int argc, char** argv, const char* name, int defVal) {
    const size_t nlen = std::strlen(name);
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], name, nlen) == 0) {
            const char* eq = argv[i] + nlen;
            if (*eq == '=') return std::strtol(eq + 1, nullptr, 10);
            if (*eq == '\0' && i + 1 < argc) return std::strtol(argv[i + 1], nullptr, 10);
        }
    }
    return defVal;
}

int main(int argc, char** argv) {
    // CLI11: parse options
    int rateHz = 0;
    int durationSec = 0;
    bool wsEnable = false;
    int wsPort = 9002;
    std::string wsPath = "/stream";
    std::vector<std::string> setArgs;
    bool list = false;

    try {
        CLI::App app{"NodeFlow AOT Host"};
        app.add_option("--rate", rateHz, "Tick rate Hz (optional)");
        app.add_option("--duration", durationSec, "Duration seconds for timed run");
        app.add_flag("--ws-enable", wsEnable, "Enable WebSocket server");
        app.add_option("--ws-port", wsPort, "WebSocket port (default 9002)");
        app.add_option("--ws-path", wsPath, "WebSocket path (default /stream)");
        app.add_option("--set", setArgs, "Set input as node=value (repeatable)")->expected(-1);
        app.add_flag("--list", list, "List input fields for this flow and exit");
        app.set_help_all_flag("--help-all", "Show all help");
        app.allow_extras();
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        CLI::App dummy;
        return dummy.exit(e);
    }
    if (NODEFLOW_NUM_PORTS > 0) {
        fmt::print("[host] schema: ports={} topo={}\n", NODEFLOW_NUM_PORTS, NODEFLOW_NUM_TOPO);
        for (int i = 0; i < NODEFLOW_NUM_PORTS; ++i) {
            const auto &p = NODEFLOW_PORTS[i];
            fmt::print("  handle={} {}:{} dir={} dtype={}\n", p.handle, p.nodeId, p.portId, p.is_output?"out":"in", p.dtype);
        }
    }
    NodeFlowInputs in{};   // zero-initialized; use --set node=value to set inputs
    NodeFlowOutputs out{};
    NodeFlowState state{};

    // Apply dynamic --set node=value using NODEFLOW_INPUT_FIELDS
    auto sets = parseSets(argc, argv);
    for (const auto &s : setArgs) {
        const char* c = s.c_str();
        const char* eq = std::strchr(c, '=');
        if (!eq) continue;
        sets.emplace_back(std::string(c, (size_t)(eq-c)), std::strtod(eq+1, nullptr));
    }
    for (const auto &kv : sets) {
        const std::string &node = kv.first;
        double val = kv.second;
        bool applied = false;
        for (int i = 0; i < NODEFLOW_NUM_INPUT_FIELDS; ++i) {
            const auto &f = NODEFLOW_INPUT_FIELDS[i];
            if (node == f.nodeId) {
                char* base = reinterpret_cast<char*>(&in);
                char* loc = base + f.offset;
                // f.dtype is one of "int","double","float"
                if (std::strcmp(f.dtype, "int") == 0) {
                    *reinterpret_cast<int*>(loc) = static_cast<int>(val);
                } else if (std::strcmp(f.dtype, "double") == 0) {
                    *reinterpret_cast<double*>(loc) = static_cast<double>(val);
                } else { // float default
                    *reinterpret_cast<float*>(loc) = static_cast<float>(val);
                }
                applied = true;
            }
        }
        if (!applied) fmt::print("[host] warn: no input field for '{}'\n", node);
    }

    // Optional loop: --rate <Hz> and --duration <seconds> (already parsed by CLI)
    // Optionally list input fields for this flow
    if (list) {
        fmt::print("[host] input fields (use --set name=value):\n");
        for (int i = 0; i < NODEFLOW_NUM_INPUT_FIELDS; ++i) {
            const auto &f = NODEFLOW_INPUT_FIELDS[i];
            fmt::print("  {} (dtype={})\n", f.nodeId, f.dtype);
        }
        return 0;
    }

    // Optional WebSocket server for schema/snapshots (compare with runtime)
    using WsServer = SimpleWeb::SocketServer<SimpleWeb::WS>;
    std::unique_ptr<WsServer> wsServer;
    std::thread wsThread;
    std::mutex wsMutex;
    std::mutex hostMutex; // guards in/out/state during WS access
    std::string latestJson;
    std::function<std::string()> buildSnapshot;
    std::function<std::string()> buildDelta;
    std::string wsRegex; // compiled regex key used in endpoint map
    // Expose input field lookup beyond the WS init block so buildSnapshot remains valid
    std::unordered_map<std::string, std::pair<size_t,const char*>> inputLookup;
    // Track last-sent output values for delta emission
    std::unordered_map<int,double> lastOutByHandle;
    if (wsEnable) {
        wsServer = std::make_unique<WsServer>();
        wsServer->config.port = wsPort;
        std::string wsPattern = wsPath;
        if (wsPattern.empty() || wsPattern.front() != '^') wsPattern = "^" + wsPattern + "$";
        wsRegex = wsPattern;
        auto &ep = wsServer->endpoint[wsPattern];
        // Build quick lookup of input fields: nodeId -> (offset,dtype)
        for (int i = 0; i < NODEFLOW_NUM_INPUT_FIELDS; ++i) {
            inputLookup[NODEFLOW_INPUT_FIELDS[i].nodeId] = {NODEFLOW_INPUT_FIELDS[i].offset, NODEFLOW_INPUT_FIELDS[i].dtype};
        }
        auto buildSchema = [&](){
            std::string s = "{\"type\":\"schema\",\"ports\":[";
            for (int i = 0; i < NODEFLOW_NUM_PORTS; ++i) {
                if (i) s += ",";
                const auto &p = NODEFLOW_PORTS[i];
                s += "{\"handle\":" + std::to_string(p.handle) + ",\"nodeId\":\"" + p.nodeId + "\",\"portId\":\"" + p.portId + "\",\"direction\":\"" + (p.is_output?"output":"input") + "\",\"dtype\":\"" + p.dtype + "\"}";
            }
            s += "]}\n";
            return s;
        };
        buildSnapshot = [&](){
            // Count outputs per node to emit plain node alias for single-output nodes
            std::unordered_map<std::string,int> outCount;
            for (int i = 0; i < NODEFLOW_NUM_PORTS; ++i) {
                const auto &p = NODEFLOW_PORTS[i];
                if (p.is_output) ++outCount[p.nodeId];
            }
            std::string js = "{\"type\":\"snapshot\"";
            std::lock_guard<std::mutex> lock(hostMutex);
            for (int i = 0; i < NODEFLOW_NUM_PORTS; ++i) {
                const auto &p = NODEFLOW_PORTS[i];
                if (!p.is_output) continue;
                double v = 0.0;
                auto itIn = inputLookup.find(p.nodeId);
                if (itIn != inputLookup.end()) {
                    const auto &fld = itIn->second;
                    const char* base = reinterpret_cast<const char*>(&in);
                    const char* loc = base + fld.first;
                    if (std::strcmp(fld.second, "int") == 0)      v = (double)(*reinterpret_cast<const int*>(loc));
                    else if (std::strcmp(fld.second, "double") == 0) v = (*reinterpret_cast<const double*>(loc));
                    else                                             v = (double)(*reinterpret_cast<const float*>(loc));
                } else {
                    v = nodeflow_get_output(p.handle, &out, &state);
                }
                js += ",\""; js += p.nodeId; js += ":"; js += p.portId; js += "\":";
                js += fmt::format("{:.3f}", v);
                if (outCount[p.nodeId] == 1) {
                    js += ",\""; js += p.nodeId; js += "\":"; js += fmt::format("{:.3f}", v);
                }
            }
            js += "}\n";
            return js;
        };
        buildDelta = [&](){
            // Builds {"type":"delta", "node:port": value, ...} only for changed outputs
            std::string js;
            int changed = 0;
            {
                std::lock_guard<std::mutex> lock(hostMutex);
                for (int i = 0; i < NODEFLOW_NUM_PORTS; ++i) {
                    const auto &p = NODEFLOW_PORTS[i];
                    if (!p.is_output) continue;
                    double v = 0.0;
                    auto itIn = inputLookup.find(p.nodeId);
                    if (itIn != inputLookup.end()) {
                        const auto &fld = itIn->second;
                        const char* base = reinterpret_cast<const char*>(&in);
                        const char* loc = base + fld.first;
                        if (std::strcmp(fld.second, "int") == 0)      v = (double)(*reinterpret_cast<const int*>(loc));
                        else if (std::strcmp(fld.second, "double") == 0) v = (*reinterpret_cast<const double*>(loc));
                        else                                             v = (double)(*reinterpret_cast<const float*>(loc));
                    } else {
                        v = nodeflow_get_output(p.handle, &out, &state);
                    }
                    auto itPrev = lastOutByHandle.find(p.handle);
                    bool isChanged = (itPrev == lastOutByHandle.end()) || (std::abs(itPrev->second - v) > 1e-9);
                    if (isChanged) {
                        if (changed == 0) js = "{\"type\":\"delta\""; // start object lazily
                        js += ",\""; js += p.nodeId; js += ":"; js += p.portId; js += "\":";
                        js += fmt::format("{:.3f}", v);
                        ++changed;
                        lastOutByHandle[p.handle] = v;
                    }
                }
            }
            if (changed > 0) { js += "}\n"; return js; }
            return std::string{};
        };
        auto has = [&](const std::string &data, const char* key){ return data.find(key) != std::string::npos; };
        auto getStr = [&](const std::string &data, const char* key)->std::string{
            auto p = data.find(std::string("\"") + key + "\"");
            if (p == std::string::npos) return {};
            p = data.find(':', p);
            if (p == std::string::npos) return {};
            auto q1 = data.find('"', p+1);
            auto q2 = data.find('"', q1+1);
            if (q1 == std::string::npos || q2 == std::string::npos) return {};
            return data.substr(q1+1, q2-q1-1);
        };
        auto getNum = [&](const std::string &data, const char* key)->double{
            auto p = data.find(std::string("\"") + key + "\"");
            if (p == std::string::npos) return 0.0;
            p = data.find(':', p);
            if (p == std::string::npos) return 0.0;
            auto q = data.find_first_of(",}\n ", p+1);
            auto s = data.substr(p+1, (q==std::string::npos?data.size():q) - (p+1));
            return std::atof(s.c_str());
        };
        auto setInputByNode = [&](const std::string &node, double val){
            for (int i = 0; i < NODEFLOW_NUM_INPUT_FIELDS; ++i) {
                const auto &f = NODEFLOW_INPUT_FIELDS[i];
                if (node == f.nodeId) {
                    char* base = reinterpret_cast<char*>(&in);
                    char* loc = base + f.offset;
                    if (std::strcmp(f.dtype, "int") == 0)      *reinterpret_cast<int*>(loc) = static_cast<int>(val);
                    else if (std::strcmp(f.dtype, "double") == 0) *reinterpret_cast<double*>(loc) = static_cast<double>(val);
                    else                                           *reinterpret_cast<float*>(loc) = static_cast<float>(val);
                }
            }
        };
        ep.on_message = [&](auto conn, auto msg){
            try {
                auto data = msg->string();
                auto type = getStr(data, "type");
                if (type == "set") {
                    double value = getNum(data, "value");
                    if (has(data, "\"handle\"")) {
                        int handle = static_cast<int>(getNum(data, "handle"));
                        {
                            std::lock_guard<std::mutex> lock(hostMutex);
                            nodeflow_set_input(handle, value, &in, &state);
                        }
                    } else {
                        auto node = getStr(data, "node");
                        if (!node.empty()) {
                            std::lock_guard<std::mutex> lock(hostMutex);
                            setInputByNode(node, value);
                        }
                    }
                    // Recompute immediately for instant feedback
                    {
                        std::lock_guard<std::mutex> lock(hostMutex);
                        nodeflow_step(&in, &out, &state);
                    }
                    try { conn->send("{\"ok\":true}\n"); } catch(...) {}
                    // Broadcast new snapshot immediately
                    if (buildSnapshot && wsServer) {
                        std::string snap = buildSnapshot();
                        {
                            std::lock_guard<std::mutex> lock(wsMutex);
                            latestJson = snap;
                        }
                        auto it = wsServer->endpoint.find(wsRegex);
                        if (it != wsServer->endpoint.end()) {
                            for (auto &c : it->second.get_connections()) c->send(snap);
                        }
                        if (buildDelta) {
                            std::string delta = buildDelta();
                            if (!delta.empty()) {
                                auto it2 = wsServer->endpoint.find(wsRegex);
                                if (it2 != wsServer->endpoint.end()) {
                                    for (auto &c : it2->second.get_connections()) c->send(delta);
                                }
                            }
                        }
                    }
                } else if (type == "subscribe") {
                    try { conn->send("{\"ok\":true}\n"); } catch(...) {}
                } else {
                    try { conn->send("{\"ok\":false}\n"); } catch(...) {}
                }
            } catch(...) {
                try { conn->send("{\"ok\":false}\n"); } catch(...) {}
            }
        };
        ep.on_open = [&](auto conn){
            try { conn->send(buildSchema()); } catch(...) {}
            {
                std::lock_guard<std::mutex> lock(wsMutex);
                if (!latestJson.empty()) conn->send(latestJson);
            }
            fmt::print("[host] ws client connected\n");
        };
        ep.on_close = [&](auto, int, const std::string&){ fmt::print("[host] ws client disconnected\n"); };
        wsThread = std::thread([&]{
            try {
                fmt::print("[host] ws listening on {}{}\n", wsPort, wsPath);
                wsServer->start();
            } catch (const std::exception &ex) {
                fmt::print("[host] ws start error: {}\n", ex.what());
            } catch (...) {
                fmt::print("[host] ws start unknown error\n");
            }
        });
    }

    if (rateHz > 0 && durationSec > 0) {
        using namespace std::chrono;
        const auto tick = milliseconds(1000 / rateHz);
        const auto endAt = steady_clock::now() + seconds(durationSec);
        while (steady_clock::now() < endAt) {
            {
                std::lock_guard<std::mutex> lock(hostMutex);
                nodeflow_step(&in, &out, &state);
            }
            // Print all outputs generically
            for (int i = 0; i < NODEFLOW_NUM_PORTS; ++i) {
                const auto &p = NODEFLOW_PORTS[i];
                if (!p.is_output) continue;
                double v = nodeflow_get_output(p.handle, &out, &state);
                fmt::print("{}:{}={:.6f}\n", p.nodeId, p.portId, v);
            }
            if (wsEnable && wsServer && buildSnapshot) {
                std::string snap = buildSnapshot();
                {
                    std::lock_guard<std::mutex> lock(wsMutex);
                    latestJson = snap;
                }
                auto it = wsServer->endpoint.find(wsRegex);
                if (it != wsServer->endpoint.end()) {
                    for (auto &c : it->second.get_connections()) c->send(snap);
                }
            }
            std::this_thread::sleep_for(tick);
        }
    } else {
        {
            std::lock_guard<std::mutex> lock(hostMutex);
            nodeflow_step(&in, &out, &state);
        }
        for (int i = 0; i < NODEFLOW_NUM_PORTS; ++i) {
            const auto &p = NODEFLOW_PORTS[i];
            if (!p.is_output) continue;
            double v = nodeflow_get_output(p.handle, &out, &state);
            fmt::print("{}:{}={:.6f}\n", p.nodeId, p.portId, v);
        }
        if (wsEnable && wsServer && buildSnapshot) {
            // If WS is enabled without a rate/duration, keep serving snapshots until Ctrl+C
            using namespace std::chrono_literals;
            fmt::print("[host] WS enabled - serving snapshots. Press Ctrl+C to exit.\n");
            while (true) {
                {
                    std::lock_guard<std::mutex> lock(hostMutex);
                    nodeflow_step(&in, &out, &state);
                }
                std::string snap = buildSnapshot();
                {
                    std::lock_guard<std::mutex> lock(wsMutex);
                    latestJson = snap;
                }
                auto it = wsServer->endpoint.find(wsRegex);
                if (it != wsServer->endpoint.end()) {
                    for (auto &c : it->second.get_connections()) c->send(snap);
                }
                if (buildDelta) {
                    std::string delta = buildDelta();
                    if (!delta.empty()) {
                        auto it2 = wsServer->endpoint.find(wsRegex);
                        if (it2 != wsServer->endpoint.end()) {
                            for (auto &c : it2->second.get_connections()) c->send(delta);
                        }
                    }
                }
                std::this_thread::sleep_for(100ms);
            }
        }
    }

    // Graceful shutdown of WS thread if running
    if (wsEnable && wsServer) {
        try { wsServer->stop(); } catch(...) {}
        if (wsThread.joinable()) wsThread.join();
    }
    return 0;
}


