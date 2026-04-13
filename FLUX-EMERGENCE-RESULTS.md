# FLUX Emergence Experiments — Jetson Orin GPU (sm_87)

## Results Matrix

| Ver | Change | Clustering | Specialization | Efficiency | Verdict |
|-----|--------|-----------|----------------|-----------|---------|
| v1 | Baseline | 0.397 ✓ | 0.019 ✗ | 1.30x | WEAK |
| v2 | 10x coupling | 0.413 ✓ | 0.002 ✗ | 1.30x | FALSIFIED |
| v3 | Anti-convergence | 0.395 ✓ | 0.794 ✓ | 1.30x | PARTIAL |
| v4 | Behavioral roles | 0.409 ✓ | 0.794 ✓ | 1.30x | PARTIAL |
| v5 | Resource respawn | 0.415 ✓ | 0.794 ✓ | 1.00x | PARTIAL |
| v6 | A/B control | 0.419 ✓ | 0.712 ✓ | 1.11x | PARTIAL |

## Key Discoveries

1. **Anti-convergence drift is essential** — without it, coupling either does nothing (v1) or creates clones (v2)
2. **Strong coupling homogenizes** — 0.05 influence = identical role vectors within groups
3. **Moderate coupling (0.02) + drift (0.01) = genuine diversity** — 40x specialization improvement
4. **Specialization gives 11% fitness advantage** over uniform control (v6, A/B test)
5. **Agent/resource ratio matters** — 4096:256 saturates instantly; 1024:512 shows scarcity effects
6. **Interaction count** — v2 grid was broken (4K), random sampling gives 493K (realistic)

## Configuration
- 1024 agents, 512 resources, 500 ticks, 5 experiments, 4 archetypes
- Roles: explore(0), collect(1), communicate(2), defend(3)
- Behavioral effects: detection range, grab range, speed, energy boost, perturbation resistance
- Anti-convergence: drift non-dominant roles when similarity > 0.9
- Perturbation at tick 250 (energy halved, position randomized)

## Next Experiments
- v7: Message-passing (communicators share resource locations)
- v8: Territory defense (defenders claim regions, collectors gain bonus in territory)
- v9: Multi-population (isolated groups vs mixed groups)
