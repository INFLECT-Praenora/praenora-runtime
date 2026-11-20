// The sandbox profiles of a module host (isolation/sandbox.hpp): their names, the restricted
// profile's text, which allows writes below the module host's temporary directory and to /dev/null
// only, quoting what the path holds, where the platform has it, and the private temporary
// directory the runtime makes for a module host and removes with what is in it. Applying the
// profile is tested in module hosts (tests/isolation/sandbox_test.cpp), never in this process.
#include "isolation/sandbox.hpp"

#include "core/paths.hpp"

#include "sandbox_support.hpp"

#include <gtest/gtest.h>

#if defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;
using praenora::detail::PrivateTemporaryDirectory;
using praenora::detail::restricted_profile_text;
using praenora::detail::SandboxProfile;

TEST(Sandbox, ProfilesHaveTheConfigurationsNames) {
    EXPECT_EQ(praenora::detail::sandbox_profile_name(SandboxProfile::none), "none");
    EXPECT_EQ(praenora::detail::sandbox_profile_name(SandboxProfile::restricted), "restricted");
    EXPECT_EQ(praenora::detail::sandbox_profile_from_name("none"), SandboxProfile::none);
    EXPECT_EQ(praenora::detail::sandbox_profile_from_name("restricted"),
              SandboxProfile::restricted);
    EXPECT_EQ(praenora::detail::sandbox_profile_from_name("Restricted"), std::nullopt);
    EXPECT_EQ(praenora::detail::sandbox_profile_from_name(""), std::nullopt);
}

// Allowed by default; denied: the network, new processes (also through launchd jobs,
// LaunchServices and Apple events) and every write but those below the directory and to /dev/null,
// which come last, so that they are the writes left.
TEST(Sandbox, TheRestrictedProfileAllowsWritesBelowItsDirectoryOnly) {
    EXPECT_EQ(restricted_profile_text("/private/var/folders/x/T/praenora-module-host-abc"),
              "(version 1)\n"
              "(allow default)\n"
              "(deny network*)\n"
              "(deny process-fork)\n"
              "(deny process-exec)\n"
              "(deny job-creation)\n"
              "(deny lsopen)\n"
              "(deny appleevent-send)\n"
              "(deny file-write*)\n"
              "(allow file-write* (subpath \"/private/var/folders/x/T/praenora-module-host-abc\") "
              "(literal \"/dev/null\"))\n");
}

TEST(Sandbox, TheRestrictedProfileQuotesItsDirectory) {
    const std::string text = restricted_profile_text("/tmp/a \"b\" \\c/d\xC3\xA9");
    EXPECT_NE(text.find("(subpath \"/tmp/a \\\"b\\\" \\\\c/d\xC3\xA9\")"), std::string::npos)
        << text;
}

TEST(Sandbox, TheRestrictedProfileIsAvailableOnMacOSOnly) {
#if defined(__APPLE__)
    EXPECT_TRUE(praenora::detail::restricted_sandbox_available());
#else
    EXPECT_FALSE(praenora::detail::restricted_sandbox_available());
    EXPECT_NE(praenora::detail::apply_restricted_profile("/tmp"), std::nullopt);
#endif
}

#ifndef _WIN32
// A private temporary directory is new, below the real path of the system's temporary directory,
// for its owner only, and goes away with what is in it; a moved one is removed once.
TEST(Sandbox, APrivateTemporaryDirectoryIsTheModuleHostsOwnAndIsRemoved) {
    std::string problem;
    std::optional<PrivateTemporaryDirectory> directory = PrivateTemporaryDirectory::make(problem);
    ASSERT_TRUE(directory.has_value()) << problem;
    const fs::path path = praenora::detail::path_from_utf8(directory->path());
    EXPECT_TRUE(fs::is_directory(path));
    EXPECT_EQ(path.parent_path(), fs::canonical(fs::temp_directory_path()));
    EXPECT_EQ(path.filename().string().rfind("praenora-module-host-", 0), 0u) << path;
    EXPECT_EQ(fs::status(path).permissions() & fs::perms::all, fs::perms::owner_all);
    std::string other_problem;
    const auto other = PrivateTemporaryDirectory::make(other_problem);
    ASSERT_TRUE(other.has_value()) << other_problem;
    EXPECT_NE(other->path(), directory->path());

    fs::create_directories(path / "inner");
    std::ofstream(path / "inner" / "file") << "written";
    PrivateTemporaryDirectory moved = std::move(*directory);
    EXPECT_EQ(praenora::detail::path_from_utf8(moved.path()), path);
    directory.reset(); // the moved-from object removes nothing
    EXPECT_TRUE(fs::exists(path / "inner" / "file"));
    {
        const PrivateTemporaryDirectory gone = std::move(moved);
    }
    std::error_code ec;
    EXPECT_FALSE(fs::exists(path, ec)) << path;
}

// A module in the directory may take every right away from what it made in it: a directory of
// mode 000 that holds a file, below another of mode 000. Removal gives the owner its rights back
// first, never through a symbolic link (one to a directory outside keeps that directory's mode),
// and the directory goes.
TEST(Sandbox, APrivateTemporaryDirectoryGoesAwayWhateverModesAreLeftInIt) {
    std::string problem;
    std::optional<PrivateTemporaryDirectory> directory = PrivateTemporaryDirectory::make(problem);
    ASSERT_TRUE(directory.has_value()) << problem;
    const fs::path path = praenora::detail::path_from_utf8(directory->path());
    fs::create_directories(path / "closed" / "inner");
    std::ofstream(path / "closed" / "inner" / "file") << "written";
    std::string other_problem;
    std::optional<PrivateTemporaryDirectory> outside =
        PrivateTemporaryDirectory::make(other_problem);
    ASSERT_TRUE(outside.has_value()) << other_problem;
    const fs::path elsewhere = praenora::detail::path_from_utf8(outside->path());
    fs::permissions(elsewhere, fs::perms::owner_read | fs::perms::owner_exec);
    fs::create_directory_symlink(elsewhere, path / "link");
    fs::permissions(path / "closed" / "inner", fs::perms::none);
    fs::permissions(path / "closed", fs::perms::none);

    EXPECT_EQ(directory->remove(), std::nullopt);
    std::error_code ec;
    EXPECT_FALSE(fs::exists(path, ec)) << path;
    EXPECT_EQ(directory->path(), "");
    EXPECT_EQ(directory->remove(), std::nullopt); // once is enough
    // The directory the link named was left alone.
    EXPECT_EQ(fs::status(elsewhere).permissions() & fs::perms::all,
              fs::perms::owner_read | fs::perms::owner_exec);
    fs::permissions(elsewhere, fs::perms::owner_all);
}

// A module may replace the directory itself with a symbolic link (the directory is its own to
// write in), here to a directory outside that holds a file and a directory of mode 000. Removal
// then removes the link only: it follows it neither to give rights back nor to remove, so what the
// link names keeps its contents and every mode.
TEST(Sandbox, APrivateTemporaryDirectoryReplacedByALinkLosesOnlyTheLink) {
    std::string problem;
    std::optional<PrivateTemporaryDirectory> directory = PrivateTemporaryDirectory::make(problem);
    ASSERT_TRUE(directory.has_value()) << problem;
    const fs::path path = praenora::detail::path_from_utf8(directory->path());
    std::string other_problem;
    std::optional<PrivateTemporaryDirectory> outside =
        PrivateTemporaryDirectory::make(other_problem);
    ASSERT_TRUE(outside.has_value()) << other_problem;
    const fs::path elsewhere = praenora::detail::path_from_utf8(outside->path());
    std::ofstream(elsewhere / "file") << "kept";
    fs::create_directory(elsewhere / "closed");
    fs::permissions(elsewhere / "closed", fs::perms::none);
    fs::permissions(elsewhere, fs::perms::owner_read | fs::perms::owner_exec);
    ASSERT_TRUE(fs::remove(path));
    fs::create_directory_symlink(elsewhere, path);

    EXPECT_EQ(directory->remove(), std::nullopt);
    std::error_code ec;
    EXPECT_FALSE(fs::exists(fs::symlink_status(path, ec))) << path;
    EXPECT_EQ(directory->path(), "");
    // What the link named is as it was.
    EXPECT_EQ(fs::status(elsewhere).permissions() & fs::perms::all,
              fs::perms::owner_read | fs::perms::owner_exec);
    EXPECT_EQ(fs::symlink_status(elsewhere / "closed").permissions() & fs::perms::all,
              fs::perms::none);
    std::ifstream in(elsewhere / "file");
    std::string text;
    std::getline(in, text);
    EXPECT_EQ(text, "kept");
    // `outside` removes it, giving its owner the rights back first.
}

#if defined(__APPLE__)
// What cannot be removed (here a file its owner made immutable, as a module may) is left, and
// remove() says what and why. The test removes it then, however it ends.
TEST(Sandbox, APrivateTemporaryDirectoryThatCannotBeRemovedSaysWhatIsLeft) {
    std::string problem;
    std::optional<PrivateTemporaryDirectory> directory = PrivateTemporaryDirectory::make(problem);
    ASSERT_TRUE(directory.has_value()) << problem;
    const fs::path path = praenora::detail::path_from_utf8(directory->path());
    const praenora::test::RemoveAllEvenImmutable cleanup(path);
    fs::create_directories(path / "kept");
    const fs::path file = path / "kept" / "file";
    std::ofstream(file) << "written";
    ASSERT_EQ(::chflags(file.c_str(), UF_IMMUTABLE), 0);

    const std::optional<std::string> left = directory->remove();
    ASSERT_TRUE(left.has_value());
    EXPECT_NE(left->find(praenora::detail::path_to_utf8(path)), std::string::npos) << *left;
    EXPECT_TRUE(fs::exists(file));
    EXPECT_EQ(directory->path(), "");
}
#endif
#endif
