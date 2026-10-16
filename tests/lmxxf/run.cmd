@echo off
rem lmxxf runtime and submission tests. Every test in tests\lmxxf belongs to exactly one tier below.
rem Usage: tests\lmxxf\run.cmd abi^|warp^|device^|inline^|inline-discard^|inline-gpu^|slots^|wait-profile^|split-state^|early-unity^|hip-passthrough^|gpu [out-dir]
rem   abi    : lmxxf_nr_abi, lmxxf_zero_fallback_abi.c, test_runtime_validation.py (no GPU)
rem   warp   : lmxxf_same_frame_boundary, lmxxf_color_probe (D3D12 WARP; no GPU)
rem   split-state: focused inherited-legacy/enhanced split admission and pixel readback (also in warp).
rem   device : lmxxf_list_split, lmxxf_list1_wrap, lmxxf_create_execute, lmxxf_evaluate_cut
rem            plus early Unity caller admission, retained split/Reset (hardware D3D12 adapter)
rem   early-unity: only the early caller/retained-list regression (also included in device).
rem   hip-passthrough: focused HIP round-trip/codec comparison (also included in gpu).
rem   slots  : private inline history/lifetime, split vs 1..5 hashes and timed GPU sweeps.
rem   wait-profile: 1440p old/new wait budgets, split baseline, hashes and paced/unpaced sweeps.
rem   gpu    : runtime formats/exposure/output hashes, recording lifecycle and bridge regressions.
rem            Bridge fixtures use /std:c++17 for upstream header compatibility.
rem            Needs AMD GPU and LMXXF_ASSETS = native-game-tiled-assets weights folder.
rem            Modules come from third_party\lmxxf\modules.
rem abi and gpu use LMXXF_TEST_RUNTIME when it names a built LmxxfNrRuntime.dll; otherwise they build
rem one into <out-dir>\runtime with tools\build\build-lmxxf-runtime.cmd.
setlocal EnableExtensions
cd /d "%~dp0..\.."
set "REPO=%CD%"
set "TIER=%~1"
set "OUT=%~2"
if not defined OUT set "OUT=exports\test-run\lmxxf"
for %%I in ("%OUT%") do set "OUT=%%~fI"
if not exist "%OUT%" mkdir "%OUT%"
if not defined AMD_TEST_PYTHON set "AMD_TEST_PYTHON=python"
call "%REPO%\tests\_lib\msvc-env.cmd" 2>nul || exit /b 1
rem Repo-rooted includes (#include "OptiScaler-..." / "third_party/..."). The repo root goes at the END of
rem INCLUDE, not in /I: /I is searched before the SDK/STL dirs, and on a case-insensitive disk
rem <version> would then resolve to the repo's VERSION file.
set "INCLUDE=%INCLUDE%;%REPO%"
set "RT_INC=%REPO%\OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\dlssnr\backend\lmxxf_runtime"
set "INC=%REPO%\OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\include"
set "DETOURS=%REPO%\OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\library\detours\detours.lib"
set "MODS=%REPO%\third_party\lmxxf\modules"
set "CXX=cl /nologo /std:c++20 /EHsc /W4 /utf-8"
set "D3D=d3d12.lib dxgi.lib dxguid.lib uuid.lib"
if /i "%TIER%"=="abi" goto abi
if /i "%TIER%"=="warp" goto warp
if /i "%TIER%"=="release" goto release
if /i "%TIER%"=="device" goto device
if /i "%TIER%"=="early-unity" goto early-unity
if /i "%TIER%"=="gpu" goto gpu
if /i "%TIER%"=="hip-passthrough" goto hip-passthrough
if /i "%TIER%"=="inline" goto inline
if /i "%TIER%"=="inline-discard" goto inline-discard
if /i "%TIER%"=="diag-lifetime" goto diag-lifetime
if /i "%TIER%"=="inline-gpu" goto inline-gpu
if /i "%TIER%"=="slots" goto slots
if /i "%TIER%"=="wait-profile" goto slots
if /i "%TIER%"=="split-state" goto split-state
echo usage: tests\lmxxf\run.cmd abi^|warp^|device^|inline^|inline-discard^|inline-gpu^|slots^|wait-profile^|split-state^|early-unity^|hip-passthrough^|gpu [out-dir]
exit /b 2

:abi
%CXX% tests\lmxxf\hip_runtime_load.cpp /Fe"%OUT%\hip_runtime_load.exe" /Fo"%OUT%\hip_runtime_load.obj" || goto fail
"%OUT%\hip_runtime_load.exe" || goto fail
call :Runtime || goto fail
cl /nologo /std:c++17 /EHsc /W4 /utf-8 tests\lmxxf\lmxxf_nr_abi.cpp /Fe"%OUT%\lmxxf_nr_abi.exe" /Fo"%OUT%\lmxxf_nr_abi.obj" || goto fail
"%OUT%\lmxxf_nr_abi.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" || goto fail
rem C host smoke test: export table, capabilities, create-flag accept/reject (no GPU).
cl /nologo /TC /W4 /I"%RT_INC%" tests\lmxxf\lmxxf_zero_fallback_abi.c /Fe"%OUT%\lmxxf_zero_fallback_abi.exe" /Fo"%OUT%\lmxxf_zero_fallback_abi.obj" || goto fail
rem It loads LmxxfNrRuntime.dll by bare name, i.e. from beside the exe: stage the DLL under test there.
copy /Y "%LMXXF_TEST_RUNTIME%" "%OUT%\LmxxfNrRuntime.dll" >nul || goto fail
"%OUT%\lmxxf_zero_fallback_abi.exe" || goto fail
"%AMD_TEST_PYTHON%" -B tests\lmxxf\test_runtime_validation.py || goto fail
goto pass

:release
call :RunSplitState || goto fail
call :RunInlineDiscard || goto fail
call :RunNativeTemporal || goto fail
goto warpSmoke

:warp
call :RunSplitState || goto fail
call :RunInlineFixture warp || goto fail
call :RunDiagLifetime || goto fail
%CXX% /I"%INC%" tests\lmxxf\lmxxf_wrapped_command_list.cpp /Fe"%OUT%\lmxxf_wrapped_command_list.exe" /Fo"%OUT%\lmxxf_wrapped_command_list.obj" /link %D3D% "%DETOURS%" || goto fail
"%OUT%\lmxxf_wrapped_command_list.exe" || goto fail
%CXX% tests\lmxxf\lmxxf_temporal_control.cpp /Fe"%OUT%\lmxxf_temporal_control.exe" /Fo"%OUT%\lmxxf_temporal_control.obj" /link %D3D% d3dcompiler.lib || goto fail
"%OUT%\lmxxf_temporal_control.exe" || goto fail
call :RunNativeTemporal || goto fail
%CXX% /I"%INC%" tests\lmxxf\lmxxf_legacy_root_capture.cpp /Fe"%OUT%\lmxxf_legacy_root_capture.exe" /Fo"%OUT%\lmxxf_legacy_root_capture.obj" /link %D3D% "%DETOURS%" || goto fail
"%OUT%\lmxxf_legacy_root_capture.exe" || goto fail
%CXX% tests\lmxxf\lmxxf_recording_timing.cpp /Fe"%OUT%\lmxxf_recording_timing.exe" /Fo"%OUT%\lmxxf_recording_timing.obj" /link %D3D% || goto fail
"%OUT%\lmxxf_recording_timing.exe" || goto fail
%CXX% /I"%REPO%\third_party\lmxxf\src" tests\lmxxf\lmxxf_exposure_recording.cpp /Fe"%OUT%\lmxxf_exposure_recording.exe" /Fo"%OUT%\lmxxf_exposure_recording.obj" /link %D3D% || goto fail
"%OUT%\lmxxf_exposure_recording.exe" || goto fail
call tests\lmxxf\shader-compiler.cmd "%OUT%\shader-compiler" || goto fail
%CXX% /I"%INC%" tests\lmxxf\lmxxf_recording_lifecycle.cpp /Fe"%OUT%\lmxxf_recording_lifecycle.exe" /Fo"%OUT%\lmxxf_recording_lifecycle.obj" /link %D3D% "%DETOURS%" || goto fail
"%OUT%\lmxxf_recording_lifecycle.exe" || goto fail
:warpSmoke
%CXX% /I"%INC%" tests\lmxxf\lmxxf_same_frame_boundary.cpp /Fe"%OUT%\lmxxf_same_frame_boundary.exe" /Fo"%OUT%\lmxxf_same_frame_boundary.obj" /link %D3D% d3dcompiler.lib "%DETOURS%" || goto fail
"%OUT%\lmxxf_same_frame_boundary.exe" || goto fail
%CXX% tests\lmxxf\lmxxf_color_probe.cpp /Fe"%OUT%\lmxxf_color_probe.exe" /Fo"%OUT%\lmxxf_color_probe.obj" /link d3d12.lib dxgi.lib dxguid.lib || goto fail
"%OUT%\lmxxf_color_probe.exe" || goto fail
goto pass

:device
call :RunInlineFixture hardware || goto fail
%CXX% tests\lmxxf\lmxxf_list_split.cpp /Fe"%OUT%\lmxxf_list_split.exe" /Fo"%OUT%\lmxxf_list_split.obj" /link %D3D% || goto fail
"%OUT%\lmxxf_list_split.exe" || goto fail
for %%T in (lmxxf_list1_wrap lmxxf_create_execute lmxxf_evaluate_cut) do (
  %CXX% /I"%INC%" tests\lmxxf\%%T.cpp /Fe"%OUT%\%%T.exe" /Fo"%OUT%\%%T.obj" /link %D3D% "%DETOURS%" || goto fail
  "%OUT%\%%T.exe" || goto fail
)
call :EarlyUnity || goto fail
goto pass

:early-unity
call :EarlyUnity || goto fail
goto pass

:inline
call :RunInlineFixture hardware || goto fail
goto pass

:split-state
call :RunSplitState || goto fail
goto pass

:RunSplitState
%CXX% /I"%INC%" tests\lmxxf\nr_inline_recording.cpp /Fe"%OUT%\nr_split_state.exe" /Fo"%OUT%\nr_split_state.obj" /link %D3D% d3dcompiler.lib || exit /b 1
"%OUT%\nr_split_state.exe" --split-state
exit /b %errorlevel%

:slots
if not defined LMXXF_ASSETS goto fail
set "LMXXF_WEIGHTS_DIR=%LMXXF_ASSETS%"
call :Runtime || goto fail
%CXX% /O2 /I"%INC%" tests\lmxxf\nr_slots_benchmark.cpp /Fe"%OUT%\nr_slots_benchmark.exe" /Fo"%OUT%\nr_slots_benchmark.obj" /link %D3D% d3dcompiler.lib "%DETOURS%" || goto fail
if /i "%TIER%"=="wait-profile" (
  %AMD_TEST_PYTHON% -B tests\lmxxf\compare_wait_profiles.py "%OUT%\nr_slots_benchmark.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" "%OUT%" || goto fail
  goto pass
)
%CXX% tests\lmxxf\lmxxf_native_history_gpu.cpp /Fe"%OUT%\lmxxf_native_history_gpu.exe" /Fo"%OUT%\lmxxf_native_history_gpu.obj" /link %D3D% d3dcompiler.lib || goto fail
"%OUT%\lmxxf_native_history_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" --inline || goto fail
%AMD_TEST_PYTHON% -B tests\lmxxf\compare_slots.py "%OUT%\nr_slots_benchmark.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" "%OUT%" || goto fail
goto pass

:inline-gpu
if not defined LMXXF_ASSETS goto fail
set "LMXXF_WEIGHTS_DIR=%LMXXF_ASSETS%"
call :Runtime || goto fail
%CXX% /I"%INC%" tests\lmxxf\nr_inline_runtime_gpu.cpp /Fe"%OUT%\nr_inline_runtime_gpu.exe" /Fo"%OUT%\nr_inline_runtime_gpu.obj" /link %D3D% d3dcompiler.lib "%DETOURS%" || goto fail
"%OUT%\nr_inline_runtime_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" || goto fail
"%OUT%\nr_inline_runtime_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" --timeout || goto fail
"%OUT%\nr_inline_runtime_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" --ahead || goto fail
"%OUT%\nr_inline_runtime_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" --timeout --ahead || goto fail
goto pass

:RunInlineFixture
call :RunInlineDiscard || exit /b 1
%CXX% /I"%INC%" tests\lmxxf\nr_inline_recording.cpp /Fe"%OUT%\nr_inline_recording.exe" /Fo"%OUT%\nr_inline_recording.obj" /link %D3D% d3dcompiler.lib || exit /b 1
if /i "%~1"=="hardware" ("%OUT%\nr_inline_recording.exe" --hardware) else ("%OUT%\nr_inline_recording.exe")
exit /b %errorlevel%

:inline-discard
call :RunInlineDiscard || goto fail
goto pass

:diag-lifetime
call :RunDiagLifetime || goto fail
goto pass

:RunDiagLifetime
%CXX% /I"%INC%" tests\lmxxf\nr_diag_lifetime.cpp /Fe"%OUT%\nr_diag_lifetime.exe" /Fo"%OUT%\nr_diag_lifetime.obj" /link %D3D% d3dcompiler.lib "%DETOURS%" || exit /b 1
"%OUT%\nr_diag_lifetime.exe"
exit /b %errorlevel%

:RunInlineDiscard
%CXX% /I"%INC%" tests\lmxxf\nr_inline_discard.cpp /Fe"%OUT%\nr_inline_discard.exe" /Fo"%OUT%\nr_inline_discard.obj" /link %D3D% d3dcompiler.lib "%DETOURS%" || exit /b 1
"%OUT%\nr_inline_discard.exe"
exit /b %errorlevel%

:EarlyUnity
for %%N in (UnityPlayer OtherEngine) do (
  %CXX% /O2 /LD tests\lmxxf\early_caller_fixture.cpp /Fe"%OUT%\%%N.dll" /Fo"%OUT%\%%N.obj" /link /IMPLIB:"%OUT%\%%N.lib" %D3D% || exit /b 1
)
%CXX% /O2 /I"%INC%" tests\lmxxf\lmxxf_early_unity.cpp /Fe"%OUT%\lmxxf_early_unity.exe" /Fo"%OUT%\lmxxf_early_unity.obj" /link %D3D% "%DETOURS%" || exit /b 1
"%OUT%\lmxxf_early_unity.exe" "%OUT%\UnityPlayer.dll" "%OUT%\OtherEngine.dll"
exit /b %errorlevel%

:gpu
rem Run the native pre/post shaders at ultrawide/4K without allocating a full network.
%CXX% tests\lmxxf\lmxxf_native_temporal.cpp /Fe"%OUT%\lmxxf_native_temporal.exe" /Fo"%OUT%\lmxxf_native_temporal.obj" /link %D3D% d3dcompiler.lib || goto fail
"%OUT%\lmxxf_native_temporal.exe" --hardware || goto fail
rem Explicit 0.39 compatibility configuration; new product defaults are exercised below.
set "DLSS5_SKIP_BLOCKS=42,43,46"
set "DLSS5_FAST_NUMERIC=0"
set "DLSS5_NETWORK_FREE_RES=0"
set "DLSS5_MULTI_PASS=1"
set "DLSS5_MULTI_PASS_PREDICT=1"
set "DLSS5_MULTI_PASS_SKIN_PROTECT=0"
set "DLSS5_MULTI_PASS_SKIP_BLOCKS=none"
cl /nologo /std:c++17 /EHsc /W4 /utf-8 tests\lmxxf\lmxxf_module_load_gpu.cpp /Fe"%OUT%\lmxxf_module_load_gpu.exe" /Fo"%OUT%\lmxxf_module_load_gpu.obj" || goto fail
"%OUT%\lmxxf_module_load_gpu.exe" tests\lmxxf\lmxxf_module_load_gpu.cpp || goto fail
if not defined LMXXF_ASSETS (
  echo FAIL: set LMXXF_ASSETS to the lmxxf weights folder ^(native-game-tiled-assets^).
  goto fail
)
if not exist "%LMXXF_ASSETS%\block0-ffn.f16" if not exist "%LMXXF_ASSETS%\block0-ffn.f32" (
  echo FAIL: LMXXF_ASSETS="%LMXXF_ASSETS%" has no block0-ffn weights.
  goto fail
)
set "LMXXF_WEIGHTS_DIR=%LMXXF_ASSETS%"
call :Runtime || goto fail
%CXX% tests\lmxxf\lmxxf_native_history_gpu.cpp /Fe"%OUT%\lmxxf_native_history_gpu.exe" /Fo"%OUT%\lmxxf_native_history_gpu.obj" /link %D3D% d3dcompiler.lib || goto fail
"%OUT%\lmxxf_native_history_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" || goto fail
"%OUT%\lmxxf_native_history_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" --history-excludes-adaptive || goto fail
rem Optional fast 900-tier module must support the product auxiliary History output.
"%OUT%\lmxxf_native_history_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" --history-900 || goto fail
%CXX% /I"%REPO%\third_party\lmxxf\Development\HIP" /I"%REPO%\third_party\lmxxf\src" tests\lmxxf\lmxxf_multipass_aux_gpu.cpp /Fe"%OUT%\lmxxf_multipass_aux_gpu.exe" /Fo"%OUT%\lmxxf_multipass_aux_gpu.obj" /link user32.lib || goto fail
"%OUT%\lmxxf_multipass_aux_gpu.exe" "%LMXXF_ASSETS%" "%MODS%" || goto fail
rem The runtime resolves modules from its assets argument and weights from LMXXF_WEIGHTS_DIR.
rem Passing the weights folder itself would make it prefer the (possibly stale) HIP\ copy beside
rem the weights over this checkout's modules.
set "LMXXF_WEIGHTS_DIR=%LMXXF_ASSETS%"
%CXX% /I"%RT_INC%" tests\lmxxf\lmxxf_nr_gpu.cpp /Fe"%OUT%\lmxxf_nr_gpu.exe" /Fo"%OUT%\lmxxf_nr_gpu.obj" /link d3d12.lib dxgi.lib || goto fail
for %%M in ("" "--resize" "--queue-mismatch" "--rgb9e5" "--reject-formats" "--exposure" "--exposure-bad" "--ultrawide" "--subrect" "--rgba32" "--upstream-controls") do (
  echo --- lmxxf_nr_gpu %%~M
  "%OUT%\lmxxf_nr_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" %%~M || goto fail
)
%CXX% /I"%RT_INC%" /I"OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\include" tests\lmxxf\lmxxf_recording_runtime_gpu.cpp /Fe"%OUT%\lmxxf_recording_runtime_gpu.exe" /Fo"%OUT%\lmxxf_recording_runtime_gpu.obj" /link d3d12.lib dxgi.lib dxguid.lib "OptiScaler-DLSSNR-PreSR-Multipass-main\OptiScaler\library\detours\detours.lib" || goto fail
"%OUT%\lmxxf_recording_runtime_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" || goto fail
rem Output-hash baselines: fixed pattern, 1920x1080, seed 1 (RX 9070 XT, this weights set).
rem Re-baselined 2026-10-16 for 3a1be68c 0.4.2 W16/inter-byte/compact-QKV stack (see docs/lmxxf-042-consumer-review.md).
call :Hash f05d0177e0a82a9a --output-hash || goto fail
call :Hash b849c8ff3d30e462 --auto-exposure || goto fail
call :Hash b849c8ff3d30e462 --auto-exposure --scale16 || goto fail
call :Hash 10d1507268da9a75 --r10g10b10a2 || goto fail
cl /nologo /std:c++17 /EHsc /W4 /utf-8 /DNOMINMAX /D_WIN32_WINNT=0x0A00 /I"%REPO%\third_party\lmxxf\Development\HIP" /I"%REPO%\third_party\lmxxf\src" tests\lmxxf\lmxxf_bridge_recording_gpu.cpp /Fe"%OUT%\lmxxf_bridge_recording_gpu.exe" /Fo"%OUT%\lmxxf_bridge_recording_gpu.obj" /link d3d12.lib dxgi.lib user32.lib || goto fail
"%OUT%\lmxxf_bridge_recording_gpu.exe" "%LMXXF_ASSETS%" "%MODS%" || goto fail
"%OUT%\lmxxf_bridge_recording_gpu.exe" "%LMXXF_ASSETS%" "%MODS%" --adaptive-reset || goto fail
"%OUT%\lmxxf_bridge_recording_gpu.exe" "%LMXXF_ASSETS%" "%MODS%" --adaptive-reset-1080 || goto fail
cl /nologo /std:c++17 /EHsc /W4 /utf-8 /DNOMINMAX /D_WIN32_WINNT=0x0A00 tests\lmxxf\lmxxf_bridge_zero_gpu.cpp /Fe"%OUT%\lmxxf_bridge_zero_gpu.exe" /Fo"%OUT%\lmxxf_bridge_zero_gpu.obj" /link d3d12.lib dxgi.lib || goto fail
"%OUT%\lmxxf_bridge_zero_gpu.exe" "%LMXXF_ASSETS%" "%MODS%" || goto fail
"%OUT%\lmxxf_bridge_zero_gpu.exe" "%LMXXF_ASSETS%" "%MODS%" --probe-drain || goto fail
rem 0.40 product defaults and live network rebuild/output checks.
set "DLSS5_SKIP_BLOCKS=none"
set "DLSS5_FAST_NUMERIC=1"
set "DLSS5_NETWORK_FREE_RES=1"
rem 1:1 active-size input must not enter bilinear fitting (PR12 interface regression).
call :Hash 806dd30da2c516da --size 1920 1080 || goto fail
"%AMD_TEST_PYTHON%" -B tests\lmxxf\test_shader_precedence.py "%OUT%\lmxxf_recording_runtime_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" "%OUT%" || goto fail
"%OUT%\lmxxf_nr_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" --040-controls || goto fail
"%OUT%\lmxxf_nr_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" --041-controls || goto fail
rem Active input smaller than its allocation must work in free-resolution mode too.
"%OUT%\lmxxf_nr_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" --subrect || goto fail
"%OUT%\lmxxf_nr_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" --size 3840 2160 --active-size 2259 1271 || goto fail
for %%S in ("1280 720" "1707 961" "2560 1440" "3440 1440") do (
  "%OUT%\lmxxf_nr_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" --size %%~S || goto fail
)
set "DLSS5_MULTI_PASS=2"
"%OUT%\lmxxf_recording_runtime_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" || goto fail
rem Cold three-pass prediction + skin must prepare allocations before the producer wait.
set "DLSS5_MULTI_PASS=3"
set "DLSS5_MULTI_PASS_SKIN_PROTECT=1"
"%OUT%\lmxxf_recording_runtime_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" || goto fail
goto pass

:hip-passthrough
rem Focused new-diagnostic test; also run in gpu. No module or network-default changes.
if not defined LMXXF_ASSETS goto fail
set "LMXXF_WEIGHTS_DIR=%LMXXF_ASSETS%"
set "DLSS5_SKIP_BLOCKS=none"
set "DLSS5_FAST_NUMERIC=1"
set "DLSS5_NETWORK_FREE_RES=1"
set "DLSS5_MULTI_PASS=1"
call :Runtime || goto fail
%CXX% /I"%RT_INC%" /I"%INC%" tests\lmxxf\lmxxf_recording_runtime_gpu.cpp /Fe"%OUT%\lmxxf_recording_runtime_gpu.exe" /Fo"%OUT%\lmxxf_recording_runtime_gpu.obj" /link %D3D% "%DETOURS%" || goto fail
"%AMD_TEST_PYTHON%" -B tests\lmxxf\test_shader_precedence.py "%OUT%\lmxxf_recording_runtime_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%" "%OUT%" || goto fail
goto pass

:pass
echo lmxxf %TIER%: PASS
exit /b 0

:fail
echo lmxxf %TIER%: FAIL
exit /b 1

:Runtime
if defined LMXXF_TEST_RUNTIME if exist "%LMXXF_TEST_RUNTIME%" exit /b 0
call "%REPO%\tools\build\build-lmxxf-runtime.cmd" "%OUT%\runtime" || exit /b 1
set "LMXXF_TEST_RUNTIME=%OUT%\runtime\LmxxfNrRuntime.dll"
exit /b 0

:Hash
set "EXPECT=%~1"
shift
set "MODE="
:HashArgs
if "%~1"=="" goto HashRun
set "MODE=%MODE% %~1"
shift
goto HashArgs
:HashRun
echo --- lmxxf_nr_gpu%MODE% (expect output_hash=%EXPECT%)
"%OUT%\lmxxf_nr_gpu.exe" "%LMXXF_TEST_RUNTIME%" "%MODS%"%MODE% > "%OUT%\nr_gpu_hash.txt" 2>&1
set "RC=%errorlevel%"
type "%OUT%\nr_gpu_hash.txt"
if not "%RC%"=="0" exit /b 1
findstr /c:"output_hash=%EXPECT% " "%OUT%\nr_gpu_hash.txt" >nul || (
  echo FAIL: output hash differs from baseline %EXPECT%
  exit /b 1
)
exit /b 0

:RunNativeTemporal
%CXX% tests\lmxxf\lmxxf_native_temporal.cpp /Fe"%OUT%\lmxxf_native_temporal.exe" /Fo"%OUT%\lmxxf_native_temporal.obj" /link %D3D% d3dcompiler.lib || exit /b 1
"%OUT%\lmxxf_native_temporal.exe"
exit /b %errorlevel%
