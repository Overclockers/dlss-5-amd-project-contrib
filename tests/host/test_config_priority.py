"""Contract tests: Ins/ini win over native-game-flags.txt; key names stay stable."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
KEYS = (ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/ConfigKeys.h").read_text(encoding="utf-8")
CONFIG = (ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/Config.cpp").read_text(encoding="utf-8")
RUNTIME = (
    ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrRuntime.cpp"
).read_text(encoding="utf-8")
MENU = (ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/DlssNr_Menu.cpp").read_text(encoding="utf-8")


class ConfigPriorityTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt" and shutil.which("cl"), "requires MSVC")
    def test_mochizuki_reset_preserves_standalone_aco(self):
        product = ROOT / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler'
        header = (product / 'Config.h').read_text(encoding='utf-8')
        options = (product / 'dlssnr/backend/MochizukiOptions.inc').read_text(encoding='utf-8')
        fields = []
        for kind, name, default in re.findall(r'MZ_([FUB])\((\w+),\s*([^,]+),', options):
            fields.append(f'CustomOptional<{dict(F="float", U="uint32_t", B="bool")[kind]}> {name}{{{default}}};')
        source = '''
#include <optional>
#include <concepts>
#include <utility>
#include <cstdint>
#include <string_view>
#include <cassert>
#include "ConfigKeys.h"
'''
        source += header[header.index('enum HasDefaultValue'):header.index('constexpr inline int UnboundKey')]
        source += '\nstruct Config {\n' + '\n'.join(fields) + '''
CustomOptional<bool> MochizukiUnlockPasses{false};
CustomOptional<uint32_t> MochizukiLayerMode{0};
CustomOptional<uint32_t> MochizukiWhitePointSource{0};
};
namespace PipelineUi { enum class Section {Input, Model, Output}; }
namespace AmdBridge {void InvalidateHistory(){}}
'''
        source += MENU[MENU.index('static bool MochizukiOptionOnPage'):MENU.index('static bool MochizukiChoice')]
        source += '''
int main() {
    using Section = PipelineUi::Section;
    for (bool current : {false, true}) for (bool pending : {false, true})
    for (bool captured : {false, true}) for (int group = -1; group < 8; ++group)
    for (int page = -1; page < 3; ++page) {
        Config config;
        config.MochizukiAcoMode = current;
        config.MochizukiAcoMode.set_for_next_launch(pending);
        AcoMenu() = {captured, current, pending};
        ResetMochizuki(&config, group, page < 0 ? std::nullopt : std::optional<Section>(Section(page)));
        assert(config.MochizukiAcoMode.value_or_default() == current);
        assert(config.MochizukiAcoMode.value_for_config().value_or(true) == pending);
        assert(AcoMenu().captured == captured && AcoMenu().session == current && AcoMenu().shown == pending);
    }
    Config ordinary;
    ordinary.MochizukiIntensity = 1.7f;
    ResetMochizuki(&ordinary);
    assert(ordinary.MochizukiIntensity.value_or_default() == 1.f);
}
'''
        work = ROOT / 'exports/config-aco-reset'
        work.mkdir(parents=True, exist_ok=True)
        cpp, exe = work / 'reset.cpp', work / 'reset.exe'
        cpp.write_text(source, encoding='utf-8')
        result = subprocess.run(['cl', '/nologo', '/std:c++20', '/EHsc', '/utf-8',
                                 '/I' + str(product), '/I' + str(product / 'dlssnr'), str(cpp),
                                 '/Fo' + str(work / 'reset.obj'), '/Fe' + str(exe)],
                                capture_output=True, text=True, errors='replace', timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        run = subprocess.run([str(exe)], capture_output=True, text=True, errors='replace', timeout=10)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    @unittest.skipUnless(os.name == "nt" and shutil.which("cl"), "requires MSVC")
    def test_unlocked_mochizuki_passes_reload(self):
        # Compile the actual Reload statements with a real SimpleIni reader;
        # an independently expected round-trip catches the old unconditional clamp.
        start = CONFIG.index('MochizukiUnlockPasses.set_from_config(')
        end = CONFIG.index('MochizukiTemporal.set_from_config(', start)
        statements = CONFIG[start:end]
        header = (ROOT / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/Config.h').read_text(encoding='utf-8')
        ceiling = re.search(r'kMochizukiLayerPasses\s*=\s*(\d+)', header).group(1)
        source = r'''
#include <cstdint>
#include <optional>
#include <string>
#include <cassert>
#include <iostream>
#include "SimpleIni.h"
#include "ConfigKeys.h"
#define LOG_WARN(...) ((void)0)
template<class T> struct Option {
    std::optional<T> value;
    void set_from_config(std::optional<T> v) { value=v; }
    T value_or_default() const { return value.value_or(T{}); }
};
struct ConfigFixture {
    CSimpleIniA ini;
    Option<bool> MochizukiUnlockPasses;
    Option<uint32_t> MochizukiLayerMode, MochizukiPasses;
    static constexpr uint32_t kMochizukiLayerPasses=@@CEILING@@;
    std::optional<bool> readBool(const char* section,const char* key) {
        return ini.GetValue(section,key)?std::optional<bool>(ini.GetBoolValue(section,key)):std::nullopt;
    }
    std::optional<uint32_t> readUInt(const char* section,const char* key) {
        return ini.GetValue(section,key)?std::optional<uint32_t>(uint32_t(ini.GetLongValue(section,key))):std::nullopt;
    }
    void Reload() { @@RELOAD@@ }
};
int main() {
    for(bool unlocked:{false,true}) for(unsigned passes:{0u,1u,3u,4u,15u,30u,31u}) {
        ConfigFixture saved;
        saved.ini.SetBoolValue(CfgKey::kSection,CfgKey::MochizukiUnlockPasses,unlocked);
        saved.ini.SetLongValue(CfgKey::kSection,CfgKey::MochizukiPasses,passes);
        std::string text; assert(saved.ini.Save(text)>=0);
        ConfigFixture fresh; assert(fresh.ini.LoadData(text.data(),text.size())>=0); fresh.Reload();
        const unsigned limit=unlocked?30u:3u;
        const unsigned expected=passes<1?1u:passes>limit?limit:passes;
        assert(fresh.MochizukiPasses.value && *fresh.MochizukiPasses.value==expected);
        assert(fresh.MochizukiUnlockPasses.value_or_default()==unlocked);
    }
    ConfigFixture missing; missing.Reload(); assert(!missing.MochizukiPasses.value);
    std::cout<<"MOCHIZUKI_PASS_RELOAD_OK 14 saved/reloaded cases plus absent key\n";
}
'''.replace('@@CEILING@@', ceiling).replace('@@RELOAD@@', statements)
        work = ROOT / 'exports/config-pass-reload'
        work.mkdir(parents=True, exist_ok=True)
        cpp, exe = work / 'reload.cpp', work / 'reload.exe'
        cpp.write_text(source, encoding='utf-8')
        product = ROOT / 'OptiScaler-DLSSNR-PreSR-Multipass-main'
        command = ['cl', '/nologo', '/std:c++20', '/EHsc', '/utf-8',
                   '/I' + str(product / 'external/simpleini'), '/I' + str(product / 'OptiScaler'),
                   str(cpp), '/Fo' + str(work / 'reload.obj'), '/Fe' + str(exe)]
        result = subprocess.run(command, capture_output=True, text=True, errors='replace', timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        run = subprocess.run([str(exe)], capture_output=True, text=True, errors='replace', timeout=10)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('MOCHIZUKI_PASS_RELOAD_OK', run.stdout)

    @unittest.skipUnless(os.name == "nt" and shutil.which("cl"), "requires an x64 MSVC developer environment")
    def test_host_updates_cross_independent_crt_caches(self):
        source = ROOT / "tests/host/lmxxf_config_crt.cpp"
        with tempfile.TemporaryDirectory(prefix="lmxxf-config-crt-") as td:
            out = Path(td)
            for fixture in (True, False):
                name = "runtime" if fixture else "host"
                target = out / (name + (".dll" if fixture else ".exe"))
                command = ["cl", "/nologo", "/std:c++17", "/EHsc", "/MT", "/W4", "/utf-8",
                           "/D_CRT_SECURE_NO_WARNINGS", str(source),
                           f"/Fo:{out / (name + '.obj')}", f"/Fe:{target}"]
                if fixture:
                    command += ["/LD", "/DLMXXF_CONFIG_CRT_FIXTURE"]
                    command += [f"/I{ROOT / 'third_party/lmxxf/Development/HIP'}",
                                f"/I{ROOT / 'third_party/lmxxf/src'}", "/DNOMINMAX"]
                result = subprocess.run(command, cwd=out, capture_output=True, text=True,
                                        errors="replace", timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(out / "host.exe"), str(out / "runtime.dll")],
                                    capture_output=True, text=True, errors="replace", timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("LMXXF_CONFIG_CRT_OK", result.stdout)

    def test_cross_layer_keys_use_upstream_dlss5_names(self):
        for name, ini_key in re.findall(r'inline constexpr const char \*(\w+) = "([^"]+)"', KEYS):
            if name.startswith("Mochizuki"):
                # Independent C-struct controls, not HIP environment variables.
                self.assertEqual(name, ini_key)
                self.assertIn(f"CfgKey::{name}", CONFIG)
                self.assertIn(f"    {name},", KEYS)
                continue
            if name.endswith("Legacy"):
                self.assertFalse(ini_key.startswith("DLSS5_"), name)
                continue
            if name in {
                "AutoExposure",
                "AutoExposureScale",
                "PaperWhite",
                "AllowEnhancedBarriers",
                "LmxxfInlineSlots",  # Host-owned inline admission limit.
                "NrInline",  # Host-owned submission mode, no upstream environment alias.
                "NrCaptureBackoff",  # Host-owned inline capture polling policy.
                "NrInlineReuse",  # Host-owned inline recording reuse.
                "NrGpuHandoff",  # Host-owned GPU-armed inline handoff.
                "NrInlineAutoSpin",  # Host-owned inline auto-spin controller switch.
                "NrSpinPollDelay",  # Host-owned inline spin experiment.
                "NrSpinSlices",  # Host-owned inline spin experiment.
                "NrWatchSlices",  # Host-owned inline spin experiment.
                "NrWatchStartDelay",  # Host-owned inline spin experiment.
                "NrInlineComputeConvert",  # Host-owned inline experiment (frame flag to the runtime).
                "NrInlineComputeMotion",  # Host-owned inline experiment.
                "NrInlineComputeDownscale",  # Host-owned inline experiment.
                "NrInlineComputeExport",  # Host-owned inline experiment.
                "NrAdaptiveWaitBudget",  # Host-owned inline experiment.
                "EarlyExeWrap",
                "EarlyAnyModuleWrap",
                "Diagnostic",
                "NrTimingEnabled",  # Product telemetry, not an upstream kernel option.
                "NrTimingLog",
                "NrFrameDiag",
                "NrGameWaitDiag",  # Host-owned game-thread timing diag.
                "LmxxfNetworkModel",  # Product model key aliases DLSS5_NETWORK_MODEL.
                "LmxxfModelHistory",  # Host-owned FrameInfo flag, not an upstream environment key.
                "NrStabilizerEnabled", "NrStabilizerAlpha", "NrStabilizerThreshold",
                "NrOverallIntensity",  # Shared final-output blend, no runtime environment alias.
                "NrPersonPartition", "NrPersonModel", "NrPersonStrength", "NrPersonDetailGain", "NrPersonDebugMask", "NrFaceInputSize",  # Host mask/compositor.
                "NrResidualLowGain", "NrResidualDetailGain", "NrResidualSkinProtection", "NrResidualEdgeProtection",
                "kSection",
                "kDanielSection",
                "kMenuSection",  # Host window settings do not cross the runtime boundary.
                "MenuLanguage", "MenuBlur", "MenuBlurStrength", "MenuWindowWidth", "MenuWindowHeight", "MenuWindowAnchor",
                "FpsOverlayColorR", "FpsOverlayColorG", "FpsOverlayColorB",
                "ToneCurve",
                "ToneLift",
                "AmdUseGameExposure",
                "AmdToneChannels",
                "Quality",
                "XeFGInterpolationCount",
                "kXeFGSection",
                "XeFGDynamicMFG",
                "XeFGFramerateTargetDMFG",
                "XeFGRoundingDMFG",
                "XeFGDynamicAllow1X",
                "QueuePriority",
                "QueuePriorityLegacy",
                "Inline",
                "Async",
            }:
                continue
            self.assertTrue(ini_key.startswith("DLSS5_"), f"{name} -> {ini_key}")

    def test_env_alias_is_identity_for_unified_keys(self):
        self.assertIn("if (std::strncmp(iniKey, \"DLSS5_\", 6) == 0)", KEYS)
        self.assertNotIn('return "DLSS5_FIT_LARGE"', KEYS)

    def test_shared_residual_controls_are_host_owned_and_neutral(self):
        product = ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main"
        header = (product / "OptiScaler/Config.h").read_text(encoding="utf-8")
        ini = (product / "OptiScaler.ini").read_text(encoding="utf-8")
        packager = (ROOT / "tools/release/PACKAGE_RELEASE.ps1").read_text(encoding="utf-8")
        reset = MENU.split("static void ResetSharedEffectsDefaults", 1)[1].split("static void ResetSharedNrDefaults", 1)[0]
        for key, value in (("NrPersonStrength", "1.0"), ("NrPersonDetailGain", "1.0"), ("NrResidualLowGain", "1.0"), ("NrResidualDetailGain", "1.0"),
                           ("NrResidualSkinProtection", "0.0"), ("NrResidualEdgeProtection", "0.0")):
            with self.subTest(key=key):
                self.assertIn(f'{key} = "{key}"', KEYS)
                self.assertIn(f"    {key},", KEYS.split("kKnown[]", 1)[1])
                self.assertIn(f"{key} {{ {value}f }}", header)
                self.assertIn(f"{key}.set_from_config(readFloat(CfgKey::kSection, CfgKey::{key}))", CONFIG)
                self.assertIn(f"ini.SetValue(CfgKey::kSection, CfgKey::{key},", CONFIG)
                self.assertIn(f"Instance()->{key}.value_for_config()", CONFIG)
                self.assertNotRegex(CONFIG, rf"PutEnv\w*\(CfgKey::{key}")
                self.assertNotIn(key, RUNTIME)
                self.assertIn(f"config->{key} = std::optional<float>{{}}", reset)
                for source in (ini, packager):
                    self.assertRegex(source, rf"(?m)^{key}={value}$")

    def test_nr_multiplier_has_separate_persistent_key(self):
        self.assertIn('XeFGInterpolationCount = "XeFGInterpolationCount"', KEYS)
        self.assertIn('    XeFGInterpolationCount,', KEYS.split('kKnown[]', 1)[1])
        self.assertIn('DlssNrXeFGInterpolationCount.set_from_config(readInt(CfgKey::kSection, CfgKey::XeFGInterpolationCount))', CONFIG)
        self.assertIn('ini.SetValue(CfgKey::kSection, CfgKey::XeFGInterpolationCount,', CONFIG)
        self.assertIn('Instance()->DlssNrXeFGInterpolationCount.value_for_config()', CONFIG)
        self.assertIn('DlssNrXeFGInterpolationCount.value() < 0', CONFIG)
        xefg = (ROOT / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/framegen/xefg/XeFG_Dx12.cpp').read_text(encoding='utf-8')
        self.assertNotRegex(xefg, r'FGXeFGInterpolationCount\s*=')
        self.assertNotIn('FGXeFGInterpolationCount.set_volatile_value', xefg)
        self.assertIn('DlssNr::IsActiveForFrameGeneration()', xefg)
        nr = (ROOT / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp').read_text(encoding='utf-8')
        activity = nr.split('bool IsActiveForFrameGeneration()', 1)[1].split('const char* FailureReason()', 1)[0]
        self.assertIn('g_activityBackend.load', activity)
        self.assertNotIn('HasFiles()', activity)

    def test_flags_fallback_does_not_overwrite_env(self):
        self.assertIn("if (std::getenv(key))", RUNTIME)
        self.assertIn("continue; // menu / ini / caller already owns this key", RUNTIME)
        self.assertIn("ApplyFlagsFileFallback", RUNTIME)

    def test_host_writes_env_from_ini_before_flags(self):
        self.assertIn("PutEnvAlias(CfgKey::FitLarge", CONFIG)
        self.assertIn("PutEnvAlias(CfgKey::Pdl", CONFIG)
        self.assertIn("PutEnvString(CfgKey::NetworkHeight", CONFIG)
        self.assertIn("PutEnvString(CfgKey::SkipBlocks", CONFIG)
        self.assertIn("Instance()->LmxxfSkipBlocks.value_for_config_or", CONFIG)
        # Legacy names are read, never written back as the save key.
        self.assertIn("FitLargeLegacy", CONFIG)
        self.assertNotIn('SetValue(CfgKey::kSection, CfgKey::FitLargeLegacy', CONFIG)

    def test_inline_auto_spin_and_soft_default_axes_contract(self):
        product = ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main"
        header = (product / "OptiScaler/Config.h").read_text(encoding="utf-8")
        ini = (product / "OptiScaler.ini").read_text(encoding="utf-8")
        self.assertIn('NrInlineAutoSpin = "NrInlineAutoSpin"', KEYS)
        self.assertIn("    NrInlineAutoSpin,", KEYS.split("kKnown[]", 1)[1])
        self.assertIn("CustomOptional<bool> NrInlineAutoSpin { true };", header)
        self.assertIn("CustomOptional<int, SoftDefault> NrSpinPollDelay { 16 };", header)
        self.assertIn("CustomOptional<int, SoftDefault> NrSpinSlices { 0 };", header)
        self.assertIn("NrInlineAutoSpin.set_from_config(readBool(CfgKey::kSection, CfgKey::NrInlineAutoSpin))", CONFIG)
        self.assertIn("ini.SetValue(CfgKey::kSection, CfgKey::NrInlineAutoSpin, GetBoolValue(Instance()->NrInlineAutoSpin.value_for_config()).c_str())", CONFIG)
        self.assertIn("NrSpinPollDelay.reset();", CONFIG)
        self.assertIn("NrSpinSlices.reset();", CONFIG)
        self.assertRegex(ini, r"(?m)^NrInlineAutoSpin=auto$")
        self.assertRegex(ini, r"(?m)^NrSpinPollDelay=auto$")
        self.assertRegex(ini, r"(?m)^NrSpinSlices=auto$")

    def test_temporal_mode_and_enhance_strength_contract(self):
        product = ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main"
        header = (product / "OptiScaler/Config.h").read_text(encoding="utf-8")
        ini = (product / "OptiScaler.ini").read_text(encoding="utf-8")
        self.assertIn('TemporalMode = "DLSS5_TEMPORAL_MODE"', KEYS)
        self.assertIn('TemporalEnhanceStrength = "DLSS5_TEMPORAL_ENHANCE_STRENGTH"', KEYS)
        self.assertIn("    TemporalMode,", KEYS.split("kKnown[]", 1)[1])
        self.assertIn("    TemporalEnhanceStrength,", KEYS.split("kKnown[]", 1)[1])
        self.assertIn("CustomOptional<uint32_t> LmxxfTemporalMode { 1u };", header)
        self.assertIn("CustomOptional<float> LmxxfTemporalEnhanceStrength { 1.0f };", header)
        self.assertIn("PutEnvString(CfgKey::TemporalMode", CONFIG)
        self.assertIn("PutEnvString(CfgKey::TemporalEnhanceStrength", CONFIG)
        self.assertIn("ini.SetValue(CfgKey::kSection, CfgKey::TemporalMode", CONFIG)
        self.assertIn("ini.SetValue(CfgKey::kSection, CfgKey::TemporalEnhanceStrength", CONFIG)
        self.assertRegex(ini, r"(?m)^DLSS5_TEMPORAL_MODE=1$")
        self.assertRegex(ini, r"(?m)^DLSS5_TEMPORAL_ENHANCE_STRENGTH=1\.0$")

    def test_menu_labels_are_not_ini_keys(self):
        # Display strings must not appear as SetValue / CfgKey string literals.
        for label in ("High resolution", "Network tier", "Wave-owned attention", "ViT adaptive reuse", "Inline auto-tuning", "Spin poll delay", "Game wait dispatches", "Temporal mode", "Enhance strength"):
            self.assertNotIn(f'"{label}"', CONFIG)
            self.assertNotIn(f'"{label}"', KEYS)



if __name__ == "__main__":
    unittest.main(verbosity=2)
