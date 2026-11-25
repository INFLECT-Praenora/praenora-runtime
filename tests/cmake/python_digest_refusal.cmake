# Helper of check_python_digest.cmake: calls praenora_python_directory_digest() with the
# arguments of -DARGUMENTS, which it must refuse.
include("${PRAENORA_SOURCE_DIR}/cmake/PraenoraPythonDigest.cmake")
praenora_python_directory_digest(ignored ${ARGUMENTS})
