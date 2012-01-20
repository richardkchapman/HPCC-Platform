# Define the macro that lets us build ecl

MACRO(COMPILE_WORKUNIT source)

GET_FILENAME_COMPONENT(baseName ${source} NAME_WE)

SET (DEPENDENCIES_DIR "${CMAKE_BINARY_DIR}/dependencies")
FILE (MAKE_DIRECTORY ${DEPENDENCIES_DIR})

IF(NOT EXISTS ${DEPENDENCIES_DIR}/${baseName}.dep)
  MESSAGE ("Checking dependencies for ${baseName}")
  SET (OUTPUT ${DEPENDENCIES_DIR}/${baseName}.dep)
  EXECUTE_PROCESS(
        COMMAND bash -c "echo SET \\\(DEPENDENCIES  >${OUTPUT};
                         eclcc -I ${CMAKE_CURRENT_SOURCE_DIR} -Md ${source} 2>/dev/null >> ${OUTPUT};
                         echo \\\) >>${OUTPUT}"
  )
ENDIF()

INCLUDE(${DEPENDENCIES_DIR}/${baseName}.dep)

ADD_CUSTOM_COMMAND(
    OUTPUT ${DEPENDENCIES_DIR}/${baseName}.dep
    COMMAND
    ${CMAKE_COMMAND}
     -DTEMPLATE=${CMAKE_SOURCE_DIR}/filelist.cmake.in
     -DVARIABLE=DEPENDENCIES
     -Dsource=${source}
     -Dsourcedir=${CMAKE_CURRENT_SOURCE_DIR}
     -DOUTPUT=${DEPENDENCIES_DIR}/${baseName}.dep
     -P ${CMAKE_SOURCE_DIR}/ecl_dependencies.cmake
    DEPENDS ${DEPENDENCIES}
)

IF ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
  SET (USE_ECL_OPTIONS ${ECL_OPTIONS} -g -save-temps -I ${CMAKE_CURRENT_SOURCE_DIR} ${ECL_OPTIONS_DEBUG})
ELSE()
  SET (USE_ECL_OPTIONS ${ECL_OPTIONS} -I ${CMAKE_CURRENT_SOURCE_DIR} ${ECL_OPTIONS_RELEASE})
ENDIF()

IF (REGRESS_LOCAL)
  SET (BINARY_DIR "${CMAKE_BINARY_DIR}/bin")
  FILE (MAKE_DIRECTORY ${BINARY_DIR})
  ADD_CUSTOM_COMMAND(
    OUTPUT ${baseName}.out
    COMMAND eclcc -o${baseName}.out ${USE_ECL_OPTIONS} ${source}
    DEPENDS ${DEPENDENCIES})

  ADD_CUSTOM_TARGET(${baseName} ALL DEPENDS ${baseName}.out ${DEPENDENCIES_DIR}/${baseName}.dep)
ELSE()
  SET (ARCHIVE_DIR "${CMAKE_BINARY_DIR}/archive")
  FILE (MAKE_DIRECTORY ${ARCHIVE_DIR})
  SET (DEPLOY_DIR "${CMAKE_BINARY_DIR}/deploy")
  FILE (MAKE_DIRECTORY ${DEPLOY_DIR})

  ADD_CUSTOM_COMMAND(
    OUTPUT ${ARCHIVE_DIR}/${baseName}.xml
    COMMAND eclcc -o${ARCHIVE_DIR}/${baseName}.xml -E ${USE_ECL_OPTIONS} ${source}
    DEPENDS ${DEPENDENCIES})

  ADD_CUSTOM_COMMAND(
    OUTPUT ${DEPLOY_DIR}/${baseName}.log
    COMMAND ecl publish ${ARCHIVE_DIR}/${baseName}.xml --activate --cluster=${REGRESS_CLUSTER} --name=${baseName} > ${DEPLOY_DIR}/${baseName}.log
    DEPENDS ${ARCHIVE_DIR}/${baseName}.xml ${DEPENDENCIES_DIR}/${baseName}.dep)

  ADD_CUSTOM_TARGET(${baseName} ALL DEPENDS ${DEPLOY_DIR}/${baseName}.log)
ENDIF()

ENDMACRO()
