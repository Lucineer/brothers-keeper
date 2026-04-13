/* flux-emergence-v9.cu — Population isolation vs mixing.
   v8 CONFIRMED emergence at 1.61x with scarcity+territory+comms.
   v9: Do isolated populations evolve DIFFERENT strategies?
   Run 4 separate populations (one per archetype) vs 1 mixed population.
   Compare: does isolation create stronger specialization or weaker? */

#include <cstdio>
#include <cstdlib>
#include <cmath>

#define N_PER_POP    256
#define N_ARCH       4
#define N_TOTAL      (N_PER_POP * N_ARCH)
#define N_RESOURCES  128
#define MAX_TICKS    500
#define N_EXP        5
#define SAMPLE_SZ    256
#define SAMPLE_NBR   128

__device__ __host__ unsigned int lcg(unsigned int *s) {
    *s = *s * 1103515245u + 12345u;
    return (*s >> 16) & 0x7fff;
}
__device__ __host__ float lcgf(unsigned int *s) { return (float)lcg(s) / 32768.0f; }

typedef struct {
    float x, y, vx, vy, energy, role[4], fitness;
    int arch, res_held, interactions, group, pop_id;
    float tip_x, tip_y, tip_val;
    unsigned int rng;
} Agent;

typedef struct { float x, y, value; int collected; } Resource;

__global__ void init_mixed(Agent *a, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    a[i].rng = (unsigned int)(i * 2654435761u + 17);
    a[i].x = lcgf(&a[i].rng); a[i].y = lcgf(&a[i].rng);
    a[i].vx = a[i].vy = 0.0f;
    a[i].energy = 0.5f + lcgf(&a[i].rng) * 0.5f;
    a[i].arch = i % N_ARCH; a[i].pop_id = 0;
    a[i].fitness = 0.0f; a[i].res_held = 0;
    a[i].interactions = 0; a[i].group = -1;
    a[i].tip_x = a[i].tip_y = a[i].tip_val = 0.0f;
    for (int r = 0; r < 4; r++) {
        float base = (r == a[i].arch) ? 0.7f : 0.1f;
        a[i].role[r] = base + (lcgf(&a[i].rng) - 0.5f) * 0.4f;
    }
}

__global__ void init_isolated(Agent *a, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    a[i].rng = (unsigned int)(i * 2654435761u + 17);
    a[i].x = lcgf(&a[i].rng); a[i].y = lcgf(&a[i].rng);
    a[i].vx = a[i].vy = 0.0f;
    a[i].energy = 0.5f + lcgf(&a[i].rng) * 0.5f;
    int pop = i / N_PER_POP; /* each 256-agent chunk = one population */
    a[i].arch = pop; a[i].pop_id = pop;
    a[i].fitness = 0.0f; a[i].res_held = 0;
    a[i].interactions = 0; a[i].group = -1;
    a[i].tip_x = a[i].tip_y = a[i].tip_val = 0.0f;
    /* Isolated: start as specialists in their pop's archetype */
    for (int r = 0; r < 4; r++) {
        float base = (r == pop) ? 0.7f : 0.1f;
        a[i].role[r] = base + (lcgf(&a[i].rng) - 0.5f) * 0.4f;
    }
}

__global__ void init_res(Resource *r, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    unsigned int s = (unsigned int)(i * 2654435761u + 99999);
    r[i].x = lcgf(&s); r[i].y = lcgf(&s);
    r[i].value = 0.5f + lcgf(&s) * 0.5f; r[i].collected = 0;
}



__global__ void tick(Agent *a, Resource *r, int na, int nr, int t, int pt, int isolated) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= na) return;
    Agent *ag = &a[i];
    float ep = ag->role[0], cp = ag->role[1], comm = ag->role[2], def = ag->role[3];
    float detect = 0.03f + ep * 0.04f;
    float grab = 0.02f + cp * 0.02f;

    float bd = detect; int br = -1;
    for (int j = 0; j < nr; j++) {
        if (r[j].collected) continue;
        float dx = r[j].x - ag->x, dy = r[j].y - ag->y;
        float d = sqrtf(dx*dx + dy*dy);
        if (d < bd) { bd = d; br = j; }
    }
    if (br < 0 && ag->tip_val > 0.3f) {
        float td = sqrtf((ag->tip_x-ag->x)*(ag->tip_x-ag->x)+(ag->tip_y-ag->y)*(ag->tip_y-ag->y));
        for (int j = 0; j < nr; j++) {
            if (r[j].collected) continue;
            float dx = r[j].x - ag->tip_x, dy = r[j].y - ag->tip_y;
            if (sqrtf(dx*dx+dy*dy) < 0.03f && td+0.03f < detect*2.0f) { bd=td+0.03f; br=j; break; }
        }
        ag->tip_val *= 0.95f;
    }
    if (br >= 0 && bd < grab) {
        r[br].collected = 1; ag->res_held++;
        float terr = 1.0f;
        for (int k = 0; k < 16; k++) {
            int j = lcg(&ag->rng) % na;
            if (j == i || a[j].arch != ag->arch) continue;
            float dx = a[j].x-ag->x, dy = a[j].y-ag->y;
            if (sqrtf(dx*dx+dy*dy) < 0.05f) terr += a[j].role[3] * 0.2f;
        }
        float bonus = (1.0f + cp * 0.5f) * terr;
        ag->energy = fminf(1.0f, ag->energy + r[br].value * 0.1f * bonus);
        ag->fitness += r[br].value * bonus;
    } else if (br >= 0) {
        float dx = r[br].x-ag->x, dy = r[br].y-ag->y, d = sqrtf(dx*dx+dy*dy);
        float speed = 0.008f + cp*0.008f + ep*0.006f;
        ag->vx = ag->vx*0.8f + (dx/d)*speed; ag->vy = ag->vy*0.8f + (dy/d)*speed;
    } else {
        ag->vx = ag->vx*0.95f + (lcgf(&ag->rng)-0.5f)*0.006f*(1.0f+ep);
        ag->vy = ag->vy*0.95f + (lcgf(&ag->rng)-0.5f)*0.006f*(1.0f+ep);
    }
    ag->x = fmodf(ag->x+ag->vx+1.0f, 1.0f);
    ag->y = fmodf(ag->y+ag->vy+1.0f, 1.0f);

    for (int k = 0; k < 32; k++) {
        int j = lcg(&ag->rng) % na;
        if (j == i) continue;
        /* ISOLATION: skip agents from different populations */
        if (isolated && a[j].pop_id != ag->pop_id) continue;
        float dx = a[j].x-ag->x, dy = a[j].y-ag->y, dist = sqrtf(dx*dx+dy*dy);
        if (dist >= 0.06f) continue;
        ag->interactions++;
        if (a[j].role[2] > 0.5f && comm > 0.2f) {
            float jbd = 0.1f; int jbr = -1;
            for (int m = 0; m < nr; m++) {
                if (r[m].collected) continue;
                float mdx = r[m].x-a[j].x, mdy = r[m].y-a[j].y, md = sqrtf(mdx*mdx+mdy*mdy);
                if (md < jbd) { jbd = md; jbr = m; }
            }
            if (jbr >= 0) { ag->tip_x=r[jbr].x; ag->tip_y=r[jbr].y; ag->tip_val=a[j].role[2]; }
        }
        float infl = (a[j].arch == ag->arch) ? 0.02f : 0.002f;
        for (int r = 0; r < 4; r++) ag->role[r] += (a[j].role[r]-ag->role[r])*infl;
        float sim = 0.0f;
        for (int r = 0; r < 4; r++) sim += 1.0f - fminf(1.0f, fabsf(ag->role[r]-a[j].role[r]));
        sim /= 4.0f;
        if (sim > 0.9f) { int dr=(ag->arch+1+lcg(&ag->rng)%3)%4; ag->role[dr]+=(lcgf(&ag->rng)-0.5f)*0.01f; }
        if (dist < 0.02f) { ag->vx -= dx*0.01f; ag->vy -= dy*0.01f; }
    }
    int dom=0; float dv=ag->role[0];
    for (int r=1;r<4;r++) if(ag->role[r]>dv){dv=ag->role[r];dom=r;}
    if (dom == ag->arch) ag->energy = fminf(1.0f, ag->energy+0.0005f);
    else ag->energy *= 0.9995f;
    ag->energy *= 0.999f;
    for (int r=0;r<4;r++){if(ag->role[r]<0)ag->role[r]=0;if(ag->role[r]>1)ag->role[r]=1;}
    if (t == pt) {
        ag->energy *= (1.0f - 0.5f*(1.0f-def*0.5f));
        ag->x = lcgf(&ag->rng); ag->y = lcgf(&ag->rng);
        ag->vx = ag->vy = 0.0f; ag->tip_val = 0.0f;
    }
}

void kmeans(Agent *a, int n, int k) {
    float cx[8], cy[8];
    for (int i=0;i<k;i++){cx[i]=a[i].x;cy[i]=a[i].y;}
    for(int it=0;it<20;it++){
        float sx[8]={0},sy[8]={0};int cn[8]={0};
        for(int i=0;i<n;i++){
            float bd=2.0f;int bk=0;
            for(int c=0;c<k;c++){float d=(a[i].x-cx[c])*(a[i].x-cx[c])+(a[i].y-cy[c])*(a[i].y-cy[c]);if(d<bd){bd=d;bk=c;}}
            a[i].group=bk;sx[bk]+=a[i].x;sy[bk]+=a[i].y;cn[bk]++;
        }
        for(int c=0;c<k;c++)if(cn[c]>0){cx[c]=sx[c]/cn[c];cy[c]=sy[c]/cn[c];}
    }
}

float sil(Agent *a, int n) {
    float total=0;int cnt=0;
    for(int i=0;i<SAMPLE_SZ;i++){
        int ai=(i*n)/SAMPLE_SZ;int gi=a[ai].group;
        float cd[8]={0};int cc[8]={0};
        for(int j=0;j<SAMPLE_NBR;j++){
            int aj=(j*n)/SAMPLE_NBR;if(aj==ai)continue;
            float dx=a[ai].x-a[aj].x,dy=a[ai].y-a[aj].y,d=sqrtf(dx*dx+dy*dy);
            cd[a[aj].group]+=d;cc[a[aj].group]++;
        }
        float ad=(cc[gi]>0)?cd[gi]/cc[gi]:0;float bd=1e10f;
        for(int c=0;c<N_ARCH;c++)if(c!=gi&&cc[c]>0&&cd[c]/cc[c]<bd)bd=cd[c]/cc[c];
        float mx=fmaxf(ad,bd);if(mx>1e-6f){total+=(bd-ad)/mx;cnt++;}
    }
    return cnt>0?total/cnt:0.0f;
}

float spec(Agent *a, int n) {
    float mean[4]={0},sd[4]={0};
    for(int i=0;i<n;i++)for(int r=0;r<4;r++)mean[r]+=a[i].role[r];
    for(int r=0;r<4;r++)mean[r]/=n;
    for(int i=0;i<n;i++)for(int r=0;r<4;r++)sd[r]+=(a[i].role[r]-mean[r])*(a[i].role[r]-mean[r]);
    float cv=0;
    for(int r=0;r<4;r++){sd[r]=sqrtf(sd[r]/n);if(mean[r]>0.01f)cv+=sd[r]/mean[r];}
    return cv/4.0f;
}

int main() {
    printf("═══════════════════════════════════════════════════════\n");
    printf("  FLUX v9 — Population Isolation vs Mixing\n");
    printf("  A: 4 mixed populations (1024 agents, interact freely)\n");
    printf("  B: 4 isolated populations (256 each, no cross-interaction)\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    Agent *da,*ha,*db,*hb; Resource *dr;
    cudaMalloc(&da,N_TOTAL*sizeof(Agent));
    cudaMalloc(&db,N_TOTAL*sizeof(Agent));
    cudaMalloc(&dr,N_RESOURCES*sizeof(Resource));
    ha=(Agent*)malloc(N_TOTAL*sizeof(Agent));
    hb=(Agent*)malloc(N_TOTAL*sizeof(Agent));

    int blk=(N_TOTAL+255)/256, rblk=(N_RESOURCES+255)/256;
    float mixed_fit=0, iso_fit=0, mixed_spec=0, iso_spec=0;

    for(int e=0;e<N_EXP;e++){
        /* MIXED */
        init_mixed<<<blk,256>>>(da,N_TOTAL);
        init_res<<<rblk,256>>>(dr,N_RESOURCES);
        cudaDeviceSynchronize();
        for(int t=0;t<MAX_TICKS;t++){tick<<<blk,256>>>(da,dr,N_TOTAL,N_RESOURCES,t,MAX_TICKS/2,0);cudaDeviceSynchronize();}
        cudaMemcpy(ha,da,N_TOTAL*sizeof(Agent),cudaMemcpyDeviceToHost);
        kmeans(ha,N_TOTAL,N_ARCH);
        float mf=0;
        for(int i=0;i<N_TOTAL;i++) mf+=ha[i].fitness;
        mixed_fit+=mf; mixed_spec+=spec(ha,N_TOTAL);

        /* ISOLATED */
        init_isolated<<<blk,256>>>(db,N_TOTAL);
        init_res<<<rblk,256>>>(dr,N_RESOURCES);
        cudaDeviceSynchronize();
        for(int t=0;t<MAX_TICKS;t++){tick<<<blk,256>>>(db,dr,N_TOTAL,N_RESOURCES,t,MAX_TICKS/2,1);cudaDeviceSynchronize();}
        cudaMemcpy(hb,db,N_TOTAL*sizeof(Agent),cudaMemcpyDeviceToHost);
        float isof=0;
        for(int i=0;i<N_TOTAL;i++) isof+=hb[i].fitness;
        iso_fit+=isof; iso_spec+=spec(hb,N_TOTAL);

        float ratio = (isof>0.01f)?mf/isof:1.0f;
        printf("Exp %d: mixed=%.1f iso=%.1f ratio=%.2fx m_spec=%.3f i_spec=%.3f\n",
            e+1,mf,isof,ratio,spec(ha,N_TOTAL),spec(hb,N_TOTAL));
    }
    mixed_fit/=N_EXP; iso_fit/=N_EXP; mixed_spec/=N_EXP; iso_spec/=N_EXP;
    float ratio = (iso_fit>0.01f)?mixed_fit/iso_fit:1.0f;

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Mixed fitness:    %.1f (spec: %.3f)\n",mixed_fit,mixed_spec);
    printf("  Isolated fitness: %.1f (spec: %.3f)\n",iso_fit,iso_spec);
    printf("  Mixed/Isolated:   %.2fx\n",ratio);
    if(ratio > 1.1) printf("  → MIXING WINS: cross-pollination helps\n");
    else if(ratio < 0.9) printf("  → ISOLATION WINS: diverse strategies emerge\n");
    else printf("  → NO DIFFERENCE: structure doesn\\'t matter at this scale\n");
    printf("═══════════════════════════════════════════════════════\n");

    cudaFree(da);cudaFree(db);cudaFree(dr);free(ha);free(hb);
    return 0;
}
