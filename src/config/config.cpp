#include "mf/config/config.hpp"

#include "mf/core/errors.hpp"
#include "mf/util/fs.hpp"
#include "mf/util/strings.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace mf::config
{

    namespace
    {

        // Values that look like numbers/booleans become JSON scalars; comma separated
        // lists become arrays. Everything else stays a string.
        bool looksLikeArray(const std::string &text)
        {
            return text.find(',') != std::string::npos && text.find(' ') == std::string::npos;
        }

        std::string cleanupDottedPath(const std::string &raw)
        {
            return str::trim(raw);
        }

    } // namespace

    json::Value coerceScalar(const std::string &raw)
    {
        const std::string text = str::trim(raw);
        if (text.empty())
            return json::Value(std::string());

        if (const auto asBool = str::toBool(text))
            return json::Value(*asBool);
        if (text == "null")
            return json::Value();

        if (text.front() == '[')
        {
            if (const auto parsed = json::tryParse(text))
                return *parsed;
        }

        // Only treat as an integer when there is no decimal point, so that
        // "8080" becomes a number but "1.0.0" stays a version string.
        if (text.find('.') == std::string::npos)
        {
            if (const auto asInt = str::toInt(text))
                return json::Value(*asInt);
        }
        else if (const auto asDouble = str::toDouble(text))
        {
            // Guard against version strings like 1.0.0 which strtod would accept
            // partially - toDouble rejects trailing characters, so this is safe.
            return json::Value(*asDouble);
        }

        if (looksLikeArray(text))
        {
            json::Value array = json::Value::array();
            for (const auto &part : str::split(text, ','))
            {
                const std::string item = str::trim(part);
                if (!item.empty())
                    array.push(json::Value(item));
            }
            return array;
        }

        // Guard the "1.0.0" case explicitly: a value with two dots is a string.
        return json::Value(text);
    }

    const std::map<std::string, std::string> &environmentMap()
    {
        static const std::map<std::string, std::string> map = {
            {"MF_SYSTEM_NAME", "system.name"},
            {"MF_SYSPLEX", "system.sysplex"},
            {"MF_REGION", "system.region"},
            {"MF_TIMEZONE", "system.timezone"},
            {"MF_MAX_JOBS", "system.maxConcurrentJobs"},

            {"MF_HTTP_ENABLED", "http.enabled"},
            {"MF_HTTP_HOST", "http.host"},
            {"MF_HTTP_PORT", "http.port"},
            {"MF_HTTP_BODY_LIMIT", "http.bodyLimitBytes"},
            {"MF_HTTP_AUTH_REQUIRED", "http.auth.required"},
            {"MF_API_KEYS", "http.auth.apiKeys"},

            {"MF_TCP_ENABLED", "tcp.enabled"},
            {"MF_TCP_HOST", "tcp.host"},
            {"MF_TCP_TERMINAL_PORT", "tcp.terminalPort"},
            {"MF_TCP_DATA_PORT", "tcp.dataPort"},

            {"MF_DB_DRIVER", "database.driver"},
            {"MF_DB_FILE", "database.file"},
            {"MF_DB_AUTO_MIGRATE", "database.autoMigrate"},
            {"MF_DB_AUTO_SEED", "database.autoSeed"},

            {"MF_SPREADSHEET_ENGINE", "spreadsheet.engine"},
            {"MF_STORAGE_DATASETS", "storage.datasets"},
            {"MF_STORAGE_SPREADSHEETS", "storage.spreadsheets"},
            {"MF_STORAGE_UPLOADS", "storage.uploads"},

            {"MF_LOG_LEVEL", "logging.level"},
            {"MF_LOG_FORMAT", "logging.format"},
            {"MF_LOG_DIRECTORY", "logging.directory"},
            {"MF_LOG_CONSOLE", "logging.console"},

            {"MF_SESSION_TTL", "security.session.ttlMinutes"},
            {"MF_PASSWORD_MIN_LENGTH", "security.passwordPolicy.minLength"},
        };
        return map;
    }

    json::Value environmentOverrides(const std::map<std::string, std::string> &env)
    {
        json::Value overrides = json::Value::object();
        for (const auto &[envKey, path] : environmentMap())
        {
            const auto it = env.find(envKey);
            if (it == env.end() || it->second.empty())
                continue;
            json::setPath(overrides, path, coerceScalar(it->second));
        }
        return overrides;
    }

    CommandLine parseCommandLine(const std::vector<std::string> &argv)
    {
        CommandLine out;

        for (std::size_t i = 0; i < argv.size(); ++i)
        {
            const std::string &arg = argv[i];
            const auto next = [&]() -> std::string
            {
                if (i + 1 >= argv.size())
                    return std::string();
                return argv[++i];
            };

            if (arg == "--config")
            {
                out.configPath = next();
            }
            else if (arg == "--port")
            {
                json::setPath(out.overrides, "http.port", coerceScalar(next()));
            }
            else if (arg == "--host")
            {
                json::setPath(out.overrides, "http.host", json::Value(next()));
            }
            else if (arg == "--terminal-port")
            {
                json::setPath(out.overrides, "tcp.terminalPort", coerceScalar(next()));
            }
            else if (arg == "--data-port")
            {
                json::setPath(out.overrides, "tcp.dataPort", coerceScalar(next()));
            }
            else if (arg == "--db")
            {
                json::setPath(out.overrides, "database.file", json::Value(next()));
            }
            else if (arg == "--driver")
            {
                json::setPath(out.overrides, "database.driver", json::Value(str::lower(next())));
            }
            else if (arg == "--log-level")
            {
                json::setPath(out.overrides, "logging.level", json::Value(str::lower(next())));
            }
            else if (arg == "--log-format")
            {
                json::setPath(out.overrides, "logging.format", json::Value(str::lower(next())));
            }
            else if (arg == "--system-name")
            {
                json::setPath(out.overrides, "system.name", json::Value(next()));
            }
            else if (arg == "--no-http")
            {
                json::setPath(out.overrides, "http.enabled", json::Value(false));
            }
            else if (arg == "--no-tcp")
            {
                json::setPath(out.overrides, "tcp.enabled", json::Value(false));
            }
            else if (arg == "--tcp-only")
            {
                out.tcpOnly = true;
                json::setPath(out.overrides, "http.enabled", json::Value(false));
            }
            else if (arg == "--http-only")
            {
                out.httpOnly = true;
                json::setPath(out.overrides, "tcp.enabled", json::Value(false));
            }
        }

        return out;
    }

    std::filesystem::path projectRootFromExecutable()
    {
        // The binary lives in <root>/build/bin (or <root>/bin). Walk up until we
        // find CMakeLists.txt, falling back two levels.
        std::filesystem::path dir = fsutil::executableDirectory();
        std::filesystem::path probe = dir;
        for (int i = 0; i < 4; ++i)
        {
            if (fsutil::exists(probe / "CMakeLists.txt"))
                return probe;
            if (!probe.has_parent_path())
                break;
            probe = probe.parent_path();
        }
        // Fall back: assume <root>/build/bin
        return dir.parent_path().parent_path();
    }

    ValidationResult validate(const json::Value &config)
    {
        ValidationResult result;

        const auto isPort = [](const json::Value &value)
        {
            return value.isNumber() && value.isInteger() && value.asInt() >= 0 && value.asInt() <= 65535;
        };

        const json::Value &system = config["system"];
        if (!system.isObject() || system["name"].toString().empty())
        {
            result.errors.push_back("system.name is required");
        }
        if (system["maxConcurrentJobs"].isNumber() && system["maxConcurrentJobs"].asInt() < 1)
        {
            result.errors.push_back("system.maxConcurrentJobs must be >= 1");
        }

        const json::Value &http = config["http"];
        if (http["enabled"].toBool(true))
        {
            if (!isPort(http["port"]))
            {
                result.errors.push_back("http.port must be an integer 0-65535 (got " + http["port"].dumpCompact() + ")");
            }
        }

        const json::Value &tcp = config["tcp"];
        if (tcp["enabled"].toBool(true))
        {
            if (!tcp["terminalPort"].isNumber() || tcp["terminalPort"].asInt() < 1 || tcp["terminalPort"].asInt() > 65535)
            {
                result.errors.push_back("tcp.terminalPort must be an integer 1-65535");
            }
            if (!tcp["dataPort"].isNumber() || tcp["dataPort"].asInt() < 1 || tcp["dataPort"].asInt() > 65535)
            {
                result.errors.push_back("tcp.dataPort must be an integer 1-65535");
            }
            if (tcp["terminalPort"].toInt() == tcp["dataPort"].toInt())
            {
                result.errors.push_back("tcp.terminalPort and tcp.dataPort must differ");
            }
        }

        const std::string driver = config["database"]["driver"].toString("sqlite");
        if (driver != "sqlite" && driver != "memory")
        {
            result.errors.push_back("database.driver must be \"sqlite\" or \"memory\" (got " + driver + ")");
        }
        if (config["database"]["backup"]["retain"].toInt(10) < 1)
        {
            result.warnings.push_back("database.backup.retain is < 1 - no backups will be kept");
        }

        const std::string level = config["logging"]["level"].toString("info");
        if (level != "trace" && level != "debug" && level != "info" && level != "warn" && level != "error" && level != "silent")
        {
            result.errors.push_back("logging.level invalid (got " + level + ")");
        }

        if (config["http"]["auth"]["required"].toBool(false))
        {
            const json::Value &keys = config["http"]["auth"]["apiKeys"];
            if (!keys.isArray() || keys.empty())
            {
                result.warnings.push_back(
                    "http.auth.required is true but no apiKeys configured - all requests will be rejected");
            }
        }

        if (config["security"]["session"]["ttlMinutes"].toInt(30) <= 0)
        {
            result.errors.push_back("security.session.ttlMinutes must be > 0");
        }

        const std::string engine = config["spreadsheet"]["engine"].toString("excel");
        if (engine != "excel" && engine != "csv")
        {
            result.errors.push_back("spreadsheet.engine must be \"excel\" or \"csv\" (got " + engine + ")");
        }

        return result;
    }

    json::Value maskSecrets(const json::Value &config)
    {
        if (config.isArray())
        {
            json::Value out = json::Value::array();
            for (const auto &item : config.items())
                out.push(json::Value(maskSecrets(item)));
            return out;
        }
        if (config.isObject())
        {
            json::Value out = json::Value::object();
            for (const auto &[key, value] : config.fields())
            {
                const std::string lowered = str::lower(key);
                if (lowered == "apikeys" || lowered == "apikey" || lowered.find("password") != std::string::npos || lowered.find("secret") != std::string::npos || lowered.find("token") != std::string::npos)
                {
                    if (value.isArray())
                    {
                        json::Value masked = json::Value::array();
                        for (std::size_t i = 0; i < value.size(); ++i)
                            masked.push(json::Value("***"));
                        out.set(key, masked);
                    }
                    else
                    {
                        out.set(key, json::Value("***"));
                    }
                    continue;
                }
                out.set(key, maskSecrets(value));
            }
            return out;
        }
        return config;
    }

    Resolved load(const LoadOptions &options)
    {
        Resolved resolved;

        resolved.projectRoot = options.projectRoot.empty()
                                   ? projectRootFromExecutable()
                                   : options.projectRoot;

        const std::filesystem::path defaultFile = resolved.projectRoot / "config" / "default.json";
        const auto defaultText = fsutil::tryReadFile(defaultFile);
        if (!defaultText)
        {
            throw Error(ErrorCode::Configuration,
                        "Missing base configuration file: " + defaultFile.string());
        }

        try
        {
            resolved.values = json::parse(*defaultText);
        }
        catch (const std::exception &ex)
        {
            throw Error(ErrorCode::Configuration,
                        "Invalid JSON in " + defaultFile.string() + ": " + ex.what());
        }

        const CommandLine cli = parseCommandLine(options.argv);
        resolved.tcpOnly = cli.tcpOnly;
        resolved.httpOnly = cli.httpOnly;

        const std::optional<std::string> explicitPath = options.configPath
                                                            ? options.configPath
                                                            : cli.configPath;

        if (explicitPath)
        {
            const auto absolute = fsutil::resolveAgainst(resolved.projectRoot, *explicitPath);
            const auto text = fsutil::tryReadFile(absolute);
            if (!text)
                throw Error(ErrorCode::Configuration, "Config file not found: " + absolute.string());
            json::Value overlay;
            try
            {
                overlay = json::parse(*text);
            }
            catch (const std::exception &ex)
            {
                throw Error(ErrorCode::Configuration,
                            "Invalid JSON in " + absolute.string() + ": " + ex.what());
            }
            resolved.values = json::merge(resolved.values, overlay);
            resolved.sourceFile = absolute.string();
        }
        else
        {
            const std::filesystem::path localFile = resolved.projectRoot / "config" / "local.json";
            if (const auto text = fsutil::tryReadFile(localFile))
            {
                try
                {
                    resolved.values = json::merge(resolved.values, json::parse(*text));
                    resolved.sourceFile = localFile.string();
                }
                catch (const std::exception &ex)
                {
                    throw Error(ErrorCode::Configuration,
                                "Invalid JSON in " + localFile.string() + ": " + ex.what());
                }
            }
        }

        std::map<std::string, std::string> environment;
        if (options.environment)
        {
            environment = *options.environment;
        }
        else
        {
#if defined(_WIN32)
            // Environment access is handled by the caller on Windows; the CRT
            // provides _environ. We use std::getenv for the known keys instead.
#endif
            for (const auto &[key, _] : environmentMap())
            {
                if (const char *value = std::getenv(key.c_str()))
                    environment[key] = value;
            }
        }

        resolved.values = json::merge(resolved.values, environmentOverrides(environment));
        resolved.values = json::merge(resolved.values, cli.overrides);
        resolved.values = json::merge(resolved.values, options.overrides);

        const ValidationResult validation = validate(resolved.values);
        if (!validation.ok())
        {
            std::string message = "Configuration invalid:";
            for (const auto &problem : validation.errors)
                message += "\n  - " + problem;
            throw Error(ErrorCode::Configuration, message);
        }
        resolved.warnings = validation.warnings;

        // Make filesystem locations absolute.
        auto resolveValue = [&](const std::string &path, const std::string &fallback)
        {
            const std::string value = resolved.values.find(path).toString(fallback);
            if (value.empty())
                return;
            json::setPath(resolved.values, path,
                          json::Value(fsutil::resolveAgainst(resolved.projectRoot, value).string()));
        };

        resolveValue("database.file", "./data/mainframe.db");
        resolveValue("database.backup.directory", "./data/backups");
        resolveValue("storage.datasets", "./data/datasets");
        resolveValue("storage.temp", "./data/tmp");
        resolveValue("storage.spreadsheets", "./data/spreadsheets");
        resolveValue("storage.uploads", "./data/uploads");
        resolveValue("logging.directory", "./logs");

        return resolved;
    }

} // namespace mf::config
