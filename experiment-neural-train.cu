/* experiment-neural-train.cu — Tiny Neural Net Training from Scratch on GPU
   64→32→4 network, trains on XOR-like patterns using SGD.
   Proves Jetson GPU can do learning, not just inference. */

#include <cstdio>
#include <cstdlib>
#include <cmath>

#define INPUT 8
#define HIDDEN 16
#define OUTPUT 4
#define N_SAMPLES 256
#define N_EPOCHS 200
#define LR 0.01f

__device__ float sigmoid(float x){return 1.0f/(1.0f+expf(-x));}
__device__ float relu(float x){return x>0?x:0;}
__device__ float relu_d(float x){return x>0?1.0f:0.0f;}

__global__ void init_params(float*w1,float*b1,float*w2,float*b2,unsigned int seed){
    int i=blockIdx.x*blockDim.x+threadIdx.x;
    unsigned int s=seed+i*2654435761u;
    auto rng=[&](){s=s*1103515245u+12345u;return(float)(((s>>16)&0x7fff)-16384)/16384.0f;};
    // Xavier init
    float s1=sqrtf(2.0f/INPUT);
    if(i<INPUT*HIDDEN)w1[i]=rng()*s1;
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
    for(int j=0;j<INPUT;j++)X[i*INPUT+j]=(rng()>.5f)?1:-1;
    // Target: classify into 4 categories based on parity/sum patterns
    int pos=0,neg=0;
    for(int j=0;j<INPUT;j++){if(X[i*INPUT+j]>0)pos++;else neg++;}
    int cat=(pos%2)*2+(neg%2);
    for(int j=0;j<OUTPUT;j++)Y[i*OUTPUT+j]=(j==cat)?1:0;
}

__global__ void forward(const float*X,const float*w1,const float*b1,const float*w2,const float*b2,
                        float*h_z,float*h_a,float*o_z,float*o_a,float*losses,int n){
    int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=n)return;
    // Hidden layer
    for(int j=0;j<HIDDEN;j++){
        float sum=b1[j];
        for(int k=0;k<INPUT;k++)sum+=X[i*INPUT+k]*w1[k*HIDDEN+j];
        h_z[i*HIDDEN+j]=sum;
        h_a[i*HIDDEN+j]=relu(sum);
    }
    // Output layer
    for(int j=0;j<OUTPUT;j++){
        float sum=b2[j];
        for(int k=0;k<HIDDEN;k++)sum+=h_a[i*HIDDEN+k]*w2[k*OUTPUT+j];
        o_z[i*OUTPUT+j]=sum;
        o_a[i*OUTPUT+j]=sigmoid(sum);
    }
    // Loss computed in train_step
}

// Simpler: compute forward + loss + accuracy in one kernel
__global__ void train_step(const float*X,const float*Y,float*w1,float*b1,float*w2,float*b2,
                           float*h_a,float*o_a,float*loss,int n,unsigned int seed,int epoch){
    int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=n)return;
    // Forward: hidden
    float h_z[HIDDEN];
    for(int j=0;j<HIDDEN;j++){
        float sum=b1[j];
        for(int k=0;k<INPUT;k++)sum+=X[i*INPUT+k]*w1[k*HIDDEN+j];
        h_z[j]=sum;h_a[i*HIDDEN+j]=relu(sum);
    }
    // Forward: output
    float o_pred[OUTPUT];
    for(int j=0;j<OUTPUT;j++){
        float sum=b2[j];
        for(int k=0;k<HIDDEN;k++)sum+=h_a[i*HIDDEN+k]*w2[k*OUTPUT+j];
        o_pred[j]=sigmoid(sum);o_a[i*OUTPUT+j]=o_pred[j];
    }
    // Loss (MSE)
    float l=0;int pred_class=0;float max_p=0;
    for(int j=0;j<OUTPUT;j++){
        float d=o_pred[j]-Y[i*OUTPUT+j];l+=d*d;
        if(o_pred[j]>max_p){max_p=o_pred[j];pred_class=j;}
    }
    loss[i]=l/OUTPUT;
    // Backward: output gradients
    float d_o[OUTPUT];
    for(int j=0;j<OUTPUT;j++)d_o[j]=2*(o_pred[j]-Y[i*OUTPUT+j])/OUTPUT*o_pred[j]*(1-o_pred[j]);
    // Update w2, b2
    for(int k=0;k<HIDDEN;k++){
        for(int j=0;j<OUTPUT;j++){
            float grad=h_a[i*HIDDEN+k]*d_o[j];
            atomicAdd(&w2[k*OUTPUT+j],-LR*grad);
        }
    }
    for(int j=0;j<OUTPUT;j++)atomicAdd(&b2[j],-LR*d_o[j]);
    // Backward: hidden gradients
    float d_h[HIDDEN];
    for(int k=0;k<HIDDEN;k++){
        float sum=0;
        for(int j=0;j<OUTPUT;j++)sum+=w2[k*OUTPUT+j]*d_o[j];
        d_h[k]=sum*relu_d(h_z[k]);
    }
    // Update w1, b1
    for(int k=0;k<INPUT;k++){
        for(int j=0;j<HIDDEN;j++){
            float grad=X[i*INPUT+k]*d_h[j];
            atomicAdd(&w1[k*HIDDEN+j],-LR*grad);
        }
    }
    for(int j=0;j<HIDDEN;j++)atomicAdd(&b1[j],-LR*d_h[j]);
}

__global__ void eval_accuracy(const float*X,const float*Y,const float*w1,const float*b1,
                              const float*w2,const float*b2,int*correct,int n){
    int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=n)return;
    float h[HIDDEN];
    for(int j=0;j<HIDDEN;j++){
        float sum=b1[j];
        for(int k=0;k<INPUT;k++)sum+=X[i*INPUT+k]*w1[k*HIDDEN+j];
        h[j]=relu(sum);
    }
    int pred=0;float max_p=-1;
    for(int j=0;j<OUTPUT;j++){
        float sum=b2[j];
        for(int k=0;k<HIDDEN;k++)sum+=h[k]*w2[k*OUTPUT+j];
        float p=sigmoid(sum);
        if(p>max_p){max_p=p;pred=j;}
    }
    int true_class=0;for(int j=0;j<OUTPUT;j++)if(Y[i*OUTPUT+j]>0.5f)true_class=j;
    if(pred==true_class)atomicAdd(correct,1);
}

int main(){
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Experiment 4: Tiny Neural Net on GPU (from scratch)\n");
    printf("  8→16→4, %d samples, %d epochs, SGD lr=%.3f\n",N_SAMPLES,N_EPOCHS,LR);
    printf("═══════════════════════════════════════════════════════\n\n");
    float*d_X,*d_Y,*d_w1,*d_b1,*d_w2,*d_b2,*d_h,*d_o,*d_loss;
    int*d_correct;
    cudaMalloc(&d_X,N_SAMPLES*INPUT*sizeof(float));
    cudaMalloc(&d_Y,N_SAMPLES*OUTPUT*sizeof(float));
    cudaMalloc(&d_w1,INPUT*HIDDEN*sizeof(float));
    cudaMalloc(&d_b1,HIDDEN*sizeof(float));
    cudaMalloc(&d_w2,HIDDEN*OUTPUT*sizeof(float));
    cudaMalloc(&d_b2,OUTPUT*sizeof(float));
    cudaMalloc(&d_h,N_SAMPLES*HIDDEN*sizeof(float));
    cudaMalloc(&d_o,N_SAMPLES*OUTPUT*sizeof(float));
    cudaMalloc(&d_loss,N_SAMPLES*sizeof(float));
    cudaMalloc(&d_correct,sizeof(int));
    
    gen_data<<<1,N_SAMPLES>>>(d_X,d_Y,42);cudaDeviceSynchronize();
    init_params<<<1,INPUT*HIDDEN>>>(d_w1,d_b1,d_w2,d_b2,17);cudaDeviceSynchronize();
    
    float*losses=(float*)malloc(N_SAMPLES*sizeof(float));
    int blk=(N_SAMPLES+255)/256;
    for(int ep=0;ep<N_EPOCHS;ep++){
        train_step<<<blk,256>>>(d_X,d_Y,d_w1,d_b1,d_w2,d_b2,d_h,d_o,d_loss,N_SAMPLES,42,ep);
        cudaDeviceSynchronize();
        if(ep%20==0||ep==N_EPOCHS-1){
            cudaMemcpy(losses,d_loss,N_SAMPLES*sizeof(float),cudaMemcpyDeviceToHost);
            float avg=0;for(int i=0;i<N_SAMPLES;i++)avg+=losses[i];avg/=N_SAMPLES;
            cudaMemset(d_correct,0,sizeof(int));
            eval_accuracy<<<blk,256>>>(d_X,d_Y,d_w1,d_b1,d_w2,d_b2,d_correct,N_SAMPLES);
            cudaDeviceSynchronize();
            int corr=0;cudaMemcpy(&corr,d_correct,sizeof(int),cudaMemcpyDeviceToHost);
            printf("Epoch %3d: loss=%.4f accuracy=%d/%d (%.1f%%)\n",ep,avg,corr,N_SAMPLES,100.0f*corr/N_SAMPLES);
        }
    }
    // Final eval
    cudaMemset(d_correct,0,sizeof(int));
    eval_accuracy<<<blk,256>>>(d_X,d_Y,d_w1,d_b1,d_w2,d_b2,d_correct,N_SAMPLES);
    cudaDeviceSynchronize();
    int corr=0;cudaMemcpy(&corr,d_correct,sizeof(int),cudaMemcpyDeviceToHost);
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("Final accuracy: %d/%d (%.1f%%)\n",corr,N_SAMPLES,100.0f*corr/N_SAMPLES);
    printf("Conclusion: %s\n",corr>N_SAMPLES*0.8?"GPU CAN learn from scratch":"Need more capacity/tuning");
    printf("═══════════════════════════════════════════════════════\n");
    cudaFree(d_X);cudaFree(d_Y);cudaFree(d_w1);cudaFree(d_b1);cudaFree(d_w2);cudaFree(d_b2);
    cudaFree(d_h);cudaFree(d_o);cudaFree(d_loss);cudaFree(d_correct);free(losses);
    return 0;
}
