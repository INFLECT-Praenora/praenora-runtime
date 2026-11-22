// The rules of format "sbml" that need libSBML's parser (sbml_checks.cpp), applied under the
// provider's lock before libAntimony reads the document (docs/specs/model-conversion-service.md,
// the antimony provider's section). libSBML's types stay in sbml_checks.cpp.
#ifndef PRAENORA_PROVIDERS_ANTIMONY_SBML_CHECKS_HPP
#define PRAENORA_PROVIDERS_ANTIMONY_SBML_CHECKS_HPP

#include "source_checks.hpp"

#include <string>

namespace praenora::antimony_provider {

// Reads the document with libSBML and applies, in order:
// - rule "comp": a document whose sbml element declares the comp package, whose documents can
//   name other documents by URI or path (first: libSBML itself reports a comp declaration that is
//   not required as an error);
// - libSBML's first error or fatal error: DATA_INVALID with details.engine_error
//   {"type": "libsbml", "message"}, details.line and details.column located in the source;
// - rule "package": a Level 3 document that requires a package (required="true").
// Throws the Failure of the first; returns when the document passes. Not thread-safe: called under
// the provider's lock only.
void check_sbml_document(const SbmlDocumentText& text);

// Whether the SBML document `sbml` (one that libAntimony wrote) declares the layout or render
// package: a layout that SBMLNetwork computed. Read with libSBML; under the provider's lock only.
bool declares_layout(const std::string& sbml);

} // namespace praenora::antimony_provider

#endif // PRAENORA_PROVIDERS_ANTIMONY_SBML_CHECKS_HPP
