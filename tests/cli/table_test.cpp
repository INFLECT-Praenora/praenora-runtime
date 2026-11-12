// The text tables of the praenora tool (src/cli/table.hpp): columns padded by UTF-8 code points.
#include "table.hpp"

#include <gtest/gtest.h>

#include <string>

using praenora::cli::display_width;
using praenora::cli::table;

TEST(CliTable, PadsColumnsByCodePoints) {
    // "ü" and "é" are two bytes each in UTF-8 but one column wide.
    EXPECT_EQ(display_width("t\xC3\xBC"), 2u);
    EXPECT_EQ(table({{"t\xC3\xBC", "a", "first"}, {"ab\xC3\xA9", "bb", "second"}}),
              "t\xC3\xBC   a   first\n"
              "ab\xC3\xA9  bb  second\n");
}

TEST(CliTable, LinesEndWithoutPadding) {
    EXPECT_EQ(table({{"a", "b", ""}, {"ccc", "d", "e"}}), "a    b\nccc  d  e\n");
}
