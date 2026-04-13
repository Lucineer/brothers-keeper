/* flux-emergence-v7.cu — Message passing: communicators share resource locations.
   v6: spec=0.712, 11% advantage, but communicators (role[2]) didn't actually DO anything visible.
   v7: Communicators broadcast nearest resource location to neighbors.
   Explorers who receive a broadcast move toward that resource.
   This tests whether information flow amplifies specialization benefits. */

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
    float tip_x, tip_y, tip_valid; /* received resource tip from communicator */
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
    a[i].tip_x = a[i].tip_y = 0.0f; a[i].tip_valid = 0.0f;
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
    a[i].tip_x = a[i].tip_y = 0.0f; a[i].tip_valid = 0.0f;
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

    float ep = ag->role[0], cp = ag->role[1], comm = ag->role[2];
    float detect = 0.08f + ep * 0.12f;
    float grab = 0.04f + cp * 0.04f;

    /* Find nearest resource */
    float bd = detect; int br = -1;
    for (int j = 0; j < nr; j++) {
        if (r[j].collected) continue;
        float dx = r[j].x - ag->x, dy = r[j].y - ag->y;
        float d = sqrtf(dx*dx + dy*dy);
        if (d < bd) { bd = d; br = j; }
    }

    /* v7: If no resource found but have a tip, check the tip location */
    if (br < 0 && ag->tip_valid > 0.0f) {
        float tdx = ag->tip_x - ag->x, tdy = ag->tip_y - ag->y;
        float td = sqrtf(tdx*tdx + tdy*tdy);
        /* Check if tip resource still exists */
        for (int j = 0; j < nr; j++) {
            if (r[j].collected) continue;
            float dx = r[j].x - ag->tip_x, dy = r[j].y - ag->tip_y;
            if (sqrtf(dx*dx+dy*dy) < 0.05f) {
                float full_dist = td + 0.05f;
                if (full_dist < detect) {
                    bd = full_dist; br = j;
                }
                break;
            }
        }
        ag->tip_valid *= 0.9f; /* tip decays */
    }

    if (br >= 0 && bd < grab) {
        r[br].collected = 1; ag->res_held++;
        float bonus = 1.0f + cp * 0.5f;
        ag->energy = fminf(1.0f, ag->energy + r[br].value * 0.1f * bonus);
        ag->fitness += r[br].value * bonus;
    } else if (br >= 0) {
        float tx = (br >= 0) ? r[br].x : ag->x;
        float ty = (br >= 0) ? r[br].y : ag->y;
        if (br < 0 && ag->tip_valid > 0.0f) { tx = ag->tip_x; ty = ag->tip_y; }
        float dx = tx - ag->x, dy = ty - ag->y;
        float d = sqrtf(dx*dx + dy*dy);
        float speed = 0.012f + cp * 0.012f + ep * 0.008f;
        ag->vx = ag->vx * 0.8f + (dx/d) * speed;
        ag->vy = ag->vy * 0.8f + (dy/d) * speed;
    } else {
        ag->vx = ag->vx * 0.95f + (lcgf(&ag->rng) - 0.5f) * 0.008f * (1.0f + ep);
        ag->vy = ag->vy * 0.95f + (lcgf(&ag->rng) - 0.5f) * 0.008f * (1.0f + ep);
    }
    ag->x = fmodf(ag->x + ag->vx + 1.0f, 1.0f);
    ag->y = fmodf(ag->y + ag->vy + 1.0f, 1.0f);

    /* Neighbor interaction */
    for (int k = 0; k < 32; k++) {
        int j = lcg(&ag->rng) % na;
        if (j == i) continue;
        float dx = a[j].x - ag->x, dy = a[j].y - ag->y;
        float dist = sqrtf(dx*dx + dy*dy);
        if (dist >= 0.06f) continue;
        ag->interactions++;

        /* v7: COMMUNICATORS broadcast resource locations */
        if (a[j].role[2] > 0.5f && comm > 0.3f) {
            /* Communicator neighbor shares its nearest resource location */
            float jbd = 0.2f; int jbr = -1;
            for (int m = 0; m < nr; m++) {
                if (r[m].collected) continue;
                float mdx = r[m].x - a[j].x, mdy = r[m].y - a[j].y;
                float md = sqrtf(mdx*mdx + mdy*mdy);
                if (md < jbd) { jbd = md; jbr = m; }
            }
            if (jbr >= 0) {
                ag->tip_x = r[jbr].x;
                ag->tip_y = r[jbr].y;
                ag->tip_valid = 1.0f;
            }
        }

        /* Role coupling + anti-convergence */
        float infl = (a[j].arch == ag->arch) ? 0.02f : 0.002f;
        for (int r = 0; r < 4; r++)
            ag->role[r] += (a[j].role[r] - ag->role[r]) * infl;
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
        ag->vx = ag->vy = 0.0f; ag->tip_valid = 0.0f;
    }
}

__global__ void tick_control(Agent *a, Resource *r, int na, int nr, int t, int pt) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= na) return;
    Agent *ag = &a[i];
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
    printf("  FLUX EMERGENCE v7 — Message Passing\n");
    printf("  Communicators broadcast resource locations to neighbors\n");
    printf("  Explorers/collectors follow tips to find resources faster\n");
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
        printf("Exp %d: sil=%.3f spec=%.3f fit_s=%.1f fit_c=%.1f ratio=%.2fx\n",
            e+1,s1,s2,fit_s,fit_c,ratio);
    }
    as_sil/=N_EXP; as_spec/=N_EXP; af_spec/=N_EXP; af_ctrl/=N_EXP;
    float ratio_avg = (af_ctrl > 0.01f) ? af_spec / af_ctrl : 1.0f;

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  v6 (no comms): 11%% advantage\n");
    printf("  v7 (with comms): %.0f%% advantage\n", (ratio_avg - 1.0f) * 100.0f);
    printf("  Clustering:     %.3f\n",as_sil);
    printf("  Specialization: %.3f\n",as_spec);
    printf("  A/B ratio:      %.2fx %s\n",ratio_avg,
        ratio_avg>1.3?"SIGNIFICANT":ratio_avg>1.15?"MODERATE":ratio_avg>1.1?"MARGINAL":"NONE");
    int p=(as_sil>0.3)+(as_spec>0.2)+(ratio_avg>1.3);
    if(p>=3) printf("  VERDICT: EMERGENCE CONFIRMED\n");
    else if(p>=2) printf("  VERDICT: PARTIAL\n");
    else printf("  VERDICT: WEAK\n");
    printf("═══════════════════════════════════════════════════════\n");

    cudaFree(da);cudaFree(db);cudaFree(dr);free(ha);free(hb);free(hr);
    return 0;
}
