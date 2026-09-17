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
