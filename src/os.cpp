#include "os.hpp"

#include <cstdint>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/utsname.h>
#include <unistd.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace linkbench {

std::filesystem::path executable_path() {
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    buf.resize(n);
    return std::filesystem::path(buf);
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(buf.data(), ec);
    return ec ? std::filesystem::path(buf.data()) : canonical;
#else
    std::error_code ec;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : path;
#endif
}

std::string host_name() {
#if defined(_WIN32)
    wchar_t buf[256] = {};
    DWORD n = 256;
    if (!GetComputerNameW(buf, &n)) return {};
    return wide_to_utf8(buf, static_cast<int>(n));
#else
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) != 0) return {};
    buf[sizeof(buf) - 1] = 0;
    return buf;
#endif
}

std::string kernel_name() {
#if defined(_WIN32)
    using RtlGetVersionPtr = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    auto* fn = reinterpret_cast<RtlGetVersionPtr>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (!fn || fn(&info) != 0) return "Windows";
    return "Windows " + std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) + "." +
           std::to_string(info.dwBuildNumber);
#else
    utsname name{};
    if (uname(&name) != 0) return {};
    return std::string(name.sysname) + " " + name.release;
#endif
}

}  // namespace linkbench
