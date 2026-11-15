#include "skull/SkullEntity.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <format>
#include <optional>
#include <utility>

#include <nlohmann/json.hpp>

#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDestroyBlockEvent.h"
#include "ll/api/event/player/PlayerInteractBlockEvent.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/event/player/PlayerPlaceBlockEvent.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ServerThreadExecutor.h"

#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/platform/BuildPlatform.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/network/packet/AddPlayerPacket.h"
#include "mc/network/packet/NetworkChunkPublisherUpdatePacket.h"
#include "mc/network/packet/PlayerListPacket.h"
#include "mc/network/packet/PlayerListPacketPayload.h"
#include "mc/network/packet/PlayerSkinPacket.h"
#include "mc/network/packet/RemoveActorPacket.h"
#include "mc/network/packet/cerealize/core/SerializationMode.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/PlayerPermissionLevel.h"
#include "mc/world/actor/DataItem.h"
#include "mc/world/actor/SerializedAbilitiesData.h"
#include "mc/deps/vanilla_components/ActorDataFlagComponent.h"
#include "mc/server/ServerLevel.h"
#include "mc/server/commands/ServerCommandOrigin.h"
#include "mc/world/Direction.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorFlags.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/phys/AABB.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/NetworkItemStackDescriptor.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/ChunkPos.h"
#include "mc/world/level/Level.h"
#include "mc/server/NetworkChunkPublisher.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/states/BuiltInBlockStates.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/block/actor/BlockActorType.h"
#include "mc/world/level/block/actor/SkullBlockActor.h"

#include "hologram/HologramBridge.h"
#include "mod/CustomSkull.h"
#include "skull/SkinFactory.h"
#include "skull/SkinStore.h"
#include "skull/Uuid.h"

namespace custom_skull {

namespace {

using Key = std::array<int, 3>;

bool isSkullBlockName(std::string const& name) {
    return name == "minecraft:skull" || name == "minecraft:player_head";
}

float flipSkullYaw(float yaw) {
    yaw += 180.0f;
    if (yaw >= 360.0f) {
        yaw -= 360.0f;
    }
    return yaw;
}

struct WallSkullInfo {
    float yaw;
    float xOffset;
    float yOffset;
    float zOffset;
};

std::optional<WallSkullInfo> wallSkullInfo(BlockSource& source, BlockPos const& pos) {
    auto& block = source.getBlock(pos);
    auto& type  = block.getBlockType();
    auto readState = [&](char const* name) {
        return type.getState<int>(HashedString::computeHash(name), block.getData());
    };

    auto facingU = block.getState<uchar>(BuiltInBlockStates::FacingDirection());
    auto facingI = block.getState<int>(BuiltInBlockStates::FacingDirection());
    auto namedFacing = readState("facing_direction");
    int  facing      = -1;
    if (facingU) {
        facing = *facingU;
    } else if (facingI) {
        facing = *facingI;
    } else if (namedFacing) {
        facing = *namedFacing;
    }
    switch (facing) {
    case 2:
        return WallSkullInfo{180.0f, 0.0f, 0.25f, 0.23f};
    case 3:
        return WallSkullInfo{0.0f, 0.0f, 0.25f, -0.23f};
    case 4:
        return WallSkullInfo{90.0f, 0.23f, 0.25f, 0.0f};
    case 5:
        return WallSkullInfo{270.0f, -0.23f, 0.25f, 0.0f};
    default:
        break;
    }

    auto cardinalU = block.getState<uchar>(BuiltInBlockStates::CardinalDirection());
    auto cardinalE = block.getState<Direction::Type>(BuiltInBlockStates::CardinalDirection());
    auto namedCardinal = readState("cardinal_direction");
    int  cardinal  = -1;
    if (cardinalU) {
        cardinal = *cardinalU;
    } else if (cardinalE) {
        cardinal = static_cast<int>(*cardinalE);
    } else if (namedCardinal) {
        cardinal = *namedCardinal;
    }
    switch (cardinal) {
    case 0:
        return WallSkullInfo{0.0f, 0.0f, 0.25f, -0.23f};
    case 1:
        return WallSkullInfo{90.0f, 0.23f, 0.25f, 0.0f};
    case 2:
        return WallSkullInfo{180.0f, 0.0f, 0.25f, 0.23f};
    case 3:
        return WallSkullInfo{270.0f, -0.23f, 0.25f, 0.0f};
    default:
        break;
    }

    // Wall-mounted player heads store the attachment face in the block data
    // nibble (2..5); floor heads use the entity rotation instead.
    int data = block.getData();
    if (data >= 2 && data <= 5) {
        BlockPos below{pos.x, pos.y - 1, pos.z};
        if (source.getBlock(below).getTypeName() == "minecraft:air") {
            switch (data) {
            case 2:
                return WallSkullInfo{180.0f, 0.0f, 0.25f, 0.23f};
            case 3:
                return WallSkullInfo{0.0f, 0.0f, 0.25f, -0.23f};
            case 4:
                return WallSkullInfo{90.0f, 0.23f, 0.25f, 0.0f};
            case 5:
                return WallSkullInfo{270.0f, -0.23f, 0.25f, 0.0f};
            default:
                break;
            }
        }
    }
    return std::nullopt;
}

Key keyOf(BlockPos const& pos) {
    return {pos.x, pos.y, pos.z};
}

Actor* spawnServerAnchor(Player& player, BlockPos const& pos, int dimension, float yOffset) {
    auto level = ll::service::getLevel();
    if (!level) {
        return nullptr;
    }
    auto& serverLevel = static_cast<ServerLevel&>(*level);
    auto  x           = static_cast<float>(pos.x) + 0.5f;
    auto  y           = static_cast<float>(pos.y) - 0.01f + yOffset;
    auto  z           = static_cast<float>(pos.z) + 0.5f;

    auto& region = player.getDimensionBlockSource();
    AABB  box{Vec3{x - 1.0f, y - 2.0f, z - 1.0f}, Vec3{x + 1.5f, y + 2.0f, z + 1.5f}};
    auto  findNear = [&]() -> Actor* {
        auto  actors   = region.getEntities(box, 4.0f, ActorType::ArmorStand, false);
        CustomSkull::getInstance().logger().info("[skull] anchor candidates={}", actors.size());
        Actor* best    = nullptr;
        float  bestDist = 1e9f;
        for (auto* actor : actors) {
            if (!actor) {
                continue;
            }
            auto dist = (actor->getPosition() - Vec3{x, y, z}).length();
            if (dist < bestDist) {
                bestDist = dist;
                best     = actor;
            }
        }
        return bestDist < 1.5f ? best : nullptr;
    };

    Actor* anchor = findNear();
    if (!anchor) {
        ServerCommandOrigin origin(
            "CustomSkull",
            serverLevel,
            CommandPermissionLevel::GameDirectors,
            static_cast<DimensionType>(dimension)
        );
        auto command = std::format("summon minecraft:armor_stand {} {} {}", x, y, z);
        auto output  = ll::command::CommandRegistrar::getServerInstance().executeCommand(command, origin);
        std::string message;
        for (auto const& entry : output.mMessages) {
            if (!message.empty()) {
                message += "; ";
            }
            message += entry.mMessageId;
        }
        CustomSkull::getInstance().logger().info(
            "[skull] summon '{}' success={} messages='{}'",
            command,
            output.mSuccessCount,
            message
        );
        anchor = findNear();
    }
    if (!anchor) {
        return nullptr;
    }

    if (auto flags = anchor->getEntityContext().tryGetComponent<ActorDataFlagComponent>()) {
        flags->mValue.set(static_cast<size_t>(ActorFlags::HasGravity), false);
        flags->mValue.set(static_cast<size_t>(ActorFlags::HasCollision), false);
    }
    anchor->setInvisible(false);
    anchor->setPersistent();
    return anchor;
}

int64_t uniqueIdFor(std::string const& uuid) {
    auto hash = static_cast<uint64_t>(std::hash<std::string>{}(uuid));
    return static_cast<int64_t>(hash & 0x7FFFFFFFFFFFFFFFULL);
}

std::string entityUuidFor(std::string const& ownerUuid) {
    auto base = parseUuid(ownerUuid);
    return uuidToString(mce::UUID(base.a, base.b ^ 0x9E3779B97F4A7C15ULL));
}

std::string fakeName(std::string const& uuid) {
    (void)uuid;
    return "\xC2\xA7r";
}

} // namespace

SkullEntityManager::SkullEntityManager(SkinStore& store)
: mStore(store),
  mPersistencePath(store.root() / "placed_skulls.json") {
    loadPersistence();
}

SkullEntityManager::~SkullEntityManager() { unregisterEvents(); }

void SkullEntityManager::registerEvents() {
    auto& bus = ll::event::EventBus::getInstance();

    mListeners.push_back(bus.emplaceListener<ll::event::PlayerInteractBlockEvent>(
        [this](ll::event::PlayerInteractBlockEvent& event) { onInteract(event.self(), event.item()); }
    ));
    mListeners.push_back(bus.emplaceListener<ll::event::PlayerPlacedBlockEvent>(
        [this](ll::event::PlayerPlacedBlockEvent& event) {
            onPlaced(event.self(), event.pos(), event.placedBlock());
        }
    ));
    mListeners.push_back(bus.emplaceListener<ll::event::PlayerDestroyBlockEvent>(
        [this](ll::event::PlayerDestroyBlockEvent& event) { onDestroy(event.pos()); }
    ));
    mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>(
        [this](ll::event::BlockChangedEvent& event) {
            onBlockChanged(event.blockSource(), event.pos(), event.previousBlock(), event.newBlock());
        }
    ));
}

void SkullEntityManager::unregisterEvents() {
    if (mTickAlive) {
        *mTickAlive = false;
    }
    if (mTabAlive) {
        *mTabAlive = false;
    }
    auto& bus = ll::event::EventBus::getInstance();
    for (auto& listener : mListeners) {
        if (listener) {
            bus.removeListener(listener);
        }
    }
    mListeners.clear();
}

void SkullEntityManager::onInteract(Player& player, ItemStack& item) {
    std::string playerUuid = uuidToString(player.getUuid());
    auto&       logger     = CustomSkull::getInstance().logger();
    if (!isSkullBlockName(item.getTypeName()) || !item.mUserData || !item.mUserData->contains("SkullOwner")) {
        if (isSkullBlockName(item.getTypeName())) {
            logger.info("[skull] interact skull without SkullOwner type={}", item.getTypeName());
        }
        mPendingOwner.erase(playerUuid);
        mPendingPlayer.erase(playerUuid);
        return;
    }
    auto& owner = (*item.mUserData)["SkullOwner"];
    if (!owner.is_string()) {
        logger.warn("[skull] SkullOwner is not a string");
        mPendingOwner.erase(playerUuid);
        mPendingPlayer.erase(playerUuid);
        return;
    }
    mPendingOwner[playerUuid]  = static_cast<std::string const&>(owner);
    mPendingPlayer[playerUuid] = &player;
    logger.info("[skull] interact skull owner={}", mPendingOwner[playerUuid]);
}

void SkullEntityManager::onPlaced(Player& player, BlockPos const& pos, Block const& block) {
    auto& logger = CustomSkull::getInstance().logger();
    logger.info("[skull] placed block={} pos={}", block.getTypeName(), pos.toString());
    if (!isSkullBlockName(block.getTypeName())) {
        return;
    }
    if (mPlaced.contains(keyOf(pos))) {
        return;
    }
    auto it = mPendingOwner.find(uuidToString(player.getUuid()));
    if (it == mPendingOwner.end()) {
        logger.warn("[skull] placed skull but no pending owner, trying give queue");
        while (!mGiveQueue.empty()) {
            auto [queuedOwner, queuedPlayer] = mGiveQueue.front();
            mGiveQueue.pop_front();
            auto* queuedRecord = mStore.findByUuid(queuedOwner);
            if (!queuedRecord) {
                continue;
            }
            auto queuedImage = mStore.image(*queuedRecord);
            if (!queuedImage) {
                continue;
            }
            float queuedYaw = player.getRotation().y;
            if (auto* actor = player.getDimensionBlockSource().getBlockEntity(pos);
                actor && actor->getType() == BlockActorType::Skull) {
                queuedYaw = flipSkullYaw(static_cast<SkullBlockActor*>(actor)->mRotation);
            }
            logger.info("[skull] placed-queue spawn owner={} yaw={}", queuedOwner, queuedYaw);
            spawnAt(pos, *queuedRecord, *queuedImage, &player, static_cast<int>(player.getDimensionId()), queuedYaw);
            return;
        }
        return;
    }
    std::string ownerUuid = it->second;
    mPendingOwner.erase(it);
    mPendingPlayer.erase(uuidToString(player.getUuid()));
    for (auto queueIt = mGiveQueue.begin(); queueIt != mGiveQueue.end(); ++queueIt) {
        if (queueIt->first == ownerUuid) {
            mGiveQueue.erase(queueIt);
            break;
        }
    }
    logger.info("[skull] placed skull owner={}", ownerUuid);

    auto* record = mStore.findByUuid(ownerUuid);
    if (!record) {
        return;
    }
    auto image = mStore.image(*record);
    if (!image) {
        return;
    }

    float yaw = player.getRotation().y;
    if (auto* actor = player.getDimensionBlockSource().getBlockEntity(pos);
        actor && actor->getType() == BlockActorType::Skull) {
        float raw = static_cast<SkullBlockActor*>(actor)->mRotation;
        yaw       = flipSkullYaw(raw);
        logger.info("[skull] rotation actor=true raw={} final={}", raw, yaw);
    } else {
        logger.info("[skull] rotation actor=false playerYaw={}", yaw);
    }
    logger.info("[skull] spawn skull yaw={}", yaw);

    SkinRecord recordCopy = *record;
    SkinImage  imageCopy  = *image;
    Player*    source     = &player;
    int        dimension  = static_cast<int>(player.getDimensionId());
    ll::thread::ServerThreadExecutor::getDefault().executeAfter(
        [this, pos, recordCopy = std::move(recordCopy), imageCopy = std::move(imageCopy), source, dimension, yaw]() {
            BlockPos actualPos = pos;
            if (source) {
                auto& region = source->getDimensionBlockSource();
                for (int dy = -2; dy <= 2; ++dy) {
                    BlockPos probe{pos.x, pos.y + dy, pos.z};
                    CustomSkull::getInstance().logger().info(
                        "[skull] probe y={} type={}",
                        probe.y,
                        region.getBlock(probe).getTypeName()
                    );
                }
            }
            if (mPlaced.contains(keyOf(actualPos))) {
                return;
            }
            float spawnYaw    = yaw;
            float spawnXOffset = 0.0f;
            float spawnOffset = 0.0f;
            float spawnZOffset = 0.0f;
            if (source) {
                auto& region   = source->getDimensionBlockSource();
                auto  wallInfo = wallSkullInfo(region, actualPos);
                auto  facingState = region.getBlock(actualPos).getState<uchar>(BuiltInBlockStates::FacingDirection());
                auto  cardinalState = region.getBlock(actualPos).getState<Direction::Type>(BuiltInBlockStates::CardinalDirection());
                auto& blockNow = region.getBlock(actualPos);
                auto namedFacing = blockNow.getBlockType().getState<int>(
                    HashedString::computeHash("facing_direction"),
                    blockNow.getData()
                );
                auto namedCardinal = blockNow.getBlockType().getState<int>(
                    HashedString::computeHash("cardinal_direction"),
                    blockNow.getData()
                );
                CustomSkull::getInstance().logger().info(
                    "[skull] wall state facing={} cardinal={} namedFacing={} namedCardinal={} data={}",
                    facingState ? static_cast<int>(*facingState) : -1,
                    cardinalState ? static_cast<int>(*cardinalState) : -1,
                    namedFacing.value_or(-1),
                    namedCardinal.value_or(-1),
                    blockNow.getData()
                );
                for (auto const& [stateName, stateId] : blockNow.getBlockType().mStateNameMap.get()) {
                    auto value = blockNow.getBlockType().getState<int>(stateId, blockNow.getData());
                    CustomSkull::getInstance().logger().info(
                        "[skull] state {}={}",
                        stateName.getString(),
                        value.value_or(-999)
                    );
                }
                if (wallInfo) {
                    spawnYaw     = wallInfo->yaw;
                    spawnXOffset = wallInfo->xOffset;
                    spawnOffset  = wallInfo->yOffset;
                    spawnZOffset = wallInfo->zOffset;
                    CustomSkull::getInstance().logger().info(
                        "[skull] wall skull facing={} yaw={} offset=({},{},{})",
                        static_cast<int>(region.getBlock(actualPos).getState<uchar>(BuiltInBlockStates::FacingDirection()).value_or(0)),
                        spawnYaw,
                        spawnXOffset,
                        spawnOffset,
                        spawnZOffset
                    );
                } else if (auto* actor = region.getBlockEntity(actualPos);
                           actor && actor->getType() == BlockActorType::Skull) {
                    spawnYaw = flipSkullYaw(static_cast<SkullBlockActor*>(actor)->mRotation);
                }
            }
            spawnAt(actualPos, recordCopy, imageCopy, source, dimension, spawnYaw, spawnXOffset, spawnOffset, spawnZOffset);
        },
        std::chrono::milliseconds(50)
    );
}

void SkullEntityManager::onDestroy(BlockPos const& pos) {
    auto it = mPlaced.find(keyOf(pos));
    if (it == mPlaced.end()) {
        return;
    }
    forget(pos);
    remove(it->second, nullptr);
    mPlaced.erase(it);
}

void SkullEntityManager::onBlockChanged(BlockSource& source, BlockPos const& pos, Block const& previous, Block const& current) {
    if (isSkullBlockName(previous.getTypeName()) && !isSkullBlockName(current.getTypeName())) {
        onDestroy(pos);
        return;
    }
    if (isSkullBlockName(previous.getTypeName()) || !isSkullBlockName(current.getTypeName())) {
        return;
    }
    if (mPlaced.contains(keyOf(pos))) {
        auto placedIt = mPlaced.find(keyOf(pos));
        if (placedIt != mPlaced.end() && placedIt->second.hologramId >= 0) {
            float yaw    = placedIt->second.yaw;
            float xOff   = placedIt->second.xOffset;
            float offset = placedIt->second.yOffset;
            float zOff   = placedIt->second.zOffset;
            if (auto wallInfo = wallSkullInfo(source, pos)) {
                yaw    = wallInfo->yaw;
                xOff   = wallInfo->xOffset;
                offset = wallInfo->yOffset;
                zOff   = wallInfo->zOffset;
            } else if (auto* actor = source.getBlockEntity(pos);
                       actor && actor->getType() == BlockActorType::Skull) {
                yaw = flipSkullYaw(static_cast<SkullBlockActor*>(actor)->mRotation);
            }
            placedIt->second.yaw     = yaw;
            placedIt->second.xOffset = xOff;
            placedIt->second.yOffset = offset;
            placedIt->second.zOffset = zOff;
            if (auto& hologram = HologramBridge::getInstance(); hologram.available()) {
                hologram.setRotationLight(placedIt->second.hologramId, yaw);
                hologram.setPositionLight(
                    placedIt->second.hologramId,
                    static_cast<float>(pos.x) + 0.5f + placedIt->second.xOffset,
                    static_cast<float>(pos.y) - 0.01f + offset,
                    static_cast<float>(pos.z) + 0.5f + placedIt->second.zOffset,
                    placedIt->second.dimension
                );
            }
            CustomSkull::getInstance().logger().info("[skull] late rotation yaw={} offset={}", yaw, offset);
        }
        return;
    }

    auto& logger = CustomSkull::getInstance().logger();
    for (auto it = mPendingOwner.begin(); it != mPendingOwner.end(); ++it) {
        auto playerIt = mPendingPlayer.find(it->first);
        if (playerIt == mPendingPlayer.end() || !playerIt->second) {
            continue;
        }
        auto* record = mStore.findByUuid(it->second);
        if (!record) {
            continue;
        }
        auto image = mStore.image(*record);
        if (!image) {
            continue;
        }
        auto* player       = playerIt->second;
        float yaw          = player->getRotation().y;
        float visualX     = 0.0f;
        float visualOffset = 0.0f;
        float visualZ     = 0.0f;
        if (auto wallInfo = wallSkullInfo(source, pos)) {
            yaw          = wallInfo->yaw;
            visualX      = wallInfo->xOffset;
            visualOffset = wallInfo->yOffset;
            visualZ      = wallInfo->zOffset;
        } else if (auto* actor = source.getBlockEntity(pos);
                   actor && actor->getType() == BlockActorType::Skull) {
            yaw = flipSkullYaw(static_cast<SkullBlockActor*>(actor)->mRotation);
        }
        logger.info("[skull] block-changed fallback spawn owner={} pos={} yaw={}", it->second, pos.toString(), yaw);
        mPendingOwner.erase(it);
        mPendingPlayer.erase(playerIt);
        for (auto queueIt = mGiveQueue.begin(); queueIt != mGiveQueue.end(); ++queueIt) {
            if (queueIt->first == record->uuid) {
                mGiveQueue.erase(queueIt);
                break;
            }
        }
        spawnAt(
            pos,
            *record,
            *image,
            player,
            static_cast<int>(source.getDimensionId()),
            yaw,
            visualX,
            visualOffset,
            visualZ
        );
        return;
    }

    while (!mGiveQueue.empty()) {
        auto [ownerUuid, playerUuid] = mGiveQueue.front();
        mGiveQueue.pop_front();
        auto* record = mStore.findByUuid(ownerUuid);
        if (!record) {
            continue;
        }
        auto image = mStore.image(*record);
        if (!image) {
            continue;
        }
        float yaw          = 0.0f;
        float visualX      = 0.0f;
        float visualOffset = 0.0f;
        float visualZ      = 0.0f;
        if (auto wallInfo = wallSkullInfo(source, pos)) {
            yaw          = wallInfo->yaw;
            visualX      = wallInfo->xOffset;
            visualOffset = wallInfo->yOffset;
            visualZ      = wallInfo->zOffset;
        } else if (auto* actor = source.getBlockEntity(pos);
                   actor && actor->getType() == BlockActorType::Skull) {
            yaw = flipSkullYaw(static_cast<SkullBlockActor*>(actor)->mRotation);
        }
        logger.info("[skull] give-queue fallback spawn owner={} pos={} yaw={}", ownerUuid, pos.toString(), yaw);
        spawnAt(pos, *record, *image, nullptr, static_cast<int>(source.getDimensionId()), yaw, visualX, visualOffset, visualZ);
        return;
    }
}

void SkullEntityManager::sendAll(Player& player) { onJoin(player); }

void SkullEntityManager::startTicking() {
    if (mTickAlive || CustomSkull::getInstance().config().useHologramLib) {
        return;
    }
    mTickAlive = std::make_shared<std::atomic<bool>>(true);
    ll::thread::ServerThreadExecutor::getDefault().executeAfter(
        [this, alive = mTickAlive]() {
            if (!alive->load()) {
                return;
            }
            tick();
        },
        std::chrono::seconds(1)
    );
}

void SkullEntityManager::startTabCleanup() {
    if (mTabAlive) {
        return;
    }
    mTabAlive = std::make_shared<std::atomic<bool>>(true);
    scheduleTabCleanup();
}

void SkullEntityManager::scheduleTabCleanup() {
    ll::thread::ServerThreadExecutor::getDefault().executeAfter(
        [this, alive = mTabAlive]() {
            if (!alive->load()) {
                return;
            }
            cleanTabEntries();
            if (alive->load()) {
                scheduleTabCleanup();
            }
        },
        std::chrono::seconds(1)
    );
}

void SkullEntityManager::cleanTabEntries() {
    if (!mTabAlive || !mTabAlive->load()) {
        return;
    }
    std::vector<std::pair<std::string, mce::UUID>> entries;
    auto add = [&](mce::UUID uuid) {
        auto key = uuidToString(uuid);
        for (auto const& [existing, _] : entries) {
            if (existing == key) {
                return;
            }
        }
        entries.emplace_back(std::move(key), uuid);
    };

    for (auto const& [key, skull] : mPlaced) {
        if (skull.hologramId >= 0) {
            add(mce::UUID(0xF0B34E5043000000ULL, static_cast<uint64>(skull.hologramId)));
        }
        if (!skull.entityUuid.empty()) {
            add(parseUuid(skull.entityUuid));
        }
    }
    for (auto const& record : mStore.all()) {
        add(parseUuid(entityUuidFor(record.uuid)));
    }
    for (uint64 i = 1; i <= 64; ++i) {
        add(mce::UUID(0x534B554C4C000000ULL, i));
        add(mce::UUID(0xF0B34E5043000000ULL, i));
    }
    if (entries.empty()) {
        return;
    }

    for (auto const& [key, uuid] : entries) {
        PlayerListPacket remove;
        remove.setSerializationMode(SerializationMode::CerealOnly);
        PlayerListPacketPayload::RemoveEntry entry;
        entry.mAction = PlayerListPacketType::Remove;
        entry.mUUID   = uuid;
        remove.mEntries.get().emplace_back(std::move(entry));
        remove.sendToClients();
    }
}

void SkullEntityManager::tick() {
    auto level = ll::service::getLevel();
    if (!level || !mTickAlive || !mTickAlive->load()) {
        return;
    }
    if (CustomSkull::getInstance().config().useServerAnchor) {
        auto& hologram = HologramBridge::getInstance();
        if (hologram.available()) {
            for (auto const& [key, skull] : mPlaced) {
                if (skull.anchorUniqueId == 0) {
                    continue;
                }
                auto* record = mStore.findByUuid(skull.ownerUuid);
                if (!record) {
                    continue;
                }
                auto image = mStore.image(*record);
                if (!image) {
                    continue;
                }
                std::string skinId = std::format(
                    "customskull_anchor_{}_{}_{}_{}",
                    record->uuid,
                    skull.x,
                    skull.y,
                    skull.z
                );
                std::string file = record->file.empty() ? record->uuid + ".png" : record->file;
                std::string png  = std::filesystem::absolute(mStore.imageDir() / file).string();
                if (hologram.registerSkin(png, skinId, std::string(skullEntityGeometry()))) {
                    hologram.overrideSkin(skull.anchorUniqueId, skinId, 0.0f);
                }
            }
        }
    } else {
        level->forEachPlayer([&](Player& player) {
            for (auto const& [key, skull] : mPlaced) {
                if (skull.dimension != static_cast<int>(player.getDimensionId())) {
                    continue;
                }
                Vec3 position{
                    static_cast<float>(skull.x) + 0.5f + skull.xOffset,
                    static_cast<float>(skull.y),
                    static_cast<float>(skull.z) + 0.5f + skull.zOffset
                };
                if ((player.getPosition() - position).length() > 96.0f) {
                    continue;
                }
                auto* record = mStore.findByUuid(skull.ownerUuid);
                if (!record) {
                    continue;
                }
                auto image = mStore.image(*record);
                if (!image) {
                    continue;
                }
                spawn(skull, *record, *image, player, &player, true);
            }
            return true;
        });
    }
    if (mTickAlive && mTickAlive->load()) {
        ll::thread::ServerThreadExecutor::getDefault().executeAfter(
            [this, alive = mTickAlive]() {
                if (!alive->load()) {
                    return;
                }
                tick();
            },
            std::chrono::seconds(1)
        );
    }
}

void SkullEntityManager::noteGiven(Player& player, SkinRecord const& record) {
    mGiveQueue.emplace_back(record.uuid, uuidToString(player.getUuid()));
    if (mGiveQueue.size() > 128) {
        mGiveQueue.pop_front();
    }
    CustomSkull::getInstance().logger().info("[skull] queued given skull owner={}", record.uuid);
}

void SkullEntityManager::loadPersistence() {
    std::ifstream input(mPersistencePath);
    if (!input) {
        return;
    }
    auto document = nlohmann::json::parse(input, nullptr, false);
    if (!document.is_object() || !document.contains("skulls") || !document["skulls"].is_array()) {
        return;
    }
    bool const needsYawFix = document.value("version", 1) < 3;
    for (auto const& item : document["skulls"]) {
        PlacedRecord record;
        record.x         = item.value("x", 0);
        record.y         = item.value("y", 0);
        record.z         = item.value("z", 0);
        record.dimension = item.value("dimension", 0);
        record.ownerUuid = item.value("owner", std::string{});
        record.yaw       = item.value("yaw", 0.0f);
        record.xOffset   = item.value("xOffset", 0.0f);
        record.yOffset   = item.value("yOffset", 0.0f);
        record.zOffset   = item.value("zOffset", 0.0f);
        record.anchorUniqueId = item.value("anchor", static_cast<int64_t>(0));
        if (needsYawFix) {
            record.yaw = flipSkullYaw(record.yaw);
        }
        if (!record.ownerUuid.empty()) {
            mPersisted.push_back(std::move(record));
        }
    }
    if (needsYawFix && !mPersisted.empty()) {
        savePersistence();
    }
}

void SkullEntityManager::savePersistence() const {
    nlohmann::json document;
    document["version"] = 3;
    auto skulls = nlohmann::json::array();
    for (auto const& record : mPersisted) {
        nlohmann::json item;
        item["x"]         = record.x;
        item["y"]         = record.y;
        item["z"]         = record.z;
        item["dimension"] = record.dimension;
        item["owner"]     = record.ownerUuid;
        item["yaw"]       = record.yaw;
        item["xOffset"]   = record.xOffset;
        item["yOffset"]   = record.yOffset;
        item["zOffset"]   = record.zOffset;
        item["anchor"]    = record.anchorUniqueId;
        skulls.push_back(std::move(item));
    }
    document["skulls"] = std::move(skulls);
    std::ofstream output(mPersistencePath);
    if (output) {
        output << document.dump(2);
    }
}

void SkullEntityManager::remember(
    BlockPos const&   pos,
    std::string const& ownerUuid,
    int               dimension,
    float             yaw,
    float             xOffset,
    float             yOffset,
    float             zOffset,
    int64_t           anchorUniqueId
) {
    for (auto& record : mPersisted) {
        if (record.x == pos.x && record.y == pos.y && record.z == pos.z) {
            record.dimension = dimension;
            record.ownerUuid = ownerUuid;
            record.yaw       = yaw;
            record.xOffset   = xOffset;
            record.yOffset   = yOffset;
            record.zOffset   = zOffset;
            record.anchorUniqueId = anchorUniqueId;
            savePersistence();
            return;
        }
    }
    mPersisted.push_back(PlacedRecord{pos.x, pos.y, pos.z, dimension, ownerUuid, yaw, xOffset, yOffset, zOffset, anchorUniqueId});
    savePersistence();
}

void SkullEntityManager::forget(BlockPos const& pos) {
    for (auto it = mPersisted.begin(); it != mPersisted.end(); ++it) {
        if (it->x == pos.x && it->y == pos.y && it->z == pos.z) {
            mPersisted.erase(it);
            savePersistence();
            return;
        }
    }
}

void SkullEntityManager::restoreAll(Player& player) {
    if (mRestored) {
        return;
    }
    auto& hologram = HologramBridge::getInstance();
    if (CustomSkull::getInstance().config().useHologramLib) {
        if (!hologram.available()) {
            hologram.initialize();
        }
        if (!hologram.available()) {
            CustomSkull::getInstance().logger().warn(
                "[skull] cannot restore placed skulls: {}",
                hologram.lastError()
            );
            return;
        }
    }
    mRestored = true;
    for (auto const& placed : mPersisted) {
        BlockPos pos{placed.x, placed.y, placed.z};
        if (mPlaced.contains(keyOf(pos))) {
            continue;
        }
        auto* record = mStore.findByUuid(placed.ownerUuid);
        if (!record) {
            continue;
        }
        auto image = mStore.image(*record);
        if (!image) {
            continue;
        }
        CustomSkull::getInstance().logger().info(
            "[skull] restoring placed skull owner={} pos=({},{},{})",
            placed.ownerUuid,
            placed.x,
            placed.y,
            placed.z
        );
        spawnAt(pos, *record, *image, &player, placed.dimension, placed.yaw, placed.xOffset, placed.yOffset, placed.zOffset, false, false);
    }

    ll::thread::ServerThreadExecutor::getDefault().executeAfter(
        [this]() {
            auto& hologram = HologramBridge::getInstance();
            if (!hologram.available()) {
                return;
            }
            for (auto const& [key, skull] : mPlaced) {
                if (skull.hologramId < 0) {
                    continue;
                }
                hologram.setPositionLight(
                    skull.hologramId,
                    static_cast<float>(skull.x) + 0.5f + skull.xOffset,
                    static_cast<float>(skull.y) - 0.01f + skull.yOffset,
                    static_cast<float>(skull.z) + 0.5f + skull.zOffset,
                    skull.dimension
                );
                hologram.setRotationLight(skull.hologramId, skull.yaw);
                CustomSkull::getInstance().logger().info(
                    "[skull] refreshed restored skull id={} pos=({},{},{})",
                    skull.hologramId,
                    skull.x,
                    skull.y,
                    skull.z
                );
            }
        },
        std::chrono::milliseconds(2000)
    );
}

void SkullEntityManager::spawnAt(
    BlockPos const&   pos,
    SkinRecord const& record,
    SkinImage const&  image,
    Player*           source,
    int               dimension,
    float             yaw,
    float             visualXOffset,
    float             visualYOffset,
    float             visualZOffset,
    bool              reuseAny,
    bool              persist
) {
    FakeSkull skull;
    skull.x          = pos.x;
    skull.y          = pos.y;
    skull.z          = pos.z;
    skull.ownerUuid  = record.uuid;
    static std::atomic<uint64_t> entityCounter{1};
    auto const                   entityToken = entityCounter.fetch_add(1);
    skull.entityUuid = uuidToString(mce::UUID(0x534B554C4C000000ULL, entityToken));
    skull.uniqueId   = uniqueIdFor(skull.entityUuid);
    skull.runtimeId  = static_cast<uint64_t>(skull.uniqueId);
    skull.yaw        = yaw;
    skull.xOffset    = visualXOffset;
    skull.yOffset    = visualYOffset;
    skull.zOffset    = visualZOffset;
    skull.dimension  = dimension;

    auto& hologram = HologramBridge::getInstance();
    if (!hologram.available()) {
        hologram.initialize();
    }
    if (!hologram.available()) {
        CustomSkull::getInstance().logger().warn("[skull] Hologram unavailable: {}", hologram.lastError());
    }
    if (reuseAny && hologram.available()) {
        for (auto it = mPlaced.begin(); it != mPlaced.end(); ++it) {
            if (it->second.ownerUuid != record.uuid || it->second.hologramId < 0) {
                continue;
            }
            CustomSkull::getInstance().logger().info(
                "[skull] hologram reused unchanged id={} (no respawn)",
                it->second.hologramId
            );
            return;
        }
    }
    auto old = mPlaced.find(keyOf(pos));
    if (CustomSkull::getInstance().config().useServerAnchor && hologram.available() && source) {
        static std::atomic<uint64_t> anchorCounter{1};
        auto const                   token  = anchorCounter.fetch_add(1);
        std::string                  skinId = "customskull_" + record.uuid + "_anchor_" + std::to_string(token);
        std::string                  file   = record.file.empty() ? record.uuid + ".png" : record.file;
        std::string                  png    = std::filesystem::absolute(mStore.imageDir() / file).string();
        bool skinOk = hologram.registerSkin(png, skinId, std::string(skullEntityGeometry()));
        CustomSkull::getInstance().logger().info("[skull] anchor skin registered={} skin={}", skinOk, skinId);
        if (skinOk) {
            if (auto* anchor = spawnServerAnchor(
                    *source,
                    pos,
                    dimension,
                    0.0f
                )) {
                auto uniqueId = static_cast<int64_t>(anchor->getOrCreateUniqueID().rawID);
                bool overridden = hologram.overrideSkin(uniqueId, skinId, 0.0f);
                CustomSkull::getInstance().logger().info(
                    "[skull] anchor uniqueId={} override={}",
                    uniqueId,
                    overridden
                );
                if (overridden) {
                    if (old != mPlaced.end()) {
                        remove(old->second, nullptr);
                        mPlaced.erase(old);
                    }
                    skull.anchorUniqueId = uniqueId;
                    mPlaced.emplace(keyOf(pos), skull);
                    if (persist) {
                        remember(pos, record.uuid, dimension, yaw, 0.0f, 0.0f, 0.0f, uniqueId);
                    }
                    CustomSkull::getInstance().logger().info(
                        "[skull] server anchor created uniqueId={} skin={}",
                        uniqueId,
                        skinId
                    );
                    return;
                }
                anchor->remove();
            }
        }
        CustomSkull::getInstance().logger().warn("[skull] server anchor failed, falling back");
        if (CustomSkull::getInstance().config().useServerAnchor) {
            return;
        }
    }

    if (CustomSkull::getInstance().config().useHologramLib && hologram.available()) {
        static std::atomic<uint64_t> skinCounter{1};
        auto const                   token   = skinCounter.fetch_add(1);
        std::string                  skinId  = "customskull_" + record.uuid + "_" + std::to_string(token);
        std::string                  file    = record.file.empty() ? record.uuid + ".png" : record.file;
        std::string                  png     = std::filesystem::absolute(mStore.imageDir() / file).string();
        bool                         skinOk  = hologram.registerSkin(png, skinId, std::string(skullEntityGeometry()));
        auto&                        logger  = CustomSkull::getInstance().logger();
        logger.info(
            "[skull] hologram spawn pos=({},{},{}) skin={} registered={}",
            pos.x,
            pos.y,
            pos.z,
            skinId,
            skinOk
        );
        if (old != mPlaced.end() && old->second.hologramId >= 0) {
            if (skinOk && old->second.ownerUuid != record.uuid) {
                hologram.setSkin(old->second.hologramId, skinId);
            }
            hologram.setRotation(old->second.hologramId, yaw);
            old->second.ownerUuid = record.uuid;
            old->second.yaw       = yaw;
            old->second.dimension = dimension;
            if (persist) {
                remember(pos, record.uuid, dimension, yaw, skull.xOffset, skull.yOffset, skull.zOffset);
            }
            logger.info("[skull] hologram skin updated id={} yaw={} skinOk={}", old->second.hologramId, yaw, skinOk);
            return;
        }
        if (old != mPlaced.end()) {
            remove(old->second, nullptr);
            mPlaced.erase(old);
        }
        int64_t id = hologram.create(
            skinId,
            "\xC2\xA7r",
            static_cast<float>(pos.x) + 0.5f + visualXOffset,
            static_cast<float>(pos.y) - 0.01f + visualYOffset,
            static_cast<float>(pos.z) + 0.5f + visualZOffset,
            dimension,
            yaw,
            1.08f
        );
        if (id >= 0) {
            skull.hologramId = id;
            mPlaced.emplace(keyOf(pos), skull);
            if (persist) {
                remember(pos, record.uuid, dimension, yaw, skull.xOffset, skull.yOffset, skull.zOffset);
            }
            logger.info("[skull] hologram created id={} yaw={}", id, yaw);
            auto tabUuid = mce::UUID(0xF0B34E5043000000ULL, static_cast<uint64>(id));
            ll::thread::ServerThreadExecutor::getDefault().executeAfter(
                [this, tabUuid]() { removeTabEntry(tabUuid); },
                std::chrono::milliseconds(1000)
            );
            ll::thread::ServerThreadExecutor::getDefault().executeAfter(
                [this, id, pos, dimension, visualXOffset, visualYOffset, visualZOffset]() {
                    auto& hologram = HologramBridge::getInstance();
                    float finalYaw = 0.0f;
                    if (auto placedIt = mPlaced.find(keyOf(pos)); placedIt != mPlaced.end()) {
                        finalYaw = placedIt->second.yaw;
                    }
                    hologram.setPositionLight(
                        id,
                        static_cast<float>(pos.x) + 0.5f + visualXOffset,
                        static_cast<float>(pos.y) - 0.01f + visualYOffset,
                        static_cast<float>(pos.z) + 0.5f + visualZOffset,
                        dimension
                    );
                    hologram.setRotationLight(id, finalYaw);
                    CustomSkull::getInstance().logger().info(
                        "[skull] delayed light id={} pos=({},{},{}) yaw={}",
                        id,
                        pos.x,
                        pos.y,
                        pos.z,
                        finalYaw
                    );
                },
                std::chrono::milliseconds(1000)
            );
            if (source) {
                auto* player = source;
                ll::thread::ServerThreadExecutor::getDefault().executeAfter(
                    [this, player, pos, id, dimension, visualXOffset, visualYOffset, visualZOffset, persist, ownerUuid = record.uuid]() {
                        if (!player) {
                            return;
                        }
                        auto& hologram = HologramBridge::getInstance();
                        float finalX = visualXOffset;
                        float finalY = visualYOffset;
                        float finalZ = visualZOffset;
                        float finalYaw = 180.0f;
                        auto& region = player->getDimensionBlockSource();
                        if (auto wallInfo = wallSkullInfo(region, pos)) {
                            finalYaw = wallInfo->yaw;
                            finalX   = wallInfo->xOffset;
                            finalY   = wallInfo->yOffset;
                            finalZ   = wallInfo->zOffset;
                        } else if (auto* actor = region.getBlockEntity(pos);
                                   actor && actor->getType() == BlockActorType::Skull) {
                            finalYaw = flipSkullYaw(static_cast<SkullBlockActor*>(actor)->mRotation);
                        }
                        hologram.setPositionLight(
                            id,
                            static_cast<float>(pos.x) + 0.5f + finalX,
                            static_cast<float>(pos.y) - 0.01f + finalY,
                            static_cast<float>(pos.z) + 0.5f + finalZ,
                            dimension
                        );
                        hologram.setRotationLight(id, finalYaw);
                        if (auto placedIt = mPlaced.find(keyOf(pos)); placedIt != mPlaced.end()) {
                            placedIt->second.xOffset = finalX;
                            placedIt->second.yOffset = finalY;
                            placedIt->second.zOffset = finalZ;
                            placedIt->second.yaw     = finalYaw;
                            if (persist) {
                                remember(pos, ownerUuid, dimension, finalYaw, finalX, finalY, finalZ);
                            }
                        }
                        CustomSkull::getInstance().logger().info(
                            "[skull] delayed rotation yaw={} offset=({},{},{})",
                            finalYaw,
                            finalX,
                            finalY,
                            finalZ
                        );
                    },
                    std::chrono::milliseconds(100)
                );
            }
            return;
        }
        logger.warn("[skull] HologramLib create failed, falling back to built-in entity");
    }

    if (old != mPlaced.end()) {
        remove(old->second, nullptr);
        mPlaced.erase(old);
    }
    if (!source) {
        CustomSkull::getInstance().logger().error("[skull] no player source for built-in fallback");
        return;
    }
    mPlaced.emplace(keyOf(pos), skull);
    if (persist) {
        remember(pos, record.uuid, dimension, yaw, skull.xOffset, skull.yOffset, skull.zOffset);
    }
    spawn(skull, record, image, *source, nullptr);
}

void SkullEntityManager::onJoin(Player& player) {
    {
        PlayerListPacket remove;
        remove.setSerializationMode(SerializationMode::CerealOnly);
        for (auto const& record : mStore.all()) {
            PlayerListPacketPayload::RemoveEntry entry;
            entry.mAction = PlayerListPacketType::Remove;
            entry.mUUID   = parseUuid(entityUuidFor(record.uuid));
            remove.mEntries.get().emplace_back(std::move(entry));
        }
        if (!remove.mEntries.get().empty()) {
            remove.sendTo(player);
        }
    }
    for (auto const& [key, skull] : mPlaced) {
        if (skull.hologramId >= 0) {
            continue;
        }
        auto* record = mStore.findByUuid(skull.ownerUuid);
        if (!record) {
            continue;
        }
        if (auto image = mStore.image(*record)) {
            sendSkullTo(skull, *record, *image, &player);
        }
    }
}

void SkullEntityManager::spawn(
    FakeSkull const&  skull,
    SkinRecord const& record,
    SkinImage const&  image,
    Player&           source,
    Player const*     target,
    bool              refreshOnly
) {
    if (!refreshOnly) {
        sendSkullTo(skull, record, image, target);
    }

    AddPlayerPacket packet(source);
    packet.mUuid             = parseUuid(skull.entityUuid);
    packet.mName             = fakeName(skull.entityUuid);
    packet.mRuntimeId        = ActorRuntimeID(skull.runtimeId);
    packet.mPlatformOnlineId = "";
    packet.mPos              = Vec3(static_cast<float>(skull.x) + 0.5f + skull.xOffset, static_cast<float>(skull.y) - 0.01f + skull.yOffset, static_cast<float>(skull.z) + 0.5f + skull.zOffset);
    packet.mVelocity         = Vec3(0.0f, 0.0f, 0.0f);
    packet.mRot              = Vec2(0.0f, skull.yaw);
    packet.mYHeadRot         = skull.yaw;
    packet.mPlayerGameType   = GameType::Survival;
    packet.mBuildPlatform    = BuildPlatform::Google;
    packet.mDeviceId         = "";

    auto& carried = packet.mCarriedItem.get();
    carried.mImpl           = nullptr;
    carried.mStackSize      = 0;
    carried.mIncludeNetIds  = false;
    carried.mBlockRuntimeId = 0;
    carried.mUserDataBuffer.get().clear();

    auto& unpack = packet.mUnpack.get();
    unpack.mData.get().clear();
    unpack.mData.get().push_back(DataItem::create(static_cast<ushort>(0), static_cast<int64>(0)));
    unpack.mData.get().push_back(DataItem::create(static_cast<ushort>(38), 1.08f));
    unpack.mData.get().push_back(DataItem::create(static_cast<ushort>(53), 0.001f));
    unpack.mData.get().push_back(DataItem::create(static_cast<ushort>(54), 0.001f));

    auto& abilities               = packet.mAbilitiesData.get();
    abilities.mTargetPlayer.get() = ActorUniqueID{skull.uniqueId};
    abilities.mCommandPermissions = CommandPermissionLevel::Any;
    abilities.mPlayerPermissions  = PlayerPermissionLevel::Member;
    abilities.mLayers.get().clear();
    SerializedAbilitiesData::SerializedLayer layer;
    layer.mSerializedLayer  = SerializedAbilitiesData::SerializedAbilitiesLayer::CustomCache;
    layer.mAbilitiesSet     = 0;
    layer.mAbilityValues    = 0;
    layer.mFlySpeed         = 0.05f;
    layer.mVerticalFlySpeed = 0.1f;
    layer.mWalkSpeed        = 0.1f;
    abilities.mLayers.get().push_back(layer);

    if (target) {
        packet.sendTo(*target);
    } else {
        packet.sendToClients();
    }

    if (!refreshOnly) {
        SerializedSkinRef entitySkin;
        if (buildSkullEntitySkin(entitySkin, image, skull.entityUuid + "_skull")) {
            PlayerSkinPacket skinPacket;
            skinPacket.mSerializationMode         = SerializationMode::CerealOnly;
            skinPacket.mUUID                      = parseUuid(skull.entityUuid);
            skinPacket.mSkin                      = entitySkin;
            skinPacket.mLocalizedNewSkinName      = "";
            skinPacket.mLocalizedOldSkinName      = "";
            if (target) {
                skinPacket.sendTo(*target);
            } else {
                skinPacket.sendToClients();
            }
        }
        auto tabUuid = parseUuid(skull.entityUuid);
        ll::thread::ServerThreadExecutor::getDefault().executeAfter(
            [this, tabUuid]() { removeTabEntry(tabUuid); },
            std::chrono::milliseconds(1000)
        );
    }
}

void SkullEntityManager::remove(FakeSkull const& skull, Player const* target) {
    if (skull.anchorUniqueId != 0) {
        HologramBridge::getInstance().clearOverride(skull.anchorUniqueId);
        if (auto level = ll::service::getLevel()) {
            if (auto* actor = level->fetchEntity(ActorUniqueID{skull.anchorUniqueId}, false)) {
                actor->remove();
            }
        }
        return;
    }
    if (skull.hologramId >= 0) {
        HologramBridge::getInstance().destroy(skull.hologramId);
        if (skull.anchorId >= 0) {
            HologramBridge::getInstance().destroyAnchor(skull.anchorId);
        }
        return;
    }
    RemoveActorPacket packet;
    packet.mSerializationMode = SerializationMode::CerealOnly;
    packet.mEntityId          = ActorUniqueID{skull.uniqueId};
    if (target) {
        packet.sendTo(*target);
    } else {
        packet.sendToClients();
    }
}

void SkullEntityManager::removeTabEntry(mce::UUID const& uuid) {
    PlayerListPacket remove;
    remove.setSerializationMode(SerializationMode::CerealOnly);
    PlayerListPacketPayload::RemoveEntry entry;
    entry.mAction = PlayerListPacketType::Remove;
    entry.mUUID   = uuid;
    remove.mEntries.get().emplace_back(std::move(entry));
    remove.sendToClients();
}

void SkullEntityManager::sendSkullTo(
    FakeSkull const&  skull,
    SkinRecord const& record,
    SkinImage const&  image,
    Player const*     target
) {
    SerializedSkinRef skin;
    if (!buildSkullEntitySkin(skin, image, skull.entityUuid + "_skull")) {
        return;
    }

    PlayerListPacket list;
    list.setSerializationMode(SerializationMode::CerealOnly);
    PlayerListPacketPayload::AddEntry entry;
    entry.mUUID             = parseUuid(skull.entityUuid);
    entry.mId.get().rawID   = static_cast<int64>(skull.uniqueId);
    entry.mName             = fakeName(skull.entityUuid);
    entry.mXUID             = "0";
#if !defined(CS_LL_MINOR) || CS_LL_MINOR >= 60
    entry.mPlayFabId        = "";
#endif
    entry.mPlatformOnlineId = "";
    entry.mBuildPlatform    = BuildPlatform::Google;
    entry.mSkin             = skin;
    entry.mIsTeacher        = false;
    entry.mIsHost           = false;
    entry.mIsSubClient      = false;
    entry.mColor            = mce::Color(170, 170, 170, 255);
    list.mEntries.get().emplace_back(std::move(entry));

    if (target) {
        list.sendTo(*target);
    } else {
        list.sendToClients();
    }
}

} // namespace custom_skull














































