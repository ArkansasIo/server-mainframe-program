#include "mf/util/fs.hpp"

#include "mf/core/errors.hpp"
#include "mf/util/strings.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace mf::fsutil
{

    bool exists(const fs::path &path)
    {
        std::error_code ec;
        return fs::exists(path, ec);
    }

    bool isFile(const fs::path &path)
    {
        std::error_code ec;
        return fs::is_regular_file(path, ec);
    }

    bool isDirectory(const fs::path &path)
    {
        std::error_code ec;
        return fs::is_directory(path, ec);
    }

    void ensureDirectory(const fs::path &path)
    {
        if (path.empty())
            return;
        std::error_code ec;
        if (fs::exists(path, ec))
        {
            if (!fs::is_directory(path, ec))
            {
                throw Error(ErrorCode::Io, "Path exists and is not a directory: " + path.string());
            }
            return;
        }
        fs::create_directories(path, ec);
        if (ec)
            throw Error(ErrorCode::Io, "Cannot create directory " + path.string() + ": " + ec.message());
    }

    void ensureParentDirectory(const fs::path &path)
    {
        ensureDirectory(path.parent_path());
    }

    std::string readFile(const fs::path &path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            throw Error(ErrorCode::Io, "Cannot open file for reading: " + path.string());
        std::string contents;
        stream.seekg(0, std::ios::end);
        const auto size = stream.tellg();
        if (size > 0)
            contents.reserve(static_cast<std::size_t>(size));
        stream.seekg(0, std::ios::beg);
        contents.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        return contents;
    }

    std::optional<std::string> tryReadFile(const fs::path &path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return std::nullopt;
        std::string contents;
        stream.seekg(0, std::ios::end);
        const auto size = stream.tellg();
        if (size > 0)
            contents.reserve(static_cast<std::size_t>(size));
        stream.seekg(0, std::ios::beg);
        contents.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        return contents;
    }

    void writeFile(const fs::path &path, std::string_view contents)
    {
        ensureParentDirectory(path);
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
            throw Error(ErrorCode::Io, "Cannot open file for writing: " + path.string());
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream)
            throw Error(ErrorCode::Io, "Write failed: " + path.string());
    }

    void appendFile(const fs::path &path, std::string_view contents)
    {
        ensureParentDirectory(path);
        std::ofstream stream(path, std::ios::binary | std::ios::app);
        if (!stream)
            throw Error(ErrorCode::Io, "Cannot open file for appending: " + path.string());
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    std::uintmax_t fileSize(const fs::path &path)
    {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        return ec ? 0 : size;
    }

    std::vector<fs::path> listFiles(const fs::path &dir, bool recursive)
    {
        std::vector<fs::path> out;
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            return out;

        if (recursive)
        {
            for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec))
            {
                if (ec)
                    break;
                if (it->is_regular_file(ec))
                    out.push_back(it->path());
            }
        }
        else
        {
            for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec))
            {
                if (ec)
                    break;
                if (it->is_regular_file(ec))
                    out.push_back(it->path());
            }
        }

        std::sort(out.begin(), out.end());
        return out;
    }

    std::vector<fs::path> listFilesWithExtension(const fs::path &dir, std::string_view extension)
    {
        std::vector<fs::path> out;
        const std::string wanted = str::lower(std::string(extension));
        for (const auto &file : listFiles(dir))
        {
            if (str::lower(file.extension().string()) == wanted)
                out.push_back(file);
        }
        return out;
    }

    bool remove(const fs::path &path)
    {
        std::error_code ec;
        return fs::remove(path, ec) && !ec;
    }

    bool rename(const fs::path &from, const fs::path &to)
    {
        std::error_code ec;
        ensureParentDirectory(to);
        fs::rename(from, to, ec);
        if (ec)
        {
            // Cross-device rename: fall back to copy + delete.
            fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
            if (ec)
                return false;
            fs::remove(from, ec);
            return !ec;
        }
        return true;
    }

    void copyFile(const fs::path &from, const fs::path &to)
    {
        ensureParentDirectory(to);
        std::error_code ec;
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (ec)
            throw Error(ErrorCode::Io, "Copy failed " + from.string() + " -> " + to.string() + ": " + ec.message());
    }

    fs::path resolveAgainst(const fs::path &root, const std::string &value)
    {
        if (value.empty())
            return root;
        fs::path candidate(value);
        if (candidate.is_absolute())
            return candidate.lexically_normal();
        return (root / candidate).lexically_normal();
    }

    std::string stem(const fs::path &path)
    {
        return path.stem().string();
    }

    std::string extension(const fs::path &path)
    {
        return path.extension().string();
    }

    fs::path uniquePath(const fs::path &desired)
    {
        if (!fsutil::exists(desired))
            return desired;
        const fs::path parent = desired.parent_path();
        const std::string base = desired.stem().string();
        const std::string ext = desired.extension().string();
        for (int i = 1; i < 10000; ++i)
        {
            fs::path candidate = parent / (base + "-" + std::to_string(i) + ext);
            if (!fsutil::exists(candidate))
                return candidate;
        }
        throw Error(ErrorCode::Io, "Cannot find a unique path for " + desired.string());
    }

    std::string timestampedName(std::string_view stemValue, std::string_view extensionValue)
    {
        const std::time_t tt = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &tt);
#else
        gmtime_r(&tt, &tm);
#endif
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%s-%04d%02d%02d-%02d%02d%02d%s",
                      std::string(stemValue).c_str(),
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec,
                      std::string(extensionValue).c_str());
        return std::string(buffer);
    }

    fs::path executableDirectory()
    {
#if defined(_WIN32)
        char buffer[MAX_PATH];
        const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
        if (length > 0)
            return fs::path(std::string(buffer, length)).parent_path();
#endif
        std::error_code ec;
        return fs::current_path(ec);
    }

} // namespace mf::fsutil
