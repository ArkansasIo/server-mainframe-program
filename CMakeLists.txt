cmake_minimum_required(VERSION 3.20)

project(mainframe
    VERSION 1.0.0
    DESCRIPTION "Mainframe Server System: HTTP control plane, TN3270-style terminal gateway, dataset transfer, relational database layer"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Platform / third-party
# ---------------------------------------------------------------------------
if(WIN32)
    # MinGW-w64 static runtime so the produced .exe is self contained.
    add_link_options(-static -static-libgcc -static-libstdc++)
    set(MF_PLATFORM_LIBS ws2_32)
else()
    set(MF_PLATFORM_LIBS pthread)
endif()

find_package(Threads REQUIRED)
list(APPEND MF_PLATFORM_LIBS Threads::Threads)

# SQLite: use a system library when present, otherwise fall back to the
# amalgamation shipped in third_party/sqlite (define MF_WITH_SQLITE=ON).
option(MF_BUNDLED_SQLITE "Compile the bundled sqlite3.c amalgamation" ON)
option(MF_BUILD_TESTS "Build the test executables" ON)

set(MF_SQLITE_SOURCES "")
if(MF_BUNDLED_SQLITE AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/sqlite/sqlite3.c")
    set(MF_SQLITE_SOURCES "${CMAKE_SOURCE_DIR}/third_party/sqlite/sqlite3.c")
    add_definitions(-DMF_HAS_SQLITE=1)
    message(STATUS "Using bundled SQLite amalgamation")
else()
    find_library(MF_SQLITE_LIB NAMES sqlite3 libsqlite3)
    if(MF_SQLITE_LIB)
        list(APPEND MF_PLATFORM_LIBS ${MF_SQLITE_LIB})
        add_definitions(-DMF_HAS_SQLITE=1)
        message(STATUS "Using system SQLite: ${MF_SQLITE_LIB}")
    else()
        message(WARNING "SQLite not found - building with the in-memory driver only")
    endif()
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(MF_SOURCES
    src/core/errors.cpp
    src/core/logger.cpp
    src/core/audit.cpp
    src/core/security.cpp
    src/core/datasets.cpp
    src/core/jobs.cpp
    src/core/transfer.cpp

    src/db/sqlite.cpp
    src/db/migrations.cpp
    src/db/repository.cpp

    src/config/config.cpp

    src/http/http.cpp
    src/http/router.cpp
    src/http/handlers.cpp
    src/http/static_files.cpp

    src/tcp/tcp.cpp
    src/tcp/terminal.cpp
    src/tcp/transfer_server.cpp

    src/util/csv.cpp
    src/util/json.cpp
    src/util/strings.cpp
    src/util/table.cpp
    src/util/time.cpp
    src/util/fs.cpp
    ${MF_SQLITE_SOURCES}
)

add_library(mainframe_core STATIC ${MF_SOURCES})
target_include_directories(mainframe_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sqlite)
target_link_libraries(mainframe_core PUBLIC ${MF_PLATFORM_LIBS})

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
add_executable(mainframe src/main.cpp)
target_link_libraries(mainframe PRIVATE mainframe_core)

add_executable(mf-db scripts/db_tool.cpp)
target_link_libraries(mf-db PRIVATE mainframe_core)

add_executable(mf-sheet scripts/sheet_tool.cpp)
target_link_libraries(mf-sheet PRIVATE mainframe_core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(MF_BUILD_TESTS)
    enable_testing()
    add_executable(mf-tests
        tests/test_main.cpp
        tests/test_strings.cpp
        tests/test_json.cpp
        tests/test_config.cpp
        tests/test_csv.cpp
        tests/test_hash.cpp
        tests/test_table.cpp
        tests/test_envelope.cpp
        tests/test_transfer.cpp
        tests/test_database.cpp)
    target_link_libraries(mf-tests PRIVATE mainframe_core)
    add_test(NAME mf-tests COMMAND mf-tests)
endif()

message(STATUS "mainframe ${PROJECT_VERSION} configured (${CMAKE_BUILD_TYPE})")
