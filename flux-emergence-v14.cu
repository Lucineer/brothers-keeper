/* flux-emergence-v14.cu — Stigmergy: agents leave pheromone trails.
   Novel: when an agent collects a resource, it deposits a pheromone at that location.
   Other agents can detect pheromones and are attracted to them.
   Prediction: pheromones amplify specialist advantage by 20%+. */

#include <cstdio>
#include <cstdlib>
#include <cmath>

#define NA 1024
#define NR 128
#define MAXT 500
#define N_ARCH 4
#define GRID 32
#define PHER_DECAY 0.995f

__device__ __host__ unsigned int lcg(unsigned int*s){*s=*s*1103515245u+12345u;return(*s>>16)&0x7fff;}
__device__ __host__ float lcgf(unsigned int*s){return(float)lcg(s)/32768.0f;}

typedef struct{float x,y,vx,vy,energy,role[4],fitness;int arch,res_held,interactions,group;float tip_x,tip_y,tip_val;unsigned int rng;}Agent;
typedef struct{float x,y,value;int collected;}Resource;

__global__ void init_a(Agent*a,int n){int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=n)return;a[i].rng=(unsigned int)(i*2654435761u+17);a[i].x=lcgf(&a[i].rng);a[i].y=lcgf(&a[i].rng);a[i].vx=a[i].vy=0;a[i].energy=.5f+lcgf(&a[i].rng)*.5f;a[i].arch=i%N_ARCH;a[i].fitness=a[i].res_held=a[i].interactions=0;a[i].group=-1;a[i].tip_x=a[i].tip_y=a[i].tip_val=0;for(int r=0;r<4;r++){float b=(r==a[i].arch)?.7f:.1f;a[i].role[r]=b+(lcgf(&a[i].rng)-.5f)*.4f;}}

__global__ void init_c(Agent*a,int n){int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=n)return;a[i].rng=(unsigned int)(i*2654435761u+99917);a[i].x=lcgf(&a[i].rng);a[i].y=lcgf(&a[i].rng);a[i].vx=a[i].vy=0;a[i].energy=.5f+lcgf(&a[i].rng)*.5f;a[i].arch=i%N_ARCH;a[i].fitness=a[i].res_held=a[i].interactions=0;a[i].group=-1;a[i].tip_x=a[i].tip_y=a[i].tip_val=0;for(int r=0;r<4;r++)a[i].role[r]=.25f;}

__global__ void init_r(Resource*r,int n){int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=n)return;unsigned int s=(unsigned int)(i*2654435761u+99999);r[i].x=lcgf(&s);r[i].y=lcgf(&s);r[i].value=.5f+lcgf(&s)*.5f;r[i].collected=0;}

__global__ void init_pher(float*p,int n){int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=n)return;p[i]=0;}

__device__ float sample_pher(float*p,float x,float y){
    int gx=(int)(x*GRID)%GRID, gy=(int)(y*GRID)%GRID;
    if(gx<0)gx+=GRID;if(gy<0)gy+=GRID;
    float total=0,cnt=0;
    for(int dx=-1;dx<=1;dx++)for(int dy=-1;dy<=1;dy++){
        int nx=(gx+dx+GRID)%GRID, ny=(gy+dy+GRID)%GRID;
        total+=p[nx*GRID+ny]; cnt++;
    }
    return total/cnt;
}

__device__ void deposit_pher(float*p,float x,float y,float amt){
    int gx=(int)(x*GRID)%GRID, gy=(int)(y*GRID)%GRID;
    if(gx<0)gx+=GRID;if(gy<0)gy+=GRID;
    atomicAdd(&p[gx*GRID+gy],amt);
}

__global__ void tick_s(Agent*a,Resource*r,float*pher,int na,int nr,int t,int pt){
    int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=na)return;
    Agent*ag=&a[i];
    float ep=ag->role[0],cp=ag->role[1],cm=ag->role[2],df=ag->role[3];
    float det=.03f+ep*.04f, grab=.02f+cp*.02f;
    
    /* Pheromone attraction: adds to movement bias */
    float pv=sample_pher(pher,ag->x,ag->y);
    float pher_dx=0,pher_dy=0;
    if(pv>0.01f){
        /* Gradient: sample neighbors to find direction */
        float sl=sample_pher(pher,ag->x-.02f,ag->y);
        float sr_=sample_pher(pher,ag->x+.02f,ag->y);
        float su=sample_pher(pher,ag->x,ag->y-.02f);
        float sd=sample_pher(pher,ag->x,ag->y+.02f);
        pher_dx=(sr_-sl)*5.0f; pher_dy=(sd-su)*5.0f;
    }
    
    float bd=det;int br=-1;
    for(int j=0;j<nr;j++){if(r[j].collected)continue;float dx=r[j].x-ag->x,dy=r[j].y-ag->y,d=sqrtf(dx*dx+dy*dy);if(d<bd){bd=d;br=j;}}
    if(br<0&&ag->tip_val>.3f){float td=sqrtf((ag->tip_x-ag->x)*(ag->tip_x-ag->x)+(ag->tip_y-ag->y)*(ag->tip_y-ag->y));for(int j=0;j<nr;j++){if(r[j].collected)continue;float dx=r[j].x-ag->tip_x,dy=r[j].y-ag->tip_y;if(sqrtf(dx*dx+dy*dy)<.03f&&td+.03f<det*2){bd=td+.03f;br=j;break;}}ag->tip_val*=.95f;}
    
    if(br>=0&&bd<grab){
        r[br].collected=1;ag->res_held++;
        deposit_pher(pher,r[br].x,r[br].y,0.3f); /* Leave pheromone! */
        float tb=1;for(int k=0;k<16;k++){int j=lcg(&ag->rng)%na;if(j==i||a[j].arch!=ag->arch)continue;float dx=a[j].x-ag->x,dy=a[j].y-ag->y;if(sqrtf(dx*dx+dy*dy)<.05f)tb+=a[j].role[3]*.2f;}
        float bn=(1+cp*.5f)*tb;ag->energy=fminf(1,ag->energy+r[br].value*.1f*bn);ag->fitness+=r[br].value*bn;
    }else if(br>=0){
        float dx=r[br].x-ag->x,dy=r[br].y-ag->y,d=sqrtf(dx*dx+dy*dy),sp=.008f+cp*.008f+ep*.006f;
        ag->vx=ag->vx*.8f+(dx/d)*sp;ag->vy=ag->vy*.8f+(dy/d)*sp;
    }else{
        ag->vx=ag->vx*.95f+(lcgf(&ag->rng)-.5f)*.006f*(1+ep);
        ag->vy=ag->vy*.95f+(lcgf(&ag->rng)-.5f)*.006f*(1+ep);
    }
    /* Add pheromone gradient to velocity */
    ag->vx+=pher_dx*0.002f; ag->vy+=pher_dy*0.002f;
    ag->x=fmodf(ag->x+ag->vx+1,1);ag->y=fmodf(ag->y+ag->vy+1,1);
    
    for(int k=0;k<32;k++){int j=lcg(&ag->rng)%na;if(j==i)continue;float dx=a[j].x-ag->x,dy=a[j].y-ag->y,dist=sqrtf(dx*dx+dy*dy);if(dist>=.06f)continue;ag->interactions++;if(a[j].role[2]>.5f&&cm>.2f){float jbd=.1f;int jbr=-1;for(int m=0;m<nr;m++){if(r[m].collected)continue;float mdx=r[m].x-a[j].x,mdy=r[m].y-a[j].y,md=sqrtf(mdx*mdx+mdy*mdy);if(md<jbd){jbd=md;jbr=m;}}if(jbr>=0){ag->tip_x=r[jbr].x;ag->tip_y=r[jbr].y;ag->tip_val=a[j].role[2];}}float infl=(a[j].arch==ag->arch)?.02f:.002f;for(int r=0;r<4;r++)ag->role[r]+=(a[j].role[r]-ag->role[r])*infl;float sim=0;for(int r=0;r<4;r++)sim+=1-fminf(1,fabsf(ag->role[r]-a[j].role[r]));sim/=4;if(sim>.9f){int dr=(ag->arch+1+lcg(&ag->rng)%3)%4;ag->role[dr]+=(lcgf(&ag->rng)-.5f)*.01f;}if(dist<.02f){ag->vx-=dx*.01f;ag->vy-=dy*.01f;}}
    int dom=0;float dv=ag->role[0];for(int r=1;r<4;r++)if(ag->role[r]>dv){dv=ag->role[r];dom=r;}if(dom==ag->arch)ag->energy=fminf(1,ag->energy+.0005f);else ag->energy*=.9995f;ag->energy*=.999f;for(int r=0;r<4;r++){if(ag->role[r]<0)ag->role[r]=0;if(ag->role[r]>1)ag->role[r]=1;}if(t==pt){ag->energy*=(1-.5f*(1-df*.5f));ag->x=lcgf(&ag->rng);ag->y=lcgf(&ag->rng);ag->vx=ag->vy=0;ag->tip_val=0;}
}



__global__ void decay_pher(float*p,int n){
    int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i<n) p[i]*=PHER_DECAY;
}

__global__ void tick_c(Agent*a,Resource*r,float*pher,int na,int nr,int t,int pt){
    int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=na)return;Agent*ag=&a[i];float det=.05f,grab=.03f;float bd=det;int br=-1;for(int j=0;j<nr;j++){if(r[j].collected)continue;float dx=r[j].x-ag->x,dy=r[j].y-ag->y,d=sqrtf(dx*dx+dy*dy);if(d<bd){bd=d;br=j;}}if(br>=0&&bd<grab){r[br].collected=1;ag->res_held++;ag->energy=fminf(1,ag->energy+r[br].value*.1f);ag->fitness+=r[br].value;}else if(br>=0){float dx=r[br].x-ag->x,dy=r[br].y-ag->y,d=sqrtf(dx*dx+dy*dy);ag->vx=ag->vx*.8f+(dx/d)*.014f;ag->vy=ag->vy*.8f+(dy/d)*.014f;}else{ag->vx=ag->vx*.95f+(lcgf(&ag->rng)-.5f)*.008f;ag->vy=ag->vy*.95f+(lcgf(&ag->rng)-.5f)*.008f;}ag->x=fmodf(ag->x+ag->vx+1,1);ag->y=fmodf(ag->y+ag->vy+1,1);for(int k=0;k<32;k++){int j=lcg(&ag->rng)%na;if(j==i)continue;float dx=a[j].x-ag->x,dy=a[j].y-ag->y;if(sqrtf(dx*dx+dy*dy)>=.06f)continue;ag->interactions++;if(sqrtf(dx*dx+dy*dy)<.02f){ag->vx-=dx*.01f;ag->vy-=dy*.01f;}}ag->energy*=.999f;if(t==pt){ag->energy*=.5f;ag->x=lcgf(&ag->rng);ag->y=lcgf(&ag->rng);ag->vx=ag->vy=0;}}

int main(){
    printf("═══════════════════════════════════════════════════════\n");
    printf("  FLUX v14 — Stigmergy (Pheromone Trails)\n");
    printf("  Novel: agents deposit pheromones at collected resources\n");
    printf("  Others follow pheromone gradient to find resources\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    
    int pn=GRID*GRID;
    Agent*da,*ha,*db,*hb;Resource*dr;float*pher,*pher_zero;
    cudaMalloc(&da,NA*sizeof(Agent));cudaMalloc(&db,NA*sizeof(Agent));
    cudaMalloc(&dr,NR*sizeof(Resource));cudaMalloc(&pher,pn*sizeof(float));
    cudaMalloc(&pher_zero,pn*sizeof(float));
    ha=(Agent*)malloc(NA*sizeof(Agent));hb=(Agent*)malloc(NA*sizeof(Agent));
    cudaMemset(pher_zero,0,pn*sizeof(float));
    
    int blk=(NA+255)/256,rblk=(NR+255)/256,pblk=(pn+255)/256;
    float fs_pher=0,fs_base=0,fc=0;
    
    for(int e=0;e<5;e++){
        /* A: Specialized + pheromones */
        init_a<<<blk,256>>>(da,NA);init_r<<<rblk,256>>>(dr,NR);init_pher<<<pblk,256>>>(pher,pn);cudaDeviceSynchronize();
        for(int t=0;t<MAXT;t++){tick_s<<<blk,256>>>(da,dr,pher,NA,NR,t,250);decay_pher<<<pblk,256>>>(pher,pn);cudaDeviceSynchronize();}
        cudaMemcpy(ha,da,NA*sizeof(Agent),cudaMemcpyDeviceToHost);
        float f=0;for(int i=0;i<NA;i++)f+=ha[i].fitness;fs_pher+=f;
        
        /* B: Specialized without pheromones (v8 baseline) */
        init_a<<<blk,256>>>(da,NA);init_r<<<rblk,256>>>(dr,NR);cudaMemcpy(pher,pher_zero,pn*sizeof(float),cudaMemcpyDeviceToDevice);cudaDeviceSynchronize();
        for(int t=0;t<MAXT;t++){tick_s<<<blk,256>>>(da,dr,pher,NA,NR,t,250);cudaDeviceSynchronize();}
        cudaMemcpy(ha,da,NA*sizeof(Agent),cudaMemcpyDeviceToHost);
        f=0;for(int i=0;i<NA;i++)f+=ha[i].fitness;fs_base+=f;
        
        /* C: Control */
        init_c<<<blk,256>>>(db,NA);init_r<<<rblk,256>>>(dr,NR);cudaDeviceSynchronize();
        for(int t=0;t<MAXT;t++){tick_c<<<blk,256>>>(db,dr,pher,NA,NR,t,250);cudaDeviceSynchronize();}
        cudaMemcpy(hb,db,NA*sizeof(Agent),cudaMemcpyDeviceToHost);
        f=0;for(int i=0;i<NA;i++)f+=hb[i].fitness;fc+=f;
    }
    fs_pher/=5;fs_base/=5;fc/=5;
    float r_pher=(fc>.01f)?fs_pher/fc:1;
    float r_base=(fc>.01f)?fs_base/fc:1;
    float boost=(r_base>.01f)?r_pher/r_base:1;
    
    printf("Specialized+pher: %.1f (ratio: %.2fx)\n",fs_pher,r_pher);
    printf("Specialized base: %.1f (ratio: %.2fx)\n",fs_base,r_base);
    printf("Control:          %.1f\n",fc);
    printf("Pheromone boost:  %.2fx (%+.0f%%)\n",boost,(boost-1)*100);
    printf("\nVerdict: %s\n",boost>1.15?"PHEROMONES HELP":boost>1.05?"MARGINAL":"NO EFFECT");
    
    cudaFree(da);cudaFree(db);cudaFree(dr);cudaFree(pher);cudaFree(pher_zero);free(ha);free(hb);
    return 0;
}
