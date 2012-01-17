# CMake file to build the ecl regression suite

CMAKE_MINIMUM_REQUIRED(VERSION 2.8 FATAL_ERROR)
PROJECT(EclRegressionSuite)

IF( NOT REGRESS_CLUSTER )
  SET ( REGRESS_LOCAL 1)
ENDIF()

INCLUDE (ecl_compile.cmake)

FILE(GLOB ECL_FILES "*.ecl")
FOREACH ( LOOP_VAR ${ECL_FILES} )
 compile_workunit(${LOOP_VAR})
ENDFOREACH ( LOOP_VAR ${ECL_FILES} )
