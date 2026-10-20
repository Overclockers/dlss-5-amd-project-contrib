#pragma once
#include <stdexcept>

namespace AmdPreSr
{
// Module retention protects HIP registrations; only a completed InitPass permits reuse.
class PassInitialization
{
    bool initialized = false;
public:
    void RequireReady() const
    {
        if (!initialized)
            throw std::runtime_error("AMD pass initialization previously failed; restart the game");
    }
    void Complete() noexcept { initialized = true; }
};
}
