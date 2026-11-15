# CustomSkull

为 **LeviLamina** 的 Minecraft 基岩版专用服务器提供自定义玩家头颅。

可以把在线基岩版玩家的皮肤、Java 版账号皮肤或本地 PNG 制作成头颅物品，放置到世界里作为自定义玩家头。客户端不需要安装资源包。

> 当前版本核心功能可用，欢迎提交 PR 与 issue。

## 功能特性

- `/skull get <目标选择器> [数量]`：用在线基岩版玩家当前皮肤制作头颅，支持目标选择器。
- `/skull java <玩家> [数量]`：通过 Mojang API 下载 Java 版玩家皮肤制作头颅。
- `/skull create <名称> [数量]`：用 `skinData/<名称>.png` 制作头颅。
- `/skull list`：列出已保存的皮肤。
- `/skull reload`：重新加载 `skins.json`。
- `/skull entity <名称>`：在脚下生成一个测试头颅实体。
- `/skull blockinfo`：诊断命令，把附近头颅方块的坐标、旋转和 NBT 同时输出到聊天框和控制台。
- 玩家进服时自动恢复已保存的头颅。
- 已放置的头颅记录在 `placed_skulls.json`，服务器重启后自动恢复。
- 玩家重载区块自动重建显示实体。

## 前置要求

- LeviLamina

## 安装（服务端）

### 使用 LIP

```text
lip install github.com/FLAI-Studio/CustomSkull
```

### 手动安装

从 Releases 下载 `CustomSkull-server-windows-x64.zip`，解压到服务端 `mods/`：

```text
mods/
`-- CustomSkull/
    |-- CustomSkull.dll
    |-- manifest.json
    |-- LICENSE
    `-- config/config.json
```

## 命令

所有命令需要管理员权限（`GameDirectors`）。`数量` 会被限制在 1~64。

| 命令 | 说明 |
| --- | --- |
| `/skull get <目标选择器> [数量]` | 用在线基岩版玩家的皮肤制作头颅，支持 `@s`、`@p`、`@a`。 |
| `/skull java <玩家> [数量]` | 用 Java 版账号皮肤制作头颅。 |
| `/skull create <名称> [数量]` | 用 `skinData/<名称>.png` 制作头颅。 |
| `/skull list` | 列出已保存的皮肤。 |
| `/skull reload` | 重新加载皮肤索引。 |
| `/skull entity <名称>` | 在脚下生成测试头颅实体。 |
| `/skull blockinfo` | 输出附近头颅方块数据（聊天框 + 控制台）。 |

## 配置

`mods/CustomSkull/config/config.json`：

```json
{
    "version": 1,
    "enableJavaFetch": true,
    "debug": false,
    "skullNameFormat": "\u00a7e{}\u7684\u5934\u9885",
    "maxEntriesPerPacket": 8,
    "keepSkinListed": true,
    "useHologramLib": false,
    "useServerAnchor": false
}
```

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `enableJavaFetch` | true | 是否允许 `/skull java`。 |
| `debug` | false | 输出更多调试日志；关闭时只保留警告和错误。 |
| `skullNameFormat` | "\u00a7e{}\u7684\u5934\u9885" | 头颅物品显示名，`{}` 会替换成玩家名。 |
| `maxEntriesPerPacket` | 8 | 旧版皮肤广播的分包大小，当前显示路径不再使用。 |
| `keepSkinListed` | true | 旧版皮肤广播配置，当前显示路径不再使用。 |
| `useHologramLib` | false | 待移除：前版本强制使用内置显示方案。 |
| `useServerAnchor` | false | 实验性：armor_stand 锚点，强制关闭。 |

当前发布版在加载配置后会强制：

```cpp
useHologramLib  = false;
useServerAnchor = false;
```

## 数据文件

```text
mods/CustomSkull/
|-- config/config.json
|-- skins.json
|-- placed_skulls.json
`-- skinData/
    |-- <uuid>.png
    `-- <名称>.png
```

- `skins.json`：皮肤索引。
- `skinData/<uuid>.png`：规范化后的皮肤副本。
- `placed_skulls.json`：已放置头颅的坐标、维度、owner UUID、最终 yaw 和墙体偏移。

## 实现原理

1. 皮肤统一转换为 64x64 RGBA，并构建 `SerializedSkinRef`。
2. 放置头颅时保留原版 `player_head` 方块，同时生成一个客户端假玩家实体用于显示皮肤。
3. 假玩家使用 `PlayerList + AddPlayer + PlayerSkin` 下发，碰撞箱缩到 0.001，避免重力/碰撞问题。
4. 地板头颅按 `SkullBlockActor::mRotation + 180` 修正方向。
5. 墙挂头颅读取方块 `data` 的 2/3/4/5 判断方向，并做 0.23 格贴墙偏移、0.25 格高度偏移。
6. 玩家离开 512 格再回来时，使用新的 UUID 重新生成假玩家，避免客户端旧皮肤缓存导致 Steve。
7. 服务器重启或玩家重新进服时，从 `placed_skulls.json` 恢复并重新下发完整皮肤。
8. 假玩家生成 2 秒后发送 PlayerList Remove，清理玩家列表条目。

头颅方块本身保留在世界中，用于放置、破坏和物品行为；假玩家只是客户端显示层。

## 已知问题

### Java API 限流

`/skull java` 使用 Mojang 公共 API，可能返回 HTTP 429。命令会提示：

```text
Mojang rate limited, please wait a few minutes and retry
```

### Persona 皮肤

Persona / 4D / 5D 皮肤无法转换为 64x64 头颅贴图。

## 故障排查

启动日志应出现：

```text
[CustomSkull] CustomSkull enabled
```

如果出现：

```text
HologramLib player-NPC bridge unavailable, using built-in fallback
```

这是正常现象，当前版本本来就不需要 HologramLib。

放置头颅后可以执行 `/skull blockinfo`，聊天框和控制台都会输出方块坐标、rotation 和 NBT。

## 致谢

- 原 LL2 [CustomSkull](https://www.minebbs.com/resources/customskull.6291/)：命令布局和数据文件设计参考。
- [GeyserMC](https://geysermc.org/)：基岩版自定义头颅方案和 `geometry.humanoid.customskull` 几何体。
