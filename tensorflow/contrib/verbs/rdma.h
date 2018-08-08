/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef TENSORFLOW_CONTRIB_VERBS_RDMA_H_
#define TENSORFLOW_CONTRIB_VERBS_RDMA_H_

#ifdef TENSORFLOW_USE_VERBS

#include <infiniband/verbs.h>
#include <cstring>  // for memset
#include <functional>
#include <memory>  // for shared_ptr
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "tensorflow/contrib/verbs/verbs_util.h"
#include "tensorflow/core/distributed_runtime/worker_env.h"
#include "tensorflow/core/framework/rendezvous.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/mutex.h"

namespace tensorflow {
#define PKEY_DEFAULT 0
#define QUEUE_DEPTH_DEFAULT 1024
#define TIMEOUT_DEFAULT 14
#define RETRY_CNT_DEFAULT 7
#define SL_DEFAULT 0
#define TRAFFIC_CLASS 0

#define RDMA_LOG_0 LOG(INFO)
#define RDMA_LOG_1 LOG(INFO)
#define RDMA_LOG_2 VLOG(2)
#define RDMA_LOG(LEVEL) RDMA_LOG_##LEVEL

struct RdmaParams {
  uint8_t port_num;
  uint8_t sgid_index;
  uint8_t pkey_index;
  uint32_t queue_depth;
  uint8_t timeout;
  uint8_t retry_cnt;
  uint8_t sl;
  enum ibv_mtu mtu;
  uint8_t traffic_class;
};
// structure to save the address of remote channels.
struct RdmaAddress {
  uint32_t lid;
  uint32_t qpn;
  uint32_t psn;
  uint64_t snp;
  uint64_t iid;
};
// structure to save information for remote memory regions.
struct RemoteMR {
  uint64_t remote_addr;
  uint32_t rkey;
};
enum BufferStatus { none, idle, busy };
enum Location { local, remote };

enum RdmaMessageType {
  RDMA_MESSAGE_META_DATA_UPDATE,
  RDMA_MESSAGE_TENSOR_RE_REQUEST,
  RDMA_MESSAGE_TENSOR_REQUEST,
  RDMA_MESSAGE_ERROR_STATUS,
};

struct RdmaMessage {
  RdmaMessageType type_;
  uint16_t name_size_;
  string name_;
  int64 step_id_;
  uint64_t request_index_;
  union {
    uint64_t remote_addr_;
#ifdef RDMA_DATA_VALIDATION
    uint64_t checksum_;
#endif
  };
  uint32_t rkey_;
  bool is_dead_;
  DataType data_type_;
  TensorShape tensor_shape_;
  size_t tensor_bytes_;

  // For error status:
  Status status_;

  // type|name_size|name|step_id|request_index|remote_addr/checksum|rkey|...
  //   1B|    2B   | 512|  8B   |     8B      |       8B           | 4B |...
  // ...|is_dead|data_type|tensor_shape|tensor_bytes|error_status          |
  // ...|    1B |   XB    |    XB      |    8B      |size - 4B, proto - XB |
  static const size_t kNameCapacity = 512;
  static const size_t kTypeStartIndex = 0;
  static const size_t kNameSizeStartIndex = kTypeStartIndex + sizeof(type_);
  static const size_t kNameStartIndex =
      kNameSizeStartIndex + sizeof(name_size_);
  static const size_t kStepIdStartIndex = kNameStartIndex + kNameCapacity;
  static const size_t kRequestIndexStartIndex =
      kStepIdStartIndex + sizeof(step_id_);
  static const size_t kRemoteAddrStartIndex =
      kRequestIndexStartIndex + sizeof(request_index_);
  static const size_t kChecksumStartIndex = kRemoteAddrStartIndex;
  static const size_t kRkeyStartIndex =
      kRemoteAddrStartIndex + sizeof(remote_addr_);
  static const size_t kIsDeadStartIndex = kRkeyStartIndex + sizeof(rkey_);
  static const size_t kDataTypeStartIndex =
      kIsDeadStartIndex + sizeof(is_dead_);
  static const size_t kTensorShapeStartIndex =
      kDataTypeStartIndex + sizeof(data_type_);
  static const size_t kTensorBytesStartIndex =
      kTensorShapeStartIndex + sizeof(TensorShape);
  static const size_t kErrorStatusStartIndex =
      kTensorBytesStartIndex + sizeof(tensor_bytes_);
  static const size_t kErrorStatusMaxSize = 4096;

  static const size_t kMessageTotalBytes = kErrorStatusStartIndex;
  static const size_t kRdmaMessageBufferSize =
      kMessageTotalBytes + kErrorStatusMaxSize;
  static string CreateMessage(const RdmaMessage& rm);
  static void ParseMessage(RdmaMessage& rm, const void* buffer);
};

// Immediate types for RDMA write
enum RdmaImmDataType {
  RDMA_IMM_MAX_REQUEST_ID = 0xFFFFFFFD,
  RDMA_IMM_DATA_ACK = 0xFFFFFFFE,
  RDMA_IMM_DATA_MESSAGE = 0xFFFFFFFF
};

// Write types for RDMA write-complete events
enum RdmaWriteIDType {
  RDMA_WRITE_ID_ACK,
  RDMA_WRITE_ID_MESSAGE,
  RDMA_WRITE_ID_TENSOR_WRITE
};

// Context for RDMA write-complete events
class RdmaWriteID {
 public:
  RdmaWriteID(RdmaWriteIDType write_type, void* write_context, int id)
      : write_type(write_type), write_context(write_context), id(id) {}

  RdmaWriteIDType write_type;
  void* write_context;
  int id;
};

// Tensor meta-data
class TensorMetaData {
 public:
  TensorShape tensor_shape_;
  DataType data_type_;
  size_t proto_size_;
  bool is_dead_;

  std::ostream& print(std::ostream& out) const {
    out << "Dtype = " << DataTypeString(data_type_)
        << ", Shape = " << tensor_shape_.DebugString() << ", Proto size = 0x"
        << std::hex << proto_size_ << ", Is dead = " << is_dead_;
    return out;
  }
};

inline std::ostream& operator<<(std::ostream& out,
                                const TensorMetaData& meta_data) {
  return meta_data.print(out);
}

class RdmaChannel;

void MRDeleter(ibv_mr* mr);
using MemoryRegionPtr = std::unique_ptr<ibv_mr, decltype(&MRDeleter)>;

// RdmaMemoryMgr
// Manages the local meta-data cache, and the registered RDMA memory regions.
class RdmaMemoryMgr {
 public:
  static RdmaMemoryMgr& Singleton() {
    static RdmaMemoryMgr instance;
    return instance;
  }

  // Memory regions
  ibv_mr* FindMemoryRegion(void* addr, size_t length);
  void InsertMemoryRegion(void* addr, size_t length,
                          const std::string& allocator_name);
  void EvictMemoryRegion(void* addr, size_t length);

  // Tensor meta-data cache
  const TensorMetaData* GetTensorMetaData(const std::string& tensor_name);
  const TensorMetaData* SetTensorMetaData(const std::string& tensor_name,
                                          DataType dtype,
                                          const TensorShape& shape,
                                          bool is_dead, size_t proto_size);

  struct ibv_pd* pd_;

 protected:
  RdmaMemoryMgr() : pd_(nullptr) {}

  static bool Comparator(const void* ptr, const MemoryRegionPtr& other) {
    return ptr < reinterpret_cast<char*>(other->addr) + other->length;
  }

 private:
  mutex tensor_meta_data_mu_;
  std::unordered_map<std::string, TensorMetaData> tensors_meta_data_;

  // Managed memory regions
  mutex mrs_mu_;
  std::vector<MemoryRegionPtr> mrs_ GUARDED_BY(mrs_mu_);
};

// RdmaTensorRequest
// Represents a single tensor request.
class RdmaTensorRequest {
 public:
  typedef Rendezvous::DoneCallback RecvDoneCallback;

  // Creates a tensor request identified by index.
  RdmaTensorRequest(uint32_t index, const string& key, int64 step_id,
                    RdmaChannel* channel, Device* dst_dev,
                    const Rendezvous::Args recv_args,
                    const RecvDoneCallback& done);
  ~RdmaTensorRequest();

  // Request unique index.
  uint32_t index() { return index_; }

  // Start the tensor request sequence.
  //
  // 1. Allocate the result tensor (and proxy tensor if required).
  // 2. Send RDMA_MESSAGE_TENSOR_REQUEST to the remote side.
  void Start();

  // Receive tensor meta-data.
  //
  // 1. Update the local meta-data cache.
  // 2. Reallocate the result tensor (and proxy tensor if required).
  // 3. Re-send the request to the remote side.
  void RecvTensorMetaData(DataType dtype, TensorShape shape, bool is_dead,
                          size_t proto_size);

  // Receive tensor content (RDMA write was completed).
  //
  // Decode proto if required and/or move to GPU if the content was not
  // written to it directly (GPU direct is not available). Afterwards,
  // invoke Done().
  void RecvTensorContent();

  // Receive error status (in case of a remote error).
  // Invoke Done() with the status code.
  void RecvErrorStatus(const Status& status);

#ifdef RDMA_DATA_VALIDATION
  // Receive tensor checksum
  //
  // For validation: Get and store the Tensor's expected checksum for the
  // current request. Compare the result Tensor's checksum with the stored
  // checksum right before invoking Done().
  void RecvTensorChecksum(uint64_t checksum) { checksum_ = checksum; }
#endif

 private:
  void Done(const Status& s);
  void Send(RdmaMessageType message_type);
  bool AllocateTensors();
  void AllocateTensorsAsync(StatusCallback done);
  void DeallocateTensors();

  uint32_t index_;
  string key_;
  int64 step_id_;
  RdmaChannel* channel_;
  Device* dst_dev_;
  Rendezvous::Args recv_args_;
  const TensorMetaData* meta_data_;
  Tensor* result_tensor_;
  Tensor* proxy_tensor_;
  void* rdma_addr_;
  ibv_mr* mr_;
  RecvDoneCallback done_;
#ifdef RDMA_DATA_VALIDATION
  uint64_t checksum_;
#endif
};

// RdmaTensorResponse
// Represents a single tensor response.
class RdmaTensorResponse {
 public:
  // Creates a response for request message.
  RdmaTensorResponse(RdmaChannel* channel, const RdmaMessage& rm)
      : channel_(channel), rm_(rm) {}

  void Update(const RdmaMessage& rm) { rm_ = rm; }

  // Start the tensor response sequence.
  //
  // 1. Find the tensor in the local tag-match table and invoke RecvHandler.
  //    (Using RecvLocalAsync()).
  // 2. Compare the tensor's meta-data to the meta-data in the message (taken
  //    from the requester's local cache).
  //    If meta-data changed:
  //    a. Clone the tensor to be sent later.
  //    b. Send a meta-data update message and wait for re-request.
  //    Else:
  //    a. Send the tensor's content (using direct RDMA write).
  void Start();

  // Resume the response sequence, after a re-request.
  //
  // 1. Send the tensor's content that was cloned earlier.
  void Resume();

  // Destroy the response's resources and remove it from the pending list.
  void Destroy();

 private:
  void RecvHandler(Rendezvous::ParsedKey parsed,
                   const Rendezvous::Args& send_args,
                   const Rendezvous::Args& recv_args, const Tensor& in,
                   bool is_dead);
  void Clone(const Tensor& in, const TensorProto& proto, bool is_dead);
  void Send(const Tensor& in, const TensorProto& proto, bool is_dead,
            const Status& status);
  bool TensorMetaDataChanged(const Tensor& in, bool is_dead);
  Status PrepareRecvTensor(const Rendezvous::ParsedKey& parsed,
                           Device** src_dev);
  void SendMetaData(const Tensor& in, const TensorProto& proto, bool is_dead);
  void SendContent(const Tensor& in, const TensorProto& proto, bool is_dead);
  void SendErrorStatus(const Status& status);

  RdmaChannel* channel_;
  RdmaMessage rm_;  // The request message
  Device* src_dev_ = nullptr;
  TensorBuffer* src_buffer_ = nullptr;
  void* src_addr_ = nullptr;
  ibv_mr* mr_ = nullptr;
  uint64_t checksum_ = 0;
  bool meta_data_changed_ = false;

  // Re-item:
  TensorProto* proto_ = nullptr;
  Tensor* tensor_ = nullptr;
  bool is_dead_ = false;
};

class RdmaMessageBuffers;
// Class that represents the Rdma Adapter.
// Responsible for creation of the completion queue, and handling
// of work completions.
class RdmaAdapter {
  friend class RdmaChannel;
  friend class RdmaMessageBuffers;
  friend class RdmaTensorResponse;
  friend class RdmaMgr;
  friend class RdmaRemoteRendezvous;

 public:
  RdmaAdapter(const WorkerEnv* worker_env);
  ~RdmaAdapter();
  // Adapter name, e.g. mlx5_0.
  string name() const;
  void StartPolling();
  void Process_CQ();

 protected:
  ibv_context* context_;
  // RDMA configuration parameters
  RdmaParams params_;
  // ibverbs protection domain
  ibv_pd* pd_;
  // Completion event channel, to wait for work completions
  ibv_comp_channel* event_channel_;
  // Completion queue, to poll on work completions
  ibv_cq* cq_;
  // Pre-allocated work completions array used for polling
  ibv_wc* wc_;
  // worker env for thread
  const WorkerEnv* worker_env_;
  // thread for cq.
  std::unique_ptr<Thread> polling_thread_;
};

// Class that represents a connection to a remote Rdma peer.
// Responsible for connecting queue pairs.
class RdmaChannel {
  friend class RdmaAdapter;
  friend class RdmaMessageBuffers;
  friend class RdmaTensorBuffer;
  friend class RdmaTensorRequest;
  friend class RdmaTensorResponse;
  friend class RdmaMgr;
  friend class RdmaRemoteRendezvous;

 public:
  explicit RdmaChannel(const RdmaAdapter* adapter, const string local_name,
                       const string remote_name_);
  ~RdmaChannel();
  inline const RdmaAddress& self() { return self_; }
  RdmaAddress address() const;
  void Connect(const RdmaAddress& remoteAddr);
  void Connect();
  void Recv();
  void SetRemoteAddress(const RdmaAddress& ra, bool override);

  // Requests:
  RdmaTensorRequest* InsertTensorRequest(
      const string& key, int64 step_id, Device* dst_dev,
      const Rendezvous::Args recv_args,
      const RdmaTensorRequest::RecvDoneCallback& done);
  void RemoveTensorRequest(uint32_t request_index);
  RdmaTensorRequest* GetTensorRequest(uint32_t request_index);

  // Responses:
  RdmaTensorResponse* AddTensorResponse(const RdmaMessage& rm);
  RdmaTensorResponse* UpdateTensorResponse(const RdmaMessage& rm);
  void RemoveTensorResponse(uint32_t request_index);

  static const int kPingRecvWrid = 0;

 private:
  static const int kPingBuffSize = 1024;
  char ping_buff_[kPingBuffSize];
  struct ibv_mr* mr_;
  struct ibv_sge ping_sge_list_;
  int PingPostRecv();
  int PingPostSend();

 protected:
  const RdmaAdapter* adapter_;
  RdmaAddress self_;
  string local_name_;
  string remote_name_;
  ibv_qp* qp_;
  mutex mu_;
  bool connected_ GUARDED_BY(mu_) = false;
  RdmaAddress remote_ GUARDED_BY(mu_);
  bool remote_set_ GUARDED_BY(mu_) = false;
  mutex ct_mu_;
  typedef std::unordered_map<uint32_t, RdmaTensorRequest> RequestTable;
  RequestTable request_table_ GUARDED_BY(ct_mu_);
  uint32_t request_serial_ GUARDED_BY(ct_mu_);
  mutex responses_mu_;
  typedef std::unordered_map<uint32_t, RdmaTensorResponse> ResponsesTable;
  typedef std::unordered_map<uint32_t, uint32_t> ResponsesCheck;
  ResponsesTable responses_table_ GUARDED_BY(responses_mu_);
  ResponsesCheck responses_check_ GUARDED_BY(responses_mu_);
  RdmaMessageBuffers* message_buffers_;
};

class RdmaMR {
  public:
    RdmaMR(void* buffer, ibv_mr* mr, int id)
      : buffer_(buffer), mr_(mr), id_(id) {}
    RdmaMR(const RdmaMR& other)
      : buffer_(other.buffer_), mr_(other.mr_), id_(other.id_) {}

    void* buffer_;
    ibv_mr* mr_;
    int id_;
};

// Class that represents a buffer for Rdma message sending.
class RdmaMessageBuffers {
  friend class RdmaChannel;
  friend class RdmaAdapter;
  friend class RdmaMgr;
  friend class RdmaRemoteRendezvous;

 public:
  explicit RdmaMessageBuffers(RdmaChannel* channel);
  ~RdmaMessageBuffers();

  void EnqueueItem(string Item);
  void SendNextItem();
  static void Write(void * thiz, RdmaChannel* channel, uint32_t imm_data,
                    size_t buffer_size, uint64_t src_addr, uint32_t lkey,
                    uint64_t remote_addr, uint32_t rkey,
                    RdmaWriteIDType write_type, void* write_context);
  RdmaMR AcquireRecvBuffer();
  void ReleaseRecvBuffer(RdmaMR rmr);
  void ReleaseSendBuffer(RdmaMR rmr);

 protected:
  RdmaChannel* channel_;
  std::vector<RdmaMR> mr_send_;
  std::vector<RdmaMR> mr_recv_;
  mutex mu_;
  size_t qid_ GUARDED_BY(mu_);
  std::queue<std::pair<size_t,string> > queue_ GUARDED_BY(mu_);
  std::queue<RdmaMR> free_send_ GUARDED_BY(mu_);
  std::queue<RdmaMR> free_recv_ GUARDED_BY(mu_);
};

}  // namespace tensorflow

#endif  // TENSORFLOW_USE_VERBS
#endif  // TENSORFLOW_CONTRIB_VERBS_RDMA_H_
