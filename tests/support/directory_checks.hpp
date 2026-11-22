// Checks of the directories that the files of a run name: a path written as the placeholder of the
// directory it lies in, the forms of a directory (as given, and with its symbolic links resolved),
// and a check that a JSON document names none of them. Header-only; uses only the standard
// library, nlohmann/json and GoogleTest, so white-box and black-box tests alike can use it.
#ifndef PRAENORA_TESTS_SUPPORT_DIRECTORY_CHECKS_HPP
#define PRAENORA_TESTS_SUPPORT_DIRECTORY_CHECKS_HPP

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace praenora::test {

// `path` as UTF-8, the way the runtime spells paths.
inline std::string path_utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// `path`, which lies in `directory`, as `placeholder` followed by the rest of the path:
// named_path("<output>", "/runs/out", "/runs/out/run-record.json") is "<output>/run-record.json".
inline std::string named_path(const std::string& placeholder,
                              const std::filesystem::path& directory,
                              const std::filesystem::path& path) {
    return placeholder + path_utf8(path).substr(path_utf8(directory).size());
}

// Each of `directories` as given (lexically normal) and, where it differs, with its symbolic links
// resolved (std::filesystem::weakly_canonical()).
inline std::vector<std::string>
directory_forms(const std::vector<std::filesystem::path>& directories) {
    std::vector<std::string> forms;
    for (const std::filesystem::path& directory : directories) {
        const std::filesystem::path normal = directory.lexically_normal();
        forms.push_back(path_utf8(normal));
        std::error_code ec;
        const std::filesystem::path resolved = std::filesystem::weakly_canonical(directory, ec);
        if (!ec && resolved != normal) {
            forms.push_back(path_utf8(resolved));
        }
    }
    return forms;
}

// Adds a test failure for every string of `document`, object keys included, that holds one of
// `forms`; `pointer` is where `document` lies, for the messages.
inline void expect_names_none(const nlohmann::json& document, const std::vector<std::string>& forms,
                              const std::string& pointer = "") {
    const auto check = [&](const std::string& where, const std::string& text) {
        for (const std::string& form : forms) {
            EXPECT_EQ(text.find(form), std::string::npos)
                << where << ": '" << text << "' names " << form;
        }
    };
    if (document.is_string()) {
        check(pointer, document.get<std::string>());
    } else if (document.is_object()) {
        for (auto it = document.begin(); it != document.end(); ++it) {
            check(pointer + " (a key)", it.key());
            expect_names_none(it.value(), forms, pointer + "/" + it.key());
        }
    } else if (document.is_array()) {
        for (std::size_t i = 0; i < document.size(); ++i) {
            expect_names_none(document[i], forms, pointer + "/" + std::to_string(i));
        }
    }
}

} // namespace praenora::test

#endif // PRAENORA_TESTS_SUPPORT_DIRECTORY_CHECKS_HPP
