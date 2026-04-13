/*
CUDA Simulation Experiment v67: Stigmergy with Pheromone Trails
Testing: Agents leave pheromone markers at collected resource locations that decay over time.
Prediction: Pheromones will enhance specialist coordination, increasing their advantage over uniform agents.
Baseline: v8 mechanisms (scarcity, territory, communication) included.
Novelty: Pheromone trails that agents can detect and follow.
*/
#include <cstdio>
#include <cstdlib>
#include <cmath>

#define AGENTS 1024
#define RESOURCES 128
#define TICKS 500
#define ARCHETYPES 4
#define PHEROMONE_GRID_SIZE 256
#define PHEROMONE_DECAY 0.97f

// Linear Congruential Generator
__device__ __host__ unsigned int lcg(unsigned int &state) {
    state = state * 1664525u + 1013904223u;
    return state;
}
__device__ __host__ float lcgf(unsigned int &state) {
    return (lcg(state) & 0xFFFFFF) / 16777216.0f;
}

struct Agent {
    float x, y;
    float vx, vy;
    float energy;
    float role[ARCHETYPES];
    float fitness;
    int arch;
    unsigned int rng;
};

struct Resource {
    float x, y;
    float value;
    bool collected;
};

struct Pheromone {
    float intensity;
    float arch_contrib[ARCHETYPES];
};

__global__ void init_agents(Agent *agents, Pheromone *pheromone) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= AGENTS) return;
    
    unsigned int seed = idx * 17 + 12345;
    agents[idx].x = lcgf(seed);
    agents[idx].y = lcgf(seed);
    agents[idx].vx = lcgf(seed) * 0.02f - 0.01f;
    agents[idx].vy = lcgf(seed) * 0.02f - 0.01f;
    agents[idx].energy = 1.0f;
    agents[idx].fitness = 0.0f;
    agents[idx].rng = idx * 8191 + 1;
    
    // Specialized vs uniform control groups
    if (idx < AGENTS/2) {
        // Specialized: role[arch]=0.7, others 0.1
        agents[idx].arch = idx % ARCHETYPES;
        for (int i = 0; i < ARCHETYPES; i++) {
            agents[idx].role[i] = (i == agents[idx].arch) ? 0.7f : 0.1f;
        }
    } else {
        // Uniform control: all roles 0.25
        agents[idx].arch = ARCHETYPES; // mark as uniform
        for (int i = 0; i < ARCHETYPES; i++) {
            agents[idx].role[i] = 0.25f;
        }
    }
}

__global__ void init_resources(Resource *resources) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= RESOURCES) return;
    
    unsigned int seed = idx * 23 + 67890;
    resources[idx].x = lcgf(seed);
    resources[idx].y = lcgf(seed);
    resources[idx].value = 0.8f + lcgf(seed) * 0.4f;
    resources[idx].collected = false;
}

__global__ void init_pheromone(Pheromone *pheromone) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= PHEROMONE_GRID_SIZE * PHEROMONE_GRID_SIZE) return;
    
    pheromone[idx].intensity = 0.0f;
    for (int i = 0; i < ARCHETYPES; i++) {
        pheromone[idx].arch_contrib[i] = 0.0f;
    }
}

__device__ int pheromone_grid_index(float x, float y) {
    int ix = (int)(x * PHEROMONE_GRID_SIZE);
    int iy = (int)(y * PHEROMONE_GRID_SIZE);
    ix = max(0, min(PHEROMONE_GRID_SIZE-1, ix));
    iy = max(0, min(PHEROMONE_GRID_SIZE-1, iy));
    return iy * PHEROMONE_GRID_SIZE + ix;
}

__global__ void tick(Agent *agents, Resource *resources, Pheromone *pheromone, int tick_id) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= AGENTS) return;
    
    Agent &a = agents[idx];
    
    // Energy decay
    a.energy *= 0.999f;
    
    // Anti-convergence: detect similarity > 0.9 and apply drift
    float role_sum = 0.0f;
    float max_role = 0.0f;
    int max_idx = 0;
    for (int i = 0; i < ARCHETYPES; i++) {
        role_sum += a.role[i];
        if (a.role[i] > max_role) {
            max_role = a.role[i];
            max_idx = i;
        }
    }
    float similarity = max_role / (role_sum / ARCHETYPES);
    if (similarity > 0.9f) {
        // Apply random drift to non-dominant roles
        for (int i = 0; i < ARCHETYPES; i++) {
            if (i != max_idx) {
                a.role[i] += lcgf(a.rng) * 0.02f - 0.01f;
                a.role[i] = max(0.01f, min(0.99f, a.role[i]));
            }
        }
        // Renormalize
        float sum = 0.0f;
        for (int i = 0; i < ARCHETYPES; i++) sum += a.role[i];
        for (int i = 0; i < ARCHETYPES; i++) a.role[i] /= sum;
    }
    
    // Movement with pheromone influence
    float dx = 0.0f, dy = 0.0f;
    
    // Sample pheromone in neighborhood
    float max_ph = 0.0f;
    float ph_dir_x = 0.0f, ph_dir_y = 0.0f;
    for (int dyi = -1; dyi <= 1; dyi++) {
        for (int dxi = -1; dxi <= 1; dxi++) {
            float sx = a.x + dxi * 0.02f;
            float sy = a.y + dyi * 0.02f;
            if (sx < 0 || sx >= 1 || sy < 0 || sy >= 1) continue;
            
            int pi = pheromone_grid_index(sx, sy);
            float ph_val = pheromone[pi].intensity;
            // Weight by archetype match if specialized
            if (a.arch < ARCHETYPES) {
                ph_val *= pheromone[pi].arch_contrib[a.arch];
            }
            
            if (ph_val > max_ph) {
                max_ph = ph_val;
                ph_dir_x = dxi * 0.01f;
                ph_dir_y = dyi * 0.01f;
            }
        }
    }
    
    // Combine random movement with pheromone attraction
    dx = a.vx + ph_dir_x * a.role[0] * 2.0f;  // explore role amplifies pheromone response
    dy = a.vy + ph_dir_y * a.role[0] * 2.0f;
    
    // Update position
    a.x += dx;
    a.y += dy;
    
    // Boundary check
    if (a.x < 0) { a.x = 0; a.vx = fabsf(a.vx); }
    if (a.x >= 1) { a.x = 0.999f; a.vx = -fabsf(a.vx); }
    if (a.y < 0) { a.y = 0; a.vy = fabsf(a.vy); }
    if (a.y >= 1) { a.y = 0.999f; a.vy = -fabsf(a.vy); }
    
    // Resource interaction
    float detect_range = 0.03f + a.role[0] * 0.04f;  // explore role
    float grab_range = 0.02f + a.role[1] * 0.02f;    // collect role
    float best_dist = 1e6;
    int best_res = -1;
    
    for (int i = 0; i < RESOURCES; i++) {
        Resource &r = resources[i];
        if (r.collected) continue;
        
        float dist = hypotf(a.x - r.x, a.y - r.y);
        if (dist < detect_range && dist < best_dist) {
            best_dist = dist;
            best_res = i;
        }
    }
    
    if (best_res != -1 && best_dist < grab_range) {
        Resource &r = resources[best_res];
        
        // Collect resource
        float value = r.value;
        // Collect role bonus
        value *= (1.0f + a.role[1] * 0.5f);
        
        // Territory bonus from defenders
        int nearby_defenders = 0;
        for (int j = 0; j < AGENTS; j++) {
            if (j == idx) continue;
            Agent &other = agents[j];
            float dist = hypotf(a.x - other.x, a.y - other.y);
            if (dist < 0.06f && other.arch == a.arch) {
                nearby_defenders++;
            }
        }
        value *= (1.0f + nearby_defenders * 0.2f);
        
        a.energy += value;
        a.fitness += value;
        r.collected = true;
        
        // Leave pheromone at collection site
        int pi = pheromone_grid_index(r.x, r.y);
        atomicAdd(&pheromone[pi].intensity, 1.0f);
        if (a.arch < ARCHETYPES) {
            atomicAdd(&pheromone[pi].arch_contrib[a.arch], 1.0f);
        }
    }
    
    // Communication role: broadcast nearest resource
    if (a.role[2] > 0.3f && best_res != -1) {
        Resource &r = resources[best_res];
        for (int j = 0; j < AGENTS; j++) {
            if (j == idx) continue;
            Agent &other = agents[j];
            float dist = hypotf(a.x - other.x, a.y - other.y);
            if (dist < 0.06f && lcgf(a.rng) < a.role[2]) {
                // Influence neighbor's velocity toward resource
                float influence = 0.01f * a.role[2];
                other.vx += (r.x - other.x) * influence;
                other.vy += (r.y - other.y) * influence;
            }
        }
    }
    
    // Perturbation (every 50 ticks)
    if (tick_id % 50 == 0) {
        // Defenders resist perturbation
        if (lcgf(a.rng) > a.role[3] * 0.8f) {
            a.energy *= 0.5f;
            a.vx += lcgf(a.rng) * 0.02f - 0.01f;
            a.vy += lcgf(a.rng) * 0.02f - 0.01f;
        }
    }
    
    // Velocity damping
    a.vx *= 0.95f;
    a.vy *= 0.95f;
}

__global__ void decay_pheromone(Pheromone *pheromone) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= PHEROMONE_GRID_SIZE * PHEROMONE_GRID_SIZE) return;
    
    pheromone[idx].intensity *= PHEROMONE_DECAY;
    for (int i = 0; i < ARCHETYPES; i++) {
        pheromone[idx].arch_contrib[i] *= PHEROMONE_DECAY;
    }
}

__global__ void respawn_resources(Resource *resources, int tick_id) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= RESOURCES) return;
    
    // Respawn every 50 ticks
    if (tick_id % 50 == 0) {
        unsigned int seed = idx * 19 + tick_id;
        resources[idx].x = lcgf(seed);
        resources[idx].y = lcgf(seed);
        resources[idx].value = 0.8f + lcgf(seed) * 0.4f;
        resources[idx].collected = false;
    }
}

int main() {
    // Allocate memory
    Agent *agents;
    Resource *resources;
    Pheromone *pheromone;
    
    cudaMallocManaged(&agents, AGENTS * sizeof(Agent));
    cudaMallocManaged(&resources, RESOURCES * sizeof(Resource));
    cudaMallocManaged(&pheromone, PHEROMONE_GRID_SIZE * PHEROMONE_GRID_SIZE * sizeof(Pheromone));
    
    // Initialize
    init_agents<<<(AGENTS+255)/256, 256>>>(agents, pheromone);
    init_resources<<<(RESOURCES+255)/256, 256>>>(resources);
    init_pheromone<<<(PHEROMONE_GRID_SIZE*PHEROMONE_GRID_SIZE+255)/256, 256>>>(pheromone);
    cudaDeviceSynchronize();
    
    // Run simulation
    for (int t = 0; t < TICKS; t++) {
        tick<<<(AGENTS+255)/256, 256>>>(agents, resources, pheromone, t);
        decay_pheromone<<<(PHEROMONE_GRID_SIZE*PHEROMONE_GRID_SIZE+255)/256, 256>>>(pheromone);
        respawn_resources<<<(RESOURCES+255)/256, 256>>>(resources, t);
        cudaDeviceSynchronize();
    }
    
    // Calculate results
    float spec_fitness = 0.0f, unif_fitness = 0.0f;
    float spec_energy = 0.0f, unif_energy = 0.0f;
    
    for (int i = 0; i < AGENTS; i++) {
        if (agents[i].arch < ARCHETYPES) { // Specialized
            spec_fitness += agents[i].fitness;
            spec_energy += agents[i].energy;
        } else { // Uniform
            unif_fitness += agents[i].fitness;
            unif_energy += agents[i].energy;
        }
    }
    
    spec_fitness /= (AGENTS/2);
    unif_fitness /= (AGENTS/2);
    spec_energy /= (AGENTS/2);
    unif_energy /= (AGENTS/2);
    
    // Print results
    printf("=== EXPERIMENT v67: Stigmergy with Pheromone Trails ===\n");
    printf("Configuration: %d agents, %d resources, %d ticks\n", AGENTS, RESOURCES, TICKS);
    printf("Specialized agents: role[arch]=0.7, others=0.1\n");
    printf("Uniform agents: all roles=0.25\n");
    printf("Novel mechanism: Pheromone trails at collection sites (grid=%d^2, decay=%.2f)\n", 
           PHEROMONE_GRID_SIZE, PHEROMONE_DECAY);
    printf("\n");
    printf("RESULTS:\n");
    printf("Specialized avg fitness: %.4f\n", spec_fitness);
    printf("Uniform avg fitness:     %.4f\n", unif_fitness);
    printf("Ratio (spec/unif):       %.3fx\n", spec_fitness / unif_fitness);
    printf("\n");
    printf("Specialized avg energy:  %.4f\n", spec_energy);
    printf("Uniform avg energy:      %.4f\n", unif_energy);
    printf("Energy ratio:            %.3fx\n", spec_energy / unif_energy);
    printf("\n");
    
    // Final assessment
    float ratio = spec_fitness / unif_fitness;
    printf("PREDICTION: Pheromones enhance specialist coordination.\n");
    printf("BASELINE v8 (no pheromones): 1.61x advantage\n");
    printf("v67 WITH pheromones: %.3fx advantage\n", ratio);
    
    if (ratio > 1.61f) {
        printf("CONCLUSION: CONFIRMED - Pheromones increase specialist advantage by %.1f%%\n", 
               (ratio - 1.61f) / 1.61f * 100);
    } else if (ratio < 1.61f) {
        printf("CONCLUSION: FALSIFIED - Pheromones reduce specialist advantage by %.1f%%\n", 
               (1.61f - ratio) / 1.61f * 100);
    } else {
        printf("CONCLUSION: INCONCLUSIVE - No significant change\n");
    }
    
    // Cleanup
    cudaFree(agents);
    cudaFree(resources);
    cudaFree(pheromone);
    
    return 0;
}
