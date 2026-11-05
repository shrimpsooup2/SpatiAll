#include "app/Ini.h"

#include "app/Settings.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <filesystem>

namespace app::ini {

std::wstring readString(const std::wstring& file, const wchar_t* section, const wchar_t* key) {
    std::wstring buf(512, L'\0');
    for (;;) {
        const DWORD n = GetPrivateProfileStringW(section, key, L"", buf.data(), static_cast<DWORD>(buf.size()),
                                                 file.c_str());
        // A truncated read returns size - 1; grow until the value fits.
        if (n + 1 < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

float readFloat(const std::wstring& file, const wchar_t* section, const wchar_t* key, float def, float lo, float hi) {
    const std::wstring s = readString(file, section, key);
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    const float v = std::wcstof(s.c_str(), &end);
    if (end == s.c_str() || !std::isfinite(v)) return def;
    return std::clamp(v, lo, hi);
}

int readInt(const std::wstring& file, const wchar_t* section, const wchar_t* key, int def, int lo, int hi) {
    const std::wstring s = readString(file, section, key);
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    const long v = std::wcstol(s.c_str(), &end, 10);
    if (end == s.c_str()) return def;
    return static_cast<int>(std::clamp<long>(v, lo, hi));
}

bool readBool(const std::wstring& file, const wchar_t* section, const wchar_t* key, bool def) {
    return readInt(file, section, key, def ? 1 : 0, 0, 1) != 0;
}

void put(std::wstring& sec, const std::wstring& key, const std::wstring& value) {
    sec += key;
    sec += L'=';
    sec += value;
    sec.push_back(L'\0');
}

void putFloat(std::wstring& sec, const std::wstring& key, float v) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.4f", v);
    put(sec, key, buf);
}

void putInt(std::wstring& sec, const std::wstring& key, int v) { put(sec, key, std::to_wstring(v)); }

bool writeSection(const std::wstring& file, const std::wstring& section, const std::wstring& body) {
    return WritePrivateProfileSectionW(section.c_str(), body.c_str(), file.c_str()) != FALSE;
}

void readStem(const std::wstring& file, const wchar_t* section, const std::wstring& prefix, engine::StemParams& p) {
    auto key = [&](const wchar_t* name) { return prefix + name; };
    p.azimuthDeg = readFloat(file, section, key(L"Azimuth").c_str(), p.azimuthDeg, -180.0f, 180.0f);
    p.elevationDeg = readFloat(file, section, key(L"Elevation").c_str(), p.elevationDeg, -45.0f, 90.0f);
    p.distance = readFloat(file, section, key(L"Distance").c_str(), p.distance, 0.3f, 5.0f);
    p.widthDeg = readFloat(file, section, key(L"Width").c_str(), p.widthDeg, 0.0f, 300.0f);
    p.gainDb = readFloat(file, section, key(L"Gain").c_str(), p.gainDb, -24.0f, 12.0f);
    p.mute = readBool(file, section, key(L"Mute").c_str(), p.mute);
    p.solo = readBool(file, section, key(L"Solo").c_str(), p.solo);
}

void putStem(std::wstring& sec, const std::wstring& prefix, const engine::StemParams& p) {
    putFloat(sec, prefix + L"Azimuth", p.azimuthDeg);
    putFloat(sec, prefix + L"Elevation", p.elevationDeg);
    putFloat(sec, prefix + L"Distance", p.distance);
    putFloat(sec, prefix + L"Width", p.widthDeg);
    putFloat(sec, prefix + L"Gain", p.gainDb);
    putInt(sec, prefix + L"Mute", p.mute ? 1 : 0);
    putInt(sec, prefix + L"Solo", p.solo ? 1 : 0);
}

std::wstring prepareFile(const wchar_t* name) {
    const std::wstring dir = Settings::directory();
    if (dir.empty()) return {};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::wstring file = dir + L"\\" + name;
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const WORD bom = 0xFEFF;
            DWORD written = 0;
            WriteFile(h, &bom, sizeof(bom), &written, nullptr);
            CloseHandle(h);
        }
    }
    return file;
}

void flush(const std::wstring& file) { WritePrivateProfileStringW(nullptr, nullptr, nullptr, file.c_str()); }

}  // namespace app::ini
