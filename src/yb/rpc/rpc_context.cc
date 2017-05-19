// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "yb/rpc/rpc_context.h"

#include <ostream>
#include <sstream>

#include "yb/rpc/connection.h"
#include "yb/rpc/inbound_call.h"
#include "yb/rpc/outbound_call.h"
#include "yb/rpc/service_if.h"
#include "yb/util/hdr_histogram.h"
#include "yb/util/metrics.h"
#include "yb/util/trace.h"
#include "yb/util/debug/trace_event.h"
#include "yb/util/jsonwriter.h"
#include "yb/util/pb_util.h"

using google::protobuf::Message;

namespace yb {
namespace rpc {

using std::shared_ptr;

namespace {

// Wrapper for a protobuf message which lazily converts to JSON when
// the trace buffer is dumped. This pushes the work of stringification
// to the trace dumping process.
class PbTracer : public debug::ConvertableToTraceFormat {
 public:
  enum {
    kMaxFieldLengthToTrace = 100
  };

  explicit PbTracer(const Message& msg) : msg_(msg.New()) {
    msg_->CopyFrom(msg);
  }

  void AppendAsTraceFormat(std::string* out) const override {
    pb_util::TruncateFields(msg_.get(), kMaxFieldLengthToTrace);
    std::stringstream ss;
    JsonWriter jw(&ss, JsonWriter::COMPACT);
    jw.Protobuf(*msg_);
    out->append(ss.str());
  }
 private:
  const gscoped_ptr<Message> msg_;
};

scoped_refptr<debug::ConvertableToTraceFormat> TracePb(const Message& msg) {
  return make_scoped_refptr(new PbTracer(msg));
}
}  // anonymous namespace

RpcContext::RpcContext(InboundCallPtr call,
                       const google::protobuf::Message *request_pb,
                       const google::protobuf::Message *response_pb,
                       RpcMethodMetrics metrics)
  : call_(std::move(call)),
    request_pb_(request_pb),
    response_pb_(response_pb),
    metrics_(metrics) {
  VLOG(4) << call_->remote_method().service_name()
          << ": Received RPC request for " << call_->ToString()
          << std::endl
          << " request_pb_ : "
          << (request_pb_.get() != nullptr ? request_pb_->DebugString() : "nullptr")
          << std::endl
          << " response_pb_ : "
          << (response_pb_.get() != nullptr ? response_pb_->DebugString() : "nullptr")
          << std::endl;
  TRACE_EVENT_ASYNC_BEGIN2("rpc_call", "RPC", this,
                           "call", call_->ToString(),
                           "request", TracePb(*request_pb_));
}

static const shared_ptr<const Message> EMPTY_MESSAGE =
    shared_ptr<const Message>(new EmptyMessagePB());

RpcContext::RpcContext(InboundCallPtr call,
                       RpcMethodMetrics metrics)
  : call_(std::move(call)),
    request_pb_(EMPTY_MESSAGE),
    response_pb_(EMPTY_MESSAGE),
    metrics_(metrics) {
  VLOG(4) << call_->remote_method().service_name()
          << ": Received RPC request for " << call_->ToString()
          << std::endl
          << " request_pb_ : "
          << (request_pb_.get() != nullptr ? request_pb_->DebugString() : "nullptr")
          << std::endl
          << " response_pb_ : "
          << (response_pb_.get() != nullptr ? response_pb_->DebugString() : "nullptr")
          << std::endl;
  TRACE_EVENT_ASYNC_BEGIN2("rpc_call", "RPC", this,
                           "call", call_->ToString(),
                           "request", TracePb(*request_pb_));
}

RpcContext::~RpcContext() {
}

void RpcContext::RespondSuccess() {
  call_->RecordHandlingCompleted(metrics_.handler_latency);
  VLOG(4) << call_->remote_method().service_name() << ": Sending RPC success response for "
          << call_->ToString() << ":" << std::endl << response_pb_->DebugString();
  TRACE_EVENT_ASYNC_END2("rpc_call", "RPC", this,
                         "response", TracePb(*response_pb_),
                         "trace", trace()->DumpToString(true));
  call_->RespondSuccess(*response_pb_);
  delete this;
}

void RpcContext::RespondFailure(const Status &status) {
  call_->RecordHandlingCompleted(metrics_.handler_latency);
  VLOG(4) << call_->remote_method().service_name() << ": Sending RPC failure response for "
          << call_->ToString() << ": " << status.ToString();
  TRACE_EVENT_ASYNC_END2("rpc_call", "RPC", this,
                         "status", status.ToString(),
                         "trace", trace()->DumpToString(true));
  call_->RespondFailure(ErrorStatusPB::ERROR_APPLICATION,
                        status);
  delete this;
}

void RpcContext::RespondRpcFailure(ErrorStatusPB_RpcErrorCodePB err, const Status& status) {
  call_->RecordHandlingCompleted(metrics_.handler_latency);
  VLOG(4) << call_->remote_method().service_name() << ": Sending RPC failure response for "
          << call_->ToString() << ": " << status.ToString();
  TRACE_EVENT_ASYNC_END2("rpc_call", "RPC", this,
                         "status", status.ToString(),
                         "trace", trace()->DumpToString(true));
  call_->RespondFailure(err, status);
  delete this;
}

void RpcContext::RespondApplicationError(int error_ext_id, const std::string& message,
                                         const Message& app_error_pb) {
  call_->RecordHandlingCompleted(metrics_.handler_latency);
  if (VLOG_IS_ON(4)) {
    ErrorStatusPB err;
    InboundCall::ApplicationErrorToPB(error_ext_id, message, app_error_pb, &err);
    VLOG(4) << call_->remote_method().service_name() << ": Sending application error response for "
            << call_->ToString() << ":" << std::endl << err.DebugString();
    TRACE_EVENT_ASYNC_END2("rpc_call", "RPC", this,
                           "response", TracePb(app_error_pb),
                           "trace", trace()->DumpToString(true));
  }
  call_->RespondApplicationError(error_ext_id, message, app_error_pb);
  delete this;
}

Status RpcContext::AddRpcSidecar(util::RefCntBuffer car, int* idx) {
  return call_->AddRpcSidecar(car, idx);
}

const UserCredentials& RpcContext::user_credentials() const {
  return call_->user_credentials();
}

const Sockaddr& RpcContext::remote_address() const {
  return call_->remote_address();
}

std::string RpcContext::requestor_string() const {
  return call_->user_credentials().ToString() + " at " +
    call_->remote_address().ToString();
}

MonoTime RpcContext::GetClientDeadline() const {
  return call_->GetClientDeadline();
}

Trace* RpcContext::trace() {
  return call_->trace();
}

void RpcContext::Panic(const char* filepath, int line_number, const string& message) {
  // Use the LogMessage class directly so that the log messages appear to come from
  // the line of code which caused the panic, not this code.
#define MY_ERROR google::LogMessage(filepath, line_number, google::GLOG_ERROR).stream()
#define MY_FATAL google::LogMessageFatal(filepath, line_number).stream()

  MY_ERROR << "Panic handling " << call_->ToString() << ": " << message;
  MY_ERROR << "Request:\n" << request_pb_->DebugString();
  Trace* t = trace();
  if (t) {
    MY_ERROR << "RPC trace:";
    t->Dump(&MY_ERROR, true);
  }
  MY_FATAL << "Exiting due to panic.";

#undef MY_ERROR
#undef MY_FATAL
}

void RpcContext::CloseConnection() {
  shutdown(call_->connection()->socket()->GetFd(), SHUT_RD | SHUT_WR);
}

}  // namespace rpc
}  // namespace yb
