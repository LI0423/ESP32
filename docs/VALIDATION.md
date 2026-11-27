# Desktop Companion 验证记录

验证日期：2026-11-27

## 本轮范围

完成 DeskPal 宠物 v2 的产品逻辑与接口实现：

- 饱食度、健康、心情、清洁度、限量食物/药品、经验、年龄和四阶段成长
- 喂食、用药、陪玩、清洁及冷却/库存规则
- 饥饿、吃撑、脏污三类可恢复疾病；健康最低 10，不会死亡
- 夜间半速消耗、最多 48 小时离线生理补算
- 专注完成获得 XP/食物，每 4 轮额外获得药品
- NVS schema v2 宠物记录、显式编码与范围校验
- 旧计时器数据迁移；旧 `missed_day` 不再映射为宠物疾病
- USB 串口 `feed / medicine / play / clean`
- HTTPS 路由 `/api/pet/feed|medicine|play|clean`、状态中的 `pet` 对象和网页照护按钮
- 屏幕阶段标签、照护属性行与 3 秒动作反馈
- 接入 Grok 版四阶段与四动作索引精灵；休息/待命显示成长外观，照护显示动作素材

HTTPS 证书/私钥长度已修正为传入含结尾 NUL 的完整数组大小（`sizeof(...)`），与原 `sizeof(...)-1` 丢失 NUL 导致 mbedTLS 走 DER 路径的启动失败原因一致。网页 API 已构建，但本轮未执行 `http_smoke_test.py`。

## 主机测试

命令：

```bash
bash tools/run_tests.sh
```

实测输出：

```text
companion timer tests passed
companion pet tests passed
deskpal_sprites.c is up to date
```

覆盖：

- 原专注/庆祝/休息循环、08:00/22:00 边界、跨午夜待命、stale-save gap 恢复
- 今日日界、目标修改、连续天数、旧 vitality 回归
- 清醒每小时 `-4`、睡眠每小时 `-2`
- 饥饿 6 小时生病、脏污 12 小时生病、2 小时内吃撑 2 次生病
- 药品/清洁恢复、健康下限、动作冷却、缺物品、无效时间和时钟回拨保护
- 每轮专注奖励、每 4 轮药品、年龄 + XP 阶段门槛
- 48 小时离线补算上限与真实夜间消耗
- 串口照护命令及非法参数
- 14 张索引精灵与 2 枚 HUD 徽章（药丸、米饭碗）的数据/调色板访问和生成文件一致性

## 固件构建

环境：ESP-IDF v5.5.4 / ESP32-S3。

命令：

```bash
source /Users/litengjiang/.espressif/v5.5.4/esp-idf/export.sh
idf.py build
```

实测结果：**Project build complete**。

```text
desktop_companion.bin binary size: 0x13dac0 bytes
decimal:                          1,301,184 bytes (about 1.241MB)
smallest app partition:           0x180000
free in app partition:            0x42540 (17.3%)
```

## 实机验收（2026-11-27）

设备：ESP32-S3，`/dev/cu.usbmodem1401`，USB Serial/JTAG 控制台；已烧录最终固件。

- 启动后 SNTP 同步成功，`status` 中 `time_valid=true`、`online=true`，时钟正常
- `status` 包含完整 `pet` 对象，阶段、年龄、四项属性、食物/药品、XP、专注轮次、生病原因齐全
- 串口照护命令实测：
  - 饱食高于 90 时 `feed` 返回 `overfed`（健康 −10、食物 −1），紧接着再次 `feed` 返回 `cooldown`
  - `clean`、`play` 返回 `ok`；未生病时 `medicine` 返回 `not_sick`
  - `goal 6` 保存成功；`goal 0` 被拒绝并打印用法
- NVS 持久化：打开 USB 串口会复位板子，重启后 `food / health / mood / hygiene / xp / goal` 全部保持，验证属性变更会写入且能恢复
- `standby` 后进入待命、轮次不再增加；`focus` 后立刻回到 1500 秒专注
- HUD 徽章：通过临时演示固件分别注入吃撑病和饥饿状态，确认药丸与米饭碗按规则显示；临时代码已删除，最终固件不含演示逻辑
- 徽章规则：药丸仅在“药能治”的生病状态显示（吃撑病，或已喂食后的饥饿病）；米饭碗在饱食 <30 或饥饿病时显示；两者互斥、生病优先、1Hz 闪烁
- 本轮未执行网页 API / OTA / Secure Boot 相关实机验证

## 美术素材

- Grok 原始概念图：`assets/deskpal/pet-v2/grok-growth-stages.jpg`、`grok-action-sheet.jpg`
- 处理后素材：8 张 128×128、P 模式、index 0 透明、最多 32 色的索引 PNG
- 已加入 `tools/gen_deskpal_sprites.py`，生成 14 张精灵与 2 枚 HUD 徽章（药丸、米饭碗）的 C 数组
- 徽章由 `tools/gen_sick_pill_icon.py` 和 `tools/gen_hunger_icon.py` 按共享调色板生成，含 16/24/32px 版本与屏幕预览
- 新素材约 128KB 索引数据，当前嵌入 app；OTA 槽剩余约 17.3%
- 完整表情包或更多动画帧后续应迁移到 6MB `assets` 分区
- 制作规范见 `assets/deskpal/pet-v2/ART_REQUIREMENTS.md`

## 代码审查与安全检查

保留并沿用此前修复：

- HTTP JSON 解析释放后使用（UAF）
- 管理接口认证与常量时间令牌比较
- HTTP 请求体大小、类型和绝对接收期限
- 错误 WiFi 凭据下保留配网 AP
- OTA 总超时、无进展超时和取消
- SNTP 初始化失败重试
- NVS 计时记录版本与范围校验
- 看门狗订阅与主动喂养
- LCD 校准值不被硬编码覆盖

宠物持久化使用定长显式小端编码，不直接保存带填充的 C 结构体；完整 blob 为 56 字节（版本字节 + 55 字节负载），占用三个 BLOB_DATA 跨度项和一个索引项。NVS 空间不足时会先清理已废弃的 DesktopStatusPad 键再重试。读取时校验版本、长度、范围、疾病原因和时间戳；阶段和分数累积时间在加载时重建。

## 已知限制 / 待实测

- 网页 API 尚未实机验证（本轮未运行 `tools/http_smoke_test.py`），自签名证书需浏览器手动信任
- OTA 端到端流程、成功后 NVS 宠物记录恢复尚未实机验证
- 跨午夜年龄增长、夜间半速消耗、最长 48 小时离线补算仍需长时间板端验证
- 旧 NVS 迁移不会把 `missed_day` 变成生病，仍需在旧版设备上验证
- 生产环境启用 Secure Boot / Flash Encryption 仍需单独规划

## 后续板端验证清单

1. 运行 `tools/http_smoke_test.py` 与网页照护按钮流程
2. 注入饥饿/脏污状态，确认米饭徽章、疾病恢复与健康下限
3. 验证跨午夜年龄增长、夜间半速和重启后最多 48 小时补算
4. 验证 OTA 后宠物记录仍可恢复
5. 在生产环境规划签名密钥并启用 Secure Boot / Flash Encryption
