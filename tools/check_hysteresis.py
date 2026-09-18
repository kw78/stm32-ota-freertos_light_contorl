#!/usr/bin/env python3
"""
光照状态机迟滞有序性检查 —— Bug #22 的静态守护

规则（Schmitt 触发器无抖动的充要条件）：
  对每一对相邻状态，从高值侧离开的阈值必须严格小于从低值侧离开的阈值。
  等价说法：进入某状态的阈值必须落在离开它的阈值之外——死区必须"外宽内窄"。
  违反即必然出现边界无限来回跳变（Bug #22：IDEAL↔GLARE 的 1000/800 接反，
  ADC 落在 (800,1000) 时双向条件同时成立，恒定光照下也 10 拍翻一次）。

做法：直接解析 Core/Src/main.c 的 LightState_Update——
  * 六个阈值 #define 的取值
  * 每个 case 状态块里的转移条件（哪个常量、哪个方向、去哪个状态）
  * 首次采样定性链（区域阈值必须严格递减）
从源码而非复制逻辑，改代码漏改约束时这里会红。

用法: python3 tools/check_hysteresis.py [main.c 路径]   （默认 Core/Src/main.c）
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

THRESHOLDS = ['DARK_EXIT', 'DIM_DOWN', 'DIM_UP', 'IDEAL_DOWN', 'IDEAL_UP', 'GLARE_EXIT']
# 按 ADC 值升序排列的状态链（ADC 越低 = 环境越亮）
ORDER = ['STATE_GLARE', 'STATE_IDEAL', 'STATE_DIM', 'STATE_DARK']


def parse_defines(src: str) -> dict[str, int]:
    vals = {}
    for name in THRESHOLDS:
        m = re.search(rf'#define\s+{name}\s+(\d+)', src)
        if not m:
            raise SystemExit(f'FAIL: 找不到 {name} 的 #define')
        vals[name] = int(m.group(1))
    return vals


def extract_function(src: str) -> str:
    start = src.index('void LightState_Update(')
    brace = src.index('{', start)
    depth, i = 0, brace
    while i < len(src):
        if src[i] == '{':
            depth += 1
        elif src[i] == '}':
            depth -= 1
            if depth == 0:
                return src[brace:i + 1]
        i += 1
    raise SystemExit('FAIL: LightState_Update 函数体括号不闭合')


def parse_transitions(body: str) -> dict[tuple[str, str], tuple[str, str]]:
    """→ {(src_state, dst_state): (比较符, 阈值常量名)}"""
    trans = {}
    # 按 case 切块
    cases = list(re.finditer(r'case\s+(STATE_\w+)\s*:', body))
    for idx, m in enumerate(cases):
        src_state = m.group(1)
        end = cases[idx + 1].start() if idx + 1 < len(cases) else len(body)
        chunk = body[m.end():end]
        for c in re.finditer(
                r'if\s*\(\s*adc_value\s*([<>])\s*(\w+)\s*\)[^\n]*\n'
                r'\s*state_target\s*=\s*(STATE_\w+)\s*;', chunk):
            cmp_op, const, dst = c.groups()
            trans[(src_state, dst)] = (cmp_op, const)
    return trans


def parse_boot_chain(body: str) -> list[tuple[str, str]]:
    """首次采样定性链：[(阈值常量, 状态)]，应为严格递减的区域边界"""
    out = []
    for m in re.finditer(
            r'if\s*\(\s*adc_value\s*>=\s*(\w+)\s*\)[^\n]*\n'
            r'\s*state_target\s*=\s*state_now\s*=\s*(STATE_\w+)\s*;', body):
        out.append((m.group(1), m.group(2)))
    return out


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else 'Core/Src/main.c')
    src = path.read_text(encoding='utf-8')
    vals = parse_defines(src)
    body = extract_function(src)
    trans = parse_transitions(body)

    print(f'检查对象: {path}（LightState_Update + {len(THRESHOLDS)} 个阈值 #define）')
    print(f'阈值: ' + ', '.join(f'{k}={v}' for k, v in vals.items()))

    problems: list[str] = []

    # 1) 相邻状态对的死区必须"外宽内窄"
    print('\n相邻状态对死区检查（高值侧离开阈值 必须 < 低值侧离开阈值）：')
    for i in range(len(ORDER) - 1):
        lo, hi = ORDER[i], ORDER[i + 1]
        t_lo = trans.get((lo, hi))      # 低值态 → 高值态，应为 '>'
        t_hi = trans.get((hi, lo))      # 高值态 → 低值态，应为 '<'
        if not t_lo or not t_hi:
            problems.append(f'{lo}↔{hi}: 缺少转移条件（低→高 {t_lo}，高→低 {t_hi}）')
            print(f'  {lo}↔{hi}: 转移缺失 ✗')
            continue
        if t_lo[0] != '>' or t_hi[0] != '<':
            problems.append(f'{lo}↔{hi}: 比较方向异常（低→高用 {t_lo[0]}，高→低用 {t_hi[0]}）')
        v_lo, v_hi = vals[t_lo[1]], vals[t_hi[1]]
        ok = v_hi < v_lo
        print(f'  {lo}↔{hi}: 高值侧离开 {t_hi[1]}={v_hi} < 低值侧离开 {t_lo[1]}={v_lo} '
              f'→ {"OK" if ok else "违反（死区接反，必抖）"}')
        if not ok:
            problems.append(
                f'{lo}↔{hi} 死区接反：离开阈值 {t_hi[1]}={v_hi} >= {t_lo[1]}={v_lo}，'
                f'区间 ({v_hi},{v_lo}) 双向条件同时成立 → 必然来回跳变')

    # 2) 首次采样定性链的区域边界必须严格递减
    chain = parse_boot_chain(body)
    print('\n首次采样定性链（区域阈值应严格递减）：')
    prev = None
    for const, state in chain:
        v = vals[const]
        ok = prev is None or v < prev[1]
        print(f'  adc >= {const}={v} → {state} {"OK" if ok else "违反（区域边界次序错乱）"}')
        if not ok:
            problems.append(f'定性链次序错乱：{const}={v} 未小于上一级 {prev[0]}={prev[1]}')
        prev = (const, v)

    print()
    if problems:
        print('结论: FAIL —— 迟滞设计规则被违反：')
        for p in problems:
            print(f'  ✗ {p}')
        return 1
    print('结论: PASS —— 三对相邻状态死区全部"外宽内窄"，定性链严格递减（Bug #22 类缺陷免疫）')
    return 0


if __name__ == '__main__':
    sys.exit(main())
