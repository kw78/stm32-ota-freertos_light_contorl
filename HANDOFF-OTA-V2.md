# HANDOFF — OTA v2 上板调试交接（feat/ota-v2 分支）

> 交接时间点：2026-09-17。本文件为临时调试文档，问题闭环后删除。

## 分支状态

- `feat/ota-v2`（基于 `fix/ota-bugfix`），HEAD = 2f3363f + 本文档提交
- master 未动；**用户 MPU6050 WIP（MPU6050.h/main.h/MPU6050.c/freertos.c 4 个文件）未提交保留，勿动勿提交**
- 提交链：caf24d2(bug修复) → 75c128f(P1 OTA v2) → ba5bbad(P2 黑匣子) → 4b4dbe0(P3 调光) → 081cad3(docs) → 2bf95ea(BAD_FW_TEST钩子) → 2f3363f(IWDG加固)
- CI（GitHub Actions）截至 081cad3 全绿；2f3363f 之后如未推需补推

## 已完成的硬件验收（全部通过）

1. ST-Link 烧录 boot(7352B)+app(43128B)，banner `APP v2 2BF95EA3`、v1 flag 迁移、QUERY 正常
2. OTA 升级 → PENDING → 搬运 → TESTING → 10s 确认 → 金固件建立（QUERY: IDLE golden=1）
3. 坏固件 BADF0001（BAD_FW_TEST 构建，banner 后写空指针）→ 崩溃循环 → **~20s 自动回滚**到 good
4. 黑匣子取证：BOOT 复位原因链 + FAULT 现场（CFSR=IMPRECISERR/HFSR=FORCED/PC=0x0800AB08，
   addr2line 落在 OLED_Init——写缓冲延迟归因，教科书案例）
5. dim on/off 协议链路 OK
6. 实测发现并已加固：**IWDG PR/RLR 跨系统复位保留**（BUGS.md #15）→ bootloader 搬运逐块喂狗 + OTA END 复位前喂狗

## ✅ 未解之谜结案（2026-09-17 第三会话，真板复测）

1. **"扫描中途设备无响应"已破案**：上位机 `read_packet_v2` 最小帧按 8 字节算，
   空槽的 7 字节响应解析不出（BUGS.md #19）——设备从未在扫描中复位，
   "供电毛刺导致复位"假说不成立（原始扫描点固定在第 9 槽=第一条空槽）。
2. **good2 上传后运行旧版：不复现**。新 bootloader（含 BT 跟踪 + #18 修复）下，
   金固件已建立状态做连续第二次上传（版本 CAFE0001 实测）：TESTING 运行新版 →
   确认 → IDLE，安装正确。原始异常伴随"热换 bootloader"的脏状态（黑匣子 seq 6
   为证），无法归因；黑匣子 8 条记录已全部取出归档（/tmp/blackbox_before_reflash.txt），
   无丢失。若复发，开机 BT 决策行会直接暴露岔路。
3. **SPI Flash JEDEC 0x207017 实锤**（BT id=00207017），驱动按通用 SPI NOR
   时序工作正常（本会话 3 次 OTA + 回滚 + 黑匣子读写全部正常），维持"兼容使用"结论。

### 本会话真板验收结果

- 基线重烧（boot 7748B @BRR 修复 + app E5A92E13）✓，BT 跟踪可读 ✓
- HIL 全链路 8/8：good 升级确认金固件 ✓ / BADF0001 崩溃→IWDG×4→自动回滚→
  版本恢复 ✓ / 黑匣子 REC_FAULT 转录 ✓（`tools/hil_accept.py`）
- good2 圍复测（上文）✓；混沌测试结果见 /tmp/chaos_real.csv

## ⚠️ 未解之谜（新会话第一优先）

**good2（v2F3363F，含加固）OTA 上传后设备运行的是旧版 2BF95EA3。**

黑匣子证据（第 8 条后扫描中途超时，可能还有未取到的记录）：

| seq | 版本 | 复位原因 | 状态 | 解读（假设） |
|-----|------|---------|------|--------------|
| 6 | 2BF95EA3 | IWDG\|PIN | IDLE, golden=1 | 热换 bootloader 后首启；IWDG 来自 openocd halt 超 8s 窗口（halt 停 CPU 不停 IWDG） |
| 7 | 2BF95EA3 | SFT\|PIN | IDLE | good2 END 复位后的启动，**未进入 TESTING** → slot A 校验失败？未安装？ |
| 8 | 2BF95EA3 | SFT\|PIN | TESTING | 旧 App 观察到 TESTING → 有一次"安装完成后内部 Flash 仍是旧 App"或"slot A 内容实际是旧固件" |

**关键异常**：黑匣子 log 扫描两次都在末尾"设备无响应"中止——扫描期间设备疑似复位（同源问题的另一表现）。

假说（按怀疑度排序）：
1. slot A 镜像在"确认备份/第二次上传"交互中被污染（golden_backup 或 END 写 hdr/flag 的地址/时序问题）
2. SPI Flash 实为 JEDEC 0x207017（ST 系，非 Winbond 0xEF4017），4KB 擦除语义/时序差异（但第 1 次 OTA 全链路成功，矛盾点）
3. IWDG 在安装中途打断造成半写循环

### 本轮已落地的调查增强（2026-09-17 第二会话）

- **Bootloader UART 跟踪已实现**（BOOT_TRACE，寄存器级 TX ~400B，boot 7748B/8KB 闸门内）：
  每次开机打印 `BT rst=<CSR> id=<JEDEC> st=<state><golden> ry=<retry>` 及
  vA/cp/rbg 等决策摘要——上面步骤 2/3 的"openocd 读 Flash"大部分可以换成直接看串口
- **PVD 欠压记录已实现**：VDD<2.9V 触发 EXTI16 → noinit 邮箱 → 黑匣子 REC_PVD
  （`ota_tool.py log` 已解码）。"扫描中途设备无响应"若与欠压相关，重启后会有 PVD 记录
- **模型检查/混沌台/fuzz 台/HIL 流水线**见 README P5；fuzz 顺手修了 ota.c 两个
  潜伏缺陷（BUGS.md #16/#17），bootloader 回滚门槛修复见 #18——**bootloader 已改动，
  复测 good2 前需重烧 boot**

### 调查步骤建议

1. **单进程串口监听 + 重传**（串口严禁双进程同时读，上次因此失败）；直接看启动 banner 序列与崩溃循环
2. openocd 短暂 halt（**<3s**，防 IWDG halt 咬人）读内部 Flash 0x08002000 头部字节，与 good.bin/good2.bin 对比确认"内部到底装的谁"
3. 给 bootloader 加极简 UART 跟踪（寄存器级 TX 初始化 ~20 行 ~100B，预算余 648B），打印 slot_verify 结果/安装/回滚决策——一次性提升所有此类问题的可观测性
4. 复现前先记录黑匣子 seq 基线，或临时擦除黑匣子扇区归零

## 其他剩余任务

- [ ] ota_tool log 扫描超时根因（可能与"设备中途复位"同源）
- [ ] 调试收尾：worktree `/tmp/gcctest-deploy` 清理（`git worktree remove`）
- [ ] README/BUGS.md 同步最终结论；HANDOFF 本文件删除
- [ ] 合并 feat/ota-v2 → master（用户决定时机）

## 环境备忘

- 串口 `/dev/ttyUSB0` = CH340（usbipd busid **5-2**），ST-Link = busid **5-1**；掉线时：
  `usbipd.exe detach --busid 5-2 && usbipd.exe attach --wsl --busid 5-2`（bind 需 UAC 已持久）
- Python 用 `/home/kaiwen/miniconda3/envs/ota/bin/python`（有 pyserial；系统 python 无）
- GitHub push 22 端口不稳，用：
  `GIT_SSH_COMMAND="ssh -o BatchMode=yes -p 443" git push ssh://git@ssh.github.com:443/kw78/stm32-ota-freertos_light_contorl.git feat/ota-v2`
  Gitee 直连正常
- openocd halt 停 CPU 不停 IWDG（运行期窗口 8s）→ halt 必须 <3s
- 尺寸：app 43132B/54K，boot 7352B/8K（闸门 tools/ci_manifest.py）
