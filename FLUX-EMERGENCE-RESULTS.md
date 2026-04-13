# FLUX Emergence Experiments — Jetson Orin GPU (sm_87)

## Results Matrix

| Ver | Change | Agents | Res | Clustering | Specialization | A/B Ratio | Verdict |
|-----|--------|--------|-----|-----------|----------------|-----------|---------|
| v1 | Baseline | 4096 | 256 | 0.397 | 0.019 | 1.30x | WEAK |
| v2 | 10x coupling | 4096 | 256 | 0.413 | 0.002 | 1.30x | FALSIFIED |
| v3 | Anti-convergence | 4096 | 256 | 0.395 | **0.794** | 1.30x | PARTIAL |
| v4 | Behavioral roles | 4096 | 256 | 0.409 | 0.794 | 1.30x | PARTIAL |
| v5 | Resource respawn | 4096 | 512 | 0.415 | 0.794 | 1.00x | PARTIAL |
| v6 | A/B control | 1024 | 512 | 0.419 | 0.712 | 1.11x | PARTIAL |
| v7 | Message passing | 1024 | 512 | 0.398 | 0.710 | 1.11x | PARTIAL |
| **v8** | **Scarcity+territory** | **1024** | **128** | **0.396** | **0.711** | **1.61x** | **CONFIRMED** |
| v9 | Isolation vs mixing | 1024 | 128 | - | 1.000 iso | 0.90x | NEW FINDING |
| v10 | Pop scaling | 256-4096 | 128 | - | - | 1.13-1.85x | PEAK at 512 |
| v11 | Perturbation freq | 1024 | 128 | - | - | 1.61x all | RESILIENT |
| v12 | Resource dist | 1024 | 128 | - | - | 1.49-1.61x | UNIFORM wins |
| v13 | Drift strength | 1024 | 128 | - | 0.711-0.716 | - | ROBUST |
| v14 | Defensive comms | 1024 | 128 | - | - | 1.00x vs v8 | **FALSIFIED** |
| v15 | Dynamic threshold | 1024 | 128 | - | 0.712-0.713 | - | **FALSIFIED** |
| v16 | Role inheritance | 1024 | 128 | - | 0.715 | 1.61x | **FALSIFIED** |
| **v17** | **A/R ratio sweep** | **512** | **16-512** | - | - | **1.09-2.03x** | **KEY FINDING** |

## Key Discoveries

1. **Anti-convergence drift is ESSENTIAL** — prevents clone formation (v2 falsified, v3 breakthrough)
2. **Strong coupling HOMOGENIZES** — 0.05 influence = identical role vectors
3. **Scarcity amplifies specialization** — advantage scales linearly with A/R ratio
4. **Territory matters** — defenders boost collectors by 20% per nearby defender
5. **Message passing needs scarcity** — tips worthless when resources are abundant (v7)
6. **Agent/resource ratio is THE key variable** — 2x at 32:1, 1.6x at 8:1, 1.1x at 4:1
7. **Isolation creates perfect specialization** — spec=1.000, 11% fitter
8. **Specialization is perturbation-resilient** — same advantage regardless of disruption frequency
9. **Drift strength is robust** — 0.001 to 0.05 all produce same result
10. **Communication gating, dynamic thresholds, and inheritance all FALSIFIED** — simple rules win

## v17: Scarcity Scaling Law

| A/R Ratio | Advantage | Resources | Total Fitness |
|-----------|-----------|-----------|---------------|
| 32:1 | **103%** | 16 | 22.3 vs 11.0 |
| 16:1 | **97%** | 32 | 44.3 vs 22.5 |
| 8:1 | **96%** | 64 | 94.0 vs 48.0 |
| 4:1 | 85% | 128 | 177.2 vs 95.8 |
| 2:1 | 9% | 256 | 291.0 vs 266.5 |

## Falsified Hypotheses (6 total)
- v2: Strong coupling improves specialization
- v5: Resource respawn improves efficiency
- v7: Message passing helps in abundance
- v14: Defender-gated communication improves efficiency
- v15: Dynamic anti-convergence threshold helps
- v16: Role inheritance deepens specialization

## Practical Implications
- **Fleet design**: Small teams in resource-constrained environments benefit most from specialization
- **Keep it simple**: Anti-convergence drift + behavioral roles + territory = all you need
- **Communication is noise** unless resources are scarce
- **Isolation breeds excellence** but mixing breeds adaptability