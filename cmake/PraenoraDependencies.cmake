# praenora_find_dependencies()
#
# Makes the third-party dependencies available. It is a macro, so the variables and imported
# targets it creates stay in the caller's scope.
#
# - By default every dependency comes from find_package() in CONFIG mode, at the minimum versions
#   below, and nothing is downloaded.
# - With PRAENORA_FETCH_DEPENDENCIES=ON, FetchContent first tries the same find_package() call and
#   downloads only what is missing, at the pinned commits below.
# - GoogleTest is needed only when PRAENORA_BUILD_TESTS is ON.
#
# Afterwards these targets exist: nlohmann_json::nlohmann_json,
# nlohmann_json_schema_validator::validator, yaml-cpp::yaml-cpp, the spdlog target named by
# PRAENORA_SPDLOG_TARGET and, with tests, GTest::gtest and GTest::gtest_main. One status line per
# dependency reports its version and where it came from.
#
# praenora_copy_runtime_dlls(<target>) is also defined here.

include_guard(GLOBAL)

# Minimum versions accepted from find_package(). yaml-cpp 0.8 is the tested minimum; 0.7 is
# accepted too, because it exports only the unnamespaced yaml-cpp target, which the shim below
# turns into yaml-cpp::yaml-cpp.
set(PRAENORA_NLOHMANN_JSON_MIN_VERSION 3.11)
set(PRAENORA_JSON_SCHEMA_VALIDATOR_MIN_VERSION 2.3)
set(PRAENORA_YAML_CPP_MIN_VERSION 0.7)
set(PRAENORA_SPDLOG_MIN_VERSION 1.10)
set(PRAENORA_GTEST_MIN_VERSION 1.11)

# praenora_copy_runtime_dlls(<target>)
#
# On Windows, copies the DLLs that <target> loads at run time ($<TARGET_RUNTIME_DLLS>) next to it
# after every build, so tests and tools run without PATH changes. Does nothing when the list is
# empty and on platforms without DLLs.
function(praenora_copy_runtime_dlls target)
    if(WIN32)
        set(dlls "$<TARGET_RUNTIME_DLLS:${target}>")
        set(copy "${CMAKE_COMMAND};-E;copy_if_different;${dlls};$<TARGET_FILE_DIR:${target}>")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "$<$<BOOL:${dlls}>:${copy}>"
            COMMAND_EXPAND_LISTS
            VERBATIM)
    endif()
endfunction()

# _praenora_report_dependency(<label> <package> <content> <pinned-version> [<extra>])
# <package> is the find_package() name, <content> the FetchContent name.
function(_praenora_report_dependency label package content pinned_version)
    string(TOLOWER "${content}" content_lower)
    set(source_dir "")
    if(DEFINED ${content_lower}_SOURCE_DIR)
        set(source_dir "${${content_lower}_SOURCE_DIR}")
    endif()
    if(source_dir)
        set(version "${pinned_version}")
        set(origin "fetched into ${source_dir}")
    else()
        set(version "${${package}_VERSION}")
        set(origin "found at ${${package}_DIR}")
    endif()
    message(STATUS "Praenora: dependency ${label} ${version} (${origin}${ARGN})")
endfunction()

macro(praenora_find_dependencies)
    if(PRAENORA_FETCH_DEPENDENCIES)
        include(FetchContent)

        # Static dependencies are linked into the shared runtime, so fetched sub-projects build
        # position-independent code. The previous setting is restored afterwards.
        if(DEFINED CMAKE_POSITION_INDEPENDENT_CODE)
            set(_praenora_saved_pic "${CMAKE_POSITION_INDEPENDENT_CODE}")
        endif()
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)

        # Sub-project options. CMP0077 lets option() calls in older sub-projects honor them.
        set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
        set(INSTALL_GTEST OFF)
        set(gtest_force_shared_crt ON)
        set(YAML_CPP_BUILD_TOOLS OFF)
        # No install rules of the fetched projects: they are header-only or static libraries,
        # used privately by the shared runtime, so neither cmake --install nor a wheel may pick
        # up their headers, libraries or CMake files. The option names are the upstream projects'
        # own (nlohmann/json, json-schema-validator, yaml-cpp, spdlog); they could not be checked
        # without downloading the projects.
        set(JSON_Install OFF)
        set(JSON_VALIDATOR_INSTALL OFF)
        set(YAML_CPP_INSTALL OFF)
        set(SPDLOG_INSTALL OFF)

        # CMake 3.25 and newer treat the fetched headers as system headers.
        set(_praenora_fetch_system "")
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
            set(_praenora_fetch_system SYSTEM)
        endif()

        FetchContent_Declare(nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG 55f93686c01528224f448c19128836e7df245f72 # v3.12.0
            ${_praenora_fetch_system}
            FIND_PACKAGE_ARGS ${PRAENORA_NLOHMANN_JSON_MIN_VERSION} CONFIG)
        FetchContent_Declare(nlohmann_json_schema_validator
            GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
            GIT_TAG 9abddfa905bb4889cc4ae55f87adcaa71dd3bc23 # 2.4.1
            ${_praenora_fetch_system}
            FIND_PACKAGE_ARGS ${PRAENORA_JSON_SCHEMA_VALIDATOR_MIN_VERSION} CONFIG)
        FetchContent_Declare(yaml-cpp
            GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
            GIT_TAG 56e3bb550c91fd7005566f19c079cb7a503223cf # yaml-cpp-0.9.0
            ${_praenora_fetch_system}
            FIND_PACKAGE_ARGS ${PRAENORA_YAML_CPP_MIN_VERSION} CONFIG)
        FetchContent_Declare(spdlog
            GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG 79524ddd08a4ec981b7fea76afd08ee05f83755d # v1.17.0
            ${_praenora_fetch_system}
            FIND_PACKAGE_ARGS ${PRAENORA_SPDLOG_MIN_VERSION} CONFIG)
        # The content is named googletest, not GTest: GoogleTest's own gtest project would
        # otherwise clash with the gtest_SOURCE_DIR variable that FetchContent sets.
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG 063de7e9578f82b369302001269680b4b1553359 # v1.18.0
            ${_praenora_fetch_system}
            FIND_PACKAGE_ARGS ${PRAENORA_GTEST_MIN_VERSION} CONFIG NAMES GTest)

        set(_praenora_fetched nlohmann_json nlohmann_json_schema_validator yaml-cpp spdlog)
        if(PRAENORA_BUILD_TESTS)
            list(APPEND _praenora_fetched googletest)
        endif()
        # The fetched libraries are static, whatever a super-build's BUILD_SHARED_LIBS says: a
        # fetched shared library would be neither installed (the rules above are off) nor found
        # by an installed runtime. Each project's own shared-library option is set OFF (the names
        # are the upstream projects'; they could not be checked without downloading them), and
        # BUILD_SHARED_LIBS too, which some of them also follow, as a normal variable for these
        # projects only: it is unset right after them. This project never reads it.
        set(YAML_BUILD_SHARED_LIBS OFF)
        set(SPDLOG_BUILD_SHARED OFF)
        set(JSON_VALIDATOR_SHARED_LIBS OFF)
        set(BUILD_SHARED_LIBS OFF)
        FetchContent_MakeAvailable(${_praenora_fetched})
        unset(BUILD_SHARED_LIBS)

        if(DEFINED _praenora_saved_pic)
            set(CMAKE_POSITION_INDEPENDENT_CODE "${_praenora_saved_pic}")
            unset(_praenora_saved_pic)
        else()
            unset(CMAKE_POSITION_INDEPENDENT_CODE)
        endif()
        set(_praenora_gtest_package googletest)
    else()
        find_package(nlohmann_json ${PRAENORA_NLOHMANN_JSON_MIN_VERSION} CONFIG REQUIRED)
        find_package(nlohmann_json_schema_validator ${PRAENORA_JSON_SCHEMA_VALIDATOR_MIN_VERSION}
            CONFIG REQUIRED)
        find_package(yaml-cpp ${PRAENORA_YAML_CPP_MIN_VERSION} CONFIG REQUIRED)
        find_package(spdlog ${PRAENORA_SPDLOG_MIN_VERSION} CONFIG REQUIRED)
        if(PRAENORA_BUILD_TESTS)
            find_package(GTest ${PRAENORA_GTEST_MIN_VERSION} CONFIG REQUIRED)
        endif()
        set(_praenora_gtest_package GTest)
    endif()

    # Target shims for packages that export only the older, unnamespaced target names.
    if(NOT TARGET yaml-cpp::yaml-cpp)
        if(NOT TARGET yaml-cpp)
            message(FATAL_ERROR "yaml-cpp provides neither the yaml-cpp::yaml-cpp nor the "
                "yaml-cpp target.")
        endif()
        add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
    endif()
    if(DEFINED yaml-cpp_VERSION)
        if("${yaml-cpp_VERSION}" VERSION_LESS 0.8)
            message(STATUS "Praenora: yaml-cpp ${yaml-cpp_VERSION} is older than the tested "
                "minimum 0.8; it is used through the yaml-cpp target shim")
        endif()
    endif()
    if(NOT TARGET nlohmann_json_schema_validator::validator)
        if(NOT TARGET nlohmann_json_schema_validator)
            message(FATAL_ERROR "json-schema-validator provides neither the "
                "nlohmann_json_schema_validator::validator nor the "
                "nlohmann_json_schema_validator target.")
        endif()
        add_library(nlohmann_json_schema_validator::validator ALIAS nlohmann_json_schema_validator)
    endif()
    # spdlog: the compiled library when available, otherwise the header-only target.
    if(TARGET spdlog::spdlog)
        set(PRAENORA_SPDLOG_TARGET spdlog::spdlog)
    elseif(TARGET spdlog::spdlog_header_only)
        set(PRAENORA_SPDLOG_TARGET spdlog::spdlog_header_only)
    else()
        message(FATAL_ERROR "spdlog provides neither the spdlog::spdlog nor the "
            "spdlog::spdlog_header_only target.")
    endif()

    _praenora_report_dependency(nlohmann_json nlohmann_json nlohmann_json 3.12.0)
    _praenora_report_dependency(nlohmann_json_schema_validator nlohmann_json_schema_validator
        nlohmann_json_schema_validator 2.4.1)
    _praenora_report_dependency(yaml-cpp yaml-cpp yaml-cpp 0.9.0)
    _praenora_report_dependency(spdlog spdlog spdlog 1.17.0 "; target ${PRAENORA_SPDLOG_TARGET}")
    if(PRAENORA_BUILD_TESTS)
        _praenora_report_dependency(GTest ${_praenora_gtest_package} googletest 1.18.0)
    else()
        message(STATUS "Praenora: dependency GTest not needed (PRAENORA_BUILD_TESTS is OFF)")
    endif()
    unset(_praenora_gtest_package)
endmacro()
