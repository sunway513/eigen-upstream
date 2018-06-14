// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2014 Benoit Steiner <benoit.steiner.goog@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#if defined(EIGEN_USE_GPU) && !defined(EIGEN_CXX11_TENSOR_TENSOR_DEVICE_GPU_H)
#define EIGEN_CXX11_TENSOR_TENSOR_DEVICE_GPU_H

#if defined(EIGEN_HIPCC)

#define gpuStream_t hipStream_t
#define gpuDeviceProp_t hipDeviceProp_t
#define gpuError_t hipError_t
#define gpuSuccess hipSuccess
#define gpuErrorNotReady hipErrorNotReady
#define gpuGetDeviceCount hipGetDeviceCount
#define gpuGetErrorString hipGetErrorString
#define gpuGetDeviceProperties hipGetDeviceProperties
// FIXME : use hipStreamDefault instead of 0x00
#define gpuStreamDefault 0x00
#define gpuGetDevice hipGetDevice
#define gpuSetDevice hipSetDevice
#define gpuMalloc hipMalloc
#define gpuFree hipFree
#define gpuMemsetAsync hipMemsetAsync
#define gpuMemcpyAsync hipMemcpyAsync
#define gpuMemcpyDeviceToDevice hipMemcpyDeviceToDevice
#define gpuMemcpyDeviceToHost hipMemcpyDeviceToHost
#define gpuMemcpyHostToDevice hipMemcpyHostToDevice
#define gpuStreamQuery hipStreamQuery
#define gpuSharedMemConfig hipSharedMemConfig
#define gpuDeviceSetSharedMemConfig hipDeviceSetSharedMemConfig
#define gpuStreamSynchronize hipStreamSynchronize

#else

#define gpuStream_t cudaStream_t
#define gpuDeviceProp_t cudaDeviceProp
#define gpuError_t cudaError_t
#define gpuSuccess cudaSuccess
#define gpuErrorNotReady cudaErrorNotReady
#define gpuGetDeviceCount cudaGetDeviceCount
#define gpuGetErrorString cudaGetErrorString
#define gpuGetDeviceProperties cudaGetDeviceProperties
#define gpuStreamDefault cudaStreamDefault
#define gpuGetDevice cudaGetDevice
#define gpuSetDevice cudaSetDevice
#define gpuMalloc cudaMalloc
#define gpuFree cudaFree
#define gpuMemsetAsync cudaMemsetAsync
#define gpuMemcpyAsync cudaMemcpyAsync
#define gpuMemcpyDeviceToDevice cudaMemcpyDeviceToDevice
#define gpuMemcpyDeviceToHost cudaMemcpyDeviceToHost
#define gpuMemcpyHostToDevice cudaMemcpyHostToDevice
#define gpuStreamQuery cudaStreamQuery
#define gpuSharedMemConfig cudaSharedMemConfig
#define gpuDeviceSetSharedMemConfig cudaDeviceSetSharedMemConfig
#define gpuStreamSynchronize cudaStreamSynchronize

#endif


#if defined(EIGEN_HIP_DEVICE_COMPILE)
// HIPCC does not support the use of assert on the GPU side.
#undef assert
#define assert(COND)
#endif

namespace Eigen {

static const int kGpuScratchSize = 1024;

// This defines an interface that GPUDevice can take to use
// HIP / CUDA streams underneath.
class StreamInterface {
 public:
  virtual ~StreamInterface() {}

  virtual const gpuStream_t& stream() const = 0;
  virtual const gpuDeviceProp_t& deviceProperties() const = 0;

  // Allocate memory on the actual device where the computation will run
  virtual void* allocate(size_t num_bytes) const = 0;
  virtual void deallocate(void* buffer) const = 0;

  // Return a scratchpad buffer of size 1k
  virtual void* scratchpad() const = 0;

  // Return a semaphore. The semaphore is initially initialized to 0, and
  // each kernel using it is responsible for resetting to 0 upon completion
  // to maintain the invariant that the semaphore is always equal to 0 upon
  // each kernel start.
  virtual unsigned int* semaphore() const = 0;
};

static gpuDeviceProp_t* m_deviceProperties;
static bool m_devicePropInitialized = false;

static void initializeDeviceProp() {
  if (!m_devicePropInitialized) {
    // Attempts to ensure proper behavior in the case of multiple threads
    // calling this function simultaneously. This would be trivial to
    // implement if we could use std::mutex, but unfortunately mutex don't
    // compile with nvcc, so we resort to atomics and thread fences instead.
    // Note that if the caller uses a compiler that doesn't support c++11 we
    // can't ensure that the initialization is thread safe.
#if __cplusplus >= 201103L
    static std::atomic<bool> first(true);
    if (first.exchange(false)) {
#else
    static bool first = true;
    if (first) {
      first = false;
#endif
      // We're the first thread to reach this point.
      int num_devices;
      gpuError_t status = gpuGetDeviceCount(&num_devices);
      if (status != gpuSuccess) {
        std::cerr << "Failed to get the number of GPU devices: "
                  << gpuGetErrorString(status)
                  << std::endl;
        assert(status == gpuSuccess);
      }
      m_deviceProperties = new gpuDeviceProp_t[num_devices];
      for (int i = 0; i < num_devices; ++i) {
        status = gpuGetDeviceProperties(&m_deviceProperties[i], i);
        if (status != gpuSuccess) {
          std::cerr << "Failed to initialize GPU device #"
                    << i
                    << ": "
                    << gpuGetErrorString(status)
                    << std::endl;
          assert(status == gpuSuccess);
        }
      }

#if __cplusplus >= 201103L
      std::atomic_thread_fence(std::memory_order_release);
#endif
      m_devicePropInitialized = true;
    } else {
      // Wait for the other thread to inititialize the properties.
      while (!m_devicePropInitialized) {
#if __cplusplus >= 201103L
        std::atomic_thread_fence(std::memory_order_acquire);
#endif
        EIGEN_SLEEP(1000);
      }
    }
  }
}

static const gpuStream_t default_stream = gpuStreamDefault;

class GpuStreamDevice : public StreamInterface {
 public:
  // Use the default stream on the current device
  GpuStreamDevice() : stream_(&default_stream), scratch_(NULL), semaphore_(NULL) {
    gpuGetDevice(&device_);
    initializeDeviceProp();
  }
  // Use the default stream on the specified device
  GpuStreamDevice(int device) : stream_(&default_stream), device_(device), scratch_(NULL), semaphore_(NULL) {
    initializeDeviceProp();
  }
  // Use the specified stream. Note that it's the
  // caller responsibility to ensure that the stream can run on
  // the specified device. If no device is specified the code
  // assumes that the stream is associated to the current gpu device.
  GpuStreamDevice(const gpuStream_t* stream, int device = -1)
      : stream_(stream), device_(device), scratch_(NULL), semaphore_(NULL) {
    if (device < 0) {
      gpuGetDevice(&device_);
    } else {
      int num_devices;
      gpuError_t err = gpuGetDeviceCount(&num_devices);
      EIGEN_UNUSED_VARIABLE(err)
      assert(err == gpuSuccess);
      assert(device < num_devices);
      device_ = device;
    }
    initializeDeviceProp();
  }

  virtual ~GpuStreamDevice() {
    if (scratch_) {
      deallocate(scratch_);
    }
  }

  const gpuStream_t& stream() const { return *stream_; }
  const gpuDeviceProp_t& deviceProperties() const {
    return m_deviceProperties[device_];
  }
  virtual void* allocate(size_t num_bytes) const {
    gpuError_t err = gpuSetDevice(device_);
    EIGEN_UNUSED_VARIABLE(err)
    assert(err == gpuSuccess);
    void* result;
    err = gpuMalloc(&result, num_bytes);
    assert(err == gpuSuccess);
    assert(result != NULL);
    return result;
  }
  virtual void deallocate(void* buffer) const {
    gpuError_t err = gpuSetDevice(device_);
    EIGEN_UNUSED_VARIABLE(err)
    assert(err == gpuSuccess);
    assert(buffer != NULL);
    err = gpuFree(buffer);
    assert(err == gpuSuccess);
  }

  virtual void* scratchpad() const {
    if (scratch_ == NULL) {
      scratch_ = allocate(kGpuScratchSize + sizeof(unsigned int));
    }
    return scratch_;
  }

  virtual unsigned int* semaphore() const {
    if (semaphore_ == NULL) {
      char* scratch = static_cast<char*>(scratchpad()) + kGpuScratchSize;
      semaphore_ = reinterpret_cast<unsigned int*>(scratch);
      gpuError_t err = gpuMemsetAsync(semaphore_, 0, sizeof(unsigned int), *stream_);
      EIGEN_UNUSED_VARIABLE(err)
      assert(err == gpuSuccess);
    }
    return semaphore_;
  }

 private:
  const gpuStream_t* stream_;
  int device_;
  mutable void* scratch_;
  mutable unsigned int* semaphore_;
};

struct GpuDevice {
  // The StreamInterface is not owned: the caller is
  // responsible for its initialization and eventual destruction.
  explicit GpuDevice(const StreamInterface* stream) : stream_(stream), max_blocks_(INT_MAX) {
    eigen_assert(stream);
  }
  explicit GpuDevice(const StreamInterface* stream, int num_blocks) : stream_(stream), max_blocks_(num_blocks) {
    eigen_assert(stream);
  }
  // TODO(bsteiner): This is an internal API, we should not expose it.
  EIGEN_STRONG_INLINE const gpuStream_t& stream() const {
    return stream_->stream();
  }

  EIGEN_STRONG_INLINE void* allocate(size_t num_bytes) const {
    return stream_->allocate(num_bytes);
  }

  EIGEN_STRONG_INLINE void deallocate(void* buffer) const {
    stream_->deallocate(buffer);
  }

  EIGEN_STRONG_INLINE void* scratchpad() const {
    return stream_->scratchpad();
  }

  EIGEN_STRONG_INLINE unsigned int* semaphore() const {
    return stream_->semaphore();
  }

  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void memcpy(void* dst, const void* src, size_t n) const {
#ifndef EIGEN_GPU_COMPILE_PHASE
    gpuError_t err = gpuMemcpyAsync(dst, src, n, gpuMemcpyDeviceToDevice,
                                      stream_->stream());
    EIGEN_UNUSED_VARIABLE(err)
    assert(err == gpuSuccess);
#else
    EIGEN_UNUSED_VARIABLE(dst);
    EIGEN_UNUSED_VARIABLE(src);
    EIGEN_UNUSED_VARIABLE(n);
    eigen_assert(false && "The default device should be used instead to generate kernel code");
#endif
  }

  EIGEN_STRONG_INLINE void memcpyHostToDevice(void* dst, const void* src, size_t n) const {
    gpuError_t err =
        gpuMemcpyAsync(dst, src, n, gpuMemcpyHostToDevice, stream_->stream());
    EIGEN_UNUSED_VARIABLE(err)
    assert(err == gpuSuccess);
  }

  EIGEN_STRONG_INLINE void memcpyDeviceToHost(void* dst, const void* src, size_t n) const {
    gpuError_t err =
        gpuMemcpyAsync(dst, src, n, gpuMemcpyDeviceToHost, stream_->stream());
    EIGEN_UNUSED_VARIABLE(err)
    assert(err == gpuSuccess);
  }

  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void memset(void* buffer, int c, size_t n) const {
#ifndef EIGEN_GPU_COMPILE_PHASE
    gpuError_t err = gpuMemsetAsync(buffer, c, n, stream_->stream());
    EIGEN_UNUSED_VARIABLE(err)
    assert(err == gpuSuccess);
#else
  eigen_assert(false && "The default device should be used instead to generate kernel code");
#endif
  }

  EIGEN_STRONG_INLINE size_t numThreads() const {
    // FIXME
    return 32;
  }

  EIGEN_STRONG_INLINE size_t firstLevelCacheSize() const {
    // FIXME
    return 48*1024;
  }

  EIGEN_STRONG_INLINE size_t lastLevelCacheSize() const {
    // We won't try to take advantage of the l2 cache for the time being, and
    // there is no l3 cache on hip/cuda devices.
    return firstLevelCacheSize();
  }

  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void synchronize() const {
#if defined(EIGEN_GPUCC) && !defined(EIGEN_GPU_COMPILE_PHASE)
    gpuError_t err = gpuStreamSynchronize(stream_->stream());
    if (err != gpuSuccess) {
      std::cerr << "Error detected in GPU stream: "
                << gpuGetErrorString(err)
                << std::endl;
      assert(err == gpuSuccess);
    }
#else
    assert(false && "The default device should be used instead to generate kernel code");
#endif
  }

  EIGEN_STRONG_INLINE int getNumGpuMultiProcessors() const {
    return stream_->deviceProperties().multiProcessorCount;
  }
  EIGEN_STRONG_INLINE int maxGpuThreadsPerBlock() const {
    return stream_->deviceProperties().maxThreadsPerBlock;
  }
  EIGEN_STRONG_INLINE int maxGpuThreadsPerMultiProcessor() const {
    return stream_->deviceProperties().maxThreadsPerMultiProcessor;
  }
  EIGEN_STRONG_INLINE int sharedMemPerBlock() const {
    return stream_->deviceProperties().sharedMemPerBlock;
  }
  EIGEN_STRONG_INLINE int majorDeviceVersion() const {
    return stream_->deviceProperties().major;
  }
  EIGEN_STRONG_INLINE int minorDeviceVersion() const {
    return stream_->deviceProperties().minor;
  }

  EIGEN_STRONG_INLINE int maxBlocks() const {
    return max_blocks_;
  }

  // This function checks if the GPU runtime recorded an error for the
  // underlying stream device.
  inline bool ok() const {
#ifdef EIGEN_GPUCC
    gpuError_t error = gpuStreamQuery(stream_->stream());
    return (error == gpuSuccess) || (error == gpuErrorNotReady);
#else
    return false;
#endif
  }

 private:
  const StreamInterface* stream_;
  int max_blocks_;
};

#if defined(EIGEN_HIPCC)

#define LAUNCH_GPU_KERNEL(kernel, gridsize, blocksize, sharedmem, device, ...)             \
  hipLaunchKernelGGL(HIP_KERNEL_NAME(kernel), dim3(gridsize), dim3(blocksize), (sharedmem), (device).stream(), __VA_ARGS__); \
  assert(hipGetLastError() == hipSuccess);

#else
 
#define LAUNCH_GPU_KERNEL(kernel, gridsize, blocksize, sharedmem, device, ...)             \
  (kernel) <<< (gridsize), (blocksize), (sharedmem), (device).stream() >>> (__VA_ARGS__);   \
  assert(cudaGetLastError() == cudaSuccess);

#endif
 
// FIXME: Should be device and kernel specific.
#ifdef EIGEN_GPUCC
static EIGEN_DEVICE_FUNC inline void setGpuSharedMemConfig(gpuSharedMemConfig config) {
#ifndef EIGEN_GPU_COMPILE_PHASE
  gpuError_t status = gpuDeviceSetSharedMemConfig(config);
  EIGEN_UNUSED_VARIABLE(status)
  assert(status == gpuSuccess);
#else
  EIGEN_UNUSED_VARIABLE(config)
#endif
}
#endif

}  // end namespace Eigen

#endif  // EIGEN_CXX11_TENSOR_TENSOR_DEVICE_GPU_H
