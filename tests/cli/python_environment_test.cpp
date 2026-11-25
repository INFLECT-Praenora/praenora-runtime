// What the runtime's own programs do first thing (src/frontend/python_environment.hpp): with the
// environment file of the provider python in their tree, they set each variable of the Python
// environment's environment.paths but PYTHONHOME, and PYTHONHASHSEED to 0, when it is unset; a
// value set before stays. The file is the installation's, at the path the program was built with
// (below the installation's library directory, lib64/ for example), or the build tree's, below
// lib/. Each test builds a tree of its own below a scratch directory (bin/, libexec/praenora/,
// <libdir>/praenora/providers/python/python-environment.json and the environment's manifest) and
// restores every variable it touches. No Python is needed.
#include <python_environment.hpp>

#include "scoped_environment.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace {

namespace fs = std::filesystem;
namespace frontend = praenora::frontend;
using praenora::test::ScopedEnvironment;
using praenora::test::ScratchDir;
using praenora::test::utf8_path;

// The variable the environment's manifest names besides PYTHONHOME, as a bundle with data that its
// packages read names one.
constexpr const char* kDataVariable = "PRAENORA_TEST_DATA_DIR";

// The variable's value as UTF-8, read where the Python core reads it (GetEnvironmentVariableW on
// Windows), or nullopt when it is unset.
std::optional<std::string> variable(const char* name) {
#ifdef _WIN32
    const std::wstring wide_name = utf8_path(name).wstring();
    std::vector<wchar_t> buffer(256);
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetEnvironmentVariableW(wide_name.c_str(), buffer.data(),
                                                     static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
                return std::nullopt;
            }
            return std::string();
        }
        if (length < buffer.size()) {
            return fs::path(std::wstring(buffer.data(), length)).u8string();
        }
        buffer.resize(static_cast<std::size_t>(length));
    }
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::nullopt : std::optional<std::string>(value);
#endif
}

void write_file(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << text;
}

// A manifest of the shape praenora-runtime-pyenv writes, cut down to what the programs read and a
// little more.
const char kManifest[] = R"({
  "manifest_version": 1,
  "name": "praenora-runtime-pyenv",
  "version": "0.1.0",
  "package": "praenora-runtime-pyenv-0.1.0-test",
  "lock": {"sha256": "0000000000000000000000000000000000000000000000000000000000000000"},
  "python": {"home": "python", "executable": "python/bin/python3.12", "version": "3.12.16"},
  "environment": {
    "paths": {"PRAENORA_TEST_DATA_DIR": "data/share/runtime", "PYTHONHOME": "python"},
    "values": {"PYTHONNOUSERSITE": "1"}
  }
}
)";

// A tree of the runtime below `root`: the programs' directories bin/ and libexec/praenora/, and,
// unless `with_file` is false, the provider python's environment file naming the environment
// <root>/<libdir>/praenora/python-environment, whose manifest is `manifest`. The file names it
// relative to itself, as an installation's does, or by its absolute path, as a build tree's does.
struct Tree {
    fs::path root;
    fs::path environment;

    fs::path tool() const { return root / "bin" / "praenora"; }
    fs::path module_host() const { return root / "libexec" / "praenora" / "praenora-module-host"; }
    // The path the manifest's PRAENORA_TEST_DATA_DIR names, as the programs set it.
    std::string data_dir() const {
        return (environment / "data" / "share" / "runtime").lexically_normal().u8string();
    }
};

Tree make_tree(const fs::path& root, bool with_file = true, bool relative = true,
               const std::string& manifest = kManifest, const std::string& libdir = "lib") {
    Tree tree{root, root / fs::u8path(libdir) / "praenora" / "python-environment"};
    fs::create_directories(root / "bin");
    fs::create_directories(root / "libexec" / "praenora");
    write_file(tree.environment / "praenora-runtime-pyenv.json", manifest);
    if (with_file) {
        const fs::path provider = root / fs::u8path(libdir) / "praenora" / "providers" / "python";
        const std::string environment =
            relative ? std::string("../../python-environment") : tree.environment.u8string();
        std::string escaped;
        for (const char c : environment) {
            if (c == '\\' || c == '"') {
                escaped += '\\';
            }
            escaped += c;
        }
        write_file(provider / "python-environment.json",
                   "{\"environment\": \"" + escaped + "\", \"core\": \"praenora-python-core\"}\n");
    }
    return tree;
}

// Every variable the programs may set, unset for the test's life and restored after it.
struct CleanVariables {
    ScopedEnvironment data{kDataVariable, std::nullopt};
    ScopedEnvironment seed{"PYTHONHASHSEED", std::nullopt};
    ScopedEnvironment home{"PYTHONHOME", std::nullopt};
};

} // namespace

// Each variable of environment.paths but PYTHONHOME is set to its absolute path below the
// environment, and PYTHONHASHSEED to 0, when they are unset; PYTHONHOME stays unset (the core gives
// the interpreter its home itself).
TEST(Frontend, PythonEnvironmentSetsUnsetVariables) {
    const ScratchDir scratch;
    const Tree tree = make_tree(scratch.path());
    const CleanVariables clean;

    frontend::apply_python_environment(tree.tool(), frontend::ProgramDirectory::bin);

    EXPECT_EQ(variable(kDataVariable), tree.data_dir());
    EXPECT_TRUE(utf8_path(*variable(kDataVariable)).is_absolute());
    EXPECT_EQ(variable("PYTHONHASHSEED"), "0");
    EXPECT_EQ(variable("PYTHONHOME"), std::nullopt);
}

// A variable that is set keeps its value, whatever it is, and so does an empty one.
TEST(Frontend, PythonEnvironmentKeepsSetVariables) {
    const ScratchDir scratch;
    const Tree tree = make_tree(scratch.path());
    const ScopedEnvironment data(kDataVariable, std::string("/somewhere/else"));
    const ScopedEnvironment seed("PYTHONHASHSEED", std::string("7"));
    const ScopedEnvironment home("PYTHONHOME", std::string("/a/home"));

    frontend::apply_python_environment(tree.tool(), frontend::ProgramDirectory::bin);

    EXPECT_EQ(variable(kDataVariable), "/somewhere/else");
    EXPECT_EQ(variable("PYTHONHASHSEED"), "7");
    EXPECT_EQ(variable("PYTHONHOME"), "/a/home");
}

// A tree without the provider python's environment file: nothing is set, not even PYTHONHASHSEED.
TEST(Frontend, PythonEnvironmentAbsentFileDoesNothing) {
    const ScratchDir scratch;
    const Tree tree = make_tree(scratch.path(), false);
    const CleanVariables clean;

    frontend::apply_python_environment(tree.tool(), frontend::ProgramDirectory::bin);
    frontend::apply_python_environment(tree.module_host(), frontend::ProgramDirectory::libexec);
    // A program path that names no file, and an empty one, are no problem either.
    frontend::apply_python_environment(scratch.path() / "nowhere" / "praenora",
                                       frontend::ProgramDirectory::bin);
    frontend::apply_python_environment(fs::path(), frontend::ProgramDirectory::bin);

    EXPECT_EQ(variable(kDataVariable), std::nullopt);
    EXPECT_EQ(variable("PYTHONHASHSEED"), std::nullopt);
    EXPECT_EQ(variable("PYTHONHOME"), std::nullopt);
}

// The module host, in <root>/libexec/praenora/, finds the same file as the tool in <root>/bin/,
// here one that names the environment by its absolute path, as a build tree's does.
TEST(Frontend, PythonEnvironmentModuleHostLayout) {
    const ScratchDir scratch;
    const Tree tree = make_tree(scratch.path(), true, false);
    const CleanVariables clean;

    frontend::apply_python_environment(tree.module_host(), frontend::ProgramDirectory::libexec);

    EXPECT_EQ(variable(kDataVariable), tree.data_dir());
    EXPECT_EQ(variable("PYTHONHASHSEED"), "0");
    EXPECT_EQ(variable("PYTHONHOME"), std::nullopt);
}

// A broken environment manifest, or a broken environment file, is no reason to stop: the provider's
// start reports it. The tree has the provider python, so PYTHONHASHSEED is still set; no path is.
TEST(Frontend, PythonEnvironmentIgnoresBrokenFiles) {
    const ScratchDir scratch;
    int count = 0;
    for (const std::string& manifest :
         {std::string("not JSON"), std::string("[1, 2]"),
          std::string(R"({"environment": {"paths": ["PRAENORA_TEST_DATA_DIR"]}})"),
          std::string(R"({"environment": {"paths": {"PRAENORA_TEST_DATA_DIR": 3}}})"),
          std::string(R"({"environment": {"paths": {"": "a", "A=B": "b"}}})")}) {
        SCOPED_TRACE(manifest);
        const fs::path root = scratch.path() / ("manifest " + std::to_string(++count));
        const Tree tree = make_tree(root, true, true, manifest);
        const CleanVariables clean;

        frontend::apply_python_environment(tree.tool(), frontend::ProgramDirectory::bin);

        EXPECT_EQ(variable(kDataVariable), std::nullopt);
        EXPECT_EQ(variable("PYTHONHASHSEED"), "0");
    }
    for (const std::string& file : {std::string("{"), std::string("{\"core\": \"x\"}"),
                                    std::string("{\"environment\": 1, \"core\": \"x\"}")}) {
        SCOPED_TRACE(file);
        const fs::path root = scratch.path() / ("file " + std::to_string(++count));
        const Tree tree = make_tree(root);
        write_file(root / "lib" / "praenora" / "providers" / "python" / "python-environment.json",
                   file);
        const CleanVariables clean;

        frontend::apply_python_environment(tree.tool(), frontend::ProgramDirectory::bin);

        EXPECT_EQ(variable(kDataVariable), std::nullopt);
        EXPECT_EQ(variable("PYTHONHASHSEED"), "0");
    }
}

// An installation whose library directory is not lib/ (lib64/ here, as on Fedora): the tool and
// the module host find the environment file at the path relative to their directory that they
// were built with, which names that library directory; without it they look only where a build
// tree has it, below lib/, and set nothing.
TEST(Frontend, PythonEnvironmentInstalledLibraryDirectory) {
    const ScratchDir scratch;
    const Tree tree = make_tree(scratch.path(), true, true, kManifest, "lib64");
    const std::string from_bin = "../lib64/praenora/providers/python/python-environment.json";
    const std::string from_libexec =
        "../../lib64/praenora/providers/python/python-environment.json";
    {
        const CleanVariables clean;
        frontend::apply_python_environment(tree.tool(), frontend::ProgramDirectory::bin);
        frontend::apply_python_environment(tree.module_host(), frontend::ProgramDirectory::libexec);
        EXPECT_EQ(variable(kDataVariable), std::nullopt);
        EXPECT_EQ(variable("PYTHONHASHSEED"), std::nullopt);
    }
    {
        const CleanVariables clean;
        frontend::apply_python_environment(tree.tool(), frontend::ProgramDirectory::bin, from_bin);
        EXPECT_EQ(variable(kDataVariable), tree.data_dir());
        EXPECT_EQ(variable("PYTHONHASHSEED"), "0");
    }
    {
        const CleanVariables clean;
        frontend::apply_python_environment(tree.module_host(), frontend::ProgramDirectory::libexec,
                                           from_libexec);
        EXPECT_EQ(variable(kDataVariable), tree.data_dir());
        EXPECT_EQ(variable("PYTHONHASHSEED"), "0");
    }
    EXPECT_EQ(
        frontend::python_environment_file(tree.tool(), frontend::ProgramDirectory::bin, from_bin),
        (scratch.path() / "lib64" / "praenora" / "providers" / "python" / "python-environment.json")
            .lexically_normal());
}

// A build tree's programs are built with the installation's path too, which names no file there:
// they find the build tree's file below lib/. When both files exist, the installation's wins.
TEST(Frontend, PythonEnvironmentBuildTreeAndInstalledFile) {
    const ScratchDir scratch;
    const fs::path build_tree = scratch.path() / "build";
    const Tree tree = make_tree(build_tree);
    const std::string from_bin = "../lib64/praenora/providers/python/python-environment.json";
    {
        const CleanVariables clean;
        frontend::apply_python_environment(tree.tool(), frontend::ProgramDirectory::bin, from_bin);
        EXPECT_EQ(variable(kDataVariable), tree.data_dir());
        EXPECT_EQ(variable("PYTHONHASHSEED"), "0");
    }
    // The same tree with an installation's file below lib64/, whose environment's manifest names
    // another directory for the variable.
    std::string other(kManifest);
    other.replace(other.find("data/share/runtime"), std::string("data/share/runtime").size(),
                  "other/data");
    const Tree installed = make_tree(build_tree, true, true, other, "lib64");
    {
        const CleanVariables clean;
        frontend::apply_python_environment(tree.tool(), frontend::ProgramDirectory::bin, from_bin);
        EXPECT_EQ(variable(kDataVariable),
                  (installed.environment / "other" / "data").lexically_normal().u8string());
    }
    EXPECT_EQ(frontend::python_environment_file(tree.tool(), frontend::ProgramDirectory::bin,
                                                "../lib/praenora/none.json"),
              (build_tree / "lib" / "praenora" / "providers" / "python" / "python-environment.json")
                  .lexically_normal());
}

// The running program's own path, as the tool and the module host pass it: absolute, and this
// test executable's file.
TEST(Frontend, PythonEnvironmentProgramPathIsAbsolute) {
    const fs::path program = frontend::current_program_path();
    ASSERT_FALSE(program.empty());
    EXPECT_TRUE(program.is_absolute());
    std::error_code ec;
    EXPECT_TRUE(fs::is_regular_file(program, ec)) << program.u8string();
}
