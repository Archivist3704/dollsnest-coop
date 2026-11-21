#include "dsh/sync/entity_registry.h"

#include "dsh/platform/log.h"

#include <cstdio>

namespace dsh::sync {

CreateResult EntityRegistry::create(PeerId authority, std::uint8_t kind,
                                    std::uint64_t ownerHandle, WorldGeneration generation,
                                    EntityId& outId) {
    if (authority == kInvalidPeer) {
        return CreateResult::rejected_invalid_id;
    }
    if (generation != generation_) {

        DSH_WARN("registry: refuse create for stale generation %u (current %u) -- "
                 "caller must re-resolve its world first",
                 generation, generation_);
        return CreateResult::rejected_invalid_id;
    }

    std::uint64_t& counter = nextCounter_[authority.value];
    if (counter == 0x00FFFFFFFFFFFFFFull) {
        return CreateResult::rejected_counters_exhausted;
    }
    ++counter;

    EntityId id;
    id.authority = authority;
    id.counter = counter;

    const auto key = pack(id);
    EntityRecord rec;
    rec.id = id;
    rec.ownerHandle = ownerHandle;
    rec.generation = generation;
    rec.kind = kind;
    rec.alive = true;

    const auto [it, inserted] = records_.emplace(key, rec);
    if (!inserted) {

        DSH_ERROR("registry: id %u:%llu already exists (kind %u, existing owner %llu) -- "
                  "this means two creation paths are live; returning already_exists",
                  id.authority.value, static_cast<unsigned long long>(id.counter),
                  kind, static_cast<unsigned long long>(it->second.ownerHandle));
        outId = it->second.id;
        return CreateResult::already_exists;
    }

    outId = id;
    DSH_DEBUG("registry: created %u:%llu kind=%u owner=%llu gen=%u (total %zu)",
              id.authority.value, static_cast<unsigned long long>(id.counter), kind,
              static_cast<unsigned long long>(ownerHandle), generation_, records_.size());
    return CreateResult::created;
}

bool EntityRegistry::destroy(EntityId id) {
    const auto key = pack(id);
    const auto it = records_.find(key);
    if (it == records_.end()) {

        DSH_DEBUG("registry: destroy for unknown id %u:%llu (ignored)",
                  id.authority.value, static_cast<unsigned long long>(id.counter));
        return false;
    }
    it->second.alive = false;
    records_.erase(it);
    DSH_DEBUG("registry: destroyed %u:%llu (total %zu)",
              id.authority.value, static_cast<unsigned long long>(id.counter), records_.size());
    return true;
}

const EntityRecord* EntityRegistry::find(EntityId id) const {
    const auto it = records_.find(pack(id));
    return it == records_.end() ? nullptr : &it->second;
}

std::size_t EntityRegistry::on_world_replaced() {
    const std::size_t dropped = records_.size();
    records_.clear();
    ++generation_;
    if (generation_ == kNoGeneration) ++generation_;

    DSH_INFO("registry: world replaced -> dropped %zu entities, generation now %u "
             "(all cached handles are dead by definition)",
             dropped, generation_);
    return dropped;
}

}
