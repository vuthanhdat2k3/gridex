add_library(gridex-app STATIC
    DependencyInjection/DIContainer.cpp
    Headless/ConnectionSeeder.cpp
    MCP/MCPConnectionProvider.cpp
)

target_include_directories(gridex-app PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(gridex-app PUBLIC
    gridex-core
    gridex-data
    gridex-services
)
