# Menu navigation and background rendering

The Insert menu uses eight stable page IDs: Neural Rendering, Upscaling, Frame
Generation, Latency & FPS, Textures, Compatibility, Menu, and Help / Status.
The NR page retains the existing input/model/output navigation, backend controls,
history/reuse exclusion and reset groups. Language and Save Settings remain in
the footer. Labels do not become INI identifiers; MenuLocale provides translations.

The sidebar measures translated labels. Narrow windows or high UI scale use a
compact page selector instead. Page content wraps to its viewport and scrolls
vertically; it must not preserve an old horizontal content width after resizing.
Cards reuse the first section heading instead of introducing another foldout.
Custom, bundled CJK and fallback font paths retain the same mixed-script sizing.

## Shortcuts and window placement

Menu, FPS overlay, FPS-cycle, frame generation and NR bindings accept a base key
with Ctrl, Shift, Alt and Tab. Existing decimal/hex virtual-key INIs still load;
text such as `Ctrl+Shift+Home` is also accepted. Saved numeric modifier bits are
Ctrl `0x10000`, Shift `0x20000`, Alt `0x40000`, Tab `0x80000`. Exact modifiers
must match. All actions sharing a chord fire together; an accepted release is
consumed once across message/poll delivery. Capture accepts standalone modifiers,
including the UWP polling path. NR stays End by default and FG stays unbound.
The existing Ctrl+F11 Mochizuki model switch keeps ownership of F11 until release.

`[Menu] WindowAnchor` remains the only placement key: 0 free, 1 top-left,
2 top-right, 3 bottom-left and 4 bottom-right preserve their saved meanings.
5/6/7/8 dock left/right/top/bottom using the bundled docking ImGui. Free remains
the default. Dragging out floats the window; Save Settings keeps the choice.
The dockspace has a pass-through centre, survives hidden/reopened menus, and
retains unrelated ImGui config flags. The main window has a stable `###` identity
across caption and language changes. Existing branding, wiki and navigation remain.

The footer's GPU curve is **sampled completed-execution history**, not every
frame. A fixed 360-point ring appends only a newly observed measured completion:
lmxxf network GPU or Mochizuki Vulkan-core GPU (core input/composition included,
bridge copies/shared effects excluded). Cached polling never invents points for
unseen executions. Session/measurement epoch, backend, route, NR off, missing or
stale data clears/partitions history even with menu/overlay hidden. Measured zero
is valid; missing data, including Daniel without a network metric, is `NR GPU: N/A`.
No CPU wall time is substituted and opening the menu does not enable sampling.

Focused CPU input/lifetime/shortcut/history/layout tests run through
`tests\host\run.cmd input-menu`. `tests/host/menu-localization.cmd` also renders
the production docking and history helpers in WARP with all sides, old anchors,
hidden/reopen, title/language identity and viewport resize. The history pictures
use explicitly labelled deterministic measurement fixtures, not live NR telemetry.


Layout and blur rendering are selectively adapted from OptiScaler upstream
`434b9555d960e6e471a461c2b0d98b8e1fdacfa2`; existing NR logic and overlay entry
points remain local. `[Menu] BackgroundBlur` and `BackgroundBlurStrength` belong
to ConfigKeys/Config, with defaults true/1.0 and strength clamped to 0..4. They
are persisted and reset with other appearance controls. Only the visible menu
requests blur. Unsupported backbuffer layouts/usage retain the ordinary background.

DX11 copies the current backbuffer and restores compute bindings. DX12 uses a
menu fence, per-backbuffer source descriptors and allocator completion checks;
an unfinished frame skips the overlay. Queue changes and cleanup drain submitted
work; failed completion retains resources and disables their reuse. Blur textures
are released before the ImGui descriptor heap. Vulkan uses the existing overlay
queue and command buffers, requires transfer-source usage and compute capability,
and waits for prior work before resize/destruction. The private Mochizuki Vulkan
scope is unchanged. No temporary rendering device or global spoofing is added.

`tests/host/menu-localization.cmd` renders real ImGui components with WARP across
fonts/languages/scales, including every navigation page and narrow/wide resize.
This covers component layout, not full-game rendering. Open/close, HDR,
Alt+Tab/resize and device shutdown on DX11/DX12/Vulkan remain game acceptance items.
