#include "ui/resource_order.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

#include "util/string_util.h"

namespace nexus::ui {

void SortResources(std::vector<const Resource*>* items, ResourceSort mode,
                   bool browse)
{
  std::stable_sort(
      items->begin(), items->end(),
      [mode, browse](const Resource* left, const Resource* right)
      {
        if (browse) {
          const bool left_parent = left->originalFilename == "..";
          const bool right_parent = right->originalFilename == "..";
          if (left_parent != right_parent) {
            return left_parent;
          }
          const bool left_folder = left->type == ResourceType::kFolder;
          const bool right_folder = right->type == ResourceType::kFolder;
          if (left_folder != right_folder) {
            return left_folder;
          }
          if (left_folder) {
            return AsciiLower(left->originalFilename) <
                   AsciiLower(right->originalFilename);
          }
        }
        switch (mode) {
          case ResourceSort::kRelevance:
            return false;
          case ResourceSort::kRecentOpen:
            if (left->lastOpenTime != right->lastOpenTime) {
              return left->lastOpenTime > right->lastOpenTime;
            }
            break;
          case ResourceSort::kHot:
            if (left->hotScore != right->hotScore) {
              return left->hotScore > right->hotScore;
            }
            break;
          case ResourceSort::kAdded:
          case ResourceSort::kCreated:
            if (left->addedTime != right->addedTime) {
              return left->addedTime > right->addedTime;
            }
            break;
          case ResourceSort::kFileSize:
            if (left->fileSize != right->fileSize) {
              return left->fileSize > right->fileSize;
            }
            break;
        }
        return left->id > right->id;
      });
}

void ResourceOrder::Reset(const std::vector<const Resource*>& ordered)
{
  initialized_ = true;
  ranks_.clear();
  next_rank_ = 0;
  Reconcile(ordered);
}

void ResourceOrder::Reconcile(const std::vector<const Resource*>& items)
{
  std::unordered_set<ResourceId> present;
  for (const Resource* item : items) {
    present.insert(item->id);
    if (!ranks_.contains(item->id)) {
      ranks_.emplace(item->id, next_rank_++);
    }
  }
  std::erase_if(ranks_, [&present](const auto& entry)
                { return !present.contains(entry.first); });
}

void ResourceOrder::Apply(std::vector<const Resource*>* items) const
{
  const auto rank = [this](ResourceId id)
  {
    const auto found = ranks_.find(id);
    return found == ranks_.end() ? std::numeric_limits<std::size_t>::max()
                                 : found->second;
  };
  std::stable_sort(items->begin(), items->end(),
                   [&rank](const Resource* left, const Resource* right)
                   { return rank(left->id) < rank(right->id); });
}

}  // namespace nexus::ui
