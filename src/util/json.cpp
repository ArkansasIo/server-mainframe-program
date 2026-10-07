#include "mf/util/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace mf::json
{

    namespace
    {

        inline void skipWhitespace(std::string_view text, std::size_t &pos)
        {
            while (pos < text.size())
            {
                const char c = text[pos];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                {
                    ++pos;
                }
                else
                {
                    break;
                }
            }
        }

        std::size_t lineOf(std::string_view text, std::size_t pos)
        {
            std::size_t line = 1;
            for (std::size_t i = 0; i < pos && i < text.size(); ++i)
            {
                if (text[i] == '\n')
                    ++line;
            }
            return line;
        }

        std::size_t columnOf(std::string_view text, std::size_t pos)
        {
            std::size_t column = 1;
            for (std::size_t i = 0; i < pos && i < text.size(); ++i)
            {
                if (text[i] == '\n')
                {
                    column = 1;
                }
                else
                {
                    ++column;
                }
            }
            return column;
        }

        [[noreturn]] void fail(std::string_view text, std::size_t pos, const std::string &message)
        {
            throw ParseError(message, lineOf(text, pos), columnOf(text, pos));
        }

        void appendUtf8(std::string &out, unsigned int codepoint)
        {
            if (codepoint <= 0x7F)
            {
                out.push_back(static_cast<char>(codepoint));
            }
            else if (codepoint <= 0x7FF)
            {
                out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
            else if (codepoint <= 0xFFFF)
            {
                out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
                out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
        }

        unsigned int parseHex4(std::string_view text, std::size_t &pos)
        {
            unsigned int value = 0;
            for (int i = 0; i < 4; ++i)
            {
                if (pos >= text.size())
                    fail(text, pos, "truncated \\u escape");
                const char c = text[pos++];
                value <<= 4;
                if (c >= '0' && c <= '9')
                    value |= static_cast<unsigned int>(c - '0');
                else if (c >= 'a' && c <= 'f')
                    value |= static_cast<unsigned int>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')
                    value |= static_cast<unsigned int>(c - 'A' + 10);
                else
                    fail(text, pos - 1, "invalid hex digit in \\u escape");
            }
            return value;
        }

        std::string parseString(std::string_view text, std::size_t &pos)
        {
            // caller guarantees text[pos] == '"'
            ++pos;
            std::string out;
            while (true)
            {
                if (pos >= text.size())
                    fail(text, pos, "unterminated string");
                const char c = text[pos++];
                if (c == '"')
                    break;
                if (c != '\\')
                {
                    out.push_back(c);
                    continue;
                }
                if (pos >= text.size())
                    fail(text, pos, "unterminated escape sequence");
                const char esc = text[pos++];
                switch (esc)
                {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u':
                {
                    unsigned int cp = parseHex4(text, pos);
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos + 1 < text.size() && text[pos] == '\\' && text[pos + 1] == 'u')
                    {
                        pos += 2;
                        const unsigned int low = parseHex4(text, pos);
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    fail(text, pos - 1, std::string("invalid escape \\") + esc);
                }
            }
            return out;
        }

        Value parseValue(std::string_view text, std::size_t &pos, int depth);

        Value parseNumber(std::string_view text, std::size_t &pos)
        {
            const std::size_t start = pos;
            if (pos < text.size() && (text[pos] == '-' || text[pos] == '+'))
                ++pos;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
                ++pos;
            if (pos < text.size() && text[pos] == '.')
            {
                ++pos;
                while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
                    ++pos;
            }
            if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E'))
            {
                ++pos;
                if (pos < text.size() && (text[pos] == '-' || text[pos] == '+'))
                    ++pos;
                while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
                    ++pos;
            }
            if (start == pos)
                fail(text, pos, "invalid number");
            const std::string token(text.substr(start, pos - start));
            try
            {
                return Value(std::stod(token));
            }
            catch (const std::exception &)
            {
                fail(text, start, "number out of range: " + token);
            }
        }

        Value parseArray(std::string_view text, std::size_t &pos, int depth)
        {
            ++pos; // consume '['
            Array items;
            skipWhitespace(text, pos);
            if (pos < text.size() && text[pos] == ']')
            {
                ++pos;
                return Value(std::move(items));
            }
            while (true)
            {
                items.push_back(parseValue(text, pos, depth + 1));
                skipWhitespace(text, pos);
                if (pos >= text.size())
                    fail(text, pos, "unterminated array");
                if (text[pos] == ',')
                {
                    ++pos;
                    skipWhitespace(text, pos);
                    continue;
                }
                if (text[pos] == ']')
                {
                    ++pos;
                    return Value(std::move(items));
                }
                fail(text, pos, "expected ',' or ']' in array");
            }
        }

        Value parseObject(std::string_view text, std::size_t &pos, int depth)
        {
            ++pos; // consume '{'
            Object fields;
            skipWhitespace(text, pos);
            if (pos < text.size() && text[pos] == '}')
            {
                ++pos;
                return Value(std::move(fields));
            }
            while (true)
            {
                skipWhitespace(text, pos);
                if (pos >= text.size() || text[pos] != '"')
                    fail(text, pos, "expected object key string");
                std::string key = parseString(text, pos);
                skipWhitespace(text, pos);
                if (pos >= text.size() || text[pos] != ':')
                    fail(text, pos, "expected ':' after object key");
                ++pos;
                fields[key] = parseValue(text, pos, depth + 1);
                skipWhitespace(text, pos);
                if (pos >= text.size())
                    fail(text, pos, "unterminated object");
                if (text[pos] == ',')
                {
                    ++pos;
                    continue;
                }
                if (text[pos] == '}')
                {
                    ++pos;
                    return Value(std::move(fields));
                }
                fail(text, pos, "expected ',' or '}' in object");
            }
        }

        Value parseLiteral(std::string_view text, std::size_t &pos)
        {
            auto match = [&](std::string_view literal)
            {
                return text.compare(pos, literal.size(), literal) == 0;
            };
            if (match("null"))
            {
                pos += 4;
                return Value();
            }
            if (match("true"))
            {
                pos += 4;
                return Value(true);
            }
            if (match("false"))
            {
                pos += 5;
                return Value(false);
            }
            fail(text, pos, "unexpected token");
        }

        Value parseValue(std::string_view text, std::size_t &pos, int depth)
        {
            if (depth > 64)
                fail(text, pos, "maximum nesting depth exceeded");
            skipWhitespace(text, pos);
            if (pos >= text.size())
                fail(text, pos, "unexpected end of input");

            switch (text[pos])
            {
            case '{':
                return parseObject(text, pos, depth);
            case '[':
                return parseArray(text, pos, depth);
            case '"':
                return Value(parseString(text, pos));
            case 't':
            case 'f':
            case 'n':
                return parseLiteral(text, pos);
            default:
                return parseNumber(text, pos);
            }
        }

        std::string numberToString(double value)
        {
            if (std::isnan(value) || std::isinf(value))
                return "null";
            if (value == static_cast<double>(static_cast<long long>(value)) && std::fabs(value) < 1e15)
            {
                long long asInt = static_cast<long long>(value);
                return std::to_string(asInt);
            }
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.10g", value);
            return std::string(buffer);
        }

    } // namespace

    const char *Value::typeName() const noexcept
    {
        switch (type_)
        {
        case Type::Null:
            return "null";
        case Type::Bool:
            return "boolean";
        case Type::Number:
            return "number";
        case Type::String:
            return "string";
        case Type::Array:
            return "array";
        case Type::Object:
            return "object";
        }
        return "unknown";
    }

    bool Value::asBool() const
    {
        if (!isBool())
            throw std::runtime_error(std::string("JSON value is not a boolean (got ") + typeName() + ")");
        return bool_;
    }

    double Value::asNumber() const
    {
        if (!isNumber())
            throw std::runtime_error(std::string("JSON value is not a number (got ") + typeName() + ")");
        return number_;
    }

    long long Value::asInt() const
    {
        if (!isNumber())
            throw std::runtime_error(std::string("JSON value is not a number (got ") + typeName() + ")");
        return static_cast<long long>(number_);
    }

    const std::string &Value::asString() const
    {
        if (!isString())
            throw std::runtime_error(std::string("JSON value is not a string (got ") + typeName() + ")");
        return string_;
    }

    Array &Value::items()
    {
        if (!isArray())
            throw std::runtime_error("JSON value is not an array");
        return *array_;
    }

    const Array &Value::items() const
    {
        if (!isArray())
            throw std::runtime_error("JSON value is not an array");
        return *array_;
    }

    Object &Value::fields()
    {
        if (!isObject())
            throw std::runtime_error("JSON value is not an object");
        return *object_;
    }

    const Object &Value::fields() const
    {
        if (!isObject())
            throw std::runtime_error("JSON value is not an object");
        return *object_;
    }

    std::size_t Value::size() const noexcept
    {
        if (isArray() && array_)
            return array_->size();
        if (isObject() && object_)
            return object_->size();
        return 0;
    }

    bool Value::empty() const noexcept
    {
        return size() == 0;
    }

    void Value::push(Value v)
    {
        if (type_ == Type::Null)
        {
            type_ = Type::Array;
            array_ = std::make_shared<Array>();
        }
        if (!isArray())
            throw std::runtime_error("JSON value is not an array");
        if (!array_)
            array_ = std::make_shared<Array>();
        array_->push_back(std::move(v));
    }

    void Value::set(const std::string &key, Value v)
    {
        if (type_ == Type::Null)
        {
            type_ = Type::Object;
            object_ = std::make_shared<Object>();
        }
        if (!isObject())
            throw std::runtime_error("JSON value is not an object");
        if (!object_)
            object_ = std::make_shared<Object>();
        (*object_)[key] = std::move(v);
    }

    bool Value::has(const std::string &key) const
    {
        return isObject() && object_ && object_->find(key) != object_->end();
    }

    void Value::erase(const std::string &key)
    {
        if (isObject() && object_)
            object_->erase(key);
    }

    const Value &Value::nullSentinel()
    {
        static const Value sentinel;
        return sentinel;
    }

    Value &Value::mutableNullSentinel()
    {
        static Value sentinel;
        return sentinel;
    }

    Value &Value::operator[](const std::string &key)
    {
        if (type_ == Type::Null)
        {
            type_ = Type::Object;
            object_ = std::make_shared<Object>();
        }
        if (!isObject())
            return mutableNullSentinel();
        if (!object_)
            object_ = std::make_shared<Object>();
        return (*object_)[key];
    }

    const Value &Value::operator[](const std::string &key) const
    {
        if (!isObject() || !object_)
            return nullSentinel();
        const auto it = object_->find(key);
        return it == object_->end() ? nullSentinel() : it->second;
    }

    Value &Value::operator[](std::size_t index)
    {
        if (!isArray() || !array_ || index >= array_->size())
            return mutableNullSentinel();
        return (*array_)[index];
    }

    const Value &Value::operator[](std::size_t index) const
    {
        if (!isArray() || !array_ || index >= array_->size())
            return nullSentinel();
        return (*array_)[index];
    }

    Value &Value::at(const std::string &key)
    {
        if (!isObject() || !object_)
            throw std::out_of_range("JSON object has no key: " + key);
        return object_->at(key);
    }

    const Value &Value::at(const std::string &key) const
    {
        if (!isObject() || !object_)
            throw std::out_of_range("JSON object has no key: " + key);
        return object_->at(key);
    }

    const Value &Value::find(std::string_view dottedPath) const
    {
        const Value *cursor = this;
        std::size_t start = 0;
        while (start <= dottedPath.size())
        {
            const std::size_t dot = dottedPath.find('.', start);
            const std::string_view segment = dottedPath.substr(
                start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
            cursor = &((*cursor)[std::string(segment)]);
            if (!cursor || cursor->isNull())
                return nullSentinel();
            if (dot == std::string_view::npos)
                break;
            start = dot + 1;
        }
        return *cursor;
    }

    bool Value::operator==(const Value &other) const
    {
        if (type_ != other.type_)
            return false;
        switch (type_)
        {
        case Type::Null:
            return true;
        case Type::Bool:
            return bool_ == other.bool_;
        case Type::Number:
            return number_ == other.number_;
        case Type::String:
            return string_ == other.string_;
        case Type::Array:
            return items() == other.items();
        case Type::Object:
            return fields() == other.fields();
        }
        return false;
    }

    void Value::dumpInto(std::string &out, int indent, int depth) const
    {
        const bool pretty = indent > 0;
        const std::string pad = pretty ? std::string(static_cast<std::size_t>(indent * (depth + 1)), ' ') : std::string();
        const std::string padEnd = pretty ? std::string(static_cast<std::size_t>(indent * depth), ' ') : std::string();

        switch (type_)
        {
        case Type::Null:
            out += "null";
            break;
        case Type::Bool:
            out += bool_ ? "true" : "false";
            break;
        case Type::Number:
            out += numberToString(number_);
            break;
        case Type::String:
            out += escapeString(string_);
            break;
        case Type::Array:
        {
            if (!array_ || array_->empty())
            {
                out += "[]";
                break;
            }
            out += '[';
            for (std::size_t i = 0; i < array_->size(); ++i)
            {
                if (i)
                    out += ',';
                if (pretty)
                {
                    out += '\n';
                    out += pad;
                }
                (*array_)[i].dumpInto(out, indent, depth + 1);
            }
            if (pretty)
            {
                out += '\n';
                out += padEnd;
            }
            out += ']';
            break;
        }
        case Type::Object:
        {
            if (!object_ || object_->empty())
            {
                out += "{}";
                break;
            }
            out += '{';
            bool first = true;
            for (const auto &[key, value] : *object_)
            {
                if (!first)
                    out += ',';
                first = false;
                if (pretty)
                {
                    out += '\n';
                    out += pad;
                }
                out += escapeString(key);
                out += pretty ? ": " : ":";
                value.dumpInto(out, indent, depth + 1);
            }
            if (pretty)
            {
                out += '\n';
                out += padEnd;
            }
            out += '}';
            break;
        }
        }
    }

    std::string Value::dump(int indent) const
    {
        std::string out;
        out.reserve(256);
        dumpInto(out, indent, 0);
        return out;
    }

    Value parse(std::string_view text)
    {
        std::size_t pos = 0;
        Value value = parseValue(text, pos, 0);
        skipWhitespace(text, pos);
        if (pos != text.size())
            fail(text, pos, "trailing characters after JSON value");
        return value;
    }

    std::optional<Value> tryParse(std::string_view text)
    {
        try
        {
            return parse(text);
        }
        catch (const std::exception &)
        {
            return std::nullopt;
        }
    }

    std::string escapeString(std::string_view input)
    {
        std::string out;
        out.reserve(input.size() + 2);
        out.push_back('"');
        for (const char c : input)
        {
            switch (c)
            {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
                    out += buffer;
                }
                else
                {
                    out.push_back(c);
                }
            }
        }
        out.push_back('"');
        return out;
    }

    void setPath(Value &root, std::string_view dottedPath, Value value)
    {
        if (!root.isObject())
            root = Value::object();
        Value *cursor = &root;
        std::size_t start = 0;
        while (start <= dottedPath.size())
        {
            const std::size_t dot = dottedPath.find('.', start);
            const bool last = dot == std::string_view::npos;
            const std::string key(dottedPath.substr(start, last ? std::string_view::npos : dot - start));
            if (last)
            {
                cursor->set(key, std::move(value));
                return;
            }
            Value &child = (*cursor)[key];
            if (!child.isObject())
                child = Value::object();
            cursor = &child;
            start = dot + 1;
        }
    }

    const Value &getPath(const Value &root, std::string_view dottedPath)
    {
        return root.find(dottedPath);
    }

    Value merge(Value base, const Value &overlay)
    {
        if (overlay.isNull())
            return base;
        if (!base.isObject() || !overlay.isObject())
        {
            return overlay;
        }
        for (const auto &[key, value] : overlay.fields())
        {
            if (value.isObject() && base[key].isObject())
            {
                base.set(key, merge(base[key], value));
            }
            else
            {
                base.set(key, value);
            }
        }
        return base;
    }

} // namespace mf::json
