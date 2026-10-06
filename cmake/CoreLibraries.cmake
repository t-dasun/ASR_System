# Shared contracts and system dependencies.
add_library(asr_core INTERFACE)
add_library(asr::core ALIAS asr_core)
target_include_directories(asr_core INTERFACE src/core/include)
include(cmake/FoundationDependencies.cmake)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
add_library(asr_warnings INTERFACE)
target_compile_options(asr_warnings INTERFACE -Wall -Wextra -Wpedantic)
add_library(asr_engines INTERFACE)
target_include_directories(asr_engines INTERFACE src/engines/interfaces/include)
target_link_libraries(asr_engines INTERFACE asr::core)

# Audio and runtime-independent engine implementations.
add_library(asr_audio src/audio/src/wav.cpp src/audio/src/delivery.cpp)
target_include_directories(asr_audio PUBLIC src/audio/include)
target_link_libraries(asr_audio PUBLIC asr::core PRIVATE asr_warnings)
add_library(asr_mock src/engines/mock/src/mock_engine.cpp)
target_include_directories(asr_mock PUBLIC src/engines/mock/include)
target_link_libraries(asr_mock PUBLIC asr_engines PRIVATE asr_warnings)

# Worker routing and session lifecycle.
add_library(asr_scheduler src/backend/scheduler/src/scheduler.cpp)
target_include_directories(asr_scheduler PUBLIC src/backend/scheduler/include)
target_link_libraries(asr_scheduler PUBLIC asr::core PRIVATE asr_warnings)
add_library(asr_worker_executor src/backend/workers/src/executor.cpp)
target_include_directories(asr_worker_executor PUBLIC src/backend/workers/include)
target_link_libraries(asr_worker_executor PUBLIC asr_engines PRIVATE asr_warnings)
add_library(asr_session_manager src/backend/sessions/src/session_manager.cpp)
target_include_directories(asr_session_manager PUBLIC src/backend/sessions/include)
target_link_libraries(asr_session_manager PUBLIC asr_scheduler asr_worker_executor asr_engines PRIVATE asr_warnings)

# Configuration and artifact storage.
add_library(asr_config src/config/src/config.cpp)
target_include_directories(asr_config PUBLIC src/config/include)
target_link_libraries(asr_config PUBLIC nlohmann_json::nlohmann_json PRIVATE yaml-cpp::yaml-cpp asr_warnings)
add_library(asr_storage src/storage/src/repository.cpp)
target_include_directories(asr_storage PUBLIC src/storage/include)
target_link_libraries(asr_storage PUBLIC asr::core nlohmann_json::nlohmann_json PRIVATE asr_warnings)

# Simulation runners, measurements, and service transport.
add_library(asr_baseline src/benchmark/src/baseline.cpp src/benchmark/src/load.cpp src/benchmark/src/sweep.cpp)
add_library(asr_observability src/observability/src/metrics.cpp src/observability/src/system_sampler.cpp)
target_include_directories(asr_observability PUBLIC src/observability/include)
target_link_libraries(asr_observability PUBLIC asr::core nlohmann_json::nlohmann_json Threads::Threads PRIVATE asr_warnings)
add_library(asr_websocket src/backend/transport/src/websocket.cpp src/backend/transport/src/api_service.cpp)
target_include_directories(asr_websocket PUBLIC src/backend/transport/include)
target_link_libraries(asr_websocket PUBLIC asr_engines asr_storage asr_observability asr_config
  OpenSSL::Crypto Threads::Threads PRIVATE asr_warnings)
target_include_directories(asr_baseline PUBLIC src/benchmark/include)
target_link_libraries(asr_baseline PUBLIC asr_audio asr_config asr_engines asr_storage asr_observability asr_session_manager OpenSSL::Crypto PRIVATE asr_warnings)

# User-facing executable; child workers are defined in RuntimeTargets.cmake.
add_executable(asr-cli
  apps/asr_cli/main.cpp
  apps/asr_cli/engine_runtime.cpp
  apps/asr_cli/service.cpp)
target_link_libraries(asr-cli PRIVATE asr_baseline asr_mock asr_session_manager asr_websocket Threads::Threads asr_warnings)
