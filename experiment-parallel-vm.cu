/* experiment-parallel-vm.cu — Parallel Greenhorn VM Evolution
   1024 VMs each running different bytecode, fitness = instructions executed before halt.
   Mutation between generations. Tests if simple programs evolve complex behavior. */

#include <cstdio>
#include <cstdlib>
#include <cmath>

#define N_VMS 1024
#define CODE_SIZE 64
#define MEM_SIZE 256
#define MAX_STEPS 500
#define N_REGS 8
#define N_GENS 20

// Opcodes
#define OP_HALT 0
#define OP_LOAD 1   // LOAD reg imm
#define OP_ADD  2   // ADD reg reg
#define OP_SUB  3
#define OP_MUL  4
#define OP_DIV  5
#define OP_AND  6
#define OP_OR   7
#define OP_XOR  8
#define OP_SHL  9
#define OP_SHR  10
#define OP_JZ   11  // JZ reg addr
#define OP_JNZ  12
#define OP_JMP  13
#define OP_CMP  14  // CMP reg reg -> set flag
#define OP_MOV  15  // MOV reg reg
#define OP_LD   16  // LD reg [addr]
#define OP_ST   17  // ST reg [addr]
#define OP_INC  18
#define OP_DEC  19
#define OP_NOP  20
#define OP_NEG  21
#define OP_MOD  22

__device__ int run_vm(int*code,int*mem,int*regs,int*pc,int*flag,int*steps){
    for(int s=0;s<MAX_STEPS;s++){
        (*steps)++;
        int op=code[*pc];
        switch(op){
            case OP_HALT: return 0;
            case OP_LOAD: {int r=code[(*pc+1)%CODE_SIZE];regs[r]=code[(*pc+2)%CODE_SIZE];*pc+=3;break;}
            case OP_ADD: {int a=code[(*pc+1)%CODE_SIZE],b=code[(*pc+2)%CODE_SIZE];regs[a]+=regs[b];*pc+=3;break;}
            case OP_SUB: regs[code[(*pc+1)%CODE_SIZE]]-=regs[code[(*pc+2)%CODE_SIZE]];*pc+=3;break;
            case OP_MUL: regs[code[(*pc+1)%CODE_SIZE]]*=regs[code[(*pc+2)%CODE_SIZE]];*pc+=3;break;
            case OP_DIV: {int a=code[(*pc+1)%CODE_SIZE];int b=code[(*pc+2)%CODE_SIZE];if(regs[b]!=0)regs[a]/=regs[b];*pc+=3;break;}
            case OP_AND: regs[code[(*pc+1)%CODE_SIZE]]&=regs[code[(*pc+2)%CODE_SIZE]];*pc+=3;break;
            case OP_OR:  regs[code[(*pc+1)%CODE_SIZE]]|=regs[code[(*pc+2)%CODE_SIZE]];*pc+=3;break;
            case OP_XOR: regs[code[(*pc+1)%CODE_SIZE]]^=regs[code[(*pc+2)%CODE_SIZE]];*pc+=3;break;
            case OP_SHL: regs[code[(*pc+1)%CODE_SIZE]]<<=regs[code[(*pc+2)%CODE_SIZE]];*pc+=3;break;
            case OP_SHR: regs[code[(*pc+1)%CODE_SIZE]]>>=regs[code[(*pc+2)%CODE_SIZE]];*pc+=3;break;
            case OP_JZ:  if(regs[code[(*pc+1)%CODE_SIZE]]==0)*pc=code[(*pc+2)%CODE_SIZE]%CODE_SIZE;else *pc+=3;break;
            case OP_JNZ: if(regs[code[(*pc+1)%CODE_SIZE]]!=0)*pc=code[(*pc+2)%CODE_SIZE]%CODE_SIZE;else *pc+=3;break;
            case OP_JMP: *pc=code[(*pc+1)%CODE_SIZE]%CODE_SIZE;break;
            case OP_CMP: *flag=(regs[code[(*pc+1)%CODE_SIZE]]==regs[code[(*pc+2)%CODE_SIZE]])?1:0;*pc+=3;break;
            case OP_MOV: regs[code[(*pc+1)%CODE_SIZE]]=regs[code[(*pc+2)%CODE_SIZE]];*pc+=3;break;
            case OP_LD:  {int r=code[(*pc+1)%CODE_SIZE];int addr=code[(*pc+2)%CODE_SIZE]%MEM_SIZE;regs[r]=mem[addr];*pc+=3;break;}
            case OP_ST:  {int r=code[(*pc+1)%CODE_SIZE];int addr=code[(*pc+2)%CODE_SIZE]%MEM_SIZE;mem[addr]=regs[r];*pc+=3;break;}
            case OP_INC: regs[code[(*pc+1)%CODE_SIZE]]++;*pc+=2;break;
            case OP_DEC: regs[code[(*pc+1)%CODE_SIZE]]--;*pc+=2;break;
            case OP_NOP: *pc+=1;break;
            case OP_NEG: regs[code[(*pc+1)%CODE_SIZE]]=-regs[code[(*pc+1)%CODE_SIZE]];*pc+=2;break;
            case OP_MOD: {int a=code[(*pc+1)%CODE_SIZE];int b=code[(*pc+2)%CODE_SIZE];if(regs[b]!=0)regs[a]%=regs[b];*pc+=3;break;}
            default: return -1; // invalid
        }
        *pc%=CODE_SIZE;
    }
    return 1; // timeout
}

__global__ void init_vms(int*codes,int*fitness){
    int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=N_VMS)return;
    unsigned int s=(unsigned int)(i*2654435761u+17);
    for(int j=0;j<CODE_SIZE;j++){
        codes[i*CODE_SIZE+j]=(int)(((s*1103515245u+12345u)>>16)&0x7fff)%23; // 0-22 opcodes
        s=s*1103515245u+12345u;
    }
    codes[i*CODE_SIZE]=OP_HALT; // ensure halt at start (will get mutated away)
    fitness[i]=0;
}

__global__ void eval_vms(int*codes,int*fitness,unsigned int*seeds){
    int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=N_VMS)return;
    int mem[MEM_SIZE];int regs[N_REGS];int pc=0;int flag=0;int steps=0;
    for(int j=0;j<MEM_SIZE;j++)mem[j]=0;
    for(int j=0;j<N_REGS;j++)regs[j]=0;
    // Seed memory with challenge: compute fibonacci-like sequence
    mem[0]=1;mem[1]=1;
    run_vm(codes+i*CODE_SIZE,mem,regs,&pc,&flag,&steps);
    // Fitness: steps alive + unique memory values + output at mem[63]
    int unique=0;int seen[256]={0};
    for(int j=0;j<MEM_SIZE;j++){int v=(mem[j]%256+256)%256;if(!seen[v]){seen[v]=1;unique++;}}
    fitness[i]=steps+unique*2+(regs[0]%100)*3;
}

__global__ void mutate(int*codes,int*fitness,unsigned int*order){
    int idx=blockIdx.x*blockDim.x+threadIdx.x;
    if(idx>=N_VMS)return;
    // Tournament selection: pick 2 random, copy better one
    int a=order[(idx*3)%N_VMS],b=order[(idx*3+1)%N_VMS];
    int src=(fitness[a]>=fitness[b])?a:b;
    for(int j=0;j<CODE_SIZE;j++)codes[idx*CODE_SIZE+j]=codes[src*CODE_SIZE+j];
    // 5% mutation rate
    for(int j=0;j<CODE_SIZE;j++){
        if(((idx*CODE_SIZE+j)*7+13)%20==0){
            codes[idx*CODE_SIZE+j]=(codes[idx*CODE_SIZE+j]+((idx+j)*3+7)%23)%23;
        }
    }
}

__global__ void shuffle_order(unsigned int*order){
    int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=N_VMS)return;
    unsigned int s=(unsigned int)(i*2654435761u+42);
    int j=(int)(((s*1103515245u+12345u)>>16)&0x7fff)%N_VMS;
    int tmp=order[i];order[i]=order[j];order[j]=tmp;
}

int main(){
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Experiment 1: Parallel Greenhorn VM Evolution\n");
    printf("  1024 VMs, 64-byte code, 20 generations\n");
    printf("  Fitness = steps alive + unique mem + output\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    int*codes,*d_codes,*fitness,*d_fitness;unsigned int*d_order;
    codes=(int*)malloc(N_VMS*CODE_SIZE*sizeof(int));
    fitness=(int*)malloc(N_VMS*sizeof(int));
    cudaMalloc(&d_codes,N_VMS*CODE_SIZE*sizeof(int));
    cudaMalloc(&d_fitness,N_VMS*sizeof(int));
    cudaMalloc(&d_order,N_VMS*sizeof(unsigned int));
    int blk=(N_VMS+255)/256;
    // Initialize
    init_vms<<<blk,256>>>(d_codes,d_fitness);cudaDeviceSynchronize();
    for(int gen=0;gen<N_GENS;gen++){
        eval_vms<<<blk,256>>>(d_codes,d_fitness,d_order);cudaDeviceSynchronize();
        cudaMemcpy(fitness,d_fitness,N_VMS*sizeof(int),cudaMemcpyDeviceToHost);
        float avg=0,best=0;for(int i=0;i<N_VMS;i++){avg+=fitness[i];if(fitness[i]>best)best=fitness[i];}
        avg/=N_VMS;
        printf("Gen %2d: avg=%.1f best=%d\n",gen,avg,best);
        // Shuffle order for tournament
        shuffle_order<<<blk,256>>>(d_order);cudaDeviceSynchronize();
        mutate<<<blk,256>>>(d_codes,d_fitness,d_order);cudaDeviceSynchronize();
    }
    // Final eval
    eval_vms<<<blk,256>>>(d_codes,d_fitness,d_order);cudaDeviceSynchronize();
    cudaMemcpy(fitness,d_fitness,N_VMS*sizeof(int),cudaMemcpyDeviceToHost);
    float avg=0,best=0;int best_idx=0;
    for(int i=0;i<N_VMS;i++){avg+=fitness[i];if(fitness[i]>best){best=fitness[i];best_idx=i;}}
    avg/=N_VMS;
    cudaMemcpy(codes,d_codes,N_VMS*CODE_SIZE*sizeof(int),cudaMemcpyDeviceToHost);
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("Final: avg=%.1f best=%d (vm %d)\n",avg,best,best_idx);
    printf("Best program: ");
    for(int j=0;j<CODE_SIZE;j++)printf("%d ",codes[best_idx*CODE_SIZE+j]);
    printf("\n═══════════════════════════════════════════════════════\n");
    cudaFree(d_codes);cudaFree(d_fitness);cudaFree(d_order);free(codes);free(fitness);
    return 0;
}
