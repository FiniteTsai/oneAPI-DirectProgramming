#include <CL/sycl.hpp>
#include <dpct/dpct.hpp>

using namespace std;

#ifdef SINGLE_PRECISION
#define T float 
#define T2 float2
#else
#define T double
#define T2 sycl::double2
#endif


#ifndef M_SQRT1_2
# define M_SQRT1_2      0.70710678118654752440f
#endif


#define exp_1_8   (T2){  1, -1 }//requires post-multiply by 1/sqrt(2)
#define exp_1_4   (T2){  0, -1 }
#define exp_3_8   (T2){ -1, -1 }//requires post-multiply by 1/sqrt(2)

#define iexp_1_8   (T2){  1, 1 }//requires post-multiply by 1/sqrt(2)
#define iexp_1_4   (T2){  0, 1 }
#define iexp_3_8   (T2){ -1, 1 }//requires post-multiply by 1/sqrt(2)

SYCL_EXTERNAL
T2 exp_i( T phi ) {
  return (T2){sycl::cos(phi), sycl::sin(phi)};
}

SYCL_EXTERNAL
T2 cmplx_mul(T2 a, T2 b) {
  return (T2){a.x() * b.x() - a.y() * b.y(), a.x() * b.y() + a.y() * b.x()};
}

T2 cm_fl_mul(T2 a, T b) { return (T2){b * a.x(), b *a.y()}; }

T2 cmplx_add(T2 a, T2 b) { return (T2){a.x() + b.x(), a.y() + b.y()}; }
SYCL_EXTERNAL
T2 cmplx_sub(T2 a, T2 b) { return (T2){a.x() - b.x(), a.y() - b.y()}; }

#define FFT2(a0, a1)                            \
{                                               \
  T2 c0 = *a0;                           \
  *a0 = cmplx_add(c0,*a1);                    \
  *a1 = cmplx_sub(c0,*a1);                    \
}

#define FFT4(a0, a1, a2, a3)                    \
{                                               \
  FFT2( a0, a2 );                             \
  FFT2( a1, a3 );                             \
  *a3 = cmplx_mul(*a3,exp_1_4);               \
  FFT2( a0, a1 );                             \
  FFT2( a2, a3 );                             \
}

#define FFT8(a)                                                 \
{                                                               \
  FFT2( &a[0], &a[4] );                                       \
  FFT2( &a[1], &a[5] );                                       \
  FFT2( &a[2], &a[6] );                                       \
  FFT2( &a[3], &a[7] );                                       \
  \
  a[5] = cm_fl_mul( cmplx_mul(a[5],exp_1_8) , M_SQRT1_2 );    \
  a[6] =  cmplx_mul( a[6] , exp_1_4);                         \
  a[7] = cm_fl_mul( cmplx_mul(a[7],exp_3_8) , M_SQRT1_2 );    \
  \
  FFT4( &a[0], &a[1], &a[2], &a[3] );                         \
  FFT4( &a[4], &a[5], &a[6], &a[7] );                         \
}

#define IFFT2 FFT2

#define IFFT4( a0, a1, a2, a3 )                 \
{                                               \
  IFFT2( a0, a2 );                            \
  IFFT2( a1, a3 );                            \
  *a3 = cmplx_mul(*a3 , iexp_1_4);            \
  IFFT2( a0, a1 );                            \
  IFFT2( a2, a3);                             \
}

#define IFFT8( a )                                              \
{                                                               \
  IFFT2( &a[0], &a[4] );                                      \
  IFFT2( &a[1], &a[5] );                                      \
  IFFT2( &a[2], &a[6] );                                      \
  IFFT2( &a[3], &a[7] );                                      \
  \
  a[5] = cm_fl_mul( cmplx_mul(a[5],iexp_1_8) , M_SQRT1_2 );   \
  a[6] = cmplx_mul( a[6] , iexp_1_4);                         \
  a[7] = cm_fl_mul( cmplx_mul(a[7],iexp_3_8) , M_SQRT1_2 );   \
  \
  IFFT4( &a[0], &a[1], &a[2], &a[3] );                        \
  IFFT4( &a[4], &a[5], &a[6], &a[7] );                        \
}

// CUDA kernels
#include "fft1D_512.h"
#include "ifft1D_512.h"

int main(int argc, char** argv)
{
  dpct::device_ext &dev_ct1 = dpct::get_current_device();
  sycl::queue &q_ct1 = dev_ct1.default_queue();

  srand(2);
  int i;

  int select = atoi(argv[1]);
  int passes = atoi(argv[2]);

  // Convert to MB
  int probSizes[4] = { 1, 8, 96, 256 };
  unsigned long bytes = 0;
  bytes = probSizes[select];
  bytes *= 1024 * 1024;

  // now determine how much available memory will be used
  const int half_n_ffts = bytes / (512*sizeof(T2)*2);
  const int n_ffts = half_n_ffts * 2;
  const int half_n_cmplx = half_n_ffts * 512;
  const unsigned long used_bytes = half_n_cmplx * 2 * sizeof(T2);
  const double N = (double)half_n_cmplx*2.0;

  fprintf(stdout, "used_bytes=%lu, N=%g\n", used_bytes, N);

  // allocate host memory, in-place FFT/iFFT operations
  T2 *source = (T2*) malloc (used_bytes);

  T2 *reference = (T2*) malloc (used_bytes);


  // init host memory...
  for (i = 0; i < half_n_cmplx; i++) {
    source[i].x() = (rand() / (float)RAND_MAX) * 2 - 1;
    source[i].y() = (rand() / (float)RAND_MAX) * 2 - 1;
    source[i + half_n_cmplx].x() = source[i].x();
    source[i + half_n_cmplx].y() = source[i].y();
  }

  memcpy(reference, source, used_bytes);

  const char *sizeStr;
  stringstream ss;
  ss << "N=" << (long)N;
  sizeStr = strdup(ss.str().c_str());

  auto start = std::chrono::steady_clock::now();

  T2 *d_source;
  d_source = sycl::malloc_device<sycl::double2>((long)N, q_ct1);
  q_ct1.memcpy(d_source, source, (long)N * sizeof(T2)).wait();

  for (int k=0; k<passes; k++) {
    q_ct1.submit([&](sycl::handler &cgh) {
      sycl::accessor<T, 1, sycl::access::mode::read_write,
                     sycl::access::target::local>
          smem_acc_ct1(sycl::range<1>(576 /*8*8*9*/), cgh);

      cgh.parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, n_ffts) *
                                             sycl::range<3>(1, 1, 64),
                                         sycl::range<3>(1, 1, 64)),
                       [=](sycl::nd_item<3> item_ct1) {
                         fft1D_512(d_source, item_ct1,
                                   smem_acc_ct1.get_pointer());
                       });
    });
    q_ct1.submit([&](sycl::handler &cgh) {
      sycl::accessor<T, 1, sycl::access::mode::read_write,
                     sycl::access::target::local>
          smem_acc_ct1(sycl::range<1>(576 /*8*8*9*/), cgh);

      cgh.parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, n_ffts) *
                                             sycl::range<3>(1, 1, 64),
                                         sycl::range<3>(1, 1, 64)),
                       [=](sycl::nd_item<3> item_ct1) {
                         ifft1D_512(d_source, item_ct1,
                                    smem_acc_ct1.get_pointer());
                       });
    });
  }

  q_ct1.memcpy(source, d_source, (long)N * sizeof(T2)).wait();
  sycl::free(d_source, q_ct1);

  auto end = std::chrono::steady_clock::now();
  auto time = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  std::cout << "Average time " << (time * 1e-9f) / passes << " (s)\n";

  // Verification
  bool error = false;
  for (int i = 0; i < N; i++) {
    if (fabs((T)source[i].x() - (T)reference[i].x()) > 1e-6) {
      //std::cout << i << " " << (T)source[i].x << " " << (T)reference[i].x << std::endl;
      error = true;
      break;
    }
    if (fabs((T)source[i].y() - (T)reference[i].y()) > 1e-6) {
      //std::cout << i << " " << (T)source[i].y << " " << (T)reference[i].y << std::endl;
      error = true;
      break;
    }
  }
  std::cout << (error ? "FAIL" : "PASS")  << std::endl;
  free(reference);
  free(source);
}
