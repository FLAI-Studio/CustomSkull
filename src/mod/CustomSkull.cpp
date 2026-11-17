#include "mod/CustomSkull.h"

#include <chrono>
#include <filesystem>
#include <vector>

#include "ll/api/Config.h"
#include "ll/api/Versions.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/io/LogLevel.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ServerThreadExecutor.h"

#include "skull/SkinBroadcast.h"
#include "skull/SkullEntity.h"

namespace custom_skull {

CustomSkull& CustomSkull::getInstance() {
    static CustomSkull instance;
    return instance;
}

CustomSkull::CustomSkull() : mSelf(*ll::mod::NativeMod::current()) {}

bool CustomSkull::load() {
    auto& logger = getSelf().getLogger();
    logger.debug("Loading CustomSkull...");

    std::error_code code;
    std::filesystem::create_directories(getSelf().getConfigDir(), code);
    if (code) {
        logger.error("Cannot create config directory {}", getSelf().getConfigDir().string());
        return false;
    }

    auto configPath = getSelf().getConfigDir() / "config.json";
    if (!ll::config::loadConfig(mConfig, configPath)) {
        logger.warn("Cannot load config from {}, writing defaults", configPath.string());
        if (!ll::config::saveConfig(mConfig, configPath)) {
            logger.error("Cannot save config to {}", configPath.string());
        }
    }
    mConfig.keepSkinListed = true;
    logger.setLevel(mConfig.debug ? ll::io::LogLevel::Debug : ll::io::LogLevel::Warn);

    mStore = std::make_unique<SkinStore>(getSelf().getModDir());
    std::string error;
    if (!mStore->load(error)) {
        logger.error("Cannot load skins: {}", error);
        return false;
    }
    mEntities = std::make_unique<SkullEntityManager>(*mStore);
    logger.info("Loaded {} saved skull skins", mStore->all().size());
    return true;
}

bool CustomSkull::enable() {
    auto& logger = getSelf().getLogger();

#if defined(CS_LL_MAJOR) && defined(CS_LL_MINOR)
    auto const loader = ll::getLoaderVersion();
    if (static_cast<int>(loader.major) != CS_LL_MAJOR || static_cast<int>(loader.minor) != CS_LL_MINOR) {
        logger.error(
            "CustomSkull was built for LeviLamina {}.{} but the server runs {}.{}.{}; refusing to load",
            CS_LL_MAJOR,
            CS_LL_MINOR,
            loader.major,
            loader.minor,
            loader.patch
        );
        return false;
    }
#endif

    registerSkullCommand(*this);

    mJoinListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::PlayerJoinEvent>(
        [](ll::event::PlayerJoinEvent& event) { CustomSkull::getInstance().onPlayerJoin(event.self()); }
    );
    if (!mJoinListener) {
        logger.error("Cannot register PlayerJoinEvent listener");
        return false;
    }
    if (mEntities) {
        mEntities->registerEvents();
        mEntities->startTabCleanup();
        mEntities->startTicking();
    }

    logger.info("CustomSkull enabled");
    return true;
}

bool CustomSkull::disable() {
    if (mEntities) {
        mEntities->unregisterEvents();
    }
    if (mJoinListener) {
        ll::event::EventBus::getInstance().removeListener<ll::event::PlayerJoinEvent>(mJoinListener);
        mJoinListener.reset();
    }
    return true;
}

bool CustomSkull::unload() {
    disable();
    mEntities.reset();
    mStore.reset();
    return true;
}

void CustomSkull::broadcastSkin(SkinRecord const& record, SkinImage const& image) {
    (void)record;
    (void)image;
    return;
}

void CustomSkull::broadcastAllSkins() {
    return;
}

void CustomSkull::sendAllSkins(Player& player) {
    (void)player;
    return;
}

void CustomSkull::onPlayerJoin(Player& player) {
    if (mStore && !mStore->all().empty()) {
        sendAllSkins(player);
    }
    if (mEntities) {
        mEntities->sendAll(player);
        ll::thread::ServerThreadExecutor::getDefault().executeAfter(
            [this, playerPtr = &player]() {
                if (mEntities && playerPtr) {
                    mEntities->restoreAll(*playerPtr);
                }
            },
            std::chrono::seconds(2)
        );
    }
}

} // namespace custom_skull

#include "skull/PlayerSkin.h"
#include "skull/SkullItem.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"

extern "C" __declspec(dllexport) bool customskull_give_skull_from_player(Player* victim, Player* receiver, int count) {
    if (!victim || !receiver || count <= 0) {
        return false;
    }
    auto image = custom_skull::imageFromPlayer(*victim);
    if (!image) {
        return false;
    }
    std::string error;
    auto&       mod    = custom_skull::CustomSkull::getInstance();
    auto        record = mod.store().upsert(victim->getRealName(), "bedrock", *image, error);
    if (!error.empty()) {
        return false;
    }
    auto display = custom_skull::formatSkullName(mod.config().skullNameFormat, record.name);
    return custom_skull::giveSkull(*receiver, record, count, display);
}

extern "C" __declspec(dllexport) bool customskull_drop_skull_from_player(Player* victim, int count) {
    if (!victim || count <= 0) {
        return false;
    }
    auto image = custom_skull::imageFromPlayer(*victim);
    if (!image) {
        return false;
    }
    std::string error;
    auto&       mod    = custom_skull::CustomSkull::getInstance();
    auto        record = mod.store().upsert(victim->getRealName(), "bedrock", *image, error);
    if (!error.empty()) {
        return false;
    }
    auto stack = custom_skull::createSkullItem(
        record,
        count,
        custom_skull::formatSkullName(mod.config().skullNameFormat, record.name)
    );
    auto level = ll::service::getLevel();
    if (!level) {
        return false;
    }
    auto& region = victim->getDimensionBlockSource();
    level->getSpawner().spawnItem(region, stack, victim, victim->getPosition(), 0);
    return true;
}

LL_REGISTER_MOD(custom_skull::CustomSkull, custom_skull::CustomSkull::getInstance())


