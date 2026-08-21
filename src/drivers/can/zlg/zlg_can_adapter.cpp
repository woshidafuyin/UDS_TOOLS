#include "drivers/can/zlg/zlg_can_adapter.hpp"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4201 4828)
#endif
#include <zlgcan.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace uds {
namespace {

constexpr unsigned kDefaultDeviceType = ZCAN_USBCANFD_200U;
constexpr unsigned kDefaultChannelCount = 2;

std::filesystem::path executable_directory() {
  std::wstring buffer(32768, L'\0');
  const auto length =
      GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return {};
  buffer.resize(length);
  return std::filesystem::path(buffer).parent_path();
}

std::vector<std::filesystem::path> library_candidates() {
  std::vector<std::filesystem::path> candidates;
  std::array<wchar_t, 32768> configured{};
  const auto configured_length = GetEnvironmentVariableW(
      L"UDS_ZLG_DRIVER_DIR", configured.data(),
      static_cast<DWORD>(configured.size()));
  if (configured_length > 0 && configured_length < configured.size()) {
    candidates.emplace_back(std::filesystem::path(configured.data()) /
                            L"zlgcan.dll");
  }
  const auto executable = executable_directory();
  if (!executable.empty()) {
    candidates.emplace_back(executable / L"drivers" / L"zlg" / L"zlgcan.dll");
    candidates.emplace_back(executable / L"zlgcan.dll");
  }
  return candidates;
}

std::string narrow_ascii(const UCHAR* value, std::size_t capacity) {
  const auto* begin = reinterpret_cast<const char*>(value);
  const auto length = std::find(begin, begin + capacity, '\0') - begin;
  return std::string(begin, length);
}

std::pair<unsigned, unsigned> parse_device_id(std::string_view device_id) {
  if (device_id.empty()) return {kDefaultDeviceType, 0};
  constexpr std::string_view prefix = "zlg:";
  if (!device_id.starts_with(prefix)) {
    unsigned index{};
    const auto result = std::from_chars(
        device_id.data(), device_id.data() + device_id.size(), index);
    if (result.ec == std::errc{} &&
        result.ptr == device_id.data() + device_id.size()) {
      return {kDefaultDeviceType, index};
    }
    throw std::invalid_argument(
        "ZLG device id must be zlg:<device-type>:<index>");
  }
  const auto separator = device_id.find(':', prefix.size());
  if (separator == std::string_view::npos) {
    throw std::invalid_argument(
        "ZLG device id must be zlg:<device-type>:<index>");
  }
  unsigned device_type{};
  unsigned device_index{};
  const auto type_text =
      device_id.substr(prefix.size(), separator - prefix.size());
  const auto index_text = device_id.substr(separator + 1);
  const auto type_result = std::from_chars(
      type_text.data(), type_text.data() + type_text.size(), device_type);
  const auto index_result = std::from_chars(
      index_text.data(), index_text.data() + index_text.size(), device_index);
  if (type_result.ec != std::errc{} ||
      type_result.ptr != type_text.data() + type_text.size() ||
      index_result.ec != std::errc{} ||
      index_result.ptr != index_text.data() + index_text.size()) {
    throw std::invalid_argument(
        "ZLG device id must be zlg:<device-type>:<index>");
  }
  return {device_type, device_index};
}

std::string library_search_detail(
    const std::vector<std::filesystem::path>& candidates) {
  std::ostringstream detail;
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    if (index != 0) detail << "; ";
    detail << candidates[index].string();
  }
  return detail.str();
}

std::string describe_channel_error(UINT error_code) {
  struct NamedBit {
    UINT bit;
    const char* name;
  };
  constexpr NamedBit known_bits[] = {
      {ZCAN_ERROR_CAN_OVERFLOW, "CAN_OVERFLOW"},
      {ZCAN_ERROR_CAN_ERRALARM, "CAN_ERROR_ALARM"},
      {ZCAN_ERROR_CAN_PASSIVE, "CAN_PASSIVE"},
      {ZCAN_ERROR_CAN_LOSE, "ARBITRATION_LOST"},
      {ZCAN_ERROR_CAN_BUSERR, "CAN_BUS_ERROR"},
      {ZCAN_ERROR_CAN_BUSOFF, "CAN_BUS_OFF"},
      {ZCAN_ERROR_CAN_BUFFER_OVERFLOW, "CAN_BUFFER_OVERFLOW"},
  };
  std::ostringstream names;
  for (const auto& entry : known_bits) {
    if ((error_code & entry.bit) == 0) continue;
    if (names.tellp() > 0) names << '|';
    names << entry.name;
  }
  return names.str();
}

} // namespace

struct ZlgCanAdapter::Impl {
  using FnOpenDevice = DEVICE_HANDLE(FUNC_CALL*)(UINT, UINT, UINT);
  using FnCloseDevice = UINT(FUNC_CALL*)(DEVICE_HANDLE);
  using FnGetDeviceInfo = UINT(FUNC_CALL*)(DEVICE_HANDLE, ZCAN_DEVICE_INFO*);
  using FnInitCan = CHANNEL_HANDLE(FUNC_CALL*)(
      DEVICE_HANDLE, UINT, ZCAN_CHANNEL_INIT_CONFIG*);
  using FnStartCan = UINT(FUNC_CALL*)(CHANNEL_HANDLE);
  using FnResetCan = UINT(FUNC_CALL*)(CHANNEL_HANDLE);
  using FnReadChannelError =
      UINT(FUNC_CALL*)(CHANNEL_HANDLE, ZCAN_CHANNEL_ERR_INFO*);
  using FnReadChannelStatus =
      UINT(FUNC_CALL*)(CHANNEL_HANDLE, ZCAN_CHANNEL_STATUS*);
  using FnTransmit = UINT(FUNC_CALL*)(
      CHANNEL_HANDLE, ZCAN_Transmit_Data*, UINT);
  using FnReceive = UINT(FUNC_CALL*)(
      CHANNEL_HANDLE, ZCAN_Receive_Data*, UINT, int);
  using FnTransmitFd = UINT(FUNC_CALL*)(
      CHANNEL_HANDLE, ZCAN_TransmitFD_Data*, UINT);
  using FnReceiveFd = UINT(FUNC_CALL*)(
      CHANNEL_HANDLE, ZCAN_ReceiveFD_Data*, UINT, int);
  using FnSetValue = UINT(FUNC_CALL*)(DEVICE_HANDLE, const char*, const void*);

  HMODULE library{};
  DEVICE_HANDLE device{};
  CHANNEL_HANDLE channel{};
  unsigned device_type{kDefaultDeviceType};
  unsigned device_index{};
  unsigned channel_index{};
  unsigned channel_count{kDefaultChannelCount};
  bool configured_fd{true};

  FnOpenDevice open_device{};
  FnCloseDevice close_device{};
  FnGetDeviceInfo get_device_info{};
  FnInitCan init_can{};
  FnStartCan start_can{};
  FnResetCan reset_can{};
  FnReadChannelError read_channel_error{};
  FnReadChannelStatus read_channel_status{};
  FnTransmit transmit{};
  FnReceive receive{};
  FnTransmitFd transmit_fd{};
  FnReceiveFd receive_fd{};
  FnSetValue set_value{};
};

ZlgCanAdapter::ZlgCanAdapter() = default;
ZlgCanAdapter::~ZlgCanAdapter() { release(); }

CanVendor ZlgCanAdapter::vendor() const noexcept { return CanVendor::Zlg; }

void ZlgCanAdapter::initialize() {
  {
    std::scoped_lock lock(state_mutex_);
    if (status_.initialized) return;
  }

  const auto candidates = library_candidates();
  auto implementation = std::make_unique<Impl>();
  for (const auto& candidate : candidates) {
    if (!std::filesystem::is_regular_file(candidate)) continue;
    implementation->library = LoadLibraryExW(
        candidate.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (implementation->library) break;
  }
  if (!implementation->library) {
    fail(CanAdapterErrorCode::DriverMissing,
         "ZLG zlgcan.dll was not found or could not be loaded; searched: " +
             library_search_detail(candidates));
  }

  const auto required = [&implementation](const char* name) -> FARPROC {
    const auto function = GetProcAddress(implementation->library, name);
    if (!function) {
      throw std::runtime_error(std::string("ZLG API is missing: ") + name);
    }
    return function;
  };
  try {
    implementation->open_device =
        reinterpret_cast<Impl::FnOpenDevice>(required("ZCAN_OpenDevice"));
    implementation->close_device =
        reinterpret_cast<Impl::FnCloseDevice>(required("ZCAN_CloseDevice"));
    implementation->get_device_info =
        reinterpret_cast<Impl::FnGetDeviceInfo>(required("ZCAN_GetDeviceInf"));
    implementation->init_can =
        reinterpret_cast<Impl::FnInitCan>(required("ZCAN_InitCAN"));
    implementation->start_can =
        reinterpret_cast<Impl::FnStartCan>(required("ZCAN_StartCAN"));
    implementation->reset_can =
        reinterpret_cast<Impl::FnResetCan>(required("ZCAN_ResetCAN"));
    implementation->read_channel_error =
        reinterpret_cast<Impl::FnReadChannelError>(
            required("ZCAN_ReadChannelErrInfo"));
    implementation->read_channel_status =
        reinterpret_cast<Impl::FnReadChannelStatus>(
            required("ZCAN_ReadChannelStatus"));
    implementation->transmit =
        reinterpret_cast<Impl::FnTransmit>(required("ZCAN_Transmit"));
    implementation->receive =
        reinterpret_cast<Impl::FnReceive>(required("ZCAN_Receive"));
    implementation->transmit_fd =
        reinterpret_cast<Impl::FnTransmitFd>(required("ZCAN_TransmitFD"));
    implementation->receive_fd =
        reinterpret_cast<Impl::FnReceiveFd>(required("ZCAN_ReceiveFD"));
    implementation->set_value =
        reinterpret_cast<Impl::FnSetValue>(required("ZCAN_SetValue"));
  } catch (const std::exception& error) {
    FreeLibrary(implementation->library);
    implementation->library = nullptr;
    fail(CanAdapterErrorCode::DriverMissing, error.what());
  }

  std::scoped_lock lock(state_mutex_);
  impl_ = std::move(implementation);
  status_ = {CanVendor::Zlg, CanAdapterState::Ready, true, false, false};
  last_error_ = {};
}

void ZlgCanAdapter::release() noexcept {
  close_device();
  std::unique_ptr<Impl> implementation;
  {
    std::scoped_lock lock(state_mutex_);
    implementation = std::move(impl_);
    status_ = {CanVendor::Zlg, CanAdapterState::Uninitialized,
               false, false, false};
  }
  if (implementation && implementation->library) {
    FreeLibrary(implementation->library);
  }
}

std::vector<CanDeviceInfo> ZlgCanAdapter::enumerate_devices() {
  initialize();
  Impl* implementation{};
  {
    std::scoped_lock lock(state_mutex_);
    if (status_.device_open) {
      return {{CanVendor::Zlg,
               "zlg:" + std::to_string(impl_->device_type) + ":" +
                   std::to_string(impl_->device_index),
               "ZLG CAN device (open)", impl_->channel_count}};
    }
    implementation = impl_.get();
  }

  std::vector<CanDeviceInfo> devices;
  for (unsigned index = 0; index < 8; ++index) {
    const auto handle =
        implementation->open_device(kDefaultDeviceType, index, 0);
    if (handle == INVALID_DEVICE_HANDLE) {
      if (index == 0) continue;
      break;
    }
    ZCAN_DEVICE_INFO info{};
    const auto info_status =
        implementation->get_device_info(handle, &info);
    implementation->close_device(handle);
    std::string display_name = "ZLG USBCANFD-200U";
    unsigned channel_count = kDefaultChannelCount;
    if (info_status == STATUS_OK) {
      const auto hardware_type =
          narrow_ascii(info.str_hw_Type, sizeof(info.str_hw_Type));
      const auto serial =
          narrow_ascii(info.str_Serial_Num, sizeof(info.str_Serial_Num));
      if (!hardware_type.empty()) display_name = hardware_type;
      if (!serial.empty()) display_name += " [" + serial + "]";
      if (info.can_Num != 0) channel_count = info.can_Num;
    }
    devices.push_back(
        {CanVendor::Zlg,
         "zlg:" + std::to_string(kDefaultDeviceType) + ":" +
             std::to_string(index),
         std::move(display_name), channel_count});
  }
  return devices;
}

void ZlgCanAdapter::open_device(std::string_view device_id) {
  initialize();
  unsigned device_type{};
  unsigned device_index{};
  try {
    std::tie(device_type, device_index) = parse_device_id(device_id);
  } catch (const std::exception& error) {
    fail(CanAdapterErrorCode::InvalidConfiguration, error.what());
  }

  Impl* implementation{};
  {
    std::scoped_lock lock(state_mutex_);
    if (status_.device_open) return;
    implementation = impl_.get();
  }
  const auto handle =
      implementation->open_device(device_type, device_index, 0);
  if (handle == INVALID_DEVICE_HANDLE) {
    fail(CanAdapterErrorCode::DeviceNotFound,
         "ZLG device could not be opened: type=" +
             std::to_string(device_type) +
             ", index=" + std::to_string(device_index));
  }

  ZCAN_DEVICE_INFO info{};
  const auto info_status =
      implementation->get_device_info(handle, &info);
  std::scoped_lock lock(state_mutex_);
  implementation->device = handle;
  implementation->device_type = device_type;
  implementation->device_index = device_index;
  implementation->channel_count =
      info_status == STATUS_OK && info.can_Num != 0
          ? static_cast<unsigned>(info.can_Num)
          : kDefaultChannelCount;
  status_.device_open = true;
  status_.state = CanAdapterState::DeviceOpen;
  last_error_ = {};
}

void ZlgCanAdapter::close_device() noexcept {
  stop_channel();
  Impl* implementation{};
  DEVICE_HANDLE device{};
  {
    std::scoped_lock lock(state_mutex_);
    implementation = impl_.get();
    if (implementation) {
      device = implementation->device;
      implementation->device = {};
      implementation->channel = {};
    }
    status_.device_open = false;
    status_.channel_started = false;
    if (status_.initialized) status_.state = CanAdapterState::Ready;
  }
  if (implementation && device != INVALID_DEVICE_HANDLE) {
    implementation->close_device(device);
  }
}

void ZlgCanAdapter::configure_channel(const CanChannelConfig& config) {
  Impl* implementation{};
  DEVICE_HANDLE device{};
  unsigned channel_count{};
  {
    std::scoped_lock lock(state_mutex_);
    implementation = impl_.get();
    device = implementation ? implementation->device : DEVICE_HANDLE{};
    channel_count = implementation ? implementation->channel_count : 0;
  }
  if (!implementation || device == INVALID_DEVICE_HANDLE) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "ZLG device must be opened before channel configuration");
  }
  if (config.channel == 0 || config.channel > channel_count ||
      config.nominal_bitrate == 0 ||
      (config.can_fd && config.data_bitrate == 0)) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "ZLG CAN channel configuration is invalid");
  }

  const auto channel_index = config.channel - 1;
  const auto channel_path = std::to_string(channel_index) + "/";
  const auto set_text = [&](std::string_view key, const std::string& value) {
    const auto path = channel_path + std::string(key);
    if (implementation->set_value(device, path.c_str(), value.c_str()) !=
        STATUS_OK) {
      fail(CanAdapterErrorCode::VendorError,
           "ZLG ZCAN_SetValue failed: " + path + "=" + value);
    }
  };
  set_text("canfd_standard", "0");
  set_text("canfd_abit_baud_rate",
           std::to_string(config.nominal_bitrate));
  set_text("canfd_dbit_baud_rate",
           std::to_string(config.data_bitrate == 0
                              ? config.nominal_bitrate
                              : config.data_bitrate));
  set_text("tx_timeout", "100");

  ZCAN_CHANNEL_INIT_CONFIG initialization{};
  initialization.can_type = TYPE_CANFD;
  initialization.canfd.acc_code = 0;
  initialization.canfd.acc_mask = 0xFFFFFFFFU;
  initialization.canfd.filter = 0;
  initialization.canfd.mode = 0;
  const auto channel =
      implementation->init_can(device, channel_index, &initialization);
  if (channel == INVALID_CHANNEL_HANDLE) {
    fail(CanAdapterErrorCode::VendorError,
         "ZLG ZCAN_InitCAN failed for channel " +
             std::to_string(config.channel));
  }

  std::scoped_lock lock(state_mutex_);
  implementation->channel = channel;
  implementation->channel_index = channel_index;
  implementation->configured_fd = config.can_fd;
  status_.state = CanAdapterState::ChannelConfigured;
  status_.channel_started = false;
  last_error_ = {};
}

void ZlgCanAdapter::start_channel() {
  Impl* implementation{};
  CHANNEL_HANDLE channel{};
  {
    std::scoped_lock lock(state_mutex_);
    if (status_.channel_started) return;
    implementation = impl_.get();
    channel = implementation ? implementation->channel : CHANNEL_HANDLE{};
  }
  if (!implementation || channel == INVALID_CHANNEL_HANDLE) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "ZLG channel must be configured before start");
  }
  if (implementation->start_can(channel) != STATUS_OK) {
    fail(CanAdapterErrorCode::VendorError, "ZLG ZCAN_StartCAN failed");
  }
  std::scoped_lock lock(state_mutex_);
  status_.state = CanAdapterState::ChannelStarted;
  status_.channel_started = true;
  last_error_ = {};
}

void ZlgCanAdapter::stop_channel() noexcept {
  Impl* implementation{};
  CHANNEL_HANDLE channel{};
  {
    std::scoped_lock lock(state_mutex_);
    implementation = impl_.get();
    channel = implementation ? implementation->channel : CHANNEL_HANDLE{};
    status_.channel_started = false;
    if (status_.device_open && channel != INVALID_CHANNEL_HANDLE) {
      status_.state = CanAdapterState::ChannelConfigured;
    }
  }
  if (implementation && channel != INVALID_CHANNEL_HANDLE) {
    implementation->reset_can(channel);
  }
}

void ZlgCanAdapter::send(const CanFrame& frame) {
  Impl* implementation{};
  CHANNEL_HANDLE channel{};
  {
    std::scoped_lock lock(state_mutex_);
    if (!status_.channel_started) {
      implementation = nullptr;
    } else {
      implementation = impl_.get();
      channel = implementation ? implementation->channel : CHANNEL_HANDLE{};
    }
  }
  if (!implementation || channel == INVALID_CHANNEL_HANDLE) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "ZLG channel is not started");
  }
  if (frame.data.size() > (frame.fd ? CANFD_MAX_DLEN : CAN_MAX_DLEN)) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "ZLG CAN payload length is invalid");
  }

  const auto can_id =
      MAKE_CAN_ID(frame.id, frame.extended ? 1U : 0U, 0U, 0U);
  UINT transmitted{};
  if (frame.fd) {
    ZCAN_TransmitFD_Data outgoing{};
    outgoing.frame.can_id = can_id;
    outgoing.frame.len = static_cast<BYTE>(frame.data.size());
    outgoing.frame.flags = frame.brs ? CANFD_BRS : 0;
    std::copy(frame.data.begin(), frame.data.end(), outgoing.frame.data);
    outgoing.transmit_type = 0;
    transmitted = implementation->transmit_fd(channel, &outgoing, 1);
  } else {
    ZCAN_Transmit_Data outgoing{};
    outgoing.frame.can_id = can_id;
    outgoing.frame.can_dlc = static_cast<BYTE>(frame.data.size());
    std::copy(frame.data.begin(), frame.data.end(), outgoing.frame.data);
    outgoing.transmit_type = 0;
    transmitted = implementation->transmit(channel, &outgoing, 1);
  }
  if (transmitted != 1) {
    ZCAN_CHANNEL_ERR_INFO error{};
    std::ostringstream detail;
    detail << "ZLG transmitted zero CAN frames";
    if (implementation->read_channel_error(channel, &error) == STATUS_OK) {
      detail << "; channel_error=0x" << std::hex << std::uppercase
             << error.error_code;
      const auto description = describe_channel_error(error.error_code);
      if (!description.empty()) detail << '(' << description << ')';
    }
    ZCAN_CHANNEL_STATUS channel_status{};
    if (implementation->read_channel_status(channel, &channel_status) ==
        STATUS_OK) {
      detail << std::dec
             << "; tx_error_counter="
             << static_cast<unsigned>(channel_status.regTECounter)
             << "; rx_error_counter="
             << static_cast<unsigned>(channel_status.regRECounter);
    }
    const auto message = detail.str();
    remember_error(CanAdapterErrorCode::TransmitFailedNoFrames, message);
    throw CanAdapterException(
        {CanAdapterErrorCode::TransmitFailedNoFrames, message});
  }
}

std::optional<CanFrame> ZlgCanAdapter::receive(
    std::chrono::milliseconds timeout) {
  Impl* implementation{};
  CHANNEL_HANDLE channel{};
  {
    std::scoped_lock lock(state_mutex_);
    if (status_.channel_started) {
      implementation = impl_.get();
      channel = implementation ? implementation->channel : CHANNEL_HANDLE{};
    }
  }
  if (!implementation || channel == INVALID_CHANNEL_HANDLE) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "ZLG channel is not started");
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::max(timeout, std::chrono::milliseconds::zero());
  do {
    ZCAN_ReceiveFD_Data incoming_fd{};
    if (implementation->receive_fd(channel, &incoming_fd, 1, 0) == 1) {
      const auto length =
          std::min<std::size_t>(incoming_fd.frame.len, CANFD_MAX_DLEN);
      return CanFrame{
          GET_ID(incoming_fd.frame.can_id),
          {incoming_fd.frame.data, incoming_fd.frame.data + length},
          IS_EFF(incoming_fd.frame.can_id) != 0, true,
          (incoming_fd.frame.flags & CANFD_BRS) != 0};
    }

    ZCAN_Receive_Data incoming{};
    if (implementation->receive(channel, &incoming, 1, 0) == 1) {
      const auto length =
          std::min<std::size_t>(incoming.frame.can_dlc, CAN_MAX_DLEN);
      return CanFrame{
          GET_ID(incoming.frame.can_id),
          {incoming.frame.data, incoming.frame.data + length},
          IS_EFF(incoming.frame.can_id) != 0, false, false};
    }
    if (std::chrono::steady_clock::now() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (true);
  return std::nullopt;
}

CanAdapterStatus ZlgCanAdapter::status() const noexcept {
  std::scoped_lock lock(state_mutex_);
  return status_;
}

CanAdapterError ZlgCanAdapter::last_error() const {
  std::scoped_lock lock(state_mutex_);
  return last_error_;
}

[[noreturn]] void ZlgCanAdapter::fail(CanAdapterErrorCode code,
                                      std::string message) {
  remember_error(code, message);
  throw CanAdapterException({code, std::move(message)});
}

void ZlgCanAdapter::remember_error(CanAdapterErrorCode code,
                                   std::string message) noexcept {
  try {
    std::scoped_lock lock(state_mutex_);
    last_error_ = {code, std::move(message)};
    status_.state = CanAdapterState::Error;
  } catch (...) {
  }
}

} // namespace uds
