# Copies Python sources into a directory of the build tree, which holds those files and nothing
# else afterwards (it is emptied first, so a file removed from the sources goes from the copy too),
# then writes a stamp file. Run as
#   cmake -DSOURCE_ROOT=<dir> -DDESTINATION=<dir> -DLIST=<file> -DSTAMP=<file>
#         -P copy_python_sources.cmake
# where LIST holds the files' paths relative to SOURCE_ROOT, one per line, and each keeps its path
# below DESTINATION. file(COPY_FILE) keeps every byte as it is.

foreach(variable IN ITEMS SOURCE_ROOT DESTINATION LIST STAMP)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "copy_python_sources.cmake: give -D${variable}")
    endif()
endforeach()

file(STRINGS "${LIST}" files ENCODING UTF-8)
file(REMOVE_RECURSE "${DESTINATION}")
foreach(file IN LISTS files)
    get_filename_component(directory "${DESTINATION}/${file}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(COPY_FILE "${SOURCE_ROOT}/${file}" "${DESTINATION}/${file}")
endforeach()
file(TOUCH "${STAMP}")
