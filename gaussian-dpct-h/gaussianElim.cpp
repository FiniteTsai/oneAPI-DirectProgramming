#ifndef __GAUSSIAN_ELIMINATION__
#define __GAUSSIAN_ELIMINATION__

#define DPCT_USM_LEVEL_NONE
#include <CL/sycl.hpp>
#include <dpct/dpct.hpp>
#include <sys/time.h>
#include "gaussianElim.h"

#define BLOCK_SIZE_0 256
#define BLOCK_SIZE_1_X 16
#define BLOCK_SIZE_1_Y 16


long long get_time() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec * 1000000) +tv.tv_usec;
}

// create both matrix and right hand side, Ke Wang 2013/08/12 11:51:06
void
create_matrix(float *m, int size){
  int i,j;
  float lamda = -0.01;
  float coe[2*size-1];
  float coe_i =0.0;

  for (i=0; i < size; i++)
  {
        coe_i = 10 * exp(lamda * i);
    j=size-1+i;     
    coe[j]=coe_i;
    j=size-1-i;     
    coe[j]=coe_i;
  }


  for (i=0; i < size; i++) {
    for (j=0; j < size; j++) {
      m[i*size+j]=coe[size-1-i+j];
    }
  }
}


int main(int argc, char *argv[]) {

  printf("WG size of kernel 1 = %d, WG size of kernel 2= %d X %d\n", BLOCK_SIZE_0, BLOCK_SIZE_1_X, BLOCK_SIZE_1_Y);
  float *a=NULL, *b=NULL, *finalVec=NULL;
  float *m=NULL;
  int size = -1;

  FILE *fp;

  // args
  char filename[200];
  int quiet=0,timing=0;

  // parse command line
  if (parseCommandline(argc, argv, filename,
        &quiet, &timing, &size)) {
    printUsage();
    return 0;
  }


  if(size < 1)
  {
    fp = fopen(filename, "r");
    fscanf(fp, "%d", &size);

    a = (float *) malloc(size * size * sizeof(float));
    InitMat(fp,size, a, size, size);

    b = (float *) malloc(size * sizeof(float));
    InitAry(fp, b, size);

    fclose(fp);

  }
  else
  {
    printf("create input internally before create, size = %d \n", size);

    a = (float *) malloc(size * size * sizeof(float));
    create_matrix(a, size);

    b = (float *) malloc(size * sizeof(float));
    for (int i =0; i< size; i++)
      b[i]=1.0;

  }

  if (!quiet) {    
    printf("The input matrix a is:\n");
    PrintMat(a, size, size, size);

    printf("The input array b is:\n");
    PrintAry(b, size);
  }

  // create the solution matrix
  m = (float *) malloc(size * size * sizeof(float));

  // create a new vector to hold the final answer

  finalVec = (float *) malloc(size * sizeof(float));

  InitPerRun(size,m);

  long long offload_start = get_time();
  ForwardSub(a,b,m,size,timing);
  long long offload_end = get_time();

  if (timing) {
    printf("Device offloading time %lld (us)\n\n",offload_end - offload_start);
  }

  if (!quiet) {
    printf("The result of array a is after forwardsub: \n");
    PrintMat(a, size, size, size);
    printf("The result of array b is after forwardsub: \n");
    PrintAry(b, size);
    printf("The result of matrix m is after forwardsub: \n");
    PrintMat(m, size, size, size);
    BackSub(a,b,finalVec,size);
    printf("The final solution is: \n");
    PrintAry(finalVec,size);
  }

  free(m);
  free(a);
  free(b);
  free(finalVec);
  return 0;
}

void
fan1 (const float* a, float* m, const int size, const int t,
      sycl::nd_item<3> item_ct1)
{
    int globalId = item_ct1.get_local_range().get(2) * item_ct1.get_group(2) +
                   item_ct1.get_local_id(2);
  if (globalId < size-1-t) {
    m[size * (globalId + t + 1)+t] = 
      a[size * (globalId + t + 1) + t] / a[size * t + t];
  }
}

void
fan2 (float* a, float* b, float* m, const int size, const int t,
      sycl::nd_item<3> item_ct1)
{
    int globalIdy = item_ct1.get_local_range().get(2) * item_ct1.get_group(2) +
                    item_ct1.get_local_id(2);
    int globalIdx = item_ct1.get_local_range().get(1) * item_ct1.get_group(1) +
                    item_ct1.get_local_id(1);
  if (globalIdx < size-1-t && globalIdy < size-t) {
    a[size*(globalIdx+1+t)+(globalIdy+t)] -= 
      m[size*(globalIdx+1+t)+t] * a[size*t+(globalIdy+t)];

    if(globalIdy == 0){
      b[globalIdx+1+t] -= 
        m[size*(globalIdx+1+t)+(globalIdy+t)] * b[t];
    }
  }
}

/*------------------------------------------------------
 ** ForwardSub() -- Forward substitution of Gaussian
 ** elimination.
 **------------------------------------------------------
 */
void ForwardSub(float *a, float *b, float *m, int size, int timing) {
    dpct::device_ext &dev_ct1 = dpct::get_current_device();
    sycl::queue &q_ct1 = dev_ct1.default_queue();

    sycl::range<3> blockDim_fan1(BLOCK_SIZE_0, 1, 1);
    sycl::range<3> gridDim_fan1((size + BLOCK_SIZE_0 - 1) / BLOCK_SIZE_0, 1, 1);

    sycl::range<3> blockDim_fan2(BLOCK_SIZE_1_Y, BLOCK_SIZE_1_X, 1);
    sycl::range<3> gridDim_fan2((size + BLOCK_SIZE_1_Y - 1) / BLOCK_SIZE_1_Y,
                                (size + BLOCK_SIZE_1_X - 1) / BLOCK_SIZE_1_X,
                                1);

  float *d_a, *d_b, *d_m;
    dpct::dpct_malloc((void **)&d_a, size * size * sizeof(float));
    dpct::dpct_malloc((void **)&d_b, size * sizeof(float));
    dpct::dpct_malloc((void **)&d_m, size * size * sizeof(float));

    dpct::dpct_memcpy(d_a, a, size * size * sizeof(float),
                      dpct::host_to_device);
    dpct::dpct_memcpy(d_b, b, size * sizeof(float), dpct::host_to_device);
    dpct::dpct_memcpy(d_m, m, size * size * sizeof(float),
                      dpct::host_to_device);

  for (int t=0; t<(size-1); t++) {
        {
            dpct::buffer_t d_a_buf_ct0 = dpct::get_buffer(d_a);
            dpct::buffer_t d_m_buf_ct1 = dpct::get_buffer(d_m);
            q_ct1.submit([&](sycl::handler &cgh) {
                auto d_a_acc_ct0 =
                    d_a_buf_ct0.get_access<sycl::access::mode::read_write>(cgh);
                auto d_m_acc_ct1 =
                    d_m_buf_ct1.get_access<sycl::access::mode::read_write>(cgh);

                auto dpct_global_range = gridDim_fan1 * blockDim_fan1;

                cgh.parallel_for(
                    sycl::nd_range<3>(sycl::range<3>(dpct_global_range.get(2),
                                                     dpct_global_range.get(1),
                                                     dpct_global_range.get(0)),
                                      sycl::range<3>(blockDim_fan1.get(2),
                                                     blockDim_fan1.get(1),
                                                     blockDim_fan1.get(0))),
                    [=](sycl::nd_item<3> item_ct1) {
                        fan1((const float *)(&d_a_acc_ct0[0]),
                             (float *)(&d_m_acc_ct1[0]), size, t, item_ct1);
                    });
            });
        }
        {
            dpct::buffer_t d_a_buf_ct0 = dpct::get_buffer(d_a);
            dpct::buffer_t d_b_buf_ct1 = dpct::get_buffer(d_b);
            dpct::buffer_t d_m_buf_ct2 = dpct::get_buffer(d_m);
            q_ct1.submit([&](sycl::handler &cgh) {
                auto d_a_acc_ct0 =
                    d_a_buf_ct0.get_access<sycl::access::mode::read_write>(cgh);
                auto d_b_acc_ct1 =
                    d_b_buf_ct1.get_access<sycl::access::mode::read_write>(cgh);
                auto d_m_acc_ct2 =
                    d_m_buf_ct2.get_access<sycl::access::mode::read_write>(cgh);

                auto dpct_global_range = gridDim_fan2 * blockDim_fan2;

                cgh.parallel_for(
                    sycl::nd_range<3>(sycl::range<3>(dpct_global_range.get(2),
                                                     dpct_global_range.get(1),
                                                     dpct_global_range.get(0)),
                                      sycl::range<3>(blockDim_fan2.get(2),
                                                     blockDim_fan2.get(1),
                                                     blockDim_fan2.get(0))),
                    [=](sycl::nd_item<3> item_ct1) {
                        fan2((float *)(&d_a_acc_ct0[0]),
                             (float *)(&d_b_acc_ct1[0]),
                             (float *)(&d_m_acc_ct2[0]), size, t, item_ct1);
                    });
            });
        }
  }

    dpct::dpct_memcpy(a, d_a, size * size * sizeof(float),
                      dpct::device_to_host);
    dpct::dpct_memcpy(b, d_b, size * sizeof(float), dpct::device_to_host);
    dpct::dpct_memcpy(m, d_m, size * size * sizeof(float),
                      dpct::device_to_host);

    dpct::dpct_free(d_a);
    dpct::dpct_free(d_b);
    dpct::dpct_free(d_m);
}


// Ke Wang add a function to generate input internally
int parseCommandline(int argc, char *argv[], char* filename,
    int *q, int *t, int *size){
  int i;
  if (argc < 2) return 1; // error
  // strncpy(filename,argv[1],100);
  char flag;

  for(i=1;i<argc;i++) {
    if (argv[i][0]=='-') {// flag
      flag = argv[i][1];
      switch (flag) {
        case 's': // matrix size
          i++;
          *size = atoi(argv[i]);
          printf("Create matrix internally in parse, size = %d \n", *size);
          break;
        case 'f': // file name
          i++;
          strncpy(filename,argv[i],100);
          printf("Read file from %s \n", filename);
          break;
        case 'h': // help
          return 1;
        case 'q': // quiet
          *q = 1;
          break;
        case 't': // timing
          *t = 1;
          break;
      }
    }
  }
  return 0;
}

void printUsage(){
  printf("Gaussian Elimination Usage\n");
  printf("\n");
  printf("gaussianElimination -f [filename] [-hqt]\n");
  printf("\n");
  printf("example:\n");
  printf("$ ./gaussianElimination matrix4.txt\n");
  printf("\n");
  printf("filename     the filename that holds the matrix data\n");
  printf("\n");
  printf("-h           Display the help file\n");
  printf("-q           Quiet mode. Suppress all text output.\n");
  printf("-t           Print timing information.\n");
  printf("-s           Specifiy the matrix size when the path to a matrix data file is not set.\n");
  printf("\n");
  printf("\n");
  printf("Notes: 1. The filename is required as the first parameter.\n");
  printf("       2. If you declare either the device or the platform,\n");
  printf("          you must declare both.\n\n");
}


/*------------------------------------------------------
 ** InitPerRun() -- Initialize the contents of the
 ** multipier matrix **m
 **------------------------------------------------------
 */
void InitPerRun(int size,float *m) 
{
  int i;
  for (i=0; i<size*size; i++)
    *(m+i) = 0.0;
}
void BackSub(float *a, float *b, float *finalVec, int size)
{
  // solve "bottom up"
  int i,j;
  for(i=0;i<size;i++){
    finalVec[size-i-1]=b[size-i-1];
    for(j=0;j<i;j++)
    {
      finalVec[size-i-1]-=*(a+size*(size-i-1)+(size-j-1)) * finalVec[size-j-1];
    }
    finalVec[size-i-1]=finalVec[size-i-1]/ *(a+size*(size-i-1)+(size-i-1));
  }
}
void InitMat(FILE *fp, int size, float *ary, int nrow, int ncol)
{
  int i, j;

  for (i=0; i<nrow; i++) {
    for (j=0; j<ncol; j++) {
      fscanf(fp, "%f",  ary+size*i+j);
    }
  }  
}
/*------------------------------------------------------
 ** InitAry() -- Initialize the array (vector) by reading
 ** data from the data file
 **------------------------------------------------------
 */
void InitAry(FILE *fp, float *ary, int ary_size)
{
  int i;

  for (i=0; i<ary_size; i++) {
    fscanf(fp, "%f",  &ary[i]);
  }
}  
/*------------------------------------------------------
 ** PrintMat() -- Print the contents of the matrix
 **------------------------------------------------------
 */
void PrintMat(float *ary, int size, int nrow, int ncol)
{
  int i, j;

  for (i=0; i<nrow; i++) {
    for (j=0; j<ncol; j++) {
      printf("%8.2e ", *(ary+size*i+j));
    }
    printf("\n");
  }
  printf("\n");
}

/*------------------------------------------------------
 ** PrintAry() -- Print the contents of the array (vector)
 **------------------------------------------------------
 */
void PrintAry(float *ary, int ary_size)
{
  int i;
  for (i=0; i<ary_size; i++) {
    printf("%.2e ", ary[i]);
  }
  printf("\n\n");
}
#endif

