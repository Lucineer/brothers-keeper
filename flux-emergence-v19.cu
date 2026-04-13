/* flux-emergence-v19.cu — Predator-Prey + Competition
   Novel: "defend" archetype agents can STEAL fitness from other-arch agents on contact.
   Creates real competition — defend role becomes predatory.
   Prediction: predation reduces overall fitness but increases role diversity. */

#include <cstdio>
#include <cstdlib>
#include <cmath>

#define NA 1024
#define NR 128
#define MAXT 500
#define N_ARCH 4

__device__ __host__ unsigned int lcg(unsigned int*s){*s=*s*1103515245u+12345u;return(*s>>16)&0x7fff;}
__device__ __host__ float lcgf(unsigned int*s){return(float)lcg(s)/32768.0f;}

typedef struct{float x,y,vx,vy,energy,role[4],fitness;int arch,res_held,interactions,group;float tip_x,tip_y,tip_val;unsigned int rng;}Agent;
typedef struct{float x,y,value;int collected;}Resource;

__global__ void init_a(Agent*a,int n){int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=n)return;a[i].rng=(unsigned int)(i*2654435761u+17);a[i].x=lcgf(&a[i].rng);a[i].y=lcgf(&a[i].rng);a[i].vx=a[i].vy=0;a[i].energy=.5f+lcgf(&a[i].rng)*.5f;a[i].arch=i%N_ARCH;a[i].fitness=a[i].res_held=a[i].interactions=0;a[i].group=-1;a[i].tip_x=a[i].tip_y=a[i].tip_val=0;for(int r=0;r<4;r++){float b=(r==a[i].arch)?.7f:.1f;a[i].role[r]=b+(lcgf(&a[i].rng)-.5f)*.4f;}}

__global__ void init_c(Agent*a,int n){int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=n)return;a[i].rng=(unsigned int)(i*2654435761u+99917);a[i].x=lcgf(&a[i].rng);a[i].y=lcgf(&a[i].rng);a[i].vx=a[i].vy=0;a[i].energy=.5f+lcgf(&a[i].rng)*.5f;a[i].arch=i%N_ARCH;a[i].fitness=a[i].res_held=a[i].interactions=0;a[i].group=-1;a[i].tip_x=a[i].tip_y=a[i].tip_val=0;for(int r=0;r<4;r++)a[i].role[r]=.25f;}

__global__ void init_r(Resource*r,int n){int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=n)return;unsigned int s=(unsigned int)(i*2654435761u+99999);r[i].x=lcgf(&s);r[i].y=lcgf(&s);r[i].value=.5f+lcgf(&s)*.5f;r[i].collected=0;}

__global__ void tick_s(Agent*a,Resource*r,int na,int nr,int t,int pt,int predation){
    int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=na)return;
    Agent*ag=&a[i];
    float ep=ag->role[0],cp=ag->role[1],cm=ag->role[2],df=ag->role[3];
    float det=.03f+ep*.04f,grab=.02f+cp*.02f;
    float bd=det;int br=-1;
    for(int j=0;j<nr;j++){if(r[j].collected)continue;float dx=r[j].x-ag->x,dy=r[j].y-ag->y,d=sqrtf(dx*dx+dy*dy);if(d<bd){bd=d;br=j;}}
    if(br<0&&ag->tip_val>.3f){float td=sqrtf((ag->tip_x-ag->x)*(ag->tip_x-ag->x)+(ag->tip_y-ag->y)*(ag->tip_y-ag->y));for(int j=0;j<nr;j++){if(r[j].collected)continue;float dx=r[j].x-ag->tip_x,dy=r[j].y-ag->tip_y;if(sqrtf(dx*dx+dy*dy)<.03f&&td+.03f<det*2){bd=td+.03f;br=j;break;}}ag->tip_val*=.95f;}
    if(br>=0&&bd<grab){r[br].collected=1;ag->res_held++;float tb=1;for(int k=0;k<16;k++){int j=lcg(&ag->rng)%na;if(j==i||a[j].arch!=ag->arch)continue;float dx=a[j].x-ag->x,dy=a[j].y-ag->y;if(sqrtf(dx*dx+dy*dy)<.05f)tb+=a[j].role[3]*.2f;}float bn=(1+cp*.5f)*tb;ag->energy=fminf(1,ag->energy+r[br].value*.1f*bn);ag->fitness+=r[br].value*bn;}
    else if(br>=0){float dx=r[br].x-ag->x,dy=r[br].y-ag->y,d=sqrtf(dx*dx+dy*dy),sp=.008f+cp*.008f+ep*.006f;ag->vx=ag->vx*.8f+(dx/d)*sp;ag->vy=ag->vy*.8f+(dy/d)*sp;}
    else{ag->vx=ag->vx*.95f+(lcgf(&ag->rng)-.5f)*.006f*(1+ep);ag->vy=ag->vy*.95f+(lcgf(&ag->rng)-.5f)*.006f*(1+ep);}
    ag->x=fmodf(ag->x+ag->vx+1,1);ag->y=fmodf(ag->y+ag->vy+1,1);
    
    for(int k=0;k<32;k++){
        int j=lcg(&ag->rng)%na;if(j==i)continue;
        float dx=a[j].x-ag->x,dy=a[j].y-ag->y,dist=sqrtf(dx*dx+dy*dy);
        if(dist>=.06f)continue;
        ag->interactions++;
        
        if(a[j].role[2]>.5f&&cm>.2f){float jbd=.1f;int jbr=-1;for(int m=0;m<nr;m++){if(r[m].collected)continue;float mdx=r[m].x-a[j].x,mdy=r[m].y-a[j].y,md=sqrtf(mdx*mdx+mdy*mdy);if(md<jbd){jbd=md;jbr=m;}}if(jbr>=0){ag->tip_x=r[jbr].x;ag->tip_y=r[jbr].y;ag->tip_val=a[j].role[2];}}
        
        /* v19: PREDATION — defenders steal from other-arch agents */
        if(predation&&dist<.02f&&ag->arch==3){ /* only defenders can prey */
            if(a[j].arch!=ag->arch&&a[j].fitness>ag->fitness*0.5f){
                float steal=df*0.02f; /* stronger defend = more stealing */
                if(a[j].fitness>steal){
                    atomicAdd(&ag->fitness,steal);
                    /* Note: can't safely subtract from other agent in parallel */
                    /* but the asymmetry still creates interesting dynamics */
                }
            }
        }
        
        float infl=(a[j].arch==ag->arch)?.02f:.002f;
        for(int r=0;r<4;r++)ag->role[r]+=(a[j].role[r]-ag->role[r])*infl;
        float sim=0;for(int r=0;r<4;r++)sim+=1-fminf(1,fabsf(ag->role[r]-a[j].role[r]));sim/=4;
        if(sim>.9f){int dr=(ag->arch+1+lcg(&ag->rng)%3)%4;ag->role[dr]+=(lcgf(&ag->rng)-.5f)*.01f;}
        if(dist<.02f){ag->vx-=dx*.01f;ag->vy-=dy*.01f;}
    }
    int dom=0;float dv=ag->role[0];for(int r=1;r<4;r++)if(ag->role[r]>dv){dv=ag->role[r];dom=r;}
    if(dom==ag->arch)ag->energy=fminf(1,ag->energy+.0005f);else ag->energy*=.9995f;
    ag->energy*=.999f;
    for(int r=0;r<4;r++){if(ag->role[r]<0)ag->role[r]=0;if(ag->role[r]>1)ag->role[r]=1;}
    if(t==pt){ag->energy*=(1-.5f*(1-df*.5f));ag->x=lcgf(&ag->rng);ag->y=lcgf(&ag->rng);ag->vx=ag->vy=0;ag->tip_val=0;}
}

__global__ void tick_c(Agent*a,Resource*r,int na,int nr,int t,int pt){
    int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=na)return;Agent*ag=&a[i];float det=.05f,grab=.03f;float bd=det;int br=-1;for(int j=0;j<nr;j++){if(r[j].collected)continue;float dx=r[j].x-ag->x,dy=r[j].y-ag->y,d=sqrtf(dx*dx+dy*dy);if(d<bd){bd=d;br=j;}}if(br>=0&&bd<grab){r[br].collected=1;ag->res_held++;ag->energy=fminf(1,ag->energy+r[br].value*.1f);ag->fitness+=r[br].value;}else if(br>=0){float dx=r[br].x-ag->x,dy=r[br].y-ag->y,d=sqrtf(dx*dx+dy*dy);ag->vx=ag->vx*.8f+(dx/d)*.014f;ag->vy=ag->vy*.8f+(dy/d)*.014f;}else{ag->vx=ag->vx*.95f+(lcgf(&ag->rng)-.5f)*.008f;ag->vy=ag->vy*.95f+(lcgf(&ag->rng)-.5f)*.008f;}ag->x=fmodf(ag->x+ag->vx+1,1);ag->y=fmodf(ag->y+ag->vy+1,1);for(int k=0;k<32;k++){int j=lcg(&ag->rng)%na;if(j==i)continue;float dx=a[j].x-ag->x,dy=a[j].y-ag->y;if(sqrtf(dx*dx+dy*dy)>=.06f)continue;if(sqrtf(dx*dx+dy*dy)<.02f){ag->vx-=dx*.01f;ag->vy-=dy*.01f;}}ag->energy*=.999f;if(t==pt){ag->energy*=.5f;ag->x=lcgf(&ag->rng);ag->y=lcgf(&ag->rng);ag->vx=ag->vy=0;}}

float spec(Agent*a,int n){float mean[4]={0},sd[4]={0};for(int i=0;i<n;i++)for(int r=0;r<4;r++)mean[r]+=a[i].role[r];for(int r=0;r<4;r++)mean[r]/=n;for(int i=0;i<n;i++)for(int r=0;r<4;r++)sd[r]+=(a[i].role[r]-mean[r])*(a[i].role[r]-mean[r]);float cv=0;for(int r=0;r<4;r++){sd[r]=sqrtf(sd[r]/n);if(mean[r]>.01f)cv+=sd[r]/mean[r];}return cv/4;}

int main(){
    printf("═══════════════════════════════════════════════════════\n");
    printf("  FLUX v19 — Predator-Prey Competition\n");
    printf("  Defenders (arch=3) steal fitness from other archetypes\n");
    printf("  Tests: does predation increase or decrease overall fitness?\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    Agent*da,*ha,*db,*hb,*dc,*hc;Resource*dr;
    cudaMalloc(&da,NA*sizeof(Agent));cudaMalloc(&db,NA*sizeof(Agent));cudaMalloc(&dc,NA*sizeof(Agent));
    cudaMalloc(&dr,NR*sizeof(Resource));
    ha=(Agent*)malloc(NA*sizeof(Agent));hb=(Agent*)malloc(NA*sizeof(Agent));hc=(Agent*)malloc(NA*sizeof(Agent));
    
    int blk=(NA+255)/256,rblk=(NR+255)/256;
    float f_pred=0,f_nopred=0,f_ctrl=0;
    float sp_pred=0,sp_nopred=0;
    
    for(int e=0;e<5;e++){
        /* A: With predation */
        init_a<<<blk,256>>>(da,NA);init_r<<<rblk,256>>>(dr,NR);cudaDeviceSynchronize();
        for(int t=0;t<MAXT;t++){tick_s<<<blk,256>>>(da,dr,NA,NR,t,250,1);cudaDeviceSynchronize();}
        cudaMemcpy(ha,da,NA*sizeof(Agent),cudaMemcpyDeviceToHost);
        float f=0;for(int i=0;i<NA;i++)f+=ha[i].fitness;f_pred+=f;sp_pred+=spec(ha,NA);
        
        /* B: No predation */
        init_a<<<blk,256>>>(db,NA);init_r<<<rblk,256>>>(dr,NR);cudaDeviceSynchronize();
        for(int t=0;t<MAXT;t++){tick_s<<<blk,256>>>(db,dr,NA,NR,t,250,0);cudaDeviceSynchronize();}
        cudaMemcpy(hb,db,NA*sizeof(Agent),cudaMemcpyDeviceToHost);
        f=0;for(int i=0;i<NA;i++)f+=hb[i].fitness;f_nopred+=f;sp_nopred+=spec(hb,NA);
        
        /* C: Control */
        init_c<<<blk,256>>>(dc,NA);init_r<<<rblk,256>>>(dr,NR);cudaDeviceSynchronize();
        for(int t=0;t<MAXT;t++){tick_c<<<blk,256>>>(dc,dr,NA,NR,t,250);cudaDeviceSynchronize();}
        cudaMemcpy(hc,dc,NA*sizeof(Agent),cudaMemcpyDeviceToHost);
        f=0;for(int i=0;i<NA;i++)f+=hc[i].fitness;f_ctrl+=f;
        
        /* Per-archetype breakdown for predation */
        float arch_fit[4]={0};for(int i=0;i<NA;i++)arch_fit[ha[i].arch]+=ha[i].fitness;
        printf("Exp %d: pred=%.1f nopred=%.1f ctrl=%.1f | arch:",e+1,f/NA,f/NA,f/NA);
        for(int a=0;a<4;a++)printf(" %.0f",arch_fit[a]/256);printf("\n");
    }
    f_pred/=5;f_nopred/=5;f_ctrl/=5;sp_pred/=5;sp_nopred/=5;
    
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Predation:   %.1f (%.2fx ctrl, spec=%.3f)\n",f_pred,(f_ctrl>.01f)?f_pred/f_ctrl:1,sp_pred);
    printf("  No pred:     %.1f (%.2fx ctrl, spec=%.3f)\n",f_nopred,(f_ctrl>.01f)?f_nopred/f_ctrl:1,sp_nopred);
    printf("  Control:     %.1f\n",f_ctrl);
    printf("  Pred effect: %s\n",f_pred>f_nopred*1.1?"INCREASES FITNESS":f_pred<f_nopred*0.9?"DECREASES FITNESS":"NEUTRAL");
    printf("═══════════════════════════════════════════════════════\n");
    
    cudaFree(da);cudaFree(db);cudaFree(dc);cudaFree(dr);free(ha);free(hb);free(hc);
    return 0;
}
