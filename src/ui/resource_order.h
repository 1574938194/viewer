#ifndef NEXUS_SRC_UI_RESOURCE_ORDER_H_
#define NEXUS_SRC_UI_RESOURCE_ORDER_H_

#include <unordered_map>
#include <vector>

#include "domain/resource.h"

namespace nexus::ui {

enum class ResourceSort {
  kRelevance,
  kRecentOpen,
  kHot,
  kAdded,
  kCreated,
  kFileSize
};

// Input is already relevance-ranked when using kRelevance.
void SortResources(std::vector<const Resource*>* items, ResourceSort mode,
                   bool browse);

class ResourceOrder {
 public:
  void Reset(const std::vector<const Resource*>& ordered);
  // Preserve existing ranks, append new IDs, and prune deleted IDs.
  void Reconcile(const std::vector<const Resource*>& items);
  void Apply(std::vector<const Resource*>* items) const;
  bool initialized() const { return initialized_; }

 private:
  bool initialized_ = false;
  std::size_t next_rank_ = 0;
  std::unordered_map<ResourceId, std::size_t> ranks_;
};

}  // namespace nexus::ui

#endif  // NEXUS_SRC_UI_RESOURCE_ORDER_H_
