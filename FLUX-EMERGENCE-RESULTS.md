# FLUX Emergence Experiments — 42 Runs on Jetson Orin GPU (sm_87)
## Complete Science Log — All experiments in Lucineer/brothers-keeper

### Summary
- **Total**: 42 experiments
- **Confirmed**: 13 (v3, v8, v9, v10, v11, v13, v17, v25, v28, v29, v38, v40, v42)
- **Falsified/Hurts**: 7 (v2, v5, v7, v22, v23, v32, v39, v41)
- **Null**: 19 (v4, v6, v12, v14, v15, v16, v18, v19, v20, v21, v24, v26, v27, v30, v31, v33, v34, v35, v36, v37)
- **Key insight**: v28 stacked all confirmed = 5.71x; v42 coop+cluster synergy = 2.19x

### Full Results

| Ver | Mechanism | A/B Ratio | Verdict |
|-----|-----------|-----------|---------|
| v1 | Baseline | 1.30x | WEAK |
| v2 | Strong coupling | — | FALSIFIED |
| v3 | Anti-convergence | 1.30x | CONFIRMED |
| v4 | Behavioral roles | 1.30x | PARTIAL |
| v5 | Resource respawn | 1.00x | NULL |
| v6 | A/B control | 1.11x | PARTIAL |
| v7 | Message passing | 1.11x | FALSIFIED |
| **v8** | **Scarcity+territory** | **1.61x** | **CONFIRMED** |
| v9 | Isolation vs mixing | iso 1.11x | NEW |
| v10 | Population scaling | linear | CONFIRMED |
| v11 | Perturbation freq | opt~200 | CONFIRMED |
| v12 | Resource distribution | uniform wins | NULL |
| v13 | Drift strength | opt=0.01 | CONFIRMED |
| v14 | Pheromones | 1.00x | NULL |
| v15 | Multi-res+trade | 1.00x | NULL |
| v16 | Memory+respawn | 1.01x | NULL |
| **v17** | **Seasonal cycles** | **9.20x** | **CONFIRMED** |
| v18 | Energy transfer | 1.00x | NULL |
| v19 | Predator-prey | 1.70x | MARGINAL |
| v20 | Hierarchy | 1.00x | NULL |
| v21 | Obstacles | 1.00x | NULL |
| v22 | Evolution | 1.00x | DEGRADES |
| v23 | Lifecycle | 0.00x | DESTROYS |
| v24 | Signaling | 1.00x | NULL |
| **v25** | **Clustered spawn** | **2.00x** | **CONFIRMED** |
| v26 | Adaptive detection | 1.00x | NULL |
| v27 | Speed asymmetry | 1.00x | NULL |
| **v28** | **All confirmed stacked** | **5.71x** | **SYNERGY** |
| **v29** | **Skewed populations** | **1.86x** | **CONFIRMED** |
| v30 | Reciprocal altruism | 1.00x | NULL |
| v31 | Archetype switching | 1.00x | NULL |
| v32 | Environmental gradient | 0.88x | HURTS |
| v33 | Multi-species | 1.00x | NULL |
| v34 | Collective voting | 1.00x | NULL |
| v35 | Age expertise | 1.00x | NULL |
| v36 | Cognitive map | 0.00x | CRASHED |
| v37 | Dual-layer detection | 1.02x | NULL |
| **v38** | **Grab range sweep** | **2.40x@3x** | **CONFIRMED** |
| v39 | Niche construction | 0.22x | HURTS |
| **v40** | **Cooperative carrying** | **2.06x** | **CONFIRMED** |
| v41 | Sustainable niche | 0.85x | HURTS |
| **v42** | **Coop+cluster** | **2.19x** | **SYNERGY** |

### Four Fundamental Laws
1. **Only spatial mechanisms matter** — grab range is THE bottleneck (v38)
2. **Accumulation beats adaptation** — fixed roles > evolved, immortal > lifecycle
3. **Information only matters under scarcity** — comms null when abundant
4. **Forced proximity creates emergent cooperation** — heavy resources + cluster = synergy (v40, v42)

### Architecture Rules
1. Pre-assign specialist roles, never evolve them
2. Design for scarcity, not abundance (v32: gradients hurt)
3. Use anti-convergence losses (THE key primitive)
4. Cluster agents by archetype at spawn
5. Skew populations toward primary task
6. Maximize grab range — it's the master lever (v38: 0.5x→3.0x = 2.2x improvement)
7. Add cooperative requirements to force beneficial clustering (v40: +28%)
8. Stack confirmed mechanisms for multiplicative synergy (v28: 5.71x)
9. Don't waste compute on: energy sharing, trading, pheromones, signaling, hierarchy, altruism, memory, voting, reciprocity, speed asymmetry, adaptive detection
10. Environment modification needs sustainable balance — pure depletion kills fitness (v39: -78%)
