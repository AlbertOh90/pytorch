#pragma once

#include <c10/core/Device.h>
#include <c10/core/Event.h>
#include <c10/core/Stream.h>
#include <torch/csrc/autograd/profiler.h>
#include <torch/csrc/distributed/rpc/rpc_command_base.h>
#include <torch/csrc/jit/serialization/pickle.h>
#include <torch/csrc/utils/byte_order.h>

namespace tensorpipe {
class Message;
} // namespace tensorpipe

namespace torch {
namespace distributed {
namespace rpc {

// Parse error message and return RPCErrorType based on the message.
TORCH_API RPCErrorType getRPCErrorType(const JitFuture& jitFuture);
// Create an error string given the error description and error type
TORCH_API std::string makeRPCError(
    const std::string& rpcErrorStr,
    RPCErrorType errorType);

// Given an RPC message received as a request over the wire, deserialize it into
// the appropriate 'RpcCommandBase' type.
TORCH_API std::unique_ptr<RpcCommandBase> deserializeRequest(
    const Message& request);

// Given an RPC message received as a response over the wire, deserialize it
// into the appropriate 'RpcCommandBase' type, if the response is
// FORWARD_AUTOGRAD_RESP type, unwrap it, attach recvBackward() functions
// to received tensors and set the wrappedMsgType to its wrapped message type.
TORCH_API std::unique_ptr<RpcCommandBase> deserializeResponse(
    const Message& response,
    MessageType& wrappedMsgType);

// Given an RPC message received as a response over the wire, deserialize it
// into the valid IValue if the message is for a script rpc result,
// otherwise deserialize it into dummy none ivalue that will never be used.
// In this deserialization, we also attach recv rpc backward functions if
// needed.
IValue deserializeResptoIValueInternal(
    RpcCommandBase& rpc,
    MessageType messageType);
TORCH_API IValue deserializeRespToIValue(const Message& message);

// Note: format is subject to change and intended for RPCs.
// For saving persistently to disk, use torch::save().
TORCH_API std::string wireSerialize(
    const std::vector<char>& payload,
    const std::vector<at::Tensor>& tensors);

TORCH_API std::pair<std::vector<char>, std::vector<at::Tensor>> wireDeserialize(
    const void* data,
    size_t data_size);

// We use vector<char> as the type of blobs because it's what rpc::Message uses
// for its payload, even though it has the disadvantage that it cannot be
// allocated with uninitialized memory: it is always zeroed out.

// Some Tensors are effectively views of larger Tensors, where only a small
// subset of the Storage data is referenced. This normally is good and avoids
// copies when kept locally, but if we naively push the whole Storage over the
// wire, we'll end up with excess network traffic. This change clones tensors if
// we'd save at least half the data, and over a minimum hurdle.
TORCH_API c10::List<at::Tensor> cloneSparseTensors(
    const std::vector<at::Tensor>& tensors);

// Combines an original payload and wrapped payload into the original payload.
// Used to generate the overall payload for the wrapped RPC.
TORCH_API void writeWrappedPayload(
    std::vector<char>& originalPayload,
    std::vector<char>& additionalPayload);

// Reads the additional, wrapped payload from a wrapped RPC off of the input
// payload. After this, payload will contain the payload of the original,
// un-wrapped RPC.
TORCH_API std::vector<at::IValue> readWrappedPayload(
    std::vector<char>& payload,
    const rpc::Message& message);

// Takes a list of events from autograd profiler and populates them into
// profiledEvents to be carried over RPC.
TORCH_API void populateRemoteProfiledEvents(
    std::vector<torch::autograd::profiler::LegacyEvent>& profiledEvents,
    const torch::autograd::profiler::ProfilerConfig& profilerConfig,
    const std::vector<std::vector<torch::autograd::profiler::LegacyEvent>>&
        eventLists);

using stream_factory_t = std::function<
    c10::optional<c10::Stream>(c10::DeviceType, c10::DeviceIndex)>;

// A general device context class for both CPU and CUDA. If CUDA is not
// available, all CUDA-related methods will be no-ops.
struct TORCH_API LazyStreamContext {
  LazyStreamContext(const LazyStreamContext& other) = delete;
  LazyStreamContext(LazyStreamContext&& other) = delete;
  LazyStreamContext& operator=(const LazyStreamContext& rhs) = delete;
  LazyStreamContext& operator=(LazyStreamContext&& rhs) & = delete;

  LazyStreamContext(
      stream_factory_t streamCreator,
      stream_factory_t currentStreamProvider)
      : streamCreator_(std::move(streamCreator)),
        currentStreamProvider_(std::move(currentStreamProvider)){};
  ~LazyStreamContext() = default;

  void waitForCurrentStreams(const std::vector<torch::Tensor>& tensors) {
    for (const auto& tensor : tensors) {
      getStream(tensor.device().type(), tensor.device().index());
    }

    for (const auto& entry : streams_) {
      auto current_stream =
          currentStreamProvider_(entry.second.device_type(), entry.first);
      if (current_stream) {
        c10::Event event{entry.second.device_type()};
        event.record(
            *current_stream); // at::cuda::getCurrentCUDAStream(entry.first)
        event.block(entry.second);
      }
    }
  }

  // get all streams used in this context
  std::vector<c10::Stream> getReservedStreams() const {
    std::vector<c10::Stream> reservedStreams;
    reservedStreams.reserve(streams_.size());
    for (const auto& entry : streams_) {
      reservedStreams.push_back(entry.second);
    }
    return reservedStreams;
  }

  // get a stream for the given device. If it is the first time using that
  // device, allocate a new stream and store it in the map.
  c10::optional<c10::Stream> getStream(
      c10::DeviceType device_type,
      c10::DeviceIndex index) {
    auto iter = streams_.find(index);
    if (iter == streams_.end()) {
      auto stream = streamCreator_(
          device_type, index); // at::cuda::getStreamFromPool(/* isHighPriority
                               // */ false, /* device */ index);
      if (stream) {
        streams_.emplace(index, *stream);
      } else {
        throw std::runtime_error("Only CUDA streams are supported");
      }
      return stream;
    } else {
      return iter->second;
    }
  }

  std::set<c10::DeviceIndex> devices() const {
    std::set<c10::DeviceIndex> devices;
    for (const auto& entry : streams_) {
      devices.insert(entry.first);
    }
    return devices;
  }

 private:
  std::unordered_map<c10::DeviceIndex, c10::Stream> streams_;
  stream_factory_t streamCreator_;
  stream_factory_t currentStreamProvider_;
};

inline std::shared_ptr<LazyStreamContext> createLazyStreamContext(
    stream_factory_t streamCreator,
    stream_factory_t currentStreamProvider) {
  return std::make_shared<LazyStreamContext>(
      std::move(streamCreator), std::move(currentStreamProvider));
}

} // namespace rpc
} // namespace distributed
} // namespace torch
