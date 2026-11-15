# CustomSkull

为 **LeviLamina** 的 Minecraft 基岩版专用服务器提供自定义玩家头颅。

可以把在线基岩版玩家的皮肤、Java 版账号皮肤或本地 PNG 制作成头颅物品，放置到世界里作为自定义玩家头。客户端不需要安装资源包。

> 当前版本核心功能可用；放置后有两个已知问题，见[已知问题](#已知问题)。欢迎提交 PR。

## 功能特性

- `/skull get <玩家> [数量]`：用在线基岩版玩家当前皮肤制作头颅。
- `/skull java <玩家> [数量]`：通过 Mojang API 下载 Java 版玩家皮肤制作头颅。
- `/skull create <名称> [数量]`：用 `skinData/<名称>.png` 制作头颅。
- `/skull list`：列出已保存的皮肤。
- `/skull reload`：重新加载 `skins.json` 并重新广播皮肤。
- `/skull entity <名称>`：在脚下生成一个测试头颅实体。
- `/skull blockinfo`：诊断命令，输出附近头颅方块的坐标、旋转和 NBT。
- 玩家进服时自动重发已保存的皮肤。
- 已放置的头颅记录在 `placed_skulls.json`，服务器重启后自动恢复。

## 前置要求

[HologramLib](https://github.com/Ieckseyr/HologramLib)是显示头颅的必需前置，请安装到 `./mods/HologramLib/`。

如果 HologramLib 缺失或 ABI 不匹配，插件会输出警告并回退到内置的客户端假实体方案；该回退方案不是受支持的正式配置。

## 安装（服务端）

### 使用 LIP

```
lip install github.com/FLAI-Studio/CustomSkull
```

### 手动安装

1. 把 HologramLib 安装到 `./mods/HologramLib/`。
2. 从 Releases 下载 `CustomSkull-server-windows-x64.zip` 并解压到服务端`./mods`

   ```text
   mods/
   |-- CustomSkull/
   |   |-- CustomSkull.dll
   |   `-- manifest.json
   `-- HologramLib/
       |-- HologramLib.dll
       `-- manifest.json
   ```

## 命令

所有命令需要管理员权限（`GameDirectors`）。`数量` 会被限制在 1~64。

| 命令 | 说明 |
| --- | --- |
| `/skull get <玩家> [数量]` | 用在线基岩版玩家的皮肤制作头颅。 |
| `/skull java <玩家> [数量]` | 用 Java 版账号皮肤制作头颅。 |
| `/skull create <名称> [数量]` | 用 `skinData/<名称>.png` 制作头颅。 |
| `/skull list` | 列出已保存的皮肤。 |
| `/skull reload` | 重新加载皮肤索引并重新广播。 |
| `/skull entity <名称>` | 在脚下生成测试头颅实体。 |
| `/skull blockinfo` | 输出附近头颅方块数据（诊断用）。 |

## 配置

`mods/CustomSkull/config/config.json`：

```json
{
    "version": 1,
    "enableJavaFetch": true,                          //Java账号头颅获得功能开关
    "debug": false,                                   //调试模式：输出排查日志
    "skullNameFormat": "\u00a7e{}\u7684\u5934\u9885", //头颅物品名
    "maxEntriesPerPacket": 8,                         //PlayerListPacket携带的皮肤数量
    "keepSkinListed": true,                           //是否保留普通皮肤条目
    "useHologramLib": true,                           //HologramLib模式开关
    "useServerAnchor": false                          //实验性：盔甲架锚点模式开关
}
```

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `enableJavaFetch` | true | 是否允许 `/skull java`。 |
| `debug` | false | 输出更多调试日志；关闭时只保留警告和错误。 |
| `skullNameFormat` | "\u00a7e{}\u7684\u5934\u9885" | 头颅物品显示名，`{}` 会替换成玩家名。 |
| `maxEntriesPerPacket` | 8 | 每个 `PlayerListPacket` 携带的皮肤数量（每个皮肤约 16 KiB）。 |
| `keepSkinListed` | true |  是否保留普通皮肤条目。 |
| `useHologramLib` | true | 使用 HologramLib `IPlayerNpc` 在世界中显示头颅。 |
| `useServerAnchor` | false | 实验性：真实 armor_stand 锚点方案，当前版本强制关闭。 |

当前发布版在加载配置后会强制：

```cpp
useHologramLib  = true;
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
- `placed_skulls.json`：已放置头颅的坐标、维度、owner UUID 和最终 yaw，用于持久化。

## 实现原理

1. 皮肤统一转换为 64x64 RGBA，并构建 `SerializedSkinRef`。
2. 通过 HologramLib 的 `IPlayerNpc` 注册皮肤。
3. 放置头颅时，在方块坐标创建一个 HologramLib 玩家 NPC，使用 `geometry.humanoid.customskull` 几何体，只显示头部。
4. 放置后和服务器重启后，读取方块的 `SkullBlockActor::mRotation`，把 NPC 的 yaw 修正为 `rotation + 180`，与头颅方块方向一致。
5. 客户端重载区块后，HologramLib NPC 会在正确位置重新渲染。

头颅方块本身保留在世界中，用于放置、破坏和物品行为；NPC 只是客户端显示层。

## 已知问题

### 放置后头颅低两格，重载客户端区块后恢复正常

放置后和服务器重启后，HologramLib 玩家 NPC 会从头颅方块位置落下去两格；等客户端卸载并重载该区块后显示正常。方向、皮肤和持久化均正常。

这是 HologramLib 玩家 NPC 的渲染/物理行为，不是坐标错误：`/skull blockinfo` 可以确认事件坐标和真实的 `player_head` 方块坐标完全一致。

已经尝试过以下方案，均未解决或带来其他问题：

- `metaScale` 缩小碰撞箱：不能关闭客户端重力
- 真实 armor_stand + `IViewOverride`：`overrideEntity` 返回 `false`
- 真实 armor_stand 骑乘锚点：自定义实体无法被服务端 `fetchEntity`，骑乘失败
- 周期重发 `PlayerList/AddPlayer/PlayerSkin`：皮肤重置、闪烁、变 Steve

要修复需要在 HologramLib 层处理玩家 NPC 的垂直偏移，或实现安全的客户端区块重载。欢迎 PR。

### Java API 限流

`/skull java` 使用 Mojang 公共 API，小概率可能返回 HTTP 429。请等待一段时间后重试。

### Persona 皮肤

Persona / 4D / 5D 皮肤无法转换为 64x64 头颅贴图。

## 故障排查

启动日志应出现：

```text
[CustomSkull] HologramLib player-NPC bridge enabled
```

如果出现：

```text
[CustomSkull] HologramLib player-NPC bridge unavailable: ...
```

说明 HologramLib 未正确安装或加载，请检查 HologramLib 版本是否与服务端匹配以及安装目录是否正确。

放置头颅后可以执行 `/skull blockinfo`，查看真实方块坐标、rotation 和 block actor NBT。

## 致谢

- 原 LL2 [CustomSkull](https://www.minebbs.com/resources/customskull.6291/)：命令布局和数据文件设计参考。
- [GeyserMC](https://geysermc.org/)：基岩版自定义头颅方案和 `geometry.humanoid.customskull` 几何体。
- [HologramLib](https://github.com/Ieckseyr/HologramLib)：26.60 玩家 NPC 协议层和皮肤注册表。
