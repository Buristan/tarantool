set(CMAKE_REQUIRED_FLAGS "-fprofile-arcs -ftest-coverage")
if ((CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 11) OR
    (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 12))
    check_library_exists("" __gcov_dump "" HAVE_GCOV)
    if (HAVE_GCOV)
        check_library_exists("" __gcov_reset "" HAVE_GCOV)
      endif()
else()
    check_library_exists("" __gcov_flush "" HAVE_GCOV)
endif()
set(CMAKE_REQUIRED_FLAGS "")

set(ENABLE_GCOV_DEFAULT OFF)
option(ENABLE_GCOV "Enable integration with gcov, a code coverage program" ${ENABLE_GCOV_DEFAULT})

if (ENABLE_GCOV)
    if (NOT HAVE_GCOV)
    message (FATAL_ERROR
         "ENABLE_GCOV option requested but gcov library is not found")
    endif()

    add_compile_flags("C;CXX"
        "-fprofile-arcs"
        "-ftest-coverage"
    )

    set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fprofile-arcs")
    set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -ftest-coverage")
    set (CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fprofile-arcs")
    set (CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -ftest-coverage")

   # add_library(gcov SHARED IMPORTED)
endif()

if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(ENABLE_GPROF_DEFAULT ON)
else()
    set(ENABLE_GPROF_DEFAULT OFF)
endif()
option(ENABLE_GPROF "Enable integration with gprof, a performance analyzing tool" ${GPROF_DEFAULT})

if (ENABLE_GPROF)
    add_compile_flags("C;CXX" "-pg")
endif()

option(ENABLE_VALGRIND "Enable integration with valgrind, a memory analyzing tool" OFF)
if (ENABLE_VALGRIND)
    add_definitions(-UNVALGRIND)
else()
    add_definitions(-DNVALGRIND=1)
endif()

option(OSS_FUZZ "Set this option to use flags by oss-fuzz" OFF)
option(ENABLE_FUZZER "Enable fuzzing testing" OFF)
option(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION "Enable fuzzing-friendly mode" ${ENABLE_FUZZER})
if(ENABLE_FUZZER)
    if (CMAKE_COMPILER_IS_GNUCC)
        message(FATAL_ERROR
            "\n"
            "Fuzzing is unsupported with GCC compiler. Use Clang:\n"
            " $ git clean -xfd; git submodule foreach --recursive git clean -xfd\n"
            " $ CC=clang CXX=clang++ cmake . <...> -DENABLE_FUZZER=ON && make -j\n"
            "\n")
    endif()
    if(OSS_FUZZ AND NOT DEFINED ENV{LIB_FUZZING_ENGINE})
        message(FATAL_ERROR
            "OSS-Fuzz builds require the environment variable "
            "LIB_FUZZING_ENGINE to be set. If you are seeing this "
            "warning, it points to a deeper problem in the ossfuzz "
            "build setup.")
    endif()
    if (NOT OSS_FUZZ)
        add_compile_flags("C;CXX" -fsanitize=fuzzer-no-link)
    endif()
endif()

option(ENABLE_ASAN "Enable AddressSanitizer, a fast memory error detector based on compiler instrumentation" OFF)
if (ENABLE_ASAN)
    # AddressSanitizer has been added to GCC since version 4.8.0,
    # see https://gcc.gnu.org/gcc-4.8/changes.html.
    if (CMAKE_COMPILER_IS_GNUCC AND CMAKE_C_COMPILER_VERSION VERSION_LESS 4.8.0)
        message(FATAL_ERROR
            "\n"
            " GCC has AddressSanitizer support since 4.8.0. Update GCC version\n"
            " or use Clang:\n"
            " $ git clean -xfd; git submodule foreach --recursive git clean -xfd\n"
            " $ CC=clang CXX=clang++ cmake . <...> -DENABLE_ASAN=ON && make -j\n"
            "\n")
    endif()

    set(ASAN_FLAGS "-fsanitize=address")
    if (CMAKE_C_COMPILER STREQUAL "Clang")
        # Bug 61978 - implement blacklist for sanitizer,
        # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=61978
        set(ASAN_FLAGS "${ASAN_FLAGS} -fsanitize-blacklist=${PROJECT_SOURCE_DIR}/asan/asan.supp")
    endif()
    set(CMAKE_REQUIRED_FLAGS ${ASAN_FLAGS})
    check_c_source_compiles("int main(void) {
        #include <sanitizer/asan_interface.h>
        void *x;
	    __sanitizer_finish_switch_fiber(x);
        return 0;
        }" ASAN_INTERFACE_OLD)
    check_c_source_compiles("int main(void) {
        #include <sanitizer/asan_interface.h>
        void *x;
	    __sanitizer_finish_switch_fiber(x, 0, 0);
        return 0;
    }" ASAN_INTERFACE_NEW)
    set(CMAKE_REQUIRED_FLAGS "")

    if (ASAN_INTERFACE_OLD)
        add_definitions(-DASAN_INTERFACE_OLD=1)
    elseif (ASAN_INTERFACE_NEW)
        add_definitions(-UASAN_INTERFACE_OLD)
    else()
        message(FATAL_ERROR "Cannot enable AddressSanitizer")
    endif()

    add_compile_flags("C;CXX" ${ASAN_FLAGS})
endif()
