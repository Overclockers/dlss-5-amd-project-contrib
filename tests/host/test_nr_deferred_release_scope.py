"""Plan section 10 DeferredRelease constraints, checked against the shipped sources.

1. NR objects never enter the DeferredRelease queue: native NR retires its own
   objects (Retirement::Immediate), so the NR trees must not call DeferredRelease
   and must opt every shared helper out of deferred retirement.
2. Private devices never trigger DropOwnersExcept: every call site outside
   misc/DeferredRelease.* has to sit behind a ScopedSkipVulkanHooks guard.

deferred_release_contract.cpp covers the queue semantics by linking the real
misc/DeferredRelease.cpp; this file covers who is allowed to use it.
"""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main" / "OptiScaler"
NR_TREES = [SRC / "dlssnr", SRC / "shaders" / "dlssnr"]
CODE = {".cpp", ".h", ".hpp", ".inl"}


def code_files(*roots):
    for root in roots:
        for path in sorted(root.rglob("*")):
            if path.is_file() and path.suffix.lower() in CODE:
                yield path


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


class NrDeferredReleaseScope(unittest.TestCase):
    def test_nr_trees_never_call_deferred_release(self):
        offenders = [str(p.relative_to(ROOT)) for p in code_files(*NR_TREES)
                     if "DeferredRelease" in strip_comments(p.read_text(encoding="utf-8", errors="replace"))]
        self.assertEqual(offenders, [], "NR code must retire immediately, not through DeferredRelease")

    def test_nr_shaders_opt_out_of_deferred_retirement(self):
        dx12 = strip_comments((SRC / "shaders/dlssnr/DlssNr_Dx12.cpp").read_text(encoding="utf-8"))
        vk = strip_comments((SRC / "shaders/dlssnr/DlssNr_Vk.cpp").read_text(encoding="utf-8"))
        self.assertRegex(dx12, r"Shader_Dx12\([^)]*Retirement::Immediate\)")
        self.assertIn("SetDeferredRetirement(false)", dx12)
        self.assertRegex(vk, r"Shader_Vk\([^)]*Retirement::Immediate\)")
        for match in re.finditer(r"GpuTime_Dx12>\(([^)]*)\)", dx12):
            self.assertTrue(match.group(1).replace(" ", "").endswith(",false"),
                            "NR GpuTime_Dx12 must not use deferred retirement: " + match.group(0))
        # Shader_Dx12::CreateBufferResource always defers the old buffer, whatever _retirement says.
        self.assertNotRegex(dx12, r"\bCreateBufferResource\s*\(",
                            "Shader_Dx12::CreateBufferResource ignores Retirement::Immediate")

    def test_drop_owners_except_is_guarded_against_private_devices(self):
        for path in code_files(SRC):
            if path.parent.name == "misc" and path.stem == "DeferredRelease":
                continue
            lines = strip_comments(path.read_text(encoding="utf-8", errors="replace")).splitlines()
            for index, line in enumerate(lines):
                if "DropOwnersExcept(" not in line:
                    continue
                window = "\n".join(lines[max(0, index - 8):index + 1])
                self.assertIn("ScopedSkipVulkanHooks::Active()", window,
                              f"{path.relative_to(ROOT)}:{index + 1} drops owners without the private-device guard")


if __name__ == "__main__":
    unittest.main(verbosity=2)
