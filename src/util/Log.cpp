#include "util/Log.h"

#include <windows.h>
#include <shlobj.h>

#include <share.h>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>

namespace logx {
namespace {
std::mutex g_mu;
std::deque<std::string> g_lines;
FILE* g_file = nullptr;
constexpr size_t kKeep = 1000;
}  // namespace

void init() {
    std::lock_guard lk(g_mu);
    if (g_file) return;
    PWSTR base = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        std::filesystem::path dir = std::filesystem::path(base) / L"Spatiall";
        CoTaskMemFree(base);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        // Shared read access so the log can be viewed while the app runs.
        g_file = _wfsopen((dir / L"spatiall.log").c_str(), L"w", _SH_DENYWR);
    }
}

void write(const char* level, const std::string& msg) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char prefix[48];
    std::snprintf(prefix, sizeof(prefix), "%02d:%02d:%02d.%03d %-5s ", st.wHour, st.wMinute, st.wSecond,
                  st.wMilliseconds, level);
    std::string line = prefix + msg;
    std::lock_guard lk(g_mu);
    if (g_file) {
        std::fputs(line.c_str(), g_file);
        std::fputc('\n', g_file);
        std::fflush(g_file);
    }
    OutputDebugStringA((line + "\n").c_str());
    g_lines.push_back(std::move(line));
    if (g_lines.size() > kKeep) g_lines.pop_front();
}

std::vector<std::string> recent(size_t maxLines) {
    std::lock_guard lk(g_mu);
    const size_t n = g_lines.size() < maxLines ? g_lines.size() : maxLines;
    return {g_lines.end() - static_cast<std::ptrdiff_t>(n), g_lines.end()};
}

std::string hr(long code) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(code));
    std::string out = buf;
    wchar_t* msg = nullptr;
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr)) {
        std::string m = narrow(msg);
        LocalFree(msg);
        while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ')) m.pop_back();
        out += " (" + m + ")";
    }
    return out;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

}  // namespace logx
