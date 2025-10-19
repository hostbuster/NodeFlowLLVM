#include "NodeFlowCore.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <ncurses.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <iostream>

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

int main() {
    // Initialize random seed
    std::srand(std::time(nullptr));

    // Load JSON flow
    NodeFlow::FlowEngine engine;
    nlohmann::json json;
    {
        std::ifstream file1("devicetrigger_addition.json");
        if (file1.good()) {
            file1 >> json;
        } else {
            std::ifstream file2("../devicetrigger_addition.json");
            if (file2.good()) {
                file2 >> json;
            } else {
                std::ifstream file3("../../devicetrigger_addition.json");
                if (file3.good()) {
                    file3 >> json;
                } else {
                    throw std::runtime_error("Could not find devicetrigger_addition.json in ., .., or ../..\n");
                }
            }
        }
    }
    engine.loadFromJson(json);

    // Generate standalone binary from current flow (no interaction needed)
#if NODEFLOW_CODEGEN
    engine.compileToExecutable("nodeflow_output");
#endif

    // Start random number thread
    std::thread random_thread(randomNumberThread, 100, 500);

    // Initialize ncurses
    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    printw("Press '1' or '2' to trigger values, 'q' to quit\n");

    // Main loop: Run flow and update display
    float last_sum = -1.0f;
    while (running) {
        engine.execute();
        auto outputs = engine.getOutputs();
        float sum = 0.0f;
        if (outputs.count("add1") && !outputs["add1"].empty() && std::holds_alternative<float>(outputs["add1"][0])) {
            sum = std::get<float>(outputs["add1"][0]);
        }

        // Update display only if sum changes
        if (sum != last_sum) {
            clear();
            printw("Press '1' or '2' to trigger values, 'q' to quit\n");
            printw("Current sum: %.2f\n", sum);
            printw("Key1 (1.0): %s\n", key1_triggered ? "Triggered" : "Waiting");
            printw("Key2 (2.0): %s\n", key2_triggered ? "Triggered" : "Waiting");
            printw("Random (0-100): %.2f\n", random_value.load());
            refresh();
            last_sum = sum;
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