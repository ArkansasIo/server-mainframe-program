// ---------------------------------------------------------------------------
// Mainframe Server System - JSON value, parser and serializer.
//
// A dependency free DOM: Json::Value holds null, bool, number, string, array
// or object. Ordering of object keys is preserved (insertion order) which
// keeps generated configuration and API responses readable.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mf::json {

class Value;

using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

enum class Type { Null, Bool, Number, String, Array, Object };

class ParseError : public std::runtime_error {
public:
    ParseError(const std::string& message, std::size_t line, std::size_t column)
        : std::runtime_error("JSON parse error at line " + std::to_string(line)
                             + ", column " + std::to_string(column) + ": " + message)
        , line_(line)
        , column_(column) {}

    std::size_t line() const noexcept { return line_; }
    std::size_t column() const noexcept { return column_; }

private:
    std::size_t line_;
    std::size_t column_;
};

class Value {
public:
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool v) : type_(Type::Bool), bool_(v) {}
    Value(double v) : type_(Type::Number), number_(v) {}
    Value(int v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Value(long v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Value(long long v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Value(unsigned v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Value(unsigned long v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Value(unsigned long long v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Value(const char* v) : type_(Type::String), string_(v ? v : "") {}
    Value(std::string v) : type_(Type::String), string_(std::move(v)) {}
    Value(std::string_view v) : type_(Type::String), string_(v) {}
    Value(Array v) : type_(Type::Array), array_(std::make_shared<Array>(std::move(v))) {}
    Value(Object v) : type_(Type::Object), object_(std::make_shared<Object>(std::move(v))) {}

    static Value array() { return Value(Array{}); }
    static Value object() { return Value(Object{}); }
    static Value array(std::initializer_list<Value> items) { return Value(Array(items)); }

    Type type() const noexcept { return type_; }
    bool isNull() const noexcept { return type_ == Type::Null; }
    bool isBool() const noexcept { return type_ == Type::Bool; }
    bool isNumber() const noexcept { return type_ == Type::Number; }
    bool isString() const noexcept { return type_ == Type::String; }
    bool isArray() const noexcept { return type_ == Type::Array; }
    bool isObject() const noexcept { return type_ == Type::Object; }
    bool isInteger() const noexcept { return isNumber() && number_ == static_cast<double>(static_cast<long long>(number_)); }

    const char* typeName() const noexcept;

    // -- scalar accessors (loud) -------------------------------------------
    bool asBool() const;
    double asNumber() const;
    long long asInt() const;
    const std::string& asString() const;

    // -- scalar accessors (safe) -------------------------------------------
    bool toBool(bool fallback = false) const { return isBool() ? bool_ : fallback; }
    double toNumber(double fallback = 0.0) const { return isNumber() ? number_ : fallback; }
    long long toInt(long long fallback = 0) const { return isNumber() ? static_cast<long long>(number_) : fallback; }
    std::string toString(const std::string& fallback = "") const {
        return isString() ? string_ : fallback;
    }

    // -- containers ---------------------------------------------------------
    Array& items();
    const Array& items() const;
    Object& fields();
    const Object& fields() const;

    std::size_t size() const noexcept;
    bool empty() const noexcept;

    void push(Value v);
    void set(const std::string& key, Value v);
    bool has(const std::string& key) const;
    void erase(const std::string& key);

    // Object lookup. Out-of-range access yields a shared null sentinel so
    // chained navigation (a["x"]["y"]) never crashes.
    Value& operator[](const std::string& key);
    const Value& operator[](const std::string& key) const;
    Value& operator[](std::size_t index);
    const Value& operator[](std::size_t index) const;

    Value& at(const std::string& key);
    const Value& at(const std::string& key) const;

    // Convenience: read a nested path with a fallback, never throws.
    const Value& find(std::string_view dottedPath) const;

    bool operator==(const Value& other) const;
    bool operator!=(const Value& other) const { return !(*this == other); }

    // -- serialization ------------------------------------------------------
    std::string dump(int indent = 0) const;
    std::string dumpCompact() const { return dump(0); }

private:
    void dumpInto(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::shared_ptr<Array> array_;
    std::shared_ptr<Object> object_;

    static const Value& nullSentinel();
    static Value& mutableNullSentinel();
};

// -- free functions ---------------------------------------------------------
Value parse(std::string_view text);
std::optional<Value> tryParse(std::string_view text);

// Dotted-path helpers shared with the configuration layer.
Value merge(Value base, const Value& overlay);
void setPath(Value& root, std::string_view dottedPath, Value value);
const Value& getPath(const Value& root, std::string_view dottedPath);

std::string escapeString(std::string_view input);

} // namespace mf::json
