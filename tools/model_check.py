#!/usr/bin/env python3
"""
Bootloader 升级状态机 —— 穷举式断电模型检查（P5 可靠性证明）

方法
----
固件里所有会改持久状态的代码路径被拆解成"原子操作"（一次 SPI Flash 扇区
擦除 / 一页写 / 内部 Flash 按页擦 / 按块编程）。真实硬件上每个原子操作之间
都可能断电——本脚本在每个原子操作后注入断电，然后模拟"上电恢复"，穷举：

  深度 1：生命周期任意一步断电
  深度 2：生命周期一步断电 + 恢复路径中再断电一次（提交×恢复 双故障）

断电模型分两种（对应硬件真实行为）：
  POR      —— 彻底掉电再上电：IWDG 配置被清（RM0008：IWDG 仅由复位停止），
              RCC 复位原因 = POR，SRAM noinit 邮箱内容可能丢失
  BROWNOUT —— 电压跌落触发复位但电源未断：IWDG 配置跨复位保留（BUGS.md #15），
              若复位前 IWDG 已武装且代码跳进半擦除 App，看门狗会救回来

判定（对应 README "设备永不变砖" 的主张）：
  OK        —— 最终运行某个完整固件（OLD/NEW/GOLD 均算），状态一致
  WARN      —— 运行但保护降级（金固件标志丢失 / 崩溃循环但 OTA 通道尚在）
  BRICK     —— 跳入半擦除内部 Flash 且 IWDG 未武装 → 永久挂死，唯一出路 SWD

  结论 = PASS 当且仅当穷举空间内无 BRICK 可达。

与源码的对应关系（改代码后请同步这里）：
  bootloader/Src/main.c  main()          —— boot_decision()
  Core/Src/ota.c         CMD_OTA_END     —— phase_end()
  Core/Src/supervisor.c  golden_backup   —— phase_confirm()
                                     —— flag_boot_init  —— app_flag_normalize()

用法
----
  python3 tools/model_check.py                # 检查当前（修复后）逻辑
  python3 tools/model_check.py --variant old  # 复现修复前的变砖路径（回归演示）
  python3 tools/model_check.py -v             # 打印每个故障场景的轨迹
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass, replace

# ---------------------------------------------------------------- 状态空间

VALID_APP = ('OLD', 'NEW', 'GOLD')       # 内部 Flash 三种"完整可运行"内容
GARBAGE = 'GARBAGE'                      # 擦除后/半写的内部 Flash

IDLE, PENDING, TESTING = 0, 1, 2


@dataclass(frozen=True)
class St:
    """持久状态抽象：SPI Flash 上的控制块/镜像 + 内部 Flash + IWDG。"""
    flag_valid: bool            # 控制块 magic/ver 合法（断电在 erase 后 write 前会丢）
    state: int                  # IDLE/PENDING/TESTING（flag_valid=False 时无意义）
    golden: bool                # flag.golden_valid
    retry: int                  # flag.retry_cnt
    internal: str               # 内部 Flash @0x08002000
    slotA: bool                 # 槽 A 镜像头+数据完整（可搬运）
    slotB: bool                 # 槽 B 金固件完整（可回滚）
    iwdg: bool                  # IWDG 是否武装（POR 清除；系统复位保留）


# 故障注入点之后的设备演化结果
OK, WARN_GOLDEN_LOST, WARN_CRASHLOOP, BRICK = 'OK', 'WARN(golden-lost)', 'WARN(crash-loop)', 'BRICK'

MAX_BOOTS = 8                   # 恢复闭包的步数上限（超过即认为不收敛）


# ---------------------------------------------------------------- 恢复语义

def boot_decision(st: St, reason: str, variant: str):
    """镜像 bootloader main() 的决策逻辑，返回 (新状态, 跳转目标, 是否发生搬运)。

    跳转目标是 boot 后 CPU 执行的东西：'app'（内部 Flash，好或坏）或 'loop'
    （黑屏决策——TESTING 分支只计不搬时同样跳内部 App）。
    """
    st2 = st
    if not st2.flag_valid:
        return st2, 'app', False          # 杂数据/擦除态：直接跳 App（README 行为）

    if st2.state == PENDING:
        if st2.slotA:
            # 搬运成功（闭包内假设不再断电）：内部 Flash ← 槽 A
            return replace(st2, state=TESTING, retry=0, internal='NEW'), 'app', True
        # 槽 A 损坏：不动内部 Flash；有金固件顺带回滚
        if st2.golden and st2.slotB:
            st2 = replace(st2, internal='GOLD', state=IDLE, retry=0)
            return st2, 'app', True
        return replace(st2, state=IDLE, retry=0), 'app', False

    if st2.state == TESTING:
        old_gate = (reason == 'IWDG')                        # 修复前：只认看门狗复位
        new_gate = (reason == 'IWDG' or st2.retry > 3)       # 修复后：retry 已超限也回滚
        gate = old_gate if variant == 'old' else new_gate
        if gate:
            if reason == 'IWDG' and st2.retry < 0xFFFF:
                st2 = replace(st2, retry=st2.retry + 1)
            if st2.retry > 3 and st2.golden and st2.slotB:
                return replace(st2, internal='GOLD', state=IDLE, retry=0), 'app', True
            return st2, 'app', False
    return st2, 'app', False


def closure(st: St, reason: str, app_new_good: bool, variant: str):
    """断电恢复闭包：反复开机直到稳定，返回终态分类。

    app_new_good：内部 App 是 NEW 时是否健康（BAD_FW 场景 = False → 崩溃循环）。
    App 健康 + TESTING + 10s → 确认（金备份 + IDLE）；崩溃 → IWDG 复位再来。
    Bug #21 语义：Bootloader 跳入 TESTING 候选前替它武装 IWDG（26s）——
    "装得进去但既不武装看门狗也不 HardFault 的安静挂死镜像"也会被咬，
    回滚不再依赖候选固件自身的合作。
    """
    reason_next = reason
    for _ in range(MAX_BOOTS):
        st2, target, installed = boot_decision(st, reason_next, variant)
        if st2.flag_valid and st2.state == TESTING:
            st2 = replace(st2, iwdg=True)     # Bootloader 兜底武装（Bug #21）

        # 搬运是非原子序列（擦内部→编程→写 flag），恢复路径上的第二次断电
        # 由深度 2 枚举覆盖；闭包内假设搬运一次完成。
        if st2.internal == GARBAGE:
            # 跳进半擦除 App：唯一的救命稻草是已武装的 IWDG
            if st.iwdg:
                reason_next = 'IWDG'
                st = replace(st, iwdg=True)      # 复位后 IWDG 配置仍在
                continue
            return BRICK, st2

        if st2.internal == 'NEW' and not app_new_good:
            # 崩溃循环：IWDG 复位（若武装）→ retry 爬升 → 超限回滚
            if not st2.iwdg:
                # 无看门狗的崩溃循环（理论态：IWDG 从未武装过的坏固件）
                return WARN_CRASHLOOP, st2
            reason_next = 'IWDG'
            st = replace(st2, retry=min(st2.retry + 1, 4))
            continue

        # App 健康运行（OLD/NEW/GOLD）
        if st2.flag_valid and st2.state == TESTING and app_new_good:
            # 确认：金备份成功（深度 2 覆盖备份中断电）→ IDLE
            return OK, replace(st2, state=IDLE, golden=st2.slotB, retry=0)
        if st2.internal in VALID_APP:
            if not st2.flag_valid:
                # App 侧 flag_boot_init 归一化：golden 信息丢失（golden=0）
                if st.slotB:
                    return WARN_GOLDEN_LOST, replace(st2, flag_valid=True, state=IDLE, golden=False, retry=0)
                return OK, replace(st2, flag_valid=True, state=IDLE, golden=False, retry=0)
            if st2.golden and not st2.slotB:
                return WARN_GOLDEN_LOST, st2     # 声称有金固件但槽 B 实际损坏
            return OK, st2
        reason_next = 'PIN'                      # 无异常继续开机
    return WARN_CRASHLOOP, st


# ---------------------------------------------------------------- 生命周期

@dataclass
class Atom:
    label: str          # 原子操作名（与源码对应）
    mutate: object      # St -> St，操作完成后的状态
    note: str = ''      # 源码出处


def lifecycle_atoms(app_new_good: bool, variant: str):
    """从"金固件已建立、健康运行 OLD"出发的一次完整升级生命周期的原子序列。

    new_good=True  —— 好固件：安装 → 确认 → 金备份 → IDLE
    new_good=False —— 坏固件：安装 → 崩溃 ×4（IWDG 复位爬 retry）→ 超限回滚
    """
    base = St(flag_valid=True, state=IDLE, golden=True, retry=0,
              internal='OLD', slotA=True, slotB=True, iwdg=True)

    atoms = [Atom('idle-boot', lambda s: s, '起点：健康运行中')]
    atoms += [
        Atom('end:erase hdrA',  lambda s: replace(s, slotA=False),            'ota.c END：W25_EraseSector(OTA_HDR_A_ADDR)'),
        Atom('end:write hdrA',  lambda s: replace(s, slotA=True),             'ota.c END：W25_WritePage(hdr)'),
        Atom('end:erase flag',  lambda s: replace(s, flag_valid=False),       'ota.c END：W25_EraseSector(OTA_FLAG_ADDR)'),
        Atom('end:write PENDING', lambda s: replace(s, flag_valid=True, state=PENDING, retry=0), 'ota.c END：置 PENDING 后复位'),
        Atom('boot:verify A',   lambda s: s,                                  'bootloader slot_verify(A)'),
        Atom('boot:erase int',  lambda s: replace(s, internal=GARBAGE),       'copy_firmware 按页擦除内部 Flash'),
        Atom('boot:program',    lambda s: replace(s, internal='NEW'),         'copy_firmware 编程+回读'),
        Atom('boot:erase flag', lambda s: replace(s, flag_valid=False),       'bootloader flag_write 前的擦除'),
        Atom('boot:write TESTING', lambda s: replace(s, flag_valid=True, state=TESTING, retry=0), 'bootloader：进入 TESTING'),
    ]
    if app_new_good:
        atoms += [
            Atom('confirm:verify A', lambda s: s,                             'supervisor golden_backup 校验槽 A'),
            Atom('confirm:erase hdrB', lambda s: replace(s, slotB=False),     'W25_EraseSector(OTA_HDR_B_ADDR)'),
            Atom('confirm:erase B data', lambda s: replace(s, slotB=False),   '逐扇区擦槽 B（长窗口）'),
            Atom('confirm:copy A>B', lambda s: replace(s, slotB=True),        '槽 A→槽 B 拷贝 + 回读'),
            Atom('confirm:write hdrB', lambda s: s,                           '写槽 B 镜像头'),
            Atom('confirm:erase flag', lambda s: replace(s, flag_valid=False), 'flag_commit 擦除'),
            Atom('confirm:write IDLE', lambda s: replace(s, flag_valid=True, state=IDLE, golden=True, retry=0), '确认完成'),
        ]
    else:
        # 坏固件崩溃循环：4 次 IWDG 复位把 retry 推过上限
        for i in range(1, 5):
            atoms.append(Atom(f'crash:IWDG reset #{i}',
                              (lambda n: lambda s: replace(s, state=TESTING, retry=n))(i),
                              'HardFault 等死 → IWDG 咬人'))
        atoms += [
            Atom('roll:verify B',  lambda s: s,                               'bootloader slot_verify(B)'),
            Atom('roll:erase int', lambda s: replace(s, internal=GARBAGE),    'copy_firmware 擦内部 Flash'),
            Atom('roll:program',   lambda s: replace(s, internal='GOLD'),     '金固件写入'),
            Atom('roll:erase flag', lambda s: replace(s, flag_valid=False),   'flag_write 擦除'),
            Atom('roll:write IDLE', lambda s: replace(s, flag_valid=True, state=IDLE, retry=0), '回滚完成'),
        ]
    return base, atoms


RECOVERY_ATOMS = {                      # 恢复路径上的第二次断电（深度 2）
    'install': [                        # PENDING 搬运（boot_decision 的非原子部分）
        Atom('rec:erase int', lambda s: replace(s, internal=GARBAGE)),
        Atom('rec:program',   lambda s: replace(s, internal='NEW')),
        Atom('rec:erase flag', lambda s: replace(s, flag_valid=False)),
        Atom('rec:write TESTING', lambda s: replace(s, flag_valid=True, state=TESTING, retry=0)),
    ],
    'rollback': [
        Atom('rec:erase int', lambda s: replace(s, internal=GARBAGE)),
        Atom('rec:program',   lambda s: replace(s, internal='GOLD')),
        Atom('rec:erase flag', lambda s: replace(s, flag_valid=False)),
        Atom('rec:write IDLE', lambda s: replace(s, flag_valid=True, state=IDLE, retry=0)),
    ],
}


# ---------------------------------------------------------------- 穷举引擎

def apply_cut(st: St, power: str):
    """断电瞬间的硬件语义。"""
    if power == 'POR':
        return replace(st, iwdg=False)
    return st                            # BROWNOUT：配置保留


def run_check(variant: str, verbose: bool):
    scenarios = []                       # (描述, 结果, 终态)

    for new_good in (True, False):
        base, atoms = lifecycle_atoms(new_good, variant)
        # ---- 深度 1：生命周期任意原子后断电 ----
        for power in ('POR', 'BROWNOUT'):
            for i, cut in enumerate(atoms):
                st = base
                for a in atoms[:i + 1]:
                    st = a.mutate(st)
                st = apply_cut(st, power)
                reason = 'POR' if power == 'POR' else 'PIN'
                res, final = closure(st, reason, new_good, variant)
                scenarios.append((f"{'good' if new_good else 'bad'}/{power}/{cut.label}", res, final))

        # ---- 深度 2：第一次断电后，恢复搬运路径中再断一次 ----
        for power in ('POR', 'BROWNOUT'):
            for i, cut in enumerate(atoms):
                st = base
                for a in atoms[:i + 1]:
                    st = a.mutate(st)
                st_cut1 = apply_cut(st, power)
                # 只对"接下来 bootloader 真的会搬运"的状态做恢复期注入
                _, _, installed = boot_decision(st_cut1, 'POR' if power == 'POR' else 'PIN', variant)
                if not installed:
                    continue
                kind = 'rollback' if (st_cut1.flag_valid and st_cut1.state == TESTING) else 'install'
                rec_atoms = RECOVERY_ATOMS[kind]
                for j, rc in enumerate(rec_atoms):
                    st2 = boot_decision_pre(st_cut1, kind)      # 搬运前的等价状态
                    for ra in rec_atoms[:j + 1]:                # 累积执行到断电点
                        st2 = ra.mutate(st2)
                    for power2 in ('POR', 'BROWNOUT'):
                        st3 = apply_cut(st2, power2)
                        reason = 'POR' if power2 == 'POR' else 'PIN'
                        res, final = closure(st3, reason, new_good, variant)
                        scenarios.append(
                            (f"{'good' if new_good else 'bad'}/{power}/{cut.label}+{power2}/{rc.label}", res, final))

    # 不变式专项：flag erase→write 窗口必须无害（断在 erase flag 时 internal 恒为完整镜像）
    inv_ok = True
    for new_good in (True, False):
        base, atoms = lifecycle_atoms(new_good, variant)
        for i, a in enumerate(atoms):
            if 'erase flag' in a.label:
                st = base
                for x in atoms[:i + 1]:
                    st = x.mutate(st)
                if st.internal == GARBAGE:
                    inv_ok = False
                    scenarios.append((f"INV-flag-window/{a.label}", BRICK, st))

    return scenarios, inv_ok


def boot_decision_pre(st: St, kind: str) -> St:
    """构造'搬运开始前'的等价状态（深度 2 注入用）。"""
    if kind == 'install':
        return replace(st, state=PENDING, retry=0) if st.flag_valid else replace(
            st, flag_valid=True, state=PENDING, retry=0, golden=st.golden and st.slotB)
    return replace(st, state=TESTING, retry=4, golden=st.golden and st.slotB)


# ---------------------------------------------------------------- 报告

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument('--variant', choices=['fixed', 'old'], default='fixed',
                    help='fixed=当前代码；old=修复前（TESTING 只认 IWDG 复位）')
    ap.add_argument('-v', '--verbose', action='store_true', help='打印全部场景轨迹')
    args = ap.parse_args()

    scenarios, inv_ok = run_check(args.variant, args.verbose)

    bricks = [s for s in scenarios if s[1] == BRICK]
    warns = [s for s in scenarios if s[1].startswith('WARN')]
    oks = [s for s in scenarios if s[1] == OK]

    print(f"模型检查（variant={args.variant}）")
    print(f"  故障场景总数 : {len(scenarios)}（穷举：生命周期×断电位置×POR/BROWNOUT×单/双故障）")
    print(f"  OK           : {len(oks)}")
    print(f"  WARN         : {len(warns)}")
    print(f"  BRICK        : {len(bricks)}")
    print(f"  不变式[flag 擦写窗口无害] : {'PASS' if inv_ok else 'FAIL'}")
    print()

    for name, res, final in scenarios:
        if res == BRICK:
            print(f"  ✗ BRICK  {name}")
            print(f"      终态: {final}")
    for name, res, final in warns[:10]:
        print(f"  ⚠ {res:18s} {name}")
    if len(warns) > 10:
        print(f"  ⚠ ... 其余 {len(warns) - 10} 条 WARN 同类")

    if args.verbose:
        print("\n全部场景：")
        for name, res, _ in scenarios:
            print(f"  {res:20s} {name}")

    print()
    if bricks or not inv_ok:
        print("结论: FAIL —— 存在可达的永久挂死路径（违反\"不变砖\"主张）")
        return 1
    print("结论: PASS —— 任意单点断电 + 恢复期再断电，设备总能回到某个完整固件")
    print("       （WARN 项为保护降级，不违反不变砖；详见上方列表）")
    return 0


if __name__ == '__main__':
    sys.exit(main())
