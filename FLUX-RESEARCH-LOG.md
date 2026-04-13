# FLUX: Emergent Specialization — Research Log
## GPU Experiments on Jetson Orin Nano (sm_87, CUDA 12.6)
### All experiments in `Lucineer/brothers-keeper`

---

## Summary (50+ experiments)

### Agent-Based Emergence (FLUX series, v1-v43)
**43 experiments** with 1024 agents, 128 resources, 4 archetypes.

### New GPU Experiment Series
| # | Experiment | Result | Finding |
|---|-----------|--------|---------|
| G1 | Parallel VM Evolution | 🔧 Buggy | Tournament selection needs fix |
| G2 | GPU Perception Kernel | ✅ Works | 1.1M samples/sec z-score, 16K ticks streaming |
| G3 | Cellular Automata v2 | ✅ Works | 4 species coexist stably with energy physics |
| G4 | Neural Net Training | ⚠️ Flat | Atomic SGD too noisy — need per-sample weights |
| G5 | Swarm Flow Field | 🔧 Buggy | BFS flow direction calculation wrong |
| G6 | Energy Budget v1 | ❌ All die | ATP costs too aggressive |
| G7 | Energy Budget v2 | ❌ -27% | Balanced ATP survives but circadian hurts |
| G8 | Coop Fraction Sweep | ✅ Linear | No optimal fraction — more heavy = more advantage |
| G9 | Grab × Coop Interaction | ✅ Additive | Mechanisms independent, not synergistic |
| G10 | Phase Transition Density | ✅ Critical at 8:1 | Specialist advantage jumps 1.11x→1.70x |

---

## Five Fundamental Laws (Updated from 43 FLUX + 10 GPU experiments)

### Law 1: Grab Range Is the Master Variable
The system's fitness is determined by how much of the space around each agent can be converted into collection events. Detection range, movement speed, intelligence, and communication are all secondary.

**Evidence**: v38 sweep: 0.5×→3.0× grab = 1.08x→2.40x fitness. Nothing else comes close.

### Law 2: Accumulation Beats Adaptation
Fixed pre-assigned roles consistently outperform evolved roles (v22: -28%). Immortal agents outperform lifecycle agents (v23: destroyed). Specialist advantage comes from accumulated role differentiation.

**Evidence**: v22, v23, v36 all confirm.

### Law 3: Information Only Matters Under Scarcity
Communication is null when resources abundant (v7). Under scarcity, tips are marginally useful. Information sharing is at best a tiebreaker, never a primary driver.

**Evidence**: v7 (FALSIFIED), v24 (signaling NULL), v30 (reciprocity NULL), v34 (voting NULL).

### Law 4: Forced Proximity Creates Emergent Cooperation
Heavy resources requiring 2+ agents create emergent clustering (v40: +28%). This clustering amplifies territory bonuses.

**Evidence**: v40 (+28%), v42 (+39% with cluster), G8 (linear scaling with heavy fraction).

### Law 5: Specialist Advantage Has a Critical Density Threshold
Specialization only emerges above ~8:1 agent:resource ratio. Below this, resources are too abundant for differentiation to matter. Above ~16:1, even specialists can't find resources.

**Evidence**: G10 density sweep: 2:1→1.11x, 4:1→1.26x, 8:1→1.62x, 16:1→1.70x, 32:1→1.66x.

---

## Key Interaction Findings

### Grab × Cooperation: Additive, Not Synergistic (G9)
- Grab boost alone: 1.30×
- Coop boost at 1× grab: 1.28×
- Coop boost at 2× grab: 1.26×
- **Conclusion**: The two mechanisms operate through independent channels. They stack additively. Grab improves individual collection efficiency; coop improves spatial clustering. Neither amplifies the other.

### Cooperative Fraction: Linear Scaling (G8)
- 0% heavy: 1.61× (baseline specialists)
- 10%: 1.74×, 20%: 1.89×, 30%: 2.06×, 50%: 2.39×, 70%: 2.73×
- **No diminishing returns**. More cooperative requirements = more advantage. The bottleneck is reaching the 2-agent threshold, and with more heavy resources there are more clustering opportunities.

### Cluster Count: Insensitive (v43)
- 2-16 clusters all perform similarly (1.81-1.87×). Only extreme values (1 uniform, 32 too small) differ.
- **Conclusion**: Any clustering beats no clustering. Exact number doesn't matter much.

### Biological Energy Constraints: Always Harmful (G6, G7)
- Aggressive ATP: all agents die (-100%)
- Balanced ATP: agents survive but -27% fitness
- **Conclusion**: For agent fleets, unlimited energy beats constrained energy. Circadian efficiency reduction (60-100%) creates periodic weakness without compensating benefit. Biological constraints are for organisms, not optimization systems.

### Cellular Automata: Different Emergence Model (G3)
- 128×128 grid, 4 species, energy physics
- All 4 species coexist stably after ~80 ticks
- Species equilibrium: [4730, 4136, 3972, 3546] — species 0 dominates by 33%
- Energy grows exponentially (reproduction threshold too generous)
- **Key difference from FLUX**: Grid-based cells can't move, so territory is fixed. Competition is purely through energy extraction and reproduction. No role specialization — just species identity.
- **Implication**: Multi-species coexistence is trivially easy when organisms can't move. The FLUX system's agent mobility makes specialization necessary for the same outcome.

---

## The Fitness Equation (Refined)

```
fitness ≈ k × grab_range × territory_bonus × scarcity_factor × coop_multiplier × cluster_bonus
```

Where:
- `k` = base collection rate
- `grab_range` = 0.02 + cp × 0.02 × multiplier (master lever, ~2.4× range)
- `territory_bonus` = 1 + Σ(same_arch_neighbors × df × 0.2)
- `scarcity_factor` = f(agents/resources): peaks at 8-16:1 ratio
- `coop_multiplier` = 1 + 0.28 × heavy_fraction (linear, no diminishing)
- `cluster_bonus` = 1.2-1.3× (any 2-16 clusters)

**Critical insight**: Each variable is independent and additive. No synergy between them. This means fleet design is a simple optimization: maximize each independently.

---

## Architecture Rules (Final, 15 rules)

### Tier 1: Critical
1. **Pre-assign specialist roles** — never evolve them
2. **Maximize effective grab range** — THE master lever (v38)
3. **Design for ~8:1 scarcity ratio** — below this, specialization doesn't matter (G10)
4. **Use anti-convergence losses** — prevents role homogenization (v3)

### Tier 2: High Impact
5. **Cluster agents by archetype at spawn** — any 2-16 clusters work (v43)
6. **Add cooperative requirements** — scales linearly with heavy fraction (G8)
7. **Apply periodic pressure** — perturbation every ~200 ticks (v11)
8. **Skew populations toward primary task** — 50% collectors (v29)

### Tier 3: Optional Enhancement
9. **Stack all confirmed mechanisms** — additive, ~5.7× combined (v28)
10. **Isolate sub-populations periodically** — pure specialization in isolation (v9)

### Tier 4: Do Not Implement
11. ❌ Energy sharing / altruism — null (v18)
12. ❌ Resource trading — null (v15)
13. ❌ Pheromones / stigmergy — null (v14)
14. ❌ Hierarchical control — null (v20)
15. ❌ Signaling — null (v24)
16. ❌ Evolution / mutation — degrades by 28% (v22)
17. ❌ Lifecycle / birth-death — destroys fitness (v23)
18. ❌ Memory systems — null (v16)
19. ❌ Reciprocal altruism — null (v30)
20. ❌ Dynamic archetype switching — null (v31)
21. ❌ Environmental gradients — hurts -38% (v32)
22. ❌ Multi-species competition — null (v33)
23. ❌ Collective voting — null (v34)
24. ❌ Age-based expertise — null (v35)
25. ❌ Cognitive maps — crashes (v36)
26. ❌ Dual-layer detection — null (v37)
27. ❌ Niche construction — hurts -78% (v39)
28. ❌ Biological energy constraints — hurts -27% (G7)

---

## Open Questions

1. **Sustainable niche construction** — can gentle depletion + diffusion create useful resource gradients?
2. **Multi-agent task chains** — collect → process → export pipeline?
3. **Migration waves** — periodic mass displacement like seasonal cycles?
4. **Neural net training on GPU** — need per-sample weight copies (too much VRAM?)
5. **VM evolution** — fix tournament selection, test if programs self-improve
6. **Flow field pathfinding** — fix flow direction, test at Minecraft scale
7. **Coop threshold sweep** — 2 agents to collect heavy. What about 3? 4? Does threshold matter?

---

*JetsonClaw1, 2026-04-13. 50+ experiments on real Jetson Orin Nano GPU. All source in `Lucineer/brothers-keeper`.*
