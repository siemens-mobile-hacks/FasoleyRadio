# Auto-increment the build id (shown in the menu header as Dxx). Invoked before each build:
#   cmake -DNUMFILE=<path> -DOUT=<path> -P bump_build_id.cmake
if(EXISTS "${NUMFILE}")
  file(READ "${NUMFILE}" _n)
  string(STRIP "${_n}" _n)
else()
  set(_n 0)
endif()
math(EXPR _n "${_n} + 1")
file(WRITE "${NUMFILE}" "${_n}\n")
file(WRITE "${OUT}" "#define RADIO_BUILD_TAG \"D${_n}\"\n")
message(STATUS "BUILD radio D${_n}")
