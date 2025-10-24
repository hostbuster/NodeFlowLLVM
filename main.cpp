// main.cpp
//
// Headless NodeFlow runtime with WebSocket IPC. Parses CLI (CLI11), loads the
// JSON flow, and starts a WS server that:
// - Sends a schema describing nodes/ports/handles
// - Broadcasts generic snapshots and per-port deltas
// - Accepts control messages (set/config/reload)
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

// Type-aware JSON number formatting for core runtime
static inline std::string jsonNumberForDtype(const std::string &dtype, double v, int floatPrecision = 3, bool trimZeros = true) {
    if (dtype == "int") {
        return fmt::format("{}", static_cast<int>(v));
    }
    if (trimZeros) {
        return fmt::format("{:.{}g}", v, floatPrecision);
    }
    return fmt::format("{:.{}f}", v, floatPrecision);
}

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
    bool buildAOTLLVM = false;
    std::string outDir;
    int wsPort = 9002;
    std::string wsPath = "/stream";
    bool bench = false;            // compute-only benchmark disables WS
    int benchRate = 0;             // Hz feeder
    int benchDuration = 0;         // seconds
    std::string perfOut;           // NDJSON file
    int perfIntervalMs = 1000;     // summary interval
    // WS delta aggregation
    int wsDeltaRateHz = 60;        // 0 = immediate
    int wsDeltaMaxBatch = 512;
    double wsDeltaEpsilon = 0.0;   // 0 disables epsilon filtering
    int wsHeartbeatSec = 15;       // 0 disables heartbeat
    bool wsDeltaFast = true;       // send immediate tiny delta on set
    int wsSnapshotIntervalSec = 0; // 0 = no periodic snapshots (only on connect)
    bool wsIncludeTime = false;    // include timing metadata in WS messages
    // Control/time model
    bool paused = false;
    std::string clockType = "wall"; // "wall" | "virtual"
    double timeScale = 1.0;
    int fixedRateHz = 0;            // virtual clock fixed step; 0=off
    CLI::App app{"NodeFlowCore"};
    try {
        app.add_option("--flow", flowPath, "Path to flow JSON file");
        app.add_flag("--build-aot", buildAOT, "Generate AOT step library using flow basename");
        app.add_flag("--aot-llvm", buildAOTLLVM, "Use LLVM-style backend when generating AOT (experimental)");
        app.add_option("--out-dir", outDir, "Directory to write generated AOT files");
        app.add_option("--ws-port", wsPort, "WebSocket port");
        app.add_option("--ws-path", wsPath, "WebSocket path (e.g., /stream)");
        // Bench/perf
        app.add_flag("--bench", bench, "Compute-only benchmark (disable WS)");
        app.add_option("--bench-rate", benchRate, "Feeder rate Hz for benchmark");
        app.add_option("--bench-duration", benchDuration, "Benchmark duration seconds");
        app.add_option("--perf-out", perfOut, "Write NDJSON perf summaries to file");
        app.add_option("--perf-interval", perfIntervalMs, "Perf summary interval ms");
        // WS delta aggregation
        app.add_option("--ws-delta-rate-hz", wsDeltaRateHz, "Delta flush rate in Hz (0=immediate)");
        app.add_option("--ws-delta-max-batch", wsDeltaMaxBatch, "Max keys per delta batch");
        app.add_option("--ws-delta-epsilon", wsDeltaEpsilon, "Float epsilon to suppress tiny changes (0=off)");
        app.add_option("--ws-heartbeat-sec", wsHeartbeatSec, "Send heartbeat every N seconds when idle (0=off)");
        app.add_flag("--ws-delta-fast", wsDeltaFast, "Send immediate tiny delta for set operations");
        app.add_option("--ws-snapshot-interval", wsSnapshotIntervalSec, "Periodic full snapshot interval seconds (0=off)");
        
        app.add_flag("--ws-time", wsIncludeTime, "Include timing metadata in WS messages");
        app.add_option("--clock", clockType, "Clock type: wall|virtual");
        app.add_option("--time-scale", timeScale, "Time scale multiplier (0..N)");
        app.add_option("--ws-fixed-rate", fixedRateHz, "Virtual clock fixed step Hz (0=off)");
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
        if (buildAOTLLVM || NODEFLOW_AOT_LLVM) engine.generateStepLibraryLLVM(base); else engine.generateStepLibrary(base);
        // Do not start runtime when building AOT artifacts
        return 0;
    }


    // No random thread needed; runtime is fully headless

    // Initialize ncurses (optional)
    // Startup message
    fmt::print("NodeFlowCore started. WS=on, flow='{}'\n", flowPath);

    // Bench compute-only mode: disable WS; feed inputs and measure
    if (bench) {
        using clk = std::chrono::steady_clock;
        auto tLast = clk::now();
        unsigned long long evalCount = 0, evalNsAccum = 0, evalNsMin = ~0ull, evalNsMax = 0;
        auto flushPerf = [&](bool force){
            if (!perfOut.empty()) {
                static FILE* fp = nullptr;
                if (!fp) fp = std::fopen(perfOut.c_str(), "w");
                if (fp) {
                    auto ps = engine.getAndResetPerfStats();
                    std::fprintf(fp,
                        "{\"type\":\"perf\",\"evalCount\":%llu,\"evalTimeNsAccum\":%llu,\"evalTimeNsMin\":%llu,\"evalTimeNsMax\":%llu,\"nodesEvaluated\":%llu,\"dependentsEnqueued\":%llu,\"readyQueueMax\":%llu}\n",
                        evalCount, evalNsAccum, evalNsMin, evalNsMax,
                        ps.nodesEvaluated, ps.dependentsEnqueued, ps.readyQueueMax);
                    if (force) std::fflush(fp);
                }
            }
            evalCount = 0; evalNsAccum = 0; evalNsMin = ~0ull; evalNsMax = 0;
        };
        using namespace std::chrono;
        const auto tick = (benchRate > 0) ? milliseconds(1000 / benchRate) : milliseconds(0);
        const auto endAt = (benchDuration > 0) ? clk::now() + seconds(benchDuration) : time_point<clk>::max();
        // Choose device triggers (outputs) as inputs; set round-robin
        std::vector<std::string> inputNodes;
        for (const auto &n : engine.getNodeDescs()) if (n.type == "DeviceTrigger") inputNodes.push_back(n.id);
        if (inputNodes.empty()) for (const auto &n : engine.getNodeDescs()) inputNodes.push_back(n.id);
        size_t rr = 0;
        while (clk::now() < endAt) {
            auto t0 = clk::now();
            if (!inputNodes.empty()) {
                const auto &node = inputNodes[rr % inputNodes.size()];
                float oldv = 0.0f;
                engine.setNodeValue(node, oldv); // ensure exists
                engine.setNodeValue(node, (rr & 1) ? 1.0f : 0.0f);
                ++rr;
            }
            engine.execute();
            auto t1 = clk::now();
            auto ns = (unsigned long long)duration_cast<nanoseconds>(t1 - t0).count();
            ++evalCount; evalNsAccum += ns; if (ns < evalNsMin) evalNsMin = ns; if (ns > evalNsMax) evalNsMax = ns;
            if (benchRate > 0 && tick.count() > 0) std::this_thread::sleep_for(tick);
            if (duration_cast<milliseconds>(clk::now() - tLast).count() >= perfIntervalMs) { flushPerf(true); tLast = clk::now(); }
        }
        flushPerf(true);
        return 0;
    }

    // WebSocket server (headless IPC)
    using WsServer = SimpleWeb::SocketServer<SimpleWeb::WS>;
    std::unique_ptr<WsServer> wsServer;
    std::thread wsThread;
    std::mutex wsMutex;
    std::mutex engineMutex;
    std::string latestJson; // last snapshot/delta, for simple demo
    // Delta aggregation state
    std::unordered_map<std::string, std::string> pendingDelta;
    auto lastFlush = std::chrono::steady_clock::now();
    auto lastActivity = std::chrono::steady_clock::now();
    std::string wsRegex; // compiled endpoint regex key for lookups
    // Timing metadata helpers available across WS handlers and main loop
    using SteadyClock = std::chrono::steady_clock;
    using SystemClock = std::chrono::system_clock;
    auto processStartSteady = SteadyClock::now();
    unsigned long long msgSeq = 0;
    double lastDtMsObserved = 0.0;
    auto buildT = [&]() {
        if (!wsIncludeTime) return std::string();
        auto nowSteady = SteadyClock::now();
        auto nowSys = SystemClock::now();
        auto wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(nowSys.time_since_epoch()).count();
        auto monoNs = std::chrono::duration_cast<std::chrono::nanoseconds>(nowSteady - processStartSteady).count();
        ++msgSeq;
        std::string t = ",\"t\":{\"wall_ms\":" + std::to_string((long long)wallMs)
                        + ",\"mono_ns\":" + std::to_string((long long)monoNs)
                        + ",\"dt_ms\":" + fmt::format("{:.3f}", lastDtMsObserved)
                        + ",\"clock\":\"" + clockType + "\""
                        + ",\"time_scale\":" + fmt::format("{:.3f}", timeScale)
                        + ",\"rate_hz\":" + std::to_string(fixedRateHz)
                        + ",\"seq\":" + std::to_string((long long)msgSeq) + "}";
        return t;
    };
    {
        wsServer = std::make_unique<WsServer>();
        wsServer->config.port = wsPort;

        // Simple-WebSocket-Server expects a regex endpoint; wrap /path as ^/path$
        std::string wsPattern = wsPath;
        if (wsPattern.empty() || wsPattern.front() != '^') wsPattern = "^" + wsPattern + "$";
        wsRegex = wsPattern; // keep compiled regex key for lookups

        auto &ep = wsServer->endpoint[wsPattern];

        auto buildSchema = [&]() {
            const auto &nodes = engine.getNodeDescs();
            const auto &ports = engine.getPortDescs();
            std::string s;
            s += "{\"type\":\"schema\"";
            s += buildT();
            s += ",";
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
            if (std::holds_alternative<float>(v)) return jsonNumberForDtype("float", (double)std::get<float>(v), 3);
            if (std::holds_alternative<double>(v)) return jsonNumberForDtype("double", (double)std::get<double>(v), 3);
            if (std::holds_alternative<int>(v)) return jsonNumberForDtype("int", (double)std::get<int>(v), 3);
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
            std::string js = "{\"type\":\"snapshot\"";
            js += buildT();
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
                    auto it_ep = wsServer->endpoint.find(wsRegex);
                    if (it_ep != wsServer->endpoint.end()) {
                        for (auto &c2 : it_ep->second.get_connections()) {
                            c2->send(snap);
                        }
                    }
                    lastActivity = std::chrono::steady_clock::now();
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
                    if (wsDeltaFast) {
                        // Try to send a tiny delta immediately for responsiveness
                        std::string node;
                        if (hasKey("handle")) {
                            int h2 = static_cast<int>(getNum("handle"));
                            const auto &ports2 = engine.getPortDescs();
                            if (h2 >= 0 && static_cast<size_t>(h2) < ports2.size()) node = ports2[h2].nodeId;
                        } else {
                            node = getStr("node");
                        }
                        // Prefer canonical key nodeId:portId for deltas
                        std::string key = node;
                        const auto &ports2b = engine.getPortDescs();
                        for (const auto &p2 : ports2b) {
                            if (p2.nodeId == node && p2.direction == "output") { key = node + ":" + p2.portId; break; }
                        }
                        // Value as formatted JSON number (use node's first output dtype if available)
                        std::string dtype = "float";
                        for (const auto &p2b : engine.getPortDescs()) { if (p2b.nodeId == node && p2b.direction == "output") { dtype = p2b.dataType; break; } }
                        std::string val = jsonNumberForDtype(dtype, (double)value, 3);
                std::string delta = std::string("{\"type\":\"delta\"");
                delta += buildT();
                delta += ",\"" + key + "\":" + val + "}\n";
                        auto it_ep = wsServer->endpoint.find(wsRegex);
                        if (it_ep != wsServer->endpoint.end()) {
                            for (auto &c2 : it_ep->second.get_connections()) c2->send(delta);
                        }
                        lastActivity = std::chrono::steady_clock::now();
                    } else {
                        broadcastSnapshot();
                    }
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
                } else if (type == "control") {
                    auto cmd = getStr("cmd");
                    if (cmd == "pause") { paused = true; conn->send("{\"ok\":true}\n"); }
                    else if (cmd == "resume") { paused = false; conn->send("{\"ok\":true}\n"); }
                    else if (cmd == "reset") {
                        std::lock_guard<std::mutex> engLock(engineMutex);
                        engine.loadFromJson(json);
                        conn->send("{\"ok\":true}\n");
                    }
                    else if (cmd == "step_eval") {
                        std::lock_guard<std::mutex> engLock(engineMutex);
                        engine.execute();
                        conn->send("{\"ok\":true}\n");
                    }
                    else if (cmd == "step_tick") {
                        double dt = getNum("dt_ms"); if (dt < 0) dt = 0.0;
                        {
                            std::lock_guard<std::mutex> engLock(engineMutex);
                            engine.tick(dt); engine.execute();
                        }
                        conn->send("{\"ok\":true}\n");
                    }
                    else if (cmd == "set_rate") { int hz = (int)getNum("hz"); fixedRateHz = std::max(0, hz); conn->send("{\"ok\":true}\n"); }
                    else if (cmd == "set_clock") { auto c = getStr("clock"); if (c=="wall"||c=="virtual") { clockType = c; conn->send("{\"ok\":true}\n"); } else conn->send("{\"ok\":false}\n"); }
                    else if (cmd == "set_time_scale") { double sc = getNum("scale"); if (sc < 0) sc = 0; timeScale = sc; conn->send("{\"ok\":true}\n"); }
                    else if (cmd == "status") {
                        std::string s = std::string("{\"type\":\"status\",\"mode\":\"") + (paused?"paused":"running") + "\",";
                        s += "\"clock\":\"" + clockType + "\",\"time_scale\":" + fmt::format("{:.3f}", timeScale) + ",\"rate_hz\":" + std::to_string(fixedRateHz) + ",\"eval_gen\":" + std::to_string((long long)engine.currentEvalGeneration()) + "}\n";
                        conn->send(s);
                    }
                    else { conn->send("{\"ok\":false}\n"); }
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
                // Always send a fresh snapshot on connect
                {
                    std::lock_guard<std::mutex> engLock(engineMutex);
                    // Ensure at least one execute so initial values/params propagate
                    engine.execute();
                }
                auto snap = buildSnapshot();
                latestJson = snap;
                conn->send(snap);
            } catch(...) {}
            fmt::print("client connected\n");
        };
        ep.on_close = [&](auto /*conn*/, int /*status*/, const std::string& /*reason*/){ fmt::print("client disconnected\n"); };

        wsThread = std::thread([&]{ wsServer->start(); });
    }

    // Main loop: Run flow and broadcast generic snapshots and deltas
    NodeFlow::Generation lastSnapshotGen = 0;
    using Steady = std::chrono::steady_clock;
    auto lastTs = Steady::now();
    auto lastFullSnapshot = Steady::now();
    while (running) {
        auto nowTs = Steady::now();
        double dtMs = (double)std::chrono::duration_cast<std::chrono::milliseconds>(nowTs - lastTs).count();
        if (clockType == "virtual") {
            if (fixedRateHz > 0) dtMs = 1000.0 / std::max(1, fixedRateHz);
            else dtMs = 16.667;
        }
        dtMs *= timeScale;
        lastTs = nowTs;
        // Track dt for timing envelope
        // Note: buildT captures this by value when composing messages
        (void)lastDtMsObserved; // keep var used when --ws-time enabled
        lastDtMsObserved = dtMs;
        if (!paused && dtMs > 0.0) engine.tick(dtMs);
        if (!paused) engine.execute();
        auto valueToJsonLoop = [](const NodeFlow::Value &v) -> std::string {
            if (std::holds_alternative<float>(v)) return jsonNumberForDtype("float", (double)std::get<float>(v), 3);
            if (std::holds_alternative<double>(v)) return jsonNumberForDtype("double", (double)std::get<double>(v), 3);
            if (std::holds_alternative<int>(v)) return jsonNumberForDtype("int", (double)std::get<int>(v), 3);
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

        // Periodic full snapshot (optional)
        if (wsServer && wsSnapshotIntervalSec > 0 && std::chrono::duration_cast<std::chrono::seconds>(Steady::now() - lastFullSnapshot).count() >= wsSnapshotIntervalSec) {
            std::string jsonOut = "{\"type\":\"snapshot\"";
            jsonOut += buildT();
            const auto &portsOut = engine.getPortDescs();
            std::unordered_map<std::string,int> outCount2;
            for (const auto &p : portsOut) if (p.direction == "output") ++outCount2[p.nodeId];
            for (const auto &p : portsOut) {
                if (p.direction != "output") continue;
                auto v = engine.readPort(p.handle);
                jsonOut += ",\""; jsonOut += p.nodeId; jsonOut += ":"; jsonOut += p.portId; jsonOut += "\":";
                jsonOut += valueToJsonLoop(v);
            }
            jsonOut += "}\n";
            {
                std::lock_guard<std::mutex> lock(wsMutex);
                latestJson = jsonOut;
            }
            auto endpoint_it = wsServer->endpoint.find(wsRegex);
            if (endpoint_it != wsServer->endpoint.end()) for (auto &conn : endpoint_it->second.get_connections()) conn->send(jsonOut);
            lastFullSnapshot = Steady::now();
        }

        // Delta aggregation using evaluation generation counters (per-port)
        auto curEvalGen = engine.currentEvalGeneration();
        auto deltas = engine.getPortDeltasChangedSince(lastSnapshotGen);
        if (!deltas.empty()) {
            for (const auto &t : deltas) {
                const auto &nodeId = std::get<0>(t);
                const auto &portId = std::get<1>(t);
                const auto &val = std::get<2>(t);
                // Build canonical key and formatted value
                std::string key = nodeId + ":" + portId;
                std::string v = valueToJsonLoop(val);
                // Optional epsilon suppression for floats
                if (wsDeltaEpsilon > 0.0) {
                    try {
                        double dv = std::stod(v);
                        auto itPrev = pendingDelta.find(key);
                        if (itPrev != pendingDelta.end()) {
                            double pv = std::stod(itPrev->second);
                            if (std::abs(dv - pv) < wsDeltaEpsilon) continue;
                        }
                    } catch(...) {}
                }
                pendingDelta[key] = v;
            }
            lastActivity = std::chrono::steady_clock::now();
        }
        // Advance watermark to current evaluation generation
        lastSnapshotGen = curEvalGen;

        // Flush window / heartbeat
        auto now = std::chrono::steady_clock::now();
        bool timeToFlush = (wsDeltaRateHz == 0) ? !pendingDelta.empty() : (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFlush).count() >= (1000 / std::max(1, wsDeltaRateHz)));
        if (wsServer && timeToFlush && !pendingDelta.empty()) {
            int count = 0;
            std::string delta = "{\"type\":\"delta\"";
            delta += buildT();
            for (const auto &kv : pendingDelta) {
                if (count >= wsDeltaMaxBatch) break;
                delta += ",\"" + kv.first + "\":" + kv.second;
                ++count;
            }
            delta += "}\n";
            auto endpoint_it2 = wsServer->endpoint.find(wsRegex);
            if (endpoint_it2 != wsServer->endpoint.end()) {
                auto &endpoint2 = endpoint_it2->second;
                for (auto &conn : endpoint2.get_connections()) conn->send(delta);
            }
            pendingDelta.clear();
            lastFlush = now;
            lastActivity = now;
        } else if (wsServer && wsHeartbeatSec > 0 && std::chrono::duration_cast<std::chrono::seconds>(now - lastActivity).count() >= wsHeartbeatSec) {
            auto endpoint_it2 = wsServer->endpoint.find(wsRegex);
            if (endpoint_it2 != wsServer->endpoint.end()) {
                for (auto &conn : endpoint_it2->second.get_connections()) conn->send("{\"type\":\"heartbeat\"}\n");
            }
            lastActivity = now;
        }

        // Small delay to prevent CPU overuse
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Cleanup
    
    running = false;

    return 0;
}