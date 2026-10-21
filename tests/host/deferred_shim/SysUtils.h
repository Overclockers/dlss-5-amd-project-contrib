#pragma once
// Minimal logging shim for compiling the real misc/DeferredRelease.cpp in host
// tests. The product SysUtils.h pulls spdlog and the whole logging subsystem.
#define LOG_DEBUG(msg, ...) ((void) 0)
#define LOG_WARN(msg, ...) ((void) 0)
#define LOG_ERROR(msg, ...) ((void) 0)
