/*
 *
 * This file is part of the open-source SeetaFace engine, which includes three modules:
 * SeetaFace Detection, SeetaFace Alignment, and SeetaFace Identification.
 *
 * This file is part of the SeetaFace Detection module, containing codes implementing the
 * face detection method described in the following paper:
 *
 *
 *   VIPLFaceNet: An Open Source Deep Face Recognition SDK,
 *   Xin Liu, Meina Kan, Wanglong Wu, Shiguang Shan, Xilin Chen.
 *   In Frontiers of Computer Science.
 *
 *
 * Copyright (C) 2016, Visual Information Processing and Learning (VIPL) group,
 * Institute of Computing Technology, Chinese Academy of Sciences, Beijing, China.
 *
 * The codes are mainly developed by Zining Xu(a M.S. supervised by Prof. Shiguang Shan)
 *
 * As an open-source face recognition engine: you can redistribute SeetaFace source codes
 * and/or modify it under the terms of the BSD 2-Clause License.
 *
 * You should have received a copy of the BSD 2-Clause License along with the software.
 * If not, see < https://opensource.org/licenses/BSD-2-Clause>.
 *
 * Contact Info: you can send an email to SeetaFace@vipl.ict.ac.cn for any problems.
 *
 * Note: the above information must be kept whenever or wherever the codes are used.
 * 
 * -----------------------------------------------------------------------------------------------------
 * The GPU acceleration parts of this file are developed by Xuanzhi LIU (Walker LAU).
 * 
 * If you want to get the latest version of this project or met any problems,
 * please go to <https://github.com/WalkerLau/GPU-CNN> , 
 * I will try to help as much as I can.
 * 
 * You can redistribute this source codes and/or modify it under the terms of the BSD 2-Clause License.
 *
 * Note: the above information must be kept whenever or wherever the codes are used.
 *
 */

#include <cuda_runtime_api.h>
#include <cuda_runtime.h>
#include "conv_net.h"
#include "math_functions.h"
#include "math_functions.cuh"
#include "time.h"
#include "ctime"

#define CONV1 3*9*9*48	    // number of elements in filters of CONV1 is 3*9*9*48
#define CONV2 48*3*3*128	// number of elements in filters of CONV2 is 48*3*3*128
#define CONV3 128*3*3*128	// number of elements in filters of CONV3 is 128*3*3*128
#define CONV4 128*3*3*256	// number of elements in filters of CONV4 is 128*3*3*256
#define CONV5 256*3*3*192	// number of elements in filters of CONV5 is 256*3*3*192
#define CONV6 192*3*3*192	// number of elements in filters of CONV6 is 192*3*3*192
#define CONV7 192*3*3*128	// number of elements in filters of CONV7 is 192*3*3*128

#ifdef __VIPL_LOG__
#include <ctime>
#endif

void ConvNet::SetUp() {
  stride_h_ = stride_w_ =
      *(int*)(this->hyper_param()->param("stride"));

  // check input and output blob size
  this->input_blobs().resize(1);
  this->output_blobs().resize(1);
  this->input_plugs().resize(1);
  this->output_plugs().resize(1);
  this->params().resize(1);
}

void ConvNet::Execute() {
#ifdef __VIPL_LOG__
  double t_start, t_end, scan_time, math_time;
#endif
  // *** Argument *** //
  const bool is_binary = false;
  // *** //

  CheckInput();
  const Blob* const input = this->input_blobs(0);
  const Blob* const weight = this->params(0);
  Blob* const output = this->output_blobs(0);

  int src_num = input->num();   // 经测试，这里src_num总是等于1
  int src_channels = input->channels();
  int src_h = input->height();
  int src_w = input->width();
  int dst_channels = weight->num();
  int kernel_h = weight->height();
  int kernel_w = weight->width();

  LOG(DEBUG) << "input blob: (" <<src_num << "," << src_channels << "," << src_h
    << "," << src_w << ")";

  int dst_h = (src_h - kernel_h) / stride_h_ + 1;	// 纵向移窗数
  int dst_w = (src_w - kernel_w) / stride_w_ + 1;	// 横向移窗数
  int end_h = src_h - kernel_h + 1;
  int end_w = src_w - kernel_w + 1;
  int dst_size = dst_h * dst_w;
  int kernel_size = src_channels * kernel_h * kernel_w;
  const int src_num_offset = src_channels * src_h * src_w;

  float* dPtr_ifm;
  float* dPtr_ofm;
  size_t size_ifm = src_w * src_h * src_channels;
  size_t size_ofm = dst_w * dst_h * dst_channels;
  cudaMalloc((void **)&dPtr_ifm, size_ifm * sizeof(float));
  cudaMalloc((void **)&dPtr_ofm, size_ofm * sizeof(float));
  float* const dst_head =
	  new float[src_num * dst_size * dst_channels];		// 用来存一层卷积后的结果（output volume）。
  float* const mat_head =
	  new float[dst_size * kernel_size];	// 用来存去除了num维度的、与卷积核对应分配的input的元素。

  // chg: const float* src_data = input->data().get();
  const float* src_data = input->data().get();
  float* dst_data = dPtr_ofm;
  int didx = 0;
#ifdef __VIPL_LOG__
  scan_time = math_time = 0;
#endif
  for (int sn = 0; sn < src_num; ++sn) {				// 切换input的num维度，但记住src_num在此等于1。
#ifdef __VIPL_LOG__
    t_start = clock();
#endif

  //chg 改：双模运行模式对memcpy的改进
  if((CONV1 != kernel_size * dst_channels) && CUDA_C){   //  128*3*3*256 != kernel_size * dst_channels
    cudaMemcpy(dPtr_ifm, src_data, size_ifm * sizeof(float), cudaMemcpyHostToDevice);   // chg 加：直接拷贝ifmap volume的数据。
  }
  else{
    float* mat_data = mat_head;
    for (int sh = 0; sh < end_h; sh += stride_h_) {		// 纵向移窗。
      for (int sw = 0; sw < end_w; sw += stride_w_) {	// 横向移窗。
        for (int sc = 0; sc < src_channels; ++sc) {		// 切换input的channel维度。
          int src_off = (sc * src_h + sh) * src_w + sw; // 从src_off中可以看出src_data的数据组织结构从高维到低维是c-h-w。
          for (int hidx = 0; hidx < kernel_h; ++hidx) {
            memcpy(mat_data, src_data + src_off,		// 这几个循环的作用是将src_data中的数据进行对应卷积核的复制与分配，存到mat_data中。
                    sizeof(float) * kernel_w);
            mat_data += kernel_w;
            src_off += src_w;
          }
        } // for sc
      } // for sw
    } // for sh
    src_data += src_num_offset;
  }
  

#ifdef __VIPL_LOG__
    t_end = clock();
    scan_time += t_end - t_start;

    t_start = clock();
#endif

  //manage filter memory
  float* dPtr_weights;
  size_t size_weights = kernel_size * dst_channels;
  const float* weight_head = weight->data().get();
  float* ptr_temp = weight->data().get();
  cudaMalloc((void **)&dPtr_weights, size_weights * sizeof(float));
  cudaMemcpy((void **)dPtr_weights, ptr_temp, size_weights*sizeof(float), cudaMemcpyHostToDevice);

  if((CONV1 != kernel_size * dst_channels) && CUDA_C){
    clock_t start_clock, cnt = 0;
    cudaDeviceSynchronize();
    start_clock = clock();
    cuda_matrix_procuct(dPtr_ifm, dPtr_weights, dPtr_ofm, dst_size, dst_channels, kernel_size);
    // fetch output-feature-maps from GPU
    cudaMemcpy(dst_head, dPtr_ofm, size_ofm*sizeof(float), cudaMemcpyDeviceToHost);
    cudaDeviceSynchronize();
    cnt = clock() - start_clock;
    std::cout << "GPU Conv layer clock = " << cnt ; 
    std::cout << "     time = " << 1000.0 *  cnt / CLOCKS_PER_SEC << " ms" << std::endl;
    cudaFree(dPtr_weights);
  }
  else{
    clock_t start_clock, cnt = 0;
    start_clock = clock();
    matrix_procuct(mat_head, weight_head, dst_head, dst_size, dst_channels, kernel_size, true, false);
    cnt = clock() - start_clock;
    std::cout << "CPU Conv layer clock = " << cnt ; 
    std::cout << "    time = " << 1000.0 *  cnt / CLOCKS_PER_SEC << " ms" << std::endl;
  }

#ifdef __VIPL_LOG__
    t_end = clock();
    math_time += t_end - t_start;
#endif
    dst_data += dst_channels * dst_size;
  } // for sn

#ifdef __VIPL_LOG__
  LOG(INFO) << "scan time: " << scan_time / CLOCKS_PER_SEC * 1000 << "ms";
  LOG(INFO) << "math time: " << math_time / CLOCKS_PER_SEC * 1000 << "ms";
#endif
  output->CopyData(src_num, dst_channels, dst_h, dst_w, dst_head);

  delete[] mat_head;
  delete[] dst_head;
  cudaFree(dPtr_ifm);
  cudaFree(dPtr_ofm);


  LOG(DEBUG) << "output blob: (" << output->num() << "," << output->channels()
    << "," << output->height() << "," << output->width() << ")";
  CheckOutput();
}

REGISTER_NET_CLASS(Conv);
