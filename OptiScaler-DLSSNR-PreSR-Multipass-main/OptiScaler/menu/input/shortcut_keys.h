#pragma once

// A shortcut binding: a virtual key in the low 16 bits and the modifiers that must be held with it above them.
// A plain virtual key, as older INIs hold, is the key with no modifier. -1 or 0 is unbound.
// No Windows or OptiScaler dependency beyond the VK_ constants, so tests can include it alone.

#include <windows.h>

#include <array>
#include <charconv>
#include <cctype>
#include <system_error>
#include <optional>
#include <string>
#include <string_view>

namespace ShortcutKeys
{
constexpr int Ctrl = 1 << 16;
constexpr int Shift = 1 << 17;
constexpr int Alt = 1 << 18;
constexpr int Tab = 1 << 19;
constexpr int ModifierMask = Ctrl | Shift | Alt | Tab;

inline int Key(int binding) { return binding > 0 ? binding & 0xFFFF : 0; }
inline int Modifiers(int binding) { return binding > 0 ? binding & ModifierMask : 0; }
inline bool Bound(int binding) { return Key(binding) > 0 && Key(binding) < 256; }

// The modifier a key is, or 0.
inline int ModifierOf(int vk)
{
    switch (vk)
    {
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return Ctrl;
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return Shift;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return Alt;
    case VK_TAB:
        return Tab;
    default:
        return 0;
    }
}

template <typename Down> int HeldModifiers(Down down)
{
    int held = 0;
    if (down(VK_CONTROL) || down(VK_LCONTROL) || down(VK_RCONTROL)) held |= Ctrl;
    if (down(VK_SHIFT) || down(VK_LSHIFT) || down(VK_RSHIFT)) held |= Shift;
    if (down(VK_MENU) || down(VK_LMENU) || down(VK_RMENU)) held |= Alt;
    if (down(VK_TAB)) held |= Tab;
    return held;
}

// Fires only when exactly the binding's modifiers are held. A binding whose own key is a modifier
// (Tab alone, say) does not count that modifier.
inline bool Matches(int binding, int vk, int heldModifiers)
{
    if (!Bound(binding) || Key(binding) != vk)
        return false;

    const int own = ModifierOf(vk);
    return (heldModifiers & ModifierMask & ~own) == (Modifiers(binding) & ~own);
}

inline std::string ModifierText(int modifiers)
{
    std::string text;
    if (modifiers & Ctrl)
        text += "Ctrl+";
    if (modifiers & Shift)
        text += "Shift+";
    if (modifiers & Alt)
        text += "Alt+";
    if (modifiers & Tab)
        text += "Tab+";
    return text;
}

struct NamedKey
{
    const char* name;
    int vk;
};

// Names the INI accepts for a key, besides a letter, F1-F24, NumpadN and a number (a digit after a modifier).
inline constexpr NamedKey NamedKeys[] = {
    { "Insert", VK_INSERT },
    { "Ins", VK_INSERT },
    { "Delete", VK_DELETE },
    { "Del", VK_DELETE },
    { "Home", VK_HOME },
    { "End", VK_END },
    { "PageUp", VK_PRIOR },
    { "PgUp", VK_PRIOR },
    { "PageDown", VK_NEXT },
    { "PgDn", VK_NEXT },
    { "PrintScreen", VK_SNAPSHOT },
    { "PrtSc", VK_SNAPSHOT },
    { "Pause", VK_PAUSE },
    { "ScrollLock", VK_SCROLL },
    { "Space", VK_SPACE },
    { "Enter", VK_RETURN },
    { "Backspace", VK_BACK },
    { "Escape", VK_ESCAPE },
    { "Esc", VK_ESCAPE },
    { "Tab", VK_TAB },
    { "Up", VK_UP },
    { "Down", VK_DOWN },
    { "Left", VK_LEFT },
    { "Right", VK_RIGHT },
    { "NumpadAdd", VK_ADD },
    { "NumpadSubtract", VK_SUBTRACT },
    { "NumpadMultiply", VK_MULTIPLY },
    { "NumpadDivide", VK_DIVIDE },
    { "NumpadDecimal", VK_DECIMAL },
    { "Ctrl", VK_CONTROL },
    { "Shift", VK_SHIFT },
    { "Alt", VK_MENU },
};

inline bool SameText(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;

    return true;
}

inline std::optional<int> ParseKey(std::string_view token)
{
    if (token.empty())
        return std::nullopt;

    // A lone digit is a key code, as older INIs wrote them (8 is Backspace); Parse makes it the digit key
    // after a modifier.
    if (token.size() == 1 && std::isalpha(static_cast<unsigned char>(token[0])))
        return std::toupper(static_cast<unsigned char>(token[0]));

    for (const auto& named : NamedKeys)
        if (SameText(token, named.name))
            return named.vk;

    auto number = [](std::string_view digits, int base) -> std::optional<int>
    {
        if (digits.empty())
            return std::nullopt;
        if (digits.front() == '+') digits.remove_prefix(1);
        if (digits.empty()) return std::nullopt;
        int value = 0;
        const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
        if (result.ec != std::errc {} || result.ptr != digits.data() + digits.size())
            return std::nullopt;
        return value;
    };

    if ((token[0] == 'F' || token[0] == 'f') && token.size() <= 3)
        if (auto n = number(token.substr(1), 10); n && *n >= 1 && *n <= 24)
            return VK_F1 + *n - 1;

    if (token.size() == 7 && SameText(token.substr(0, 6), "Numpad") &&
        std::isdigit(static_cast<unsigned char>(token[6])))
        return VK_NUMPAD0 + (token[6] - '0');

    if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
        return number(token.substr(2), 16);

    return number(token, 10);
}

// "Ctrl+Shift+Home", "Alt+0x24", "0x30024", "36" or "-1". Spaces around the parts are allowed.
inline std::optional<int> Parse(std::string_view text)
{
    auto trim = [](std::string_view s)
    {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
            s.remove_prefix(1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
            s.remove_suffix(1);
        return s;
    };

    text = trim(text);
    if (text.empty())
        return std::nullopt;

    int modifiers = 0;
    for (;;)
    {
        const auto plus = text.find('+', 1);
        if (plus == std::string_view::npos)
            break;

        const auto part = trim(text.substr(0, plus));
        if (SameText(part, "Ctrl") || SameText(part, "Control"))
            modifiers |= Ctrl;
        else if (SameText(part, "Shift"))
            modifiers |= Shift;
        else if (SameText(part, "Alt"))
            modifiers |= Alt;
        else if (SameText(part, "Tab"))
            modifiers |= Tab;
        else
            return std::nullopt;

        text = trim(text.substr(plus + 1));
    }

    auto key = modifiers != 0 && text.size() == 1 && std::isdigit(static_cast<unsigned char>(text[0]))
                   ? std::optional<int>(text[0])
                   : ParseKey(text);
    if (!key)
        return std::nullopt;

    // A number on its own may carry modifier bits already (the form OptiScaler writes).
    if (*key <= 0)
        return modifiers == 0 ? key : std::nullopt;

    if ((*key & ~(0xFFFF | ModifierMask)) != 0 || (*key & 0xFFFF) == 0 || (*key & 0xFFFF) >= 256)
        return std::nullopt;

    return *key | modifiers;
}

// A release is consumed once per physical press, independent of how many input
// sources report it. Preserve the original-down modifiers through early release.
class ReleaseTracker
{
    struct Press { bool armed = false; int modifiers = 0; };
    std::array<Press, 256> keys {};
  public:
    void Reset() { keys = {}; }
    std::optional<int> Update(int vk, bool down, bool pressed, bool released, int modifiers)
    {
        if (vk <= 0 || vk >= 256) return std::nullopt;
        auto& key = keys[vk];
        if (pressed || (down && !key.armed)) key = {true, modifiers};
        if (!released || !key.armed) return std::nullopt;
        const int result = key.modifiers;
        key.armed = false;
        return result;
    }
};

// All actions bound to one chord fire together. Reserved Ctrl+F11 owns the base
// key through release and excludes every configured F11 action during ownership.
inline bool Accepts(int binding, int vk, int modifiers, bool reservedF11)
{
    return !(vk == VK_F11 && reservedF11) && Matches(binding, vk, modifiers);
}

class Capture
{
    bool chord = false;
    int modifierKey = 0;
    int modifierCount = -1;
  public:
    bool finished = false;
    std::optional<int> binding;
    void Begin() { chord = false; modifierKey = 0; modifierCount = -1; finished = false; binding.reset(); }
    void Press(int vk, int modifiers = 0)
    {
        if (vk > VK_XBUTTON2 && ModifierOf(vk) == 0) chord = true;
        else if (ModifierOf(vk) != 0)
        {
            int count = 0;
            for (int bits = modifiers & ModifierMask & ~ModifierOf(vk); bits != 0; bits &= bits - 1) ++count;
            if (count > modifierCount) { modifierCount = count; modifierKey = vk; }
        }
    }
    void Release(int vk, int modifiers)
    {
        if (finished || vk <= VK_XBUTTON2 ||
            (ModifierOf(vk) != 0 && (chord || (modifierKey != 0 && ModifierOf(vk) != ModifierOf(modifierKey))))) return;
        finished = true;
        if (vk == VK_LCONTROL || vk == VK_RCONTROL) vk = VK_CONTROL;
        else if (vk == VK_LSHIFT || vk == VK_RSHIFT) vk = VK_SHIFT;
        else if (vk == VK_LMENU || vk == VK_RMENU) vk = VK_MENU;
        if (vk == VK_ESCAPE) return;
        binding = vk == VK_BACK ? -1 : vk | (modifiers & ~ModifierOf(vk));
    }
};
} // namespace ShortcutKeys
