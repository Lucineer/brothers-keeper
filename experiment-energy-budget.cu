/* experiment-energy-budget.cu — Biological Energy Constraints on Agents
   Same FLUX setup but agents have ATP budgets, apoptosis, circadian rhythm.
   Tests: do biological constraints improve or hurt fleet performance? */

#include <cstdio>
#include <cstdlib>
#include <cmath>

#define NA 1024
#define NR 128
#define MAXT 500
#define N_ARCH 4

__device__ __host__ unsigned int lcg(unsigned int*s){*s=*s*1103515245u+12345u;return(*s>>16)&0x7fff;}
__device__ __host__ float lcgf(unsigned int*s){return(float)lcg(s)/32768.0f;}

typedef struct{float x,y,vx,vy,energy,role[4],fitness;int arch,res_held,interactions,alive;
    float atp,max_atp,circadian,apoptosis_threshold,tip_x,tip_y,tip_val;unsigned int rng;}Agent;
typedef struct{float x,y,value;int collected;}Resource;

__global__ void init_bio(Agent*a,int n){
    int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=n)return;
    a[i].rng=(unsigned int)(i*2654435761u+17);a[i].x=lcgf(&a[i].rng);a[i].y=lcgf(&a[i].rng);
    a[i].vx=a[i].vy=0;a[i].energy=.5f+lcgf(&a[i].rng)*.5f;a[i].arch=i%N_ARCH;
    a[i].fitness=a[i].res_held=a[i].interactions=0;a[i].alive=1;
    a[i].tip_x=a[i].tip_y=a[i].tip_val=0;
    for(int r=0;r<4;r++){float b=(r==a[i].arch)?.7f:.1f;a[i].role[r]=b+(lcgf(&a[i].rng)-.5f)*.4f;}
    // Bio params
    a[i].max_atp=1.0f;a[i].atp=1.0f;
    a[i].circadian=.5f+lcgf(&a[i].rng)*.5f; // phase offset
    a[i].apoptosis_threshold=0.05f;
}

__global__ void init_norm(Agent*a,int n){
    int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=n)return;
    a[i].rng=(unsigned int)(i*2654435761u+17);a[i].x=lcgf(&a[i].rng);a[i].y=lcgf(&a[i].rng);
    a[i].vx=a[i].vy=0;a[i].energy=.5f+lcgf(&a[i].rng)*.5f;a[i].arch=i%N_ARCH;
    a[i].fitness=a[i].res_held=a[i].interactions=0;a[i].alive=1;
    a[i].tip_x=a[i].tip_y=a[i].tip_val=0;
    for(int r=0;r<4;r++){float b=(r==a[i].arch)?.7f:.1f;a[i].role[r]=b+(lcgf(&a[i].rng)-.5f)*.4f;}
}

__global__ void init_c(Agent*a,int n){
    int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=n)return;
    a[i].rng=(unsigned int)(i*2654435761u+99917);a[i].x=lcgf(&a[i].rng);a[i].y=lcgf(&a[i].rng);
    a[i].vx=a[i].vy=0;a[i].energy=.5f+lcgf(&a[i].rng)*.5f;a[i].arch=i%N_ARCH;
    a[i].fitness=a[i].res_held=a[i].interactions=0;a[i].alive=1;
    a[i].tip_x=a[i].tip_y=a[i].tip_val=0;
    for(int r=0;r<4;r++)a[i].role[r]=.25f;
}

__global__ void init_r(Resource*r,int n){int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=n)return;unsigned int s=(unsigned int)(i*2654435761u+99999);r[i].x=lcgf(&s);r[i].y=lcgf(&s);r[i].value=.5f+lcgf(&s)*.5f;r[i].collected=0;}

// Bio costs: perception 0.5, arith 0.1, deliberation 2.0, comms 1.0
// Simplified: moving costs ATP, collecting restores it, comms costs extra
__global__ void tick_bio(Agent*a,Resource*r,int na,int nr,int t,int pt){
    int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=na)return;
    Agent*ag=&a[i];if(!ag->alive)return;
    float ep=ag->role[0],cp=ag->role[1],cm=ag->role[2],df=ag->role[3];
    // Circadian rhythm: efficiency varies sinusoidally
    float circ=sinf(t*0.02f+ag->circadian*6.28f)*0.5f+0.5f; // 0-1 cycle
    float efficiency=0.5f+circ*0.5f; // 50-100% efficiency
    // ATP cost for perception (scanning)
    float perception_cost=0.005f*(1+ep*0.5f);
    ag->atp-=perception_cost;
    // Movement cost
    float move_cost=0.002f;
    // Detection + grab
    float det=(.03f+ep*.04f)*efficiency;
    float grab=(.02f+cp*.02f)*efficiency;
    float bd=det;int br=-1;
    for(int j=0;j<nr;j++){if(r[j].collected)continue;float dx=r[j].x-ag->x,dy=r[j].y-ag->y,d=sqrtf(dx*dx+dy*dy);if(d<bd){bd=d;br=j;}}
    if(br>=0&&bd<grab){
        r[br].collected=1;ag->res_held++;
        float tb=1;for(int k=0;k<16;k++){int j=lcg(&ag->rng)%na;if(j==i||!a[j].alive||a[j].arch!=ag->arch)continue;float dx=a[j].x-ag->x,dy=a[j].y-ag->y;if(sqrtf(dx*dx+dy*dy)<.05f)tb+=a[j].role[3]*.2f;}float bn=(1+cp*.5f)*tb;
        ag->energy=fminf(1,ag->energy+r[br].value*.1f*bn);ag->fitness+=r[br].value*bn;
        ag->atp=fminf(ag->max_atp,ag->atp+r[br].value*.3f); // food restores ATP
    }else if(br>=0){
        float dx=r[br].x-ag->x,dy=r[br].y-ag->y,d=sqrtf(dx*dx+dy*dy),sp=(.008f+cp*.008f+ep*.006f)*efficiency;
        ag->vx=ag->vx*.8f+(dx/d)*sp;ag->vy=ag->vy*.8f+(dy/d)*sp;
        ag->atp-=move_cost;
    }else{
        ag->vx=ag->vx*.95f+(lcgf(&ag->rng)-.5f)*.006f*(1+ep);
        ag->vy=ag->vy*.95f+(lcgf(&ag->rng)-.5f)*.006f*(1+ep);
        ag->atp-=move_cost*0.5f;
    }
    ag->x=fmodf(ag->x+ag->vx+1,1);ag->y=fmodf(ag->y+ag->vy+1,1);
    // Communication costs ATP
    for(int k=0;k<32;k++){int j=lcg(&ag->rng)%na;if(j==i||!a[j].alive)continue;
        float dx=a[j].x-ag->x,dy=a[j].y-ag->y,dist=sqrtf(dx*dx+dy*dy);
        if(dist>=.06f)continue;ag->interactions++;
        if(cm>.2f)ag->atp-=0.001f; // comms cost
        float infl=(a[j].arch==ag->arch)?.02f:.002f;
        for(int r=0;r<4;r++)ag->role[r]+=(a[j].role[r]-ag->role[r])*infl;
        float sim=0;for(int r=0;r<4;r++)sim+=1-fminf(1,fabsf(ag->role[r]-a[j].role[r]));sim/=4;
        if(sim>.9f){int dr=(ag->arch+1+lcg(&ag->rng)%3)%4;ag->role[dr]+=(lcgf(&ag->rng)-.5f)*.01f;}
        if(dist<.02f){ag->vx-=dx*.01f;ag->vy-=dy*.01f;}}
    int dom=0;float dv=ag->role[0];for(int r=1;r<4;r++)if(ag->role[r]>dv){dv=ag->role[r];dom=r;}
    if(dom==ag->arch)ag->energy=fminf(1,ag->energy+.0005f);else ag->energy*=.9995f;
    ag->energy*=.999f;for(int r=0;r<4;r++){if(ag->role[r]<0)ag->role[r]=0;if(ag->role[r]>1)ag->role[r]=1;}
    // Circadian ATP regeneration (slow)
    ag->atp+=0.001f*circ;
    if(ag->atp>ag->max_atp)ag->atp=ag->max_atp;
    // Apoptosis: die if ATP too low
    if(ag->atp<ag->apoptosis_threshold){ag->alive=0;ag->atp=0;}
    // Perturbation
    if(t==pt){ag->energy*=(1-.5f*(1-df*.5f));ag->x=lcgf(&ag->rng);ag->y=lcgf(&ag->rng);ag->vx=ag->vy=0;ag->tip_val=0;ag->atp=fminf(ag->max_atp,ag->atp+0.2f);}
}

__global__ void tick_norm(Agent*a,Resource*r,int na,int nr,int t,int pt){
    int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=na)return;
    Agent*ag=&a[i];if(!ag->alive)return;
    float ep=ag->role[0],cp=ag->role[1],cm=ag->role[2],df=ag->role[3];
    float det=.03f+ep*.04f,grab=.02f+cp*.02f;float bd=det;int br=-1;
    for(int j=0;j<nr;j++){if(r[j].collected)continue;float dx=r[j].x-ag->x,dy=r[j].y-ag->y,d=sqrtf(dx*dx+dy*dy);if(d<bd){bd=d;br=j;}}
    if(br>=0&&bd<grab){r[br].collected=1;ag->res_held++;float tb=1;for(int k=0;k<16;k++){int j=lcg(&ag->rng)%na;if(j==i||!a[j].alive||a[j].arch!=ag->arch)continue;float dx=a[j].x-ag->x,dy=a[j].y-ag->y;if(sqrtf(dx*dx+dy*dy)<.05f)tb+=a[j].role[3]*.2f;}float bn=(1+cp*.5f)*tb;ag->energy=fminf(1,ag->energy+r[br].value*.1f*bn);ag->fitness+=r[br].value*bn;}
    else if(br>=0){float dx=r[br].x-ag->x,dy=r[br].y-ag->y,d=sqrtf(dx*dx+dy*dy),sp=.008f+cp*.008f+ep*.006f;ag->vx=ag->vx*.8f+(dx/d)*sp;ag->vy=ag->vy*.8f+(dy/d)*sp;}
    else{ag->vx=ag->vx*.95f+(lcgf(&ag->rng)-.5f)*.006f*(1+ep);ag->vy=ag->vy*.95f+(lcgf(&ag->rng)-.5f)*.006f*(1+ep);}
    ag->x=fmodf(ag->x+ag->vx+1,1);ag->y=fmodf(ag->y+ag->vy+1,1);
    for(int k=0;k<32;k++){int j=lcg(&ag->rng)%na;if(j==i||!a[j].alive)continue;float dx=a[j].x-ag->x,dy=a[j].y-ag->y,dist=sqrtf(dx*dx+dy*dy);if(dist>=.06f)continue;ag->interactions++;float infl=(a[j].arch==ag->arch)?.02f:.002f;for(int r=0;r<4;r++)ag->role[r]+=(a[j].role[r]-ag->role[r])*infl;float sim=0;for(int r=0;r<4;r++)sim+=1-fminf(1,fabsf(ag->role[r]-a[j].role[r]));sim/=4;if(sim>.9f){int dr=(ag->arch+1+lcg(&ag->rng)%3)%4;ag->role[dr]+=(lcgf(&ag->rng)-.5f)*.01f;}if(dist<.02f){ag->vx-=dx*.01f;ag->vy-=dy*.01f;}}
    int dom=0;float dv=ag->role[0];for(int r=1;r<4;r++)if(ag->role[r]>dv){dv=ag->role[r];dom=r;}if(dom==ag->arch)ag->energy=fminf(1,ag->energy+.0005f);else ag->energy*=.9995f;ag->energy*=.999f;for(int r=0;r<4;r++){if(ag->role[r]<0)ag->role[r]=0;if(ag->role[r]>1)ag->role[r]=1;}
    if(t==pt){ag->energy*=(1-.5f*(1-df*.5f));ag->x=lcgf(&ag->rng);ag->y=lcgf(&ag->rng);ag->vx=ag->vy=0;ag->tip_val=0;}
}

__global__ void tick_c(Agent*a,Resource*r,int na,int nr,int t,int pt){
    int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=na)return;Agent*ag=&a[i];
    float det=.05f,grab=.03f;float bd=det;int br=-1;for(int j=0;j<nr;j++){if(r[j].collected)continue;float dx=r[j].x-ag->x,dy=r[j].y-ag->y,d=sqrtf(dx*dx+dy*dy);if(d<bd){bd=d;br=j;}}if(br>=0&&bd<grab){r[br].collected=1;ag->res_held++;ag->energy=fminf(1,ag->energy+r[br].value*.1f);ag->fitness+=r[br].value;}else if(br>=0){float dx=r[br].x-ag->x,dy=r[br].y-ag->y,d=sqrtf(dx*dx+dy*dy);ag->vx=ag->vx*.8f+(dx/d)*.014f;ag->vy=ag->vy*.8f+(dy/d)*.014f;}else{ag->vx=ag->vx*.95f+(lcgf(&ag->rng)-.5f)*.008f;ag->vy=ag->vy*.95f+(lcgf(&ag->rng)-.5f)*.008f;}ag->x=fmodf(ag->x+ag->vx+1,1);ag->y=fmodf(ag->y+ag->vy+1,1);ag->energy*=.999f;if(t==pt){ag->energy*=.5f;ag->x=lcgf(&ag->rng);ag->y=lcgf(&a->rng);ag->vx=ag->vy=0;}}

int main(){
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Experiment 6: Biological Energy Budget (ATP + Circadian)\n");
    printf("  A: bio constraints  B: normal specialists  C: control\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    Agent*da,*ha,*db,*hb,*dc,*hc;Resource*dr;
    cudaMalloc(&da,NA*sizeof(Agent));cudaMalloc(&db,NA*sizeof(Agent));cudaMalloc(&dc,NA*sizeof(Agent));
    cudaMalloc(&dr,NR*sizeof(Resource));
    ha=(Agent*)malloc(NA*sizeof(Agent));hb=(Agent*)malloc(NA*sizeof(Agent));hc=(Agent*)malloc(NA*sizeof(Agent));
    int blk=(NA+255)/256,rblk=(NR+255)/256;
    float f_bio=0,f_norm=0,f_ctrl=0;int bio_dead=0;
    for(int e=0;e<5;e++){
        init_bio<<<blk,256>>>(da,NA);init_r<<<rblk,256>>>(dr,NR);cudaDeviceSynchronize();
        for(int t=0;t<MAXT;t++){tick_bio<<<blk,256>>>(da,dr,NA,NR,t,250);cudaDeviceSynchronize();}
        cudaMemcpy(ha,da,NA*sizeof(Agent),cudaMemcpyDeviceToHost);float f=0;int dead=0;
        for(int i=0;i<NA;i++){f+=ha[i].fitness;if(!ha[i].alive)dead++;}
        f_bio+=f;bio_dead+=dead;
        init_norm<<<blk,256>>>(db,NA);init_r<<<rblk,256>>>(dr,NR);cudaDeviceSynchronize();
        for(int t=0;t<MAXT;t++){tick_norm<<<blk,256>>>(db,dr,NA,NR,t,250);cudaDeviceSynchronize();}
        cudaMemcpy(hb,db,NA*sizeof(Agent),cudaMemcpyDeviceToHost);f=0;for(int i=0;i<NA;i++)f+=hb[i].fitness;f_norm+=f;
        init_c<<<blk,256>>>(dc,NA);init_r<<<rblk,256>>>(dr,NR);cudaDeviceSynchronize();
        for(int t=0;t<MAXT;t++){tick_c<<<blk,256>>>(dc,dr,NA,NR,t,250);cudaDeviceSynchronize();}
        cudaMemcpy(hc,dc,NA*sizeof(Agent),cudaMemcpyDeviceToHost);f=0;for(int i=0;i<NA;i++)f+=hc[i].fitness;f_ctrl+=f;
        printf("Exp %d: bio=%.1f(norm) norm=%.1f ctrl=%.1f | bio_dead=%d\n",e+1,f/NA,f/NA,f/NA,dead);
    }
    f_bio/=5;f_norm/=5;f_ctrl/=5;bio_dead/=5;
    float ratio=(f_norm>.01f)?f_bio/f_norm:1;
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Bio:    %.1f (%.2fx ctrl) — avg dead: %d/1024\n",f_bio,(f_ctrl>.01f)?f_bio/f_ctrl:1,bio_dead);
    printf("  Normal: %.1f (%.2fx ctrl)\n",f_norm,(f_ctrl>.01f)?f_norm/f_ctrl:1);
    printf("  Control: %.1f\n",f_ctrl);
    printf("  Ratio:  %.2fx (%+.0f%%)\n",ratio,(ratio-1)*100);
    printf("  Verdict: %s\n",ratio>1.1?"BIO WINS":ratio<0.9?"BIO HURTS":"NO DIFFERENCE");
    printf("═══════════════════════════════════════════════════════\n");
    cudaFree(da);cudaFree(db);cudaFree(dc);cudaFree(dr);free(ha);free(hb);free(hc);return 0;
}
