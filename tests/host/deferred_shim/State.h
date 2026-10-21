#pragma once
// Minimal State shim for compiling the real misc/DeferredRelease.cpp in host
// tests. Only State::Instance().isShuttingDown is referenced by that TU.
class State
{
  public:
    bool isShuttingDown = false;

    static State& Instance()
    {
        static State instance;
        return instance;
    }
};
