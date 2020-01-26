# A macro to build the bundled libujit
macro(ujit_build)
    set(LIBUJIT_SOURCE_DIR ${PROJECT_SOURCE_DIR}/third_party/luavela)
    set(LIBUJIT_BINARY_DIR ${PROJECT_BINARY_DIR}/build/luavela/work)
    set(LIBUJIT_INSTALL_DIR ${PROJECT_BINARY_DIR}/build/luavela/dest)

    # Add our LUA extensions
    set(LIBUJIT_EXTENTIONS ${PROJECT_SOURCE_DIR}/src/lua/extentions/)
    set(LIBUJIT_PATCH ${PROJECT_SOURCE_DIR}/src/lua/extentions/luavela.patch)
    # FIXME !!! REDO WITH PATCH_COMMAND INSIDE ExternalProject_Add
    if (EXISTS "${CMAKE_SOURCE_DIR}/.git" AND GIT)
        execute_process(COMMAND cp ${LIBUJIT_EXTENTIONS}/lib_tnt_luavela_ext.c ${LIBUJIT_SOURCE_DIR}/src/lib_tnt_ext.c
                        COMMAND cp ${LIBUJIT_EXTENTIONS}/lib_tnt_luavela_ext.h ${LIBUJIT_SOURCE_DIR}/src/lib_tnt_ext.h
                        COMMAND ${GIT} apply ${LIBUJIT_PATCH}
                        WORKING_DIRECTORY "${LIBUJIT_SOURCE_DIR}"
                        RESULT_VARIABLE patch_rv
                        ERROR_VARIABLE patch_ev
        )
        if (NOT patch_rv STREQUAL "0")
            message(FATAL_ERROR "${GIT} apply ${LIBUJIT_PATCH} failed: ${patch_ev}")
        endif()
    else()
        message(FATAL_ERROR "No git available to apply patch for extensions")
    endif()

    include(ExternalProject)
    ExternalProject_Add(
        bundled-libujit-project
        SOURCE_DIR ${LIBUJIT_SOURCE_DIR}
        PREFIX ${LIBUJIT_INSTALL_DIR}
        DOWNLOAD_DIR ${LIBUJIT_BINARY_DIR}
        TMP_DIR ${LIBUJIT_BINARY_DIR}/tmp
        STAMP_DIR ${LIBUJIT_BINARY_DIR}/stamp
        BINARY_DIR ${LIBUJIT_BINARY_DIR}
        INSTALL_DIR ${LIBUJIT_INSTALL_DIR}

        BUILD_COMMAND  cd <SOURCE_DIR> && $(MAKE) # && ${GIT} apply -R ${LIBUJIT_PATCH}
        INSTALL_COMMAND cd <SOURCE_DIR> && $(MAKE) install
    )

    add_library(libluajit STATIC IMPORTED GLOBAL)
    set_target_properties(libluajit PROPERTIES IMPORTED_LOCATION
        ${LIBUJIT_INSTALL_DIR}/lib/libujit.a)
    add_dependencies(libluajit bundled-libujit-project)
    add_dependencies(build_bundled_libs libluajit)


    set (inc ${PROJECT_SOURCE_DIR}/third_party/luavela/src)
    install (FILES ${inc}/lua.h ${inc}/lualib.h ${inc}/lauxlib.h
        ${inc}/luaconf.h ${inc}/lua.hpp ${inc}/luajit.h ${inc}/lib_tnt_ext.h
        DESTINATION ${MODULE_INCLUDEDIR})
    unset(LIBUJIT_INSTALL_DIR)
    unset(LIBUJIT_BINARY_DIR)
    unset(LIBUJIT_SOURCE_DIR)
    set(LUAJIT_LIB "${PROJECT_BINARY_DIR}/third_party/luavela/src/libujit.a")
    set(LUAJIT_INCLUDE "${PROJECT_BINARY_DIR}/third_party/luavela/src/")
    set(LUAJIT_INCLUDE_DIRS ${LUAJIT_INCLUDE})
    set(LUAJIT_LIBRARIES ${LUAJIT_LIB})
    find_package_handle_standard_args(LuaJIT
        REQUIRED_VARS LUAJIT_INCLUDE LUAJIT_LIB)
endmacro(ujit_build)

# vim: et sw=4 ts=4 sts=4:
