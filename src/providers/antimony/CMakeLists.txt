# The antimony service provider: praenora.model-conversion@0.0
# (include/praenora/sdk/model_conversion.h) for Antimony and SBML, translated with libAntimony
# (docs/specs/model-conversion-service.md, its section on the antimony provider). Built only with
# PRAENORA_WITH_ANTIMONY, after praenora_find_antimony() (cmake/PraenoraAntimony.cmake) made
# libAntimony available.
#
# libAntimony, SBMLNetwork, libSBML and expat are linked statically and PRIVATE into the provider
# library, whose only exported symbol is the provider entry point (praenora_add_provider(),
# cmake/PraenoraProvider.cmake). The engine object of its manifest names the versions the build's
# probe had those libraries report and that function definitions are kept; describe() reports the
# same object, and start() checks it against the libraries the provider links.
#
# The library and its manifest go where the build tree's runtime library looks for the providers of
# its installation (<directory of the runtime library>/praenora/providers/antimony; on Windows
# <runtime DLL directory>/../lib/praenora/providers/antimony), so the build tree's praenora tool
# and runtime find the provider without a provider path, as an installation does. They are
# installed to ${CMAKE_INSTALL_LIBDIR}/praenora/providers/antimony.

# Where the build tree's runtime library looks for its providers (cmake/PraenoraProvider.cmake).
praenora_build_tree_provider_dir(_praenora_ant_providers_dir)
set(_praenora_ant_dir "${_praenora_ant_providers_dir}/antimony")

set(_praenora_ant_install "")
if(PRAENORA_INSTALL_RULES)
    set(_praenora_ant_install
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/praenora/providers/antimony")
endif()

praenora_add_provider(praenora_provider_antimony
    NAME antimony
    VERSION "${PRAENORA_RUNTIME_VERSION}"
    SERVICES praenora.model-conversion@0.0
    ENGINE_JSON "{\"name\": \"libantimony\", \"version\": \"${PRAENORA_ANTIMONY_VERSION}\",
        \"libsbml\": \"${PRAENORA_ANTIMONY_LIBSBML_VERSION}\",
        \"sbmlnetwork\": \"${PRAENORA_SBMLNETWORK_VERSION}\", \"function_definitions\": \"kept\"}"
    SOURCES
        antimony_provider.cpp
        documents.hpp
        documents.cpp
        engine.hpp
        engine.cpp
        sbml_checks.hpp
        sbml_checks.cpp
        source_checks.hpp
        source_checks.cpp
        ../common/cancellable_lock.hpp
    OUTPUT_DIRECTORY "${_praenora_ant_dir}"
    LINK_LIBRARIES praenora_antimony_engine nlohmann_json::nlohmann_json
    ${_praenora_ant_install})
# The lock, shared with the fixture provider test-conversion (src/providers/common).
target_include_directories(praenora_provider_antimony PRIVATE
    "${PROJECT_SOURCE_DIR}/src/providers/common")
praenora_target_warnings(praenora_provider_antimony)
