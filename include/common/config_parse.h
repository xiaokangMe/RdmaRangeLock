#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace rrl {
inline std::uint64_t parse_unsigned(const std::string& value, const std::string& name) {
    std::uint64_t result = 0;
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument(name + ": expected an unsigned integer");
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::invalid_argument(name + ": integer out of range");
    return result;
}
inline double parse_real(const std::string& value, const std::string& name) {
    std::size_t used = 0;
    const double result = std::stod(value, &used);
    if (used != value.size() || !std::isfinite(result) || value.empty() ||
        value.find_first_not_of("+-0123456789.eE") != std::string::npos)
        throw std::invalid_argument(name + ": expected a finite decimal number");
    return result;
}

// Workload configs are flat JSON objects. Reject unsupported structures and
// malformed tokens rather than extracting a plausible prefix with a regex.
struct ConfigScalar {
    enum Kind { String, Number, Boolean, Null } kind;
    std::string text;
};
class FlatConfigParser {
public:
    explicit FlatConfigParser(const std::string& input) : input_(input) {}
    std::map<std::string, ConfigScalar> parse() {
        std::map<std::string, ConfigScalar> result;
        require('{');
        if (take('}')) { finish(); return result; }
        for (;;) {
            const auto key = string();
            require(':');
            auto scalar = value();
            if (!result.emplace(key, std::move(scalar)).second) error("duplicate key: " + key);
            if (take('}')) break;
            require(',');
        }
        finish();
        return result;
    }
private:
    [[noreturn]] void error(const std::string& message) const {
        throw std::invalid_argument("config byte " + std::to_string(pos_) + ": " + message);
    }
    void space() { while (pos_ < input_.size() && std::string(" \r\n\t").find(input_[pos_]) != std::string::npos) ++pos_; }
    bool take(char token) { space(); if (pos_ < input_.size() && input_[pos_] == token) { ++pos_; return true; } return false; }
    void require(char token) { if (!take(token)) error(std::string("expected ") + token); }
    void finish() { space(); if (pos_ != input_.size()) error("trailing input"); }
    std::string string() {
        require('"');
        std::string result;
        while (pos_ < input_.size()) {
            const unsigned char ch = input_[pos_++];
            if (ch == '"') return result;
            if (ch < 0x20) error("control character in string");
            if (ch != '\\') { result += static_cast<char>(ch); continue; }
            if (pos_ == input_.size()) error("unfinished escape");
            switch (input_[pos_++]) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'b': result += '\b'; break;
                case 'f': result += '\f'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                default: error("unsupported string escape (use UTF-8 text directly)");
            }
        }
        error("unterminated string");
    }
    ConfigScalar value() {
        space();
        if (pos_ == input_.size()) error("missing value");
        if (input_[pos_] == '"') return {ConfigScalar::String, string()};
        for (const auto* token : {"true", "false", "null"}) {
            const std::string literal(token);
            if (input_.compare(pos_, literal.size(), literal) == 0) {
                pos_ += literal.size();
                return {literal == "null" ? ConfigScalar::Null : ConfigScalar::Boolean, literal};
            }
        }
        const auto begin = pos_;
        if (input_[pos_] == '-') ++pos_;
        if (pos_ == input_.size()) error("unfinished number");
        if (input_[pos_] == '0') ++pos_;
        else digits();
        if (pos_ < input_.size() && input_[pos_] == '.') { ++pos_; digits(); }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            digits();
        }
        return {ConfigScalar::Number, input_.substr(begin, pos_ - begin)};
    }
    void digits() {
        const auto begin = pos_;
        while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        if (begin == pos_) error("expected a decimal number or scalar value");
    }
    const std::string& input_;
    std::size_t pos_{0};
};
} // namespace rrl
