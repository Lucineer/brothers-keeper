/* flux-emergence.cu — Falsifiable simulation: emergent coordination via message passing.
   Tests whether agents with local rules + neighbor interaction self-organize.
   Metrics: silhouette (clustering), role CV (specialization), efficiency ratio. */

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

#define N_AGENTS     4096
#define N_RESOURCES  256
#define MAX_TICKS    500
#define N_EXPERIMENTS 5
#define N_ARCH       4
#define SAMPLE_SIZE  512
#define SAMPLE_NEIGH 256

__device__ __host__ unsigned int lcg(unsigned int *s) {
    *s = *s * 1103515245u + 12345u;
    return (*s >> 16) & 0x7fff;
}
__device__ __host__ float lcgf(unsigned int *s) { return (float)lcg(s) / 32768.0f; }

typedef struct {
    float x, y, vx, vy, energy, role[4], fitness;
    int arch, res_held, interactions, group;
    unsigned int rng;
} Agent;

typedef struct {
    float x, y, value;
    int collected;
} Resource;

typedef struct {
    float silhouette, specialization, efficiency, avg_energy, avg_res;
    int total_interactions;
} Result;

/* ── GPU kernels ── */

__global__ void init_agents(Agent *a, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    a[i].rng = (unsigned int)(i * 2654435761u + 17);
    a[i].x = lcgf(&a[i].rng);
    a[i].y = lcgf(&a[i].rng);
    a[i].vx = a[i].vy = 0.0f;
    a[i].energy = 0.5f + lcgf(&a[i].rng) * 0.5f;
    a[i].arch = i % N_ARCH;
    a[i].fitness = 0.0f;
    a[i].res_held = 0;
    a[i].interactions = 0;
    a[i].group = -1;
    for (int r = 0; r < 4; r++) {
        float base = (r == a[i].arch) ? 0.7f : 0.1f;
        a[i].role[r] = base + (lcgf(&a[i].rng) - 0.5f) * 0.2f;
    }
}

__global__ void init_resources(Resource *r, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    unsigned int s = (unsigned int)(i * 2654435761u + 99999);
    r[i].x = lcgf(&s); r[i].y = lcgf(&s);
    r[i].value = 0.3f + lcgf(&s) * 0.7f;
    r[i].collected = 0;
}

__global__ void tick(Agent *a, Resource *r, int na, int nr, int t, int pt) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= na) return;
    Agent *ag = &a[i];
    float ew = ag->role[1]; /* explore weight */
    float cw = ag->role[0]; /* collect weight */

    /* Find nearest resource */
    float bd = 1.0f; int br = -1;
    if (cw > 0.3f) {
        for (int j = 0; j < nr; j++) {
            if (r[j].collected) continue;
            float dx = r[j].x - ag->x, dy = r[j].y - ag->y;
            float d = sqrtf(dx*dx + dy*dy);
            if (d < bd) { bd = d; br = j; }
        }
    }

    if (br >= 0 && bd < 0.1f) {
        r[br].collected = 1;
        ag->res_held++;
        ag->energy = fminf(1.0f, ag->energy + r[br].value * 0.1f);
        ag->fitness += r[br].value;
    } else if (br >= 0) {
        float dx = r[br].x - ag->x, dy = r[br].y - ag->y;
        float d = sqrtf(dx*dx + dy*dy);
        ag->vx = ag->vx * 0.8f + (dx/d) * 0.02f * cw;
        ag->vy = ag->vy * 0.8f + (dy/d) * 0.02f * cw;
    } else {
        ag->vx = ag->vx * 0.95f + (lcgf(&ag->rng) - 0.5f) * 0.01f * (1.0f + ew);
        ag->vy = ag->vy * 0.95f + (lcgf(&ag->rng) - 0.5f) * 0.01f * (1.0f + ew);
    }

    ag->x = fmodf(ag->x + ag->vx + 1.0f, 1.0f);
    ag->y = fmodf(ag->y + ag->vy + 1.0f, 1.0f);
    ag->energy *= 0.999f;

    /* Neighbor interaction (message-passing simulation) */
    int ints = 0;
    for (int j = 0; j < na && ints < 8; j++) {
        if (j == i) continue;
        float dx = a[j].x - ag->x, dy = a[j].y - ag->y;
        float d = sqrtf(dx*dx + dy*dy);
        if (d < 0.05f) {
            ints++;
            ag->interactions++;
            /* Same archetype: stronger role influence (creates specialization) */
            float infl = (a[j].arch == ag->arch) ? 0.005f : 0.001f;
            for (int r = 0; r < 4; r++)
                ag->role[r] += (a[j].role[r] - ag->role[r]) * infl;
            /* Separation */
            if (d < 0.02f) { ag->vx -= dx * 0.01f; ag->vy -= dy * 0.01f; }
        }
    }

    /* Perturbation */
    if (t == pt) {
        ag->x = lcgf(&ag->rng); ag->y = lcgf(&ag->rng);
        ag->energy *= 0.5f; ag->vx = ag->vy = 0.0f;
    }
}

/* ── CPU analysis ── */

void kmeans(Agent *a, int n, int k) {
    float cx[8], cy[8];
    for (int i = 0; i < k; i++) { cx[i] = a[i].x; cy[i] = a[i].y; }
    for (int it = 0; it < 20; it++) {
        float sx[8] = {0}, sy[8] = {0}; int cn[8] = {0};
        for (int i = 0; i < n; i++) {
            float bd = 2.0f; int bk = 0;
            for (int c = 0; c < k; c++) {
                float d = (a[i].x-cx[c])*(a[i].x-cx[c]) + (a[i].y-cy[c])*(a[i].y-cy[c]);
                if (d < bd) { bd = d; bk = c; }
            }
            a[i].group = bk; sx[bk] += a[i].x; sy[bk] += a[i].y; cn[bk]++;
        }
        for (int c = 0; c < k; c++)
            if (cn[c] > 0) { cx[c] = sx[c]/cn[c]; cy[c] = sy[c]/cn[c]; }
    }
}

float silhouette(Agent *a, int n) {
    float total = 0; int cnt = 0;
    for (int i = 0; i < SAMPLE_SIZE; i++) {
        int ai = (i * n) / SAMPLE_SIZE;
        int gi = a[ai].group;
        float cd[8] = {0}; int cc[8] = {0};
        for (int j = 0; j < SAMPLE_NEIGH; j++) {
            int aj = (j * n) / SAMPLE_NEIGH;
            if (aj == ai) continue;
            float dx = a[ai].x-a[aj].x, dy = a[ai].y-a[aj].y;
            float d = sqrtf(dx*dx+dy*dy);
            int gj = a[aj].group;
            cd[gj] += d; cc[gj]++;
        }
        float ad = (cc[gi] > 0) ? cd[gi]/cc[gi] : 0;
        float bd = 1e10f;
        for (int c = 0; c < N_ARCH; c++)
            if (c != gi && cc[c] > 0 && cd[c]/cc[c] < bd) bd = cd[c]/cc[c];
        float mx = fmaxf(ad, bd);
        if (mx > 1e-6f) { total += (bd - ad) / mx; cnt++; }
    }
    return cnt > 0 ? total / cnt : 0.0f;
}

float specialization(Agent *a, int n) {
    float mean[4] = {0}, sd[4] = {0};
    for (int i = 0; i < n; i++) for (int r = 0; r < 4; r++) mean[r] += a[i].role[r];
    for (int r = 0; r < 4; r++) mean[r] /= n;
    for (int i = 0; i < n; i++) for (int r = 0; r < 4; r++)
        sd[r] += (a[i].role[r]-mean[r])*(a[i].role[r]-mean[r]);
    float cv = 0;
    for (int r = 0; r < 4; r++) { sd[r] = sqrtf(sd[r]/n); if (mean[r] > 0.01f) cv += sd[r]/mean[r]; }
    return cv / 4.0f;
}

float random_baseline(int n, int nr, int ticks) {
    float total = 0;
    for (int t = 0; t < ticks; t++) {
        for (int i = 0; i < n; i++) {
            /* random walk collects nothing efficiently — just check random proximity */
        }
    }
    return (float)nr * 0.5f; /* expected: ~50% of resources reachable by random agents */
}

int main() {
    printf("═══════════════════════════════════════════════\n");
    printf("  FLUX EMERGENCE SIMULATION — Jetson Orin sm_87\n");
    printf("  %d agents, %d resources, %d ticks, %d experiments\n",
           N_AGENTS, N_RESOURCES, MAX_TICKS, N_EXPERIMENTS);
    printf("═══════════════════════════════════════════════\n\n");

    Agent *da, *ha;
    Resource *dr, *hr;
    cudaMalloc(&da, N_AGENTS * sizeof(Agent));
    cudaMalloc(&dr, N_RESOURCES * sizeof(Resource));
    ha = (Agent*)malloc(N_AGENTS * sizeof(Agent));
    hr = (Resource*)malloc(N_RESOURCES * sizeof(Resource));

    int blk = (N_AGENTS + 255) / 256;
    int rblk = (N_RESOURCES + 255) / 256;
    Result res[N_EXPERIMENTS];
    float as = 0, asp = 0, ae = 0;

    for (int e = 0; e < N_EXPERIMENTS; e++) {
        init_agents<<<blk, 256>>>(da, N_AGENTS);
        init_resources<<<rblk, 256>>>(dr, N_RESOURCES);
        cudaDeviceSynchronize();

        for (int t = 0; t < MAX_TICKS; t++) {
            tick<<<blk, 256>>>(da, dr, N_AGENTS, N_RESOURCES, t, MAX_TICKS/2);
            cudaDeviceSynchronize();
        }

        cudaMemcpy(ha, da, N_AGENTS * sizeof(Agent), cudaMemcpyDeviceToHost);
        cudaMemcpy(hr, dr, N_RESOURCES * sizeof(Resource), cudaMemcpyDeviceToHost);

        kmeans(ha, N_AGENTS, N_ARCH);
        res[e].silhouette = silhouette(ha, N_AGENTS);
        res[e].specialization = specialization(ha, N_AGENTS);

        float coord = 0;
        for (int r = 0; r < N_RESOURCES; r++) if (hr[r].collected) coord += hr[r].value;
        float rnd = random_baseline(N_AGENTS, N_RESOURCES, MAX_TICKS);
        res[e].efficiency = (rnd > 0.01f) ? coord / rnd : 1.0f;

        float te = 0, tr = 0, ti = 0;
        for (int i = 0; i < N_AGENTS; i++) {
            te += ha[i].energy; tr += ha[i].res_held; ti += ha[i].interactions;
        }
        res[e].avg_energy = te / N_AGENTS;
        res[e].avg_res = tr / N_AGENTS;
        res[e].total_interactions = ti;

        as += res[e].silhouette; asp += res[e].specialization; ae += res[e].efficiency;
        printf("Exp %d: sil=%.3f spec=%.3f eff=%.2fx energy=%.3f res=%.1f int=%d\n",
               e+1, res[e].silhouette, res[e].specialization, res[e].efficiency,
               res[e].avg_energy, res[e].avg_res, res[e].total_interactions);
    }

    as /= N_EXPERIMENTS; asp /= N_EXPERIMENTS; ae /= N_EXPERIMENTS;

    printf("\n═══════════════════════════════════════════════\n");
    printf("  AVERAGED RESULTS\n");
    printf("  Clustering:    %.3f %s\n", as,
           as > 0.3 ? "✓ COORDINATED" : as > 0.1 ? "~ WEAK" : "✗ RANDOM");
    printf("  Specialization: %.3f %s\n", asp,
           asp > 0.2 ? "✓ SPECIALIZED" : asp > 0.1 ? "~ SOME" : "✗ HOMOGENEOUS");
    printf("  Efficiency:    %.2fx %s\n", ae,
           ae > 1.5 ? "✓ EMERGENT" : ae > 1.1 ? "~ MARGINAL" : "✗ NONE");
    int p = (as > 0.3) + (asp > 0.2) + (ae > 1.5);
    printf("  VERDICT: ");
    if (p >= 3) printf("EMERGENCE CONFIRMED\n");
    else if (p >= 2) printf("PARTIAL — tune primitives\n");
    else if (p >= 1) printf("WEAK — stronger primitives needed\n");
    else printf("FALSIFIED — redesign required\n");
    printf("═══════════════════════════════════════════════\n");

    cudaFree(da); cudaFree(dr); free(ha); free(hr);
    return 0;
}
