# Writes a fresh build id header.  Run as a build step, not at configure time, so
# the id actually changes when you rebuild.
string(TIMESTAMP TS "%Y-%m-%d %H:%M" UTC)
set(REV "nogit")
execute_process(COMMAND git -C ${SRC} rev-parse --short HEAD
                OUTPUT_VARIABLE GITREV OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(GITREV)
  set(REV "${GITREV}")
endif()
set(CONTENT "#define GEMMS_BUILD_ID \"${TS}Z ${REV}\"\n")
if(EXISTS ${OUT})
  file(READ ${OUT} OLD)
else()
  set(OLD "")
endif()
if(NOT OLD STREQUAL CONTENT)
  file(WRITE ${OUT} "${CONTENT}")
endif()
