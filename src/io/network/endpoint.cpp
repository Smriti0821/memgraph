#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>

#include "glog/logging.h"

#include "io/network/endpoint.hpp"

namespace io::network {

Endpoint::Endpoint() {}
Endpoint::Endpoint(const std::string &address, uint16_t port)
    : address_(address), port_(port) {
  in_addr addr4;
  in6_addr addr6;
  int ipv4_result = inet_pton(AF_INET, address_.c_str(), &addr4);
  int ipv6_result = inet_pton(AF_INET6, address_.c_str(), &addr6);
  if (ipv4_result == 1)
    family_ = 4;
  else if (ipv6_result == 1)
    family_ = 6;
  CHECK(family_ != 0) << "Not a valid IPv4 or IPv6 address: " << address;
}

bool Endpoint::operator==(const Endpoint &other) const {
  return address_ == other.address_ && port_ == other.port_ &&
         family_ == other.family_;
}

std::ostream &operator<<(std::ostream &os, const Endpoint &endpoint) {
  return os << "Address:" << endpoint.address() << "Port:" << endpoint.port();
}

}  // namespace io::network
