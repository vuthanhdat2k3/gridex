#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Core/Enums/MCPConnectionMode.h"

namespace gridex {

class AppConnectionRepository;
class SecretStore;

// Headless connection seeding for container deployments.
//
// Reads a JSON file describing saved connections:
//   [
//     {
//       "id": "hrm-demo",
//       "name": "HRM Customer Demo",
//       "databaseType": "postgresql",       // postgresql|mysql|sqlite|redis|mongodb|mssql|clickhouse
//       "host": "postgres", "port": 5432,
//       "database": "hrm_demo", "username": "hrm_ro",
//       "password": "...",                  // stored in SecretStore, never in config JSON
//       "sslEnabled": false,
//       "mcpMode": "read_only",             // locked|read_only|read_write
//       "group": "Demos"                    // optional
//     }
//   ]
// and upserts each into the AppDatabase (idempotent — keyed by connection id),
// stores the password in the SecretStore, and reports the requested MCP
// connection mode so the caller can persist it (QSettings).
class ConnectionSeeder {
public:
    struct SeededConnection {
        std::string id;
        std::optional<MCPConnectionMode> mode;
        bool created = false;  // false when an existing connection was updated
    };

    // Throws gridex::GridexError on malformed input / IO failure.
    static std::vector<SeededConnection> seedFromFile(const std::string& path,
                                                      AppConnectionRepository& repo,
                                                      SecretStore& secrets);
};

}  // namespace gridex
