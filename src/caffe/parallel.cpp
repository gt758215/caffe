#ifndef CPU_ONLY
#include <cuda_runtime.h>
#endif
#include <glog/logging.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "boost/thread.hpp"
#include "caffe/caffe.hpp"
#include "caffe/parallel.hpp"

namespace caffe {

enum Op {
  copy,
  replace_cpu,
  replace_gpu,
  replace_cpu_diff,
  replace_gpu_diff
};

template<typename Dtype>
static void apply_buffers(const vector<shared_ptr<Blob<Dtype> > >& blobs,
                          Dtype* buffer, size_t total_size, Op op) {
  Dtype* ptr = buffer;
  for (int i = 0; i < blobs.size(); ++i) {
    int size = blobs[i]->count();
    switch (op) {
      case copy: {
        // Init buffer to current values of blobs
        caffe_copy(size,
                   reinterpret_cast<const Dtype*>(blobs[i]->data()->cpu_data()),
                   ptr);
        break;
      }
      case replace_cpu:
        blobs[i]->data()->set_cpu_data(ptr);
        break;
      case replace_gpu:
        blobs[i]->data()->set_gpu_data(ptr);
        break;
      case replace_cpu_diff:
        blobs[i]->diff()->set_cpu_data(ptr);
        break;
      case replace_gpu_diff:
        blobs[i]->diff()->set_gpu_data(ptr);
        break;
    }
    ptr += size;
  }
  CHECK_EQ(total_size, ptr - buffer);
}

// Buffer size necessary to store given blobs
template<typename Dtype>
static size_t total_size(const vector<shared_ptr<Blob<Dtype> > >& params) {
  size_t size = 0;
  for (int i = 0; i < params.size(); ++i)
    size += params[i]->count();
  return size;
}

template<typename Dtype>
Params<Dtype>::Params(shared_ptr<Solver<Dtype> > root_solver)
    : size_(total_size<Dtype>(root_solver->net()->params())),
      data_(),
      diff_() {
}

template<typename Dtype>
GPUParams<Dtype>::GPUParams(shared_ptr<Solver<Dtype> > root_solver, int device)
    : Params<Dtype>(root_solver) {
#ifndef CPU_ONLY
  int initial_device;
  CUDA_CHECK(cudaGetDevice(&initial_device));

  // Allocate device buffers
  CUDA_CHECK(cudaSetDevice(device));
  CUDA_CHECK(cudaMalloc(&data_, size_ * sizeof(Dtype)));

  // Copy blob values
  const vector<shared_ptr<Blob<Dtype> > >& net = root_solver->net()->params();
  apply_buffers(net, data_, size_, copy);

  CUDA_CHECK(cudaMalloc(&diff_, size_ * sizeof(Dtype)));
  caffe_gpu_set(size_, Dtype(0), diff_);

  CUDA_CHECK(cudaSetDevice(initial_device));
#else
  NO_GPU;
#endif
}

template<typename Dtype>
GPUParams<Dtype>::~GPUParams() {
#ifndef CPU_ONLY
  CUDA_CHECK(cudaFree(data_));
  CUDA_CHECK(cudaFree(diff_));
#endif
}

template<typename Dtype>
void GPUParams<Dtype>::configure(Solver<Dtype>* solver) const {
  const vector<shared_ptr<Blob<Dtype> > >& net = solver->net()->params();
  apply_buffers(net, data_, size_, replace_gpu);
  apply_buffers(net, diff_, size_, replace_gpu_diff);
}

//

void DevicePair::compute(const vector<int> devices, vector<DevicePair>* pairs) {
#ifndef CPU_ONLY
  vector<int> remaining(devices);

  // Group GPUs by board
  for (int i = 0; i < remaining.size(); ++i) {
    for (int j = i + 1; j < remaining.size(); ++j) {
      cudaDeviceProp a, b;
      CUDA_CHECK(cudaGetDeviceProperties(&a, remaining[i]));
      CUDA_CHECK(cudaGetDeviceProperties(&b, remaining[j]));
      if (a.isMultiGpuBoard && b.isMultiGpuBoard) {
        if (a.multiGpuBoardGroupID == b.multiGpuBoardGroupID) {
          pairs->push_back(DevicePair(remaining[i], remaining[j]));
          DLOG(INFO) << "GPU board: " << remaining[i] << ":" << remaining[j];
          remaining.erase(remaining.begin() + j);
          break;
        }
      }
    }
  }
  ostringstream s;
  for (int i = 0; i < remaining.size(); ++i) {
    s << (i ? ", " : "") << remaining[i];
  }
  DLOG(INFO) << "GPUs paired by boards, remaining: " << s.str();

  // Group by P2P accessibility
  for (int i = 0; i < remaining.size(); ++i) {
    for (int j = i + 1; j < remaining.size(); ++j) {
      int access;
      CUDA_CHECK(cudaDeviceCanAccessPeer(&access, remaining[i], remaining[j]));
      if (access) {
        pairs->push_back(DevicePair(remaining[i], remaining[j]));
        DLOG(INFO) << "P2P pair: " << remaining[i] << ":" << remaining[j];
        remaining.erase(remaining.begin() + j);
        break;
      }
    }
  }
  s.str("");
  for (int i = 0; i < remaining.size(); ++i) {
    s << (i ? ", " : "") << remaining[i];
  }
  DLOG(INFO) << "GPUs paired by P2P access, remaining: " << s.str();

  // Group remaining
  for (int i = 0; i < remaining.size(); ++i) {
    for (int j = i + 1; j < remaining.size(); ++j) {
      pairs->push_back(DevicePair(remaining[i], remaining[j]));
      DLOG(INFO) << "Remaining pair: " << remaining[i] << ":" << remaining[j];
      remaining.erase(remaining.begin() + j);
      break;
    }
  }
  CHECK_EQ(remaining.size(), 1);
  pairs->insert(pairs->begin(), DevicePair(-1, remaining[0]));

  CHECK(pairs->size() == devices.size());
  for (int i = 0; i < pairs->size(); ++i) {
    CHECK((*pairs)[i].parent() != (*pairs)[i].device());
    for (int j = i + 1; j < pairs->size(); ++j) {
      CHECK((*pairs)[i].device() != (*pairs)[j].device());
    }
  }
#else
  NO_GPU;
#endif
}

//

template<typename Dtype>
P2PSync<Dtype>::P2PSync(shared_ptr<Solver<Dtype> > root_solver,
                        P2PSync<Dtype>* parent, const SolverParameter& param)
    : GPUParams<Dtype>(root_solver, param.device_id()),
      parent_(parent),
      children_(),
      queue_(),
      initial_iter_(root_solver->iter()),
      solver_() {
#ifndef CPU_ONLY
  int initial_device;
  CUDA_CHECK(cudaGetDevice(&initial_device));
  const int self = param.device_id();
  CUDA_CHECK(cudaSetDevice(self));

  if (parent == NULL) {
    solver_ = root_solver;
  } else {
    Caffe::set_root_solver(false);
    solver_.reset(new Solver<Dtype>(param));
    Caffe::set_root_solver(true);
  }
  this->configure(solver_.get());
  solver_->add_callback(this);

  if (parent) {
    // Enable p2p access between devices
    const int peer = parent->solver_->param().device_id();
    int access;
    CUDA_CHECK(cudaDeviceCanAccessPeer(&access, self, peer));
    if (access) {
      CUDA_CHECK(cudaDeviceEnablePeerAccess(peer, 0));
    } else {
      LOG(INFO)<< "GPU " << self << " does not have p2p access to GPU " << peer;
    }
    // Allocate receiving buffer on parent
    CUDA_CHECK(cudaSetDevice(peer));
    CUDA_CHECK(cudaMalloc(&parent_grads_, size_ * sizeof(Dtype)));
    CUDA_CHECK(cudaSetDevice(self));
  }

  CUDA_CHECK(cudaSetDevice(initial_device));
#else
  NO_GPU;
#endif
}

template<typename Dtype>
P2PSync<Dtype>::~P2PSync() {
#ifndef CPU_ONLY
  int initial_device;
  CUDA_CHECK(cudaGetDevice(&initial_device));
  const int self = solver_->param().device_id();
  CUDA_CHECK(cudaSetDevice(self));

  if (parent_) {
    CUDA_CHECK(cudaFree(parent_grads_));
    const int peer = parent_->solver_->param().device_id();
    int access;
    CUDA_CHECK(cudaDeviceCanAccessPeer(&access, self, peer));
    if (access) {
      CUDA_CHECK(cudaDeviceDisablePeerAccess(peer));
    }
  }

  CUDA_CHECK(cudaSetDevice(initial_device));
#endif
}

template<typename Dtype>
void P2PSync<Dtype>::InternalThreadEntry() {
  Caffe::SetDevice(solver_->param().device_id());
  CHECK(Caffe::root_solver());
  Caffe::set_root_solver(false);
  // See if there is a defined seed and reset random state if so
  if (solver_->param().random_seed() >= 0) {
    // Fetch random seed and modulate by device ID to make sure
    // everyone doesn't have the same seed.  We seem to have some
    // solver instability if we have everyone with the same seed
    Caffe::set_random_seed(
        solver_->param().random_seed() + solver_->param().device_id());
  }
  solver_->Step(solver_->param().max_iter() - initial_iter_);
}

template<typename Dtype>
void P2PSync<Dtype>::on_start(Timer* timer, ostringstream* timing) {
#ifndef CPU_ONLY
#ifdef DEBUG
  int device;
  CUDA_CHECK(cudaGetDevice(&device));
  CHECK(device == solver_->param().device_id());
#else
//  CHECK(false);
#endif

  // Wait for update from parent
  if (parent_) {
    timer->Start();
    P2PSync<Dtype> *parent = queue_.pop();
    CHECK(parent == parent_);
    *timing << " recv_param: " << timer->MilliSeconds();
  }

  // Update children
  if (children_.size()) {
    timer->Start();
  }
  for (int i = 0; i < children_.size(); ++i) {
    Dtype* src = data_;
    Dtype* dst = children_[i]->data_;

#ifdef DEBUG
    cudaPointerAttributes attributes;
    CUDA_CHECK(cudaPointerGetAttributes(&attributes, src));
    CHECK(attributes.device == device);
    CUDA_CHECK(cudaPointerGetAttributes(&attributes, dst));
    CHECK(attributes.device == children_[i]->solver_->param().device_id());
#endif

    CUDA_CHECK(cudaMemcpyAsync(dst, src, size_ * sizeof(Dtype),  //
        cudaMemcpyDeviceToDevice, cudaStreamDefault));
  }
  if (children_.size()) {
    CUDA_CHECK(cudaStreamSynchronize(cudaStreamDefault));
  }
  for (int i = 0; i < children_.size(); ++i) {
    children_[i]->queue_.push(this);
  }
  if (children_.size()) {
    *timing << " send_param: " << timer->MilliSeconds();
  }
#endif
}

template<typename Dtype>
void P2PSync<Dtype>::on_gradients_ready(Timer* timer, ostringstream* timing) {
#ifndef CPU_ONLY
#ifdef DEBUG
  int device;
  CUDA_CHECK(cudaGetDevice(&device));
  CHECK(device == solver_->param().device_id());
#endif

  // Sum children gradients as they appear in the queue
  for (int i = 0; i < children_.size(); ++i) {
    timer->Start();
    P2PSync<Dtype> *child = queue_.pop();
    Dtype* src = child->parent_grads_;
    Dtype* dst = diff_;

#ifdef DEBUG
    bool ok = false;
    for (int j = 0; j < children_.size(); ++j) {
      if (child == children_[j]) {
        ok = true;
      }
    }
    CHECK(ok);
    cudaPointerAttributes attributes;
    CUDA_CHECK(cudaPointerGetAttributes(&attributes, src));
    CHECK(attributes.device == device);
    CUDA_CHECK(cudaPointerGetAttributes(&attributes, dst));
    CHECK(attributes.device == device);
#endif

    caffe_gpu_add(size_, src, dst, dst);
    *timing << " add_grad: " << timer->MilliSeconds();
  }

  // Send gradients to parent
  if (parent_) {
    timer->Start();
    Dtype* src = diff_;
    Dtype* dst = parent_grads_;

#ifdef DEBUG
    cudaPointerAttributes attributes;
    CUDA_CHECK(cudaPointerGetAttributes(&attributes, src));
    CHECK(attributes.device == device);
    CUDA_CHECK(cudaPointerGetAttributes(&attributes, dst));
    CHECK(attributes.device == parent_->solver_->param().device_id());
#endif

    CUDA_CHECK(cudaMemcpyAsync(dst, src, size_ * sizeof(Dtype),  //
        cudaMemcpyDeviceToDevice, cudaStreamDefault));
    CUDA_CHECK(cudaStreamSynchronize(cudaStreamDefault));
    parent_->queue_.push(this);
    *timing << " send_grad: " << timer->MilliSeconds();
  } else {
    // Loss functions divide gradients by the batch size, so to compensate
    // for split batch, the root solver divides by number of solvers.
    caffe_gpu_scal(size_, Dtype(1.0 / Caffe::solver_count()), diff_);
  }
#endif
}

template<typename Dtype>
void P2PSync<Dtype>::run(shared_ptr<Solver<Dtype> > root,
                         const vector<int>& gpus) {
  // Pair devices for map-reduce synchronization
  vector<DevicePair> pairs;
  DevicePair::compute(gpus, &pairs);
  ostringstream s;
  for (int i = 1; i < pairs.size(); ++i) {
    s << (i == 1 ? "" : ", ") << pairs[i].parent() << ":" << pairs[i].device();
  }
  LOG(INFO)<< "GPUs pairs " << s.str();

  SolverParameter param(root->param());
  vector<shared_ptr<P2PSync<Dtype> > > syncs(gpus.size());
  syncs[0].reset(new P2PSync<Dtype>(root, NULL, param));

  // Build the GPU tree by finding the parent for each solver
  for (int attempts = 0; attempts < pairs.size(); ++attempts) {
    for (int i = 1; i < pairs.size(); ++i) {
      if (!syncs[i].get()) {
        P2PSync<Dtype>* parent = NULL;
        for (int j = 0; j < syncs.size(); ++j) {
          if (syncs[j]) {
            const SolverParameter& p = syncs[j]->solver()->param();
            if (p.device_id() == pairs[i].parent()) {
              parent = (P2PSync<Dtype>*) syncs[j].get();
            }
          }
        }
        if (parent) {
          param.set_device_id(pairs[i].device());
          syncs[i].reset(new P2PSync<Dtype>(root, parent, param));
          parent->children_.push_back((P2PSync<Dtype>*) syncs[i].get());
        }
      }
    }
  }

  LOG(INFO)<< "Starting Optimization";

  for (int i = 1; i < syncs.size(); ++i) {
    syncs[i]->StartInternalThread();
  }

  // Run root solver on current thread
  syncs[0]->solver_->Solve();

  for (int i = 1; i < syncs.size(); ++i) {
    syncs[i]->StopInternalThread();
  }
}

template<typename Dtype>
void P2PSync<Dtype>::divide_batch_size(NetParameter* net) {
  int solver_count = Caffe::solver_count();
  for (int i = 0; i < net->layer_size(); ++i) {
    string m = "Batch size must be divisible by the number of solvers (GPUs)";
    if (net->layer(i).has_data_param()) {
      if (net->layer(i).data_param().has_batch_size()) {
        uint32_t total = net->layer(i).data_param().batch_size();
        uint32_t batch = total / solver_count;
        CHECK(batch * solver_count == total) << m;
        net->mutable_layer(i)->mutable_data_param()->set_batch_size(batch);

        // Also adjust the prefetch count, as it is shared by all solvers
        uint32_t prefetch = net->layer(i).data_param().prefetch();
        net->mutable_layer(i)->mutable_data_param()->set_prefetch(
            prefetch * solver_count);
      }
    }
    if (net->layer(i).has_hdf5_data_param()) {
      if (net->layer(i).hdf5_data_param().has_batch_size()) {
        uint32_t total = net->layer(i).hdf5_data_param().batch_size();
        uint32_t batch = total / solver_count;
        CHECK(batch * solver_count == total) << m;
        net->mutable_layer(i)->mutable_hdf5_data_param()->set_batch_size(batch);
      }
    }
    if (net->layer(i).has_image_data_param()) {
      if (net->layer(i).image_data_param().has_batch_size()) {
        uint32_t total = net->layer(i).image_data_param().batch_size();
        uint32_t batch = total / solver_count;
        CHECK(batch * solver_count == total) << m;
        net->mutable_layer(i)->mutable_image_data_param()->set_batch_size(
            batch);
      }
    }
    if (net->layer(i).has_memory_data_param()) {
      if (net->layer(i).memory_data_param().has_batch_size()) {
        uint32_t total = net->layer(i).memory_data_param().batch_size();
        uint32_t batch = total / solver_count;
        CHECK(batch * solver_count == total) << m;
        net->mutable_layer(i)->mutable_memory_data_param()->set_batch_size(
            batch);
      }
    }
    if (net->layer(i).has_window_data_param()) {
      if (net->layer(i).window_data_param().has_batch_size()) {
        uint32_t total = net->layer(i).window_data_param().batch_size();
        uint32_t batch = total / solver_count;
        CHECK(batch * solver_count == total) << m;
        net->mutable_layer(i)->mutable_window_data_param()->set_batch_size(
            batch);
      }
    }
  }
}

INSTANTIATE_CLASS(Params);
INSTANTIATE_CLASS(GPUParams);
INSTANTIATE_CLASS(P2PSync);

}  // namespace caffe

