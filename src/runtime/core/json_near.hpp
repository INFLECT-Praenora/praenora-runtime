// Comparison of JSON values with a numeric tolerance: the rule by which the contract checks
// compare module outputs with the expectations of test vectors. It is the one implementation of
// the rule: the tests' json_near() (tests/support/json_compare.hpp) forwards to it. Header-only,
// using only the standard library and nlohmann/json, so the tests' support library can use it
// without the rest of the runtime.
#ifndef PRAENORA_RUNTIME_CORE_JSON_NEAR_HPP
#define PRAENORA_RUNTIME_CORE_JSON_NEAR_HPP

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace praenora::detail {

namespace json_near_detail {

// A JSON-pointer reference token: "~" becomes "~0" and "/" becomes "~1".
inline std::string pointer_token(const std::string& key) {
    std::string token;
    for (const char c : key) {
        if (c == '~') {
            token += "~0";
        } else if (c == '/') {
            token += "~1";
        } else {
            token += c;
        }
    }
    return token;
}

// A value as a difference shows it: compact JSON, cut after 80 bytes.
inline std::string shown(const nlohmann::json& value) {
    std::string text = value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (text.size() > 80) {
        text = text.substr(0, 77) + "...";
    }
    return text;
}

class Comparison {
public:
    Comparison(double rel, double abs, std::string* where) : rel_(rel), abs_(abs), where_(where) {}

    bool near(const nlohmann::json& a, const nlohmann::json& b, const std::string& pointer) const {
        if (a.is_number() && b.is_number()) {
            const double x = a.get<double>();
            const double y = b.get<double>();
            if (x == y) {
                return true;
            }
            const double difference = std::fabs(x - y);
            const double scale = std::max(std::fabs(x), std::fabs(y));
            if (std::isfinite(difference) && difference <= std::max(abs_, rel_ * scale)) {
                return true;
            }
            return differ(pointer, shown(a) + " differs from " + shown(b));
        }
        if (a.type() != b.type()) {
            return differ(pointer, "a " + std::string(a.type_name()) + " (" + shown(a) +
                                       ") differs from a " + b.type_name() + " (" + shown(b) + ")");
        }
        if (a.is_object()) {
            for (const auto& entry : a.items()) {
                if (!b.contains(entry.key())) {
                    return differ(pointer + "/" + pointer_token(entry.key()), "only in the first");
                }
            }
            for (const auto& entry : b.items()) {
                if (!a.contains(entry.key())) {
                    return differ(pointer + "/" + pointer_token(entry.key()), "only in the second");
                }
            }
            for (const auto& entry : a.items()) {
                if (!near(entry.value(), b.at(entry.key()),
                          pointer + "/" + pointer_token(entry.key()))) {
                    return false;
                }
            }
            return true;
        }
        if (a.is_array()) {
            if (a.size() != b.size()) {
                return differ(pointer, std::to_string(a.size()) + " items differ from " +
                                           std::to_string(b.size()) + " items");
            }
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (!near(a[i], b[i], pointer + "/" + std::to_string(i))) {
                    return false;
                }
            }
            return true;
        }
        return a == b || differ(pointer, shown(a) + " differs from " + shown(b));
    }

private:
    bool differ(const std::string& pointer, const std::string& what) const {
        if (where_ != nullptr) {
            *where_ =
                (pointer.empty() ? std::string("(the whole document)") : pointer) + ": " + what;
        }
        return false;
    }

    double rel_, abs_;
    std::string* where_;
};

} // namespace json_near_detail

// True when `a` and `b` have the same structure and the same values, numbers within the
// tolerance: two numbers (integers and floats alike) match when
// |a - b| <= max(abs, rel * max(|a|, |b|)), so rel = abs = 0 asks for equal numbers. Objects must
// have the same keys and arrays the same length; strings, booleans and null must be equal. When
// they differ and `where` is not null, it receives the JSON pointer of the first difference (keys
// in sorted order) and what differs there, for example "/data/value: 1.5 differs from 1.25".
inline bool json_near(const nlohmann::json& a, const nlohmann::json& b, double rel, double abs,
                      std::string* where) {
    return json_near_detail::Comparison(rel, abs, where).near(a, b, "");
}

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_CORE_JSON_NEAR_HPP
