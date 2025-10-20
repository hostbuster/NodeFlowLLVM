#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/core.h>

#ifndef STEP_HEADER
#error "STEP_HEADER must be defined to the generated header token, e.g., devicetrigger_addition_step.h"
#endif

// Use pragma include_next-like trick by stringizing the macro
#define STR1(x) #x
#define STR(x) STR1(x)
#include STR(STEP_HEADER)

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

int main(int argc, char** argv) {
    NodeFlowInputs in{};
    NodeFlowOutputs out{};
    NodeFlowState state{};

    float key1 = parseFloatArg(argc, argv, "--key1", 1.0f);
    float key2 = parseFloatArg(argc, argv, "--key2", 2.0f);
    float random1 = parseFloatArg(argc, argv, "--random1", 0.0f);

    in.key1 = key1;
    in.key2 = key2;
    in.random1 = random1;

    nodeflow_step(&in, &out, &state);

    fmt::print("add1={:.6f}\n", out.add1);
    return 0;
}


