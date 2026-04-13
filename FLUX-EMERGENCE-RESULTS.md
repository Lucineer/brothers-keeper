# FLUX Emergence Experiments — 31 Runs on Jetson Orin GPU (sm_87)
## Complete Science Log — All experiments pushed to Lucineer/brothers-keeper

### Summary Statistics
- **Total experiments**: 31
- **Confirmed**: 9 (v3, v8, v9, v10, v11, v13, v17, v25, v28, v29)
- **Falsified**: 6 (v2, v5, v7, v22, v23)
- **Null**: 12 (v4, v6, v12, v14, v15, v16, v18, v20, v21, v24, v26, v27, v30, v31)
- **Marginal**: 2 (v19)
- **Key finding**: v28 stacked synergy = 5.71x control

### Results Matrix

| Ver | Mechanism | A/B Ratio | Spec | Verdict |
|-----|-----------|-----------|------|---------|
| v1 | Baseline | 1.30x | 0.019 | WEAK |
| v2 | Strong coupling | — | 0.002 | FALSIFIED |
| v3 | Anti-convergence | 1.30x | 0.794 | **CONFIRMED** |
| v4 | Behavioral roles | 1.30x | 0.794 | PARTIAL |
| v5 | Resource respawn | 1.00x | 0.794 | NULL |
| v6 | A/B control test | 1.11x | 0.712 | PARTIAL |
| v7 | Message passing | 1.11x | 0.710 | FALSIFIED |
| **v8** | **Scarcity+territory** | **1.61x** | **0.711** | **CONFIRMED** |
| v9 | Isolation vs mixing | iso 1.11x | 1.000 | NEW |
| v10 | Population scaling | linear | — | CONFIRMED |
| v11 | Perturbation freq | optimal~200 | — | CONFIRMED |
| v12 | Resource distribution | uniform > power | — | NULL |
| v13 | Drift strength sweep | optimal=0.01 | — | CONFIRMED |
| v14 | Pheromones | 1.00x | — | NULL |
| v15 | Multi-res + trading | 1.00x | — | NULL |
| v16 | Memory + respawn | 1.01x | — | NULL |
| **v17** | **Seasonal cycles** | **9.20x** | 0.132 | **CONFIRMED** |
| v18 | Energy transfer | 1.00x | — | NULL |
| v19 | Predator-prey | 1.70x | — | MARGINAL |
| v20 | Hierarchical control | 1.00x | — | NULL |
| v21 | Spatial obstacles | 1.00x | — | NULL |
| v22 | Evolution (mut+select) | 1.00x | 0.512↓ | DEGRADES |
| v23 | Birth/death lifecycle | 0.00x | — | DESTROYS |
| v24 | Signaling | 1.00x | — | NULL |
| **v25** | **Clustered spawn** | **2.00x** | — | **CONFIRMED** |
| v26 | Adaptive detection | 1.00x | — | NULL |
| v27 | Speed asymmetry | 1.00x | — | NULL |
| **v28** | **Combined best** | **5.71x** | 0.668 | **SYNERGY** |
| **v29** | **Skewed populations** | **1.86x** | — | **CONFIRMED** |
| v30 | Reciprocal altruism | 1.00x | — | NULL |
| v31 | Archetype switching | 1.00x | 0.713 | NULL |

### Three Fundamental Laws
1. **Only spatial mechanisms matter** — detection range, grab range, movement speed, spawn density
2. **Accumulation beats adaptation** — fixed roles > evolved, immortal > lifecycle, pre-set > adaptive
3. **Information only matters under scarcity** — comms null when abundant, marginal when scarce

### Architecture Rules for Agent Fleets
1. Pre-assign specialist roles, never evolve them
2. Design for scarcity, not abundance
3. Use anti-convergence losses (THE key primitive)
4. Cluster agents by archetype at spawn
5. Skew populations toward primary task (collectors > explorers > comms > defenders)
6. Stack confirmed mechanisms for multiplicative synergy (v28: 5.71x)
7. Seasonal pressure = largest single effect (9.2x cumulative)
8. Don't waste compute on: energy sharing, trading, pheromones, signaling, hierarchy, altruism, memory
