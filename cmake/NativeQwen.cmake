# Build only the reviewed C CPU sources, never upstream CUDA/ROCm targets.
set(QWEN_REVISION "924694251d9e0f18e5d86bbd06aa3ab5f870002d")
set(QWEN_SOURCE "${PROJECT_SOURCE_DIR}/third_party/qwen-asr")
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${QWEN_SOURCE}" rev-parse HEAD
  OUTPUT_VARIABLE actual_revision OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE git_result)
if(NOT git_result EQUAL 0 OR NOT actual_revision STREQUAL QWEN_REVISION)
  message(FATAL_ERROR "Fetch the pinned runtime first: bash scripts/fetch_native.sh")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${QWEN_SOURCE}" status --porcelain --untracked-files=no
  OUTPUT_VARIABLE vendor_changes OUTPUT_STRIP_TRAILING_WHITESPACE)
if(vendor_changes)
  message(FATAL_ERROR "Native source has tracked changes; record and review patches before building.")
endif()
find_path(OPENBLAS_INCLUDE_DIR cblas.h
  HINTS "${PROJECT_SOURCE_DIR}/third_party/openblas-sdk/usr/include/openblas"
  PATH_SUFFIXES openblas)
find_library(OPENBLAS_LIBRARY NAMES openblas openblaso libopenblaso.so.0)
if(NOT OPENBLAS_INCLUDE_DIR OR NOT OPENBLAS_LIBRARY)
  message(FATAL_ERROR "OpenBLAS headers/library missing. See third_party/README.md.")
endif()
find_package(Threads REQUIRED)
set(qwen_sources qwen_asr.c qwen_asr_kernels.c qwen_asr_kernels_generic.c
  qwen_asr_kernels_neon.c qwen_asr_kernels_avx.c qwen_asr_audio.c
  qwen_asr_encoder.c qwen_asr_decoder.c qwen_asr_tokenizer.c qwen_asr_safetensors.c)
list(TRANSFORM qwen_sources PREPEND "${QWEN_SOURCE}/")
add_library(qwen_cpu STATIC ${qwen_sources})
target_include_directories(qwen_cpu PUBLIC "${QWEN_SOURCE}" PRIVATE "${OPENBLAS_INCLUDE_DIR}")
target_compile_definitions(qwen_cpu PRIVATE USE_BLAS USE_OPENBLAS)
target_compile_options(qwen_cpu PRIVATE -O3 -march=native -ffast-math)
target_link_libraries(qwen_cpu PUBLIC "${OPENBLAS_LIBRARY}" Threads::Threads m)
add_executable(qwen-native-cli "${QWEN_SOURCE}/main.c")
target_link_libraries(qwen-native-cli PRIVATE qwen_cpu)
message(STATUS "Qwen CPU revision: ${QWEN_REVISION}")
message(STATUS "OpenBLAS: ${OPENBLAS_LIBRARY}; headers: ${OPENBLAS_INCLUDE_DIR}")
