#include "mod/CustomSkull.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <thread>

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ServerThreadExecutor.h"

#include "mc/server/commands/CommandFlag.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/SaveContext.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/block/actor/SkullBlockActor.h"

#include "skull/JavaSkin.h"
#include "skull/PlayerSkin.h"
#include "skull/Png.h"
#include "skull/SkullEntity.h"
#include "skull/SkullItem.h"

namespace custom_skull {

struct GetParams {
    std::string name;
    int         count = 1;
};

struct JavaParams {
    std::string name;
    int         count = 1;
};

struct CreateParams {
    std::string name;
    int         count = 1;
};

struct EntityParams {
    std::string name;
};

struct BlockInfoParams {};

namespace {

Player* originPlayer(CommandOrigin const& origin) {
    auto* entity = origin.getEntity();
    if (entity && entity->isPlayer()) {
        return static_cast<Player*>(entity);
    }
    return nullptr;
}

int clampCount(int count) {
    return std::clamp(count, 1, 64);
}

bool safeFileName(std::string const& name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    if (name == "." || name == "..") {
        return false;
    }
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            return false;
        }
    }
    return true;
}

void giveTo(CommandOutput& output, CustomSkull& mod, Player& player, SkinRecord const& record, int count) {
    auto display = formatSkullName(mod.config().skullNameFormat, record.name);
    if (!giveSkull(player, record, count, display)) {
        output.error("Could not add the skull to your inventory (inventory full?)");
    } else {
        mod.entities().noteGiven(player, record);
    }
}

} // namespace

void registerSkullCommand(CustomSkull& mod) {
    using namespace ll::command;

    auto& command = CommandRegistrar::getServerInstance().getOrCreateCommand(
        "skull",
        "Create and give custom player skulls",
        CommandPermissionLevel::GameDirectors,
        CommandFlagValue::NotCheat
    );
    command.alias("customskull");

    command.overload<GetParams>()
        .text("get")
        .required("name")
        .optional("count")
        .execute([](CommandOrigin const& origin, CommandOutput& output, GetParams const& params) {
            auto& mod   = CustomSkull::getInstance();
            auto  level = ll::service::getLevel();
            if (!level) {
                output.error("The server level is not ready");
                return;
            }
            Player* target = level->getPlayer(params.name);
            if (!target) {
                output.error("Online Bedrock player not found: {}", params.name);
                return;
            }
            auto image = imageFromPlayer(*target);
            if (!image) {
                output.error("Could not read that player's skin (persona skins are not supported)");
                return;
            }
            std::string error;
            auto        record = mod.store().upsert(params.name, "bedrock", *image, error);
            if (record.uuid.empty()) {
                output.error("Could not save the skin: {}", error);
                return;
            }
            mod.broadcastSkin(record, *image);

            Player* requester = originPlayer(origin);
            if (!requester) {
                output.success("Created skull: {}", record.name);
                return;
            }
            giveTo(output, mod, *requester, record, clampCount(params.count));
            output.success("Received the skull of {}", record.name);
        });

    command.overload<JavaParams>()
        .text("java")
        .required("name")
        .optional("count")
        .execute([](CommandOrigin const& origin, CommandOutput& output, JavaParams const& params) {
            auto& mod = CustomSkull::getInstance();
            if (!mod.config().enableJavaFetch) {
                output.error("Java skin fetching is disabled in the config");
                return;
            }
            Player* requester = originPlayer(origin);
            if (!requester) {
                output.error("This command must be run by a player");
                return;
            }
            mce::UUID   requesterUuid = requester->getUuid();
            std::string name          = params.name;
            int         count         = clampCount(params.count);
            output.success("Fetching the Java Edition skin of {}...", name);

            std::thread([&mod, name, count, requesterUuid]() {
                std::string error;
                auto        result = fetchJavaSkin(name, error);
                ll::thread::ServerThreadExecutor::getDefault().execute(
                    [&mod, name, count, requesterUuid, result = std::move(result), error]() mutable {
                        auto level = ll::service::getLevel();
                        if (!result) {
                            if (level) {
                                if (auto* player = level->getPlayer(requesterUuid)) {
                                    player->sendMessage("CustomSkull: " + error);
                                }
                            }
                            mod.getSelf().getLogger().warn("Java skin fetch failed for {}: {}", name, error);
                            return;
                        }
                        std::string saveError;
                        auto        record = mod.store().upsert(result->name, "java", result->image, saveError);
                        if (record.uuid.empty()) {
                            mod.getSelf().getLogger().error("Could not save Java skin: {}", saveError);
                            return;
                        }
                        mod.broadcastSkin(record, result->image);
                        if (level) {
                            if (auto* player = level->getPlayer(requesterUuid)) {
                                auto display = formatSkullName(mod.config().skullNameFormat, record.name);
                                if (!giveSkull(*player, record, count, display)) {
                                    player->sendMessage("CustomSkull: inventory full");
                                } else {
                                    mod.entities().noteGiven(*player, record);
                                    player->sendMessage("CustomSkull: received the skull of " + record.name);
                                }
                            }
                        }
                    }
                );
            }).detach();
        });

    command.overload<CreateParams>()
        .text("create")
        .required("name")
        .optional("count")
        .execute([](CommandOrigin const& origin, CommandOutput& output, CreateParams const& params) {
            auto& mod = CustomSkull::getInstance();
            if (!safeFileName(params.name)) {
                output.error("The name contains invalid characters");
                return;
            }
            auto            path = mod.store().imageDir() / (params.name + ".png");
            std::error_code code;
            if (!std::filesystem::exists(path, code)) {
                output.error("Skin file not found: {}", path.string());
                return;
            }
            std::ifstream stream(path, std::ios::binary);
            std::string   bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            auto          image = decodePng(bytes);
            if (!image) {
                output.error("Skin file is not a valid PNG: {}", path.string());
                return;
            }
            std::string saveError;
            auto        record = mod.store().upsert(params.name, "file", *image, saveError);
            if (record.uuid.empty()) {
                output.error("Could not save the skin: {}", saveError);
                return;
            }
            mod.broadcastSkin(record, *image);

            Player* requester = originPlayer(origin);
            if (!requester) {
                output.success("Created skull: {}", record.name);
                return;
            }
            giveTo(output, mod, *requester, record, clampCount(params.count));
            output.success("Received the skull of {}", record.name);
        });

    command.overload<BlockInfoParams>()
        .text("blockinfo")
        .execute([](CommandOrigin const& origin, CommandOutput& output, BlockInfoParams const&) {
            auto&   mod    = CustomSkull::getInstance();
            Player* player = originPlayer(origin);
            if (!player) {
                output.error("This command must be run by a player");
                return;
            }
            auto& region = player->getDimensionBlockSource();
            auto  center = player->getFeetBlockPos();
            int   found  = 0;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    for (int dz = -2; dz <= 2; ++dz) {
                        BlockPos pos{center.x + dx, center.y + dy, center.z + dz};
                        auto&    block = region.getBlock(pos);
                        if (block.getTypeName() != "minecraft:skull" && block.getTypeName() != "minecraft:player_head") {
                            continue;
                        }
                        ++found;
                        auto* actor = region.getBlockEntity(pos);
                        mod.logger().info(
                            "[skull] blockinfo pos={} data={} actor={}",
                            pos.toString(),
                            block.getData(),
                            actor ? static_cast<int>(actor->getType()) : -1
                        );
                        if (!actor) {
                            continue;
                        }
                        if (actor->getType() == BlockActorType::Skull) {
                            auto* skullActor = static_cast<SkullBlockActor*>(actor);
                            mod.logger().info("[skull] blockinfo rotation={}", skullActor->mRotation);
                        }
                        CompoundTag tag;
                        SaveContext context{SaveContext::SaveUseCase::BlockActorToItemUserData};
                        actor->save(tag, context);
                        mod.logger().info("[skull] blockinfo nbt={}", tag.toString());
                    }
                }
            }
            output.success("Logged {} skull block(s) near you", found);
        });

    command.overload<EntityParams>()
        .text("entity")
        .required("name")
        .execute([](CommandOrigin const& origin, CommandOutput& output, EntityParams const& params) {
            auto&     mod = CustomSkull::getInstance();
            Player*   player = originPlayer(origin);
            if (!player) {
                output.error("This command must be run by a player");
                return;
            }
            SkinRecord const* record = nullptr;
            for (auto const& candidate : mod.store().all()) {
                if (candidate.name == params.name) {
                    record = &candidate;
                    break;
                }
            }
            if (!record) {
                output.error("No saved skin named {}", params.name);
                return;
            }
            auto image = mod.store().image(*record);
            if (!image) {
                output.error("Skin file is missing for {}", record->name);
                return;
            }
            mod.entities().spawnAt(
                player->getFeetBlockPos(),
                *record,
                *image,
                player,
                static_cast<int>(player->getDimensionId()),
                player->getRotation().y,
                true,
                false
            );
            output.success("Spawned a skull entity for {}", record->name);
        });

    command.overload()
        .text("reload")
        .execute([](CommandOrigin const&, CommandOutput& output) {
            auto&       mod = CustomSkull::getInstance();
            std::string error;
            if (!mod.store().load(error)) {
                output.error("Reload failed: {}", error);
                return;
            }
            mod.broadcastAllSkins();
            output.success("Reloaded {} saved skull skins", mod.store().all().size());
        });

    command.overload()
        .text("list")
        .execute([](CommandOrigin const&, CommandOutput& output) {
            auto& mod = CustomSkull::getInstance();
            if (mod.store().all().empty()) {
                output.success("No saved skull skins");
                return;
            }
            std::string text = "Saved skull skins:";
            for (auto const& record : mod.store().all()) {
                text += " " + record.name + "[" + record.source + "]";
            }
            output.success("{}", text);
        });
}

} // namespace custom_skull







