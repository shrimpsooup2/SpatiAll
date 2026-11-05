#pragma once
// Small helpers over the Win32 profile API for Spatiall's UTF-16 ini files.

#include "engine/Engine.h"

#include <string>

namespace app::ini {

std::wstring readString(const std::wstring& file, const wchar_t* section, const wchar_t* key);
float readFloat(const std::wstring& file, const wchar_t* section, const wchar_t* key, float def, float lo, float hi);
int readInt(const std::wstring& file, const wchar_t* section, const wchar_t* key, int def, int lo, int hi);
bool readBool(const std::wstring& file, const wchar_t* section, const wchar_t* key, bool def);

// Section bodies for WritePrivateProfileSectionW: "key=value\0key=value\0" (+ the terminating \0 of c_str()).
void put(std::wstring& sec, const std::wstring& key, const std::wstring& value);
void putFloat(std::wstring& sec, const std::wstring& key, float v);
void putInt(std::wstring& sec, const std::wstring& key, int v);
bool writeSection(const std::wstring& file, const std::wstring& section, const std::wstring& body);

// Stem/speaker parameters as `<prefix>Azimuth`, `<prefix>Elevation`, ... keys.
void readStem(const std::wstring& file, const wchar_t* section, const std::wstring& prefix, engine::StemParams& p);
void putStem(std::wstring& sec, const std::wstring& prefix, const engine::StemParams& p);

// Creates %LOCALAPPDATA%\Spatiall and, if missing, `name` there with a UTF-16 BOM: the profile API only writes
// UTF-16 into files that already start with one (else it uses ANSI). Returns the path, empty when unavailable.
std::wstring prepareFile(const wchar_t* name);
// Flushes the profile cache so a crash right after writing keeps the file.
void flush(const std::wstring& file);

}  // namespace app::ini
