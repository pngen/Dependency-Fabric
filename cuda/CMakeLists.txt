# CUDA dependency-gated execution proof (RTX 5090 / sm_120).
if(NOT CMAKE_CUDA_COMPILER)
  enable_language(CUDA)
endif()
set(CMAKE_CUDA_ARCHITECTURES 120)
find_package(CUDAToolkit REQUIRED)
add_executable(df_cuda_proof df_cuda_proof.cu)
target_link_libraries(df_cuda_proof PRIVATE dependency_fabric CUDA::cudart)
# CUDA host code compiles via nvcc + MSVC; keep strict host flags but not /WX
# (nvcc host warnings are not project warnings).
if(MSVC)
  target_compile_options(df_cuda_proof PRIVATE "$<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/W4>")
else()
  target_compile_options(df_cuda_proof PRIVATE "$<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-Wall;-Xcompiler=-Wextra;...>")
endif()
