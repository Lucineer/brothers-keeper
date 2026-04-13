/* experiment-gpu-perception.cu — GPU Anomaly Detection on Simulated Sensor Streams
   Simulates 9 thermal zones × 10000 samples. Z-score detection on GPU.
   Tests: can we detect anomalies in <1ms with pure CUDA? */

#include <cstdio>
#include <cstdlib>
#include <cmath>

#define N_STREAMS 9
#define N_SAMPLES 10000
#define WINDOW 100
#define Z_THRESH 3.0f

__global__ void gen_streams(float*data,unsigned int seed){
    int s=blockIdx.x*blockDim.x+threadIdx.x;
    if(s>=N_STREAMS)return;
    unsigned int rng=seed+s*99991;
    auto lcg=[&](){rng=rng*1103515245u+12345u;return(float)((rng>>16)&0x7fff)/32768.0f;};
    float base=40+s*5; // 40-80 range like real thermal zones
    for(int i=0;i<N_SAMPLES;i++){
        data[s*N_SAMPLES+i]=base+(lcg()-.5f)*2; // normal noise ±1
    }
    // Inject 5 anomalies per stream
    for(int a=0;a<5;a++){
        int pos=(int)(lcg()*N_SAMPLES);
        float severity=base+10+lcg()*20; // 10-30 above baseline
        // Spread over 3-5 samples
        int spread=3+(int)(lcg()*3);
        for(int w=0;w<spread&&pos+w<N_SAMPLES;w++)
            data[s*N_SAMPLES+pos+w]=severity+(lcg()-.5f)*3;
    }
}

__global__ void zscore_detect(const float*data,int*n_anomalies,float*z_max,int*anomaly_pos){
    int s=blockIdx.x*blockDim.x+threadIdx.x;
    if(s>=N_STREAMS)return;
    int count=0;float maxz=0;int maxpos=0;
    for(int i=WINDOW;i<N_SAMPLES;i++){
        float mean=0;
        for(int j=i-WINDOW;j<i;j++)mean+=data[s*N_SAMPLES+j];
        mean/=WINDOW;
        float std=0;
        for(int j=i-WINDOW;j<i;j++){float d=data[s*N_SAMPLES+j]-mean;std+=d*d;}
        std=sqrtf(std/WINDOW);
        if(std<0.001f)std=0.001f;
        float z=fabsf(data[s*N_SAMPLES+i]-mean)/std;
        if(z>Z_THRESH){count++;if(z>maxz){maxz=z;maxpos=i;}}
    }
    n_anomalies[s]=count;
    z_max[s]=maxz;
    anomaly_pos[s]=maxpos;
}

// Perception kernel: continuous monitoring with circular buffer
__global__ void perception_kernel(const float*data,float*buffer,float*mean,float*std,float*alert,int tick){
    int s=blockIdx.x*blockDim.x+threadIdx.x;
    if(s>=N_STREAMS)return;
    int buf_idx=tick%WINDOW;
    buffer[s*WINDOW+buf_idx]=data[s];
    // Running stats
    float m=0;for(int j=0;j<WINDOW;j++)m+=buffer[s*WINDOW+j];m/=WINDOW;
    float sd=0;for(int j=0;j<WINDOW;j++){float d=buffer[s*WINDOW+j]-m;sd+=d*d;}sd=sqrtf(sd/WINDOW);
    if(sd<0.001f)sd=0.001f;
    float z=fabsf(data[s]-m)/sd;
    mean[s]=m;std[s]=sd;
    alert[s]=(z>Z_THRESH)?z:-1.0f;
}

int main(){
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Experiment 2: GPU Perception Kernel\n");
    printf("  9 streams × 10000 samples, z-score anomaly detection\n");
    printf("═══════════════════════════════════════════════════════\n\n");
    float*data,*d_data;int*n_anom,*d_n_anom;float*z_max,*d_z_max;int*anom_pos,*d_anom_pos;
    data=(float*)malloc(N_STREAMS*N_SAMPLES*sizeof(float));
    n_anom=(int*)malloc(N_STREAMS*sizeof(int));z_max=(float*)malloc(N_STREAMS*sizeof(float));
    anom_pos=(int*)malloc(N_STREAMS*sizeof(int));
    cudaMalloc(&d_data,N_STREAMS*N_SAMPLES*sizeof(float));
    cudaMalloc(&d_n_anom,N_STREAMS*sizeof(int));
    cudaMalloc(&d_z_max,N_STREAMS*sizeof(float));
    cudaMalloc(&d_anom_pos,N_STREAMS*sizeof(int));
    // Generate
    gen_streams<<<1,9>>>(d_data,42);cudaDeviceSynchronize();
    // Detect
    cudaEvent_t start,stop;cudaEventCreate(&start);cudaEventCreate(&stop);
    cudaEventRecord(start);
    zscore_detect<<<1,9>>>(d_data,d_n_anom,d_z_max,d_anom_pos);cudaDeviceSynchronize();
    cudaEventRecord(stop);cudaEventSynchronize(stop);
    float ms=0;cudaEventElapsedTime(&ms,start,stop);
    cudaMemcpy(n_anom,d_n_anom,N_STREAMS*sizeof(int),cudaMemcpyDeviceToHost);
    cudaMemcpy(z_max,d_z_max,N_STREAMS*sizeof(float),cudaMemcpyDeviceToHost);
    cudaMemcpy(anom_pos,d_anom_pos,N_STREAMS*sizeof(int),cudaMemcpyDeviceToHost);
    printf("Detection time: %.3f ms (%d total samples processed)\n",ms,N_STREAMS*N_SAMPLES);
    printf("Throughput: %.1f M samples/sec\n",N_STREAMS*N_SAMPLES/(ms*1000));
    const char*names[]={"cpu","gpu","cv0","cv1","cv2","soc0","soc1","soc2","tj"};
    int total_anom=0,total_expected=N_STREAMS*5;
    for(int s=0;s<N_STREAMS;s++){
        printf("  %s: %d anomalies, max z=%.1f at sample %d\n",names[s],n_anom[s],z_max[s],anom_pos[s]);
        total_anom+=n_anom[s];
    }
    printf("\nDetection rate: %d/%d (%.1f%%)\n",total_anom,total_expected,(float)total_anom/total_expected*100);
    // Perception kernel test
    printf("\n--- Perception Kernel (streaming mode) ---\n");
    float*d_buf,*d_mean,*d_std,*d_alert,*h_mean,*h_std,*h_alert;
    cudaMalloc(&d_buf,N_STREAMS*WINDOW*sizeof(float));
    cudaMalloc(&d_mean,N_STREAMS*sizeof(float));
    cudaMalloc(&d_std,N_STREAMS*sizeof(float));
    cudaMalloc(&d_alert,N_STREAMS*sizeof(float));
    cudaMemset(d_buf,0,N_STREAMS*WINDOW*sizeof(float));
    h_mean=(float*)malloc(N_STREAMS*sizeof(float));
    h_std=(float*)malloc(N_STREAMS*sizeof(float));
    h_alert=(float*)malloc(N_STREAMS*sizeof(float));
    // Warm up buffer
    float*h_data=(float*)malloc(N_STREAMS*sizeof(float));
    cudaMemcpy(data,d_data,N_STREAMS*N_SAMPLES*sizeof(float),cudaMemcpyDeviceToHost);
    for(int t=0;t<WINDOW;t++){
        cudaMemcpy(h_data,data+t*N_STREAMS,N_STREAMS*sizeof(float),cudaMemcpyHostToHost);
        cudaMemcpy(d_data,h_data,N_STREAMS*sizeof(float),cudaMemcpyHostToDevice);
        perception_kernel<<<1,9>>>(d_data,d_buf,d_mean,d_std,d_alert,t);cudaDeviceSynchronize();
    }
    // Now stream through, count alerts
    int stream_alerts=0;
    cudaEventRecord(start);
    for(int t=WINDOW;t<N_SAMPLES;t++){
        cudaMemcpy(h_data,data+t*N_STREAMS,N_STREAMS*sizeof(float),cudaMemcpyHostToHost);
        cudaMemcpy(d_data,h_data,N_STREAMS*sizeof(float),cudaMemcpyHostToDevice);
        perception_kernel<<<1,9>>>(d_data,d_buf,d_mean,d_std,d_alert,t);cudaDeviceSynchronize();
        cudaMemcpy(h_alert,d_alert,N_STREAMS*sizeof(float),cudaMemcpyDeviceToHost);
        for(int s=0;s<N_STREAMS;s++)if(h_alert[s]>0)stream_alerts++;
    }
    cudaEventRecord(stop);cudaEventSynchronize(stop);
    cudaEventElapsedTime(&ms,start,stop);
    printf("Streaming: %d alerts in %.1f ms (%.0f ticks/sec)\n",stream_alerts,ms,(N_SAMPLES-WINDOW)/ms*1000);
    printf("═══════════════════════════════════════════════════════\n");
    cudaFree(d_data);cudaFree(d_n_anom);cudaFree(d_z_max);cudaFree(d_anom_pos);
    cudaFree(d_buf);cudaFree(d_mean);cudaFree(d_std);cudaFree(d_alert);
    free(data);free(n_anom);free(z_max);free(anom_pos);free(h_mean);free(h_std);free(h_alert);free(h_data);
    return 0;
}
