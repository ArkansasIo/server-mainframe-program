// ---------------------------------------------------------------------------
// Project-wide error type. Every layer signals failure with mf::Error carrying
// a machine readable code plus an HTTP status where one applies, so the HTTP
// and terminal layers can translate without guessing.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace mf
{

    enum class ErrorCode
    {
        General,
        NotFound,
        Validation,
        Conflict,
        Unauthorized,
        Forbidden,
        Database,
        Configuration,
        Io,
        Network,
        Protocol,
        NotImplemented,
        Unavailable,
    };

    const char *errorCodeName(ErrorCode code);
    int errorHttpStatus(ErrorCode code);

    class Error : public std::runtime_error
    {
    public:
        Error(ErrorCode code, const std::string &message)
            : std::runtime_error(message), code_(code),
              httpStatus_(errorHttpStatus(code)) {}

        Error(ErrorCode code, const std::string &message, std::string details)
            : std::runtime_error(message), code_(code),
              httpStatus_(errorHttpStatus(code)), details_(std::move(details)) {}

        ErrorCode code() const noexcept { return code_; }
        const char *codeName() const noexcept { return errorCodeName(code_); }
        int httpStatus() const noexcept { return httpStatus_; }
        const std::optional<std::string> &details() const noexcept { return details_; }

        void setHttpStatus(int status) noexcept { httpStatus_ = status; }

        // JSON body matching the HTTP envelope: { code, message, details }
        std::string toJson() const;

    private:
        ErrorCode code_;
        int httpStatus_;
        std::optional<std::string> details_;
    };

    // -- convenience constructors ----------------------------------------------
    [[noreturn]] void throwNotFound(const std::string &what);
    [[noreturn]] void throwValidation(const std::string &what);
    [[noreturn]] void throwConflict(const std::string &what);
    [[noreturn]] void throwUnauthorized(const std::string &what);
    [[noreturn]] void throwForbidden(const std::string &what);
    [[noreturn]] void throwDatabase(const std::string &what);
    [[noreturn]] void throwConfig(const std::string &what);
    [[noreturn]] void throwIo(const std::string &what);
    [[noreturn]] void throwProtocol(const std::string &what);

    // Wrap an std::exception body, preserving mf::Error instances unchanged.
    template <typename Fn>
    auto guard(Fn &&fn, ErrorCode code, const std::string &what) -> decltype(fn())
    {
        try
        {
            return fn();
        }
        catch (const Error &)
        {
            throw;
        }
        catch (const std::exception &ex)
        {
            throw Error(code, what + ": " + ex.what());
        }
    }

} // namespace mf
