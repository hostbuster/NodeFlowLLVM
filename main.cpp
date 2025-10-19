#include "NodeFlowCore.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <ncurses.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <iostream>
#if NODEFLOW_HAS_CLI11
#include <CLI/CLI.hpp>
#endif

// Global state for async triggers
std::atomic<bool> key1_triggered(false);
std::atomic<bool> key2_triggered(false);
std::atomic<bool> random_triggered(false);
std::atomic<float> random_value(0.0f);
std::atomic<bool> running(true);

// Thread function for random number generation
void randomNumberThread(int min_interval, int max_interval) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(min_interval, max_interval);
    std::uniform_real_distribution<float> value_dist(0.0f, 100.0f);

    while (running) {
        random_value = value_dist(gen);
        random_triggered = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(dist(gen)));
    }
}

// Override isKeyPressed to use ncurses
extern "C" int isKeyPressed(const char* key) {
    static bool initialized = false;
    if (!initialized) {
        initscr();
        cbreak();
        noecho();
        nodelay(stdscr, TRUE);
        initialized = true;
    }
    int ch = getch();
    if (ch == 'q') {
        running = false;
        endwin();
        return 0;
    }
    if (ch == key[0]) {
        if (key[0] == '1') key1_triggered = true;
        if (key[0] == '2') key2_triggered = true;
        return 1;
    }
    // If a different key was pressed, push it back so other checks can consume it
    if (ch != ERR) {
        ungetch(ch);
    }
    return 0;
}

// Override isRandomReady to check thread trigger
extern "C" int isRandomReady(int min_interval, int max_interval) {
    if (random_triggered) {
        random_triggered = false;
        return 1;
    }
    return 0;
}

// Override getRandomNumber to use thread-generated value
extern "C" float getRandomNumber() {
    return random_value;
}

int main(int argc, char** argv) {
    // Initialize random seed
    std::srand(std::time(nullptr));

    // Load JSON flow
    NodeFlow::FlowEngine engine;
    // Resolve flow file path
    std::string flowPath = "devicetrigger_addition.json";
#if NODEFLOW_HAS_CLI11
    try {
        CLI::App app{"NodeFlowCore"};
        app.add_option("--flow", flowPath, "Path to flow JSON file");
        app.allow_extras(false);
        app.validate_positionals();
        app.set_config();
        app.set_help_all_flag("--help-all", "Show all help");
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        return CLI::Exit(e);
    }
#else
    // Simple fallback: parse --flow=<path>
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        const std::string prefix = "--flow=";
        if (arg.rfind(prefix, 0) == 0) {
            flowPath = arg.substr(prefix.size());
        } else if (arg == "--flow" && i + 1 < argc) {
            flowPath = argv[++i];
        }
    }
#endif

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

    // Generate standalone binary from current flow (no interaction needed)
#if NODEFLOW_CODEGEN
    engine.compileToExecutable("nodeflow_output");
#endif

    // Start random number thread with JSON-configured intervals
    std::thread random_thread(randomNumberThread, rand_min_ms, rand_max_ms);

    // Initialize ncurses
    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    printw("Press '1' or '2' to trigger values, 'q' to quit\n");

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
            clear();
            printw("Press '1' or '2' to trigger values, 'q' to quit\n");
            printw("Key1 value:   %.2f\n", key1_val);
            printw("Key2 value:   %.2f\n", key2_val);
            printw("Random value: %.2f\n", random_val);
            printw("Sum (calc):   %.2f\n", calc_sum);
            printw("Sum (engine): %.2f\n", engine_sum);
            refresh();
            last_sum = calc_sum;
        }

        // Small delay to prevent CPU overuse
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Cleanup
    endwin();
    running = false;
    random_thread.join();

    return 0;
}