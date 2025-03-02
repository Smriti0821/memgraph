// Copyright 2024 Memgraph Ltd.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
// License, and you may not use this file except in compliance with the Business Source License.
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0, included in the file
// licenses/APL.txt.

#pragma once

#include "utils/uuid.hpp"
#ifdef MG_ENTERPRISE

#include "coordination/coordinator_config.hpp"
#include "rpc/client.hpp"
#include "utils/scheduler.hpp"

namespace memgraph::coordination {

class CoordinatorInstance;
using HealthCheckCallback = std::function<void(CoordinatorInstance *, std::string_view)>;
using ReplicationClientsInfo = std::vector<ReplClientInfo>;

class CoordinatorClient {
 public:
  explicit CoordinatorClient(CoordinatorInstance *coord_instance, CoordinatorClientConfig config,
                             HealthCheckCallback succ_cb, HealthCheckCallback fail_cb);

  ~CoordinatorClient() = default;

  CoordinatorClient(CoordinatorClient &) = delete;
  CoordinatorClient &operator=(CoordinatorClient const &) = delete;

  CoordinatorClient(CoordinatorClient &&) noexcept = delete;
  CoordinatorClient &operator=(CoordinatorClient &&) noexcept = delete;

  void StartFrequentCheck();
  void StopFrequentCheck();
  void PauseFrequentCheck();
  void ResumeFrequentCheck();

  auto InstanceName() const -> std::string;
  auto SocketAddress() const -> std::string;

  [[nodiscard]] auto DemoteToReplica() const -> bool;
  auto SendPromoteReplicaToMainRpc(const utils::UUID &uuid, ReplicationClientsInfo replication_clients_info) const
      -> bool;

  auto SendSwapMainUUIDRpc(const utils::UUID &uuid) const -> bool;

  auto ReplicationClientInfo() const -> ReplClientInfo;

  auto SetCallbacks(HealthCheckCallback succ_cb, HealthCheckCallback fail_cb) -> void;

  auto RpcClient() -> rpc::Client & { return rpc_client_; }

  friend bool operator==(CoordinatorClient const &first, CoordinatorClient const &second) {
    return first.config_ == second.config_;
  }

 private:
  utils::Scheduler instance_checker_;

  // TODO: (andi) Pimpl?
  communication::ClientContext rpc_context_;
  mutable rpc::Client rpc_client_;

  CoordinatorClientConfig config_;
  CoordinatorInstance *coord_instance_;
  HealthCheckCallback succ_cb_;
  HealthCheckCallback fail_cb_;
};

}  // namespace memgraph::coordination
#endif
