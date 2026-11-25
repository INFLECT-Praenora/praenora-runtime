# praenora_python_directory_digest(<out_var> ROOT <dir> (FILES <file>... | GLOB_ROOT <dir>))
#
# Computes the digest of the Python sources of a module or a provider: 64 lowercase hexadecimal
# digits, the SHA-256 of a listing with one line for each file,
#
#   <SHA-256 of the file's bytes, 64 lowercase hexadecimal digits>  <path>\n
#
# (two spaces; the path relative to ROOT with / separators, in UTF-8; the line ends in a line feed),
# the lines sorted by the bytes of the path. A file below a directory named __pycache__ is left out
# at any depth. The runtime computes the same digest from the files it finds (docs/specs/
# python-service.md).
#
# FILES names the files, each absolute or relative to ROOT; a file named twice counts once.
# GLOB_ROOT takes every regular file below a directory instead (the directory's own path is
# relative to nothing: give it absolute, and ROOT the directory the paths are relative to, usually
# the same one). A path with a semicolon is refused: CMake lists cannot hold it, and so is a
# symbolic link, to a file or a directory, among the files or on the way to one (the digest is
# defined over regular files only; a link outside a __pycache__ directory stops the configuration).
#
# This file is installed with the CMake package of Praenora and has no other dependency.

include_guard(GLOBAL)

function(praenora_python_directory_digest out_var)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "ROOT;GLOB_ROOT" "FILES")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "praenora_python_directory_digest: unknown arguments ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arg_ROOT)
        message(FATAL_ERROR "praenora_python_directory_digest: ROOT is required")
    endif()
    if(NOT arg_GLOB_ROOT AND NOT DEFINED arg_FILES)
        message(FATAL_ERROR
            "praenora_python_directory_digest: give FILES or GLOB_ROOT, to name the files")
    endif()
    get_filename_component(root "${arg_ROOT}" ABSOLUTE)

    set(files ${arg_FILES})
    set(globbed "")
    if(arg_GLOB_ROOT)
        get_filename_component(glob_root "${arg_GLOB_ROOT}" ABSOLUTE)
        # Directories are listed too, so that a link to a directory is seen by the link check;
        # the directories themselves are skipped below (only for these entries: a directory named
        # in FILES is refused).
        file(GLOB_RECURSE globbed LIST_DIRECTORIES true "${glob_root}/*")
        list(APPEND files ${globbed})
    endif()

    # Paths relative to ROOT, without __pycache__ files, sorted by bytes, each once.
    set(paths "")
    foreach(file IN LISTS files)
        get_filename_component(absolute "${file}" ABSOLUTE BASE_DIR "${root}")
        file(RELATIVE_PATH relative "${root}" "${absolute}")
        if(relative MATCHES "^\\.\\./" OR relative STREQUAL "")
            message(FATAL_ERROR
                "praenora_python_directory_digest: ${file} is not below ${root}")
        endif()
        if(relative MATCHES ";")
            message(FATAL_ERROR
                "praenora_python_directory_digest: the path ${relative} holds a semicolon")
        endif()
        if(relative MATCHES "(^|/)__pycache__/")
            continue()
        endif()
        # A symbolic link is refused: the one at the path, and any on the way to it.
        set(prefix "${root}")
        string(REPLACE "/" ";" components "${relative}")
        foreach(component IN LISTS components)
            string(APPEND prefix "/${component}")
            if(IS_SYMLINK "${prefix}")
                message(FATAL_ERROR "praenora_python_directory_digest: ${prefix} is a symbolic "
                    "link; the digest covers regular files only, so remove or replace the link")
            endif()
        endforeach()
        if(IS_DIRECTORY "${absolute}" AND file IN_LIST globbed)
            continue()
        endif()
        list(APPEND paths "${relative}")
    endforeach()
    list(REMOVE_DUPLICATES paths)
    list(SORT paths COMPARE STRING CASE SENSITIVE)

    set(listing "")
    foreach(relative IN LISTS paths)
        if(NOT EXISTS "${root}/${relative}" OR IS_DIRECTORY "${root}/${relative}")
            message(FATAL_ERROR
                "praenora_python_directory_digest: ${root}/${relative} is not a file")
        endif()
        file(SHA256 "${root}/${relative}" file_digest)
        string(APPEND listing "${file_digest}  ${relative}\n")
    endforeach()
    string(SHA256 digest "${listing}")
    set(${out_var} "${digest}" PARENT_SCOPE)
endfunction()
