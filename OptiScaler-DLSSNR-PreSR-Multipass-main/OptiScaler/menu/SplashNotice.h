#pragma once

// Free-distribution notice / 免费声明
// EN: The author asks that this notice be kept. This project is distributed free
// of charge. Removing or altering it in order to sell or resell the software goes
// against the author's wishes and the distribution terms; please do not help
// with such changes.
// 中文：这是作者要求保留的免费声明。本项目免费分发。删除或修改它以便收费、
// 转售，违反作者意愿和分发条款；请勿协助此类修改。

// Fixed splash lines: shown right under the menu-key hint, above the random tip; never part of the tip
// rotation. tools/build/build-menu-font.py reads this file so the embedded menu
// font subset always carries these Chinese glyphs. Plain literals: the project
// compiles with /utf-8, so they are UTF-8 bytes (u8 would be char8_t in C++20).
namespace SplashNotice
{
inline constexpr const char* Chinese = "本软件永久免费，如付费购买请立即退款";
inline constexpr const char* English = "This software is free forever. If you paid for it, ask for a refund now.";
// Main menu title bar, top right. English key; the Chinese text
// "免费使用，谨防上当" lives in MenuStrings.inl and is translated at draw time.
inline constexpr const char* TitleBar = "Free. Don't buy.";
}
