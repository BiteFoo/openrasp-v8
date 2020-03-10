#include <cpr/cpr.h>
#include <zlib.h>
#include <algorithm>
#include <string>
#include "bundle.h"

namespace openrasp_v8 {

struct AsyncReq {
  std::string method;
  cpr::Session session;
  cpr::Response response;
  Isolate* isolate;
  std::shared_ptr<IsolateData::AsyncJob> async_job;
  v8::Persistent<v8::Value> config;
  v8::Persistent<v8::Promise::Resolver> resolver;
};

void callback(void* data) {
  auto req = reinterpret_cast<AsyncReq*>(data);
  if (req->async_job->is_disposed) {
    delete req;
    return;
  }
  auto isolate = req->isolate;
  v8::HandleScope handle_scope(isolate);
  v8::TryCatch try_catch(isolate);
  auto context = isolate->GetCurrentContext();
  v8::Context::Scope context_scope(context);
  auto resolver = req->resolver.Get(isolate);

  auto ret_val = v8::Object::New(isolate);
  ret_val->Set(context, NewV8Key(isolate, "config"), req->config.Get(isolate)).IsJust();
  if (req->response.error.code == cpr::ErrorCode::OK) {
    auto headers = v8::Object::New(isolate);
    for (auto& h : req->response.header) {
      auto key = NewV8String(isolate, h.first.data(), h.first.size());
      auto val = NewV8String(isolate, h.second.data(), h.second.size());
      headers->Set(context, key, val).IsJust();
    }
    ret_val->Set(context, NewV8Key(isolate, "status"), v8::Int32::New(isolate, req->response.status_code)).IsJust();
    ret_val
        ->Set(context, NewV8Key(isolate, "data"),
              NewV8String(isolate, req->response.text.data(), req->response.text.size()))
        .IsJust();
    ret_val->Set(context, NewV8Key(isolate, "headers"), headers).IsJust();
    resolver->Resolve(context, ret_val).IsJust();
  } else {
    auto error = v8::Object::New(isolate);
    auto code = v8::Int32::New(isolate, static_cast<int32_t>(req->response.error.code));
    auto message = NewV8String(isolate, req->response.error.message.data(), req->response.error.message.size());
    error->Set(context, NewV8Key(isolate, "code"), code).IsJust();
    error->Set(context, NewV8Key(isolate, "message"), message).IsJust();
    ret_val->Set(context, NewV8Key(isolate, "error"), error).IsJust();
    resolver->Reject(context, ret_val).IsJust();
  }
  req->config.Reset();
  req->resolver.Reset();
  delete req;
}

void async_task(AsyncReq* req) {
  if (req->method == "get") {
    req->response = req->session.Get();
  } else if (req->method == "post") {
    req->response = req->session.Post();
  } else if (req->method == "put") {
    req->response = req->session.Put();
  } else if (req->method == "patch") {
    req->response = req->session.Patch();
  } else if (req->method == "head") {
    req->response = req->session.Head();
  } else if (req->method == "options") {
    req->response = req->session.Options();
  } else if (req->method == "delete") {
    req->response = req->session.Delete();
  } else {
    req->response = req->session.Get();
  }

  std::lock_guard<std::mutex> lock(req->async_job->mtx);
  if (req->async_job->is_disposed) {
    delete req;
    return;
  }
  req->async_job->count--;
  req->async_job->cv.notify_all();
  req->async_job->callbacks.emplace_back(callback, req);
}

void request_callback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto isolate = reinterpret_cast<Isolate*>(info.GetIsolate());
  v8::HandleScope handle_scope(isolate);
  v8::TryCatch try_catch(isolate);
  auto context = isolate->GetCurrentContext();
  v8::Context::Scope context_scope(context);
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    try_catch.ReThrow();
    return;
  }
  info.GetReturnValue().Set(resolver->GetPromise());
  v8::Local<v8::Object> config;
  if (!info[0]->ToObject(context).ToLocal(&config)) {
    resolver->Reject(context, try_catch.Exception()).IsJust();
    return;
  }

  auto undefined = v8::Undefined(isolate).As<v8::Value>();

  AsyncReq* req = new AsyncReq;
  req->isolate = isolate;
  req->async_job = isolate->GetData()->async_job;
  req->config.Reset(isolate, config);
  req->resolver.Reset(isolate, resolver);

  cpr::Header header;
  {
    v8::HandleScope handle_scope(isolate);
    auto tmp = config->Get(context, NewV8Key(isolate, "method")).FromMaybe(undefined);
    if (tmp->IsString()) {
      req->method = *v8::String::Utf8Value(isolate, tmp);
      std::transform(req->method.begin(), req->method.end(), req->method.begin(), ::tolower);
    }
  }
  {
    v8::HandleScope handle_scope(isolate);
    auto tmp = config->Get(context, NewV8Key(isolate, "url")).FromMaybe(undefined);
    if (tmp->IsString()) {
      req->session.SetUrl(*v8::String::Utf8Value(isolate, tmp));
    }
  }
  {
    v8::HandleScope handle_scope(isolate);
    auto tmp = config->Get(context, NewV8Key(isolate, "params")).FromMaybe(undefined);
    if (tmp->IsObject()) {
      auto object = tmp.As<v8::Object>();
      v8::Local<v8::Array> props;
      if (object->GetOwnPropertyNames(context).ToLocal(&props)) {
        cpr::Parameters parameters;
        for (int i = 0; i < props->Length(); i++) {
          v8::HandleScope handle_scope(isolate);
          v8::Local<v8::Value> key, val;
          if (props->Get(context, i).ToLocal(&key) && object->Get(context, key).ToLocal(&val)) {
            parameters.AddParameter({*v8::String::Utf8Value(isolate, key), *v8::String::Utf8Value(isolate, val)});
          }
        }
        req->session.SetParameters(parameters);
      }
    }
  }
  {
    cpr::Body body;
    v8::HandleScope handle_scope(isolate);
    auto tmp = config->Get(context, NewV8Key(isolate, "data")).FromMaybe(undefined);
    if (tmp->IsObject()) {
      v8::Local<v8::Value> json;
      if (!v8::JSON::Stringify(context, tmp).ToLocal(&json)) {
        resolver->Reject(context, try_catch.Exception()).IsJust();
        return;
      }
      body = cpr::Body(*v8::String::Utf8Value(isolate, json));
      header.emplace("content-type", "application/json");
    } else if (tmp->IsString()) {
      body = cpr::Body(*v8::String::Utf8Value(isolate, tmp));
    } else if (tmp->IsArrayBuffer()) {
      auto arraybuffer = tmp.As<v8::ArrayBuffer>();
      auto content = arraybuffer->GetContents();
      body = cpr::Body(static_cast<const char*>(content.Data()), content.ByteLength());
      header.emplace("content-type", "application/octet-stream");
    }
    if (body.size() != 0) {
      if (config->Get(context, NewV8Key(isolate, "deflate")).FromMaybe(undefined)->IsTrue()) {
        uLong dest_len = compressBound(body.size());
        char* dest = new char[dest_len];
        int rst = compress(reinterpret_cast<Bytef*>(dest), &dest_len, reinterpret_cast<const Bytef*>(body.data()),
                           body.size());
        if (rst != Z_OK) {
          delete[] dest;
          const char* msg;
          if (rst == Z_MEM_ERROR) {
            msg = "zlib error: there was not enough memory";
          } else if (rst == Z_BUF_ERROR) {
            msg = "zlib error: there was not enough room in the output buffer";
          } else {
            msg = "zlib error: unknown error";
          }
          resolver->Reject(context, v8::Exception::Error(NewV8String(isolate, msg))).IsJust();
          return;
        }
        body = cpr::Body(dest, dest_len);
        delete[] dest;
        header.emplace("content-encoding", "deflate");
      }
      req->session.SetBody(body);
    }
  }
  {
    v8::HandleScope handle_scope(isolate);
    auto tmp = config->Get(context, NewV8Key(isolate, "maxRedirects")).FromMaybe(undefined);
    if (tmp->IsInt32()) {
      req->session.SetMaxRedirects(cpr::MaxRedirects(tmp->Int32Value(context).FromMaybe(3)));
    } else {
      req->session.SetMaxRedirects(cpr::MaxRedirects(3));
    }
  }
  {
    v8::HandleScope handle_scope(isolate);
    auto tmp = config->Get(context, NewV8Key(isolate, "timeout")).FromMaybe(undefined);
    if (tmp->IsInt32()) {
      req->session.SetTimeout(tmp->Int32Value(context).FromMaybe(1000));
    } else {
      req->session.SetTimeout(1000);
    }
  }
  {
    v8::HandleScope handle_scope(isolate);
    auto tmp = config->Get(context, NewV8Key(isolate, "connectTimeout")).FromMaybe(undefined);
    if (tmp->IsInt32()) {
      req->session.SetConnectTimeout(tmp->Int32Value(context).FromMaybe(500));
    } else {
      req->session.SetConnectTimeout(500);
    }
  }
  {
    v8::HandleScope handle_scope(isolate);
    auto tmp = config->Get(context, NewV8Key(isolate, "headers")).FromMaybe(undefined);
    if (tmp->IsObject()) {
      auto object = tmp.As<v8::Object>();
      v8::Local<v8::Array> props;
      if (object->GetOwnPropertyNames(context).ToLocal(&props)) {
        for (int i = 0; i < props->Length(); i++) {
          v8::HandleScope handle_scope(isolate);
          v8::Local<v8::Value> key, val;
          if (props->Get(context, i).ToLocal(&key) && object->Get(context, key).ToLocal(&val)) {
            header.emplace(*v8::String::Utf8Value(isolate, key), *v8::String::Utf8Value(isolate, val));
          }
        }
      }
    }
  }
  req->session.SetHeader(header);
  req->session.SetVerifySsl(false);

  std::lock_guard<std::mutex> lock(req->async_job->mtx);
  if (req->async_job->is_disposed) {
    delete req;
    return;
  }
  req->async_job->count++;
  std::thread(async_task, req).detach();
}

}  // namespace openrasp_v8