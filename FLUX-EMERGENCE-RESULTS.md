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

## Key Discoveries

1. **Anti-convergence drift is ESSENTIAL** — prevents clone formation, enables genuine diversity (40x specialization improvement v1→v3)
2. **Strong coupling HOMOGENIZES** — 0.05 influence = identical role vectors (v2 falsified)
3. **Moderate coupling (0.02) + drift (0.01) = diversity sweet spot**
4. **Scarcity amplifies specialization** — 11% advantage (abundant) vs 61% (scarce)
5. **Territory matters** — defenders boost collectors by 20% per nearby defender
6. **Message passing needs scarcity** — tips worthless when resources are easy to find
7. **Agent/resource ratio is the key variable** — 8:1 (abundant) vs 4:1 (scarce) changes everything

## Confirmed Mechanisms (v8)
- **Explore role**: detection range 0.03-0.07 (vs control 0.05)
- **Collect role**: grab range 0.02-0.04, +50% value bonus
- **Communicate role**: broadcasts resource locations to neighbors within 0.06
- **Defend role**: perturbation resistance + territory collection boost (20% per defender)
- **Anti-convergence**: random drift on non-dominant roles when similarity > 0.9

## Falsified Hypotheses
- v2: Strong coupling improves specialization → FALSIFIED (creates clones)
- v5: Resource respawn improves efficiency → FALSIFIED (same ratio)
- v7: Message passing helps in abundance → FALSIFIED (tips unnecessary)
