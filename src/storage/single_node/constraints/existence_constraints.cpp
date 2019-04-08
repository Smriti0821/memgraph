#include "storage/single_node/constraints/existence_constraints.hpp"

#include "storage/single_node/constraints/common.hpp"

namespace storage::constraints {
bool Contains(const PropertyValueStore &store,
              const std::vector<storage::Property> &properties) {
  for (auto property : properties) {
    if (store.at(property).IsNull()) {
      return false;
    }
  }

  return true;
}

bool CheckIfSatisfiesExistenceRule(const Vertex *vertex,
                                   const ExistenceRule &rule) {
  if (!utils::Contains(vertex->labels_, rule.label)) return true;
  if (!Contains(vertex->properties_, rule.properties)) return false;

  return true;
}

bool ExistenceConstraints::AddConstraint(const ExistenceRule &rule) {
  auto found = std::find(constraints_.begin(), constraints_.end(), rule);
  if (found != constraints_.end()) return false;

  constraints_.push_back(rule);
  return true;
}

bool ExistenceConstraints::RemoveConstraint(const ExistenceRule &rule) {
  auto found = std::find(constraints_.begin(), constraints_.end(), rule);
  if (found != constraints_.end()) {
    std::swap(*found, constraints_.back());
    constraints_.pop_back();
    return true;
  }

  return false;
}

bool ExistenceConstraints::Exists(const ExistenceRule &rule) const {
  auto found = std::find(constraints_.begin(), constraints_.end(), rule);
  return found != constraints_.end();
}

bool ExistenceConstraints::CheckOnAddLabel(const Vertex *vertex,
                                           storage::Label label) const {
  for (auto &constraint : constraints_) {
    if (constraint.label == label &&
        !CheckIfSatisfiesExistenceRule(vertex, constraint)) {
      return false;
    }
  }
  return true;
}

bool ExistenceConstraints::CheckOnRemoveProperty(
    const Vertex *vertex, storage::Property property) const {
  for (auto &constraint : constraints_) {
    if (utils::Contains(constraint.properties, property) &&
        !CheckIfSatisfiesExistenceRule(vertex, constraint)) {
      return false;
    }
  }
  return true;
}

const std::vector<ExistenceRule> &ExistenceConstraints::ListConstraints() const {
  return constraints_;
}
}  // namespace storage::constraints
