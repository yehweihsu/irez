cmake_minimum_required(VERSION 3.20)

# The product version lives in contract.json (single source of truth shared
# with the Rust installer and the release scripts); do not duplicate it here.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/contract.json" IREZ_CONTRACT_JSON)
string(JSON IREZ_CONTRACT_VERSION ERROR_VARIABLE _irez_contract_error
       GET "${IREZ_CONTRACT_JSON}" irez_version)
if(_irez_contract_error)
  message(FATAL_ERROR "cannot read irez_version from contract.json: ${_irez_contract_error}")
endif()

project(irez VERSION ${IREZ_CONTRACT_VERSION} LANGUAGES CXX
        HOMEPAGE_URL "https://github.com/yehweihsu/irez")

# Build provenance: bake the source revision into the binary so `irez status`
# can prove which code is running. "unknown" when VCS info is unavailable.
find_package(Git QUIET)
set(IREZ_BUILD_REVISION "unknown")
if(Git_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE _irez_git_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _irez_git_result)
  if(_irez_git_result EQUAL 0 AND _irez_git_revision)
    set(IREZ_BUILD_REVISION "${_irez_git_revision}")
  endif()
endif()
message(STATUS "IREZ build revision: ${IREZ_BUILD_REVISION}")

include(FetchContent)
if(POLICY CMP0169)
  # CMake 3.30 deprecated the direct Populate form. It remains necessary to
  # add dependency projects EXCLUDE_FROM_ALL while retaining CMake 3.20.
  cmake_policy(SET CMP0169 OLD)
endif()

# Vendored dependencies live outside this tree; point IREZ_DEPS_DIR at the
# directory that contains GoogleTest/, SQLiteCpp/, and optionally zlib/ and
# zstd/ checkouts.
set(IREZ_DEPS_DIR "" CACHE PATH
    "Optional directory containing GoogleTest/, SQLiteCpp/, zlib/, and zstd/ checkouts")

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(IREZ_PORTABLE_LINUX
  "Statically link the GCC support runtimes for portable Linux release binaries" OFF)
if(IREZ_PORTABLE_LINUX AND UNIX AND NOT APPLE)
  add_link_options(-static-libstdc++ -static-libgcc)
endif()
option(IREZ_STATIC_MSVC_RUNTIME
  "Statically link the MSVC runtime in official Windows binaries" OFF)
if(IREZ_STATIC_MSVC_RUNTIME AND MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Debian/Ubuntu's LLVM 21 CMake exports reference zstd::libzstd_shared without
# defining it when libzstd-dev is absent. Provide the imported target ourselves.
if(NOT TARGET zstd::libzstd_shared)
  find_library(IREZ_ZSTD_LIBRARY NAMES zstd libzstd.so.1)
  if(IREZ_ZSTD_LIBRARY)
    add_library(zstd::libzstd_shared SHARED IMPORTED)
    set_target_properties(zstd::libzstd_shared PROPERTIES
      IMPORTED_LOCATION "${IREZ_ZSTD_LIBRARY}")
  endif()
endif()

# LLVM 23's official Windows development archive exports LLVMSupport with a
# ZLIB::ZLIB dependency but does not ship zlib. Supply a pinned static build
# before importing LLVM's targets.
if(WIN32 AND NOT TARGET ZLIB::ZLIB)
  set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  if(IREZ_DEPS_DIR AND EXISTS "${IREZ_DEPS_DIR}/zlib/CMakeLists.txt")
    add_subdirectory("${IREZ_DEPS_DIR}/zlib" "${CMAKE_BINARY_DIR}/_deps/zlib-build"
                     EXCLUDE_FROM_ALL)
  else()
    FetchContent_Declare(zlib
      URL https://codeload.github.com/madler/zlib/tar.gz/refs/tags/v1.3.1
      URL_HASH SHA256=17e88863f3600672ab49182f217281b6fc4d3c762bde361935e436a95214d05c
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_GetProperties(zlib)
    if(NOT zlib_POPULATED)
      FetchContent_Populate(zlib)
      add_subdirectory("${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}"
                       EXCLUDE_FROM_ALL)
    endif()
  endif()
  if(TARGET zlibstatic)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
  else()
    message(FATAL_ERROR "zlib fallback did not define zlibstatic")
  endif()
endif()

# LLVM 23's official Windows development archive exports LLVMSupport with a
# zstd::libzstd_static dependency but does not ship zstd itself. Prefer an
# offline checkout when supplied and otherwise fetch the pinned upstream
# source, building only the static library LLVM needs.
if(WIN32 AND NOT TARGET zstd::libzstd_static)
  set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(ZSTD_USE_STATIC_RUNTIME ${IREZ_STATIC_MSVC_RUNTIME} CACHE BOOL "" FORCE)
  if(IREZ_DEPS_DIR AND EXISTS "${IREZ_DEPS_DIR}/zstd/build/cmake/CMakeLists.txt")
    add_subdirectory("${IREZ_DEPS_DIR}/zstd/build/cmake"
                     "${CMAKE_BINARY_DIR}/_deps/zstd-build" EXCLUDE_FROM_ALL)
  else()
    FetchContent_Declare(zstd
      URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
      URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_GetProperties(zstd)
    if(NOT zstd_POPULATED)
      FetchContent_Populate(zstd)
      add_subdirectory("${zstd_SOURCE_DIR}/build/cmake" "${zstd_BINARY_DIR}"
                       EXCLUDE_FROM_ALL)
    endif()
  endif()
  if(TARGET libzstd_static)
    add_library(zstd::libzstd_static ALIAS libzstd_static)
  else()
    message(FATAL_ERROR "zstd fallback did not define libzstd_static")
  endif()
endif()

# LLVM's DIA finder does not yet infer Visual Studio 2026 installations. Find
# diaguids.lib ourselves and define the imported target before LLVMExports
# refers to it. The older raw-library export is still repaired below.
if(WIN32 AND NOT TARGET DIASDK::Diaguids)
  set(_irez_dia_hints "")
  if(DEFINED ENV{VSINSTALLDIR})
    list(APPEND _irez_dia_hints "$ENV{VSINSTALLDIR}DIA SDK/lib/amd64")
  endif()
  file(GLOB _irez_dia_vs_roots
    "C:/Program Files/Microsoft Visual Studio/*/*/DIA SDK/lib/amd64"
    "C:/Program Files (x86)/Microsoft Visual Studio/*/*/DIA SDK/lib/amd64")
  find_file(IREZ_DIAGUIDS_LIBRARY NAMES diaguids.lib
    PATHS ${_irez_dia_hints} ${_irez_dia_vs_roots})
  if(IREZ_DIAGUIDS_LIBRARY)
    get_filename_component(_irez_dia_arch_dir "${IREZ_DIAGUIDS_LIBRARY}" DIRECTORY)
    get_filename_component(_irez_dia_lib_dir "${_irez_dia_arch_dir}" DIRECTORY)
    get_filename_component(_irez_dia_sdk_dir "${_irez_dia_lib_dir}" DIRECTORY)
    set(MSVC_DIA_SDK_DIR "${_irez_dia_sdk_dir}" CACHE PATH
        "Path to the DIA SDK" FORCE)
    add_library(DIASDK::Diaguids STATIC IMPORTED)
    set_target_properties(DIASDK::Diaguids PROPERTIES
      IMPORTED_LOCATION "${IREZ_DIAGUIDS_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${MSVC_DIA_SDK_DIR}/include")
    message(STATUS "Found DIA SDK diaguids.lib: ${IREZ_DIAGUIDS_LIBRARY}")
  endif()
endif()

find_package(LLVM REQUIRED CONFIG)
if(LLVM_VERSION_MAJOR LESS 21)
  message(FATAL_ERROR "IREZ requires LLVM 21 or newer, found ${LLVM_PACKAGE_VERSION}")
endif()
if(LLVM_VERSION_MAJOR GREATER 23)
  message(WARNING "IREZ is tested against LLVM 21-23; ${LLVM_PACKAGE_VERSION} may not compile.")
endif()
llvm_map_components_to_libnames(IREZ_LLVM_LIBS Core IRReader BitReader Analysis Support)

# Official Windows LLVM archives hard-code the DIA SDK path of the machine
# that produced them in LLVMDebugInfoPDB's interface (diaguids.lib). When
# that path does not exist locally, retarget it to a DIA SDK found on this
# machine instead of failing at link time.
if(WIN32 AND TARGET LLVMDebugInfoPDB)
  get_target_property(_irez_pdb_iface LLVMDebugInfoPDB INTERFACE_LINK_LIBRARIES)
  set(_irez_dia_missing OFF)
  foreach(_entry IN LISTS _irez_pdb_iface)
    if(_entry MATCHES "diaguids\\.lib$" AND NOT EXISTS "${_entry}")
      set(_irez_dia_missing ON)
    endif()
  endforeach()
  if(_irez_dia_missing)
    set(_irez_dia_hints "")
    if(DEFINED ENV{VSINSTALLDIR})
      list(APPEND _irez_dia_hints "$ENV{VSINSTALLDIR}DIA SDK/lib/amd64")
    endif()
    file(GLOB _irez_dia_vs_roots
      "C:/Program Files/Microsoft Visual Studio/*/*/DIA SDK/lib/amd64"
      "C:/Program Files (x86)/Microsoft Visual Studio/*/*/DIA SDK/lib/amd64")
    find_file(IREZ_DIAGUIDS_LIBRARY NAMES diaguids.lib
      PATHS ${_irez_dia_hints} ${_irez_dia_vs_roots})
    if(NOT IREZ_DIAGUIDS_LIBRARY)
      message(FATAL_ERROR
        "diaguids.lib (DIA SDK) not found; install the Visual Studio C++ "
        "workload or set IREZ_DIAGUIDS_LIBRARY to its full path")
    endif()
    list(TRANSFORM _irez_pdb_iface REPLACE ".*/diaguids\\.lib$"
         "${IREZ_DIAGUIDS_LIBRARY}")
    set_target_properties(LLVMDebugInfoPDB PROPERTIES
      INTERFACE_LINK_LIBRARIES "${_irez_pdb_iface}")
    message(STATUS "Retargeted LLVMDebugInfoPDB diaguids.lib -> ${IREZ_DIAGUIDS_LIBRARY}")
  endif()
endif()

# SQLiteCpp (bundles its own sqlite3 amalgamation by default -> self-contained
# and portable across Linux/Windows).
set(SQLITECPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SQLITECPP_INCLUDE_SCRIPT OFF CACHE BOOL "" FORCE)
set(SQLITECPP_INSTALL OFF CACHE BOOL "" FORCE)
if(IREZ_DEPS_DIR AND EXISTS "${IREZ_DEPS_DIR}/SQLiteCpp/CMakeLists.txt")
  add_subdirectory("${IREZ_DEPS_DIR}/SQLiteCpp" "${CMAKE_BINARY_DIR}/_deps/SQLiteCpp"
                   EXCLUDE_FROM_ALL)
else()
  find_package(SQLiteCpp 3.3 QUIET CONFIG)
  if(NOT TARGET SQLiteCpp)
    set(_irez_sqlite_timestamp_args "")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
      list(APPEND _irez_sqlite_timestamp_args DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    endif()
    FetchContent_Declare(SQLiteCpp
      URL https://codeload.github.com/SRombauts/SQLiteCpp/tar.gz/3.3.3
      URL_HASH SHA256=33bd4372d83bc43117928ee842be64d05e7807f511b5195f85d30015cad9cac6
      ${_irez_sqlite_timestamp_args})
    FetchContent_GetProperties(SQLiteCpp)
    if(NOT sqlitecpp_POPULATED)
      FetchContent_Populate(SQLiteCpp)
      add_subdirectory("${sqlitecpp_SOURCE_DIR}" "${sqlitecpp_BINARY_DIR}"
                       EXCLUDE_FROM_ALL)
    endif()
  endif()
endif()

add_library(irez-core STATIC
  src/util.cpp
  src/db.cpp
  src/envelope.cpp
  src/store.cpp
  src/adapter.cpp
  src/service.cpp
)
target_include_directories(irez-core PUBLIC src)
if(MSVC)
  # Treat LLVM headers as external to silence third-party warnings.
  target_include_directories(irez-core SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
else()
  # Some LLVM packages bundle a libc++-oriented <cxxabi.h> beside llvm/. An
  # -isystem on that prefix shadows libstdc++'s ABI header, while -idirafter
  # can select an older /usr/include/llvm when several LLVM majors coexist.
  # Expose only LLVM's namespaced include trees through a build-local overlay.
  # Recreate it on every configure so changing LLVM_DIR cannot retain links to
  # the previously selected major.
  set(_irez_llvm_overlay_root "${CMAKE_CURRENT_BINARY_DIR}/llvm-system-include")
  file(REMOVE_RECURSE "${_irez_llvm_overlay_root}")
  set(_irez_llvm_overlays "")
  set(_irez_llvm_overlay_index 0)
  foreach(_irez_llvm_inc IN LISTS LLVM_INCLUDE_DIRS)
    set(_irez_llvm_overlay
        "${_irez_llvm_overlay_root}/${_irez_llvm_overlay_index}")
    file(MAKE_DIRECTORY "${_irez_llvm_overlay}")
    set(_irez_llvm_overlay_used FALSE)
    foreach(_irez_llvm_tree llvm llvm-c)
      if(EXISTS "${_irez_llvm_inc}/${_irez_llvm_tree}")
        file(CREATE_LINK "${_irez_llvm_inc}/${_irez_llvm_tree}"
             "${_irez_llvm_overlay}/${_irez_llvm_tree}" SYMBOLIC)
        set(_irez_llvm_overlay_used TRUE)
      endif()
    endforeach()
    if(_irez_llvm_overlay_used)
      list(APPEND _irez_llvm_overlays "${_irez_llvm_overlay}")
    endif()
    math(EXPR _irez_llvm_overlay_index "${_irez_llvm_overlay_index} + 1")
  endforeach()
  if(NOT _irez_llvm_overlays)
    message(FATAL_ERROR "LLVM_INCLUDE_DIRS contains no llvm/ or llvm-c/ trees")
  endif()
  target_include_directories(irez-core SYSTEM PUBLIC ${_irez_llvm_overlays})
endif()
target_compile_definitions(irez-core PUBLIC ${LLVM_DEFINITIONS})
target_compile_definitions(irez-core PUBLIC IREZ_VERSION="${CMAKE_PROJECT_VERSION}")
target_compile_definitions(irez-core PUBLIC IREZ_BUILD_REVISION="${IREZ_BUILD_REVISION}")
target_link_libraries(irez-core PUBLIC SQLiteCpp ${IREZ_LLVM_LIBS})

add_executable(irez src/cli_main.cpp)
target_link_libraries(irez PRIVATE irez-core)

add_executable(irez-llvm-index src/adapter_main.cpp)
target_link_libraries(irez-llvm-index PRIVATE irez-core)

install(TARGETS irez irez-llvm-index RUNTIME DESTINATION bin)
install(FILES LICENSE NOTICE THIRD_PARTY_NOTICES.md README.md CHANGELOG.md SECURITY.md
        DESTINATION .)
install(DIRECTORY docs fixtures DESTINATION .)

option(IREZ_BUILD_TESTS "Build IREZ unit tests" ON)
if(IREZ_BUILD_TESTS)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  if(IREZ_STATIC_MSVC_RUNTIME)
    set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
  else()
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  endif()
  if(IREZ_DEPS_DIR AND EXISTS "${IREZ_DEPS_DIR}/GoogleTest/CMakeLists.txt")
    add_subdirectory("${IREZ_DEPS_DIR}/GoogleTest" "${CMAKE_BINARY_DIR}/_deps/GoogleTest"
                     EXCLUDE_FROM_ALL)
  else()
    find_package(GTest 1.17 QUIET CONFIG)
    if(NOT TARGET GTest::gtest)
      FetchContent_Declare(GoogleTest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723)
      FetchContent_GetProperties(GoogleTest)
      if(NOT googletest_POPULATED)
        FetchContent_Populate(GoogleTest)
        add_subdirectory("${googletest_SOURCE_DIR}" "${googletest_BINARY_DIR}"
                         EXCLUDE_FROM_ALL)
      endif()
    endif()
  endif()
  enable_testing()
  add_subdirectory(tests)
endif()
