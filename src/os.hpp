#pragma once

#include <filesystem>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace linkbench {

std::filesystem::path executable_path();
std::string host_name();
std::string kernel_name();

#if defined(_WIN32)
inline std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (chars <= 0) return {};
    std::wstring out(static_cast<std::size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), chars);
    return out;
}

inline std::string wide_to_utf8(const wchar_t* text, int chars) {
    if (!text || chars == 0) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, chars, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, chars, out.data(), bytes, nullptr, nullptr);
    while (!out.empty() && (out.back() == '\0' || out.back() == ' ' || out.back() == '\r' || out.back() == '\n')) out.pop_back();
    return out;
}

inline std::string wide_to_utf8(const std::wstring& text) {
    return wide_to_utf8(text.c_str(), static_cast<int>(text.size()));
}
#endif

}  // namespace linkbench
