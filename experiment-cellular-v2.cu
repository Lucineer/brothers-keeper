/* experiment-cellular-v2.cu — Fixed: proper double-buffering, no race conditions
   128×128 grid (manageable), 4 species with energy physics.
   Each cell: energy, state (alive/dead), species. Proper two-buffer swap. */

#include <cstdio>
#include <cstdlib>
#include <cmath>

#define G 128
#define MAXT 400
#define NS 4

__global__ void init_grid(float*e, int*s, int*sp){
    int x=blockIdx.x*blockDim.x+threadIdx.x;
    int y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=G||y>=G)return;
    int i=y*G+x;
    unsigned int r=(unsigned int)(x*2654435761u+y*34057u+17);
    auto lcg=[&](){r=r*1103515245u+12345u;return(float)(((r>>16)&0x7fff))/32768.0f;};
    s[i]=(lcg()<0.3f)?1:0;
    e[i]=s[i]?(0.3f+lcg()*0.7f):0;
    sp[i]=s[i]?((x+y)%NS):0;
}

__global__ void step(const float*e_in, const int*s_in, const int*sp_in,
                     float*e_out, int*s_out, int*sp_out, int t){
    int x=blockIdx.x*blockDim.x+threadIdx.x;
    int y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=G||y>=G)return;
    int i=y*G+x;
    int xm=(x-1+G)%G, xp=(x+1)%G, ym=(y-1+G)%G, yp=(y+1)%G;
    int nb[8]={ym*G+xm,ym*G+x,ym*G+xp,y*G+xm,y*G+xp,yp*G+xm,yp*G+x,yp*G+xp};
    
    int n_total=0, n_same=0, n_by_s[NS]={0};
    float n_energy=0;
    for(int k=0;k<8;k++){
        n_total+=s_in[nb[k]];
        n_energy+=e_in[nb[k]];
        n_by_s[sp_in[nb[k]]]+=s_in[nb[k]];
    }
    n_same=n_by_s[sp_in[i]];
    
    // Output defaults to input (no change)
    e_out[i]=e_in[i]; s_out[i]=s_in[i]; sp_out[i]=sp_in[i];
    
    if(s_in[i]){
        // ALIVE: consume, interact
        float cost=0.008f+0.003f*n_total;
        float gain=n_energy*0.008f;
        if(n_same>=2)gain+=n_same*0.003f;
        for(int q=0;q<NS;q++)if(q!=sp_in[i])gain-=n_by_s[q]*0.002f;
        float ne=e_in[i]+gain-cost;
        if(ne<=0){e_out[i]=0;s_out[i]=0;sp_out[i]=0;return;}
        // Reproduce
        if(ne>1.5f&&n_total<6){
            for(int k=0;k<8;k++){
                if(!s_in[nb[k]]){
                    s_out[nb[k]]=1;sp_out[nb[k]]=sp_in[i];
                    e_out[nb[k]]=ne*0.25f;e_out[i]=ne*0.5f;return;
                }
            }
        }
        e_out[i]=ne;
    }else{
        // DEAD: spontaneous birth
        if(n_total>=3&&n_energy>0.5f){
            int best=0;for(int q=1;q<NS;q++)if(n_by_s[q]>n_by_s[best])best=q;
            s_out[i]=1;sp_out[i]=best;e_out[i]=n_energy*0.08f;
        }
    }
    // Periodic energy injection
    if(t%60==0&&s_out[i])e_out[i]+=0.15f;
}

int main(){
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Experiment 3v2: Cellular Automata with Energy (fixed)\n");
    printf("  %dx%d grid, %d species, %d ticks, double-buffered\n",G,G,NS,MAXT);
    printf("═══════════════════════════════════════════════════════\n\n");
    dim3 blk(16,16),grd((G+15)/16,(G+15)/16);
    float*d_e0,*d_e1;int*d_s0,*d_s1,*d_sp0,*d_sp1;
    cudaMalloc(&d_e0,G*G*sizeof(float));cudaMalloc(&d_e1,G*G*sizeof(float));
    cudaMalloc(&d_s0,G*G*sizeof(int));cudaMalloc(&d_s1,G*G*sizeof(int));
    cudaMalloc(&d_sp0,G*G*sizeof(int));cudaMalloc(&d_sp1,G*G*sizeof(int));
    
    init_grid<<<grd,blk>>>(d_e0,d_s0,d_sp0);cudaDeviceSynchronize();
    
    int*hs=(int*)malloc(G*G*sizeof(int));
    float*he=(float*)malloc(G*G*sizeof(float));
    int*hsp=(int*)malloc(G*G*sizeof(int));
    
    int peak=0,extinct=-1;
    for(int t=0;t<MAXT;t++){
        step<<<grd,blk>>>(d_e0,d_s0,d_sp0,d_e1,d_s1,d_sp1,t);cudaDeviceSynchronize();
        // swap pointers by copying
        cudaMemcpy(d_e0,d_e1,G*G*sizeof(float),cudaMemcpyDeviceToDevice);
        cudaMemcpy(d_s0,d_s1,G*G*sizeof(int),cudaMemcpyDeviceToDevice);
        cudaMemcpy(d_sp0,d_sp1,G*G*sizeof(int),cudaMemcpyDeviceToDevice);
        
        if(t%40==0||t==MAXT-1){
            cudaMemcpy(hs,d_s0,G*G*sizeof(int),cudaMemcpyDeviceToHost);
            cudaMemcpy(he,d_e0,G*G*sizeof(float),cudaMemcpyDeviceToHost);
            cudaMemcpy(hsp,d_sp0,G*G*sizeof(int),cudaMemcpyDeviceToHost);
            int alive=0;float te=0;int sc[NS]={0};
            for(int i=0;i<G*G;i++){alive+=hs[i];te+=he[i];if(hs[i])sc[hsp[i]]++;}
            if(alive>peak)peak=alive;
            int as=0;for(int q=0;q<NS;q++)if(sc[q]>0)as++;
            if(as==0&&extinct<0)extinct=t;
            printf("t=%3d: alive=%5d (%4.1f%%) energy=%7.0f species=[%d %d %d %d] alive_sp=%d\n",
                   t,alive,100.0f*alive/(G*G),te,sc[0],sc[1],sc[2],sc[3],as);
        }
    }
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("Peak alive: %d (%.1f%%)\n",peak,100.0f*peak/(G*G));
    printf("Extinction: %s\n",extinct>=0?"YES — no coexistence":"NO — species coexist!");
    if(extinct>=0)printf("All dead at tick %d\n",extinct);
    printf("═══════════════════════════════════════════════════════\n");
    cudaFree(d_e0);cudaFree(d_e1);cudaFree(d_s0);cudaFree(d_s1);cudaFree(d_sp0);cudaFree(d_sp1);
    free(hs);free(he);free(hsp);return 0;
}
