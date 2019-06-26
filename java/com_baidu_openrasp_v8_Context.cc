/*
 * Copyright 2017-2019 Baidu Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "com_baidu_openrasp_v8_Context.h"
#include "header.h"

using namespace openrasp;

static std::vector<std::string> stringKeys;
static std::vector<std::string> objectKeys;
static std::vector<std::string> bufferKeys;

static inline void fill_keys(JNIEnv* env, jobjectArray jarr, std::vector<std::string>& keys) {
  jsize jarr_size = env->GetArrayLength(jarr);
  for (int i = 0; i < jarr_size; i++) {
    jstring jkey = reinterpret_cast<jstring>(env->GetObjectArrayElement(jarr, i));
    keys.emplace_back(Jstring2String(env, jkey));
  }
}

/*
 * Class:     com_baidu_openrasp_v8_Context
 * Method:    setStringKeys
 * Signature: ([Ljava/lang/String;)V
 */
JNIEXPORT void JNICALL Java_com_baidu_openrasp_v8_Context_setStringKeys(JNIEnv* env, jclass cls, jobjectArray jarr) {
  fill_keys(env, jarr, stringKeys);
}

/*
 * Class:     com_baidu_openrasp_v8_Context
 * Method:    setObjectKeys
 * Signature: ([Ljava/lang/String;)V
 */
JNIEXPORT void JNICALL Java_com_baidu_openrasp_v8_Context_setObjectKeys(JNIEnv* env, jclass cls, jobjectArray jarr) {
  fill_keys(env, jarr, objectKeys);
}

/*
 * Class:     com_baidu_openrasp_v8_Context
 * Method:    setBufferKeys
 * Signature: ([Ljava/lang/String;)V
 */
JNIEXPORT void JNICALL Java_com_baidu_openrasp_v8_Context_setBufferKeys(JNIEnv* env, jclass cls, jobjectArray jarr) {
  fill_keys(env, jarr, bufferKeys);
}

static void string_getter(v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  auto self = info.Holder();
  auto isolate = reinterpret_cast<openrasp::Isolate*>(info.GetIsolate());
  auto jenv = GetJNIEnv(isolate);
  auto jctx = reinterpret_cast<jobject>(self->GetInternalField(0).As<v8::External>()->Value());

  jstring jname = V8value2Jstring(jenv, name);
  jstring jstr = (jstring)jenv->CallObjectMethod(jctx, ctx_class.getString, jname);
  if (jstr == nullptr) {
    return;
  }
  auto maybe_string = Jstring2V8string(jenv, jstr);
  if (maybe_string.IsEmpty()) {
    return;
  }

  info.GetReturnValue().Set(maybe_string.ToLocalChecked());
}

static void object_getter(v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  auto self = info.Holder();
  auto isolate = reinterpret_cast<openrasp::Isolate*>(info.GetIsolate());
  auto jenv = GetJNIEnv(isolate);
  auto jctx = reinterpret_cast<jobject>(self->GetInternalField(0).As<v8::External>()->Value());

  jstring jname = V8value2Jstring(jenv, name);
  jbyteArray jbuf = reinterpret_cast<jbyteArray>(jenv->CallObjectMethod(jctx, ctx_class.getObject, jname));
  if (jbuf == nullptr) {
    return;
  }
  auto raw = jenv->GetPrimitiveArrayCritical(jbuf, nullptr);
  auto maybe_string = v8::String::NewFromOneByte(isolate, static_cast<uint8_t*>(raw), v8::NewStringType::kNormal);
  jenv->ReleasePrimitiveArrayCritical(jbuf, raw, JNI_ABORT);
  if (maybe_string.IsEmpty()) {
    return;
  }
  auto maybe_obj = v8::JSON::Parse(isolate->GetCurrentContext(), maybe_string.ToLocalChecked());
  if (maybe_obj.IsEmpty()) {
    return;
  }

  info.GetReturnValue().Set(maybe_obj.ToLocalChecked());
}

static void buffer_getter(v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  auto self = info.Holder();
  auto isolate = reinterpret_cast<openrasp::Isolate*>(info.GetIsolate());
  auto jenv = GetJNIEnv(isolate);
  auto jctx = reinterpret_cast<jobject>(self->GetInternalField(0).As<v8::External>()->Value());

  jstring jname = V8value2Jstring(jenv, name);
  jbyteArray jbuf = reinterpret_cast<jbyteArray>(jenv->CallObjectMethod(jctx, ctx_class.getBuffer, jname));
  if (jbuf == nullptr) {
    return;
  }
  jsize jbuf_size = jenv->GetArrayLength(jbuf);
  if (jbuf_size <= 0) {
    return;
  }
  auto raw = jenv->GetPrimitiveArrayCritical(jbuf, nullptr);
  auto buffer = v8::ArrayBuffer::New(isolate, raw, jbuf_size);
  jenv->ReleasePrimitiveArrayCritical(jbuf, raw, JNI_ABORT);

  info.GetReturnValue().Set(buffer);
}

v8::Local<v8::ObjectTemplate> CreateRequestContextTemplate(JNIEnv* env) {
  auto isolate = GetIsolate(env);
  auto obj_templ = v8::ObjectTemplate::New(isolate);
  for (auto& key : stringKeys) {
    obj_templ->SetLazyDataProperty(NewV8String(isolate, key), string_getter);
  }
  for (auto& key : objectKeys) {
    obj_templ->SetLazyDataProperty(NewV8String(isolate, key), object_getter);
  }
  for (auto& key : bufferKeys) {
    obj_templ->SetLazyDataProperty(NewV8String(isolate, key), buffer_getter);
  }
  obj_templ->SetInternalFieldCount(1);
  return obj_templ;
}