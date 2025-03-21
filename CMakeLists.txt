cmake_minimum_required(VERSION 2.8.12)

if(POLICY CMP0026)
  cmake_policy(SET CMP0026 NEW)
endif()

if(POLICY CMP0051)
  cmake_policy(SET CMP0051 NEW)
endif()

if(POLICY CMP0054)
  cmake_policy(SET CMP0054 NEW)
endif()

project(DuckDB)

find_package(Threads REQUIRED)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (CMAKE_VERSION VERSION_LESS "3.1")
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
else ()
  set (CMAKE_CXX_STANDARD 11)
endif ()


set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_VERBOSE_MAKEFILE OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_MACOSX_RPATH 1)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
else()
  find_program(CCACHE_PROGRAM sccache)
  if(CCACHE_PROGRAM)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
  endif()
endif()

# Determine install paths
set(INSTALL_LIB_DIR
    lib
    CACHE PATH "Installation directory for libraries")
set(INSTALL_BIN_DIR
    bin
    CACHE PATH "Installation directory for executables")
set(INSTALL_INCLUDE_DIR
    include
    CACHE PATH "Installation directory for header files")
if(WIN32 AND NOT CYGWIN)
  set(DEF_INSTALL_CMAKE_DIR cmake)
else()
  set(DEF_INSTALL_CMAKE_DIR lib/cmake/DuckDB)
endif()
set(INSTALL_CMAKE_DIR
    ${DEF_INSTALL_CMAKE_DIR}
    CACHE PATH "Installation directory for CMake files")
set(DUCKDB_EXPORT_SET "DuckDBExports")

# Make relative install paths absolute
foreach(p LIB BIN INCLUDE CMAKE)
  set(var INSTALL_${p}_DIR)
  if(NOT IS_ABSOLUTE "${${var}}")
    set(${var} "${CMAKE_INSTALL_PREFIX}/${${var}}")
  endif()
endforeach()

# This option allows --gc-sections flag during extension linking to discard any unused functions or data
if (EXTENSION_STATIC_BUILD AND "${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -ffunction-sections -fdata-sections")
  elseif(WIN32 AND MVSC)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Gy")
  endif()
endif()

option(DISABLE_UNITY "Disable unity builds." FALSE)

option(FORCE_COLORED_OUTPUT
       "Always produce ANSI-colored output (GNU/Clang only)." FALSE)
if(${FORCE_COLORED_OUTPUT})
  if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    add_compile_options(-fdiagnostics-color=always)
  elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang$")
    add_compile_options(-fcolor-diagnostics)
  endif()
endif()

option("Enable address sanitizer." TRUE)

set(M32_FLAG "")
if(FORCE_32_BIT)
  set(M32_FLAG " -m32 ")
endif()

set(OS_NAME "unknown")
set(OS_ARCH "amd64")

string(REGEX MATCH "(arm64|aarch64)" IS_ARM "${CMAKE_SYSTEM_PROCESSOR}")
if(IS_ARM)
  set(OS_ARCH "arm64")
elseif(FORCE_32_BIT)
  set(OS_ARCH "i386")
endif()

if(APPLE)
  set(OS_NAME "osx")
endif()
if(WIN32)
  set(OS_NAME "windows")
endif()
if(UNIX AND NOT APPLE)
  set(OS_NAME "linux") # sorry BSD
endif()

option(FORCE_WARN_UNUSED "Unused code objects lead to compiler warnings." FALSE)

option(WASM_LOADABLE_EXTENSIONS "WebAssembly build with loadable extensions." FALSE)
option(ENABLE_SANITIZER "Enable address sanitizer." TRUE)
option(ENABLE_THREAD_SANITIZER "Enable thread sanitizer." FALSE)
option(ENABLE_UBSAN "Enable undefined behavior sanitizer." TRUE)
option(DISABLE_VPTR_SANITIZER "Disable vptr sanitizer; work-around for sanitizer false positive on Macbook M1" FALSE)
option(
  FORCE_SANITIZER
  "Forces building with sanitizers even if the Python and R modules are enabled."
  FALSE)
if((BUILD_PYTHON OR BUILD_R OR BUILD_NODE OR CONFIGURE_R)
   AND (ENABLE_SANITIZER OR ENABLE_UBSAN)
   AND ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug"))
  if(FORCE_SANITIZER)
    message(
      WARNING
        "FORCE_SANITIZER is set and the Python/R/Node builds are enabled. Sanitizers will be linked as a shared library (-shared-libasan). You may need to do LD_PRELOAD tricks to load packages built in this way."
    )
    set(CXX_EXTRA_DEBUG "${CXX_EXTRA_DEBUG} -shared-libasan")
  else()
    message(
      WARNING
        "Sanitizers are enabled but will not be built because the Python/R builds are enabled. Use FORCE_SANITIZER to force building of the sanitizers even when building these packages."
    )
    set(ENABLE_SANITIZER FALSE)
    set(ENABLE_UBSAN FALSE)
  endif()
endif()
if(${ENABLE_THREAD_SANITIZER})
  if(${ENABLE_SANITIZER})
    message(
      WARNING
        "Both thread and address sanitizers are enabled. This is not supported. The address sanitizer will be disabled, and we will run with only the thread sanitizer."
    )
  endif()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_THREAD_SANITIZER")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread")
elseif(${ENABLE_SANITIZER})
  if(FORCE_ASSERT)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
  else()
    set(CXX_EXTRA_DEBUG "${CXX_EXTRA_DEBUG} -fsanitize=address")
  endif()
endif()


if (${DISABLE_VPTR_SANITIZER})
else()
  if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    if("${CMAKE_CXX_COMPILER_VERSION}" VERSION_GREATER 14.0)
      message(
        WARNING
          "Not disabling vptr sanitizer on M1 Macbook - set DISABLE_VPTR_SANITIZER manually if you run into issues with false positives in the sanitizer"
      )
    else()
    set(DISABLE_VPTR_SANITIZER TRUE)
    endif()
  endif()
endif()

if(${ENABLE_UBSAN})
  if(${ENABLE_THREAD_SANITIZER})
    message(
      WARNING
        "Both thread and undefined sanitizers are enabled. This is not supported. The undefined sanitizer will be disabled, and we will run with only the thread sanitizer."
    )
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_THREAD_SANITIZER")
  else()
    if(FORCE_ASSERT)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=undefined -fno-sanitize-recover=all")
      if (${DISABLE_VPTR_SANITIZER})
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-sanitize=vptr")
      endif()
    else()
      set(CXX_EXTRA_DEBUG "${CXX_EXTRA_DEBUG} -fsanitize=undefined -fno-sanitize-recover=all")
      if (${DISABLE_VPTR_SANITIZER})
        set(CXX_EXTRA_DEBUG "${CXX_EXTRA_DEBUG} -fno-sanitize=vptr")
      endif()
    endif()
  endif()
endif()

option(EXPLICIT_EXCEPTIONS "Explicitly enable C++ exceptions." FALSE)
if(${EXPLICIT_EXCEPTIONS})
  set(CXX_EXTRA "${CXX_EXTRA} -fexceptions")
endif()

option(OSX_BUILD_UNIVERSAL "Build both architectures on OSX and create a single binary containing both." FALSE)
if (OSX_BUILD_UNIVERSAL)
  if (NOT APPLE)
    error("This only makes sense on OSX")
  endif()
  SET(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for Mac OS X" FORCE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "Minimum OS X deployment version" FORCE)
endif()

set(SUN FALSE)
if(${CMAKE_SYSTEM_NAME} STREQUAL "SunOS")
  set(CXX_EXTRA "${CXX_EXTRA} -mimpure-text")
  add_definitions(-DSUN=1)
  set(SUN TRUE)
endif()

find_package(Git)
if(Git_FOUND)
  if (NOT DEFINED GIT_COMMIT_HASH)
    execute_process(
            COMMAND ${GIT_EXECUTABLE} log -1 --format=%h
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            RESULT_VARIABLE GIT_RESULT
            OUTPUT_VARIABLE GIT_COMMIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE)
  endif()
  execute_process(
          COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0
          WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
          OUTPUT_VARIABLE GIT_LAST_TAG
          OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(
          COMMAND ${GIT_EXECUTABLE} describe --tags --long
          WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
          OUTPUT_VARIABLE GIT_ITERATION
          OUTPUT_STRIP_TRAILING_WHITESPACE)
else()
  message("Git NOT FOUND")
endif()

if(GIT_RESULT EQUAL "0")
  string(REGEX REPLACE "v([0-9]+).[0-9]+.[0-9]+" "\\1" DUCKDB_MAJOR_VERSION "${GIT_LAST_TAG}")
  string(REGEX REPLACE "v[0-9]+.([0-9]+).[0-9]+" "\\1" DUCKDB_MINOR_VERSION "${GIT_LAST_TAG}")
  string(REGEX REPLACE "v[0-9]+.[0-9]+.([0-9]+)" "\\1" DUCKDB_PATCH_VERSION "${GIT_LAST_TAG}")
  string(REGEX REPLACE ".*-([0-9]+)-.*" "\\1" DUCKDB_DEV_ITERATION "${GIT_ITERATION}")

  if(DUCKDB_DEV_ITERATION EQUAL 0)
    # on a tag; directly use the version
    set(DUCKDB_VERSION "${GIT_LAST_TAG}")
  else()
    # not on a tag, increment the patch version by one and add a -devX suffix
    math(EXPR DUCKDB_PATCH_VERSION "${DUCKDB_PATCH_VERSION}+1")
    set(DUCKDB_VERSION "v${DUCKDB_MAJOR_VERSION}.${DUCKDB_MINOR_VERSION}.${DUCKDB_PATCH_VERSION}-dev${DUCKDB_DEV_ITERATION}")
  endif()
else()
  # fallback for when building from tarball
  set(DUCKDB_MAJOR_VERSION 0)
  set(DUCKDB_MINOR_VERSION 0)
  set(DUCKDB_PATCH_VERSION 1)
  set(DUCKDB_DEV_ITERATION 0)
  set(DUCKDB_VERSION "v${DUCKDB_MAJOR_VERSION}.${DUCKDB_MINOR_VERSION}.${DUCKDB_PATCH_VERSION}-dev${DUCKDB_DEV_ITERATION}")
endif()

message(STATUS "git hash ${GIT_COMMIT_HASH}, version ${DUCKDB_VERSION}")

option(AMALGAMATION_BUILD
       "Build from the amalgamation files, rather than from the normal sources."
       FALSE)

option(BUILD_MAIN_DUCKDB_LIBRARY
        "Build the main duckdb library and executable."
        TRUE)
option(EXTENSION_STATIC_BUILD
        "Extension build linking statically with DuckDB. Required for building linux loadable extensions."
        FALSE)

if(WIN32)
  set(EXTENSION_STATIC_BUILD TRUE)
endif()

set(JEMALLOC_DEFAULT_BUILD FALSE)
if(NOT CLANG_TIDY AND OS_NAME STREQUAL "linux")
  # build jemalloc by default for linux
  set(JEMALLOC_DEFAULT_BUILD TRUE)
endif()

option(BUILD_CORE_FUNCTIONS_EXTENSION "Build the core functions." TRUE)
option(BUILD_ICU_EXTENSION "Build the ICU extension." FALSE)
option(BUILD_PARQUET_EXTENSION "Build the Parquet extension." FALSE)
option(BUILD_TPCH_EXTENSION "Build the TPC-H extension." FALSE)
option(BUILD_TPCDS_EXTENSION "Build the TPC-DS extension." FALSE)
option(BUILD_FTS_EXTENSION "Build the FTS extension." FALSE)
option(BUILD_HTTPFS_EXTENSION "Build the HTTP File System extension." FALSE)
option(BUILD_VISUALIZER_EXTENSION "Build the profiler-output visualizer extension." FALSE)
option(BUILD_JSON_EXTENSION "Build the JSON extension." FALSE)
option(BUILD_JEMALLOC_EXTENSION "Build the JEMalloc extension." ${JEMALLOC_DEFAULT_BUILD})
option(BUILD_EXCEL_EXTENSION "Build the excel extension." FALSE)
option(BUILD_INET_EXTENSION "Build the inet extension." FALSE)
option(BUILD_BENCHMARKS "Enable building of the benchmark suite." FALSE)
option(BUILD_SQLSMITH_EXTENSION "Enable building of the SQLSmith extension." FALSE)
option(BUILD_TPCE "Enable building of the TPC-E tool." FALSE)
option(BUILD_AUTOCOMPLETE_EXTENSION "Enable building of the autocomplete extension" FALSE)
option(DISABLE_BUILTIN_EXTENSIONS "Disable linking extensions." FALSE)
option(JDBC_DRIVER "Build the DuckDB JDBC driver" FALSE)
option(BUILD_ODBC_DRIVER "Build the DuckDB ODBC driver" FALSE)
option(BUILD_PYTHON "Build the DuckDB Python extension" FALSE)
option(BUILD_NODE "Build the DuckDB Node extension" FALSE)
option(USER_SPACE "Build the DuckDB Python in the user space" FALSE)
option(FORCE_QUERY_LOG "If enabled, all queries will be logged to the specified path" OFF)
option(BUILD_SHELL "Build the DuckDB Shell and SQLite API Wrappers" TRUE)
option(DISABLE_THREADS "Disable support for multi-threading" FALSE)
option(DISABLE_STR_INLINE "Debug setting: disable inlining of strings" FALSE)
option(DISABLE_MEMORY_SAFETY "Debug setting: disable memory access checks at runtime" FALSE)
option(ALTERNATIVE_VERIFY "Debug setting: use alternative verify mode" FALSE)
option(DESTROY_UNPINNED_BLOCKS "Debug setting: destroy unpinned buffer-managed blocks" FALSE)
option(FORCE_ASYNC_SINK_SOURCE "Debug setting: forces sinks/sources to block the first 2 times they're called" FALSE)
option(DEBUG_STACKTRACE "Debug setting: print a stracktrace on asserts and when testing crashes" FALSE)
option(DEBUG_MOVE "Debug setting: Ensure std::move is being used" FALSE)
option(CLANG_TIDY "Enable build for clang-tidy, this disables all source files excluding the core database. This does not produce a working build." FALSE)
option(BUILD_UNITTESTS "Build the C++ Unit Tests." TRUE)
option(
  ASSERT_EXCEPTION
  "Throw an exception on an assert failing, instead of triggering a sigabort"
  TRUE)
option(FORCE_ASSERT "Enable checking of assertions, even in release mode" FALSE)

option(TREAT_WARNINGS_AS_ERRORS "Treat warnings as errors" FALSE)
option(EXPORT_DLL_SYMBOLS "Export dll symbols on Windows, else import" TRUE)
option(BUILD_RDTSC "Enable the rdtsc instruction." FALSE)
option(TEST_REMOTE_INSTALL "Test installation of specific extensions." FALSE)

if(${BUILD_RDTSC})
  add_compile_definitions(RDTSC)
endif()

if (NOT BUILD_MAIN_DUCKDB_LIBRARY)
  set(BUILD_UNITTESTS FALSE)
  set(BUILD_SHELL FALSE)
  set(DISABLE_BUILTIN_EXTENSIONS TRUE)
endif()

if(BUILD_PYTHON
   OR BUILD_R
   OR BUILD_NODE
   OR CONFIGURE_R
   OR JDBC_DRIVER)
  set(BUILD_ICU_EXTENSION TRUE)
  set(BUILD_VISUALIZER_EXTENSION TRUE)
  set(BUILD_PARQUET_EXTENSION TRUE)
endif()

if(BUILD_PYTHON)
  set(BUILD_TPCH_EXTENSION TRUE)
  set(BUILD_TPCDS_EXTENSION TRUE)
  set(BUILD_FTS_EXTENSION TRUE)
  set(BUILD_EXCEL_EXTENSION TRUE)
endif()

if (BUILD_BENCHMARKS)
  set(BUILD_PARQUET_EXTENSION TRUE)
endif()

if(BUILD_SQLSMITH)
  set(BUILD_SQLSMITH_EXTENSION TRUE)
endif()

if(TREAT_WARNINGS_AS_ERRORS)
  message("Treating warnings as errors.")
endif()

if(ASSERT_EXCEPTION)
else()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_CRASH_ON_ASSERT")
endif()

if(DISABLE_STR_INLINE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_DEBUG_NO_INLINE")
endif()

if(DISABLE_MEMORY_SAFETY)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_DEBUG_NO_SAFETY")
endif()

if(DESTROY_UNPINNED_BLOCKS)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_DEBUG_DESTROY_BLOCKS")
endif()
if(FORCE_ASYNC_SINK_SOURCE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_DEBUG_ASYNC_SINK_SOURCE")
endif()

if(ALTERNATIVE_VERIFY)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_ALTERNATIVE_VERIFY")
endif()

if(DEBUG_STACKTRACE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_DEBUG_STACKTRACE")
endif()

if(DEBUG_MOVE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_DEBUG_MOVE")
endif()

if (CLANG_TIDY)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_CLANG_TIDY")
endif()

if(FORCE_ASSERT)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_FORCE_ASSERT")
endif()

if(NOT MSVC)
  if(${FORCE_WARN_UNUSED})
    set(CXX_EXTRA "${CXX_EXTRA} -Wunused")
  endif()
  if(TREAT_WARNINGS_AS_ERRORS)
    set(CXX_EXTRA "${CXX_EXTRA} -Werror")
  endif()
  set(CMAKE_CXX_FLAGS_DEBUG
      "${CMAKE_CXX_FLAGS_DEBUG} -g -O0 -DDEBUG -Wall ${M32_FLAG} ${CXX_EXTRA}")
  set(CMAKE_CXX_FLAGS_RELEASE
      "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG ${M32_FLAG} ${CXX_EXTRA}")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELEASE} -g")

  set(CXX_EXTRA_DEBUG
      "${CXX_EXTRA_DEBUG} -Wunused -Werror=vla -Wnarrowing -pedantic"
  )

  if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION
                                                   VERSION_GREATER 8.0)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${CXX_EXTRA_DEBUG}")
  elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang$"
         AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 9.0)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${CXX_EXTRA_DEBUG}")
  else()
    message(WARNING "Please use a recent compiler for debug builds")
  endif()
else()
  set(CMAKE_CXX_WINDOWS_FLAGS
      "/wd4244 /wd4267 /wd4200 /wd26451 /wd26495 /D_CRT_SECURE_NO_WARNINGS /utf-8")
  if(TREAT_WARNINGS_AS_ERRORS)
    set(CMAKE_CXX_WINDOWS_FLAGS "${CMAKE_CXX_WINDOWS_FLAGS} /WX")
  endif()
  # remove warning from CXX flags
  string(REGEX REPLACE "/W[0-4]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  # add to-be-ignored warnings
  set(CMAKE_CXX_FLAGS
      "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_WINDOWS_FLAGS}"
  )
endif()

# todo use CHECK_CXX_COMPILER_FLAG(-fsanitize=address SUPPORTS_SANITIZER) etc.

set(CMAKE_C_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(DEFAULT_BUILD_TYPE "Release")
  message(STATUS "Setting build type to '${DEFAULT_BUILD_TYPE}'.")
  set(CMAKE_BUILD_TYPE
      "${DEFAULT_BUILD_TYPE}"
      CACHE STRING "Choose the type of build." FORCE)
endif()

if(OS_NAME STREQUAL "windows")
  if(BUILD_JEMALLOC_EXTENSION EQUAL 1)
    # have to throw an error because this will crash at runtime
    message(FATAL_ERROR "The jemalloc extension is not supported on Windows")
  endif()
endif()


include_directories(src/include)
include_directories(third_party/fsst)
include_directories(third_party/fmt/include)
include_directories(third_party/hyperloglog)
include_directories(third_party/fastpforlib)
include_directories(third_party/fast_float)
include_directories(third_party/re2)
include_directories(third_party/miniz)
include_directories(third_party/utf8proc/include)
include_directories(third_party/miniparquet)
include_directories(third_party/concurrentqueue)
include_directories(third_party/pcg)
include_directories(third_party/tdigest)
include_directories(third_party/mbedtls/include)
include_directories(third_party/jaro_winkler)

# todo only regenerate ub file if one of the input files changed hack alert
function(enable_unity_build UB_SUFFIX SOURCE_VARIABLE_NAME)
  set(files ${${SOURCE_VARIABLE_NAME}})

  # Generate a unique filename for the unity build translation unit
  set(unit_build_file ${CMAKE_CURRENT_BINARY_DIR}/ub_${UB_SUFFIX}.cpp)
  set(temp_unit_build_file ${CMAKE_CURRENT_BINARY_DIR}/ub_${UB_SUFFIX}.cpp.tmp)
  # Exclude all translation units from compilation
  set_source_files_properties(${files} PROPERTIES HEADER_FILE_ONLY true)

  set(rebuild FALSE)
  # check if any of the source files have changed
  foreach(source_file ${files})
    if(${CMAKE_CURRENT_SOURCE_DIR}/${source_file} IS_NEWER_THAN
       ${unit_build_file})
      set(rebuild TRUE)
    endif()
  endforeach(source_file)
  # write a temporary file
  file(WRITE ${temp_unit_build_file} "// Unity Build generated by CMake\n")
  foreach(source_file ${files})
    file(
      APPEND ${temp_unit_build_file}
      "#include <${CMAKE_CURRENT_SOURCE_DIR}/${source_file}>\n"
    )
  endforeach(source_file)

  execute_process(
    COMMAND ${CMAKE_COMMAND} -E compare_files ${unit_build_file}
            ${temp_unit_build_file}
    RESULT_VARIABLE compare_result
    OUTPUT_VARIABLE bla
    ERROR_VARIABLE bla)
  if(compare_result EQUAL 0)
    # files are identical: do nothing
  elseif(compare_result EQUAL 1)
    # files are different: rebuild
    set(rebuild TRUE)
  else()
    # error while compiling: rebuild
    set(rebuild TRUE)
  endif()

  if(${rebuild})
    file(WRITE ${unit_build_file} "// Unity Build generated by CMake\n")
    foreach(source_file ${files})
      file(
        APPEND ${unit_build_file}
        "#include <${CMAKE_CURRENT_SOURCE_DIR}/${source_file}>\n"
      )
    endforeach(source_file)
  endif()

  # Complement list of translation units with the name of ub
  set(${SOURCE_VARIABLE_NAME}
      ${${SOURCE_VARIABLE_NAME}} ${unit_build_file}
      PARENT_SCOPE)
endfunction(enable_unity_build)

function(add_library_unity NAME MODE)
  set(SRCS ${ARGN})
  if(NOT DISABLE_UNITY)
    enable_unity_build(${NAME} SRCS)
  endif()
  add_library(${NAME} OBJECT ${SRCS})
endfunction()

function(disable_target_warnings NAME)
  if(MSVC)
    target_compile_options(${NAME} PRIVATE "/W0")
  elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang$"
         OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    target_compile_options(${NAME} PRIVATE "-w")
  endif()
endfunction()

function(add_extension_definitions)
  include_directories(${PROJECT_SOURCE_DIR}/extension)

  if(NOT("${TEST_REMOTE_INSTALL}" STREQUAL "OFF"))
    add_definitions(-DDUCKDB_TEST_REMOTE_INSTALL="${TEST_REMOTE_INSTALL}")
  endif()

  if(${DISABLE_BUILTIN_EXTENSIONS})
    add_definitions(-DDISABLE_BUILTIN_EXTENSIONS=${DISABLE_BUILTIN_EXTENSIONS})
  endif()

  if(${BUILD_ICU_EXTENSION})
    include_directories(${PROJECT_SOURCE_DIR}/extension/icu/include)
    add_definitions(-DBUILD_ICU_EXTENSION=${BUILD_ICU_EXTENSION})
  endif()

  if(${BUILD_PARQUET_EXTENSION})
    include_directories(${PROJECT_SOURCE_DIR}/extension/parquet/include)
    add_definitions(-DBUILD_PARQUET_EXTENSION=${BUILD_PARQUET_EXTENSION})
  endif()

  if(${BUILD_TPCH_EXTENSION})
    include_directories(${PROJECT_SOURCE_DIR}/extension/tpch/include)
    add_definitions(-DBUILD_TPCH_EXTENSION=${BUILD_TPCH_EXTENSION})
  endif()

  if(${BUILD_TPCDS_EXTENSION})
    include_directories(${PROJECT_SOURCE_DIR}/extension/tpcds/include)
    add_definitions(-DBUILD_TPCDS_EXTENSION=${BUILD_TPCDS_EXTENSION})
  endif()

  if(${BUILD_FTS_EXTENSION})
    include_directories(${PROJECT_SOURCE_DIR}/extension/fts/include)
    add_definitions(-DBUILD_FTS_EXTENSION=${BUILD_FTS_EXTENSION})
  endif()

  if(${BUILD_HTTPFS_EXTENSION})
    find_package(OpenSSL REQUIRED)
    include_directories(${PROJECT_SOURCE_DIR}/extension/httpfs/include ${OPENSSL_INCLUDE_DIR})
    add_definitions(-DBUILD_HTTPFS_EXTENSION=${BUILD_HTTPFS_EXTENSION})
  endif()

  if(${BUILD_VISUALIZER_EXTENSION})
    include_directories(${PROJECT_SOURCE_DIR}/extension/visualizer/include)
    add_definitions(-DBUILD_VISUALIZER_EXTENSION=${BUILD_VISUALIZER_EXTENSION})
  endif()

  if(${BUILD_JSON_EXTENSION})
    include_directories(${PROJECT_SOURCE_DIR}/extension/json/include)
    add_definitions(-DBUILD_JSON_EXTENSION=${BUILD_JSON_EXTENSION})
  endif()

  if(${BUILD_JEMALLOC_EXTENSION})
    include_directories(${PROJECT_SOURCE_DIR}/extension/jemalloc/include)
    add_definitions(-DBUILD_JEMALLOC_EXTENSION=${BUILD_JEMALLOC_EXTENSION})
  endif()

  if(${BUILD_EXCEL_EXTENSION})
    include_directories(${PROJECT_SOURCE_DIR}/extension/excel/include)
    add_definitions(-DBUILD_EXCEL_EXTENSION=${BUILD_EXCEL_EXTENSION})
  endif()

  if(${BUILD_SQLSMITH_EXTENSION})
    include_directories(${PROJECT_SOURCE_DIR}/extension/sqlsmith/include)
    add_definitions(-DBUILD_SQLSMITH_EXTENSION=${BUILD_SQLSMITH_EXTENSION})
  endif()

  if(${BUILD_INET_EXTENSION})
    include_directories(${PROJECT_SOURCE_DIR}/extension/inet/include)
    add_definitions(-DBUILD_INET_EXTENSION=${BUILD_INET_EXTENSION})
  endif()

  if(${BUILD_AUTOCOMPLETE_EXTENSION})
    include_directories(${PROJECT_SOURCE_DIR}/extension/autocomplete/include)
    add_definitions(-DBUILD_AUTOCOMPLETE_EXTENSION=${BUILD_AUTOCOMPLETE_EXTENSION})
  endif()

  # Include paths for any registered out-of-tree extensions
  foreach(OOTE_NAME IN LISTS DUCKDB_OOT_EXTENSION_NAMES)
    string(TOUPPER ${OOTE_NAME} OOTE_NAME_UPPERCASE)
    if (DEFINED DUCKDB_OOT_EXTENSION_${OOTE_NAME_UPPERCASE}_INCLUDE_PATH)
      include_directories("${DUCKDB_OOT_EXTENSION_${OOTE_NAME_UPPERCASE}_INCLUDE_PATH}")
    else()
      # By default, we assume third-party extensions to by (symlinked) in extensions/ with <name>_clone as path
      include_directories("${PROJECT_SOURCE_DIR}/extension/${OOTE_NAME}_clone/src/include")
    endif()
  endforeach()
endfunction()

function(add_extension_dependencies LIBRARY)
  if(${BUILD_PARQUET_EXTENSION})
    add_dependencies(${LIBRARY} parquet_extension)
  endif()

  if(${BUILD_ICU_EXTENSION})
    add_dependencies(${LIBRARY} icu_extension)
  endif()

  if(${BUILD_TPCH_EXTENSION})
    add_dependencies(${LIBRARY} tpch_extension)
  endif()

  if(${BUILD_TPCDS_EXTENSION})
    add_dependencies(${LIBRARY} tpcds_extension)
  endif()

  if(${BUILD_FTS_EXTENSION})
    add_dependencies(${LIBRARY} fts_extension)
  endif()

  if(${BUILD_HTTPFS_EXTENSION})
    add_dependencies(${LIBRARY} httpfs_extension)
  endif()

  if(${BUILD_VISUALIZER_EXTENSION})
    add_dependencies(${LIBRARY} visualizer_extension)
  endif()

  if(${BUILD_JSON_EXTENSION})
    add_dependencies(${LIBRARY} json_extension)
  endif()

  if(${BUILD_JEMALLOC_EXTENSION})
    add_dependencies(${LIBRARY} jemalloc_extension)
  endif()

  if(${BUILD_EXCEL_EXTENSION})
    add_dependencies(${LIBRARY} excel_extension)
  endif()

  if(${BUILD_SQLSMITH_EXTENSION})
    add_dependencies(${LIBRARY} sqlsmith_extension)
  endif()

  if(${BUILD_INET_EXTENSION})
    add_dependencies(${LIBRARY} inet_extension)
  endif()

  foreach(OOTE_NAME IN LISTS DUCKDB_OOT_EXTENSION_NAMES)
    add_dependencies(${LIBRARY} ${OOTE_NAME}_extension)
  endforeach()

endfunction()

function(link_extension_libraries LIBRARY)
  # special case for our static-only jemalloc extension
  if(${BUILD_JEMALLOC_EXTENSION})
    target_link_libraries(${LIBRARY} jemalloc_extension)
  endif()

  if(${DISABLE_BUILTIN_EXTENSIONS})
    return()
  endif()

  if(${BUILD_PARQUET_EXTENSION})
    target_link_libraries(${LIBRARY} parquet_extension)
  endif()

  if(${BUILD_ICU_EXTENSION})
    target_link_libraries(${LIBRARY} icu_extension)
  endif()

  if(${BUILD_TPCH_EXTENSION})
    target_link_libraries(${LIBRARY} tpch_extension)
  endif()

  if(${BUILD_TPCDS_EXTENSION})
    target_link_libraries(${LIBRARY} tpcds_extension)
  endif()

  if(${BUILD_FTS_EXTENSION})
    target_link_libraries(${LIBRARY} fts_extension)
  endif()

  if(${BUILD_HTTPFS_EXTENSION})
    find_package(OpenSSL REQUIRED)
    target_link_libraries(${LIBRARY} httpfs_extension ${OPENSSL_LIBRARIES})
  endif()

  if(${BUILD_VISUALIZER_EXTENSION})
    target_link_libraries(${LIBRARY} visualizer_extension)
  endif()

  if(${BUILD_JSON_EXTENSION})
    target_link_libraries(${LIBRARY} json_extension)
  endif()

  if(${BUILD_EXCEL_EXTENSION})
    target_link_libraries(${LIBRARY} excel_extension)
  endif()

  if(${BUILD_SQLSMITH_EXTENSION})
    target_link_libraries(${LIBRARY} sqlsmith_extension)
  endif()

  if(${BUILD_INET_EXTENSION})
    target_link_libraries(${LIBRARY} inet_extension)
  endif()

  if(${BUILD_AUTOCOMPLETE_EXTENSION})
    target_link_libraries(${LIBRARY} autocomplete_extension)
  endif()

  # Now link against any registered out-of-tree extensions
  foreach(OOTE_NAME IN LISTS DUCKDB_OOT_EXTENSION_NAMES)
    string(TOUPPER ${OOTE_NAME} OOTE_NAME_UPPERCASE)
    if (${DUCKDB_OOT_EXTENSION_${OOTE_NAME_UPPERCASE}_SHOULD_LINK})
      target_link_libraries(${LIBRARY} ${OOTE_NAME}_extension)
    endif()
  endforeach()

endfunction()

function(link_threads LIBRARY)
  if (CMAKE_VERSION VERSION_LESS "3.1")
    target_link_libraries(${LIBRARY} pthread)

  else()
    target_link_libraries(${LIBRARY} Threads::Threads)
  endif()
endfunction()

function(build_loadable_extension_directory NAME OUTPUT_DIRECTORY PARAMETERS)
  set(TARGET_NAME ${NAME}_loadable_extension)
  # all parameters after output_directory
  set(FILES ${ARGV})
  # remove name
  list(REMOVE_AT FILES 0)
  # remove output_directory
  list(REMOVE_AT FILES 0)
  # remove parameters
  list(REMOVE_AT FILES 0)

  # parse parameters
  string(FIND "${PARAMETERS}" "-no-warnings" IGNORE_WARNINGS)

  add_library(${TARGET_NAME} SHARED ${FILES})
  # this disables the -Dsome_target_EXPORTS define being added by cmake which otherwise trips clang-tidy (yay)
  set_target_properties(${TARGET_NAME} PROPERTIES DEFINE_SYMBOL "")
  set_target_properties(${TARGET_NAME} PROPERTIES OUTPUT_NAME ${NAME})
  set_target_properties(${TARGET_NAME} PROPERTIES PREFIX "")
  if(${IGNORE_WARNINGS} GREATER -1)
    disable_target_warnings(${TARGET_NAME})
  endif()
  # loadable extension binaries can be built two ways:
  # 1. EXTENSION_STATIC_BUILD=1
  #    DuckDB is statically linked into each extension binary. This increases portability because in several situations
  #    DuckDB itself may have been loaded with RTLD_LOCAL. This is currently the main way we distribute the loadable
  #    extension binaries
  # 2. EXTENSION_STATIC_BUILD=0
  #    The DuckDB symbols required by the loadable extensions are left unresolved. This will reduce the size of the binaries
  #    and works well when running the DuckDB cli directly. For windows this uses delay loading. For MacOS and linux the
  #    dynamic loader will look up the missing symbols when the extension is dlopen-ed.
  if(WASM_LOADABLE_EXTENSIONS)
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -sSIDE_MODULE=1 -DWASM_LOADABLE_EXTENSIONS")
  elseif (EXTENSION_STATIC_BUILD)
    if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
      if (APPLE)
        set_target_properties(${TARGET_NAME} PROPERTIES CXX_VISIBILITY_PRESET hidden)
        # Note that on MacOS we need to use the -exported_symbol whitelist feature due to a lack of -exclude-libs flag in mac's ld variant
        set(WHITELIST "-Wl,-exported_symbol,_${NAME}_init -Wl,-exported_symbol,_${NAME}_version -Wl,-exported_symbol,_${NAME}_storage_ini*")
        target_link_libraries(${TARGET_NAME} duckdb_static ${DUCKDB_EXTRA_LINK_FLAGS} -Wl,-dead_strip ${WHITELIST})
      else()
        # For GNU we rely on fvisibility=hidden to hide the extension symbols and use -exclude-libs to hide the duckdb symbols
        set_target_properties(${TARGET_NAME} PROPERTIES CXX_VISIBILITY_PRESET hidden)
        target_link_libraries(${TARGET_NAME} duckdb_static ${DUCKDB_EXTRA_LINK_FLAGS} -Wl,--gc-sections -Wl,--exclude-libs,ALL)
      endif()
    elseif (WIN32)
      target_link_libraries(${TARGET_NAME} duckdb_static ${DUCKDB_EXTRA_LINK_FLAGS})
    else()
      error("EXTENSION static build is only intended for Linux and Windows on MVSC")
    endif()
  else()
    if (WIN32)
      target_link_libraries(${TARGET_NAME} duckdb ${DUCKDB_EXTRA_LINK_FLAGS})
    elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang$")
      if (APPLE)
        set_target_properties(${TARGET_NAME} PROPERTIES LINK_FLAGS "-undefined dynamic_lookup")
      endif()
    endif()
  endif()


  target_compile_definitions(${TARGET_NAME} PUBLIC -DDUCKDB_BUILD_LOADABLE_EXTENSION)
  set_target_properties(${TARGET_NAME} PROPERTIES SUFFIX
          ".duckdb_extension")

  if(MSVC)
    set_target_properties(
            ${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG
            "${CMAKE_BINARY_DIR}/${OUTPUT_DIRECTORY}")
    set_target_properties(
            ${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE
            "${CMAKE_BINARY_DIR}/${OUTPUT_DIRECTORY}")
  endif()
endfunction()

function(build_loadable_extension NAME PARAMETERS)
  # all parameters after name
  set(FILES ${ARGV})
  list(REMOVE_AT FILES 0)
  list(REMOVE_AT FILES 0)

  build_loadable_extension_directory(${NAME} "extension/${NAME}" "${PARAMETERS}" ${FILES})
endfunction()

if(${EXPORT_DLL_SYMBOLS})
  # For Windows DLL export symbols
  add_definitions(-DDUCKDB_BUILD_LIBRARY)
endif()

if (BUILD_MAIN_DUCKDB_LIBRARY)
  add_subdirectory(src)
  add_subdirectory(tools)
endif()

# Add In-tree extensions
add_subdirectory(extension)

# Add Out-of-tree extensions
foreach(OOTE_NAME IN LISTS DUCKDB_OOT_EXTENSION_NAMES)
  string(TOUPPER ${OOTE_NAME} OOTE_NAME_UPPERCASE)
  if (DEFINED DUCKDB_OOT_EXTENSION_${OOTE_NAME_UPPERCASE}_PATH)
    add_subdirectory(${DUCKDB_OOT_EXTENSION_${OOTE_NAME_UPPERCASE}_PATH} extension/${OOTE_NAME})
  else()
    # By default, we assume third-party extensions to by (symlinked) in extensions/ with <name>_clone as path
    add_subdirectory("extension/${OOTE_NAME}_clone/src" extension/${OOTE_NAME})
  endif()
endforeach()

if(BUILD_PYTHON
        OR BUILD_R
        OR BUILD_NODE
        OR CONFIGURE_R)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(ALL_COMPILE_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_DEBUG}")
  elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(ALL_COMPILE_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE}")
  elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    set(ALL_COMPILE_FLAGS
            "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
  else()
    set(ALL_COMPILE_FLAGS "${CMAKE_CXX_FLAGS}")
  endif()

  get_target_property(duckdb_libs duckdb LINK_LIBRARIES)

  # This searches all Out-of-tree (OOT) extensions that we have linked into duckdb for their direct dependencies and
  # adds them to the duckdb_libs to ensure they also get linked. This allows linking OOT extensions with dependencies
  # for the Python/R/Node builds
  foreach(OOTE_NAME IN LISTS DUCKDB_OOT_EXTENSION_NAMES)
    string(TOUPPER ${OOTE_NAME} OOTE_NAME_UPPERCASE)
    if (${DUCKDB_OOT_EXTENSION_${OOTE_NAME_UPPERCASE}_SHOULD_LINK})
      get_target_property(OOTE_LINKED_LIBS ${OOTE_NAME}_extension LINK_LIBRARIES)
      if (NOT "${OOTE_LINKED_LIBS}" STREQUAL "OOTE_LINKED_LIBS-NOTFOUND")
        set(duckdb_libs ${OOTE_LINKED_LIBS} ${duckdb_libs})
      endif()
    endif()
  endforeach()

  if(BUILD_PYTHON)
    if(USER_SPACE)
      add_custom_target(
              duckdb_python ALL
              COMMAND
              python3 setup.py install --user --binary-dir=${PROJECT_BINARY_DIR}
              --compile-flags=${ALL_COMPILE_FLAGS} --libs="${duckdb_libs}"
              DEPENDS duckdb duckdb_static
              WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools/pythonpkg
              COMMENT Build
              Python package)
    else()
      add_custom_target(
              duckdb_python ALL
              COMMAND python3 setup.py install --binary-dir=${PROJECT_BINARY_DIR}
              --compile-flags=${ALL_COMPILE_FLAGS} --libs="${duckdb_libs}"
              DEPENDS duckdb duckdb_static
              WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools/pythonpkg
              COMMENT Build
              Python package)
    endif()
    add_extension_dependencies(duckdb_python)
  endif()
  if(CONFIGURE_R)
    add_custom_target(
            duckdb_configure_r ALL
            COMMAND
            DUCKDB_R_BINDIR=${PROJECT_BINARY_DIR}
            DUCKDB_R_CFLAGS=\"${ALL_COMPILE_FLAGS}\"
            DUCKDB_R_LIBS=\"${duckdb_libs}\" python3 rconfigure.py
            DEPENDS duckdb duckdb_static
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools/rpkg
            COMMENT Configure
            R package)
    add_extension_dependencies(duckdb_configure_r)
  endif()
  if(BUILD_R)
    add_custom_target(
            duckdb_r ALL
            COMMAND
            DUCKDB_R_BINDIR=${PROJECT_BINARY_DIR}
            DUCKDB_R_CFLAGS=\"${ALL_COMPILE_FLAGS}\"
            DUCKDB_R_LIBS=\"${duckdb_libs}\" DUCKDB_R_DEBUG= R CMD INSTALL .
            DEPENDS duckdb duckdb_static
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools/rpkg
            COMMENT Build
            R package)
    add_extension_dependencies(duckdb_r)
  endif()
  if(BUILD_NODE)
    add_custom_target(
            duckdb_node ALL
            COMMAND
            make complete_build DUCKDB_NODE_BINDIR=${CMAKE_BINARY_DIR}
            DUCKDB_NODE_CFLAGS=\"${ALL_COMPILE_FLAGS}\"
            DUCKDB_NODE_LIBS=\"${duckdb_libs}\"
            DEPENDS duckdb duckdb_static
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools/nodejs
            COMMENT Build
            Node package)
    add_extension_dependencies(duckdb_node)
  endif()
endif()

if(NOT CLANG_TIDY)
  if(${BUILD_UNITTESTS})
    add_subdirectory(test)
    if(NOT WIN32
      AND NOT SUN
      AND ${BUILD_BENCHMARKS})
      add_subdirectory(benchmark)
    endif()
  endif()
  add_subdirectory(third_party)
endif()

if (CMAKE_VERSION VERSION_GREATER "3.0") # this does not work with 2.8
# Write the export set for build and install tree
install(EXPORT "${DUCKDB_EXPORT_SET}" DESTINATION "${INSTALL_CMAKE_DIR}")
export(EXPORT "${DUCKDB_EXPORT_SET}"
       FILE "${PROJECT_BINARY_DIR}/${DUCKDB_EXPORT_SET}.cmake")

# Only write the cmake package configuration if the templates exist
set(CMAKE_CONFIG_TEMPLATE "${CMAKE_SOURCE_DIR}/DuckDBConfig.cmake.in")
set(CMAKE_CONFIG_VERSION_TEMPLATE
    "${CMAKE_SOURCE_DIR}/DuckDBConfigVersion.cmake.in")
if(EXISTS ${CMAKE_CONFIG_TEMPLATE} AND EXISTS ${CMAKE_CONFIG_VERSION_TEMPLATE})

  # Configure cmake package config for the build tree
  set(CONF_INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/include")
  configure_file(${CMAKE_CONFIG_TEMPLATE}
                 "${PROJECT_BINARY_DIR}/DuckDBConfig.cmake" @ONLY)

  # Configure cmake package config for the install tree
  file(RELATIVE_PATH REL_INCLUDE_DIR "${INSTALL_CMAKE_DIR}"
       "${INSTALL_INCLUDE_DIR}")
  set(CONF_INCLUDE_DIRS "\${DuckDB_CMAKE_DIR}/${REL_INCLUDE_DIR}")
  configure_file(
    ${CMAKE_CONFIG_TEMPLATE}
    "${PROJECT_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/DuckDBConfig.cmake" @ONLY)

  # Configure cmake package version for build and install tree
  configure_file(${CMAKE_CONFIG_VERSION_TEMPLATE}
                 "${PROJECT_BINARY_DIR}/DuckDBConfigVersion.cmake" @ONLY)

  # Install the cmake package
  install(
    FILES "${PROJECT_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/DuckDBConfig.cmake"
          "${PROJECT_BINARY_DIR}/DuckDBConfigVersion.cmake"
    DESTINATION "${INSTALL_CMAKE_DIR}")
endif()

endif()

# TODO: This is the old way of Loading OOTEs can be removed after all OOTES in extensions.csv play ball with the new way
# build out-of-tree extensions on demand
if(NOT "${EXTERNAL_EXTENSION_DIRECTORIES}" STREQUAL "")
  separate_arguments(EXTERNAL_EXTENSION_DIRECTORIES)

  foreach(EXTERNAL_EXTENSION_DIRECTORY IN LISTS EXTERNAL_EXTENSION_DIRECTORIES)

    # the build path seems to get ignored on windows in just the right way. no idea why.
    get_filename_component(EXTERNAL_EXTENSION_NAME ${EXTERNAL_EXTENSION_DIRECTORY} NAME)
    add_subdirectory(${EXTERNAL_EXTENSION_DIRECTORY} "extension/${EXTERNAL_EXTENSION_NAME}")
  endforeach()
endif()
