#!/usr/bin/env python3
"""FLUX VM Opcode Utilization Analyzer — Task 8, Oracle1 campaign."""

import re
import random
import os
import sys
from collections import Counter, defaultdict

# ── Opcode definitions ──────────────────────────────────────────────
OPCODES = {
    # arithmetic
    "ADD":  (1, "arithmetic"), "SUB":  (1, "arithmetic"), "MUL":  (2, "arithmetic"),
    "DIV":  (3, "arithmetic"), "MOD":  (3, "arithmetic"), "SHL":  (1, "arithmetic"),
    "SHR":  (1, "arithmetic"), "AND":  (1, "arithmetic"), "OR":   (1, "arithmetic"),
    "XOR":  (1, "arithmetic"), "NEG":  (1, "arithmetic"), "INC":  (1, "arithmetic"),
    "DEC":  (1, "arithmetic"),
    # memory
    "LOAD": (3, "memory"), "STORE": (3, "memory"), "FETCH": (4, "memory"),
    "PUSH": (1, "memory"), "POP":   (1, "memory"), "MOV":  (1, "memory"),
    # comparison
    "CMP":  (1, "comparison"), "TEST": (1, "comparison"),
    # control
    "JZ":   (1, "control"), "JNZ":  (1, "control"), "JMP":  (1, "control"),
    "CALL": (2, "control"), "RET":   (1, "control"), "LOOP": (2, "control"),
    # flux
    "SPAWN": (12, "flux"), "SEND":  (8, "flux"), "RECV":  (8, "flux"),
    "GAUGE": (8, "flux"), "LINK":  (6, "flux"), "YIELD": (5, "flux"),
    "MERGE": (10, "flux"),
}

REGS = ["R0","R1","R2","R3","R4","R5","R6","R7"]
LABELS = [f"label_{i}" for i in range(50)]
AGENTS = [f"agent_{i}" for i in range(20)]
GAUGES = ['"cpu_load"', '"mem_usage"', '"net_io"', '"latency"', '"queue_depth"']

# ── Synthetic trace generation ──────────────────────────────────────
def rand_reg(): return random.choice(REGS)
def rand_imm(): return f"#{random.randint(0,255)}"
def rand_addr(): return f"0x{random.randint(0,0xFFFF):04X}"

def gen_instruction(opcode, agent_type=None):
    if opcode in ("ADD","SUB","MUL","DIV","MOD","AND","OR","XOR"):
        return f"{opcode} {rand_reg()}, {rand_reg()}, {rand_imm()}"
    elif opcode in ("SHL","SHR"):
        return f"{opcode} {rand_reg()}, {rand_reg()}, {rand_imm()}"
    elif opcode in ("NEG","INC","DEC"):
        return f"{opcode} {rand_reg()}"
    elif opcode in ("LOAD","FETCH"):
        return f"{opcode} {rand_reg()}, {rand_addr()}"
    elif opcode in ("STORE",):
        return f"{opcode} {rand_reg()}, {rand_addr()}"
    elif opcode in ("PUSH","POP"):
        return f"{opcode} {rand_reg()}"
    elif opcode in ("MOV",):
        return f"{opcode} {rand_reg()}, {rand_reg()}"
    elif opcode in ("CMP","TEST"):
        return f"{opcode} {rand_reg()}, {rand_imm()}"
    elif opcode in ("JZ","JNZ"):
        return f"{opcode} {random.choice(LABELS)}"
    elif opcode in ("JMP","CALL"):
        return f"{opcode} {random.choice(LABELS)}"
    elif opcode == "RET":
        return "RET"
    elif opcode == "LOOP":
        return f"LOOP {rand_reg()}, {random.choice(LABELS)}"
    elif opcode == "SPAWN":
        return f"SPAWN {random.choice(AGENTS)}"
    elif opcode == "SEND":
        return f"SEND {random.choice(AGENTS)}, {rand_reg()}"
    elif opcode == "RECV":
        return f"RECV {rand_reg()}"
    elif opcode == "GAUGE":
        return f"GAUGE {random.choice(GAUGES)}"
    elif opcode == "LINK":
        return f"LINK {random.choice(AGENTS)}, {random.choice(AGENTS)}"
    elif opcode == "YIELD":
        return "YIELD"
    elif opcode == "MERGE":
        return f"MERGE {random.choice(AGENTS)}"
    return f"{opcode} {rand_reg()}"

AGENT_PROFILES = {
    "navigator": {
        "ADD": 25, "SUB": 20, "MUL": 15, "DIV": 10, "MOD": 5,
        "CMP": 8, "LOAD": 5, "STORE": 3, "MOV": 4, "JZ": 3, "JNZ": 2,
        "INC": 3, "DEC": 2, "SHL": 2, "SHR": 2, "AND": 2, "OR": 2,
        "LOOP": 3, "CALL": 2, "RET": 2, "SPAWN": 1, "GAUGE": 1,
    },
    "guardian": {
        "CMP": 20, "JZ": 18, "JNZ": 15, "JMP": 5, "TEST": 10,
        "LOAD": 8, "FETCH": 5, "MOV": 5, "AND": 5, "OR": 3,
        "ADD": 3, "SUB": 3, "INC": 2, "DEC": 2, "CALL": 2, "RET": 2,
        "LOOP": 4, "SPAWN": 3, "GAUGE": 2, "LINK": 2,
    },
    "communicator": {
        "SEND": 20, "RECV": 20, "LINK": 10, "SPAWN": 8, "MERGE": 5,
        "LOAD": 8, "STORE": 5, "MOV": 5, "ADD": 3, "CMP": 4,
        "JZ": 3, "GAUGE": 5, "YIELD": 3, "PUSH": 2, "POP": 2,
    },
}

def gen_trace(agent_type, count=10000):
    profile = AGENT_PROFILES[agent_type]
    opcodes = list(profile.keys())
    weights = list(profile.values())
    total = sum(weights)
    weights = [w/total for w in weights]
    lines = []
    pc = 0
    for _ in range(count):
        op = random.choices(opcodes, weights=weights, k=1)[0]
        cycles, cat = OPCODES[op]
        args = gen_instruction(op, agent_type)
        lines.append(f"[{pc:05d}] {op} {args}    ; {cat}: {cycles} cycle{'s' if cycles!=1 else ''}")
        pc += random.randint(1, 5)
    return "\n".join(lines)

# ── Trace parser ────────────────────────────────────────────────────
TRACE_RE = re.compile(r'\[(\d+)\]\s+(\w+)\s+(.+?)\s*;\s*(\w+):\s*(\d+)\s*cycle')

def parse_trace(text):
    """Returns list of (opcode, category, cycles)."""
    ops = []
    for line in text.strip().split('\n'):
        m = TRACE_RE.search(line)
        if m:
            ops.append((m.group(2), m.group(4), int(m.group(5))))
    return ops

# ── Analysis ────────────────────────────────────────────────────────
def analyze(traces):
    """traces: dict of agent_type -> [(opcode, category, cycles), ...]"""
    agent_types = list(traces.keys())

    # Per-agent opcode frequency
    agent_freq = {}
    for at, ops in traces.items():
        agent_freq[at] = Counter(op for op, _, _ in ops)

    # Global opcode frequency + cycle cost
    global_freq = Counter()
    opcode_cycles = defaultdict(list)
    all_ops_seq = []  # flat opcode list for n-gram analysis

    for ops in traces.values():
        seq = [op for op, _, _ in ops]
        all_ops_seq.extend(seq)
        for op, _, cyc in ops:
            global_freq[op] += 1
            opcode_cycles[op].append(cyc)

    avg_cycles = {op: sum(c)/len(c) for op, c in opcode_cycles.items()}
    total_ops = sum(global_freq.values())

    # Uniqueness: 1 - (fraction of agents that use this opcode)
    uniqueness = {}
    for op in global_freq:
        using = sum(1 for at in agent_types if agent_freq[at].get(op, 0) > 0)
        uniqueness[op] = 1.0 - (using / len(agent_types)) if len(agent_types) > 1 else 0.5

    # Single-opcode candidates
    single_scores = {}
    for op, freq in global_freq.items():
        score = freq * uniqueness.get(op, 0.5) * avg_cycles.get(op, 1)
        single_scores[op] = score

    # N-gram analysis across all traces combined
    # Bigrams and trigrams
    bigrams = Counter()
    trigrams = Counter()

    for ops in traces.values():
        seq = [op for op, _, _ in ops]
        for i in range(len(seq)-1):
            bigrams[(seq[i], seq[i+1])] += 1
        for i in range(len(seq)-2):
            trigrams[(seq[i], seq[i+1], seq[i+2])] += 1

    # Score bigrams and trigrams
    # Cycle savings = sum of individual cycles - estimated single-op cost (1 cycle for simple, 2 for complex)
    HOT_THRESHOLD = 100

    candidates = []

    # Bigram candidates
    for (op1, op2), freq in bigrams.items():
        if freq < HOT_THRESHOLD:
            continue
        c1 = avg_cycles.get(op1, 1)
        c2 = avg_cycles.get(op2, 1)
        # Estimate fused cost: slightly more than max, slightly less than sum
        fused_cost = max(c1, c2) + 0.5
        savings = (c1 + c2) - fused_cost
        # Uniqueness: how specific is this pair to certain agent types
        pair_agents = 0
        for at in agent_types:
            aseq = [op for op, _, _ in traces[at]]
            a_bigrams = Counter()
            for i in range(len(aseq)-1):
                a_bigrams[(aseq[i], aseq[i+1])] += 1
            if a_bigrams.get((op1, op2), 0) > 0:
                pair_agents += 1
        u = 1.0 - (pair_agents / len(agent_types)) if len(agent_types) > 1 else 0.5
        score = freq * u * savings
        candidates.append({
            "pattern": f"{op1} → {op2}",
            "name": f"{op1}_{op2}".upper(),
            "freq": freq,
            "savings": savings * freq,
            "score": score,
            "justification": f"{'Hot' if freq > HOT_THRESHOLD*5 else 'Frequent'} {op1}+{op2} sequence; {savings:.1f} cycles saved per fusion",
        })

    # Trigram candidates
    for (op1, op2, op3), freq in trigrams.items():
        if freq < HOT_THRESHOLD:
            continue
        c1 = avg_cycles.get(op1, 1)
        c2 = avg_cycles.get(op2, 1)
        c3 = avg_cycles.get(op3, 1)
        fused_cost = max(c1, c2, c3) + 1.0
        savings = (c1 + c2 + c3) - fused_cost
        pair_agents = 0
        for at in agent_types:
            aseq = [op for op, _, _ in traces[at]]
            a_tri = Counter()
            for i in range(len(aseq)-2):
                a_tri[(aseq[i], aseq[i+1], aseq[i+2])] += 1
            if a_tri.get((op1, op2, op3), 0) > 0:
                pair_agents += 1
        u = 1.0 - (pair_agents / len(agent_types)) if len(agent_types) > 1 else 0.5
        score = freq * u * savings
        candidates.append({
            "pattern": f"{op1} → {op2} → {op3}",
            "name": f"{op1}_{op2}_{op3}".upper(),
            "freq": freq,
            "savings": savings * freq,
            "score": score,
            "justification": f"{'Hot' if freq > HOT_THRESHOLD*5 else 'Frequent'} {op1}+{op2}+{op3} chain; {savings:.1f} cycles saved per fusion",
        })

    # Sort by score
    candidates.sort(key=lambda x: x["score"], reverse=True)

    return {
        "global_freq": global_freq,
        "avg_cycles": avg_cycles,
        "single_scores": single_scores,
        "bigrams": bigrams,
        "trigrams": trigrams,
        "hot_bigrams": {k: v for k, v in bigrams.items() if v >= HOT_THRESHOLD},
        "hot_trigrams": {k: v for k, v in trigrams.items() if v >= HOT_THRESHOLD},
        "candidates": candidates[:10],
        "total_ops": total_ops,
        "agent_freq": agent_freq,
    }

# ── Name suggestion heuristics ─────────────────────────────────────
def suggest_name(op1, op2=None, op3=None):
    """Suggest a concise name for a fused opcode."""
    known = {
        ("ADD","CMP"): "TEST_ADD",
        ("SUB","CMP"): "TEST_SUB",
        ("CMP","JZ"): "JLT",  # compare and jump if less-than
        ("CMP","JNZ"): "JNE",
        ("LOAD","CMP"): "FETCH_TEST",
        ("ADD","STORE"): "INC_MEM",
        ("INC","CMP"): "INC_TEST",
        ("DEC","CMP"): "DEC_TEST",
        ("SEND","RECV"): "EXCHANGE",
        ("SPAWN","LINK"): "SPAWN_LINK",
        ("GAUGE","SEND"): "REPORT",
        ("LOAD","ADD","STORE"): "MEM_ADD",
        ("CMP","JZ","LOOP"): "LOOP_WHILE",
        ("ADD","CMP","JZ"): "ADD_TEST_JZ",
        ("SEND","GAUGE","YIELD"): "SEND_REPORT",
    }
    key = (op1, op2) if op3 is None else (op1, op2, op3)
    return known.get(key, f"{op1}_{op2}" + (f"_{op3}" if op3 else ""))

# ── Output ──────────────────────────────────────────────────────────
def print_report(results):
    print("=" * 72)
    print("  FLUX VM OPCODE UTILIZATION ANALYSIS")
    print("  Oracle1 Campaign — Task 8")
    print("=" * 72)
    print(f"\nTotal instructions analyzed: {results['total_ops']:,}")

    # Per-agent summary
    print("\n─── Agent Type Breakdown ───")
    for at, freq in results["agent_freq"].items():
        total = sum(freq.values())
        top3 = freq.most_common(3)
        top_str = ", ".join(f"{op}({cnt/total*100:.1f}%)" for op, cnt in top3)
        print(f"  {at:14s}  {total:>6,} ops  top: {top_str}")

    # Opcode frequency table
    print("\n─── Global Opcode Frequency (top 15) ───")
    print(f"  {'Opcode':<10s} {'Count':>7s} {'AvgCyc':>7s} {'Score':>9s}")
    sorted_ops = sorted(results["single_scores"].items(), key=lambda x: x[1], reverse=True)
    for op, score in sorted_ops[:15]:
        freq = results["global_freq"][op]
        cyc = results["avg_cycles"][op]
        print(f"  {op:<10s} {freq:>7,} {cyc:>7.1f} {score:>9.0f}")

    # Hot sequences
    print(f"\n─── Hot Bigrams (>100 occurrences): {len(results['hot_bigrams'])} ───")
    for (a, b), cnt in sorted(results["hot_bigrams"].items(), key=lambda x: x[1], reverse=True)[:10]:
        print(f"  {a:<8s} → {b:<8s}  ×{cnt:,}")

    print(f"\n─── Hot Trigrams (>100 occurrences): {len(results['hot_trigrams'])} ───")
    for (a, b, c), cnt in sorted(results["hot_trigrams"].items(), key=lambda x: x[1], reverse=True)[:10]:
        print(f"  {a:<8s} → {b:<8s} → {c:<8s}  ×{cnt:,}")

    # Top 10 candidates
    print("\n" + "=" * 72)
    print("  🏆 TOP 10 NEW OPCODE CANDIDATES")
    print("=" * 72)

    for i, c in enumerate(results["candidates"], 1):
        print(f"\n  #{i}  {c['name']}")
        print(f"      Pattern:     {c['pattern']}")
        print(f"      Frequency:   {c['freq']:,} occurrences")
        print(f"      Cycle saved: {c['savings']:,.0f} total")
        print(f"      Score:       {c['score']:,.0f}")
        print(f"      Why:         {c['justification']}")

    print("\n" + "=" * 72)
    print("  Analysis complete.")
    print("=" * 72)

# ── Main ────────────────────────────────────────────────────────────
def main():
    out_dir = "/tmp/opcode-utilization"
    os.makedirs(out_dir, exist_ok=True)

    print("Generating synthetic traces...")
    traces = {}
    for agent_type in ("navigator", "guardian", "communicator"):
        trace_text = gen_trace(agent_type, count=10000)
        trace_path = os.path.join(out_dir, f"trace_{agent_type}.log")
        with open(trace_path, "w") as f:
            f.write(trace_text + "\n")
        ops = parse_trace(trace_text)
        traces[agent_type] = ops
        print(f"  {agent_type}: {len(ops)} instructions → {trace_path}")

    print("\nAnalyzing traces...")
    results = analyze(traces)

    print_report(results)

    # Write results JSON-like summary
    summary_path = os.path.join(out_dir, "results_summary.txt")
    with open(summary_path, "w") as f:
        f.write("FLUX VM Opcode Utilization Analysis Results\n")
        f.write(f"Total instructions: {results['total_ops']:,}\n\n")
        for i, c in enumerate(results["candidates"], 1):
            f.write(f"#{i} {c['name']} | freq={c['freq']:,} | savings={c['savings']:,.0f} | score={c['score']:,.0f}\n")
            f.write(f"   {c['justification']}\n\n")
    print(f"\nSummary saved to {summary_path}")

if __name__ == "__main__":
    main()
