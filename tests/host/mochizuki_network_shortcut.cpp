#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/MochizukiNetworkShortcut.h"
#include <cassert>
#include <cstdio>

int main()
{
    MochizukiNetworkShortcut key;
    assert(!key.Update(false, true, true, true).toggle); // plain F11
    assert(!key.Update(true, true, true, true).toggle);  // Ctrl pressed after F11
    assert(!key.Update(true, false, true, true).toggle);
    auto e = key.Update(true, true, true, true);
    assert(e.toggle && e.suppressF11);
    for (int i = 0; i < 100; ++i) assert(!key.Update(true, true, true, true).toggle);
    assert(key.Update(false, true, true, true).suppressF11);
    assert(key.Update(false, false, true, true).suppressF11); // consume release
    assert(!key.Update(false, false, true, true).suppressF11);
    assert(!key.Update(true, true, false, true).toggle); // other backend / focus
    assert(!key.Update(true, true, true, true).toggle); // no late toggle on focus
    key.Update(false, false, true, true);
    assert(!key.Update(true, true, true, false).toggle); // text / binding capture
    assert(!key.Update(true, true, true, true).toggle);
    key.Update(false, false, true, true);
    assert(key.Update(true, true, true, true).toggle);
    puts("MOCHIZUKI_SHORTCUT_OK");
}
