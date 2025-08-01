cmake_minimum_required(VERSION 3.19)
file(GLOB LUA_HEADERS CONFIGURE_DEPENDS *.h)
file(GLOB LUA_SOURCES CONFIGURE_DEPENDS *.c)

add_library(LUA STATIC
    ${LUA_SOURCES}
    ${LUA_HEADERS}
)

if (UNIX)
    # for Linux, BSD, Solaris, Minix, Apple
    target_compile_definitions(LUA PUBLIC LUA_USE_POSIX=1)
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(LUA PRIVATE
        -Wno-string-plus-int
    )
endif()

set_target_properties(LUA PROPERTIES
    FOLDER "ThirdParty"
    POSITION_INDEPENDENT_CODE ${WICKED_PIC}
)

install(FILES ${LUA_HEADERS}
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/WickedEngine/LUA/")
