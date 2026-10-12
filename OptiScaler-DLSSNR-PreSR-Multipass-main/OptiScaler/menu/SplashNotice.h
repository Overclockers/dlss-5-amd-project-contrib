#pragma once

// Fixed splash lines: always shown under the random tip, never part of the tip
// rotation. tools/build/build-menu-font.py reads this file so the embedded menu
// font subset always carries these Chinese glyphs. Plain literals: the project
// compiles with /utf-8, so they are UTF-8 bytes (u8 would be char8_t in C++20).
namespace SplashNotice
{
inline constexpr const char* Chinese = "本软件永久免费，如付费购买请立即退款";
inline constexpr const char* English = "This software is free forever. If you paid for it, ask for a refund now.";
}
