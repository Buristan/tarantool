# A macro to build the bundled libcares
macro(ares_build)
    set(ARES_SOURCE_DIR ${PROJECT_SOURCE_DIR}/third_party/c-ares)
    set(ARES_BINARY_DIR ${PROJECT_BINARY_DIR}/build/ares/work)
    set(ARES_INSTALL_DIR ${PROJECT_BINARY_DIR}/build/ares/dest)

    # See BuildLibCURL.cmake for details.
    set(ARES_CPPFLAGS "")
    set(ARES_CFLAGS "")
    set(ARES_CXXFLAGS "")
    if (TARGET_OS_DARWIN AND NOT "${CMAKE_OSX_SYSROOT}" STREQUAL "")
        set(ARES_CPPFLAGS "${ARES_CPPFLAGS} ${CMAKE_C_SYSROOT_FLAG} ${CMAKE_OSX_SYSROOT}")
        set(ARES_CFLAGS "${ARES_CFLAGS} ${CMAKE_C_SYSROOT_FLAG} ${CMAKE_OSX_SYSROOT}")
        set(ARES_CXXFLAGS "${ARES_CXXFLAGS} ${CMAKE_CXX_SYSROOT_FLAG} ${CMAKE_OSX_SYSROOT}")
    endif()

    include(ExternalProject)
    ExternalProject_Add(
        bundled-ares-project
        SOURCE_DIR ${ARES_SOURCE_DIR}
        PREFIX ${ARES_INSTALL_DIR}
        DOWNLOAD_DIR ${ARES_BINARY_DIR}
        TMP_DIR ${ARES_BINARY_DIR}/tmp
        STAMP_DIR ${ARES_BINARY_DIR}/stamp
        BINARY_DIR ${ARES_BINARY_DIR}
        CONFIGURE_COMMAND
            cd <SOURCE_DIR> && ./buildconf &&
            cd <BINARY_DIR> && <SOURCE_DIR>/configure
                # Pass the same toolchain as is used to build
                # tarantool itself, because they can be
                # incompatible.
                CC=${CMAKE_C_COMPILER}
                CXX=${CMAKE_CXX_COMPILER}
                LD=${CMAKE_LINKER}
                AR=${CMAKE_AR}
                RANLIB=${CMAKE_RANLIB}
                NM=${CMAKE_NM}
                STRIP=${CMAKE_STRIP}

                CFLAGS=${ARES_CFLAGS}
                CPPFLAGS=${ARES_CPPFLAGS}
                CXXFLAGS=${ARES_CXXFLAGS}
                # Pass empty LDFLAGS to discard using of
                # corresponding environment variable.
                # It is possible that a linker flag assumes that
                # some compilation flag is set. We don't pass
                # CFLAGS from environment, so we should not do it
                # for LDFLAGS too.
                LDFLAGS=

                --prefix=<INSTALL_DIR>
                --enable-static

        BUILD_COMMAND cd <BINARY_DIR> && $(MAKE)
        INSTALL_COMMAND cd <BINARY_DIR> && $(MAKE) install)

    add_library(bundled-ares STATIC IMPORTED GLOBAL)
    set_target_properties(bundled-ares PROPERTIES IMPORTED_LOCATION
        ${ARES_INSTALL_DIR}/lib/libcares.a)
    add_dependencies(bundled-ares bundled-ares-project)

    unset(ARES_CPPFLAGS)
    unset(ARES_CFLAGS)
    unset(ARES_CXXFLAGS)
    unset(ARES_BINARY_DIR)
    unset(ARES_SOURCE_DIR)
endmacro(ares_build)
