#include <napi.h>
#include <mutex>
#include <map>
#include <string>
#include <vector>
#include <algorithm>  // Для std::find
#include <windows.h>  // For rpc.h, ole2.h if not pulled by opcda.h
#include <objbase.h>  // COM init
#include "OPCClientToolKit.h"  // Assume: COPCClient, COPCGroup, OnDisconnectCb, etc.

using Napi::CallbackInfo;
using Napi::Env;
using Napi::Object;
using Napi::String;
using Napi::Number;
using Napi::Array;
using Napi::Value;
using Napi::FunctionReference;
using Napi::Promise;
using Napi::AsyncWorker;
using Napi::ObjectWrap;

// Forward declare converter and emit
Napi::Value VariantToNapi(const Napi::Env& env, VARIANT* var);
void EmitEvent(napi_threadsafe_function tsfn, const std::string& type, const Napi::Object& dataObj, Env env);

// Private data for OPCDA instance
class OPCDA : public ObjectWrap<OPCDA> {
  static Napi::FunctionReference constructor;
  Napi::Env env_;

  COPCClient* opcClient = nullptr;
  std::map<std::string, COPCGroup*> groups;
  std::map<std::string, napi_threadsafe_function> tsfns;  // tsfn per key ('connection' or group)
  std::map<std::string, Napi::FunctionReference> jsCbs;   // JS refs for cleanup
  std::map<std::string, std::vector<std::string>> subscriptions;  // key -> eventTypes

  mutable std::mutex mtx_;  // Mutex for shared access (only!)
  bool hasConnectionTsfn = false;  // Flag for auto-created connection tsfn

  // OPC Callbacks (static, assume closure via user_data or global map<OPCHANDLE, OPCDA*>)
  static void OPCDataChangeCb(OPCHANDLE hGroup, LPWSTR itemName, VARIANT* value, HRESULT hResult) {
    // OPCDA* thiz = GetInstanceFromHandle(hGroup);  // Implement retrieval (e.g., static std::map<OPCHANDLE, OPCDA*> instances;)
    OPCDA* thiz = nullptr;  // Placeholder: implement based on OPC Toolkit
    if (!thiz) return;

    std::string itemStr = /* std::string(CW2A(itemName)) or similar */;
    Napi::Object dataObj = Napi::Object::New(thiz->env_);
    dataObj.Set("item", Napi::String::New(thiz->env_, itemStr));
    dataObj.Set("value", VariantToNapi(thiz->env_, value));
    dataObj.Set("quality", Napi::Number::New(thiz->env_, /* extract quality */ hResult & 0xC0000000));  // OPC quality mask
    dataObj.Set("timestamp", Napi::Date::New(thiz->env_, /* FILETIME to JS ms */ 0.0));

    std::string eventType = "dataChange";
    Napi::Object eventData = Napi::Object::New(thiz->env_);
    eventData.Set("data", dataObj);

    // Detect disconnect (e.g., bad quality or specific HRESULT)
    if (FAILED(hResult) || /* quality == OPC_QUALITY_BAD_NOT_CONNECTED */ false) {
      eventType = "disconnect";
      eventData.Set("error", Napi::String::New(thiz->env_, "Connection lost via data change"));
    }

    std::lock_guard<std::mutex> lock(thiz->mtx_);
    // Emit to group tsfn (key = groupName from hGroup)
    std::string groupKey = /* extract from hGroup */;
    auto tsIt = thiz->tsfns.find(groupKey);
    if (tsIt != thiz->tsfns.end()) {
      EmitEvent(tsIt->second, eventType, eventData, thiz->env_);
    }
  }

  static void OPCDisconnectCb(HRESULT hResult) {
    // OPCDA* thiz = /* get instance */;
    OPCDA* thiz = nullptr;  // Placeholder
    if (!thiz) return;

    Napi::Object eventData = Napi::Object::New(thiz->env_);
    eventData.Set("error", Napi::String::New(thiz->env_, /* HRESULT to string */ "Server disconnected"));
    std::lock_guard<std::mutex> lock(thiz->mtx_);
    auto tsIt = thiz->tsfns.find("connection");
    if (tsIt != thiz->tsfns.end()) {
      EmitEvent(tsIt->second, "disconnect", eventData, thiz->env_);
    }
  }

public:
  static Object Init(Env env, Object exports) {
    Function func = DefineClass(env, "OPCDA", {
      InstanceMethod<&OPCDA::Connect>("connect"),
      InstanceMethod<&OPCDA::Disconnect>("disconnect"),
      InstanceMethod<&OPCDA::UnsubscribeConnection>("unsubscribeConnection"),
      InstanceMethod<&OPCDA::CreateGroup>("createGroup"),
      InstanceMethod<&OPCDA::AddItem>("addItem"),
      InstanceMethod<&OPCDA::Subscribe>("subscribe"),
      InstanceMethod<&OPCDA::Unsubscribe>("unsubscribe"),
      InstanceMethod<&OPCDA::Read>("read"),
      InstanceMethod<&OPCDA::Write>("write"),
      InstanceMethod<&OPCDA::Browse>("browse"),
    });

    constructor = Napi::Persistent(func);
    constructor.SuppressDestruct();

    exports.Set("OPCDA", func);
    return exports;
  }

  OPCDA(const CallbackInfo& info) : ObjectWrap<OPCDA>(info), env_(info.Env()) {
    opcClient = new COPCClient();
    opcClient->init(MULTITHREADED);
    opcClient->SetDisconnectCallback(OPCDisconnectCb);  // Set once

    // Check for init callback (first arg)
    if (info.Length() > 0 && info[0].IsFunction()) {
      Function cb = info[0].As<Function>();
      SubscribeConnectionCb(cb);  // Internal setup tsfn for connection events
    }
  }

  ~OPCDA() {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& pair : tsfns) {
      napi_release_threadsafe_function(pair.second, napi_threadsafe_function_release_mode::NAPI_TSFN_RELEASE_MODE_MANUAL);
    }
    tsfns.clear();
    jsCbs.clear();
    subscriptions.clear();
    if (opcClient) {
      opcClient->stop();
      delete opcClient;
    }
  }

  // Internal: Setup tsfn for connection events from init callback
  void SubscribeConnectionCb(const Napi::Function& cb) {
    std::lock_guard<std::mutex> lock(mtx_);
    std::string key = "connection";
    Napi::FunctionReference cbRef = Napi::Persistent(cb);
    cbRef.SuppressDestruct();

    napi_threadsafe_function tsfn;
    Napi::FunctionReference* cbRefPtr = new Napi::FunctionReference(cbRef);
    auto finalize_cb = [](napi_env env, void* data) {
      auto* ref = static_cast<Napi::FunctionReference*>(data);
      if (ref) {
        ref->Unref();
        delete ref;
      }
    };
    napi_status status = napi_create_threadsafe_function(
      env_, cb.As<Value>(), nullptr, Napi::String::New(env_, "OPCConnectionEvent"),
      0, 10, finalize_cb, cbRefPtr, nullptr, nullptr, &tsfn
    );
    if (status != napi_ok) {
      cbRef.Unref();
      return;  // Silent fail or throw
    }

    tsfns[key] = tsfn;
    jsCbs[key] = cbRef;
    subscriptions[key] = {"connect", "disconnect", "error"};
    hasConnectionTsfn = true;

    // Emit initial event
    Napi::Object initData = Napi::Object::New(env_);
    initData.Set("connected", Napi::Boolean::New(env_, false));
    EmitEvent(tsfn, "init", initData, env_);
  }

  // Connect (tsfn-based, requires prior subscription or init cb)
  Value Connect(const CallbackInfo& info) {
    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
      throw Napi::TypeError::New(env_, "host, progId expected (callback via init or subscribe)");
    }
    std::string host = info[0].As<String>().Utf8Value();
    std::string progId = info[1].As<String>().Utf8Value();

    auto* worker = new ConnectWorker(info.This().As<Object>(), env_, host, progId, this);
    worker->Queue();
    return env_.Undefined();
  }

  // UnsubscribeConnection (for auto-created)
  Value UnsubscribeConnection(const CallbackInfo& info) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (hasConnectionTsfn) {
      auto tsIt = tsfns.find("connection");
      if (tsIt != tsfns.end()) {
        napi_release_threadsafe_function(tsIt->second, napi_threadsafe_function_release_mode::NAPI_TSFN_RELEASE_MODE_IMMEDIATE);
        tsfns.erase(tsIt);
      }
      auto jsIt = jsCbs.find("connection");
      if (jsIt != jsCbs.end()) {
        jsIt->second.Unref();
        jsCbs.erase(jsIt);
      }
      subscriptions.erase("connection");
      hasConnectionTsfn = false;
    }
    return env_.Undefined();
  }

  Value Disconnect(const CallbackInfo& info) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (opcClient) opcClient->Disconnect();
    // Emit to connection tsfn if exists
    Napi::Object dataObj = Napi::Object::New(env_);
    auto tsIt = tsfns.find("connection");
    if (tsIt != tsfns.end()) {
      EmitEvent(tsIt->second, "disconnect", dataObj, env_);
    }
    return env_.Undefined();
  }

  Value CreateGroup(const CallbackInfo& info) {
    if (info.Length() < 3) throw Napi::TypeError::New(env_, "groupName, rate, deadband expected");
    std::string groupName = info[0].As<String>().Utf8Value();
    int rate = info[1].As<Number>().Int32Value();
    double deadband = info[2].As<Number>().DoubleValue();

    std::lock_guard<std::mutex> lock(mtx_);
    bool success = opcClient->CreateGroup(groupName.c_str(), rate, deadband);
    if (!success) throw Napi::Error::New(env_, "Failed to create group");
    groups[groupName] = opcClient->GetGroup(groupName.c_str());
    return env_.Undefined();
  }

  Value AddItem(const CallbackInfo& info) {
    if (info.Length() < 2) throw Napi::TypeError::New(env_, "groupName, itemName expected");
    std::string groupName = info[0].As<String>().Utf8Value();
    std::string itemName = info[1].As<String>().Utf8Value();

    std::lock_guard<std::mutex> lock(mtx_);
    auto it = groups.find(groupName);
    if (it == groups.end()) throw Napi::Error::New(env_, "Group not found");
    bool success = it->second->AddItem(itemName.c_str());
    if (!success) throw Napi::Error::New(env_, "Failed to add item");
    return env_.Undefined();
  }

  Value Subscribe(const CallbackInfo& info) {
    if (info.Length() < 2) throw Napi::TypeError::New(env_, "groupName or 'connection', callback [, eventTypes] expected");
    std::string target = info[0].As<String>().Utf8Value();  // 'connection' for global, or groupName
    Function cb = info[1].As<Function>();
    Napi::FunctionReference cbRef = Napi::Persistent(cb);
    cbRef.SuppressDestruct();

    std::vector<std::string> eventTypes = {"dataChange"};  // Default
    if (info.Length() > 2 && info[2].IsArray()) {
      Array arr = info[2].As<Array>();
      for (uint32_t i = 0; i < arr.Length(); ++i) {
        eventTypes.push_back(arr.Get(i).As<String>().Utf8Value());
      }
    }

    // Create tsfn
    napi_threadsafe_function tsfn;
    Napi::FunctionReference* cbRefPtr = new Napi::FunctionReference(cbRef);
    auto finalize_cb = [](napi_env env, void* data) {
      auto* ref = static_cast<Napi::FunctionReference*>(data);
      if (ref) {
        ref->Unref();
        delete ref;
      }
    };
    napi_status status = napi_create_threadsafe_function(
      env_, cb.As<Value>(), nullptr, Napi::String::New(env_, "OPCEvent"),
      0, 10, finalize_cb, cbRefPtr, nullptr, nullptr, &tsfn  // Queue 10 for bursts
    );
    if (status != napi_ok) {
      cbRef.Unref();
      throw Napi::Error::New(env_, "Failed to create tsfn");
    }

    std::lock_guard<std::mutex> lock(mtx_);
    std::string key = (target == "connection" ? "connection" : target);
    if (key == "connection" && hasConnectionTsfn) {
      // Already subscribed, skip
      cbRef.Unref();
      napi_release_threadsafe_function(tsfn, napi_threadsafe_function_release_mode::NAPI_TSFN_RELEASE_MODE_IMMEDIATE);
      return env_.Undefined();
    }
    tsfns[key] = tsfn;
    jsCbs[key] = cbRef;
    subscriptions[key] = eventTypes;

    // Set OPC cbs if dataChange
    if (std::find(eventTypes.begin(), eventTypes.end(), "dataChange") != eventTypes.end() && target != "connection") {
      auto it = groups.find(target);
      if (it != groups.end()) {
        it->second->SetDataChangeCallback(OPCDataChangeCb);  // With closure to this/tsfn
      }
    }
    // For connect: Emit initial if subscribed
    if (std::find(eventTypes.begin(), eventTypes.end(), "connect") != eventTypes.end() && opcClient && opcClient->IsConnected()) {
      Napi::Object dataObj = Napi::Object::New(env_);
      dataObj.Set("type", Napi::String::New(env_, "connect"));
      dataObj.Set("success", Napi::Boolean::New(env_, true));
      EmitEvent(tsfn, "connect", dataObj, env_);
    }

    return env_.Undefined();
  }

  Value Unsubscribe(const CallbackInfo& info) {
    if (info.Length() < 1) throw Napi::TypeError::New(env_, "groupName or 'connection' [, eventTypes] expected");
    std::string target = info[0].As<String>().Utf8Value();
    std::vector<std::string> eventTypes;  // If empty, unsubscribe all
    if (info.Length() > 1 && info[1].IsArray()) {
      Array arr = info[1].As<Array>();
      for (uint32_t i = 0; i < arr.Length(); ++i) {
        eventTypes.push_back(arr.Get(i).As<String>().Utf8Value());
      }
    }

    std::lock_guard<std::mutex> lock(mtx_);
    std::string key = (target == "connection" ? "connection" : target);
    auto tsIt = tsfns.find(key);
    if (tsIt != tsfns.end()) {
      if (!eventTypes.empty()) {
        // Partial unsubscribe: remove specific types from subscriptions[key], but keep tsfn
        auto& types = subscriptions[key];
        types.erase(std::remove_if(types.begin(), types.end(), [&](const std::string& t) {
          return std::find(eventTypes.begin(), eventTypes.end(), t) != eventTypes.end();
        }), types.end());
        if (types.empty()) {
          // All removed, release tsfn
          napi_release_threadsafe_function(tsIt->second, napi_threadsafe_function_release_mode::NAPI_TSFN_RELEASE_MODE_IMMEDIATE);
          tsfns.erase(tsIt);
          auto jsIt = jsCbs.find(key);
          if (jsIt != jsCbs.end()) {
            jsIt->second.Unref();
            jsCbs.erase(jsIt);
          }
          subscriptions.erase(key);
        }
      } else {
        // Full unsubscribe
        napi_release_threadsafe_function(tsIt->second, napi_threadsafe_function_release_mode::NAPI_TSFN_RELEASE_MODE_IMMEDIATE);
        tsfns.erase(tsIt);
        auto jsIt = jsCbs.find(key);
        if (jsIt != jsCbs.end()) {
          jsIt->second.Unref();
          jsCbs.erase(jsIt);
        }
        subscriptions.erase(key);
      }
    }
    return env_.Undefined();
  }

  Value Read(const CallbackInfo& info) {
    if (info.Length() < 1 || !info[0].IsString()) throw Napi::TypeError::New(env_, "itemName expected");
    std::string itemName = info[0].As<String>().Utf8Value();
    auto* worker = new ReadWorker(info.This().As<Object>(), env_, itemName, this);
    worker->Queue();
    return worker->Promise();
  }

  Value Write(const CallbackInfo& info) {
    if (info.Length() < 2) throw Napi::TypeError::New(env_, "itemName, value expected");
    std::string itemName = info[0].As<String>().Utf8Value();
    Value jsValue = info[1];
    auto* worker = new WriteWorker(info.This().As<Object>(), env_, itemName, jsValue, this);
    worker->Queue();
    return worker->Promise();
  }

  Value Browse(const CallbackInfo& info) {
    std::string startingItem = info.Length() > 0 ? info[0].As<String>().Utf8Value() : "";
    auto* worker = new BrowseWorker(info.This().As<Object>(), env_, startingItem, this);
    worker->Queue();
    return worker->Promise();
  }

private:
  // ConnectWorker (emits to connection tsfn)
  class ConnectWorker : public AsyncWorker {
    std::string host_, progId_;
    OPCDA* op_;
    bool success_;
  public:
    ConnectWorker(Object recv, Env env, std::string h, std::string p, OPCDA* op)
        : AsyncWorker(recv, env, "ConnectWorker"), host_(h), progId_(p), op_(op), success_(false) {}
    void Execute() override {
      std::lock_guard<std::mutex> lock(op_->mtx_);
      success_ = op_->opcClient->Connect(host_.c_str(), progId_.c_str());
    }
    void OnOK() override {
      Napi::Object eventData = Napi::Object::New(Env());
      eventData.Set("success", Napi::Boolean::New(Env(), success_));
      if (!success_) eventData.Set("error", Napi::String::New(Env(), "Connection failed"));
      std::lock_guard<std::mutex> lock(op_->mtx_);
      auto tsIt = op_->tsfns.find("connection");
      if (tsIt != op_->tsfns.end()) {
        EmitEvent(tsIt->second, "connect", eventData, Env());
      }
    }
  };

  // ReadWorker (placeholder implementation)
  class ReadWorker : public AsyncWorker {
    std::string itemName_;
    VARIANT value_;
    OPCDA* op_;
  public:
    ReadWorker(Object recv, Env env, std::string name, OPCDA* op) : AsyncWorker(recv, env, "ReadWorker"), itemName_(name), op_(op) {}
    void Execute() override {
      std::lock_guard<std::mutex> lock(op_->mtx_);
      // op_->opcClient->Read(itemName_.c_str(), &value_);  // OPC call
    }
    void OnOK() override {
      Deferred().Resolve(Env(), VariantToNapi(Env(), &value_));
    }
  };

  // WriteWorker (placeholder)
  class WriteWorker : public AsyncWorker {
    std::string itemName_;
    Value jsValue_;
    OPCDA* op_;
    bool success_;
  public:
    WriteWorker(Object recv, Env env, std::string name, Value val, OPCDA* op) : AsyncWorker(recv, env, "WriteWorker"), itemName_(name), jsValue_(val), op_(op), success_(false) {}
    void Execute() override {
      std::lock_guard<std::mutex> lock(op_->mtx_);
      // VARIANT var = JsValueToVariant(jsValue_);  // Converter
      // success_ = op_->opcClient->Write(itemName_.c_str(), &var);
    }
    void OnOK() override {
      Deferred().Resolve(Env(), success_ ? Env().Undefined() : Napi::Error::New(Env(), "Write failed").Value());
    }
  };

  // BrowseWorker (placeholder)
  class BrowseWorker : public AsyncWorker {
    std::string starting_;
    std::vector<std::string> items_;
    OPCDA* op_;
  public:
    BrowseWorker(Object recv, Env env, std::string s, OPCDA* op) : AsyncWorker(recv, env, "BrowseWorker"), starting_(s), op_(op) {}
    void Execute() override {
      std::lock_guard<std::mutex> lock(op_->mtx_);
      // op_->opcClient->Browse(starting_.c_str(), &items_);  // IOPCBrowse call
    }
    void OnOK() override {
      Array arr = Array::New(Env(), items_.size());
      for (size_t i = 0; i < items_.size(); ++i) {
        arr.Set(i, String::New(Env(), items_[i]));
      }
      Deferred().Resolve(Env(), arr);
    }
  };
};

// Emit helper (updated with Env param)
void EmitEvent(napi_threadsafe_function tsfn, const std::string& type, const Napi::Object& dataObj, Env env) {
  Napi::Object event = Napi::Object::New(env);
  event.Set("type", Napi::String::New(env, type));
  event.Set("data", dataObj);
  napi_value argv[1] = { event };
  napi_call_threadsafe_function(tsfn, argv, napi_threadsafe_function_call_mode::NAPI_TSFN_CALL_SYNC);
}

// Simple VARIANT to Napi converter (expand for full OPC types)
Napi::Value VariantToNapi(const Napi::Env& env, VARIANT* var) {
  if (!var) return Napi::Null::New(env);
  switch (var->vt) {
    case VT_I4: return Napi::Number::New(env, var->lVal);
    case VT_R8: return Napi::Number::New(env, var->dblVal);
    case VT_BOOL: return Napi::Boolean::New(env, var->boolVal != 0);
    case VT_BSTR: return Napi::String::New(env, /* _bstr_t(var->bstrVal).GetCString() */ "");  // Use ATL or similar
    case VT_EMPTY: return Napi::Null::New(env);
    // Add VT_DATE, VT_ARRAY, quality/timestamp extraction
    default: return Napi::String::New(env, "Unsupported type");
  }
}

Napi::FunctionReference OPCDA::constructor;

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
  return OPCDA::Init(env, exports);
}

NODE_API_MODULE(opcda, InitAll)