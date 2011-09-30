# Define the macro that lets us build ecl

MACRO(COMPILE_WORKUNIT source)

GET_FILENAME_COMPONENT(baseName ${source} NAME_WE)

IF(NOT EXISTS ${CMAKE_BINARY_DIR}/dependencies_${baseName}.txt)
   EXECUTE_PROCESS(
        COMMAND ${CMAKE_COMMAND}
        -DTEMPLATE=${CMAKE_SOURCE_DIR}/filelist.cmake.in
        -Dsource=${source}
        -DVARIABLE=DEPENDENCIES
        -DOUTPUT=${CMAKE_BINARY_DIR}/dependencies_${baseName}.txt
        -P ${CMAKE_SOURCE_DIR}/ecl_dependencies.cmake)
ENDIF()

INCLUDE(${CMAKE_BINARY_DIR}/dependencies_${baseName}.txt)

ADD_CUSTOM_COMMAND(
    OUTPUT ${CMAKE_BINARY_DIR}/dependencies_${baseName}.txt
    COMMAND
     ${CMAKE_COMMAND}
     -DTEMPLATE=${CMAKE_SOURCE_DIR}/filelist.cmake.in
     -DVARIABLE=DEPENDENCIES
     -Dsource=${source}
     -DOUTPUT=${CMAKE_BINARY_DIR}/dependencies_${baseName}.txt
     -P ${CMAKE_SOURCE_DIR}/ecl_dependencies.cmake
#    DEPENDS ${DEPENDENCIES}
)

IF ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
  SET (USE_ECL_OPTIONS ${ECL_OPTIONS} -g -save-temps ${ECL_OPTIONS_DEBUG})
ELSE()
  SET (USE_ECL_OPTIONS ${ECL_OPTIONS} ${ECL_OPTIONS_RELEASE})
ENDIF()

ADD_CUSTOM_COMMAND(
    OUTPUT ${baseName}
    COMMAND eclcc -o${baseName} ${USE_ECL_OPTIONS} ${source}
    DEPENDS ${DEPENDENCIES})

ADD_CUSTOM_TARGET(remake_${baseName} ALL DEPENDS ${baseName} ${CMAKE_BINARY_DIR}/dependencies_${baseName}.txt)

ENDMACRO()

