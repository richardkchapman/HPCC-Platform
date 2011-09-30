# dependencies.cmake:
EXECUTE_PROCESS(
        COMMAND bash -c "eclcc -E ${source} 2>/dev/null | grep sourcePath= | grep -v -i lib_ | grep -v -i std[/] | sed 's/.*sourcePath=\\\"\\([^\\\"]*\\).*/\\1/'"
        OUTPUT_VARIABLE FILES
)
#SET (FILES hello.ecl world.ecl)
#message("FILES ${FILES}")
CONFIGURE_FILE(${TEMPLATE} ${OUTPUT} @ONLY)
