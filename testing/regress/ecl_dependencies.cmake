# dependencies.cmake:
EXECUTE_PROCESS(
        COMMAND bash -c "echo SET \\\(DEPENDENCIES  >${OUTPUT};
                         eclcc -E ${source} 2>/dev/null | grep sourcePath= | grep -v -i lib_ | grep -v -i std[/] | sed 's/.*sourcePath=\\\"\\([^\\\"]*\\).*/\\1/' >> ${OUTPUT};
                         echo \\\) >>${OUTPUT}"

)
