# dependencies.cmake:
EXECUTE_PROCESS(
        COMMAND bash -c "echo set \\\(DEPENDENCIES  >${OUTPUT};
                         eclcc -I ${sourcedir} -Md ${source} 2>/dev/null >> ${OUTPUT};
                         echo \\\) >>${OUTPUT}"

)
