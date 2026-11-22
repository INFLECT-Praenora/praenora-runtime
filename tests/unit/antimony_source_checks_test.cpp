// The antimony provider's rules that read a source's text alone (src/providers/antimony/
// source_checks.hpp), run in every build: they need no engine. The check of imports mirrors how
// libAntimony 3.3's lexer finds comments, strings and triple-backtick blocks
// (docs/specs/model-conversion-service.md, the antimony provider's section); the provider tests
// (tests/contract/antimony_provider_test.cpp) show through libAntimony itself that what it lets
// pass never makes libAntimony open a file.
#include "source_checks.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using praenora::antimony_provider::antimony_import_rule;
using praenora::antimony_provider::antimony_text_rule;
using praenora::antimony_provider::sbml_text_rule;

namespace {

void expect_rule(const std::vector<std::string>& sources, const std::string& rule) {
    for (const std::string& source : sources) {
        EXPECT_EQ(antimony_import_rule(source), rule) << "source '" << source << "'";
    }
}

} // namespace

TEST(AntimonyImportCheck, RefusesImportInCode) {
    expect_rule({"import \"x.txt\"\n", "IMPORT \"x.txt\"\n", "Import \"x.txt\"\n",
                 "import\"x.txt\"\n", "import", "x = 1;import \"x.txt\"\n",
                 "x = 1\nimport \"x.txt\"\n", "x = 1\n\n  \timport \"x.txt\"\n",
                 "/* a comment */import \"x.txt\"\n", "x is \"a name\"; import \"x.txt\"\n",
                 "x notes ```a note```; import \"x.txt\"\n", "x = 1 \\ import \"x.txt\"\n",
                 "x.import = 2\n", "\xC3\xA9import \"x.txt\"\n", "import\xC3\xA9\n"},
                "import");
}

TEST(AntimonyImportCheck, RefusesImportAfterANumber) {
    // libAntimony reads a number and a word that follows it without a space as two tokens: in
    // `M.sboTerm = 1import "x"`, after a module M, it opens x. Any import in a run of letters,
    // digits, '_' and '.' that starts with a digit is refused.
    expect_rule({"model M()\nend\nM.sboTerm = 1import \"x.txt\"\n", "x = 1import \"x.txt\"\n",
                 "x = 1.5import \"x.txt\"\n", "x = .5import \"x.txt\"\n",
                 "x = 0x1import \"x.txt\"\n", "x = 1e5import \"x.txt\"\n",
                 "x = 1e-3Import \"x.txt\"\n", "x = 2import.x\n"},
                "import");
    // A word that starts with a letter or '_' is one word, whatever its digits.
    expect_rule({"x1import = 1\n", "_1import = 1\n"}, "");
}

TEST(AntimonyImportCheck, AcceptsImportInComments) {
    expect_rule({"// import \"x.txt\"\nx = 1\n", "# import \"x.txt\"\nx = 1\n",
                 "x = 1 // import \"x.txt\"\n", "x = 1 # import \"x.txt\"\n",
                 "/* import \"x.txt\" */ x = 1\n", "/* first line\nimport \"x.txt\"\n*/\nx = 1\n",
                 "/**/x = 1 /*/ import \"x.txt\" */\n", "x = 1 // a \"quote\n"},
                "");
}

TEST(AntimonyImportCheck, AcceptsImportInStrings) {
    expect_rule({"x is \"import\"\n", "x is \"import \\\"x.txt\\\"\"\n", "x is \"a // b\"\n",
                 "x is \"# not a comment\"\n", "x is \"/* not a comment\"\n"},
                "");
    // A string ends at its closing quote: an import after it is code, even when the string holds
    // what would start a comment.
    expect_rule({"x is \"a // b\"; import \"y.txt\"\n", "x is \"/*\"; import \"y.txt\" /* */\n"},
                "import");
}

TEST(AntimonyImportCheck, AcceptsImportInTripleBackticks) {
    expect_rule({"x notes ```import \"x.txt\"```\n",
                 "x notes ```\n  first line\n  import \"x.txt\"\n```\n",
                 "x notes ```a `` b ` import```\n", "x notes ```a // b```\n"},
                "");
    // Two backticks are an empty string, and one is a character of its own: neither hides
    // anything.
    expect_rule({"x is ``; import \"y.txt\"\n", "x = ` import \"y.txt\"\n"}, "import");
}

TEST(AntimonyImportCheck, IgnoresImportAsPartOfAWord) {
    expect_rule({"importance = 1\n", "k_import = 1\n", "import2 = 1\n", "reimport = 1\n",
                 "IMPORTS = 1\n", "_import = 1\n", "import_ = 1\n"},
                "");
}

TEST(AntimonyImportCheck, RefusesUnterminated) {
    // What libAntimony does with these is not assumed.
    expect_rule({"x = 1 /* a block comment", "x = 1 /*/", "x is \"a string",
                 "x is \"a string\nimport \"y.txt\"\n", "x is \"a string\\", "x notes ```a block",
                 "x notes ```a block``", "x notes ```"},
                "unterminated");
}

TEST(AntimonyImportCheck, CrLfEndsLineComments) {
    // libAntimony ends a line comment at a CR as at a LF: the import on the next line is code.
    expect_rule({"// x\r\nimport \"y.txt\"\r\n", "# x\r\nimport \"y.txt\"\r\n",
                 "// x\rimport \"y.txt\"\r", "x = 1 # x\rimport \"y.txt\"\n"},
                "import");
    // A CR or a LF also ends a string that is still open: it is unterminated.
    expect_rule({"x is \"a\r\nb\"\n", "x is \"a\rb\"\n"}, "unterminated");
}

TEST(AntimonyImportCheck, BackslashEscapesInStrings) {
    // As in libAntimony, a backslash in a string takes the next character into the string, so the
    // string ends at the first quote without a backslash before it. A scan that ended the string
    // at the first quote would take the comment that starts after it here for code that hides the
    // import.
    expect_rule({"x is \"a\\\" // \"; import \"y.txt\"\n", "x is \"a\\\" # \"; import \"y.txt\"\n",
                 "x is \"a\\\\\"; import \"y.txt\"\n"},
                "import");
    // The string "a\" import " ends at the third quote; the fourth opens one that never ends.
    expect_rule({"x is \"a\\\" import \"b\"\n"}, "unterminated");
    expect_rule({"x is \"a\\\"b\"\n", "x is \"a\\\nb\"\n"}, "");
}

TEST(AntimonyImportCheck, FourBackticksStartNoBlock) {
    // libAntimony reads four backticks as one token and goes on with code after them.
    expect_rule({"x = 1; ````import \"y.txt\"```\n"}, "import");
    expect_rule({"x = 1; ``````\n"}, "");
}

TEST(AntimonyTextCheck, RefusesXmlDocuments) {
    // libAntimony reads a source that is an SBML document as SBML, outside the Antimony rules.
    for (const std::string source :
         {"<sbml/>", "<?xml version=\"1.0\"?>\n<sbml/>", "\xEF\xBB\xBF<sbml/>", " \t\r\n<sbml/>",
          "\xEF\xBB\xBF\n  <!-- a comment --><sbml/>"}) {
        EXPECT_EQ(antimony_text_rule(source), "xml") << "source '" << source << "'";
    }
    // Then the check of imports.
    EXPECT_EQ(antimony_text_rule("x = 1\nimport \"y.txt\"\n"), "import");
    EXPECT_EQ(antimony_text_rule("x = 1 /*"), "unterminated");
    for (const std::string source :
         {"x = 1\n", "// <sbml/>\nx = 1\n", "\xEF\xBB\xBFx = 1\n", "x = 2 < 3\n"}) {
        EXPECT_EQ(antimony_text_rule(source), "") << "source '" << source << "'";
    }
}

TEST(SbmlTextCheck, RequiresAMarkupStart) {
    for (const std::string source :
         {"<sbml/>", "<?xml version=\"1.0\"?><sbml/>", "\xEF\xBB\xBF<sbml/>", " \t\r\n<sbml/>",
          "\xEF\xBB\xBF \n<?xml version=\"1.0\"?><sbml/>", "<!-- a comment -->\n<sbml/>"}) {
        EXPECT_EQ(sbml_text_rule(source), "") << "source '" << source << "'";
    }
    // The source is the document's text, never a path or a URI to it.
    for (const std::string source : {"models/x.xml", "/models/x.xml", "http://example.org/x.xml",
                                     "file:x.xml", "x.xml <sbml/>", "\xEF\xBB\xBFmodels/x.xml"}) {
        EXPECT_EQ(sbml_text_rule(source), "not_document") << "source '" << source << "'";
    }
}

TEST(SbmlTextCheck, RefusesDoctypeInAnyCase) {
    for (const std::string source :
         {"<?xml version=\"1.0\"?>\n<!DOCTYPE sbml>\n<sbml/>",
          "<?xml version=\"1.0\"?><!doctype sbml [<!ENTITY e SYSTEM \"x.xml\">]><sbml/>",
          "<?xml version=\"1.0\"?><!-- a comment -->\n<!DocType sbml><sbml/>",
          "<!-- a comment --><!dOcTyPe sbml><sbml/>", "<sbml><!DOCTYPE x></sbml>"}) {
        EXPECT_EQ(sbml_text_rule(source), "document_type") << "source '" << source << "'";
    }
}

TEST(AntimonyTextCheck, EngineTextDropsByteOrderMarkAndCrBeforeLf) {
    using praenora::antimony_provider::antimony_engine_text;
    EXPECT_EQ(antimony_engine_text("\xEF\xBB\xBFx = 1\r\ny = 2\r\n"), "x = 1\ny = 2\n");
    // A CR alone is a line end of libAntimony's own; it stays, as does a byte order mark inside.
    EXPECT_EQ(antimony_engine_text("x = 1\ry = 2\r\r\n\xEF\xBB\xBF"),
              "x = 1\ry = 2\r\n\xEF\xBB\xBF");
    EXPECT_EQ(antimony_engine_text("\xEF\xBB\xBF"), "");
}

TEST(SbmlTextCheck, EngineTextStartsWithAnXmlDeclaration) {
    using praenora::antimony_provider::sbml_engine_text;
    using praenora::antimony_provider::SbmlDocumentText;
    const std::string declaration = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    // A document with its declaration is read as it is, from its first '<'.
    SbmlDocumentText text =
        sbml_engine_text("\xEF\xBB\xBF \r\n\t\n  <?xml version=\"1.0\"?><sbml/>");
    EXPECT_EQ(text.document, "<?xml version=\"1.0\"?><sbml/>");
    EXPECT_EQ(text.lines_added, 0u);
    EXPECT_EQ(text.lines_before, 2u);
    EXPECT_EQ(text.columns_before, 2u);
    // Without one, it gets a line with one.
    text = sbml_engine_text("\r<sbml/>");
    EXPECT_EQ(text.document, declaration + "<sbml/>");
    EXPECT_EQ(text.lines_added, 1u);
    EXPECT_EQ(text.lines_before, 1u);
    EXPECT_EQ(text.columns_before, 0u);
    text = sbml_engine_text("<?xml-stylesheet href=\"x.xsl\"?><sbml/>");
    EXPECT_EQ(text.document, declaration + "<?xml-stylesheet href=\"x.xsl\"?><sbml/>");
}

TEST(SbmlTextCheck, LocationsAreInTheSource) {
    using praenora::antimony_provider::sbml_engine_text;
    using praenora::antimony_provider::source_location;
    // libSBML's columns count from 0; the source's from 1.
    const auto declared = sbml_engine_text("\n\n   <?xml version=\"1.0\"?>\n<sbml/>");
    EXPECT_EQ(source_location(declared, 1, 4).line, 3u);
    EXPECT_EQ(source_location(declared, 1, 4).column, 8u);
    EXPECT_EQ(source_location(declared, 2, 0).line, 4u);
    EXPECT_EQ(source_location(declared, 2, 0).column, 1u);
    // The line the provider added comes first; nothing in the source is on it.
    const auto undeclared = sbml_engine_text("  <sbml/>");
    EXPECT_EQ(source_location(undeclared, 2, 5).line, 1u);
    EXPECT_EQ(source_location(undeclared, 2, 5).column, 8u);
    EXPECT_EQ(source_location(undeclared, 1, 3).line, 0u);
    EXPECT_EQ(source_location(undeclared, 0, 0).line, 0u);
}
