# FLUX Emergence Experiments — Jetson Orin GPU (sm_87)
## 24 Experiments — Complete Science Log

### Results Matrix

| Ver | Mechanism | Ratio | Spec | Verdict |
|-----|-----------|-------|------|---------|
| v1 | Baseline | 1.30x | 0.019 | WEAK |
| v2 | Strong coupling | — | 0.002 | **FALSIFIED** |
| v3 | Anti-convergence | 1.30x | 0.794 | **KEY** (40x spec) |
| v4 | Behavioral roles | 1.30x | 0.794 | PARTIAL |
| v5 | Resource respawn | 1.00x | 0.794 | **FALSIFIED** |
| v6 | A/B control | 1.11x | 0.712 | PARTIAL |
| v7 | Message passing | 1.11x | 0.710 | **FALSIFIED** (abundance) |
| **v8** | **Scarcity+territory** | **1.61x** | **0.711** | **CONFIRMED** |
| v9 | Isolation vs mixing | iso 1.11x | 1.000 | NEW |
| v10 | Population scaling | 1.61x | — | SCALES |
| v11 | Perturbation freq | 1.61x | — | ROBUST |
| v12 | Resource dist | 1.61/1.49 | — | Uniform > power |
| v13 | Drift strength | — | 0.711-716 | ROBUST |
| v14 | Pheromones | 1.61x | — | **NULL** |
| v15 | Multi-res trade | 1.11x | — | **NULL** |
| v16 | Memory+respawn | 1.01x | — | **NULL** |
| **v17** | **Seasonal cycles** | **9.20x** | 0.132 | **CONFIRMED** |
| v18 | Energy transfer | 1.61x | — | **NULL** |
| v19 | Predator-prey | 1.70x | 0.713 | MARGINAL (+5%) |
| v20 | Hierarchy | 1.61x | — | **NULL** |
| v21 | Obstacles | 1.61x | — | **NULL** |
| v22 | Evolution | 1.61x | 0.512 | **DEGRADES** spec |
| v23 | Lifecycle | 0.00x | — | **DESTROYS** fitness |
| v24 | Signaling | 1.61x | — | **NULL** |

### Grand Pattern (24 experiments)
1. **Only spatial parameters matter**: detection range, grab range, movement speed
2. **Energy is never the bottleneck**: sharing, trading, altruism all = zero
3. **Information only helps under scarcity**: comms null in abundance, confirmed in scarcity
4. **Accumulation beats adaptation**: lifecycle kills (reset=0), respawn kills pressure
5. **Pre-set roles beat evolved**: mutation/selection degrades spec by 28%
6. **Anti-convergence is THE primitive**: everything else is secondary
7. **Scarcity amplifies everything**: 11% → 61% advantage
8. **Seasonal pressure = largest single effect**: 9.2x cumulative

### Architecture Rules for Agent Fleets
1. Pre-assign specialist roles, don't evolve them
2. Design for scarcity, not abundance
3. Use anti-convergence losses
4. Isolate sub-populations periodically
5. Don't waste compute on energy/resource sharing
6. Spatial locality > information content
7. Avoid lifecycle/respawn (destroys accumulation)
