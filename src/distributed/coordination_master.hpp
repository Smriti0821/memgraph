#pragma once

#include <mutex>
#include <unordered_map>

#include "communication/messaging/distributed.hpp"
#include "communication/rpc/rpc.hpp"
#include "io/network/endpoint.hpp"

namespace distributed {
using Endpoint = io::network::Endpoint;

/** Handles worker registration, getting of other workers' endpoints and
 * coordinated shutdown in a distributed memgraph. Master side. */
class MasterCoordination {
  /**
   * Registers a new worker with this master server. Notifies all the known
   * workers of the new worker.
   *
   * @param desired_worker_id - The ID the worker would like to have. Set to
   * -1 if the worker doesn't care. Does not guarantee that the desired ID will
   * be returned, it is possible it's already occupied. If that's an error (for
   * example in recovery), the worker should handle it as such.
   * @return The assigned ID for the worker asking to become registered.
   */
  int RegisterWorker(int desired_worker_id, Endpoint endpoint);

 public:
  explicit MasterCoordination(communication::messaging::System &system);

  /** Shuts down all the workers and this master server. */
  ~MasterCoordination();

  /** Returns the Endpoint for the given worker_id. */
  Endpoint GetEndpoint(int worker_id) const;

 private:
  communication::messaging::System &system_;
  communication::rpc::Server server_;
  // Most master functions aren't thread-safe.
  mutable std::mutex lock_;
  std::unordered_map<int, Endpoint> workers_;
};
}  // namespace distributed
