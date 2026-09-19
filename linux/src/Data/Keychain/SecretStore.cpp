#include "Data/Keychain/SecretStore.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <libsecret/secret.h>
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <stdexcept>
#include <string>
#include <utility>

#include "Core/Errors/GridexError.h"

namespace fs = std::filesystem;

namespace gridex {

namespace {

// Schema used for all Gridex credentials. Stored attributes are:
//   "service" = logical service name (e.g. com.gridex.credentials)
//   "account" = key name (e.g. db.password.<connection-id>)
const SecretSchema* gridexSchema() {
    static const SecretSchema schema = {
        "com.gridex.Secret",
        SECRET_SCHEMA_NONE,
        {
            { "service", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { "account", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { nullptr,   SECRET_SCHEMA_ATTRIBUTE_STRING },
        },
        0, 0, 0, 0, 0, 0, 0, 0
    };
    return &schema;
}

[[noreturn]] void throwFromGError(const std::string& ctx, GError* err) {
    std::string msg = ctx;
    if (err) {
        msg += ": ";
        msg += err->message ? err->message : "unknown libsecret error";
        g_error_free(err);
    } else {
        msg += ": operation returned failure with no detail (is the Secret Service daemon running?)";
    }
    throw InternalError(msg);
}

}

SecretStore::SecretStore() : SecretStore("com.gridex.credentials") {}

SecretStore::SecretStore(std::string serviceName) : serviceName_(std::move(serviceName)) {
    // Headless deployment: GRIDEX_SECRET_STORE=file:<path> selects the JSON
    // file backend (no Secret Service daemon required, e.g. containers).
    if (const char* env = std::getenv("GRIDEX_SECRET_STORE"); env && *env) {
        const std::string spec(env);
        if (spec.rfind("file:", 0) == 0) {
            if (spec.size() <= 5) throw InternalError("GRIDEX_SECRET_STORE: empty file path");
            backend_ = Backend::File;
            filePath_ = spec.substr(5);
        }
    }
}

bool SecretStore::isAvailable() {
    if (backend_ == Backend::File) {
        // The file backend is always "available"; failures surface on use.
        return true;
    }
    // Probe by doing a harmless lookup on a reserved key. We don't cache across
    // process restarts; a single in-process static would mask a user switching
    // keyring daemons, which is rare but possible.
    GError* err = nullptr;
    gchar* value = secret_password_lookup_sync(
        gridexSchema(), nullptr, &err,
        "service", serviceName_.c_str(),
        "account", "__gridex_probe__",
        nullptr);
    if (err) { g_error_free(err); return false; }
    if (value) g_free(value);
    return true;
}

void SecretStore::save(std::string_view key, std::string_view value) {
    if (backend_ == Backend::File) {
        std::lock_guard lk(mu_);
        std::map<std::string, std::string> data;
        fileLoadAll(data);
        data[std::string(key)] = std::string(value);
        fileStoreAll(data);
        return;
    }
    const std::string account(key);
    const std::string password(value);
    const std::string label = "Gridex: " + account;

    GError* err = nullptr;
    const gboolean ok = secret_password_store_sync(
        gridexSchema(),
        SECRET_COLLECTION_DEFAULT,
        label.c_str(),
        password.c_str(),
        nullptr,
        &err,
        "service", serviceName_.c_str(),
        "account", account.c_str(),
        nullptr);
    if (!ok || err) throwFromGError("SecretStore::save", err);
}

std::optional<std::string> SecretStore::load(std::string_view key) {
    if (backend_ == Backend::File) {
        std::lock_guard lk(mu_);
        std::map<std::string, std::string> data;
        fileLoadAll(data);
        const auto it = data.find(std::string(key));
        if (it == data.end()) return std::nullopt;
        return it->second;
    }
    const std::string account(key);
    GError* err = nullptr;
    gchar* raw = secret_password_lookup_sync(
        gridexSchema(), nullptr, &err,
        "service", serviceName_.c_str(),
        "account", account.c_str(),
        nullptr);
    if (err) throwFromGError("SecretStore::load", err);
    if (!raw) return std::nullopt;

    std::string out(raw);
    secret_password_free(raw);
    return out;
}

void SecretStore::remove(std::string_view key) {
    if (backend_ == Backend::File) {
        std::lock_guard lk(mu_);
        std::map<std::string, std::string> data;
        fileLoadAll(data);
        data.erase(std::string(key));
        fileStoreAll(data);
        return;
    }
    const std::string account(key);
    GError* err = nullptr;
    secret_password_clear_sync(
        gridexSchema(), nullptr, &err,
        "service", serviceName_.c_str(),
        "account", account.c_str(),
        nullptr);
    if (err) throwFromGError("SecretStore::remove", err);
}

void SecretStore::fileLoadAll(std::map<std::string, std::string>& out) const {
    std::ifstream in(filePath_, std::ios::binary);
    if (!in) return;  // missing file → empty store
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        const auto j = nlohmann::json::parse(ss.str());
        if (j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.value().is_string()) out[it.key()] = it.value().get<std::string>();
            }
        }
    } catch (const std::exception& e) {
        throw InternalError(std::string("SecretStore: cannot parse secrets file: ") + e.what());
    }
}

void SecretStore::fileStoreAll(const std::map<std::string, std::string>& data) const {
    namespace fs = std::filesystem;
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [k, v] : data) j[k] = v;
    const std::string payload = j.dump(2);

    // Atomic write: temp sibling + rename, and restrict to owner-only perms.
    const fs::path target(filePath_);
    std::error_code ec;
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
    const fs::path tmp = target.string() + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw InternalError("SecretStore: cannot write " + tmp.string());
        out << payload;
    }
    fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, ec);
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw InternalError("SecretStore: cannot finalize " + target.string() + ": " + ec.message());
    }
}

}
