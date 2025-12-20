#define RT_CPU 0
#define RT_GPU 1
#define RT_DXR 2

#define RT_MODE RT_DXR

#if RT_MODE == RT_CPU
#include "rt_cpu.cpp"
#elif RT_MODE == RT_GPU
#include "rt_gpu.cpp"
#else
#include "rt_dxr.cpp"
#endif