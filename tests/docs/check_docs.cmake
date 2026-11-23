# The documentation consistency checks: CTest test docs.consistency (label docs), run as
#
#   cmake -DPRAENORA_SOURCE_DIR=<source tree> -P check_docs.cmake
#
# It reads only the source tree, and nothing it checks needs the network:
#   1. every enumerator of praenora::ErrorCode (include/praenora/runtime/error.hpp) appears in
#      docs/specs/error-taxonomy.md;
#   2. every spec/schemas/*.json file name appears in docs/specs/protocol.md;
#   3. every docs/adr/ADR-*.md is linked from the index in docs/adr/README.md;
#   4. every relative Markdown link "](path)" in README.md, AGENTS.md and docs/**/*.md resolves to
#      an existing file or directory: the anchor (#...) is stripped, URLs (a scheme such as
#      https:) and links to an anchor of the same document are skipped, and so are fenced code
#      blocks and inline code spans, which hold no links. A code fence that a file never closes
#      is a problem, since it would hide the rest of the file from the check;
#   5. the open-questions register (docs/open-questions.md) has at least 33 rows.
# Every check runs, and each one that finds nothing to check (no error code, schema file, ADR,
# link or row) reports that as a problem rather than passing. Each problem is printed on its own
# line, naming the file and the missing item or the broken link (with its line); the script then
# fails, giving the number of problems.

cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PRAENORA_SOURCE_DIR)
    message(FATAL_ERROR "check_docs.cmake: set PRAENORA_SOURCE_DIR to the source tree")
endif()
set(root "${PRAENORA_SOURCE_DIR}")
set(problems 0)

# _problem(<text>...): reports one problem, the arguments joined.
function(_problem)
    set(text "")
    math(EXPR last "${ARGC} - 1")
    foreach(index RANGE ${last})
        string(APPEND text "${ARGV${index}}")
    endforeach()
    message("docs.consistency: ${text}")
    math(EXPR problems "${problems} + 1")
    set(problems "${problems}" PARENT_SCOPE)
endfunction()

# _read(<variable> <path relative to the source tree>): the file's text, or empty (and a problem)
# when the file does not exist.
function(_read variable relative)
    if(EXISTS "${root}/${relative}")
        file(READ "${root}/${relative}" text)
    else()
        set(text "")
        message("docs.consistency: ${relative}: the file does not exist")
        math(EXPR problems "${problems} + 1")
        set(problems "${problems}" PARENT_SCOPE)
    endif()
    set(${variable} "${text}" PARENT_SCOPE)
endfunction()

# _contains_word(<variable> <text> <word>): whether <word> appears in <text> with no letter,
# digit, '_' or '-' right before or after it, so that OK does not match inside LOOKUP. <word>
# holds only letters, digits, '_', '-' and '.'.
function(_contains_word variable text word)
    string(REPLACE "." "\\." pattern "${word}")
    if(text MATCHES "(^|[^A-Za-z0-9_-])${pattern}([^A-Za-z0-9_-]|$)")
        set(${variable} TRUE PARENT_SCOPE)
    else()
        set(${variable} FALSE PARENT_SCOPE)
    endif()
endfunction()

# Check 1: the error codes.
set(error_header "include/praenora/runtime/error.hpp")
set(taxonomy_doc "docs/specs/error-taxonomy.md")
_read(header_text "${error_header}")
_read(taxonomy_text "${taxonomy_doc}")
string(REGEX MATCH "enum class ErrorCode[^{]*{[^}]*}" error_enum "${header_text}")
string(REGEX MATCHALL "[A-Z][A-Z0-9_]* = [0-9]+" error_entries "${error_enum}")
list(LENGTH error_entries error_count)
if(error_count EQUAL 0)
    _problem("${error_header}: no enumerators of enum class ErrorCode found")
endif()
foreach(entry IN LISTS error_entries)
    string(REGEX REPLACE " = [0-9]+$" "" name "${entry}")
    _contains_word(found "${taxonomy_text}" "${name}")
    if(NOT found)
        _problem("${taxonomy_doc}: the error code ${name} (${error_header}) is missing")
    endif()
endforeach()
message("docs.consistency: check 1: ${error_count} ErrorCode names looked for in ${taxonomy_doc}")

# Check 2: the schema files.
set(protocol_doc "docs/specs/protocol.md")
_read(protocol_text "${protocol_doc}")
file(GLOB schema_files RELATIVE "${root}/spec/schemas" "${root}/spec/schemas/*.json")
list(SORT schema_files)
list(LENGTH schema_files schema_count)
if(schema_count EQUAL 0)
    _problem("spec/schemas: no *.json files found")
endif()
foreach(schema IN LISTS schema_files)
    _contains_word(found "${protocol_text}" "${schema}")
    if(NOT found)
        _problem("${protocol_doc}: the schema file spec/schemas/${schema} is missing")
    endif()
endforeach()
message("docs.consistency: check 2: ${schema_count} schema file names looked for in "
    "${protocol_doc}")

# Check 3: the ADR index.
set(adr_index "docs/adr/README.md")
_read(adr_index_text "${adr_index}")
file(GLOB adr_files RELATIVE "${root}/docs/adr" "${root}/docs/adr/ADR-*.md")
list(SORT adr_files)
list(LENGTH adr_files adr_count)
if(adr_count EQUAL 0)
    _problem("docs/adr: no ADR-*.md files found")
endif()
foreach(adr IN LISTS adr_files)
    string(FIND "${adr_index_text}" "](${adr})" position)
    if(position EQUAL -1)
        _problem("${adr_index}: the index does not link docs/adr/${adr}")
    endif()
endforeach()
message("docs.consistency: check 3: ${adr_count} ADRs looked for in the index of ${adr_index}")

# Check 4: relative links. The text is read line by line; backslashes, semicolons and square
# brackets are replaced by markers first, so that neither CMake's list splitting nor its bracket
# handling changes the lines.
file(GLOB_RECURSE doc_files RELATIVE "${root}" "${root}/docs/*.md")
list(SORT doc_files)
set(markdown_files README.md AGENTS.md ${doc_files})
set(link_count 0)
foreach(relative IN LISTS markdown_files)
    _read(text "${relative}")
    get_filename_component(directory "${root}/${relative}" DIRECTORY)
    string(REPLACE "\\" "<backslash>" text "${text}")
    string(REPLACE ";" "<semicolon>" text "${text}")
    string(REPLACE "[" "<open>" text "${text}")
    string(REPLACE "]" "<close>" text "${text}")
    string(REPLACE "\n" ";" lines "${text}")
    set(line_number 0)
    set(fence "")
    set(fence_line 0)
    foreach(line IN LISTS lines)
        math(EXPR line_number "${line_number} + 1")
        # Fenced code blocks: a fence of three or more backticks or tildes opens one, and a
        # fence of the same character closes it.
        if(line MATCHES "^[ ]*(```|~~~)")
            if(fence STREQUAL "")
                set(fence "${CMAKE_MATCH_1}")
                set(fence_line ${line_number})
            elseif(fence STREQUAL CMAKE_MATCH_1)
                set(fence "")
            endif()
            continue()
        endif()
        if(NOT fence STREQUAL "")
            continue()
        endif()
        string(REGEX REPLACE "`[^`]*`" "" line "${line}")
        string(REGEX MATCHALL "<close>\\([^) \t]+" links "${line}")
        foreach(link IN LISTS links)
            string(REGEX REPLACE "^<close>\\(" "" target "${link}")
            string(REPLACE "<backslash>" "\\" target "${target}")
            string(REPLACE "<semicolon>" ";" target "${target}")
            string(REPLACE "<open>" "[" target "${target}")
            string(REPLACE "<close>" "]" target "${target}")
            if(target MATCHES "^[A-Za-z][A-Za-z0-9+.-]*:" OR target MATCHES "^#")
                continue()
            endif()
            math(EXPR link_count "${link_count} + 1")
            string(REGEX REPLACE "#.*$" "" path "${target}")
            if(NOT EXISTS "${directory}/${path}")
                _problem("${relative}:${line_number}: the link target ${target} does not exist")
            endif()
        endforeach()
    endforeach()
    if(NOT fence STREQUAL "")
        _problem("${relative}:${fence_line}: the code fence opened here is never closed, so the "
            "rest of the file is not checked for links")
    endif()
endforeach()
list(LENGTH markdown_files markdown_count)
if(link_count EQUAL 0)
    _problem("no relative links found in README.md, AGENTS.md and docs/**/*.md")
endif()
message("docs.consistency: check 4: ${link_count} relative links checked in ${markdown_count} "
    "files")

# Check 5: the open-questions register.
set(register "docs/open-questions.md")
set(minimum_rows 33)
_read(register_text "${register}")
string(REGEX MATCHALL "\n\\| OQ-[0-9]+ \\|" rows "\n${register_text}")
list(LENGTH rows row_count)
if(row_count LESS minimum_rows)
    _problem("${register}: the register has ${row_count} rows; it needs at least "
        "${minimum_rows}")
endif()
message("docs.consistency: check 5: ${row_count} rows in the register of ${register}")

if(problems GREATER 0)
    message(FATAL_ERROR "docs.consistency: ${problems} problem(s) found")
endif()
message("docs.consistency: no problems found")
