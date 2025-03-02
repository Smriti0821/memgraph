// Copyright 2024 Memgraph Ltd.
//
// Licensed as a Memgraph Enterprise file under the Memgraph Enterprise
// License (the "License"); by using this file, you agree to be bound by the terms of the License, and you may not use
// this file except in compliance with the License. You may obtain a copy of the License at https://memgraph.com/legal.
//
//

#include "auth/auth.hpp"

#include <iostream>
#include <optional>
#include <utility>

#include <fmt/format.h>

#include "auth/crypto.hpp"
#include "auth/exceptions.hpp"
#include "auth/rpc.hpp"
#include "license/license.hpp"
#include "system/transaction.hpp"
#include "utils/flag_validation.hpp"
#include "utils/message.hpp"
#include "utils/settings.hpp"
#include "utils/string.hpp"

DEFINE_VALIDATED_string(auth_module_executable, "", "Absolute path to the auth module executable that should be used.",
                        {
                          if (value.empty()) return true;
                          // Check the file status, following symlinks.
                          auto status = std::filesystem::status(value);
                          if (!std::filesystem::is_regular_file(status)) {
                            std::cerr << "The auth module path doesn't exist or isn't a file!" << std::endl;
                            return false;
                          }
                          return true;
                        });
DEFINE_bool(auth_module_create_missing_user, true, "Set to false to disable creation of missing users.");
DEFINE_bool(auth_module_create_missing_role, true, "Set to false to disable creation of missing roles.");
DEFINE_bool(auth_module_manage_roles, true, "Set to false to disable management of roles through the auth module.");
DEFINE_VALIDATED_int32(auth_module_timeout_ms, 10000,
                       "Timeout (in milliseconds) used when waiting for a "
                       "response from the auth module.",
                       FLAG_IN_RANGE(100, 1800000));

namespace memgraph::auth {

namespace {
#ifdef MG_ENTERPRISE
/**
 * REPLICATION SYSTEM ACTION IMPLEMENTATIONS
 */
struct UpdateAuthData : memgraph::system::ISystemAction {
  explicit UpdateAuthData(User user) : user_{std::move(user)}, role_{std::nullopt} {}
  explicit UpdateAuthData(Role role) : user_{std::nullopt}, role_{std::move(role)} {}

  void DoDurability() override { /* Done during Auth execution */
  }

  bool DoReplication(replication::ReplicationClient &client, const utils::UUID &main_uuid,
                     replication::ReplicationEpoch const &epoch,
                     memgraph::system::Transaction const &txn) const override {
    auto check_response = [](const replication::UpdateAuthDataRes &response) { return response.success; };
    if (user_) {
      return client.SteamAndFinalizeDelta<replication::UpdateAuthDataRpc>(
          check_response, main_uuid, std::string{epoch.id()}, txn.last_committed_system_timestamp(), txn.timestamp(),
          *user_);
    }
    if (role_) {
      return client.SteamAndFinalizeDelta<replication::UpdateAuthDataRpc>(
          check_response, main_uuid, std::string{epoch.id()}, txn.last_committed_system_timestamp(), txn.timestamp(),
          *role_);
    }
    // Should never get here
    MG_ASSERT(false, "Trying to update auth data that is not a user nor a role");
    return {};
  }

  void PostReplication(replication::RoleMainData &mainData) const override {}

 private:
  std::optional<User> user_;
  std::optional<Role> role_;
};

struct DropAuthData : memgraph::system::ISystemAction {
  enum class AuthDataType { USER, ROLE };

  explicit DropAuthData(AuthDataType type, std::string_view name) : type_{type}, name_{name} {}

  void DoDurability() override { /* Done during Auth execution */
  }

  bool DoReplication(replication::ReplicationClient &client, const utils::UUID &main_uuid,
                     replication::ReplicationEpoch const &epoch,
                     memgraph::system::Transaction const &txn) const override {
    auto check_response = [](const replication::DropAuthDataRes &response) { return response.success; };

    memgraph::replication::DropAuthDataReq::DataType type{};
    switch (type_) {
      case AuthDataType::USER:
        type = memgraph::replication::DropAuthDataReq::DataType::USER;
        break;
      case AuthDataType::ROLE:
        type = memgraph::replication::DropAuthDataReq::DataType::ROLE;
        break;
    }
    return client.SteamAndFinalizeDelta<replication::DropAuthDataRpc>(
        check_response, main_uuid, std::string{epoch.id()}, txn.last_committed_system_timestamp(), txn.timestamp(),
        type, name_);
  }
  void PostReplication(replication::RoleMainData &mainData) const override {}

 private:
  AuthDataType type_;
  std::string name_;
};
#endif

/**
 * CONSTANTS
 */
const std::string kUserPrefix = "user:";
const std::string kRolePrefix = "role:";
const std::string kLinkPrefix = "link:";
const std::string kVersion = "version";

static constexpr auto kVersionV1 = "V1";
}  // namespace

/**
 * All data stored in the `Auth` storage is stored in an underlying
 * `kvstore::KVStore`. Because we are using a key-value store to store the data,
 * the data has to be encoded. The encoding used is as follows:
 *
 * User: key="user:<username>", value="<json_encoded_members_of_user>"
 * Role: key="role:<rolename>", value="<json_endoded_members_of_role>"
 *
 * The User->Role relationship isn't stored in the `User` encoded data because
 * we want to be able to delete/modify a Role and have it automatically be
 * removed/modified in all linked users. Because of that we store the links to
 * the role as a foreign-key like mapping in the KVStore. It is saved as
 * follows:
 *
 * key="link:<username>", value="<rolename>"
 */

namespace {
void MigrateVersions(kvstore::KVStore &store) {
  static constexpr auto kPasswordHashV0V1 = "password_hash";
  auto version_str = store.Get(kVersion);

  if (!version_str) {
    using namespace std::string_literals;

    // pre versioning, add version to the store
    auto puts = std::map<std::string, std::string>{{kVersion, kVersionV1}};

    // also add hash kind into durability

    auto it = store.begin(kUserPrefix);
    auto const e = store.end(kUserPrefix);

    if (it != e) {
      const auto hash_algo = CurrentHashAlgorithm();
      spdlog::info("Updating auth durability, assuming previously stored as {}", AsString(hash_algo));

      for (; it != e; ++it) {
        auto const &[key, value] = *it;
        try {
          auto user_data = nlohmann::json::parse(value);

          auto password_hash = user_data[kPasswordHashV0V1];
          if (!password_hash.is_string()) {
            throw AuthException("Couldn't load user data!");
          }
          // upgrade the password_hash to include the hash algortihm
          if (password_hash.empty()) {
            user_data[kPasswordHashV0V1] = nullptr;
          } else {
            user_data[kPasswordHashV0V1] = HashedPassword{hash_algo, password_hash};
          }
          puts.emplace(key, user_data.dump());
        } catch (const nlohmann::json::parse_error &e) {
          throw AuthException("Couldn't load user data!");
        }
      }
    }

    // Perform migration to V1
    store.PutMultiple(puts);
    version_str = kVersionV1;
  }
}
};  // namespace

Auth::Auth(std::string storage_directory, Config config)
    : storage_(std::move(storage_directory)), module_(FLAGS_auth_module_executable), config_{std::move(config)} {
  MigrateVersions(storage_);
}

std::optional<User> Auth::Authenticate(const std::string &username, const std::string &password) {
  if (module_.IsUsed()) {
    const auto license_check_result = license::global_license_checker.IsEnterpriseValid(utils::global_settings);
    if (license_check_result.HasError()) {
      spdlog::warn(license::LicenseCheckErrorToString(license_check_result.GetError(), "authentication modules"));
      return std::nullopt;
    }

    nlohmann::json params = nlohmann::json::object();
    params["username"] = username;
    params["password"] = password;

    auto ret = module_.Call(params, FLAGS_auth_module_timeout_ms);

    // Verify response integrity.
    if (!ret.is_object() || ret.find("authenticated") == ret.end() || ret.find("role") == ret.end()) {
      return std::nullopt;
    }
    const auto &ret_authenticated = ret.at("authenticated");
    const auto &ret_role = ret.at("role");
    if (!ret_authenticated.is_boolean() || !ret_role.is_string()) {
      return std::nullopt;
    }
    auto is_authenticated = ret_authenticated.get<bool>();
    const auto &rolename = ret_role.get<std::string>();

    // Authenticate the user.
    if (!is_authenticated) return std::nullopt;

    /**
     * TODO
     * The auth module should not update auth data.
     * There is now way to replicate it and we should not be storing sensitive data if we don't have to.
     */

    // Find or create the user and return it.
    auto user = GetUser(username);
    if (!user) {
      if (FLAGS_auth_module_create_missing_user) {
        user = AddUser(username, password);
        if (!user) {
          spdlog::warn(utils::MessageWithLink(
              "Couldn't create the missing user '{}' using the auth module because the user already exists as a role.",
              username, "https://memgr.ph/auth"));
          return std::nullopt;
        }
      } else {
        spdlog::warn(utils::MessageWithLink(
            "Couldn't authenticate user '{}' using the auth module because the user doesn't exist.", username,
            "https://memgr.ph/auth"));
        return std::nullopt;
      }
    } else {
      UpdatePassword(*user, password);
    }
    if (FLAGS_auth_module_manage_roles) {
      if (!rolename.empty()) {
        auto role = GetRole(rolename);
        if (!role) {
          if (FLAGS_auth_module_create_missing_role) {
            role = AddRole(rolename);
            if (!role) {
              spdlog::warn(
                  utils::MessageWithLink("Couldn't authenticate user '{}' using the auth module because the user's "
                                         "role '{}' already exists as a user.",
                                         username, rolename, "https://memgr.ph/auth"));
              return std::nullopt;
            }
            SaveRole(*role);
          } else {
            spdlog::warn(utils::MessageWithLink(
                "Couldn't authenticate user '{}' using the auth module because the user's role '{}' doesn't exist.",
                username, rolename, "https://memgr.ph/auth"));
            return std::nullopt;
          }
        }
        user->SetRole(*role);
      } else {
        user->ClearRole();
      }
    }
    SaveUser(*user);
    return user;
  } else {
    auto user = GetUser(username);
    if (!user) {
      spdlog::warn(utils::MessageWithLink("Couldn't authenticate user '{}' because the user doesn't exist.", username,
                                          "https://memgr.ph/auth"));
      return std::nullopt;
    }
    if (!user->CheckPassword(password)) {
      spdlog::warn(utils::MessageWithLink("Couldn't authenticate user '{}' because the password is not correct.",
                                          username, "https://memgr.ph/auth"));
      return std::nullopt;
    }
    if (user->UpgradeHash(password)) {
      SaveUser(*user);
    }

    return user;
  }
}

std::optional<User> Auth::GetUser(const std::string &username_orig) const {
  auto username = utils::ToLowerCase(username_orig);
  auto existing_user = storage_.Get(kUserPrefix + username);
  if (!existing_user) return std::nullopt;

  nlohmann::json data;
  try {
    data = nlohmann::json::parse(*existing_user);
  } catch (const nlohmann::json::parse_error &e) {
    throw AuthException("Couldn't load user data!");
  }

  auto user = User::Deserialize(data);
  auto link = storage_.Get(kLinkPrefix + username);

  if (link) {
    auto role = GetRole(*link);
    if (role) {
      user.SetRole(*role);
    }
  }
  return user;
}

void Auth::SaveUser(const User &user, system::Transaction *system_tx) {
  bool success = false;
  if (const auto *role = user.role(); role != nullptr) {
    success = storage_.PutMultiple(
        {{kUserPrefix + user.username(), user.Serialize().dump()}, {kLinkPrefix + user.username(), role->rolename()}});
  } else {
    success = storage_.PutAndDeleteMultiple({{kUserPrefix + user.username(), user.Serialize().dump()}},
                                            {kLinkPrefix + user.username()});
  }
  if (!success) {
    throw AuthException("Couldn't save user '{}'!", user.username());
  }
  // All changes to the user end up calling this function, so no need to add a delta anywhere else
  if (system_tx) {
#ifdef MG_ENTERPRISE
    system_tx->AddAction<UpdateAuthData>(user);
#endif
  }
}

void Auth::UpdatePassword(auth::User &user, const std::optional<std::string> &password) {
  // Check if null
  if (!password) {
    if (!config_.password_permit_null) {
      throw AuthException("Null passwords aren't permitted!");
    }
  } else {
    // Check if compliant with our filter
    if (config_.custom_password_regex) {
      if (const auto license_check_result = license::global_license_checker.IsEnterpriseValid(utils::global_settings);
          license_check_result.HasError()) {
        throw AuthException(
            "Custom password regex is a Memgraph Enterprise feature. Please set the config "
            "(\"--auth-password-strength-regex\") to its default value (\"{}\") or remove the flag.\n{}",
            glue::kDefaultPasswordRegex,
            license::LicenseCheckErrorToString(license_check_result.GetError(), "password regex"));
      }
    }
    if (!std::regex_match(*password, config_.password_regex)) {
      throw AuthException(
          "The user password doesn't conform to the required strength! Regex: "
          "\"{}\"",
          config_.password_regex_str);
    }
  }

  // All checks passed; update
  user.UpdatePassword(password);
}

std::optional<User> Auth::AddUser(const std::string &username, const std::optional<std::string> &password,
                                  system::Transaction *system_tx) {
  if (!NameRegexMatch(username)) {
    throw AuthException("Invalid user name.");
  }
  auto existing_user = GetUser(username);
  if (existing_user) return std::nullopt;
  auto existing_role = GetRole(username);
  if (existing_role) return std::nullopt;
  auto new_user = User(username);
  UpdatePassword(new_user, password);
  SaveUser(new_user, system_tx);
  return new_user;
}

bool Auth::RemoveUser(const std::string &username_orig, system::Transaction *system_tx) {
  auto username = utils::ToLowerCase(username_orig);
  if (!storage_.Get(kUserPrefix + username)) return false;
  std::vector<std::string> keys({kLinkPrefix + username, kUserPrefix + username});
  if (!storage_.DeleteMultiple(keys)) {
    throw AuthException("Couldn't remove user '{}'!", username);
  }
  // Handling drop user delta
  if (system_tx) {
#ifdef MG_ENTERPRISE
    system_tx->AddAction<DropAuthData>(DropAuthData::AuthDataType::USER, username);
#endif
  }
  return true;
}

std::vector<auth::User> Auth::AllUsers() const {
  std::vector<auth::User> ret;
  for (auto it = storage_.begin(kUserPrefix); it != storage_.end(kUserPrefix); ++it) {
    auto username = it->first.substr(kUserPrefix.size());
    if (username != utils::ToLowerCase(username)) continue;
    auto user = GetUser(username);
    if (user) {
      ret.push_back(std::move(*user));
    }
  }
  return ret;
}

std::vector<std::string> Auth::AllUsernames() const {
  std::vector<std::string> ret;
  for (auto it = storage_.begin(kUserPrefix); it != storage_.end(kUserPrefix); ++it) {
    auto username = it->first.substr(kUserPrefix.size());
    if (username != utils::ToLowerCase(username)) continue;
    auto user = GetUser(username);
    if (user) {
      ret.push_back(username);
    }
  }
  return ret;
}

bool Auth::HasUsers() const { return storage_.begin(kUserPrefix) != storage_.end(kUserPrefix); }

std::optional<Role> Auth::GetRole(const std::string &rolename_orig) const {
  auto rolename = utils::ToLowerCase(rolename_orig);
  auto existing_role = storage_.Get(kRolePrefix + rolename);
  if (!existing_role) return std::nullopt;

  nlohmann::json data;
  try {
    data = nlohmann::json::parse(*existing_role);
  } catch (const nlohmann::json::parse_error &e) {
    throw AuthException("Couldn't load role data!");
  }

  return Role::Deserialize(data);
}

void Auth::SaveRole(const Role &role, system::Transaction *system_tx) {
  if (!storage_.Put(kRolePrefix + role.rolename(), role.Serialize().dump())) {
    throw AuthException("Couldn't save role '{}'!", role.rolename());
  }
  // All changes to the role end up calling this function, so no need to add a delta anywhere else
  if (system_tx) {
#ifdef MG_ENTERPRISE
    system_tx->AddAction<UpdateAuthData>(role);
#endif
  }
}

std::optional<Role> Auth::AddRole(const std::string &rolename, system::Transaction *system_tx) {
  if (!NameRegexMatch(rolename)) {
    throw AuthException("Invalid role name.");
  }
  if (auto existing_role = GetRole(rolename)) return std::nullopt;
  if (auto existing_user = GetUser(rolename)) return std::nullopt;
  auto new_role = Role(rolename);
  SaveRole(new_role, system_tx);
  return new_role;
}

bool Auth::RemoveRole(const std::string &rolename_orig, system::Transaction *system_tx) {
  auto rolename = utils::ToLowerCase(rolename_orig);
  if (!storage_.Get(kRolePrefix + rolename)) return false;
  std::vector<std::string> keys;
  for (auto it = storage_.begin(kLinkPrefix); it != storage_.end(kLinkPrefix); ++it) {
    if (utils::ToLowerCase(it->second) == rolename) {
      keys.push_back(it->first);
    }
  }
  keys.push_back(kRolePrefix + rolename);
  if (!storage_.DeleteMultiple(keys)) {
    throw AuthException("Couldn't remove role '{}'!", rolename);
  }
  // Handling drop role delta
  if (system_tx) {
#ifdef MG_ENTERPRISE
    system_tx->AddAction<DropAuthData>(DropAuthData::AuthDataType::ROLE, rolename);
#endif
  }
  return true;
}

std::vector<auth::Role> Auth::AllRoles() const {
  std::vector<auth::Role> ret;
  for (auto it = storage_.begin(kRolePrefix); it != storage_.end(kRolePrefix); ++it) {
    auto rolename = it->first.substr(kRolePrefix.size());
    if (rolename != utils::ToLowerCase(rolename)) continue;
    if (auto role = GetRole(rolename)) {
      ret.push_back(*role);
    } else {
      throw AuthException("Couldn't load role '{}'!", rolename);
    }
  }
  return ret;
}

std::vector<std::string> Auth::AllRolenames() const {
  std::vector<std::string> ret;
  for (auto it = storage_.begin(kRolePrefix); it != storage_.end(kRolePrefix); ++it) {
    auto rolename = it->first.substr(kRolePrefix.size());
    if (rolename != utils::ToLowerCase(rolename)) continue;
    if (auto role = GetRole(rolename)) {
      ret.push_back(rolename);
    }
  }
  return ret;
}

std::vector<auth::User> Auth::AllUsersForRole(const std::string &rolename_orig) const {
  const auto rolename = utils::ToLowerCase(rolename_orig);
  std::vector<auth::User> ret;
  for (auto it = storage_.begin(kLinkPrefix); it != storage_.end(kLinkPrefix); ++it) {
    auto username = it->first.substr(kLinkPrefix.size());
    if (username != utils::ToLowerCase(username)) continue;
    if (it->second != utils::ToLowerCase(it->second)) continue;
    if (it->second == rolename) {
      if (auto user = GetUser(username)) {
        ret.push_back(std::move(*user));
      } else {
        throw AuthException("Couldn't load user '{}'!", username);
      }
    }
  }
  return ret;
}

#ifdef MG_ENTERPRISE
bool Auth::GrantDatabaseToUser(const std::string &db, const std::string &name, system::Transaction *system_tx) {
  if (auto user = GetUser(name)) {
    if (db == kAllDatabases) {
      user->db_access().GrantAll();
    } else {
      user->db_access().Add(db);
    }
    SaveUser(*user, system_tx);
    return true;
  }
  return false;
}

bool Auth::RevokeDatabaseFromUser(const std::string &db, const std::string &name, system::Transaction *system_tx) {
  if (auto user = GetUser(name)) {
    if (db == kAllDatabases) {
      user->db_access().DenyAll();
    } else {
      user->db_access().Remove(db);
    }
    SaveUser(*user, system_tx);
    return true;
  }
  return false;
}

void Auth::DeleteDatabase(const std::string &db, system::Transaction *system_tx) {
  for (auto it = storage_.begin(kUserPrefix); it != storage_.end(kUserPrefix); ++it) {
    auto username = it->first.substr(kUserPrefix.size());
    if (auto user = GetUser(username)) {
      user->db_access().Delete(db);
      SaveUser(*user, system_tx);
    }
  }
}

bool Auth::SetMainDatabase(std::string_view db, const std::string &name, system::Transaction *system_tx) {
  if (auto user = GetUser(name)) {
    if (!user->db_access().SetDefault(db)) {
      throw AuthException("Couldn't set default database '{}' for user '{}'!", db, name);
    }
    SaveUser(*user, system_tx);
    return true;
  }
  return false;
}
#endif

bool Auth::NameRegexMatch(const std::string &user_or_role) const {
  if (config_.custom_name_regex) {
    if (const auto license_check_result =
            memgraph::license::global_license_checker.IsEnterpriseValid(memgraph::utils::global_settings);
        license_check_result.HasError()) {
      throw memgraph::auth::AuthException(
          "Custom user/role regex is a Memgraph Enterprise feature. Please set the config "
          "(\"--auth-user-or-role-name-regex\") to its default value (\"{}\") or remove the flag.\n{}",
          glue::kDefaultUserRoleRegex,
          memgraph::license::LicenseCheckErrorToString(license_check_result.GetError(), "user/role regex"));
    }
  }
  return std::regex_match(user_or_role, config_.name_regex);
}

}  // namespace memgraph::auth
