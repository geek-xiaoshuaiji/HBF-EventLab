// Functional GPU Core test: strict full-page stores then readback in a second kernel.
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
__global__ void write_page(unsigned long long base) {
  unsigned i=blockIdx.x*blockDim.x+threadIdx.x;
  if(i<1024)reinterpret_cast<volatile unsigned *>(base)[i]=i*3+7;
}
__global__ void read_page(unsigned long long base,unsigned *out) {
  unsigned i=blockIdx.x*blockDim.x+threadIdx.x;
  if(i<1024)out[i]=reinterpret_cast<volatile unsigned *>(base)[i];
}
int main(int argc,char **argv) {
  unsigned long long base=argc>1?strtoull(argv[1],nullptr,0):274877906944ULL;
  unsigned *out=nullptr;unsigned values[1024];
  if(cudaMalloc(&out,sizeof(values))!=cudaSuccess)return 2;
  write_page<<<8,128>>>(base);
  if(cudaDeviceSynchronize()!=cudaSuccess)return 3;
  read_page<<<8,128>>>(base,out);
  if(cudaDeviceSynchronize()!=cudaSuccess)return 4;
  if(cudaMemcpy(values,out,sizeof(values),cudaMemcpyDeviceToHost)!=cudaSuccess)return 5;
  for(unsigned i=0;i<1024;++i)if(values[i]!=i*3+7) {
    std::fprintf(stderr,"FULL-PAGE mismatch i=%u actual=%u\n",i,values[i]);return 6;
  }
  cudaFree(out);std::puts("[FULL-PAGE] 1024 values verified across two simulated kernels");return 0;
}
