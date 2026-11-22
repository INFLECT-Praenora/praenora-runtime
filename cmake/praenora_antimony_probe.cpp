// The probe of cmake/PraenoraAntimony.cmake, built and run when the build is configured with
// PRAENORA_WITH_ANTIMONY. Linked against libAntimony, SBMLNetwork and libSBML as the antimony
// provider is, it shows that they link together, and prints the versions the libraries themselves
// report, which the provider's engine object names, and whether libSBML has the comp package:
//     libantimony=<getVersionStr()>
//     libsbml=<getLibSBMLDottedVersion()>
//     sbmlnetwork=<getLibraryVersion()>
//     comp=<1 or 0>
#include <antimony_api.h>
#include <sbml/common/libsbml-version.h>
#include <sbml/extension/SBMLExtensionRegistry.h>
#include <sbmlnetwork/libsbmlnetwork_common.h>

#include <cstdio>
#include <string>

int main() {
    const char* antimony = getVersionStr();
    const std::string sbmlnetwork = getLibraryVersion();
    std::printf("libantimony=%s\n", antimony != nullptr ? antimony : "");
    std::printf("libsbml=%s\n", libsbml::getLibSBMLDottedVersion());
    std::printf("sbmlnetwork=%s\n", sbmlnetwork.c_str());
    std::printf("comp=%d\n", libsbml::SBMLExtensionRegistry::isPackageEnabled("comp") ? 1 : 0);
    freeAll();
    return 0;
}
