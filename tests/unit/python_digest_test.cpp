// The directory digest of Python sources as the runtime's Python core computes it
// (src/providers/python/core/digest.cpp): the value that praenora_python_directory_digest()
// (cmake/PraenoraPythonDigest.cmake) gives for the same files, files below __pycache__ left out,
// and the refusals: an unreadable file or a missing directory, and a symbolic link, which the
// CMake function refuses too (docs/specs/python-service.md#the-directory-digest).
#include "digest.hpp"

#include "temp_dir.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using praenora::detail::python::DigestError;
using praenora::detail::python::directory_digest;

// The value check_python_digest.cmake pins for tests/fixtures/python-digest, computed by hand.
constexpr const char* kPinnedFixtureDigest =
    "6abd23f2aa8ccc218a422d41e7e218a08fa00ba40e42455d3895446393056a85";
// The SHA-256 of no bytes: the digest of a directory without files.
constexpr const char* kEmptyDigest =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

fs::path fixture() { return fs::path(PRAENORA_TEST_FIXTURES_DIR) / "python-digest"; }

// A copy of the fixture's three files (never its __pycache__ directory) below `root`.
fs::path copy_fixture(const fs::path& root) {
    const fs::path copy = root / "sources";
    fs::create_directories(copy / "pkg");
    for (const char* name : {"a.py", "pkg/__init__.py", "pkg/data.txt"}) {
        fs::copy_file(fixture() / name, copy / name);
    }
    return copy;
}

void write_file(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << text;
}

// The digest's refusal of `root`: its status, or OK when it gave a digest.
praenora_status refusal_of(const fs::path& root, std::string* message = nullptr) {
    try {
        (void)directory_digest(root);
        return PRAENORA_STATUS_OK;
    } catch (const DigestError& error) {
        if (message != nullptr) {
            *message = error.what();
        }
        return error.code();
    }
}

} // namespace

TEST(PythonDigest, MatchesTheCmakeValue) {
    // The value pinned by hand, and the one the CMake function gave for the same directory when
    // this build was configured.
    EXPECT_EQ(directory_digest(fixture()), kPinnedFixtureDigest);
    EXPECT_EQ(directory_digest(fixture()), PRAENORA_TEST_PYTHON_DIGEST_OF_FIXTURE);

    praenora::test::ScratchDir scratch;
    const fs::path copy = copy_fixture(scratch.path());
    EXPECT_EQ(directory_digest(copy), kPinnedFixtureDigest) << "a copy in a path with a space and "
                                                               "a non-ASCII character";
    // Another content, a file added and a name changed each change it.
    write_file(copy / "pkg" / "data.txt", "other\n");
    const std::string edited = directory_digest(copy);
    EXPECT_NE(edited, kPinnedFixtureDigest);
    write_file(copy / "b.py", "");
    const std::string added = directory_digest(copy);
    EXPECT_NE(added, edited);
    fs::rename(copy / "b.py", copy / "c.py");
    EXPECT_NE(directory_digest(copy), added);

    const fs::path empty = scratch.path() / "empty";
    fs::create_directories(empty / "sub");
    EXPECT_EQ(directory_digest(empty), kEmptyDigest);
}

TEST(PythonDigest, IgnoresPycache) {
    praenora::test::ScratchDir scratch;
    const fs::path copy = copy_fixture(scratch.path());
    write_file(copy / "__pycache__" / "a.cpython-312.pyc", "bytecode");
    write_file(copy / "pkg" / "__pycache__" / "x.pyc", "bytecode");
    write_file(copy / "pkg" / "sub" / "__pycache__" / "deeper" / "y.pyc", "bytecode");
    EXPECT_EQ(directory_digest(copy), kPinnedFixtureDigest);
    // A file named __pycache__ is a file like any other, as for the CMake function, which leaves
    // out only what is below a directory of that name.
    write_file(copy / "pkg" / "sub" / "x" / "__pycache__", "a file");
    EXPECT_NE(directory_digest(copy), kPinnedFixtureDigest);
}

TEST(PythonDigest, UnreadableFileIsIoError) {
    praenora::test::ScratchDir scratch;
    std::string message;
    EXPECT_EQ(refusal_of(scratch.path() / "missing", &message), PRAENORA_STATUS_IO_ERROR);
    EXPECT_NE(message.find("missing"), std::string::npos) << message;

#ifdef _WIN32
    GTEST_SKIP() << "a file cannot be made unreadable with its permissions on Windows";
#else
    const fs::path copy = copy_fixture(scratch.path());
    const fs::path file = copy / "pkg" / "data.txt";
    fs::permissions(file, fs::perms::none);
    praenora::test::RestorePermissions restore(file,
                                               fs::perms::owner_read | fs::perms::owner_write);
    if (std::ifstream(file).is_open()) {
        GTEST_SKIP() << "this process reads files without read permission (root)";
    }
    EXPECT_EQ(refusal_of(copy, &message), PRAENORA_STATUS_IO_ERROR);
    EXPECT_NE(message.find("data.txt"), std::string::npos) << message;
#endif
}

// A root whose status cannot be read (its parent directory may not be searched) is refused with
// the system's reason, not as a directory that does not exist.
TEST(PythonDigest, RootThatCannotBeReadIsNotMissing) {
#ifdef _WIN32
    GTEST_SKIP() << "a directory cannot be made unsearchable with its permissions on Windows";
#else
    praenora::test::ScratchDir scratch;
    const fs::path locked = scratch.path() / "locked";
    const fs::path copy = copy_fixture(locked);
    fs::permissions(locked, fs::perms::none);
    praenora::test::RestorePermissions restore(locked, fs::perms::owner_all);
    std::error_code ec;
    static_cast<void>(fs::status(copy, ec));
    if (!ec) {
        GTEST_SKIP() << "this process searches directories without permission (root)";
    }
    std::string message;
    EXPECT_EQ(refusal_of(copy, &message), PRAENORA_STATUS_IO_ERROR);
    EXPECT_EQ(message.find("does not exist"), std::string::npos) << message;
    EXPECT_NE(message.find("cannot read"), std::string::npos) << message;
    EXPECT_NE(message.find(ec.message()), std::string::npos) << message;
#endif
}

TEST(PythonDigest, SymbolicLinksAreRefused) {
    praenora::test::ScratchDir scratch;
    const fs::path copy = copy_fixture(scratch.path());
    std::error_code ec;
    fs::create_symlink(copy / "a.py", copy / "link.py", ec);
    if (ec) {
        GTEST_SKIP() << "symbolic links cannot be made here: " << ec.message();
    }
    std::string message;
    EXPECT_EQ(refusal_of(copy, &message), PRAENORA_STATUS_DATA_INVALID);
    EXPECT_NE(message.find("link.py"), std::string::npos) << message;
    EXPECT_NE(message.find("symbolic link"), std::string::npos) << message;
    fs::remove(copy / "link.py");

    // A link to a directory, and a link named __pycache__, as the CMake function refuses them.
    fs::create_directory_symlink(copy / "pkg", copy / "linked", ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(refusal_of(copy), PRAENORA_STATUS_DATA_INVALID);
    fs::remove(copy / "linked");
    fs::create_directory_symlink(copy / "pkg", copy / "__pycache__", ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(refusal_of(copy), PRAENORA_STATUS_DATA_INVALID);
    fs::remove(copy / "__pycache__");

    // A link below a __pycache__ directory is left out with it.
    fs::create_directories(copy / "pkg" / "__pycache__");
    fs::create_symlink(copy / "a.py", copy / "pkg" / "__pycache__" / "link.pyc", ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(directory_digest(copy), kPinnedFixtureDigest);
}
