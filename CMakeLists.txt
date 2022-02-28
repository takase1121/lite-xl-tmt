cmake_minimum_required(VERSION 3.10)

project(lite-xl-tmt
	VERSION 0.1.0
	DESCRIPTION "terminal for lite-xl"
	LANGUAGES C)

set(PTY_SRC minivt.c minivt.h)
if(WIN32)
	find_package(winpty REQUIRED)
	set(PTY_SRC ${PTY_SRC} pty.win.c)
	set(PTY_LIB ${WINPTY_LIBRARIES})
	set(BEL_LIB winmm.lib)
else()
	set(PTY_SRC ${PTY_SRC} pty.c)
	set(PTY_LIB util)
	set(BEL_LIB dl)
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "disable libtsm.so")
add_subdirectory(libtsm EXCLUDE_FROM_ALL)
find_package(Lua REQUIRED)

add_executable(pty ${PTY_SRC})
target_link_libraries(pty PUBLIC ${PTY_LIB})

add_library(luaterm MODULE luaterm.c luatsm.c bel.c)
target_compile_options(luaterm PUBLIC -Wall -Wextra -Wno-unused-parameter -pedantic -Werror)
target_link_libraries(luaterm PUBLIC tsm ${BEL_LIB})
set_target_properties(luaterm PROPERTIES
	PREFIX ""
	POSITION_INDEPENDENT_CODE ON
)

install(TARGETS pty RUNTIME DESTINATION .)
install(TARGETS luaterm LIBRARY DESTINATION .)
install(FILES init.lua DESTINATION .)
