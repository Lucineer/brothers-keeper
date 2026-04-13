/* flux-emergence-v2.cu — Iteration 2: stronger specialization primitives.
   v1 findings: clustering=0.397, specialization=0.019, efficiency=1.30x
   v2 changes: 10x coupling, role energy bonus, sticky specialization */

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

#define N_AGENTS     4096
#define N_RESOURCES  256
#define MAX_TICKS    500
#define N_EXP        5
#define N_ARCH       4
#define SAMPLE_SZ    512
#define SAMPLE_NBR   256

__device__ __host__ unsigned int lcg(unsigned int *s) {
    *s = *s * 1103515245u + 12345u;
    return (*s >> 16) & 0x7fff;
}
__device__ __host__ float lcgf(unsigned int *s) { return (float)lcg(s) / 32768.0f; }

typedef struct {
    float x, y, vx, vy, energy, role[4], fitness;
    int arch, res_held, interactions, group, dominant_role;
    unsigned int rng;
} Agent;

typedef struct { float x, y, value; int collected; } Resource;

__global__ void init_agents(Agent *a, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    a[i].rng = (unsigned int)(i * 2654435761u + 17);
    a[i].x = lcgf(&a[i].rng); a[i].y = lcgf(&a[i].rng);
    a[i].vx = a[i].vy = 0.0f;
    a[i].energy = 0.5f + lcgf(&a[i].rng) * 0.5f;
    a[i].arch = i % N_ARCH;
    a[i].fitness = 0.0f; a[i].res_held = 0;
    a[i].interactions = 0; a[i].group = -1; a[i].dominant_role = a[i].arch;
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
    r[i].value = 0.3f + lcgf(&s) * 0.7f; r[i].collected = 0;
}

__device__ int grid_idx(float x, float y) {
    int gx = (int)(x * 8.0f); int gy = (int)(y * 8.0f);
    if (gx < 0) gx = 0; if (gx >= 8) gx = 7;
    if (gy < 0) gy = 0; if (gy >= 8) gy = 7;
    return gy * 8 + gx;
}

__global__ void tick(Agent *a, Resource *r, int na, int nr, int t, int pt) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= na) return;
    Agent *ag = &a[i];
    float ew = ag->role[0], cw = ag->role[1];

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
        r[br].collected = 1; ag->res_held++;
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

    /* Neighbor interaction via grid */
    int gi = grid_idx(ag->x, ag->y);
    int ints = 0;
    for (int ddy = -1; ddy <= 1 && ints < 16; ddy++) {
        for (int ddx = -1; ddx <= 1 && ints < 16; ddx++) {
            int cgi = gi + ddy * 8 + ddx;
            if (cgi < 0 || cgi >= 64) continue;
            for (int j = 0; j < na && ints < 16; j++) {
                if (j == i) continue;
                if (grid_idx(a[j].x, a[j].y) != cgi) continue;
                float fx = a[j].x - ag->x, fy = a[j].y - ag->y;
                float dist = sqrtf(fx*fx + fy*fy);
                if (dist >= 0.05f) continue;
                ints++; ag->interactions++;
                float infl = (a[j].arch == ag->arch) ? 0.05f : 0.005f;
                for (int r = 0; r < 4; r++)
                    ag->role[r] += (a[j].role[r] - ag->role[r]) * infl;
                if (a[j].arch == ag->arch)
                    ag->role[ag->arch] = fminf(1.0f, ag->role[ag->arch] + 0.002f);
                if (dist < 0.02f) { ag->vx -= fx * 0.01f; ag->vy -= fy * 0.01f; }
            }
        }
    }

    int dom = 0; float dom_val = ag->role[0];
    for (int r = 1; r < 4; r++) if (ag->role[r] > dom_val) { dom_val = ag->role[r]; dom = r; }
    if (dom != ag->dominant_role) { ag->energy *= 0.998f; ag->dominant_role = dom; }
    if (dom == ag->arch) ag->energy = fminf(1.0f, ag->energy + 0.001f);
    ag->energy *= 0.999f;
    for (int r = 0; r < 4; r++) {
        if (ag->role[r] < 0.0f) ag->role[r] = 0.0f;
        if (ag->role[r] > 1.0f) ag->role[r] = 1.0f;
    }
    if (t == pt) {
        ag->x = lcgf(&ag->rng); ag->y = lcgf(&ag->rng);
        ag->energy *= 0.5f; ag->vx = ag->vy = 0.0f;
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
    printf("═══════════════════════════════════════════════\n");
    printf("  FLUX EMERGENCE v2 — Stronger Primitives\n");
    printf("  10x coupling, role energy, sticky specialization\n");
    printf("═══════════════════════════════════════════════\n\n");

    Agent *da,*ha; Resource *dr,*hr;
    cudaMalloc(&da,N_AGENTS*sizeof(Agent));
    cudaMalloc(&dr,N_RESOURCES*sizeof(Resource));
    ha=(Agent*)malloc(N_AGENTS*sizeof(Agent));
    hr=(Resource*)malloc(N_RESOURCES*sizeof(Resource));

    int blk=(N_AGENTS+255)/256, rblk=(N_RESOURCES+255)/256;
    float as=0,asp=0,ae=0,asp_ag=0;

    for(int e=0;e<N_EXP;e++){
        init_agents<<<blk,256>>>(da,N_AGENTS);
        init_resources<<<rblk,256>>>(dr,N_RESOURCES);
        cudaDeviceSynchronize();
        for(int t=0;t<MAX_TICKS;t++){
            tick<<<blk,256>>>(da,dr,N_AGENTS,N_RESOURCES,t,MAX_TICKS/2);
            cudaDeviceSynchronize();
        }
        cudaMemcpy(ha,da,N_AGENTS*sizeof(Agent),cudaMemcpyDeviceToHost);
        cudaMemcpy(hr,dr,N_RESOURCES*sizeof(Resource),cudaMemcpyDeviceToHost);
        kmeans(ha,N_AGENTS,N_ARCH);
        float s1=sil(ha,N_AGENTS), s2=spec(ha,N_AGENTS);
        int sm=0;
        for(int i=0;i<N_AGENTS;i++) if(ha[i].dominant_role==ha[i].arch) sm++;
        float coord=0;
        for(int r=0;r<N_RESOURCES;r++) if(hr[r].collected) coord+=hr[r].value;
        float rnd=(float)N_RESOURCES*0.5f;
        float eff=(rnd>0.01f)?coord/rnd:1.0f;
        float te=0,tr=0,ti=0;
        for(int i=0;i<N_AGENTS;i++){te+=ha[i].energy;tr+=ha[i].res_held;ti+=ha[i].interactions;}
        as+=s1; asp+=s2; ae+=eff; asp_ag+=sm;
        printf("Exp %d: sil=%.3f spec=%.3f eff=%.2fx e=%.3f res=%.1f int=%d spec=%d/%d\n",
            e+1,s1,s2,eff,te/N_AGENTS,tr/N_AGENTS,ti,sm,N_AGENTS);
    }
    as/=N_EXP; asp/=N_EXP; ae/=N_EXP; asp_ag/=N_EXP;

    printf("\n═══════════════════════════════════════════════\n");
    printf("  AVERAGED (v1 vs v2)\n");
    printf("  Clustering:     %.3f %s  (v1: 0.397)\n",as, as>0.3?"OK":"FAIL");
    printf("  Specialization: %.3f %s  (v1: 0.019)\n",asp, asp>0.2?"YES":asp>0.1?"~":"NO");
    printf("  Efficiency:     %.2fx %s  (v1: 1.30x)\n",ae, ae>1.5?"YES":"~");
    printf("  Role fidelity:  %d/%d (%.0f%%)\n",(int)asp_ag,N_AGENTS,100.0f*asp_ag/N_AGENTS);
    int p=(as>0.3)+(asp>0.2)+(ae>1.5);
    if(p>=3) printf("  VERDICT: EMERGENCE CONFIRMED\n");
    else if(p>=2) printf("  VERDICT: PARTIAL\n");
    else if(p>=1) printf("  VERDICT: WEAK\n");
    else printf("  VERDICT: FALSIFIED\n");
    printf("═══════════════════════════════════════════════\n");

    cudaFree(da);cudaFree(dr);free(ha);free(hr);
    return 0;
}
