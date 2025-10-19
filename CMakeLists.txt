cmake_minimum_required(VERSION 3.10)
project(NodeFlowCore)

# Enable C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Codegen toggle (default ON)
option(NODEFLOW_CODEGEN "Enable standalone code generation" ON)

# Find nlohmann_json
find_package(nlohmann_json REQUIRED)

# Optional CLI11
find_package(CLI11 QUIET)

# Find ncurses
find_package(Curses REQUIRED)
include_directories(${CURSES_INCLUDE_DIR})

# Add executable
add_executable(NodeFlowCore main.cpp NodeFlowCore.cpp)

# Link libraries (drop LLVM for now)
target_link_libraries(NodeFlowCore nlohmann_json::nlohmann_json ${CURSES_LIBRARIES})
if(CLI11_FOUND)
  target_link_libraries(NodeFlowCore CLI11::CLI11)
  target_compile_definitions(NodeFlowCore PRIVATE NODEFLOW_HAS_CLI11=1)
else()
  target_compile_definitions(NodeFlowCore PRIVATE NODEFLOW_HAS_CLI11=0)
endif()

# Set output directory for the compiled executable
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Define preprocessor flag for codegen
if(NODEFLOW_CODEGEN)
  target_compile_definitions(NodeFlowCore PRIVATE NODEFLOW_CODEGEN=1)
else()
  target_compile_definitions(NodeFlowCore PRIVATE NODEFLOW_CODEGEN=0)
endif()