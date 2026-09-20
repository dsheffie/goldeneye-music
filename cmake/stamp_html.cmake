# Put the build id into the page and onto every asset URL, so a cached script can
# never be paired with a newer page.
file(READ ${ID_H} IDH)
string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" ID "${IDH}")
string(STRIP "${ID}" ID)
# a url-safe form for the query strings; the readable one is what gets displayed
string(REGEX REPLACE "[^A-Za-z0-9]+" "-" IDQ "${ID}")

file(READ ${HTML} PAGE)
string(REPLACE "__GEMMS_V__" "${ID}" PAGE "${PAGE}")
string(REPLACE "__GEMMS_Q__" "${IDQ}" PAGE "${PAGE}")
# emscripten emits the tag unquoted: <script async src=gemms.js>
string(REPLACE "src=gemms.js>" "src=\"gemms.js?v=${IDQ}\">" PAGE "${PAGE}")
string(REPLACE "src=\"gemms.js\"" "src=\"gemms.js?v=${IDQ}\"" PAGE "${PAGE}")
file(WRITE ${HTML} "${PAGE}")
message(STATUS "stamped page: ${ID} (query ${IDQ})")
