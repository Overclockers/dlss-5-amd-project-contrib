# Mochizuki 0.0.6 Windows integration

Baseline: `8a70f7f418294ff1e306b66893456e5319ff8205`.
Target: `191b97b54175d50c7ec5992b54322dd3eb0ef8ac` (0.0.6-win-preview-alpha).
Reviewed on 2026-10-17 against the final baseline-to-target diff.

## Model and runtime behavior

The compiled 310.9.16 graph has 76 layers and 31 blocks; the original 310.8.0
graph retains 152 layers and 71 blocks. Both use the existing shader and ACO
corpus. The upstream closure contains no shader, ACO record or recipe change.
The smaller graph changes the ViT boundaries, permits one-layer persistent
DS/UPS runs, aligns 1080p working height to 1152, and uses its own post-block
history blend scale (`0x3A23`, rather than the original `0x39EB`). These changes
are imported together; using only a new descriptor would be incorrect.

`[DlssNr] MochizukiNetworkModel=1` selects the new model by default; `0` selects
the original. The menu labels both DLL versions. The exact current controls/info
sizes remain mandatory between the host and runtime. Model selection is part of
the build key and its capacity-reuse comparison. Each build receives an explicit
`RuntimeConfig::network`; neither `DLSSNR_MODEL` nor upstream INI can replace it.
The published info names the installed model separately from the current request.
Retained recordings keep the runtime that produced them during a rebuild.

Missing `dlssnr-small.bin` follows the upstream documented model fallback to the
original pack, with a log and effective-model status. INT4 is admitted only for
an explicit original-model request; a small-model request never selects the INT4
ACO corpus. Model switching and Ctrl+F11 preserve the other setting's preference.
Both models remain available with native compilation, ACO and RenoDX processing.

## Extraction and installation

The Python installer accepts the two upstream DLL SHA-256 pins. A 310.9.16 DLL
extracts both `WEIGHTS_HT` and `WEIGHTS_CC_SMALL_FP8`; 310.8.0 extracts only the
original model. The descriptor, post block, file list and per-entry hash list
follow each resource. All requested packs validate before any output is replaced.
Installation keeps the weights-only parser path, without loading the DLL or
scanning unrelated CUDA modules. Both models are installed when available;
overwriting the package preserves existing menu/INI choices and model files.
The new model is excluded from the package and retained in backup cleanup just
like the original. It is not part of the owned shader/ACO inventory.

## Upstream feature coverage

| Changed paths | Decision and evidence |
| --- | --- |
| `windows/src/core/nr_{graph,native_plan,native_plan_data_small,runtime,ini_network}.*` | Import model table, graph scheduling, model selection and blend scale together. Rebase the existing optional host patches; supply explicit product selection. |
| `windows/src/core/nr_pal_binary.hpp` | Import the gfx1200 ELF machine-ID acceptance alongside gfx1201. CPU fixture accepts both and rejects an older architecture. Driver/game import on RX 9060 remains untested here. |
| Matching `linux/src/core` changes | Reviewed as the source of the Windows port. This D3D12 product compiles the Windows core only. |
| `linux/package/model-tools/{descriptor-small.json,model-files-small.*,inspect_nr.py,unpack_postblock.py,extract_model.sh}` | Import new metadata and resource-aware unpacking. Rebase patch 12 to retain weights-only extraction for both resources and the probe. |
| `windows/package/model-tools/*` | Reviewed the dual-model native extractor and generated small tables; keep the existing Python route, using the same resource names, descriptor and entry hashes. Native extractor adoption is outside this update; Python 3.10+ remains required. |
| `windows/src/pe/nr_pe_config.*`, `linux/src/pe/nr_pe_config.*` | Reviewed new defaults and Model config. Product uses its own Config/menu and runtime controls. The pinned Windows bridge and NGX ABI headers are unchanged. |
| Both platforms' `build/{assemble_product_data,build_package}.sh`, `package/{install.*,make_ini.py}` | Reviewed model packaging, default-small selection and original-only INT4. Reimplement those policies in the existing product installer/packager; do not import the PE/ReShade hosts. |
| `.github/workflows/packages.yml` | Reviewed model exclusion and Linux-only tag filtering. Extend this product's ZIP guards for small weights; retain its own Windows Actions workflow. |
| `README.md`, `THIRD_PARTY.md`, `CLAUDE.md`, `docs/ARCHITECTURE.md`, both package READMEs, `docs/ngx-verification/**` | Model documentation, archived 310.8.0 evidence and new 310.9.16 evidence. Link to upstream; do not present upstream GPU numbers as local results. |

All 23 local patches remain explicit and replayable. Patch 12 required a manual
merge because upstream added resource selection and dual-model shell extraction
at the same locations as weights-only extraction. No preservation overlay is used.

## Validation scope

Local source-pin replay passes: 568 upstream files, six added shaders, 23 patches.
Actual extraction from the supported 310.9.16 DLL also passed all 599 original
and 255 new model entry hashes; both packs were published to local build output.
Actions `release` with `test_package=true` runs full `ci`, compiles both runtime
and host, replays the pin, signs and verifies the final ZIP, then uploads a test
artifact. It does not create a Release or tag. The model-plan, resource-selection,
failure-preservation, PAL architecture and dual-model installer tests run there.

Actions completion is pending for this initial branch submission. No new-model
GPU frames, temporal/barrier oracle, driver import, retained model-switch frames
or game acceptance were run locally. Those require the user's legal model files
and a supported GPU; no FPS or output-quality claim is made from CPU CI.
