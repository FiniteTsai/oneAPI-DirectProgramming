#include <fcntl.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include "bucketsort.h"

// CUDA kernels
#include "kernel_bucketcount.h"
#include "kernel_bucketprefix.h"
#include "kernel_bucketsort.h"
#include "kernel_histogram.h"

////////////////////////////////////////////////////////////////////////////////
// Forward declarations
////////////////////////////////////////////////////////////////////////////////
void calcPivotPoints(float *histogram, int histosize, int listsize,
    int divisions, float min, float max, float *pivotPoints,
    float histo_width);

////////////////////////////////////////////////////////////////////////////////
// Given the input array of floats and the min and max of the distribution,
// sort the elements into float4 aligned buckets of roughly equal size
////////////////////////////////////////////////////////////////////////////////
void bucketSort(float *d_input, float *d_output, int listsize,
    int *sizes, int *nullElements, float minimum, float maximum,
    unsigned int *origOffsets)
{

  const int histosize = 1024;
  //  ////////////////////////////////////////////////////////////////////////////
  //  // First pass - Create 1024 bin histogram
  //  ////////////////////////////////////////////////////////////////////////////
  unsigned int* h_offsets = (unsigned int *) malloc(DIVISIONS * sizeof(unsigned int));
  for(int i = 0; i < DIVISIONS; i++){
    h_offsets[i] = 0;
  }
  float* pivotPoints = (float *)malloc(DIVISIONS * sizeof(float));
  float* historesult = (float *)malloc(histosize * sizeof(float));

  int blocks = ((listsize - 1) / (BUCKET_THREAD_N * BUCKET_BAND)) + 1;

  float *d_input_buff;
  unsigned int* d_offsets_buff;
  hipMalloc((void**)&d_input_buff, sizeof(float)*(listsize + DIVISIONS*4));
  hipMemcpyAsync(d_input_buff, d_input, sizeof(float)*(listsize + DIVISIONS*4), hipMemcpyHostToDevice, 0);
  hipMalloc((void**)&d_offsets_buff, sizeof(unsigned int)*DIVISIONS);

  size_t global_histogram = 6144;

#ifdef HISTO_WG_SIZE_0
  size_t local_histogram = HISTO_WG_SIZE_0;
#else
  size_t local_histogram = 96;
#endif

  hipMemcpyAsync(d_offsets_buff, h_offsets, sizeof(unsigned int)*DIVISIONS, hipMemcpyHostToDevice, 0);
  hipLaunchKernelGGL(histogram1024, dim3(global_histogram/local_histogram), dim3(local_histogram), 0, 0, 
      d_offsets_buff, d_input_buff, listsize, minimum, maximum);
  hipMemcpy(h_offsets, d_offsets_buff, sizeof(unsigned int)*histosize, hipMemcpyDeviceToHost);

  for(int i=0; i<histosize; i++) {
    historesult[i] = (float)h_offsets[i];
  }


  //  ///////////////////////////////////////////////////////////////////////////
  //  // Calculate pivot points (CPU algorithm)
  //  ///////////////////////////////////////////////////////////////////////////
  calcPivotPoints(historesult, histosize, listsize, DIVISIONS,
      minimum, maximum, pivotPoints,
      (maximum - minimum)/(float)histosize);
  //
  //  ///////////////////////////////////////////////////////////////////////////
  //  // Count the bucket sizes in new divisions
  //  ///////////////////////////////////////////////////////////////////////////


  float* d_pivotPoints_buff;
  int* d_indice_buff;
  unsigned int* d_prefixoffsets_buff;

  hipMalloc((void**)&d_pivotPoints_buff, sizeof(float)*DIVISIONS);
  hipMemcpyAsync(d_pivotPoints_buff, pivotPoints, sizeof(float)*DIVISIONS, hipMemcpyHostToDevice, 0);

  hipMalloc((void**)&d_indice_buff, sizeof(int)*listsize);
  hipMalloc((void**)&d_prefixoffsets_buff, sizeof(unsigned int)* blocks * BUCKET_BLOCK_MEMORY);

  hipLaunchKernelGGL(bucketcount, dim3(blocks), dim3(BUCKET_THREAD_N), 0, 0, d_input_buff, d_indice_buff, d_prefixoffsets_buff,
      d_pivotPoints_buff, listsize); 

  //
  //  ///////////////////////////////////////////////////////////////////////////
  //  // Prefix scan offsets and align each division to float4 (required by
  //  // mergesort)
  //  ///////////////////////////////////////////////////////////////////////////
#ifdef BUCKET_WG_SIZE_0
  size_t localpre = BUCKET_WG_SIZE_0;
#else
  size_t localpre = 128;
#endif
  size_t globalpre = DIVISIONS;

  hipLaunchKernelGGL(bucketprefix, dim3(globalpre/localpre), dim3(localpre), 0, 0, d_prefixoffsets_buff, d_offsets_buff, blocks);

  // copy the sizes from device to host
  hipMemcpy(h_offsets, d_offsets_buff, sizeof(unsigned int)*DIVISIONS, hipMemcpyDeviceToHost);

  origOffsets[0] = 0;
  for(int i=0; i<DIVISIONS; i++){
    origOffsets[i+1] = h_offsets[i] + origOffsets[i];
    if((h_offsets[i] % 4) != 0){
      nullElements[i] = (h_offsets[i] & ~3) + 4 - h_offsets[i];
    }
    else nullElements[i] = 0;
  }
  for(int i=0; i<DIVISIONS; i++) sizes[i] = (h_offsets[i] + nullElements[i])/4;
  for(int i=0; i<DIVISIONS; i++) {
    if((h_offsets[i] % 4) != 0)  h_offsets[i] = (h_offsets[i] & ~3) + 4;
  }
  for(int i=1; i<DIVISIONS; i++) h_offsets[i] = h_offsets[i-1] + h_offsets[i];
  for(int i=DIVISIONS - 1; i>0; i--) h_offsets[i] = h_offsets[i-1];
  h_offsets[0] = 0;

  //  ///////////////////////////////////////////////////////////////////////////
  //  // Finally, sort the lot
  //  ///////////////////////////////////////////////////////////////////////////

  // update the h_offsets on the device
  hipMemcpyAsync(d_offsets_buff, h_offsets, sizeof(unsigned int)*DIVISIONS, hipMemcpyHostToDevice, 0);

  float* d_bucketOutput;
  hipMalloc((void**)&d_bucketOutput, sizeof(float)*(listsize + DIVISIONS*4)); 
  hipMemcpyAsync(d_bucketOutput, d_output, sizeof(float)*(listsize + DIVISIONS*4), hipMemcpyHostToDevice,0); 

  size_t localfinal = BUCKET_THREAD_N;
  size_t globalfinal = blocks*BUCKET_THREAD_N;

  hipLaunchKernelGGL(bucketsort, dim3(blocks), dim3(BUCKET_THREAD_N), 0, 0, d_input_buff, d_indice_buff, d_bucketOutput,
      d_prefixoffsets_buff, d_offsets_buff, listsize);

  hipMemcpy(d_output, d_bucketOutput, sizeof(float)*(listsize + DIVISIONS*4), hipMemcpyDeviceToHost);

  hipFree(d_bucketOutput);
  hipFree(d_input_buff);
  hipFree(d_offsets_buff);
  hipFree(d_pivotPoints_buff);
  hipFree(d_indice_buff);
  hipFree(d_prefixoffsets_buff);
  free(pivotPoints);
  free(historesult);
}
////////////////////////////////////////////////////////////////////////////////
// Given a histogram of the list, figure out suitable pivotpoints that divide
// the list into approximately listsize/divisions elements each
////////////////////////////////////////////////////////////////////////////////
void calcPivotPoints(float *histogram, int histosize, int listsize,
    int divisions, float min, float max, float *pivotPoints, float histo_width)
{
  float elemsPerSlice = listsize/(float)divisions;
  float startsAt = min;
  float endsAt = min + histo_width;
  float we_need = elemsPerSlice;
  int p_idx = 0;
  for(int i=0; i<histosize; i++)
  {
    if(i == histosize - 1){
      if(!(p_idx < divisions)){
        pivotPoints[p_idx++] = startsAt + (we_need/histogram[i]) * histo_width;
      }
      break;
    }
    while(histogram[i] > we_need){
      if(!(p_idx < divisions)){
        printf("i=%d, p_idx = %d, divisions = %d\n", i, p_idx, divisions);
        exit(0);
      }
      pivotPoints[p_idx++] = startsAt + (we_need/histogram[i]) * histo_width;
      startsAt += (we_need/histogram[i]) * histo_width;
      histogram[i] -= we_need;
      we_need = elemsPerSlice;
    }
    // grab what we can from what remains of it
    we_need -= histogram[i];

    startsAt = endsAt;
    endsAt += histo_width;
  }
  while(p_idx < divisions){
    pivotPoints[p_idx] = pivotPoints[p_idx-1];
    p_idx++;
  }
}
