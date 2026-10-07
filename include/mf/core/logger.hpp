// ---------------------------------------------------------------------------
// Structured logger. Writes coloured text or newline-delimited JSON to the
// console and always appends to <logging.directory>/<logging.file>, rotating
// once maxFileBytes is exceeded.
//
// Thread safe: internal mutex guards the sink and the rotation bookkeeping.
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "mf/util/json.hpp"

namespace mf
{

    enum class LogLevel
    {
        Trace = 10,
        Debug = 20,
        Info = 30,
        Warn = 40,
        Error = 50,
        Silent = 100
    };

    LogLevel parseLogLevel(std::string_view name, LogLevel fallback = LogLevel::Info);
    const char *logLevelName(LogLevel level);

    struct LogOptions
    {
        LogLevel level = LogLevel::Info;
        std::string format = "text"; // text | json
        bool console = true;
        std::string directory; // empty == no file sink
        std::string file = "mainframe.log";
        std::size_t maxFileBytes = 5 * 1024 * 1024;
        bool colorize = true;
    };

    class Logger
    {
    public:
        Logger();
        explicit Logger(const LogOptions &options);

        // Convenience: derive from a resolved configuration tree.
        static Logger fromConfig(const json::Value &config);

        void setLevel(LogLevel level);
        LogLevel level() const;
        void setConsole(bool enabled);

        bool enabled(LogLevel level) const;

        void trace(std::string_view message, const json::Value &meta = json::Value());
        void debug(std::string_view message, const json::Value &meta = json::Value());
        void info(std::string_view message, const json::Value &meta = json::Value());
        void warn(std::string_view message, const json::Value &meta = json::Value());
        void error(std::string_view message, const json::Value &meta = json::Value());

        void log(LogLevel level, std::string_view message, const json::Value &meta = json::Value());

        // A logger that prefixes every record with the given bindings.
        Logger child(const json::Value &bindings) const;

        void flush();

    private:
        void write(LogLevel level, std::string_view message, const json::Value &meta);
        std::string textLine(LogLevel level, std::string_view message, const json::Value &meta) const;
        void rotateIfNeeded();

        // Shared so that Logger remains copyable (child() returns by value).
        struct State {
            mutable std::mutex mutex;
            LogLevel level = LogLevel::Info;
            std::string format = "text";
            bool console = true;
            bool colorize = true;
            std::string filePath;
            std::size_t maxFileBytes = 5 * 1024 * 1024;
            std::size_t bytesWritten = 0;
            json::Value bindings = json::Value::object();
        };
        std::shared_ptr<State> state_;
        std::string filePath_;
    };

    // Discard everything - used by unit tests.
    Logger makeSilentLogger();

    // Redact secret-looking fields from a JSON tree before logging.
    json::Value redactSecrets(const json::Value &value);

} // namespace mf
