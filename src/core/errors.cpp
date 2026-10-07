#include "mf/core/errors.hpp"

#include "mf/util/json.hpp"

namespace mf
{

    const char *errorCodeName(ErrorCode code)
    {
        switch (code)
        {
        case ErrorCode::General:
            return "MAINFRAME_ERROR";
        case ErrorCode::NotFound:
            return "NOT_FOUND";
        case ErrorCode::Validation:
            return "VALIDATION_ERROR";
        case ErrorCode::Conflict:
            return "CONFLICT";
        case ErrorCode::Unauthorized:
            return "UNAUTHORIZED";
        case ErrorCode::Forbidden:
            return "FORBIDDEN";
        case ErrorCode::Database:
            return "DATABASE_ERROR";
        case ErrorCode::Configuration:
            return "CONFIG_ERROR";
        case ErrorCode::Io:
            return "IO_ERROR";
        case ErrorCode::Network:
            return "NETWORK_ERROR";
        case ErrorCode::Protocol:
            return "PROTOCOL_ERROR";
        case ErrorCode::NotImplemented:
            return "NOT_IMPLEMENTED";
        case ErrorCode::Unavailable:
            return "SERVICE_UNAVAILABLE";
        }
        return "MAINFRAME_ERROR";
    }

    int errorHttpStatus(ErrorCode code)
    {
        switch (code)
        {
        case ErrorCode::NotFound:
            return 404;
        case ErrorCode::Validation:
            return 400;
        case ErrorCode::Conflict:
            return 409;
        case ErrorCode::Unauthorized:
            return 401;
        case ErrorCode::Forbidden:
            return 403;
        case ErrorCode::NotImplemented:
            return 501;
        case ErrorCode::Unavailable:
            return 503;
        case ErrorCode::General:
        case ErrorCode::Database:
        case ErrorCode::Configuration:
        case ErrorCode::Io:
        case ErrorCode::Network:
        case ErrorCode::Protocol:
            return 500;
        }
        return 500;
    }

    std::string Error::toJson() const
    {
        json::Value object = json::Value::object();
        object.set("code", json::Value(std::string(codeName())));
        object.set("message", json::Value(std::string(what())));
        if (details_)
            object.set("details", json::Value(*details_));
        return object.dump();
    }

    void throwNotFound(const std::string &what) { throw Error(ErrorCode::NotFound, what); }
    void throwValidation(const std::string &what) { throw Error(ErrorCode::Validation, what); }
    void throwConflict(const std::string &what) { throw Error(ErrorCode::Conflict, what); }
    void throwUnauthorized(const std::string &what) { throw Error(ErrorCode::Unauthorized, what); }
    void throwForbidden(const std::string &what) { throw Error(ErrorCode::Forbidden, what); }
    void throwDatabase(const std::string &what) { throw Error(ErrorCode::Database, what); }
    void throwConfig(const std::string &what) { throw Error(ErrorCode::Configuration, what); }
    void throwIo(const std::string &what) { throw Error(ErrorCode::Io, what); }
    void throwProtocol(const std::string &what) { throw Error(ErrorCode::Protocol, what); }

} // namespace mf
