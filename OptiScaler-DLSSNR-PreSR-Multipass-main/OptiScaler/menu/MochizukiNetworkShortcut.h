#pragma once

// The chord owns F11 through release, even if Ctrl is released first. Plain F11
// remains available to configured shortcuts; holds and text/key capture do not toggle.
class MochizukiNetworkShortcut
{
    bool wasDown = false, owned = false;
public:
    struct Event { bool toggle, suppressF11; };
    Event Update(bool control, bool f11, bool eligible, bool accept)
    {
        const bool toggle = control && f11 && !wasDown && eligible && accept;
        owned |= toggle;
        const Event result {toggle, owned};
        wasDown = f11;
        if (!f11) owned = false;
        return result;
    }
};
