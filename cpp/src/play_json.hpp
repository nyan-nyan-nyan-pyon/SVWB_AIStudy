#pragma once

#include <charconv>
#include <cstdint>
#include <map>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace svwb::play {

struct Json {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;
    std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double, std::string, Array, Object> value;

    Json() : value(nullptr) {}
    Json(bool data) : value(data) {}
    Json(int data) : value(static_cast<std::int64_t>(data)) {}
    Json(std::uint32_t data) : value(static_cast<std::uint64_t>(data)) {}
    Json(std::uint64_t data) : value(data) {}
    Json(double data) : value(data) {}
    Json(std::string data) : value(std::move(data)) {}
    Json(std::string_view data) : value(std::string(data)) {}
    Json(const char* data) : value(std::string(data)) {}
    Json(Array data) : value(std::move(data)) {}
    Json(Object data) : value(std::move(data)) {}
};

inline void write_string(std::ostream& out, std::string_view value) {
    constexpr std::string_view hex = "0123456789abcdef";
    out.put('"');
    for (const unsigned char c : value) {
        if (c == '\\' || c == '"') {
            out.put('\\');
            out.put(static_cast<char>(c));
        } else if (c < 0x20) {
            out << "\\u00" << hex[c >> 4] << hex[c & 15];
        } else {
            out.put(static_cast<char>(c));
        }
    }
    out.put('"');
}

inline void write_json(std::ostream& out, const Json& json) {
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Json::Object> || std::is_same_v<T, Json::Array>) {
            constexpr bool object = std::is_same_v<T, Json::Object>;
            out.put(object ? '{' : '[');
            bool first = true;
            for (const auto& item : value) {
                if (!first) {
                    out.put(',');
                }
                first = false;
                if constexpr (object) {
                    write_string(out, item.first);
                    out.put(':');
                    write_json(out, item.second);
                } else {
                    write_json(out, item);
                }
            }
            out.put(object ? '}' : ']');
        } else if constexpr (std::is_same_v<T, std::string>) {
            write_string(out, value);
        } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
            out << "null";
        } else if constexpr (std::is_same_v<T, bool>) {
            out << (value ? "true" : "false");
        } else {
            out << value;
        }
    }, json.value);
}

inline bool unsigned_integer(std::string_view input, std::uint64_t& number) {
    const auto end = input.data() + input.size();
    const auto result = std::from_chars(input.data(), end, number);
    return result.ec == std::errc{} && result.ptr == end;
}

// The play protocol and deck files use JSON with unsigned integer numbers.
class RequestParser {
public:
    explicit RequestParser(std::string_view input) : input_(input) {}

    Json::Object parse() {
        auto result = object(0);
        whitespace();
        if (!input_.empty()) fail();
        return result;
    }

private:
    Json::Object object(int depth) {
        Json::Object object;
        require('{');
        if (!take('}')) {
            do {
                auto key = string();
                require(':');
                if (!object.emplace(std::move(key), field(depth + 1)).second) {
                    fail();
                }
            } while (take(','));
            require('}');
        }
        return object;
    }

    Json field(int depth) {
        if (depth > 32) fail();
        whitespace();
        if (input_.starts_with('{')) return object(depth);
        if (take('[')) {
            Json::Array array;
            if (!take(']')) {
                do {
                    array.push_back(field(depth + 1));
                } while (take(','));
                require(']');
            }
            return array;
        }
        return scalar();
    }

    [[noreturn]] static void fail() { throw std::invalid_argument("JSONコマンドの形式が正しくありません。"); }

    void whitespace() {
        while (!input_.empty() && std::string_view(" \t\r\n").find(input_.front()) != std::string_view::npos) {
            input_.remove_prefix(1);
        }
    }

    bool take(char expected) {
        whitespace();
        return consume(expected);
    }

    bool consume(char expected) {
        if (input_.empty() || input_.front() != expected) {
            return false;
        }
        input_.remove_prefix(1);
        return true;
    }

    void require(char expected) {
        if (!take(expected)) {
            fail();
        }
    }

    unsigned hex_quad() {
        if (input_.size() < 4) {
            fail();
        }
        unsigned code = 0;
        const auto end = input_.data() + 4;
        const auto parsed = std::from_chars(input_.data(), end, code, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != end) {
            fail();
        }
        input_.remove_prefix(4);
        return code;
    }

    void unicode(std::string& result) {
        auto code = hex_quad();
        if (code >= 0xd800 && code <= 0xdbff) {
            if (!consume('\\') || !consume('u')) {
                fail();
            }
            const auto low = hex_quad();
            if (low < 0xdc00 || low > 0xdfff) {
                fail();
            }
            code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
        } else if (code >= 0xdc00 && code <= 0xdfff) {
            fail();
        }
        if (code < 0x80) {
            result.push_back(static_cast<char>(code));
            return;
        }
        const int tail = code < 0x800 ? 1 : code < 0x10000 ? 2 : 3;
        constexpr unsigned prefix[]{0, 0xc0, 0xe0, 0xf0};
        result.push_back(static_cast<char>(prefix[tail] | (code >> (tail * 6))));
        for (int i = tail - 1; i >= 0; --i) {
            result.push_back(static_cast<char>(0x80 | ((code >> (i * 6)) & 0x3f)));
        }
    }

    std::string string() {
        require('"');
        std::string result;
        while (!input_.empty()) {
            const char c = input_.front();
            input_.remove_prefix(1);
            if (c == '"') {
                return result;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                fail();
            }
            if (c != '\\') {
                result.push_back(c);
                continue;
            }
            if (consume('u')) {
                unicode(result);
                continue;
            }
            constexpr std::string_view escapes = "\"\\/bfnrt";
            constexpr std::string_view decoded = "\"\\/\b\f\n\r\t";
            const auto index = input_.empty() ? std::string_view::npos : escapes.find(input_.front());
            if (index == std::string_view::npos) {
                fail();
            }
            result.push_back(decoded[index]);
            input_.remove_prefix(1);
        }
        fail();
    }

    Json scalar() {
        whitespace();
        if (input_.starts_with('"')) {
            return string();
        }
        if (input_.starts_with("true")) {
            input_.remove_prefix(4);
            return true;
        }
        if (input_.starts_with("false")) {
            input_.remove_prefix(5);
            return false;
        }
        if (input_.starts_with("null")) {
            input_.remove_prefix(4);
            return {};
        }
        const auto count = input_.find_first_not_of("0123456789");
        const auto token = input_.substr(0, count);
        std::uint64_t number = 0;
        if (token.empty() || (token.size() > 1 && token.front() == '0') || !unsigned_integer(token, number)) {
            fail();
        }
        input_.remove_prefix(token.size());
        return number;
    }

    std::string_view input_;
};

} // namespace svwb::play
