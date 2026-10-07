#include "mf/core/logger.hpp"

#include "mf/util/fs.hpp"
#include "mf/util/strings.hpp"
#include "mf/util/time.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <sstream>

namespace mf
{

    namespace
    {
        constexpr const char *RESET = "\x1b[0m";
        constexpr const char *DIM = "\x1b[2m";

        const char *colorFor(LogLevel level)
        {
            switch (level)
            {
            case LogLevel::Trace:
                return "\x1b[90m";
            case LogLevel::Debug:
                return "\x1b[36m";
            case LogLevel::Info:
                return "\x1b[32m";
            case LogLevel::Warn:
                return "\x1b[33m";
            case LogLevel::Error:
                return "\x1b[31m";
            case LogLevel::Silent:
                return "";
            }
            return "";
        }

        bool isSecretKey(std::string_view key)
        {
            static const std::vector<std::string> needles = {
                "password", "passwd", "secret", "token", "apikey", "api_key", "authorization", "credential"};
            const std::string lowered = str::lower(key);
            for (const auto &needle : needles)
            {
                if (lowered.find(needle) != std::string::npos)
                    return true;
            }
            return false;
        }
    } // namespace

    LogLevel parseLogLevel(std::string_view name, LogLevel fallback)
    {
        const std::string lowered = str::lower(str::trim(name));
        if (lowered == "trace")
            return LogLevel::Trace;
        if (lowered == "debug")
            return LogLevel::Debug;
        if (lowered == "info")
            return LogLevel::Info;
        if (lowered == "warn" || lowered == "warning")
            return LogLevel::Warn;
        if (lowered == "error")
            return LogLevel::Error;
        if (lowered == "silent" || lowered == "off")
            return LogLevel::Silent;
        return fallback;
    }

    const char *logLevelName(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Trace:
            return "trace";
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Error:
            return "error";
        case LogLevel::Silent:
            return "silent";
        }
        return "info";
    }

    json::Value redactSecrets(const json::Value &value)
    {
        if (value.isArray())
        {
            json::Value out = json::Value::array();
            for (const auto &item : value.items())
                out.push(json::Value(redactSecrets(item)));
            return out;
        }
        if (value.isObject())
        {
            json::Value out = json::Value::object();
            for (const auto &[key, item] : value.fields())
            {
                out.set(key, isSecretKey(key) ? json::Value("***") : redactSecrets(item));
            }
            return out;
        }
        return value;
    }

    Logger::Logger() : Logger(LogOptions{}) {}

    Logger::Logger(const LogOptions &options)
        : state_(std::make_shared<State>())
    {
        state_->level = options.level;
        state_->format = options.format;
        state_->console = options.console;
        state_->colorize = options.colorize && options.format == "text";
        state_->maxFileBytes = options.maxFileBytes > 0 ? options.maxFileBytes : (5 * 1024 * 1024);
        if (!options.directory.empty())
        {
            filePath_ = (fsutil::fs::path(options.directory) / options.file).string();
            state_->filePath = filePath_;
            try
            {
                fsutil::ensureDirectory(fsutil::fs::path(options.directory));
                rotateIfNeeded();
                state_->bytesWritten = static_cast<std::size_t>(fsutil::fileSize(filePath_));
            }
            catch (...)
            {
                filePath_.clear();
                state_->filePath.clear();
            }
        }
    }

    Logger Logger::fromConfig(const json::Value &config)
    {
        LogOptions options;
        const auto &logging = config["logging"];
        options.level = parseLogLevel(logging["level"].toString("info"), LogLevel::Info);
        options.format = logging["format"].toString("text");
        options.console = logging["console"].toBool(true);
        options.directory = logging["directory"].toString("");
        options.file = logging["file"].toString("mainframe.log");
        options.maxFileBytes = static_cast<std::size_t>(logging["maxFileBytes"].toInt(5 * 1024 * 1024));
        options.colorize = logging["colorize"].toBool(true);
        return Logger(options);
    }

    Logger makeSilentLogger()
    {
        LogOptions options;
        options.level = LogLevel::Silent;
        options.console = false;
        return Logger(options);
    }

    void Logger::setLevel(LogLevel level)
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->level = level;
    }

    LogLevel Logger::level() const
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->level;
    }

    void Logger::setConsole(bool enabled)
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->console = enabled;
    }

    bool Logger::enabled(LogLevel level) const
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return static_cast<int>(level) >= static_cast<int>(state_->level);
    }

    Logger Logger::child(const json::Value &bindings) const
    {
        Logger copy(*this);
        std::lock_guard<std::mutex> lock(copy.state_->mutex);
        if (bindings.isObject())
        {
            if (!copy.state_->bindings.isObject())
                copy.state_->bindings = json::Value::object();
            for (const auto &[key, value] : bindings.fields())
                copy.state_->bindings.set(key, value);
        }
        return copy;
    }

    void Logger::trace(std::string_view message, const json::Value &meta) { log(LogLevel::Trace, message, meta); }
    void Logger::debug(std::string_view message, const json::Value &meta) { log(LogLevel::Debug, message, meta); }
    void Logger::info(std::string_view message, const json::Value &meta) { log(LogLevel::Info, message, meta); }
    void Logger::warn(std::string_view message, const json::Value &meta) { log(LogLevel::Warn, message, meta); }
    void Logger::error(std::string_view message, const json::Value &meta) { log(LogLevel::Error, message, meta); }

    void Logger::log(LogLevel level, std::string_view message, const json::Value &meta)
    {
        if (level == LogLevel::Silent)
            return;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (static_cast<int>(level) < static_cast<int>(state_->level))
                return;
        }
        write(level, message, meta);
    }

    std::string Logger::textLine(LogLevel level, std::string_view message, const json::Value &meta) const
    {
        const bool color = state_->colorize;
        const char *colorCode = color ? colorFor(level) : "";
        const char *reset = color ? RESET : "";
        const char *dim = color ? DIM : "";

        std::string levelText = str::upper(logLevelName(level));
        while (levelText.size() < 5)
            levelText.push_back(' ');

        std::string line = std::string(dim) + timeutil::nowIso() + reset + " " + colorCode + levelText + reset + " " + std::string(message);

        std::string context;
        if (state_->bindings.isObject())
        {
            for (const auto &[key, value] : state_->bindings.fields())
            {
                if (!context.empty())
                    context += " ";
                context += key + "=";
                context += value.isString() ? value.asString() : value.dumpCompact();
            }
        }
        if (!context.empty())
            line += std::string(" ") + dim + "(" + context + ")" + reset;

        if (meta.isObject() && !meta.empty())
        {
            line += std::string(" ") + dim + redactSecrets(meta).dumpCompact() + reset;
        }
        return line;
    }

    void Logger::write(LogLevel level, std::string_view message, const json::Value &meta)
    {
        std::lock_guard<std::mutex> lock(state_->mutex);

        json::Value record = json::Value::object();
        record.set("ts", json::Value(timeutil::nowIso()));
        record.set("level", json::Value(std::string(logLevelName(level))));
        record.set("message", json::Value(std::string(message)));
        if (state_->bindings.isObject())
        {
            for (const auto &[key, value] : state_->bindings.fields())
                record.set(key, value);
        }
        if (meta.isObject() && !meta.empty())
            record.set("meta", redactSecrets(meta));

        const bool fileRecord = state_->format == "json" || state_->filePath.empty();

        if (state_->console)
        {
            const std::string line = (state_->format == "json") ? record.dump() : textLine(level, message, meta);
            std::ostream &out = (level == LogLevel::Error || level == LogLevel::Warn) ? std::cerr : std::cout;
            out << line << '\n';
        }

        if (!state_->filePath.empty())
        {
            const std::string payload = record.dump() + "\n";
            state_->bytesWritten += payload.size();
            if (state_->bytesWritten > state_->maxFileBytes)
            {
                rotateIfNeeded();
                state_->bytesWritten = 0;
            }
            try
            {
                fsutil::appendFile(state_->filePath, payload);
            }
            catch (...)
            {
                // logging must never throw into callers
            }
        }
    }

    void Logger::rotateIfNeeded()
    {
        if (filePath_.empty() || !fsutil::exists(filePath_))
            return;
        if (static_cast<std::size_t>(fsutil::fileSize(filePath_)) < state_->maxFileBytes)
            return;

        const fsutil::fs::path path(filePath_);
        const std::string rotated = fsutil::timestampedName(path.stem().string(), path.extension().string());
        fsutil::rename(path, path.parent_path() / rotated);

        // Retain the five most recent rotations.
        auto rotations = fsutil::listFiles(path.parent_path());
        std::vector<fsutil::fs::path> matching;
        const std::string prefix = path.stem().string() + "-";
        for (const auto &file : rotations)
        {
            if (str::startsWith(file.filename().string(), prefix))
                matching.push_back(file);
        }
        std::sort(matching.begin(), matching.end());
        if (matching.size() > 5)
        {
            for (std::size_t i = 0; i + 5 < matching.size(); ++i)
                fsutil::remove(matching[i]);
        }
    }

    void Logger::flush()
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        std::cout.flush();
        std::cerr.flush();
    }

} // namespace mf
