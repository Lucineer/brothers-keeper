/* flux-emergence-v6.cu — Control group comparison.
   v4/v5: eff stuck at 1.30x because baseline was wrong.
   v6: Run BOTH specialized and random-control populations,
       compare fitness directly. No baseline estimation needed.
   
   Also: reduce agents to 1024, increase resources to 512.
   With 4096 agents vs 256 resources, every resource is instantly found
   regardless of specialization. Need scarcity for specialization to matter. */

#include <cstdio>
#include <cstdlib>
#include <cmath>

#define N_AGENTS     1024
#define N_RESOURCES  512
#define MAX_TICKS    500
#define N_EXP        5
#define N_ARCH       4
#define SAMPLE_SZ    256
#define SAMPLE_NBR   128

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

typedef struct { float x, y, value; int collected; } Resource;

__global__ void init_specialized(Agent *a, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    a[i].rng = (unsigned int)(i * 2654435761u + 17);
    a[i].x = lcgf(&a[i].rng); a[i].y = lcgf(&a[i].rng);
    a[i].vx = a[i].vy = 0.0f;
    a[i].energy = 0.5f + lcgf(&a[i].rng) * 0.5f;
    a[i].arch = i % N_ARCH;
    a[i].fitness = 0.0f; a[i].res_held = 0;
    a[i].interactions = 0; a[i].group = -1;
    for (int r = 0; r < 4; r++) {
        float base = (r == a[i].arch) ? 0.7f : 0.1f;
        a[i].role[r] = base + (lcgf(&a[i].rng) - 0.5f) * 0.4f;
    }
}

__global__ void init_control(Agent *a, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    a[i].rng = (unsigned int)(i * 2654435761u + 99917);
    a[i].x = lcgf(&a[i].rng); a[i].y = lcgf(&a[i].rng);
    a[i].vx = a[i].vy = 0.0f;
    a[i].energy = 0.5f + lcgf(&a[i].rng) * 0.5f;
    a[i].arch = i % N_ARCH;
    a[i].fitness = 0.0f; a[i].res_held = 0;
    a[i].interactions = 0; a[i].group = -1;
    /* Control: all roles start EQUAL (no specialization bias) */
    for (int r = 0; r < 4; r++) a[i].role[r] = 0.25f + (lcgf(&a[i].rng) - 0.5f) * 0.1f;
}

__global__ void init_resources(Resource *r, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    unsigned int s = (unsigned int)(i * 2654435761u + 99999);
    r[i].x = lcgf(&s); r[i].y = lcgf(&s);
    r[i].value = 0.3f + lcgf(&s) * 0.7f; r[i].collected = 0;
}

__global__ void tick_specialized(Agent *a, Resource *r, int na, int nr, int t, int pt) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= na) return;
    Agent *ag = &a[i];

    float ep = ag->role[0], cp = ag->role[1];
    float detect = 0.08f + ep * 0.12f;
    float grab = 0.04f + cp * 0.04f;

    float bd = detect; int br = -1;
    for (int j = 0; j < nr; j++) {
        if (r[j].collected) continue;
        float dx = r[j].x - ag->x, dy = r[j].y - ag->y;
        float d = sqrtf(dx*dx + dy*dy);
        if (d < bd) { bd = d; br = j; }
    }

    if (br >= 0 && bd < grab) {
        r[br].collected = 1; ag->res_held++;
        float bonus = 1.0f + cp * 0.5f;
        ag->energy = fminf(1.0f, ag->energy + r[br].value * 0.1f * bonus);
        ag->fitness += r[br].value * bonus;
    } else if (br >= 0) {
        float speed = 0.012f + cp * 0.012f + ep * 0.008f;
        float dx = r[br].x - ag->x, dy = r[br].y - ag->y;
        float d = sqrtf(dx*dx + dy*dy);
        ag->vx = ag->vx * 0.8f + (dx/d) * speed;
        ag->vy = ag->vy * 0.8f + (dy/d) * speed;
    } else {
        ag->vx = ag->vx * 0.95f + (lcgf(&ag->rng) - 0.5f) * 0.008f * (1.0f + ep);
        ag->vy = ag->vy * 0.95f + (lcgf(&ag->rng) - 0.5f) * 0.008f * (1.0f + ep);
    }
    ag->x = fmodf(ag->x + ag->vx + 1.0f, 1.0f);
    ag->y = fmodf(ag->y + ag->vy + 1.0f, 1.0f);

    /* Neighbor interaction + anti-convergence */
    for (int k = 0; k < 32; k++) {
        int j = lcg(&ag->rng) % na;
        if (j == i) continue;
        float dx = a[j].x - ag->x, dy = a[j].y - ag->y;
        float dist = sqrtf(dx*dx + dy*dy);
        if (dist >= 0.06f) continue;
        ag->interactions++;
        float infl = (a[j].arch == ag->arch) ? 0.02f : 0.002f;
        for (int r = 0; r < 4; r++)
            ag->role[r] += (a[j].role[r] - ag->role[r]) * infl;
        if (a[j].role[2] > 0.5f)
            ag->energy = fminf(1.0f, ag->energy + a[j].role[2] * 0.0003f);
        float sim = 0.0f;
        for (int r = 0; r < 4; r++) sim += 1.0f - fminf(1.0f, fabsf(ag->role[r] - a[j].role[r]));
        sim /= 4.0f;
        if (sim > 0.9f) {
            int dr = (ag->arch + 1 + lcg(&ag->rng) % 3) % 4;
            ag->role[dr] += (lcgf(&ag->rng) - 0.5f) * 0.01f;
        }
        if (dist < 0.02f) { ag->vx -= dx * 0.01f; ag->vy -= dy * 0.01f; }
    }

    int dom = 0; float dv = ag->role[0];
    for (int r = 1; r < 4; r++) if (ag->role[r] > dv) { dv = ag->role[r]; dom = r; }
    if (dom == ag->arch) ag->energy = fminf(1.0f, ag->energy + 0.0005f);
    else ag->energy *= 0.9995f;
    ag->energy *= 0.999f;
    for (int r = 0; r < 4; r++) {
        if (ag->role[r] < 0.0f) ag->role[r] = 0.0f;
        if (ag->role[r] > 1.0f) ag->role[r] = 1.0f;
    }
    if (t == pt) {
        float resist = ag->role[3] * 0.5f;
        ag->energy *= (1.0f - 0.5f * (1.0f - resist));
        ag->x = lcgf(&ag->rng); ag->y = lcgf(&ag->rng);
        ag->vx = ag->vy = 0.0f;
    }
}

/* Control: identical tick but NO role effects on behavior */
__global__ void tick_control(Agent *a, Resource *r, int na, int nr, int t, int pt) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= na) return;
    Agent *ag = &a[i];

    /* Fixed parameters regardless of roles */
    float detect = 0.14f; float grab = 0.06f;

    float bd = detect; int br = -1;
    for (int j = 0; j < nr; j++) {
        if (r[j].collected) continue;
        float dx = r[j].x - ag->x, dy = r[j].y - ag->y;
        float d = sqrtf(dx*dx + dy*dy);
        if (d < bd) { bd = d; br = j; }
    }
    if (br >= 0 && bd < grab) {
        r[br].collected = 1; ag->res_held++;
        ag->energy = fminf(1.0f, ag->energy + r[br].value * 0.1f);
        ag->fitness += r[br].value;
    } else if (br >= 0) {
        float dx = r[br].x - ag->x, dy = r[br].y - ag->y;
        float d = sqrtf(dx*dx + dy*dy);
        ag->vx = ag->vx * 0.8f + (dx/d) * 0.02f;
        ag->vy = ag->vy * 0.8f + (dy/d) * 0.02f;
    } else {
        ag->vx = ag->vx * 0.95f + (lcgf(&ag->rng) - 0.5f) * 0.01f;
        ag->vy = ag->vy * 0.95f + (lcgf(&ag->rng) - 0.5f) * 0.01f;
    }
    ag->x = fmodf(ag->x + ag->vx + 1.0f, 1.0f);
    ag->y = fmodf(ag->y + ag->vy + 1.0f, 1.0f);

    /* Same neighbor count for fair comparison */
    for (int k = 0; k < 32; k++) {
        int j = lcg(&ag->rng) % na;
        if (j == i) continue;
        float dx = a[j].x - ag->x, dy = a[j].y - ag->y;
        if (sqrtf(dx*dx+dy*dy) >= 0.06f) continue;
        ag->interactions++;
        if (sqrtf(dx*dx+dy*dy) < 0.02f) { ag->vx -= dx * 0.01f; ag->vy -= dy * 0.01f; }
    }

    ag->energy *= 0.999f;
    if (t == pt) {
        ag->energy *= 0.5f;
        ag->x = lcgf(&ag->rng); ag->y = lcgf(&ag->rng);
        ag->vx = ag->vy = 0.0f;
    }
}

void kmeans(Agent *a, int n, int k) {
    float cx[8], cy[8];
    for (int i = 0; i < k; i++) { cx[i] = a[i].x; cy[i] = a[i].y; }
    for (int it = 0; it < 20; it++) {
        float sx[8]={0},sy[8]={0}; int cn[8]={0};
        for (int i = 0; i < n; i++) {
            float bd = 2.0f; int bk = 0;
            for (int c = 0; c < k; c++) {
                float d = (a[i].x-cx[c])*(a[i].x-cx[c])+(a[i].y-cy[c])*(a[i].y-cy[c]);
                if (d < bd) { bd = d; bk = c; }
            }
            a[i].group = bk; sx[bk] += a[i].x; sy[bk] += a[i].y; cn[bk]++;
        }
        for (int c = 0; c < k; c++) if (cn[c]>0) { cx[c]=sx[c]/cn[c]; cy[c]=sy[c]/cn[c]; }
    }
}

float sil(Agent *a, int n) {
    float total = 0; int cnt = 0;
    for (int i = 0; i < SAMPLE_SZ; i++) {
        int ai = (i*n)/SAMPLE_SZ; int gi = a[ai].group;
        float cd[8]={0}; int cc[8]={0};
        for (int j = 0; j < SAMPLE_NBR; j++) {
            int aj = (j*n)/SAMPLE_NBR; if (aj==ai) continue;
            float dx=a[ai].x-a[aj].x, dy=a[ai].y-a[aj].y, d=sqrtf(dx*dx+dy*dy);
            cd[a[aj].group]+=d; cc[a[aj].group]++;
        }
        float ad=(cc[gi]>0)?cd[gi]/cc[gi]:0; float bd=1e10f;
        for (int c=0;c<N_ARCH;c++) if(c!=gi&&cc[c]>0&&cd[c]/cc[c]<bd) bd=cd[c]/cc[c];
        float mx=fmaxf(ad,bd);
        if(mx>1e-6f){total+=(bd-ad)/mx;cnt++;}
    }
    return cnt>0?total/cnt:0.0f;
}

float spec(Agent *a, int n) {
    float mean[4]={0},sd[4]={0};
    for(int i=0;i<n;i++) for(int r=0;r<4;r++) mean[r]+=a[i].role[r];
    for(int r=0;r<4;r++) mean[r]/=n;
    for(int i=0;i<n;i++) for(int r=0;r<4;r++) sd[r]+=(a[i].role[r]-mean[r])*(a[i].role[r]-mean[r]);
    float cv=0;
    for(int r=0;r<4;r++){sd[r]=sqrtf(sd[r]/n);if(mean[r]>0.01f) cv+=sd[r]/mean[r];}
    return cv/4.0f;
}

int main() {
    printf("═══════════════════════════════════════════════════════\n");
    printf("  FLUX EMERGENCE v6 — Control Group A/B Test\n");
    printf("  1024 agents, 512 resources, 500 ticks\n");
    printf("  A: specialized roles with behavioral effects\n");
    printf("  B: control (uniform roles, same movement)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    Agent *da,*ha,*db,*hb; Resource *dr,*hr;
    cudaMalloc(&da,N_AGENTS*sizeof(Agent));
    cudaMalloc(&db,N_AGENTS*sizeof(Agent));
    cudaMalloc(&dr,N_RESOURCES*sizeof(Resource));
    ha=(Agent*)malloc(N_AGENTS*sizeof(Agent));
    hb=(Agent*)malloc(N_AGENTS*sizeof(Agent));
    hr=(Resource*)malloc(N_RESOURCES*sizeof(Resource));

    int blk=(N_AGENTS+255)/256, rblk=(N_RESOURCES+255)/256;
    float as_sil=0,as_spec=0,af_spec=0,af_ctrl=0;

    for(int e=0;e<N_EXP;e++){
        /* Specialized population */
        init_specialized<<<blk,256>>>(da,N_AGENTS);
        init_resources<<<rblk,256>>>(dr,N_RESOURCES);
        cudaDeviceSynchronize();
        for(int t=0;t<MAX_TICKS;t++){
            tick_specialized<<<blk,256>>>(da,dr,N_AGENTS,N_RESOURCES,t,MAX_TICKS/2);
            cudaDeviceSynchronize();
        }
        cudaMemcpy(ha,da,N_AGENTS*sizeof(Agent),cudaMemcpyDeviceToHost);
        kmeans(ha,N_AGENTS,N_ARCH);
        float s1=sil(ha,N_AGENTS), s2=spec(ha,N_AGENTS);
        float fit_s=0;
        for(int i=0;i<N_AGENTS;i++) fit_s+=ha[i].fitness;

        /* Control population (same resources, fresh copy) */
        init_control<<<blk,256>>>(db,N_AGENTS);
        init_resources<<<rblk,256>>>(dr,N_RESOURCES);
        cudaDeviceSynchronize();
        for(int t=0;t<MAX_TICKS;t++){
            tick_control<<<blk,256>>>(db,dr,N_AGENTS,N_RESOURCES,t,MAX_TICKS/2);
            cudaDeviceSynchronize();
        }
        cudaMemcpy(hb,db,N_AGENTS*sizeof(Agent),cudaMemcpyDeviceToHost);
        float fit_c=0;
        for(int i=0;i<N_AGENTS;i++) fit_c+=hb[i].fitness;

        float ratio = (fit_c > 0.01f) ? fit_s / fit_c : 1.0f;
        as_sil+=s1; as_spec+=s2; af_spec+=fit_s; af_ctrl+=fit_c;
        printf("Exp %d: sil=%.3f spec=%.3f fit_spec=%.1f fit_ctrl=%.1f ratio=%.2fx\n",
            e+1,s1,s2,fit_s,fit_c,ratio);
    }
    as_sil/=N_EXP; as_spec/=N_EXP; af_spec/=N_EXP; af_ctrl/=N_EXP;
    float ratio_avg = (af_ctrl > 0.01f) ? af_spec / af_ctrl : 1.0f;

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  AVERAGED\n");
    printf("  Clustering:     %.3f %s\n",as_sil, as_sil>0.3?"OK":"FAIL");
    printf("  Specialization: %.3f %s\n",as_spec, as_spec>0.2?"YES":"NO");
    printf("  Fitness (spec): %.1f\n",af_spec);
    printf("  Fitness (ctrl): %.1f\n",af_ctrl);
    printf("  A/B ratio:      %.2fx %s\n",ratio_avg, ratio_avg>1.3?"SIGNIFICANT":ratio_avg>1.1?"MARGINAL":"NONE");
    int p=(as_sil>0.3)+(as_spec>0.2)+(ratio_avg>1.3);
    if(p>=3) printf("  VERDICT: EMERGENCE CONFIRMED\n");
    else if(p>=2) printf("  VERDICT: PARTIAL\n");
    else printf("  VERDICT: WEAK/FAIL\n");
    printf("═══════════════════════════════════════════════════════\n");

    cudaFree(da);cudaFree(db);cudaFree(dr);free(ha);free(hb);free(hr);
    return 0;
}
