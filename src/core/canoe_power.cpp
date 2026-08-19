#include "core/canoe_power.hpp"

#include <Windows.h>
#include <OleAuto.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace uds {
namespace {

class ComInit {
public:
  ComInit() {
    const auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    initialized_ = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) throw std::runtime_error("COM initialization failed");
  }
  ~ComInit() { if (initialized_) CoUninitialize(); }
private:
  bool initialized_{};
};

class Dispatch {
public:
  Dispatch() = default;
  explicit Dispatch(IDispatch* value) : value_(value) {}
  Dispatch(const Dispatch&) = delete;
  Dispatch& operator=(const Dispatch&) = delete;
  Dispatch(Dispatch&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
  Dispatch& operator=(Dispatch&& other) noexcept {
    if (this != &other) { if (value_) value_->Release(); value_ = other.value_; other.value_ = nullptr; }
    return *this;
  }
  ~Dispatch() { if (value_) value_->Release(); }
  IDispatch* get() const { return value_; }
private:
  IDispatch* value_{};
};

DISPID id_of(IDispatch* object, const wchar_t* name) {
  LPOLESTR mutable_name = const_cast<LPOLESTR>(name);
  DISPID id{};
  if (FAILED(object->GetIDsOfNames(IID_NULL, &mutable_name, 1, LOCALE_USER_DEFAULT, &id)))
    throw std::runtime_error("CANoe COM member not found");
  return id;
}

VARIANT invoke(IDispatch* object, const wchar_t* name, WORD flags,
               VARIANT* arguments = nullptr, UINT count = 0) {
  DISPPARAMS params{};
  params.rgvarg = arguments;
  params.cArgs = count;
  DISPID put_id = DISPID_PROPERTYPUT;
  if ((flags & DISPATCH_PROPERTYPUT) != 0) {
    params.rgdispidNamedArgs = &put_id;
    params.cNamedArgs = 1;
  }
  VARIANT result;
  VariantInit(&result);
  EXCEPINFO exception{};
  UINT bad_argument{};
  const auto hr = object->Invoke(id_of(object, name), IID_NULL, LOCALE_USER_DEFAULT,
                                 flags, &params, &result, &exception, &bad_argument);
  if (FAILED(hr)) {
    VariantClear(&result);
    throw std::runtime_error("CANoe COM invocation failed");
  }
  return result;
}

Dispatch dispatch_property(IDispatch* object, const wchar_t* name) {
  auto value = invoke(object, name, DISPATCH_PROPERTYGET);
  if (value.vt != VT_DISPATCH || value.pdispVal == nullptr) {
    VariantClear(&value);
    throw std::runtime_error("CANoe COM property is not an object");
  }
  auto* result = value.pdispVal;
  value.vt = VT_EMPTY;
  return Dispatch(result);
}

Dispatch item(IDispatch* collection, const wchar_t* name) {
  VARIANT argument;
  VariantInit(&argument);
  argument.vt = VT_BSTR;
  argument.bstrVal = SysAllocString(name);
  auto value = invoke(collection, L"Item", DISPATCH_PROPERTYGET, &argument, 1);
  VariantClear(&argument);
  if (value.vt != VT_DISPATCH || value.pdispVal == nullptr) {
    VariantClear(&value);
    throw std::runtime_error("CANoe COM item not found");
  }
  auto* result = value.pdispVal;
  value.vt = VT_EMPTY;
  return Dispatch(result);
}

bool bool_property(IDispatch* object, const wchar_t* name) {
  auto value = invoke(object, name, DISPATCH_PROPERTYGET);
  const auto result = value.vt == VT_BOOL ? value.boolVal == VARIANT_TRUE : value.lVal != 0;
  VariantClear(&value);
  return result;
}

void call(IDispatch* object, const wchar_t* name) {
  auto value = invoke(object, name, DISPATCH_METHOD);
  VariantClear(&value);
}

void put_int(IDispatch* object, const wchar_t* name, int number) {
  VARIANT argument;
  VariantInit(&argument);
  argument.vt = VT_I4;
  argument.lVal = number;
  auto value = invoke(object, name, DISPATCH_PROPERTYPUT, &argument, 1);
  VariantClear(&value);
}

int int_property(IDispatch* object, const wchar_t* name) {
  auto value = invoke(object, name, DISPATCH_PROPERTYGET);
  VARIANT converted;
  VariantInit(&converted);
  if (FAILED(VariantChangeType(&converted, &value, 0, VT_I4))) {
    VariantClear(&value);
    throw std::runtime_error("CANoe DOUT value is not numeric");
  }
  const auto result = converted.lVal;
  VariantClear(&converted);
  VariantClear(&value);
  return result;
}

std::wstring string_property(IDispatch* object, const wchar_t* name) {
  auto value = invoke(object, name, DISPATCH_PROPERTYGET);
  std::wstring result;
  if (value.vt == VT_BSTR && value.bstrVal) result = value.bstrVal;
  VariantClear(&value);
  return result;
}

Dispatch active_canoe() {
  CLSID clsid{};
  if (FAILED(CLSIDFromProgID(L"CANoe.Application", &clsid)))
    throw std::runtime_error("CANoe COM registration not found");
  IUnknown* unknown{};
  auto hr = GetActiveObject(clsid, nullptr, &unknown);
  if (FAILED(hr)) hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_IUnknown,
                                         reinterpret_cast<void**>(&unknown));
  if (FAILED(hr) || !unknown) throw std::runtime_error("cannot attach to CANoe");
  IDispatch* dispatch{};
  hr = unknown->QueryInterface(IID_IDispatch, reinterpret_cast<void**>(&dispatch));
  unknown->Release();
  if (FAILED(hr) || !dispatch) throw std::runtime_error("CANoe automation is unavailable");
  return Dispatch(dispatch);
}

} // namespace

CanoePowerResult set_canoe_dout(int value) {
  ComInit com;
  auto app = active_canoe();
  auto measurement = dispatch_property(app.get(), L"Measurement");
  bool started = false;
  if (!bool_property(measurement.get(), L"Running")) {
    call(measurement.get(), L"Start");
    started = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!bool_property(measurement.get(), L"Running") && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!bool_property(measurement.get(), L"Running")) throw std::runtime_error("CANoe measurement start timeout");
  }
  auto system = dispatch_property(app.get(), L"System");
  auto namespaces = dispatch_property(system.get(), L"Namespaces");
  auto io = item(namespaces.get(), L"IO");
  auto io_namespaces = dispatch_property(io.get(), L"Namespaces");
  auto device = item(io_namespaces.get(), L"VN1600_1");
  auto variables = dispatch_property(device.get(), L"Variables");
  auto dout = item(variables.get(), L"DOUT");
  put_int(dout.get(), L"Value", value);
  int actual = int_property(dout.get(), L"Value");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (actual != value && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    actual = int_property(dout.get(), L"Value");
  }
  if (actual != value) {
    throw std::runtime_error("CANoe DOUT write verification failed: requested=" +
                             std::to_string(value) + ", actual=" + std::to_string(actual));
  }
  std::wstring configuration;
  try {
    auto config = dispatch_property(app.get(), L"Configuration");
    configuration = string_property(config.get(), L"FullName");
  } catch (...) {}
  return {started, actual, std::move(configuration)};
}

} // namespace uds
