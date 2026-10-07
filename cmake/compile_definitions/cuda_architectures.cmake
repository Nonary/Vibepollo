# Select CUDA targets and enforce the Linux release compatibility policy.
message(STATUS "CUDA Compiler Version: ${CMAKE_CUDA_COMPILER_VERSION}")
set(CMAKE_CUDA_ARCHITECTURES "")

# https://docs.nvidia.com/cuda/archive/12.0.0/cuda-compiler-driver-nvcc/index.html
if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.0)
    list(APPEND CMAKE_CUDA_ARCHITECTURES 75 80 86 87 89 90)
else()
    message(FATAL_ERROR
            "Sunshine requires a minimum CUDA Compiler version of 12.0.
            Found version: ${CMAKE_CUDA_COMPILER_VERSION}"
    )
endif()

# https://docs.nvidia.com/cuda/archive/12.8.0/cuda-compiler-driver-nvcc/index.html
if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.8)
    list(APPEND CMAKE_CUDA_ARCHITECTURES 100 101 120)
endif()

# https://docs.nvidia.com/cuda/archive/12.9.0/cuda-compiler-driver-nvcc/index.html
if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.9)
    list(APPEND CMAKE_CUDA_ARCHITECTURES 103 121)
endif()

# https://docs.nvidia.com/cuda/archive/13.0.0/cuda-compiler-driver-nvcc/index.html
if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 13.0)
    list(REMOVE_ITEM CMAKE_CUDA_ARCHITECTURES 101)
    list(APPEND CMAKE_CUDA_ARCHITECTURES 110)
else()
    list(APPEND CMAKE_CUDA_ARCHITECTURES 50 52 53 60 61 62 70 72)
endif()

if(SUNSHINE_REQUIRE_CUDA_PASCAL AND
        (NOT CMAKE_CUDA_COMPILER_VERSION VERSION_LESS 13.0 OR
         CMAKE_CUDA_COMPILER_VERSION VERSION_LESS 12.9 OR
         NOT "61" IN_LIST CMAKE_CUDA_ARCHITECTURES))
    message(FATAL_ERROR
            "Release packages require CUDA 12.9 with Pascal (sm_61) support. "
            "Select the pinned CUDA 12.9 toolkit and GCC 14 CUDA host compiler.")
endif()

# sort the architectures
list(SORT CMAKE_CUDA_ARCHITECTURES COMPARE NATURAL)

# message(STATUS "CUDA NVCC Flags: ${CUDA_NVCC_FLAGS}")
message(STATUS "CUDA Architectures: ${CMAKE_CUDA_ARCHITECTURES}")
