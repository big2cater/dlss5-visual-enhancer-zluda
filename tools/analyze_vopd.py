#!/usr/bin/env python3
"""
analyze_vopd.py: Deep inspection and profiling of RDNA 3 VOPD (Dual-Issue) instructions
across all compiled AMDGPU kernels in ZLUDA's ComputeCache.
"""

import sqlite3
import os
import sys
import subprocess
import tempfile
import collections
import re

def main():
    print("=" * 70)
    print("      RDNA 3 / RDNA 4 VOPD (Dual-Issue) Utilization Profiler      ")
    print("=" * 70)

    db_path = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'zluda', 'ComputeCache', 'zluda2.db')
    if not os.path.exists(db_path):
        print(f"[!] Database not found: {db_path}")
        return 1

    objdump = r'C:\Program Files\AMD\ROCm\7.1\bin\llvm-objdump.exe'
    if not os.path.exists(objdump):
        print(f"[!] llvm-objdump not found at {objdump}")
        return 1

    conn = sqlite3.connect(db_path)
    cur = conn.cursor()
    cur.execute("SELECT id, device, length(binary), binary FROM modules ORDER BY id;")
    rows = cur.fetchall()
    print(f"[+] Found {len(rows)} compiled modules in cache\n")

    grand_total_lines = 0
    grand_vector_alu = 0
    grand_scalar_alu = 0
    grand_vopd_pairs = 0
    grand_wmma = 0
    grand_mem = 0
    vopd_opcodes = collections.Counter()
    vopd_combinations = collections.Counter()

    # Regex patterns
    re_vopd = re.compile(r'^\s*([a-z0-9_]+)\s+([^:]+)::\s*([a-z0-9_]+)\s+([^\/]+)', re.IGNORECASE)

    for mid, dev, sz, binary in rows:
        with tempfile.NamedTemporaryFile(suffix='.elf', delete=False) as f:
            f.write(binary)
            elf_path = f.name

        try:
            proc = subprocess.run([objdump, '-d', elf_path], capture_output=True, text=True, errors='ignore')
        finally:
            if os.path.exists(elf_path):
                os.remove(elf_path)

        lines = proc.stdout.splitlines()
        mod_vector = 0
        mod_scalar = 0
        mod_vopd = 0
        mod_wmma = 0
        mod_mem = 0

        for line in lines:
            line_s = line.strip()
            if not line_s or line_s.endswith(':') or line_s.startswith('Disassembly'):
                continue

            # Check for VOPD
            if '::' in line and 'v_dual_' in line:
                mod_vopd += 1
                m = re_vopd.search(line_s)
                if m:
                    opX = m.group(1).strip()
                    opY = m.group(3).strip()
                    vopd_opcodes[opX] += 1
                    vopd_opcodes[opY] += 1
                    combo = tuple(sorted([opX, opY]))
                    vopd_combinations[combo] += 1
                continue

            # Other instruction classifications
            # Remove hex address and comment
            tokens = line_s.split('//')[0].split('/*')[0].strip().split()
            if len(tokens) >= 2 and tokens[0].endswith(':'):
                instr = tokens[1]
            elif tokens:
                instr = tokens[0]
            else:
                continue

            if instr.startswith('v_wmma_'):
                mod_wmma += 1
                mod_vector += 1
            elif instr.startswith('v_'):
                mod_vector += 1
            elif instr.startswith('s_'):
                mod_scalar += 1
            elif instr.startswith('flat_') or instr.startswith('global_') or instr.startswith('buffer_') or instr.startswith('ds_'):
                mod_mem += 1

        vopd_effective_instrs = mod_vopd * 2
        total_vector_ops = mod_vector + vopd_effective_instrs
        ratio = (vopd_effective_instrs / total_vector_ops * 100) if total_vector_ops > 0 else 0

        print(f"Module #{mid:02d} [{dev}] ({sz / 1024:.1f} KB):")
        print(f"  Total lines: {len(lines):7d} | Vector ALU: {mod_vector:6d} | WMMA: {mod_wmma:5d} | VOPD pairs: {mod_vopd:5d} ({ratio:4.1f}% dual-issued)")

        grand_total_lines += len(lines)
        grand_vector_alu += mod_vector
        grand_scalar_alu += mod_scalar
        grand_vopd_pairs += mod_vopd
        grand_wmma += mod_wmma
        grand_mem += mod_mem

    grand_vopd_ops = grand_vopd_pairs * 2
    grand_total_vector = grand_vector_alu + grand_vopd_ops
    overall_ratio = (grand_vopd_ops / grand_total_vector * 100) if grand_total_vector > 0 else 0

    print("\n" + "=" * 70)
    print("                    AGGREGATE SUMMARY ACROSS ALL MODULES          ")
    print("=" * 70)
    print(f"Total Disassembly Lines:        {grand_total_lines:,}")
    print(f"Total Vector ALU Instructions:  {grand_total_vector:,}")
    print(f"  - Single-Issue Vector ALU:    {grand_vector_alu:,}")
    print(f"  - Dual-Issue VOPD Pairs:      {grand_vopd_pairs:,} (executes {grand_vopd_ops:,} instructions)")
    print(f"  - WMMA Matrix Ops:            {grand_wmma:,}")
    print(f"Total Scalar ALU Instructions:  {grand_scalar_alu:,}")
    print(f"Total Memory / LDS Ops:         {grand_mem:,}")
    print(f"\n>>> OVERALL VOPD DUAL-ISSUE UTILIZATION: {overall_ratio:.2f}% <<<")

    print("\n" + "-" * 70)
    print("VOPD Opcode Breakdown (Individual Operations):")
    print("-" * 70)
    for op, count in vopd_opcodes.most_common():
        print(f"  {op:<30}: {count:6d} ({count / grand_vopd_ops * 100:5.1f}%)")

    print("\n" + "-" * 70)
    print("Top Dual-Issue Instruction Pair Combinations (X :: Y):")
    print("-" * 70)
    for (op1, op2), count in vopd_combinations.most_common(12):
        print(f"  {op1} :: {op2:<25}: {count:6d} pairs")

    print("=" * 70)
    return 0

if __name__ == '__main__':
    sys.exit(main())
