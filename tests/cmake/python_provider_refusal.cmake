# Helper of check_python_provider.cmake: with -DENGINE, composes the engine object of ENGINE
# (_praenora_python_provider_engine()); with -DARGUMENTS, calls praenora_add_python_provider(target
# <ARGUMENTS>). Either must stop the script.
include("${PRAENORA_SOURCE_DIR}/cmake/PraenoraPythonProvider.cmake")
if(DEFINED ENGINE)
    _praenora_python_provider_engine(ignored "praenora_add_python_provider(target)" "${ENGINE}"
        "{\"name\":\"praenora-runtime-pyenv\"}")
else()
    praenora_add_python_provider(target ${ARGUMENTS})
endif()
