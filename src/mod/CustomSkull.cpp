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
#include "ll/api/thread/ServerThreadExecutor.h"

#include "hologram/HologramBridge.h"
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
    mConfig.useServerAnchor = false;
    mConfig.useHologramLib = true;
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
    }
    if (auto& bridge = HologramBridge::getInstance(); bridge.initialize()) {
        logger.info("HologramLib player-NPC bridge enabled");
    } else {
        logger.warn("HologramLib player-NPC bridge unavailable, using built-in fallback: {}", bridge.lastError());
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
    std::vector<SkinPayload> payloads;
    payloads.push_back({record, image});
    sendSkinPayloads(payloads, nullptr, mConfig.maxEntriesPerPacket, mConfig.keepSkinListed);
}

void CustomSkull::broadcastAllSkins() {
    std::vector<SkinPayload> payloads;
    payloads.reserve(mStore->all().size());
    for (auto const& record : mStore->all()) {
        if (auto image = mStore->image(record)) {
            payloads.push_back({record, std::move(*image)});
        }
    }
    sendSkinPayloads(payloads, nullptr, mConfig.maxEntriesPerPacket, mConfig.keepSkinListed);
}

void CustomSkull::sendAllSkins(Player& player) {
    std::vector<SkinPayload> payloads;
    payloads.reserve(mStore->all().size());
    for (auto const& record : mStore->all()) {
        if (auto image = mStore->image(record)) {
            payloads.push_back({record, std::move(*image)});
        }
    }
    sendSkinPayloads(payloads, &player, mConfig.maxEntriesPerPacket, mConfig.keepSkinListed);
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

LL_REGISTER_MOD(custom_skull::CustomSkull, custom_skull::CustomSkull::getInstance())

