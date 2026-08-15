// SPDX-License-Identifier: MIT
// Short, controlled GPU load for identifying the 7th sensor block.
// Pure FMA loop - keeps the SMs saturated without needing much memory.
#include <cstdio>
#include <cuda_runtime.h>

__global__ void burn(float *a, int n, int iters) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float x = a[i], y = 1.000001f, z = 0.999999f;
    for (int k = 0; k < iters; k++) {
        x = fmaf(x, y, 0.5f);
        x = fmaf(x, z, 0.25f);
    }
    a[i] = x;
}

int main(int argc, char **argv) {
    int seconds = (argc > 1) ? atoi(argv[1]) : 30;
    int n = 1 << 24;                 // 16M floats = 64 MB
    float *d = nullptr;
    if (cudaMalloc(&d, (size_t)n * sizeof(float)) != cudaSuccess) {
        fprintf(stderr, "cudaMalloc failed\n");
        return 1;
    }
    cudaMemset(d, 0, (size_t)n * sizeof(float));

    int threads = 256, blocks = (n + threads - 1) / threads;
    time_t end = time(nullptr) + seconds;
    while (time(nullptr) < end) {
        burn<<<blocks, threads>>>(d, n, 4096);
        cudaDeviceSynchronize();
    }
    cudaFree(d);
    printf("burn complete\n");
    return 0;
}
