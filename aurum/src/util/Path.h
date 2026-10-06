#pragma once

#include <filesystem>
#include <string>

namespace aurum {

// Paths are carried around as UTF-8 std::strings. On Linux that is what the
// file system takes anyway; on Windows a narrow std::string is read in the
// ANSI code page by std::filesystem and the fstreams, which breaks on any
// non-ASCII user or folder name. Every path that reaches the file system goes
// through these two.
inline std::filesystem::path toPath(const std::string& utf8)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

inline std::string fromPath(const std::filesystem::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

// With '/' separators on every platform: for names shown in the browser and
// keys stored in the settings (favourites), which must not depend on the OS.
inline std::string fromPathGeneric(const std::filesystem::path& p)
{
    const std::u8string s = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

} // namespace aurum
