// ---------------------------------------------------------------------------
// Filesystem helpers. Everything goes through std::filesystem with a thin
// error-reporting wrapper that throws mf::Error instead of leaking
// std::filesystem_error to callers.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace mf::fsutil
{

    namespace fs = std::filesystem;

    bool exists(const fs::path &path);
    bool isFile(const fs::path &path);
    bool isDirectory(const fs::path &path);

    // Create every missing parent directory. No-op when it already exists.
    void ensureDirectory(const fs::path &path);
    void ensureParentDirectory(const fs::path &path);

    // Read/write whole files.
    std::string readFile(const fs::path &path);
    std::optional<std::string> tryReadFile(const fs::path &path);
    void writeFile(const fs::path &path, std::string_view contents);
    void appendFile(const fs::path &path, std::string_view contents);

    std::uintmax_t fileSize(const fs::path &path);

    std::vector<fs::path> listFiles(const fs::path &dir, bool recursive = false);
    std::vector<fs::path> listFilesWithExtension(const fs::path &dir, std::string_view extension);

    bool remove(const fs::path &path);
    bool rename(const fs::path &from, const fs::path &to);

    // Copy a file, creating parent directories.
    void copyFile(const fs::path &from, const fs::path &to);

    // Resolve a config-relative path against the project root.
    fs::path resolveAgainst(const fs::path &root, const std::string &value);

    // Extract the base name without extension ("a/b/c.txt" -> "c").
    std::string stem(const fs::path &path);
    std::string extension(const fs::path &path);

    // Return a path that does not yet exist by appending -1, -2, ...
    fs::path uniquePath(const fs::path &desired);

    // Timestamped file name suitable for backups: mainframe-20261007-140322.db
    std::string timestampedName(std::string_view stem, std::string_view extension);

    // Directory this executable lives in (used to locate resources/ and public/).
    fs::path executableDirectory();

} // namespace mf::fsutil
