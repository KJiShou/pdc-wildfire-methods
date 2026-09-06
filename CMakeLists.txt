cmake_minimum_required(VERSION 3.21)
project(parallel_qoi LANGUAGES CXX)

option(PQOI_ENABLE_OPENMP "Build the OpenMP backend" ON)
option(PQOI_ENABLE_CUDA "Build the CUDA backend" OFF)
option(PQOI_ENABLE_MPI "Build the MPI backend" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(pqoi_core
    src/core/image_io.cpp
    src/core/stb_image_impl.cpp
    src/core/qoi_codec.cpp
    src/core/qoi_compat.cpp
    src/core/encoder.cpp
    src/core/block_partition.cpp
    src/core/block_summary.cpp
    src/core/validation.cpp
    src/core/metrics.cpp
    src/core/cli.cpp
    src/backends/serial_encoder.cpp
    src/backends/openmp_encoder.cpp
)
target_include_directories(pqoi_core PUBLIC include third_party)
target_compile_features(pqoi_core PUBLIC cxx_std_17)

if (WIN32)
    target_link_libraries(pqoi_core PRIVATE windowscodecs ole32)
endif()

if (MSVC)
    target_compile_options(pqoi_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4> $<$<COMPILE_LANGUAGE:CXX>:/permissive->)
else()
    target_compile_options(pqoi_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall> $<$<COMPILE_LANGUAGE:CXX>:-Wextra> $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>)
endif()

function(add_pqoi_cli target source backend)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE pqoi_core)
    target_compile_definitions(${target} PRIVATE PQOI_BACKEND="${backend}")
endfunction()

add_pqoi_cli(pqoi_serial src/cli/serial_main.cpp serial)
add_pqoi_cli(pqoi_control src/cli/control_main.cpp one-pass)
add_executable(pqoi_decode_preview src/cli/decode_preview_main.cpp)
target_link_libraries(pqoi_decode_preview PRIVATE pqoi_core)

if (PQOI_ENABLE_OPENMP)
    find_package(OpenMP QUIET)
    if (OpenMP_CXX_FOUND)
        add_pqoi_cli(pqoi_openmp src/cli/openmp_main.cpp openmp)
        target_link_libraries(pqoi_core PUBLIC OpenMP::OpenMP_CXX)
        target_link_libraries(pqoi_openmp PRIVATE OpenMP::OpenMP_CXX)
        target_compile_definitions(pqoi_core PUBLIC PQOI_HAS_OPENMP=1)
    else()
        message(WARNING "OpenMP requested but no OpenMP compiler was found")
    endif()
endif()

if (PQOI_ENABLE_CUDA)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    target_sources(pqoi_core PRIVATE src/backends/cuda_encoder.cu)
    target_link_libraries(pqoi_core PUBLIC CUDA::cudart)
    target_compile_definitions(pqoi_core PUBLIC PQOI_HAS_CUDA=1)
    if (MSVC)
        target_compile_options(pqoi_core PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/Zc:preprocessor>)
    endif()
    add_pqoi_cli(pqoi_cuda src/cli/cuda_main.cpp cuda)
    set_target_properties(pqoi_core PROPERTIES CUDA_STANDARD 17 CUDA_SEPARABLE_COMPILATION ON CUDA_RESOLVE_DEVICE_SYMBOLS ON)
    foreach(cuda_consumer pqoi_serial pqoi_control pqoi_openmp pqoi_cuda pqoi_mpi pqoi_core_tests)
        if (TARGET ${cuda_consumer})
            target_link_libraries(${cuda_consumer} PRIVATE CUDA::cudart)
            set_target_properties(${cuda_consumer} PROPERTIES CUDA_RESOLVE_DEVICE_SYMBOLS ON)
        endif()
    endforeach()
endif()

if (PQOI_ENABLE_MPI)
    find_package(MPI REQUIRED COMPONENTS CXX)
    add_pqoi_cli(pqoi_mpi src/cli/mpi_main.cpp mpi)
    target_sources(pqoi_mpi PRIVATE src/backends/mpi_encoder.cpp)
    target_link_libraries(pqoi_mpi PRIVATE MPI::MPI_CXX)
endif()

include(CTest)
if (BUILD_TESTING)
    add_executable(pqoi_core_tests tests/unit/core_tests.cpp)
    target_link_libraries(pqoi_core_tests PRIVATE pqoi_core)
    add_test(NAME pqoi_core_tests COMMAND pqoi_core_tests)
    if (PQOI_ENABLE_MPI AND WIN32)
        add_test(
            NAME pqoi_mpi_smoke
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/mpi_smoke.ps1"
                    -MpiExe "$<TARGET_FILE:pqoi_mpi>"
                    -SerialExe "$<TARGET_FILE:pqoi_serial>"
                    -Mpiexec "${MPIEXEC_EXECUTABLE}"
                    -WorkingDirectory "${CMAKE_CURRENT_BINARY_DIR}/mpi-smoke")
        add_test(
            NAME pqoi_mpi_server_smoke
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/mpi_server_smoke.ps1"
                    -MpiExe "$<TARGET_FILE:pqoi_mpi>"
                    -SerialExe "$<TARGET_FILE:pqoi_serial>"
                    -Mpiexec "${MPIEXEC_EXECUTABLE}"
                    -WorkingDirectory "${CMAKE_CURRENT_BINARY_DIR}/mpi-server-smoke")
    endif()
endif()
