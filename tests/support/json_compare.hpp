// Comparison of JSON documents with a numeric tolerance, for outputs checked against golden files
// or expected results. The rule is the runtime's, the one the contract checks use: this forwards
// to its header-only implementation (core/json_near.hpp). This header uses only the standard
// library and nlohmann/json, so black-box tests can use it too.
#ifndef PRAENORA_TESTS_SUPPORT_JSON_COMPARE_HPP
#define PRAENORA_TESTS_SUPPORT_JSON_COMPARE_HPP

#include <nlohmann/json.hpp>

#include <string>

namespace praenora::test {

// True when `a` and `b` have the same structure and the same values, numbers within the
// tolerance: two numbers (integers and floats alike) match when
// |a - b| <= max(abs, rel * max(|a|, |b|)), so rel = abs = 0 asks for equal numbers. Objects must
// have the same keys and arrays the same length; strings, booleans and null must be equal. When
// they differ and `where` is not null, it receives the JSON pointer of the first difference (keys
// in sorted order) and what differs there, for example "/data/value: 1.5 differs from 1.25".
bool json_near(const nlohmann::json& a, const nlohmann::json& b, double rel, double abs,
               std::string* where);

} // namespace praenora::test

#endif // PRAENORA_TESTS_SUPPORT_JSON_COMPARE_HPP
