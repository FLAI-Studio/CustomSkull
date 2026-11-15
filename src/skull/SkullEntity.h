#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <filesystem>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ll/api/event/Listener.h"
#include "mc/platform/UUID.h"

#include "skull/Types.h"

class Player;
class BlockPos;
class ItemStack;
class BlockSource;
class Block;

namespace custom_skull {

class SkinStore;

class SkullEntityManager {
public:
    explicit SkullEntityManager(SkinStore& store);
    ~SkullEntityManager();

    void registerEvents();
    void unregisterEvents();
    void startTicking();
    void startTabCleanup();

    void sendAll(Player& player);

    void spawnAt(BlockPos const& pos, SkinRecord const& record, SkinImage const& image, Player* source, int dimension, float yaw, float visualXOffset = 0.0f, float visualYOffset = 0.0f, float visualZOffset = 0.0f, bool reuseAny = false, bool persist = true);

    void restoreAll(Player& player);

    void noteGiven(Player& player, SkinRecord const& record);

private:
    struct FakeSkull {
        int         x = 0;
        int         y = 0;
        int         z = 0;
        std::string ownerUuid;
        std::string entityUuid;
        int64_t     uniqueId  = 0;
        uint64_t    runtimeId = 0;
        int64_t     hologramId = -1;
        int64_t     anchorId   = -1;
        int64_t     anchorUniqueId = 0;
        float       yaw       = 180.0f;
        float       xOffset   = 0.0f;
        float       yOffset   = 0.0f;
        float       zOffset   = 0.0f;
        int         dimension = 0;
    };

    void onInteract(Player& player, ItemStack& item);
    void onPlaced(Player& player, BlockPos const& pos, Block const& block);
    void onDestroy(BlockPos const& pos);
    void onBlockChanged(BlockSource& source, BlockPos const& pos, Block const& previous, Block const& current);
    void onJoin(Player& player);
    void tick();
    void cleanTabEntries();
    void scheduleTabCleanup();

    void spawn(FakeSkull const& skull, SkinRecord const& record, SkinImage const& image, Player& source, Player const* target, bool refreshOnly = false);
    void remove(FakeSkull const& skull, Player const* target);
    void sendSkullTo(FakeSkull const& skull, SkinRecord const& record, SkinImage const& image, Player const* target);
    void removeTabEntry(mce::UUID const& uuid);

    struct PlacedRecord {
        int         x         = 0;
        int         y         = 0;
        int         z         = 0;
        int         dimension = 0;
        std::string ownerUuid;
        float       yaw = 0.0f;
        float       xOffset = 0.0f;
        float       yOffset = 0.0f;
        float       zOffset = 0.0f;
        int64_t     anchorUniqueId = 0;
    };

    void loadPersistence();
    void savePersistence() const;
    void remember(BlockPos const& pos, std::string const& ownerUuid, int dimension, float yaw, float xOffset = 0.0f, float yOffset = 0.0f, float zOffset = 0.0f, int64_t anchorUniqueId = 0);
    void forget(BlockPos const& pos);

    SkinStore&                                                mStore;
    std::map<std::array<int, 3>, FakeSkull>                    mPlaced;
    std::unordered_map<std::string, std::string>               mPendingOwner;
    std::unordered_map<std::string, Player*>                   mPendingPlayer;
    std::deque<std::pair<std::string, std::string>>             mGiveQueue;
    std::vector<std::shared_ptr<ll::event::ListenerBase>>      mListeners;
    std::filesystem::path                                      mPersistencePath;
    std::vector<PlacedRecord>                                  mPersisted;
    bool                                                       mRestored = false;
    std::shared_ptr<std::atomic<bool>>                         mTickAlive;
    std::shared_ptr<std::atomic<bool>>                         mTabAlive;
};

} // namespace custom_skull
















