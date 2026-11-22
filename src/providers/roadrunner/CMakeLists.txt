# The roadrunner service provider: praenora.simulation@0.0 (include/praenora/sdk/simulation.h)
# for SBML models, simulated with libRoadRunner's C++ API (docs/specs/simulation-service.md, its
# section on the roadrunner provider). Built only with PRAENORA_WITH_ROADRUNNER, after
# praenora_find_roadrunner() (cmake/PraenoraRoadrunner.cmake) made libRoadRunner available.
#
# libRoadRunner, LLVM, libSBML, SUNDIALS and their other dependencies are linked statically and
# PRIVATE into the provider library, whose only exported symbol is the provider entry point
# (praenora_add_provider(), cmake/PraenoraProvider.cmake). The engine object of its manifest
# names what the build pins: the libRoadRunner, LLVM, SUNDIALS and libSBML versions, LLVM's MCJit
# backend and no LLVM optimization passes; describe() reports the same object.
#
# The library and its manifest go where the build tree's runtime library looks for the providers of
# its installation (<directory of the runtime library>/praenora/providers/roadrunner; on Windows
# <runtime DLL directory>/../lib/praenora/providers/roadrunner), so the build tree's praenora tool
# and runtime find the provider without a provider path, as an installation does. They are
# installed to ${CMAKE_INSTALL_LIBDIR}/praenora/providers/roadrunner.

# Where the build tree's runtime library looks for its providers (cmake/PraenoraProvider.cmake).
praenora_build_tree_provider_dir(_praenora_rr_providers_dir)
set(_praenora_rr_dir "${_praenora_rr_providers_dir}/roadrunner")

set(_praenora_rr_install "")
if(PRAENORA_INSTALL_RULES)
    set(_praenora_rr_install
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/praenora/providers/roadrunner")
endif()

praenora_add_provider(praenora_provider_roadrunner
    NAME roadrunner
    VERSION "${PRAENORA_RUNTIME_VERSION}"
    SERVICES praenora.simulation@0.0
    ENGINE_JSON "{\"name\": \"libroadrunner\", \"version\": \"${PRAENORA_ROADRUNNER_VERSION}\",
        \"llvm\": \"${PRAENORA_LLVM_VERSION}\", \"jit\": \"mcjit\",
        \"llvm_optimization\": \"none\", \"sundials\": \"${PRAENORA_SUNDIALS_VERSION}\",
        \"libsbml\": \"${PRAENORA_LIBSBML_VERSION}\"}"
    SOURCES
        roadrunner_provider.cpp
        documents.hpp
        documents.cpp
        engine.hpp
        engine.cpp
    OUTPUT_DIRECTORY "${_praenora_rr_dir}"
    LINK_LIBRARIES praenora_roadrunner_engine nlohmann_json::nlohmann_json
    ${_praenora_rr_install})
# The versions the manifest names, checked against the headers the provider is compiled with.
string(REPLACE "." ";" _praenora_rr_version_parts "${PRAENORA_ROADRUNNER_VERSION}")
list(GET _praenora_rr_version_parts 0 _praenora_rr_major)
list(GET _praenora_rr_version_parts 1 _praenora_rr_minor)
list(GET _praenora_rr_version_parts 2 _praenora_rr_patch)
target_compile_definitions(praenora_provider_roadrunner PRIVATE
    PRAENORA_ROADRUNNER_VERSION_MAJOR=${_praenora_rr_major}
    PRAENORA_ROADRUNNER_VERSION_MINOR=${_praenora_rr_minor}
    PRAENORA_ROADRUNNER_VERSION_PATCH=${_praenora_rr_patch})
praenora_target_warnings(praenora_provider_roadrunner)
