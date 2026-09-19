find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3    REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(LIBSECRET  REQUIRED IMPORTED_TARGET libsecret-1)
pkg_check_modules(LIBPQ      REQUIRED IMPORTED_TARGET libpq)
pkg_check_modules(LIBMARIADB REQUIRED IMPORTED_TARGET libmariadb)
pkg_check_modules(HIREDIS    REQUIRED IMPORTED_TARGET hiredis)

# ---------------------------------------------------------------------------
# mongo-c-driver (libmongoc + libbson) — required by mongo-cxx-driver
# mongo-cxx-driver (mongocxx / bsoncxx)
#
# Headless/container builds can pre-install both drivers into CMAKE_PREFIX_PATH
# (see docker images): when find_package() already provides mongocxx/bsoncxx
# targets, the FetchContent download+compile is skipped entirely.
# ---------------------------------------------------------------------------
include(FetchContent)

find_package(bsoncxx CONFIG QUIET)
find_package(mongocxx CONFIG QUIET)

if(NOT TARGET mongocxx_shared AND NOT TARGET mongocxx_static
   AND NOT TARGET mongocxx::mongocxx_shared AND NOT TARGET mongocxx::mongocxx_static)
    FetchContent_Declare(mongo-c-driver
        GIT_REPOSITORY https://github.com/mongodb/mongo-c-driver.git
        GIT_TAG        1.27.1
        GIT_SHALLOW    TRUE
    )
    set(ENABLE_TESTS                       OFF CACHE BOOL "" FORCE)
    set(ENABLE_EXAMPLES                    OFF CACHE BOOL "" FORCE)
    set(ENABLE_AUTOMATIC_INIT_AND_CLEANUP  OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(mongo-c-driver)

    FetchContent_Declare(mongo-cxx-driver
        GIT_REPOSITORY https://github.com/mongodb/mongo-cxx-driver.git
        GIT_TAG        r3.10.1
        GIT_SHALLOW    TRUE
    )
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(mongo-cxx-driver)
endif()

add_library(gridex-data STATIC
    Adapters/AdapterFactory.cpp
    Adapters/ClickHouse/ClickhouseAdapter.cpp
    Adapters/MSSQL/MssqlAdapter.cpp
    Adapters/MySQL/MysqlAdapter.cpp
    Adapters/MongoDB/MongodbAdapter.cpp
    Adapters/PostgreSQL/PostgresAdapter.cpp
    Adapters/Redis/RedisAdapter.cpp
    Adapters/SQLite/SqliteAdapter.cpp
    Keychain/SecretStore.cpp
    Persistence/AppConnectionRepository.cpp
    Persistence/AppDatabase.cpp
    Persistence/SavedQueryRepository.cpp
)

target_include_directories(gridex-data PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

find_package(Qt6 6.4 REQUIRED COMPONENTS Network)

# Prefer the FetchContent-scope (unnamespaced) targets when the drivers were
# built in-tree; installed packages expose the namespaced ones instead.
if(TARGET mongocxx_static)
    set(GRIDEX_MONGOCXX_TARGET mongocxx_static)
elseif(TARGET mongocxx_shared)
    set(GRIDEX_MONGOCXX_TARGET mongocxx_shared)
elseif(TARGET mongocxx::mongocxx_shared)
    set(GRIDEX_MONGOCXX_TARGET mongocxx::mongocxx_shared)
else()
    set(GRIDEX_MONGOCXX_TARGET mongocxx::mongocxx_static)
endif()

target_link_libraries(gridex-data
    PUBLIC  gridex-core
    PRIVATE
        PkgConfig::SQLITE3
        PkgConfig::LIBSECRET
        PkgConfig::LIBPQ
        PkgConfig::LIBMARIADB
        PkgConfig::HIREDIS
        sybdb
        ${GRIDEX_MONGOCXX_TARGET}
        # ClickHouse adapter speaks the HTTP API — Qt Network for transport,
        # nlohmann/json (already fetched in src/Services/) for JSONCompact parse.
        Qt6::Network
        nlohmann_json::nlohmann_json
)
