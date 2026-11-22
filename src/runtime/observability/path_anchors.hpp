// The directories a run knows, each named by a placeholder, and their replacement in the error
// records of the files that a run writes for others to read: the run record and the status file
// (docs/specs/observability.md#known-directories-in-the-files-of-a-run). What stays in the process
// keeps absolute paths: thrown errors, RunSummary, Workflow::status() and status_json(),
// StatusSnapshot::to_json() and the logs.
#ifndef PRAENORA_RUNTIME_OBSERVABILITY_PATH_ANCHORS_HPP
#define PRAENORA_RUNTIME_OBSERVABILITY_PATH_ANCHORS_HPP

#include "discovery/search_paths.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace praenora::detail {

// The directories of one run, by the placeholders that name them:
//   output         <output>
//   configuration  <configuration> (the directory of the configuration file)
//   modules        <modules:ORIGIN:INDEX>, or <modules:installation> for the installation's
//                  directory: a search path named as execution.manifests names it, ORIGIN being
//                  to_string(SearchPath::origin) and INDEX SearchPath::index
//   providers      <providers:ORIGIN:INDEX>, <providers:installation>
//   home           <home> (the user's home directory, home_directory())
struct RunDirectories {
    std::optional<std::filesystem::path> output;
    std::filesystem::path configuration;
    std::vector<SearchPath> modules;   // in search order
    std::vector<SearchPath> providers; // in search order
    std::optional<std::filesystem::path> home;
};

// The anchors of a run: each directory of RunDirectories in its lexical, normalized form (without
// a trailing separator) and, when it differs, in its weakly_canonical() form (symbolic links
// resolved), both giving the directory's placeholder. A root directory ("/", "C:\") and an empty
// or relative path are never anchors. Immutable once built, so any thread may use it.
class PathAnchors {
public:
    explicit PathAnchors(const RunDirectories& directories);

    // `text` with every occurrence of an anchor at a whole-path boundary replaced by its
    // placeholder, the rest of the path kept ("<output>/run-record.json"). An occurrence is at a
    // boundary when it is preceded by the start of the text or by a character that does not
    // continue a path segment, and followed by the end of the text, a separator, a character that
    // does not continue a path segment, or a run of '.' that is followed by the end of the text or
    // by a character that neither continues a segment nor is a separator ("in /home/ann." gives
    // "in <home>."; "/home/ann.d" and "/home/ann./x" are other paths). Letters, digits, '.', '_',
    // '-', '~', bytes outside ASCII and separators continue a segment; every other character
    // (quotes, whitespace, '(', ')', ',', ':', ';', '=', '[', ']', ...) does not. On macOS, an
    // occurrence that follows /System/Volumes/Preboot/Cryptexes/<one segment> directly, that prefix
    // itself at such a boundary, is at a boundary too, and the prefix is kept: dyld names a library
    // it could not open a second time behind that prefix. Where several anchors occur at one place,
    // the longest wins; of anchors of the same directory, the first in this order: output,
    // configuration, module search paths, provider search paths (each in search order), home. The
    // text is scanned from left to right, and replaced text is never scanned again. On Windows '/'
    // and '\' are both separators, either matching the other, and letters are compared without
    // regard to ASCII case.
    std::string apply(std::string_view text) const;

    // apply() on the parts of `record` (an error record in JSON) that can name files: its
    // message, the file of its location, and every string of its details and of its cause, however
    // deeply nested. Its name, code, tick and time, the pointer of its location, object keys and
    // values that are not strings stay as they are.
    void apply_to_error_record(nlohmann::json& record) const;

    // apply_to_error_record() on the error records of a status-snapshot document:
    // workflow.last_error and the last_error of every step, when they are objects. Nothing else in
    // the snapshot changes.
    void apply_to_status_snapshot(nlohmann::json& snapshot) const;

private:
    struct Anchor {
        std::string text;        // the directory, UTF-8
        std::string placeholder; // what replaces it
    };

    // Adds `directory` (both forms) with `placeholder`, unless it is no anchor; a form that an
    // earlier anchor already has keeps that anchor's placeholder.
    void add(const std::filesystem::path& directory, const std::string& placeholder);

    std::vector<Anchor> anchors_; // longest first; anchors of equal length in precedence order
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_OBSERVABILITY_PATH_ANCHORS_HPP
