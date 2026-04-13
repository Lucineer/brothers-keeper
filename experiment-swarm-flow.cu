/* experiment-swarm-flow.cu — Parallel Flow Field Pathfinding
   256×256 grid with obstacles. BFS computes flow field, 1024 agents follow it.
   Tests GPU parallel pathfinding for Minecraft bot scale. */

#include <cstdio>
#include <cstdlib>
#include <cmath>

#define GRID 256
#define N_AGENTS 1024
#define MAXT 200

__global__ void init_obstacles(int*obs,int*goal,float*flow_x,float*flow_y){
    int x=blockIdx.x*blockDim.x+threadIdx.x;
    int y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=GRID||y>=GRID)return;
    int idx=y*GRID+x;
    // Border walls
    obs[idx]=(x==0||y==0||x==GRID-1||y==GRID-1)?1:0;
    // Some internal walls (L-shape and blocks)
    if(x==64&&y>=64&&y<=192)obs[idx]=1;
    if(x==128&&y>=64&&y<=128)obs[idx]=1;
    if(x>=96&&x<=128&&y==128)obs[idx]=1;
    if(x>=160&&x<=192&&y>=96&&y<=192)obs[idx]=1;
    if(x>=32&&x<=64&&y>=160&&y<=192)obs[idx]=1;
    // Goal
    if(x==220&&y==220){goal[0]=x;goal[1]=y;obs[idx]=0;}
    flow_x[idx]=0;flow_y[idx]=0;
}

__global__ void bfs_pass(const int*obs,const int*goal,int*dist,float*flow_x,float*flow_y,int pass){
    int x=blockIdx.x*blockDim.x+threadIdx.x;
    int y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=GRID||y>=GRID)return;
    int idx=y*GRID+x;
    if(obs[idx])return;
    // Process cells at distance == pass
    if(dist[idx]!=pass)return;
    // Check 4 neighbors for distance pass-1
    int dx[]={-1,1,0,0},dy[]={0,0,-1,1};
    for(int d=0;d<4;d++){
        int nx=x+dx[d],ny=y+dy[d];
        if(nx<0||nx>=GRID||ny<0||ny>=GRID)continue;
        int ni=ny*GRID+nx;
        if(obs[ni])continue;
        if(dist[ni]==pass-1){
            // Flow toward this neighbor (shorter path)
            flow_x[idx]=(float)dx[d];
            flow_y[idx]=(float)dy[d];
            break; // take first shortest
        }
    }
}

__global__ void init_dist(int*dist,const int*goal){
    int x=blockIdx.x*blockDim.x+threadIdx.x;
    int y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=GRID||y>=GRID)return;
    dist[y*GRID+x]=(x==goal[0]&&y==goal[1])?0:99999;
}

__global__ void bfs_expand(const int*obs,int*dist,int current_d){
    int x=blockIdx.x*blockDim.x+threadIdx.x;
    int y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=GRID||y>=GRID)return;
    int idx=y*GRID+x;
    if(obs[idx]||dist[idx]!=99999)return;
    // Check if any neighbor has distance current_d
    int dx[]={-1,1,0,0},dy[]={0,0,-1,1};
    for(int d=0;d<4;d++){
        int nx=x+dx[d],ny=y+dy[d];
        if(nx<0||nx>=GRID||ny<0||ny>=GRID)continue;
        if(dist[ny*GRID+nx]==current_d){dist[idx]=current_d+1;return;}
    }
}

__global__ void init_agents(float*ax,float*ay,unsigned int seed){
    int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=N_AGENTS)return;
    unsigned int s=seed+i*2654435761u;
    auto rng=[&](){s=s*1103515245u+12345u;return(float)(((s>>16)&0x7fff))/32768.0f;};
    // Spawn in top-left quadrant
    ax[i]=10+rng()*50;
    ay[i]=10+rng()*50;
}

__global__ void move_agents(float*ax,float*ay,const float*flow_x,const float*flow_y,
                            const int*obs,const int*goal,int*n_reached){
    int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=N_AGENTS)return;
    int gx=(int)ax[i],gy=(int)ay[i];
    if(gx<0||gx>=GRID||gy<0||gy>=GRID)return;
    // Check if at goal
    if(gx==goal[0]&&gy==goal[1]){atomicAdd(n_reached,1);return;}
    // Follow flow field
    float fx=flow_x[gy*GRID+gx],fy=flow_y[gy*GRID+gx];
    if(fx==0&&fy==0)return; // no path
    int nx=(int)(ax[i]+fx),ny=(int)(ay[i]+fy);
    if(nx>=0&&nx<GRID&&ny>=0&&ny<GRID&&!obs[ny*GRID+nx]){
        ax[i]=nx;ay[i]=ny;
    }
}

int main(){
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Experiment 5: Swarm Flow Field Pathfinding\n");
    printf("  256x256 grid, %d agents, obstacles, BFS flow field\n",N_AGENTS);
    printf("═══════════════════════════════════════════════════════\n\n");
    dim3 blk(16,16),grid_dim((GRID+15)/16,(GRID+15)/16);
    int*d_obs,*d_dist,*d_goal,*d_reached;float*d_fx,*d_fy,*d_ax,*d_ay;
    cudaMalloc(&d_obs,GRID*GRID*sizeof(int));
    cudaMalloc(&d_dist,GRID*GRID*sizeof(int));
    cudaMalloc(&d_goal,2*sizeof(int));
    cudaMalloc(&d_fx,GRID*GRID*sizeof(float));
    cudaMalloc(&d_fy,GRID*GRID*sizeof(float));
    cudaMalloc(&d_ax,N_AGENTS*sizeof(float));
    cudaMalloc(&d_ay,N_AGENTS*sizeof(float));
    cudaMalloc(&d_reached,sizeof(int));
    
    init_obstacles<<<grid_dim,blk>>>(d_obs,d_goal,d_fx,d_fy);cudaDeviceSynchronize();
    
    // BFS on GPU (expand layer by layer)
    init_dist<<<grid_dim,blk>>>(d_dist,d_goal);cudaDeviceSynchronize();
    printf("Computing BFS flow field...\n");
    cudaEvent_t start,stop;cudaEventCreate(&start);cudaEventCreate(&stop);
    cudaEventRecord(start);
    for(int d=0;d<500;d++){
        bfs_expand<<<grid_dim,blk>>>(d_obs,d_dist,d);cudaDeviceSynchronize();
    }
    // Compute flow from distances
    for(int d=1;d<500;d++){
        bfs_pass<<<grid_dim,blk>>>(d_obs,d_goal,d_dist,d_fx,d_fy,d);cudaDeviceSynchronize();
    }
    cudaEventRecord(stop);cudaEventSynchronize(stop);
    float bfs_ms=0;cudaEventElapsedTime(&bfs_ms,start,stop);
    printf("BFS + flow: %.1f ms\n",bfs_ms);
    
    // Count reachable cells
    int*dist=(int*)malloc(GRID*GRID*sizeof(int));
    cudaMemcpy(dist,d_dist,GRID*GRID*sizeof(int),cudaMemcpyDeviceToHost);
    int reachable=0;for(int i=0;i<GRID*GRID;i++)if(dist[i]<99999)reachable++;
    printf("Reachable cells: %d/%d (%.1f%%)\n",reachable,GRID*GRID,100.0f*reachable/(GRID*GRID));
    free(dist);
    
    // Move agents
    init_agents<<<1,N_AGENTS>>>(d_ax,d_ay,42);cudaDeviceSynchronize();
    int reached=0;int h_reached=0;
    printf("\nAgent simulation:\n");
    int abl=(N_AGENTS+255)/256;
    for(int t=0;t<MAXT;t++){
        cudaMemcpy(d_reached,&h_reached,sizeof(int),cudaMemcpyHostToDevice);
        move_agents<<<abl,256>>>(d_ax,d_ay,d_fx,d_fy,d_obs,d_goal,d_reached);cudaDeviceSynchronize();
        cudaMemcpy(&h_reached,d_reached,sizeof(int),cudaMemcpyDeviceToHost);
        if(t%25==0)printf("  t=%3d: %d/%d reached goal\n",t,h_reached,N_AGENTS);
        if(h_reached>=N_AGENTS)break;
    }
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("Final: %d/%d agents reached goal (%.1f%%)\n",h_reached,N_AGENTS,100.0f*h_reached/N_AGENTS);
    printf("BFS time: %.1f ms for 256x256 grid\n",bfs_ms);
    printf("═══════════════════════════════════════════════════════\n");
    cudaFree(d_obs);cudaFree(d_dist);cudaFree(d_goal);cudaFree(d_fx);cudaFree(d_fy);
    cudaFree(d_ax);cudaFree(d_ay);cudaFree(d_reached);
    return 0;
}
