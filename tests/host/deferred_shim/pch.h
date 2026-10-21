#pragma once
// Minimal pch shim so the production DeferredRelease.cpp compiles in host
// contract tests without pulling the full OptiScaler precompiled header.
#include <cstdint>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <limits>
#include <functional>
#include <chrono>
