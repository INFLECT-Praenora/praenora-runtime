// The rules of format "sbml" that need libSBML's parser (sbml_checks.hpp).
#include "sbml_checks.hpp"

#include "documents.hpp"

#include <sbml/SBMLDocument.h>
#include <sbml/SBMLError.h>
#include <sbml/SBMLReader.h>
#include <sbml/xml/XMLNamespaces.h>

#include <memory>
#include <string>

namespace praenora::antimony_provider {

namespace {

// Whether `uri` is a namespace of the comp package: every version of comp has "/comp/" in its URI.
bool is_comp(const std::string& uri) { return uri.find("/comp/") != std::string::npos; }

// Whether `uri` is a namespace of the layout or render package, as every version's URI shows.
bool is_layout(const std::string& uri) {
    return uri.find("/layout/") != std::string::npos || uri.find("/render/") != std::string::npos;
}

} // namespace

void check_sbml_document(const SbmlDocumentText& text) {
    const std::unique_ptr<libsbml::SBMLDocument> document(
        libsbml::readSBMLFromString(text.document.c_str()));
    if (document == nullptr) {
        fail(PRAENORA_STATUS_INTERNAL, "libSBML read no document");
    }
    // The packages the sbml element declares: comp in any form.
    const libsbml::XMLNamespaces* namespaces = document->getNamespaces();
    for (int i = 0; namespaces != nullptr && i < namespaces->getLength(); ++i) {
        const std::string uri = namespaces->getURI(i);
        if (is_comp(uri)) {
            refuse("comp",
                   "the SBML document declares the comp package (" + uri +
                       "), whose documents can name other documents by URI or path",
                   {{"package", uri}});
        }
    }
    if (document->isPackageEnabled("comp")) {
        refuse("comp",
               "the SBML document declares the comp package, whose documents can name other "
               "documents by URI or path",
               {{"package", "comp"}});
    }
    // libSBML's first error or fatal error.
    for (unsigned int i = 0; i < document->getNumErrors(); ++i) {
        const libsbml::SBMLError* error = document->getError(i);
        if (error != nullptr && (error->isError() || error->isFatal())) {
            const SourceLocation where =
                source_location(text, error->getLine(), error->getColumn());
            engine_failure("libsbml", "libSBML cannot read the SBML document",
                           without_trailing_white_space(error->getMessage()), where.line,
                           where.column);
        }
    }
    // A package that a Level 3 document requires. Packages exist in Level 3 only (libSBML lists
    // namespaces of its own Level 2 annotations as required, which are not packages).
    if (document->getLevel() < 3) {
        return;
    }
    const std::string core = document->getSBMLNamespaces() != nullptr
                                 ? document->getSBMLNamespaces()->getURI()
                                 : std::string();
    for (int i = 0; namespaces != nullptr && i < namespaces->getLength(); ++i) {
        const std::string uri = namespaces->getURI(i);
        if (uri != core && document->getPackageRequired(uri)) {
            refuse("package", "the SBML Level 3 document requires the package " + uri,
                   {{"package", uri}});
        }
    }
}

bool declares_layout(const std::string& sbml) {
    const std::unique_ptr<libsbml::SBMLDocument> document(
        libsbml::readSBMLFromString(sbml.c_str()));
    if (document == nullptr) {
        fail(PRAENORA_STATUS_INTERNAL, "libSBML read no document");
    }
    if (document->isPackageEnabled("layout") || document->isPackageEnabled("render")) {
        return true;
    }
    const libsbml::XMLNamespaces* namespaces = document->getNamespaces();
    for (int i = 0; namespaces != nullptr && i < namespaces->getLength(); ++i) {
        if (is_layout(namespaces->getURI(i))) {
            return true;
        }
    }
    return false;
}

} // namespace praenora::antimony_provider
