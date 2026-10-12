@echo off
setlocal
cd /d "%~dp0..\.."
if /i "%~1"=="abi" (
  python -B tests\mochizuki\test_build_cache.py
  if errorlevel 1 exit /b 1
  python -B tests\mochizuki\test_source_pin.py
  if errorlevel 1 exit /b 1
  python -B tests\mochizuki\test_aco_temporal.py
  if errorlevel 1 exit /b 1
)
if /i "%~1"=="source-pin" (
  python -B tests\mochizuki\test_source_pin.py || exit /b 1
  exit /b 0
)
call tests\_lib\msvc-env.cmd || exit /b 1
if not exist exports\mochizuki-tests mkdir exports\mochizuki-tests
if /i "%~1"=="abi" call :acounit || exit /b 1
if /i "%~1"=="aco-unit" goto acounit
if /i "%~1"=="inline" goto inline
if /i "%~1"=="aco-inline" goto inline
if /i "%~1"=="aco-temporal" goto acotemporal
if /i "%~1"=="hdr-shader" goto hdrshader
if /i "%~1"=="phase-failure" goto phasefailure
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler tests/mochizuki/runtime.cpp /Foexports/mochizuki-tests/runtime.obj /Feexports/mochizuki-tests/runtime.exe /link d3d12.lib dxgi.lib || exit /b 1
if /i "%~1"=="destroy-tail" goto destroytail
if /i "%~1"=="startup" goto startup
if /i "%~1"=="profile" goto profile
if /i "%~1"=="composition" goto composition
if /i "%~1"=="first-pass" goto firstpass
if /i "%~1"=="aco" goto aco
if /i "%~1"=="aco-rapid" goto acorapid
if /i "%~1"=="shader-tail" goto shadertail
if /i "%~1"=="pass-switch" goto passswitch
if /i "%~1"=="split-retain" goto splitretain
if /i "%~1"=="gpu" (
  call tests\mochizuki\run.cmd hdr-shader || exit /b 1
  call tests\mochizuki\run.cmd phase-failure || exit /b 1
  call tests\mochizuki\run.cmd inline || exit /b 1
  exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --composition
  if errorlevel 1 exit /b 1
  exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime"
) else (
  exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll"
)
if errorlevel 1 exit /b 1
python -X utf8 -c "from pathlib import Path; import hashlib; p=Path('exports/mochizuki-runtime'); (p/'abi-ci.sha256').write_text(hashlib.sha256((p/'MochizukiNrRuntime.dll').read_bytes()).hexdigest()+'\n')"
exit /b %errorlevel%

:splitretain
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --split-retain
exit /b %errorlevel%

:passswitch
set "MZ_TEST_UPGRADE_REFUSALS="
set "MZ_TEST_UPGRADE_OOM_ONCE="
set "MZ_TEST_FRAME_BUDGET_ONCE="
set "MOCHI_PASS_MODE=normal"
if not "%~2"=="" set "MOCHI_PASS_MODE=%~2"
if /i "%~2"=="budget" set "MZ_TEST_UPGRADE_REFUSALS=1"
if /i "%~2"=="oom" set "MZ_TEST_UPGRADE_OOM_ONCE=1"
if /i "%~2"=="frame-budget" set "MZ_TEST_FRAME_BUDGET_ONCE=1"
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --pass-switch "%MOCHI_PASS_MODE%"
exit /b %errorlevel%

:phasefailure
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler tests/mochizuki/queue_segments.cpp /Foexports/mochizuki-tests/queue_segments.obj /Feexports/mochizuki-tests/queue_segments.exe /link d3d12.lib dxgi.lib dxguid.lib || exit /b 1
for %%P in (2 3) do (
  set "MZ_TEST_PHASE_SUBMIT_FAIL_AT=%%P"
  exports\mochizuki-tests\queue_segments.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" || exit /b 1
)
rem R8G8B8A8_UNORM: the whole chain is one compute phase; its submit fails.
set "MZ_TEST_PHASE_SUBMIT_FAIL_AT=1"
set "MZ_TEST_INLINE_CHAIN=1"
exports\mochizuki-tests\queue_segments.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" unorm8 || exit /b 1
set "MZ_TEST_PHASE_SUBMIT_FAIL_AT="
set "MZ_TEST_INLINE_CHAIN="
exit /b 0

:inline
set "MOCHI_ACO_ARG="
if /i "%~1"=="aco-inline" set "MOCHI_ACO_ARG=--aco"
set "INCLUDE=%INCLUDE%;%CD%"
set "MOCHI_INLINE_RUNTIME=%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll"
if not "%~2"=="" set "MOCHI_INLINE_RUNTIME=%~f2"
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/include tests/lmxxf/nr_inline_runtime_gpu.cpp /Foexports/mochizuki-tests/nr_inline_runtime_gpu.obj /Feexports/mochizuki-tests/nr_inline_runtime_gpu.exe /link d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/library/detours/detours.lib || exit /b 1
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki %MOCHI_ACO_ARG%
if errorlevel 1 exit /b 1
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --timeout %MOCHI_ACO_ARG%
if errorlevel 1 exit /b 1
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --ahead %MOCHI_ACO_ARG%
if errorlevel 1 exit /b 1
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --timeout --ahead %MOCHI_ACO_ARG%
if errorlevel 1 exit /b 1
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --large --temporal %MOCHI_ACO_ARG%
if errorlevel 1 exit /b 1
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --large --temporal --three-passes %MOCHI_ACO_ARG%
if errorlevel 1 exit /b 1
rem R16G16B16A16_FLOAT above needs the colour conversions: segments. R8G8B8A8_UNORM runs whole on compute.
set "MZ_TEST_INLINE_CHAIN=2"
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki %MOCHI_ACO_ARG%
if errorlevel 1 exit /b 1
set "MZ_TEST_INLINE_CHAIN=1"
for %%M in ("" "--timeout" "--ahead" "--timeout --ahead" "--large --temporal") do (
  exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --unorm8 %%~M %MOCHI_ACO_ARG%
  if errorlevel 1 (set "MZ_TEST_INLINE_CHAIN=" & exit /b 1)
)
rem Three passes leave the direct colour input: the UNORM8 frame falls back to segments.
set "MZ_TEST_INLINE_CHAIN=2"
exports\mochizuki-tests\nr_inline_runtime_gpu.exe "%MOCHI_INLINE_RUNTIME%" "%CD%\exports\mochizuki-runtime" --mochizuki --unorm8 --large --temporal --three-passes %MOCHI_ACO_ARG%
set "RC=%errorlevel%"
set "MZ_TEST_INLINE_CHAIN="
exit /b %RC%

:startup
set "ASSETS=%CD%\exports\mochizuki-runtime"
if not "%~2"=="" set "ASSETS=%~f2"
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%ASSETS%" --startup "%~3" %4 %5
exit /b %errorlevel%

:shadertail
rem The ViT 64-token key chunk is partial at these render resolutions.
for %%S in (1280x720 1129x635 640x360) do (
  for /f "tokens=1,2 delims=x" %%W in ("%%S") do (
    exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --startup "%CD%\exports\mochizuki-tests\tail-%%S.bin" %%W %%X
    if errorlevel 1 exit /b 1
  )
)
exit /b 0

:destroytail
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --destroy-tail
exit /b %errorlevel%

:hdrshader
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I exports/mochizuki-toolchain/Vulkan-Headers-e3b1eec08173d6b825cd3ac88c885a63b621504a/include /I third_party/mochizuki/windows/src/core tests/mochizuki/hdr_shader.cpp /Foexports/mochizuki-tests/hdr_shader.obj /Feexports/mochizuki-tests/hdr_shader.exe /link OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/library/vulkan/vulkan-1.lib || exit /b 1
set "MOCHI_HDR_SHADERS=%CD%\exports\mochizuki-runtime\dlssnr-amd\shaders\runtime"
if not "%~2"=="" set "MOCHI_HDR_SHADERS=%~f2"
exports\mochizuki-tests\hdr_shader.exe "%MOCHI_HDR_SHADERS%"
exit /b %errorlevel%

:profile
rem profile output.raw width height scale passes preprocess compact enlarge (16 warmup + 64 samples)
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --profile "%~2" %3 %4 %5 %6 %7 %8 %9
exit /b %errorlevel%

:firstpass
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --first-pass
exit /b %errorlevel%

:aco
call :acounit || exit /b 1
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --aco
if errorlevel 1 exit /b 1
set "MZ_TEST_REJECT_ACO_DEVICE=1"
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --aco-device
if errorlevel 1 exit /b 1
set "MZ_TEST_REJECT_ACO_DEVICE="
set "MZ_TEST_FRAME_BUDGET_AT=2"
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --aco-budget
if errorlevel 1 exit /b 1
set "MZ_TEST_FRAME_BUDGET_AT="
python -X utf8 -B tests\mochizuki\aco_fallback.py
exit /b %errorlevel%

:acorapid
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --aco-rapid
exit /b %errorlevel%

:acounit
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I exports/mochizuki-toolchain/Vulkan-Headers-e3b1eec08173d6b825cd3ac88c885a63b621504a/include /I third_party/mochizuki/windows/src/core tests/mochizuki/aco.cpp /Foexports/mochizuki-tests/aco.obj /Feexports/mochizuki-tests/aco.exe || exit /b 1
exports\mochizuki-tests\aco.exe third_party\mochizuki\windows\data\aco\records exports\mochizuki-tests\aco-test.nrb || exit /b 1
python -X utf8 tools\build\mochizuki-aco.py --verify --out exports\mochizuki-runtime\dlssnr-amd\aco || exit /b 1
python -X utf8 tools\install\mochizuki-owned.py --check || exit /b 1
exit /b 0

:composition
exports\mochizuki-tests\runtime.exe "%CD%\exports\mochizuki-runtime\MochizukiNrRuntime.dll" "%CD%\exports\mochizuki-runtime" --composition
exit /b %errorlevel%

:acotemporal
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /I OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler tests/mochizuki/aco_temporal.cpp /Foexports/mochizuki-tests/aco_temporal.obj /Feexports/mochizuki-tests/aco_temporal.exe /link d3d12.lib dxgi.lib || exit /b 1
python -X utf8 -B tests/mochizuki/aco_temporal.py %2 %3 %4 %5
exit /b %errorlevel%
