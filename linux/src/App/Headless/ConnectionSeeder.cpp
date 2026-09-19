#include "App/Headless/ConnectionSeeder.h"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "Core/Enums/DatabaseType.h"
#include "Core/Errors/GridexError.h"
#include "Core/Models/Database/ConnectionConfig.h"
#include "Data/Keychain/SecretStore.h"
#include "Data/Persistence/AppConnectionRepository.h"

namespace gridex {

namespace {

nlohmann::json readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw ConfigurationError("ConnectionSeeder: cannot open " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        return nlohmann::json::parse(ss.str());
    } catch (const std::exception& e) {
        throw SerializationError(std::string("ConnectionSeeder: invalid JSON in ") + path + ": " + e.what());
    }
}

std::string requireString(const nlohmann::json& j, const char* key, const std::string& id) {
    if (!j.contains(key) || !j[key].is_string()) {
        throw SerializationError(std::string("ConnectionSeeder: '") + key + "' (string) required for connection " + id);
    }
    return j[key].get<std::string>();
}

std::string normalizeDbType(const std::string& raw) {
    if (raw == "postgres" || raw == "pg") return "postgresql";
    return raw;
}

}  // namespace

std::vector<ConnectionSeeder::SeededConnection> ConnectionSeeder::seedFromFile(
    const std::string& path, AppConnectionRepository& repo, SecretStore& secrets) {
    const nlohmann::json root = readFile(path);
    // Accept either a single connection object or an array of them.
    nlohmann::json entries = root.is_array()
                                 ? root
                                 : nlohmann::json::array({root});
    if (!entries.is_array()) throw SerializationError("ConnectionSeeder: root must be a JSON object or array");

    std::vector<SeededConnection> seeded;
    seeded.reserve(entries.size());

    for (const auto& entry : entries) {
        if (!entry.is_object()) throw SerializationError("ConnectionSeeder: each connection must be an object");

        const std::string id = requireString(entry, "id", "<missing>");
        const std::string name = entry.contains("name") && entry["name"].is_string()
                                     ? entry["name"].get<std::string>()
                                     : id;
        const std::string typeRaw = normalizeDbType(requireString(entry, "databaseType", id));
        const auto dbType = databaseTypeFromRaw(typeRaw);
        if (!dbType) {
            throw SerializationError("ConnectionSeeder: unknown databaseType '" + typeRaw + "' for connection " + id);
        }

        ConnectionConfig cfg;
        cfg.id = id;
        cfg.name = name;
        cfg.databaseType = *dbType;
        if (entry.contains("host") && entry["host"].is_string())     cfg.host = entry["host"].get<std::string>();
        if (entry.contains("port") && entry["port"].is_number())     cfg.port = entry["port"].get<int>();
        if (entry.contains("database") && entry["database"].is_string()) cfg.database = entry["database"].get<std::string>();
        if (entry.contains("username") && entry["username"].is_string()) cfg.username = entry["username"].get<std::string>();
        if (entry.contains("sslEnabled") && entry["sslEnabled"].is_boolean()) cfg.sslEnabled = entry["sslEnabled"].get<bool>();
        if (entry.contains("group") && entry["group"].is_string())   cfg.group = entry["group"].get<std::string>();
        if (entry.contains("filePath") && entry["filePath"].is_string()) cfg.filePath = entry["filePath"].get<std::string>();

        const bool existed = repo.fetchById(id).has_value();
        repo.save(cfg);

        if (entry.contains("password") && entry["password"].is_string()) {
            secrets.savePassword(id, entry["password"].get<std::string>());
        }

        SeededConnection out;
        out.id = id;
        out.created = !existed;
        if (entry.contains("mcpMode") && entry["mcpMode"].is_string()) {
            out.mode = mcpConnectionModeFromRaw(entry["mcpMode"].get<std::string>());
            if (!out.mode) {
                throw SerializationError("ConnectionSeeder: invalid mcpMode for connection " + id +
                                         " (expected locked|read_only|read_write)");
            }
        }
        seeded.push_back(std::move(out));
    }
    return seeded;
}

}  // namespace gridex
