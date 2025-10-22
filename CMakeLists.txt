cmake_minimum_required(VERSION 3.10)
project(NodeFlowCore)

# Diagnostics
message(STATUS "System: ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "Compiler: C=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Enable C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Codegen toggle (default ON)
option(NODEFLOW_CODEGEN "Enable standalone code generation" ON)
option(NODEFLOW_BUILD_RUNTIME "Build interactive runtime (NodeFlowCore)" ON)
option(AOT_BACKEND_LLVM "Use LLVM-style backend for AOT generation" OFF)

# Find nlohmann_json
find_package(nlohmann_json REQUIRED)


# OpenSSL (needed by Simple-WebSocket-Server for base64/sha1 even with WS)
find_package(OpenSSL QUIET)
if(OPENSSL_FOUND)
  message(STATUS "OpenSSL version: ${OPENSSL_VERSION}")
  message(STATUS "OpenSSL root: ${OPENSSL_ROOT_DIR}")
  message(STATUS "OpenSSL include: ${OPENSSL_INCLUDE_DIR}")
  message(STATUS "OpenSSL libraries: ${OPENSSL_LIBRARIES}")
  if(TARGET OpenSSL::SSL)
    get_target_property(OPENSSL_SSL_IMPORTED_LOCATION OpenSSL::SSL IMPORTED_LOCATION)
    message(STATUS "OpenSSL::SSL IMPORTED_LOCATION: ${OPENSSL_SSL_IMPORTED_LOCATION}")
  endif()
  if(TARGET OpenSSL::Crypto)
    get_target_property(OPENSSL_CRYPTO_IMPORTED_LOCATION OpenSSL::Crypto IMPORTED_LOCATION)
    message(STATUS "OpenSSL::Crypto IMPORTED_LOCATION: ${OPENSSL_CRYPTO_IMPORTED_LOCATION}")
  endif()
else()
  message(WARNING "OpenSSL not found by find_package. Falling back to manual discovery.")
  # Try Homebrew arm64 locations first
  if(NOT OPENSSL_INCLUDE_DIR)
    find_path(OPENSSL_INCLUDE_DIR openssl/ssl.h
      PATHS /opt/homebrew/opt/openssl@3/include /usr/local/opt/openssl/include
    )
  endif()
  if(NOT OPENSSL_SSL_LIB_FOUND)
    find_library(OPENSSL_SSL_LIB_FOUND NAMES ssl libssl libssl.3
      PATHS /opt/homebrew/opt/openssl@3/lib /usr/local/opt/openssl/lib
    )
  endif()
  if(NOT OPENSSL_CRYPTO_LIB_FOUND)
    find_library(OPENSSL_CRYPTO_LIB_FOUND NAMES crypto libcrypto libcrypto.3
      PATHS /opt/homebrew/opt/openssl@3/lib /usr/local/opt/openssl/lib
    )
  endif()
  if(OPENSSL_INCLUDE_DIR AND OPENSSL_SSL_LIB_FOUND AND OPENSSL_CRYPTO_LIB_FOUND)
    message(STATUS "Manually resolved OpenSSL include: ${OPENSSL_INCLUDE_DIR}")
    message(STATUS "Manually resolved OpenSSL SSL lib: ${OPENSSL_SSL_LIB_FOUND}")
    message(STATUS "Manually resolved OpenSSL CRYPTO lib: ${OPENSSL_CRYPTO_LIB_FOUND}")
    # Create imported targets to unify downstream logic
    add_library(OpenSSL::SSL UNKNOWN IMPORTED)
    set_target_properties(OpenSSL::SSL PROPERTIES IMPORTED_LOCATION ${OPENSSL_SSL_LIB_FOUND}
      INTERFACE_INCLUDE_DIRECTORIES ${OPENSSL_INCLUDE_DIR})
    add_library(OpenSSL::Crypto UNKNOWN IMPORTED)
    set_target_properties(OpenSSL::Crypto PROPERTIES IMPORTED_LOCATION ${OPENSSL_CRYPTO_LIB_FOUND}
      INTERFACE_INCLUDE_DIRECTORIES ${OPENSSL_INCLUDE_DIR})
    set(OPENSSL_FOUND TRUE)
  else()
    message(FATAL_ERROR "Could not resolve OpenSSL manually. Set OPENSSL_ROOT_DIR to your OpenSSL prefix (e.g., /opt/homebrew/opt/openssl@3).")
  endif()
endif()

# Standalone Asio include (header-only)
find_path(ASIO_INCLUDE_DIR asio.hpp
  PATHS /opt/homebrew/opt/asio/include /usr/local/opt/asio/include
)
if(ASIO_INCLUDE_DIR)
  message(STATUS "Using Asio include dir: ${ASIO_INCLUDE_DIR}")
endif()

# CLI11 (required). Auto-fetch if not installed
include(FetchContent)
find_package(CLI11 QUIET CONFIG)
if(NOT CLI11_FOUND)
  message(STATUS "CLI11 not found, fetching...")
  FetchContent_Declare(
    CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.4.2
  )
  FetchContent_MakeAvailable(CLI11)
endif()

# fmt (required)
find_package(fmt REQUIRED)


# Header-only Simple-WebSocket-Server (INTERFACE)
add_library(Simple-WebSocket-Server INTERFACE)
target_include_directories(Simple-WebSocket-Server INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/Simple-WebSocket-Server
)
if(ASIO_INCLUDE_DIR)
  target_include_directories(Simple-WebSocket-Server INTERFACE ${ASIO_INCLUDE_DIR})
endif()

# Resolve OpenSSL link targets or libs robustly
set(NF_SSL_TARGETS 0)
if(TARGET OpenSSL::SSL)
  target_link_libraries(Simple-WebSocket-Server INTERFACE OpenSSL::SSL OpenSSL::Crypto)
  set(NF_SSL_TARGETS 1)
endif()
if(NF_SSL_TARGETS EQUAL 0)
  # Fallback for environments where imported targets are missing
  # Try to find versioned libraries (OpenSSL 3 on Homebrew uses libssl.3.dylib/libcrypto.3.dylib)
  find_library(OPENSSL_SSL_LIB_FOUND NAMES ssl libssl libssl.3 PATHS ${OPENSSL_ROOT_DIR}/lib /opt/homebrew/opt/openssl@3/lib /usr/local/opt/openssl/lib NO_DEFAULT_PATH)
  find_library(OPENSSL_CRYPTO_LIB_FOUND NAMES crypto libcrypto libcrypto.3 PATHS ${OPENSSL_ROOT_DIR}/lib /opt/homebrew/opt/openssl@3/lib /usr/local/opt/openssl/lib NO_DEFAULT_PATH)
  if(NOT OPENSSL_SSL_LIB_FOUND OR NOT OPENSSL_CRYPTO_LIB_FOUND)
    # Last resort to generic variables
    set(OPENSSL_SSL_LIB_FOUND ${OPENSSL_SSL_LIBRARY})
    set(OPENSSL_CRYPTO_LIB_FOUND ${OPENSSL_CRYPTO_LIBRARY})
  endif()
  message(STATUS "Resolved OpenSSL libs: SSL=${OPENSSL_SSL_LIB_FOUND} CRYPTO=${OPENSSL_CRYPTO_LIB_FOUND}")
  target_include_directories(Simple-WebSocket-Server INTERFACE ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(Simple-WebSocket-Server INTERFACE ${OPENSSL_SSL_LIB_FOUND} ${OPENSSL_CRYPTO_LIB_FOUND})
endif()

# Prefer Homebrew arm64 OpenSSL paths at link and runtime on macOS
if(APPLE)
  target_link_options(Simple-WebSocket-Server INTERFACE
    -L/opt/homebrew/opt/openssl@3/lib
    -Wl,-rpath,/opt/homebrew/opt/openssl@3/lib
    -Wl,-search_paths_first
  )
endif()

# Add executable
if(NODEFLOW_BUILD_RUNTIME)
  add_executable(NodeFlowCore main.cpp NodeFlowCore.cpp)

  # Link libraries
  target_link_libraries(NodeFlowCore PRIVATE
      nlohmann_json::nlohmann_json
  )
  target_link_libraries(NodeFlowCore PRIVATE fmt::fmt)
  # Always enable WebSocket IPC
  target_link_libraries(NodeFlowCore PRIVATE Simple-WebSocket-Server)
  # Fallback include/libs if imported targets are not available
  if(NOT TARGET OpenSSL::SSL)
    target_include_directories(NodeFlowCore PRIVATE ${OPENSSL_INCLUDE_DIR})
    # Reuse the resolved libs above if available
    if(OPENSSL_SSL_LIB_FOUND AND OPENSSL_CRYPTO_LIB_FOUND)
      target_link_libraries(NodeFlowCore PRIVATE ${OPENSSL_SSL_LIB_FOUND} ${OPENSSL_CRYPTO_LIB_FOUND})
    else()
      target_link_libraries(NodeFlowCore PRIVATE ${OPENSSL_LIBRARIES})
    endif()
  endif()
  target_compile_definitions(NodeFlowCore PRIVATE ASIO_STANDALONE)
  if(APPLE)
    target_link_options(NodeFlowCore PRIVATE
      -L/opt/homebrew/opt/openssl@3/lib
      -Wl,-rpath,/opt/homebrew/opt/openssl@3/lib
      -Wl,-search_paths_first
    )
  endif()
  target_link_libraries(NodeFlowCore PRIVATE CLI11::CLI11)
  target_compile_definitions(NodeFlowCore PRIVATE NODEFLOW_HAS_CLI11=1)

  # Set output directory for the compiled executable
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

  # Define preprocessor flag for codegen
  if(NODEFLOW_CODEGEN)
    target_compile_definitions(NodeFlowCore PRIVATE NODEFLOW_CODEGEN=1)
  else()
    target_compile_definitions(NodeFlowCore PRIVATE NODEFLOW_CODEGEN=0)
  endif()
  if(AOT_BACKEND_LLVM)
    target_compile_definitions(NodeFlowCore PRIVATE NODEFLOW_AOT_LLVM=1)
  else()
    target_compile_definitions(NodeFlowCore PRIVATE NODEFLOW_AOT_LLVM=0)
  endif()
endif()

# AOT: build any *_step.cpp present into static libraries (source and build dirs)
foreach(STEP_DIR IN ITEMS ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR})
  file(GLOB NODEFLOW_STEP_SOURCES "${STEP_DIR}/*_step.cpp")
  foreach(STEP_SRC ${NODEFLOW_STEP_SOURCES})
    get_filename_component(STEP_NAME ${STEP_SRC} NAME_WE)
    get_filename_component(FOUND_STEP_DIR ${STEP_SRC} DIRECTORY)
    string(REPLACE "_step" "" BASE_NAME ${STEP_NAME})
    if(NOT TARGET ${BASE_NAME}_step)
      add_library(${BASE_NAME}_step STATIC ${STEP_SRC})
      target_include_directories(${BASE_NAME}_step PUBLIC ${FOUND_STEP_DIR})
      set_target_properties(${BASE_NAME}_step PROPERTIES OUTPUT_NAME ${BASE_NAME}_step ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    endif()

    # Host example executable for each step lib
    if(NOT TARGET ${BASE_NAME}_host)
      add_executable(${BASE_NAME}_host aot_host_template.cpp)
      target_include_directories(${BASE_NAME}_host PRIVATE ${FOUND_STEP_DIR})
      target_compile_definitions(${BASE_NAME}_host PRIVATE STEP_HEADER=${STEP_NAME}.h)
      target_link_libraries(${BASE_NAME}_host PRIVATE ${BASE_NAME}_step fmt::fmt CLI11::CLI11 Simple-WebSocket-Server)
      if(ASIO_INCLUDE_DIR)
        target_include_directories(${BASE_NAME}_host PRIVATE ${ASIO_INCLUDE_DIR})
      endif()
      target_compile_definitions(${BASE_NAME}_host PRIVATE ASIO_STANDALONE)
      if(NOT TARGET OpenSSL::SSL)
        target_include_directories(${BASE_NAME}_host PRIVATE ${OPENSSL_INCLUDE_DIR})
        if(OPENSSL_SSL_LIB_FOUND AND OPENSSL_CRYPTO_LIB_FOUND)
          target_link_libraries(${BASE_NAME}_host PRIVATE ${OPENSSL_SSL_LIB_FOUND} ${OPENSSL_CRYPTO_LIB_FOUND})
        else()
          target_link_libraries(${BASE_NAME}_host PRIVATE ${OPENSSL_LIBRARIES})
        endif()
      endif()
      set_target_properties(${BASE_NAME}_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    endif()
  endforeach()
endforeach()


# AOT-LLVM: build any *_step.ll present into static libraries (plus desc glue)
#foreach(LL_DIR IN ITEMS ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR})
#  file(GLOB NODEFLOW_LL_SOURCES "${LL_DIR}/*_step.ll")
#  foreach(LL_SRC ${NODEFLOW_LL_SOURCES})
#    get_filename_component(LL_NAME ${LL_SRC} NAME_WE)
#    get_filename_component(FOUND_LL_DIR ${LL_SRC} DIRECTORY)
#    string(REPLACE "_step" "" BASE_NAME ${LL_NAME})
#    # Compile IR to object
#    set(LL_OBJ ${CMAKE_BINARY_DIR}/${LL_NAME}.o)
#    add_custom_command(OUTPUT ${LL_OBJ}
#      COMMAND ${CMAKE_CXX_COMPILER} -x ir -O3 -c ${LL_SRC} -o ${LL_OBJ}
#      DEPENDS ${LL_SRC}
#      COMMENT "Compiling LLVM IR ${LL_SRC}")
#    # Descriptor glue is generated alongside the IR
#    set(DESC_SRC ${FOUND_LL_DIR}/${BASE_NAME}_step_desc.cpp)
#    add_library(${BASE_NAME}_step_llvm STATIC ${LL_OBJ} ${DESC_SRC})
#    target_include_directories(${BASE_NAME}_step_llvm PUBLIC ${FOUND_LL_DIR})
#    set_target_properties(${BASE_NAME}_step_llvm PROPERTIES OUTPUT_NAME ${BASE_NAME}_step_llvm ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
#    # Host for LLVM variant (include dir where header lives)
#    add_executable(${BASE_NAME}_host_llvm aot_host_template.cpp)
#    target_include_directories(${BASE_NAME}_host_llvm PRIVATE ${FOUND_LL_DIR})
#    target_compile_definitions(${BASE_NAME}_host_llvm PRIVATE STEP_HEADER=${LL_NAME}.h)
#    target_link_libraries(${BASE_NAME}_host_llvm PRIVATE ${BASE_NAME}_step_llvm fmt::fmt CLI11::CLI11 Simple-WebSocket-Server)
#    if(ASIO_INCLUDE_DIR)
#      target_include_directories(${BASE_NAME}_host_llvm PRIVATE ${ASIO_INCLUDE_DIR})
#    endif()
#    target_compile_definitions(${BASE_NAME}_host_llvm PRIVATE ASIO_STANDALONE)
#    if(NOT TARGET OpenSSL::SSL)
#      target_include_directories(${BASE_NAME}_host_llvm PRIVATE ${OPENSSL_INCLUDE_DIR})
#      if(OPENSSL_SSL_LIB_FOUND AND OPENSSL_CRYPTO_LIB_FOUND)
#        target_link_libraries(${BASE_NAME}_host_llvm PRIVATE ${OPENSSL_SSL_LIB_FOUND} ${OPENSSL_CRYPTO_LIB_FOUND})
#      else()
#        target_link_libraries(${BASE_NAME}_host_llvm PRIVATE ${OPENSSL_LIBRARIES})
#      endif()
#    endif()
#    set_target_properties(${BASE_NAME}_host_llvm PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
#  endforeach()
#endforeach()