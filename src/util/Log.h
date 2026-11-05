#pragma once
// Thread-safe logger: appends to %LOCALAPPDATA%\Spatiall\spatiall.log and keeps recent lines for the UI.

#include <string>
#include <vector>

namespace logx {

void init();
void write(const char* level, const std::string& msg);
std::vector<std::string> recent(size_t maxLines = 200);

inline void info(const std::string& m) { write("INFO", m); }
inline void warn(const std::string& m) { write("WARN", m); }
inline void error(const std::string& m) { write("ERROR", m); }

std::string hr(long hr);  // "0x8889000A (message)"
std::string narrow(const std::wstring& w);
std::wstring widen(const std::string& s);

}  // namespace logx
