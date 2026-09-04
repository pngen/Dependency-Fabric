@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9\bin\nvcc.exe" -std=c++20 -Iinclude -Xcompiler=/EHsc,/MD -arch=sm_120 cuda\df_cuda_proof.cu build\Release\dependency_fabric.lib -o build\df_cuda_proof.exe
echo NVCC_EXIT=%ERRORLEVEL%
