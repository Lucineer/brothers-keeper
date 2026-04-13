/* flux-emergence-v4.cu — Wire specialization into behavior.
   v3: spec=0.794 but eff=1.30x (specialization didn't affect collection).
   v4: explorers find resources faster, collectors grab faster,
       communicators boost neighbors, defenders resist perturbation.
   Roles have BEHAVIORAL consequences. */

#include <cstdio>
#include <cstdlib>
#include <cmath>

#define N_AGENTS     4096
#define N_RESOURCES  256
#define MAX_TICKS    500
#define N_EXP        5
#define N_ARCH       4
#define SAMPLE_SZ    512
#define SAMPLE_NBR   256

/* Roles: 0=explore, 1=collect, 2=communicate, 3=defend */
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

__global__ void init_agents(Agent *a, int n) {
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

__global__ void init_resources(Resource *r, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    unsigned int s = (unsigned int)(i * 2654435761u + 99999);
    r[i].x = lcgf(&s); r[i].y = lcgf(&s);
    r[i].value = 0.3f + lcgf(&s) * 0.7f; r[i].collected = 0;
}

__global__ void tick(Agent *a, Resource *r, int na, int nr, int t, int pt) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= na) return;
    Agent *ag = &a[i];

    /* v4: ROLES AFFECT BEHAVIOR */
    float explore_power = ag->role[0];  /* detection range multiplier */
    float collect_power = ag->role[1];  /* grab range multiplier */
    float comm_power    = ag->role[2];  /* neighbor boost strength */
    float defend_power  = ag->role[3];  /* perturbation resistance */

    /* Explore: higher explore_role = larger detection radius */
    float detect_range = 0.1f + explore_power * 0.2f; /* 0.1 to 0.3 */
    float grab_range = 0.05f + collect_power * 0.05f;  /* 0.05 to 0.10 */

    /* Find nearest resource within detection range */
    float bd = detect_range; int br = -1;
    for (int j = 0; j < nr; j++) {
        if (r[j].collected) continue;
        float dx = r[j].x - ag->x, dy = r[j].y - ag->y;
        float d = sqrtf(dx*dx + dy*dy);
        if (d < bd) { bd = d; br = j; }
    }

    /* Collect: higher collect_role = larger grab radius */
    if (br >= 0 && bd < grab_range) {
        r[br].collected = 1; ag->res_held++;
        float bonus = 1.0f + collect_power * 0.5f;
        ag->energy = fminf(1.0f, ag->energy + r[br].value * 0.1f * bonus);
        ag->fitness += r[br].value * bonus;
    } else if (br >= 0) {
        float speed = 0.015f + collect_power * 0.015f + explore_power * 0.01f;
        float dx = r[br].x - ag->x, dy = r[br].y - ag->y;
        float d = sqrtf(dx*dx + dy*dy);
        ag->vx = ag->vx * 0.8f + (dx/d) * speed;
        ag->vy = ag->vy * 0.8f + (dy/d) * speed;
    } else {
        ag->vx = ag->vx * 0.95f + (lcgf(&ag->rng) - 0.5f) * 0.01f * (1.0f + explore_power);
        ag->vy = ag->vy * 0.95f + (lcgf(&ag->rng) - 0.5f) * 0.01f * (1.0f + explore_power);
    }

    ag->x = fmodf(ag->x + ag->vx + 1.0f, 1.0f);
    ag->y = fmodf(ag->y + ag->vy + 1.0f, 1.0f);

    /* Neighbor interaction with anti-convergence */
    int ints = 0;
    for (int k = 0; k < 32; k++) {
        int j = lcg(&ag->rng) % na;
        if (j == i) continue;
        float dx = a[j].x - ag->x, dy = a[j].y - ag->y;
        float dist = sqrtf(dx*dx + dy*dy);
        if (dist >= 0.05f) continue;
        ints++; ag->interactions++;

        float infl = (a[j].arch == ag->arch) ? 0.02f : 0.002f;
        for (int r = 0; r < 4; r++)
            ag->role[r] += (a[j].role[r] - ag->role[r]) * infl;

        /* v4: COMM role actually boosts neighbor energy */
        if (a[j].role[2] > 0.5f) {
            ag->energy = fminf(1.0f, ag->energy + a[j].role[2] * 0.0002f);
        }

        /* Anti-convergence drift */
        float sim = 0.0f;
        for (int r = 0; r < 4; r++) sim += 1.0f - fminf(1.0f, fabsf(ag->role[r] - a[j].role[r]));
        sim /= 4.0f;
        if (sim > 0.9f) {
            int dr = (ag->arch + 1 + lcg(&ag->rng) % 3) % 4;
            ag->role[dr] += (lcgf(&ag->rng) - 0.5f) * 0.01f;
        }

        if (dist < 0.02f) { ag->vx -= dx * 0.01f; ag->vy -= dy * 0.01f; }
    }

    /* Role energy alignment */
    int dom = 0; float dv = ag->role[0];
    for (int r = 1; r < 4; r++) if (ag->role[r] > dv) { dv = ag->role[r]; dom = r; }
    if (dom == ag->arch) ag->energy = fminf(1.0f, ag->energy + 0.0005f);
    else ag->energy *= 0.9995f;
    ag->energy *= 0.999f;

    for (int r = 0; r < 4; r++) {
        if (ag->role[r] < 0.0f) ag->role[r] = 0.0f;
        if (ag->role[r] > 1.0f) ag->role[r] = 1.0f;
    }

    /* v4: DEFEND role reduces perturbation damage */
    if (t == pt) {
        float resist = defend_power * 0.5f; /* 0 to 0.5 damage reduction */
        float damage = 0.5f * (1.0f - resist);
        ag->energy *= (1.0f - damage);
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
    printf("═══════════════════════════════════════════════════\n");
    printf("  FLUX EMERGENCE v4 — Behavioral Specialization\n");
    printf("  Roles now affect: detection range, grab range,\n");
    printf("  movement speed, energy boost, perturbation resist\n");
    printf("═══════════════════════════════════════════════════\n\n");

    Agent *da,*ha; Resource *dr,*hr;
    cudaMalloc(&da,N_AGENTS*sizeof(Agent));
    cudaMalloc(&dr,N_RESOURCES*sizeof(Resource));
    ha=(Agent*)malloc(N_AGENTS*sizeof(Agent));
    hr=(Resource*)malloc(N_RESOURCES*sizeof(Resource));

    int blk=(N_AGENTS+255)/256, rblk=(N_RESOURCES+255)/256;
    float as=0,asp=0,ae=0;

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
        float coord=0;
        for(int r=0;r<N_RESOURCES;r++) if(hr[r].collected) coord+=hr[r].value;
        float rnd=(float)N_RESOURCES*0.5f;
        float eff=(rnd>0.01f)?coord/rnd:1.0f;
        float te=0,tr=0,ti=0;
        for(int i=0;i<N_AGENTS;i++){te+=ha[i].energy;tr+=ha[i].res_held;ti+=ha[i].interactions;}
        as+=s1; asp+=s2; ae+=eff;
        printf("Exp %d: sil=%.3f spec=%.3f eff=%.2fx e=%.3f res=%.1f int=%.0f\n",
            e+1,s1,s2,eff,te/N_AGENTS,tr/N_AGENTS,(float)ti);
    }
    as/=N_EXP; asp/=N_EXP; ae/=N_EXP;

    printf("\n═══════════════════════════════════════════════════\n");
    printf("  v1=base v2=strong v3=anti-conv v4=behavior\n");
    printf("  Clustering:     %.3f %s  (v1:.397 v3:.395)\n",as, as>0.3?"OK":"FAIL");
    printf("  Specialization: %.3f %s  (v1:.019 v3:.794)\n",asp, asp>0.2?"YES":"NO");
    printf("  Efficiency:     %.2fx %s  (v1:1.30 v3:1.30)\n",ae, ae>1.5?"YES":"~");
    int p=(as>0.3)+(asp>0.2)+(ae>1.5);
    if(p>=3) printf("  VERDICT: EMERGENCE CONFIRMED\n");
    else if(p>=2) printf("  VERDICT: PARTIAL\n");
    else printf("  VERDICT: WEAK/FAIL\n");
    printf("═══════════════════════════════════════════════════\n");

    cudaFree(da);cudaFree(dr);free(ha);free(hr);
    return 0;
}
