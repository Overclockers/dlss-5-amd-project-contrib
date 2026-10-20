#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/input/shortcut_keys.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/MochizukiNetworkShortcut.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <string>

using namespace ShortcutKeys;

static void Parsing()
{
    const struct { const char* text; int binding; } cases[] = {
        {"0x2D", VK_INSERT}, {"36", VK_HOME}, {"8", VK_BACK}, {"-1", -1}, {"0", 0},
        {"Ctrl+Shift+Home", Ctrl | Shift | VK_HOME}, {" alt + Tab + F10 ", Alt | Tab | VK_F10},
        {"Ctrl+0x2D", Ctrl | VK_INSERT}, {"Ctrl+5", Ctrl | '5'}, {"Tab", VK_TAB},
        {"Ctrl", VK_CONTROL}, {"Alt+Numpad5", Alt | VK_NUMPAD5}, {"F24", VK_F24},
        {"0x30024", Ctrl | Shift | VK_HOME}, {"+36", VK_HOME},
    };
    for (const auto& c : cases)
    {
        assert(Parse(c.text) == c.binding);
        // The menu/INI numeric representation reloads without losing modifier bits.
        assert(Parse(std::to_string(c.binding)) == c.binding);
        if (c.binding > 0)
        {
            char hex[32];
            std::snprintf(hex, sizeof(hex), "0x%X", c.binding);
            assert(Parse(hex) == c.binding);
        }
    }
    for (const char* invalid : {"", "auto", "Ctrl+", "Home+Ctrl", "Ctrl+-1", "F25", "256",
         "0x100000024", "999999999999999999999", "0x1000000", "Ctrl+NotAKey", "0x"})
        assert(!Parse(invalid));
    for (int mods = 0; mods <= ModifierMask; mods += Ctrl)
    {
        assert(Matches(mods | VK_HOME, VK_HOME, mods));
        assert(!Matches(mods | VK_HOME, VK_END, mods));
        assert(!Matches(mods | VK_HOME, VK_HOME, mods ^ Ctrl));
    }
    assert(Matches(VK_TAB, VK_TAB, Tab));
    assert(Matches(VK_LCONTROL, VK_LCONTROL, Ctrl));
    assert(!Matches(-1, VK_HOME, 0));
}

static void ModifiersAndRelease()
{
    std::array<bool, 256> down {};
    down[VK_RCONTROL] = down[VK_LSHIFT] = down[VK_RMENU] = down[VK_TAB] = true;
    assert(HeldModifiers([&](int vk) { return down[vk]; }) == ModifierMask);

    ReleaseTracker release;
    // Message press, polling the same hold, message release, then duplicate poll release.
    assert(!release.Update(VK_HOME, true, true, false, Ctrl | Shift));
    assert(!release.Update(VK_HOME, true, false, false, 0));
    auto event = release.Update(VK_HOME, false, false, true, 0);
    assert(event && *event == (Ctrl | Shift));
    assert(!release.Update(VK_HOME, false, false, true, 0));
    assert(!release.Update(VK_HOME, false, false, true, 0));
    // A new press is accepted even immediately after the previous release.
    assert(!release.Update(VK_HOME, true, true, false, Alt));
    assert(release.Update(VK_HOME, false, false, true, 0) == Alt);
    // Fast message chord: down and up both before a render frame, modifiers already released.
    assert(release.Update(VK_HOME, false, true, true, Ctrl | Tab) == (Ctrl | Tab));
    // UWP polling has no LastPressedKey callback; down/release state is sufficient.
    assert(!release.Update(VK_END, true, true, false, Shift));
    assert(release.Update(VK_END, false, false, true, 0) == Shift);
    assert(!release.Update(VK_END, false, false, true, 0));
    // Losing focus invalidates an in-flight press rather than accepting its eventual up.
    release.Update(VK_HOME, true, true, false, Ctrl);
    release.Reset();
    assert(!release.Update(VK_HOME, false, false, true, Ctrl));
}

static void CaptureBindings()
{
    Capture capture;
    capture.Begin();
    capture.Press(VK_LCONTROL);
    capture.Press(VK_HOME);
    capture.Release(VK_LCONTROL, Ctrl);
    assert(!capture.finished);
    capture.Release(VK_HOME, Ctrl | Shift | Alt | Tab);
    assert(capture.finished && capture.binding == (Ctrl | Shift | Alt | Tab | VK_HOME));
    capture.Release(VK_HOME, 0);
    assert(capture.binding == (ModifierMask | VK_HOME));

    for (int vk : {VK_CONTROL, VK_LCONTROL, VK_RSHIFT, VK_MENU, VK_TAB})
    {
        capture.Begin();
        capture.Press(vk);
        capture.Release(vk, ModifierOf(vk));
        const int expected = ModifierOf(vk) == Ctrl ? VK_CONTROL :
            ModifierOf(vk) == Shift ? VK_SHIFT : ModifierOf(vk) == Alt ? VK_MENU : VK_TAB;
        assert(capture.finished && capture.binding == expected);
    }
    // Tab and modifiers can be the chord's base key, not only held modifiers.
    capture.Begin();
    capture.Press(VK_LCONTROL, Ctrl);
    capture.Press(VK_TAB, Ctrl | Tab);
    capture.Release(VK_LCONTROL, Ctrl);
    assert(!capture.finished);
    capture.Release(VK_TAB, Ctrl | Tab);
    assert(capture.binding == (Ctrl | VK_TAB));
    capture.Begin();
    capture.Press(VK_TAB, Tab);
    capture.Press(VK_RSHIFT, Tab | Shift);
    capture.Release(VK_TAB, Tab);
    assert(!capture.finished);
    capture.Release(VK_RSHIFT, Tab | Shift);
    assert(capture.binding == (Tab | VK_SHIFT));
    // Fast/UWP capture uses the same press/release interface, including polled-only chords.
    capture.Begin();
    capture.Press(VK_TAB);
    capture.Press('A');
    capture.Release(VK_TAB, Tab);
    capture.Release('A', Tab | Ctrl);
    assert(capture.binding == (Tab | Ctrl | 'A'));
    capture.Begin();
    capture.Press(VK_LBUTTON);
    capture.Release(VK_LBUTTON, 0);
    assert(!capture.finished);
    capture.Press(VK_ESCAPE);
    capture.Release(VK_ESCAPE, 0);
    assert(capture.finished && !capture.binding);
    capture.Begin();
    capture.Press(VK_BACK);
    capture.Release(VK_BACK, Ctrl);
    assert(capture.binding == -1);
}

static void SharedAndReservedChords()
{
    const int bindings[] = {Ctrl | VK_HOME, Ctrl | VK_HOME, Ctrl | VK_HOME,
                            Ctrl | VK_HOME, Ctrl | VK_HOME}; // Menu, FPS, cycle, FG, NR
    ReleaseTracker release;
    release.Update(VK_HOME, true, true, false, Ctrl);
    auto event = release.Update(VK_HOME, false, false, true, 0);
    assert(event);
    int actions = 0;
    for (int binding : bindings) actions += Accepts(binding, VK_HOME, *event, false);
    assert(actions == 5); // Never first-matching-action semantics.
    assert(!release.Update(VK_HOME, false, false, true, 0));

    MochizukiNetworkShortcut reserved;
    auto network = reserved.Update(true, true, true, true);
    assert(network.toggle && network.suppressF11);
    assert(!Accepts(Ctrl | VK_F11, VK_F11, Ctrl, network.suppressF11));
    assert(!Accepts(VK_F11, VK_F11, 0, network.suppressF11));
    assert(reserved.Update(false, true, true, true).suppressF11);
    network = reserved.Update(false, false, true, true);
    assert(network.suppressF11); // Ctrl released first: F11 up is still reserved.
    assert(!Accepts(Ctrl | VK_F11, VK_F11, Ctrl, network.suppressF11));
    reserved.Update(false, false, true, true);
    assert(Accepts(VK_F11, VK_F11, 0, false));
    assert(!Accepts(VK_F11, VK_F11, Ctrl, false));
    assert(Accepts(Ctrl | VK_F11, VK_F11, Ctrl, false)); // Other NR backend.
    // Fast reserved chord: press and release in one frame must consume F11 too.
    network = reserved.Update(true, true, true, true);
    assert(network.toggle && network.suppressF11);
    assert(reserved.Update(false, false, false, false).suppressF11);
}

int main()
{
    Parsing();
    ModifiersAndRelease();
    CaptureBindings();
    SharedAndReservedChords();
    puts("SHORTCUT_KEYS_OK");
}
