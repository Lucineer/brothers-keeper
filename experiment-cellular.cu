/* experiment-cellular.cu — Cellular Automata with Energy Physics
   256×256 grid, each cell has energy, state (alive/dead), and species (0-3).
   Energy flows between neighbors, cells with energy>threshold reproduce.
   Tests if Conway-like rules produce stable multi-species structures. */

#include <cstdio>
#include <cstdlib>
#include <cmath>

#define GRID 256
#define MAXT 300
#define N_SPECIES 4

__global__ void init_grid(float*energy,int*state,int*species){
    int x=blockIdx.x*blockDim.x+threadIdx.x;
    int y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=GRID||y>=GRID)return;
    int idx=y*GRID+x;
    unsigned int s=(unsigned int)(x*2654435761u+y*34057u+17);
    float r=(float)(((s*1103515245u+12345u)>>16)&0x7fff)/32768.0f;
    state[idx]=(r<0.3f)?1:0;
    energy[idx]=state[idx]?(0.3f+r*0.7f):0;
    species[idx]=state[idx]?((x+y)%N_SPECIES):0;
}

__global__ void step_grid(float*energy,int*state,int*species,float*energy_out,int*state_out,int*species_out,int t){
    int x=blockIdx.x*blockDim.x+threadIdx.x;
    int y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=GRID||y>=GRID)return;
    int idx=y*GRID+x;
    int xm=(x-1+GRID)%GRID,xp=(x+1)%GRID,ym=(y-1+GRID)%GRID,yp=(y+1)%GRID;
    // Count neighbors by species
    int n_total=0,n_same=0,n_by_spec[N_SPECIES]={0};
    float n_energy=0;
    int nbs[8]={ym*GRID+xm,ym*GRID+x,ym*GRID+xp,y*GRID+xm,y*GRID+xp,yp*GRID+xm,yp*GRID+x,yp*GRID+xp};
    for(int i=0;i<8;i++){n_total+=state[nbs[i]];n_energy+=energy[nbs[i]];n_by_spec[species[nbs[i]]]+=state[nbs[i]];}
    n_same=n_by_spec[species[idx]];
    
    float e=energy[idx];
    int st=state[idx];int sp=species[idx];
    
    if(st){
        // Alive cell: consume energy, interact with neighbors
        float cost=0.01f+0.005f*n_total; // crowding cost
        float gain=n_energy*0.01f; // absorb from neighborhood
        // Same-species cooperation bonus
        if(n_same>=2)gain+=n_same*0.005f;
        // Different-species competition penalty
        for(int s=0;s<N_SPECIES;s++)if(s!=sp)gain-=n_by_spec[s]*0.003f;
        
        e+=gain-cost;
        // Die if no energy
        if(e<=0){st=0;e=0;}
        // Reproduce if enough energy and space
        else if(e>1.5f&&n_total<6){
            for(int i=0;i<8;i++){
                int ni=nbs[i];
                if(!state_out[ni]){
                    state_out[ni]=1;species_out[ni]=sp;
                    energy_out[ni]=e*0.3f;e*=0.5f;break;
                }
            }
        }
    }else{
        // Dead cell: spontaneous birth if surrounded by enough life
        if(n_total>=3&&n_energy>0.5f){
            // Born as majority species
            int best=0;for(int s=1;s<N_SPECIES;s++)if(n_by_spec[s]>n_by_spec[best])best=s;
            st=1;sp=best;e=n_energy*0.1f;
        }
    }
    
    // Periodic energy injection (like sunlight)
    if(t%50==0&&st)e+=0.2f;
    
    energy_out[idx]=fmaxf(0,e);
    if(state_out[idx]==0&&st==0)species_out[idx]=0; // don't overwrite births
    else if(st)species_out[idx]=sp;
}

__global__ void measure(float*energy,int*state,int*species,float*total_e,int*alive,int*spec_count,float*spec_energy){
    __shared__ float te;__shared__ int ta;__shared__ int sc[N_SPECIES];__shared__ float se[N_SPECIES];
    if(threadIdx.x==0&&threadIdx.y==0){te=0;ta=0;for(int s=0;s<N_SPECIES;s++){sc[s]=0;se[s]=0;}}
    __syncthreads();
    int x=blockIdx.x*blockDim.x+threadIdx.x;
    int y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x<GRID&&y<GRID){
        int idx=y*GRID+x;
        atomicAdd(&te,energy[idx]);
        if(state[idx]){atomicAdd(&ta,1);atomicAdd(&sc[species[idx]],1);atomicAdd(&se[species[idx]],energy[idx]);}
    }
    __syncthreads();
    if(threadIdx.x==0&&threadIdx.y==0){*total_e=te;*alive=ta;for(int s=0;s<N_SPECIES;s++){spec_count[s]=sc[s];spec_energy[s]=se[s];}}
}

int main(){
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Experiment 3: Cellular Automata with Energy Physics\n");
    printf("  256x256 grid, 4 species, 300 ticks\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    dim3 blk(16,16),grid((GRID+15)/16,(GRID+15)/16);
    float*d_e,*d_e2,*h_e,*h_se;int*d_s,*d_s2,*h_s,*d_sp,*d_sp2,*h_sp;
    cudaMalloc(&d_e,GRID*GRID*sizeof(float));cudaMalloc(&d_e2,GRID*GRID*sizeof(float));
    cudaMalloc(&d_s,GRID*GRID*sizeof(int));cudaMalloc(&d_s2,GRID*GRID*sizeof(int));
    cudaMalloc(&d_sp,GRID*GRID*sizeof(int));cudaMalloc(&d_sp2,GRID*GRID*sizeof(int));
    float*d_te,*d_se;int*d_alive,*d_sc;
    cudaMalloc(&d_te,sizeof(float));cudaMalloc(&d_se,N_SPECIES*sizeof(float));
    cudaMalloc(&d_alive,sizeof(int));cudaMalloc(&d_sc,N_SPECIES*sizeof(int));
    h_e=(float*)malloc(sizeof(float));h_s=(int*)malloc(sizeof(int));
    h_sp=(int*)malloc(N_SPECIES*sizeof(int));h_se=(float*)malloc(N_SPECIES*sizeof(float));
    
    init_grid<<<grid,blk>>>(d_e,d_s,d_sp);cudaDeviceSynchronize();
    int peak_alive=0,extinct_tick=-1;
    for(int t=0;t<MAXT;t++){
        cudaMemset(d_e2,0,GRID*GRID*sizeof(float));
        cudaMemset(d_s2,0,GRID*GRID*sizeof(int));
        cudaMemset(d_sp2,0,GRID*GRID*sizeof(int));
        step_grid<<<grid,blk>>>(d_e,d_s,d_sp,d_e2,d_s2,d_sp2,t);cudaDeviceSynchronize();
        // Swap
        cudaMemcpy(d_e,d_e2,GRID*GRID*sizeof(float),cudaMemcpyDeviceToDevice);
        cudaMemcpy(d_s,d_s2,GRID*GRID*sizeof(int),cudaMemcpyDeviceToDevice);
        cudaMemcpy(d_sp,d_sp2,GRID*GRID*sizeof(int),cudaMemcpyDeviceToDevice);
        
        if(t%25==0){
            measure<<<grid,blk>>>(d_e,d_s,d_sp,d_te,d_alive,d_sc,d_se);cudaDeviceSynchronize();
            cudaMemcpy(h_e,d_te,sizeof(float),cudaMemcpyDeviceToHost);
            cudaMemcpy(h_s,d_alive,sizeof(int),cudaMemcpyDeviceToHost);
            cudaMemcpy(h_sp,d_sc,N_SPECIES*sizeof(int),cudaMemcpyDeviceToHost);
            cudaMemcpy(h_se,d_se,N_SPECIES*sizeof(float),cudaMemcpyDeviceToHost);
            if(*h_s>peak_alive)peak_alive=*h_s;
            int alive_species=0;for(int s=0;s<N_SPECIES;s++)if(h_sp[s]>0)alive_species++;
            if(alive_species==0&&extinct_tick<0)extinct_tick=t;
            printf("t=%3d: alive=%5d energy=%.0f species=[%d %d %d %d]\n",t,*h_s,*h_e,h_sp[0],h_sp[1],h_sp[2],h_sp[3]);
        }
    }
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("Peak alive: %d (%.1f%% of grid)\n",peak_alive,100.0f*peak_alive/(GRID*GRID));
    printf("Extinction: %s\n",extinct_tick>=0?"YES":"NO (coexistence)");
    if(extinct_tick>=0)printf("All extinct at tick %d\n",extinct_tick);
    printf("═══════════════════════════════════════════════════════\n");
    cudaFree(d_e);cudaFree(d_e2);cudaFree(d_s);cudaFree(d_s2);cudaFree(d_sp);cudaFree(d_sp2);
    cudaFree(d_te);cudaFree(d_se);cudaFree(d_alive);cudaFree(d_sc);
    free(h_e);free(h_s);free(h_sp);free(h_se);
    return 0;
}
