/* experiment-neural-train-v2.cu — Fixed: lower LR, separate forward/backward
   64→32→4 network, mini-batch SGD, lower learning rate. */

#include <cstdio>
#include <cstdlib>
#include <cmath>

#define INPUT 8
#define HIDDEN 32
#define OUTPUT 4
#define N_SAMPLES 512
#define N_EPOCHS 300
#define LR 0.001f

__device__ float sigmoid(float x){return 1.0f/(1.0f+expf(-fminf(fmaxf(x,-20),20)));}
__device__ float relu(float x){return x>0?x:0;}
__device__ float relu_d(float x){return x>0?1.0f:0.0f;}

__global__ void init_params(float*w1,float*b1,float*w2,float*b2,unsigned int seed){
    int i=blockIdx.x*blockDim.x+threadIdx.x;
    unsigned int s=seed+i*2654435761u;
    auto rng=[&](){s=s*1103515245u+12345u;return(float)(((s>>16)&0x7fff)-16384)/16384.0f;};
    if(i<INPUT*HIDDEN)w1[i]=rng()*sqrtf(2.0f/INPUT);
    if(i<HIDDEN)b1[i]=0;
    if(i<HIDDEN*OUTPUT)w2[i]=rng()*sqrtf(2.0f/HIDDEN);
    if(i<OUTPUT)b2[i]=0;
}

__global__ void gen_data(float*X,float*Y,unsigned int seed){
    int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=N_SAMPLES)return;
    unsigned int s=seed+i*9973u;
    auto rng=[&](){s=s*1103515245u+12345u;return(float)(((s>>16)&0x7fff))/32768.0f;};
    for(int j=0;j<INPUT;j++)X[i*INPUT+j]=(rng()>.5f)?1.0f:-1.0f;
    int pos=0;for(int j=0;j<INPUT;j++)if(X[i*INPUT+j]>0)pos++;
    int cat=pos%OUTPUT;
    for(int j=0;j<OUTPUT;j++)Y[i*OUTPUT+j]=(j==cat)?1.0f:0.0f;
}

// Each thread = one sample, sequential forward/backward (no atomics)
__global__ void train_step(float*X,float*Y,float*w1,float*b1,float*w2,float*b2,
                           float*g_w1,float*g_b1,float*g_w2,float*g_b2,float*losses,
                           int n,int epoch){
    int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=n)return;
    // Forward
    float h_z[HIDDEN],h_a[HIDDEN],o_z[OUTPUT],o_a[OUTPUT];
    for(int j=0;j<HIDDEN;j++){
        float sum=b1[j];
        for(int k=0;k<INPUT;k++)sum+=X[i*INPUT+k]*w1[k*HIDDEN+j];
        h_z[j]=sum;h_a[j]=relu(sum);
    }
    for(int j=0;j<OUTPUT;j++){
        float sum=b2[j];
        for(int k=0;k<HIDDEN;k++)sum+=h_a[k]*w2[k*OUTPUT+j];
        o_z[j]=sum;o_a[j]=sigmoid(sum);
    }
    // MSE loss
    float l=0;for(int j=0;j<OUTPUT;j++){float d=o_a[j]-Y[i*OUTPUT+j];l+=d*d;}
    losses[i]=l/OUTPUT;
    // Backward output
    float d_o[OUTPUT];
    for(int j=0;j<OUTPUT;j++){
        float e=o_a[j]-Y[i*OUTPUT+j];
        d_o[j]=2.0f*e/OUTPUT*o_a[j]*(1.0f-o_a[j]);
    }
    // Backward hidden
    float d_h[HIDDEN];
    for(int k=0;k<HIDDEN;k++){
        float sum=0;for(int j=0;j<OUTPUT;j++)sum+=w2[k*OUTPUT+j]*d_o[j];
        d_h[k]=sum*relu_d(h_z[k]);
    }
    // Accumulate gradients (each thread writes to its own slot then we reduce)
    for(int k=0;k<INPUT;k++)for(int j=0;j<HIDDEN;j++)
        atomicAdd(&g_w1[k*HIDDEN+j],X[i*INPUT+k]*d_h[j]);
    for(int j=0;j<HIDDEN;j++)atomicAdd(&g_b1[j],d_h[j]);
    for(int k=0;k<HIDDEN;k++)for(int j=0;j<OUTPUT;j++)
        atomicAdd(&g_w2[k*OUTPUT+j],h_a[k]*d_o[j]);
    for(int j=0;j<OUTPUT;j++)atomicAdd(&g_b2[j],d_o[j]);
}

__global__ void apply_grads(float*w1,float*b1,float*w2,float*b2,
                            float*g_w1,float*g_b1,float*g_w2,float*g_b2,
                            float lr,int n){
    int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i<INPUT*HIDDEN)w1[i]-=lr*g_w1[i]/n;
    if(i<HIDDEN)b1[i]-=lr*g_b1[i]/n;
    if(i<HIDDEN*OUTPUT)w2[i]-=lr*g_w2[i]/n;
    if(i<OUTPUT)b2[i]-=lr*g_b2[i]/n;
}

__global__ void zero_grads(float*g_w1,float*g_b1,float*g_w2,float*g_b2){
    int i=blockIdx.x*blockDim.x+threadIdx.x;
    int maxsz=INPUT*HIDDEN; // largest
    if(i<INPUT*HIDDEN)g_w1[i]=0;
    if(i<HIDDEN)g_b1[i]=0;
    if(i<HIDDEN*OUTPUT)g_w2[i]=0;
    if(i<OUTPUT)g_b2[i]=0;
}

__global__ void eval_accuracy(float*X,float*Y,float*w1,float*b1,float*w2,float*b2,int*correct,int n){
    int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=n)return;
    float h[HIDDEN];
    for(int j=0;j<HIDDEN;j++){float sum=b1[j];for(int k=0;k<INPUT;k++)sum+=X[i*INPUT+k]*w1[k*HIDDEN+j];h[j]=relu(sum);}
    int pred=0;float max_p=-1;
    for(int j=0;j<OUTPUT;j++){float sum=b2[j];for(int k=0;k<HIDDEN;k++)sum+=h[k]*w2[k*OUTPUT+j];float p=sigmoid(sum);if(p>max_p){max_p=p;pred=j;}}
    int tc=0;for(int j=0;j<OUTPUT;j++)if(Y[i*OUTPUT+j]>0.5f)tc=j;
    if(pred==tc)atomicAdd(correct,1);
}

int main(){
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Experiment 4v2: Neural Net Training (fixed)\n");
    printf("  %d→%d→%d, %d samples, %d epochs, lr=%.4f\n",INPUT,HIDDEN,OUTPUT,N_SAMPLES,N_EPOCHS,LR);
    printf("═══════════════════════════════════════════════════════\n\n");
    float*d_X,*d_Y,*d_w1,*d_b1,*d_w2,*d_b2;
    float*d_gw1,*d_gb1,*d_gw2,*d_gb2,*d_loss;
    int*d_correct;
    cudaMalloc(&d_X,N_SAMPLES*INPUT*sizeof(float));cudaMalloc(&d_Y,N_SAMPLES*OUTPUT*sizeof(float));
    cudaMalloc(&d_w1,INPUT*HIDDEN*sizeof(float));cudaMalloc(&d_b1,HIDDEN*sizeof(float));
    cudaMalloc(&d_w2,HIDDEN*OUTPUT*sizeof(float));cudaMalloc(&d_b2,OUTPUT*sizeof(float));
    cudaMalloc(&d_gw1,INPUT*HIDDEN*sizeof(float));cudaMalloc(&d_gb1,HIDDEN*sizeof(float));
    cudaMalloc(&d_gw2,HIDDEN*OUTPUT*sizeof(float));cudaMalloc(&d_gb2,OUTPUT*sizeof(float));
    cudaMalloc(&d_loss,N_SAMPLES*sizeof(float));cudaMalloc(&d_correct,sizeof(int));
    int maxsz=max(INPUT*HIDDEN,max(HIDDEN,HIDDEN*OUTPUT));
    int blk=(maxsz+255)/256;
    gen_data<<<1,N_SAMPLES>>>(d_X,d_Y,42);cudaDeviceSynchronize();
    init_params<<<blk,256>>>(d_w1,d_b1,d_w2,d_b2,17);cudaDeviceSynchronize();
    float*losses=(float*)malloc(N_SAMPLES*sizeof(float));
    int sblk=(N_SAMPLES+255)/256;
    for(int ep=0;ep<N_EPOCHS;ep++){
        zero_grads<<<blk,256>>>(d_gw1,d_gb1,d_gw2,d_gb2);cudaDeviceSynchronize();
        train_step<<<sblk,256>>>(d_X,d_Y,d_w1,d_b1,d_w2,d_b2,d_gw1,d_gb1,d_gw2,d_gb2,d_loss,N_SAMPLES,ep);
        cudaDeviceSynchronize();
        apply_grads<<<blk,256>>>(d_w1,d_b1,d_w2,d_b2,d_gw1,d_gb1,d_gw2,d_gb2,LR,N_SAMPLES);
        cudaDeviceSynchronize();
        if(ep%50==0||ep==N_EPOCHS-1){
            cudaMemcpy(losses,d_loss,N_SAMPLES*sizeof(float),cudaMemcpyDeviceToHost);
            float avg=0;for(int i=0;i<N_SAMPLES;i++)avg+=losses[i];avg/=N_SAMPLES;
            cudaMemset(d_correct,0,sizeof(int));
            eval_accuracy<<<sblk,256>>>(d_X,d_Y,d_w1,d_b1,d_w2,d_b2,d_correct,N_SAMPLES);cudaDeviceSynchronize();
            int corr=0;cudaMemcpy(&corr,d_correct,sizeof(int),cudaMemcpyDeviceToHost);
            printf("Epoch %3d: loss=%.4f acc=%d/%d (%.1f%%)\n",ep,avg,corr,N_SAMPLES,100.0f*corr/N_SAMPLES);
        }
    }
    cudaMemset(d_correct,0,sizeof(int));
    eval_accuracy<<<sblk,256>>>(d_X,d_Y,d_w1,d_b1,d_w2,d_b2,d_correct,N_SAMPLES);cudaDeviceSynchronize();
    int corr=0;cudaMemcpy(&corr,d_correct,sizeof(int),cudaMemcpyDeviceToHost);
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("Final: %d/%d (%.1f%%) — %s\n",corr,N_SAMPLES,100.0f*corr/N_SAMPLES,corr>N_SAMPLES*0.8?"GPU CAN learn":"Needs more work");
    printf("═══════════════════════════════════════════════════════\n");
    cudaFree(d_X);cudaFree(d_Y);cudaFree(d_w1);cudaFree(d_b1);cudaFree(d_w2);cudaFree(d_b2);
    cudaFree(d_gw1);cudaFree(d_gb1);cudaFree(d_gw2);cudaFree(d_gb2);cudaFree(d_loss);cudaFree(d_correct);
    free(losses);return 0;
}
