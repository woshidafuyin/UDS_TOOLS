#include "drivers/can/tosun/tosun_can_adapter.hpp"

#include <libTSCAN.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace uds {
namespace {

constexpr std::uint32_t kTscanSuccess = 0;
constexpr std::uint32_t kTscanAlreadyConnected = 5;
constexpr std::uint32_t kTransmitTimeoutMs = 500;
// libTSCAN ARxTx=0 returns RX only; values >0 include TX echoes. With TX
// echoes enabled, a 146-CF Driver block hides the ECU response behind more
// than 100 ms of one-at-a-time FIFO draining and causes a false P2 timeout.
constexpr std::uint8_t kReceiveOnly = 0;
constexpr std::array kReconnectBackoff{
    std::chrono::milliseconds(250),
    std::chrono::milliseconds(1000),
};
constexpr auto kTransportFailureCooldown = std::chrono::milliseconds(1000);

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
      L"UDS_TOSUN_DRIVER_DIR", configured.data(),
      static_cast<DWORD>(configured.size()));
  if (configured_length > 0 && configured_length < configured.size()) {
    candidates.emplace_back(std::filesystem::path(configured.data()) /
                            L"libTSCAN.dll");
  }
  const auto executable = executable_directory();
  if (!executable.empty()) {
    candidates.emplace_back(executable / L"drivers" / L"tosun" /
                            L"libTSCAN.dll");
    candidates.emplace_back(executable / L"libTSCAN.dll");
  }
  candidates.emplace_back(
      std::filesystem::path(
          L"C:\\Program Files (x86)\\TOSUN\\TSMaster\\bin64\\libTSCAN.dll"));
  return candidates;
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

std::uint8_t length_to_dlc(std::size_t length) {
  if (length <= 8) return static_cast<std::uint8_t>(length);
  if (length <= 12) return 9;
  if (length <= 16) return 10;
  if (length <= 20) return 11;
  if (length <= 24) return 12;
  if (length <= 32) return 13;
  if (length <= 48) return 14;
  if (length <= 64) return 15;
  throw std::invalid_argument("TOSUN CAN FD payload exceeds 64 bytes");
}

std::size_t dlc_to_length(std::uint8_t dlc) {
  constexpr std::array<std::size_t, 16> lengths{
      0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
  return lengths[std::min<std::size_t>(dlc, lengths.size() - 1)];
}

std::string normalize_serial(std::string_view device_id) {
  constexpr std::string_view prefix = "tosun:";
  return std::string(device_id.starts_with(prefix)
                         ? device_id.substr(prefix.size())
                         : device_id);
}

TLIBCAN make_classic_frame(const CanFrame& frame, unsigned channel_index) {
  if (frame.data.size() > 8) {
    throw std::invalid_argument("TOSUN Classic CAN payload exceeds 8 bytes");
  }
  TLIBCAN outgoing{};
  outgoing.FIdxChn = static_cast<u8>(channel_index);
  outgoing.FProperties.value = 0;
  outgoing.FProperties.bits.istx = 1;
  outgoing.FProperties.bits.extframe = frame.extended ? 1 : 0;
  outgoing.FDLC = static_cast<u8>(frame.data.size());
  outgoing.FIdentifier = static_cast<s32>(frame.id);
  std::copy(frame.data.begin(), frame.data.end(), outgoing.FData);
  return outgoing;
}

TLIBCANFD make_fd_frame(const CanFrame& frame, unsigned channel_index) {
  if (frame.data.size() > 64) {
    throw std::invalid_argument("TOSUN CAN FD payload exceeds 64 bytes");
  }
  TLIBCANFD outgoing{};
  outgoing.FIdxChn = static_cast<u8>(channel_index);
  outgoing.FProperties.value = 0;
  outgoing.FProperties.bits.istx = 1;
  outgoing.FProperties.bits.extframe = frame.extended ? 1 : 0;
  outgoing.FDLC = length_to_dlc(frame.data.size());
  outgoing.FFDProperties.value = 0;
  outgoing.FFDProperties.bits.EDL = 1;
  outgoing.FFDProperties.bits.BRS = frame.brs ? 1 : 0;
  outgoing.FIdentifier = static_cast<s32>(frame.id);
  std::copy(frame.data.begin(), frame.data.end(), outgoing.FData);
  return outgoing;
}

struct ProcessTscanRuntime {
  using FnFinalize = void(__stdcall*)();

  std::mutex mutex;
  HMODULE library{};
  FnFinalize finalize{};
  bool initialized{};
  std::chrono::steady_clock::time_point reconnect_not_before{};

  ~ProcessTscanRuntime() {
    if (initialized && finalize) finalize();
    if (library) FreeLibrary(library);
  }
};

ProcessTscanRuntime& process_tscan_runtime() {
  static ProcessTscanRuntime runtime;
  return runtime;
}

void note_transport_failure() noexcept {
  try {
    auto& runtime = process_tscan_runtime();
    std::scoped_lock lock(runtime.mutex);
    runtime.reconnect_not_before =
        std::max(runtime.reconnect_not_before,
                 std::chrono::steady_clock::now() +
                     kTransportFailureCooldown);
  } catch (...) {
  }
}

void wait_for_reconnect_window() {
  std::chrono::steady_clock::time_point not_before;
  {
    auto& runtime = process_tscan_runtime();
    std::scoped_lock lock(runtime.mutex);
    not_before = runtime.reconnect_not_before;
  }
  if (const auto now = std::chrono::steady_clock::now(); now < not_before) {
    std::this_thread::sleep_until(not_before);
  }
}

} // namespace

struct TosunCanAdapter::Impl {
  using FnInitialize = void(__stdcall*)(bool, bool, bool);
  using FnFinalize = void(__stdcall*)();
  using FnScanDevices = std::uint32_t(__stdcall*)(std::uint32_t*);
  using FnGetDeviceInfo =
      std::uint32_t(__stdcall*)(std::int32_t, char**, char**, char**);
  using FnConnect = std::uint32_t(__stdcall*)(const char*, std::size_t*);
  using FnGetCanChannelCount =
      std::uint32_t(__stdcall*)(std::size_t, std::int32_t*);
  using FnDisconnect = std::uint32_t(__stdcall*)(std::size_t);
  using FnConfigCan = std::uint32_t(__stdcall*)(
      std::size_t, APP_CHANNEL, double, std::uint32_t);
  using FnConfigCanFd = std::uint32_t(__stdcall*)(
      std::size_t, APP_CHANNEL, double, double, TLIBCANFDControllerType,
      TLIBCANFDControllerMode, std::uint32_t);
  using FnTransmitCan =
      std::uint32_t(__stdcall*)(std::size_t, const TLIBCAN*, std::uint32_t);
  using FnTransmitCanFd =
      std::uint32_t(__stdcall*)(std::size_t, const TLIBCANFD*, std::uint32_t);
  using FnTransmitCanSequence =
      std::uint32_t(__stdcall*)(std::size_t, const TLIBCAN*, std::int32_t);
  using FnTransmitCanFdSequence =
      std::uint32_t(__stdcall*)(std::size_t, const TLIBCANFD*, std::int32_t);
  using FnTransmitCanAsync =
      std::uint32_t(__stdcall*)(std::size_t, const TLIBCAN*);
  using FnTransmitCanFdAsync =
      std::uint32_t(__stdcall*)(std::size_t, const TLIBCANFD*);
  using FnReceiveCan = std::uint32_t(__stdcall*)(
      std::size_t, TLIBCAN*, std::int32_t*, std::uint8_t, std::uint8_t);
  using FnReceiveCanFd = std::uint32_t(__stdcall*)(
      std::size_t, TLIBCANFD*, std::int32_t*, std::uint8_t, std::uint8_t);
  using FnGetErrorDescription =
      std::uint32_t(__stdcall*)(std::uint32_t, char**);

  HMODULE library{};
  std::size_t device{};
  unsigned channel_index{};
  unsigned channel_count{32};
  bool configured_fd{true};
  bool initialized{};
  bool device_connected{};
  std::string device_serial;

  FnInitialize initialize{};
  FnFinalize finalize{};
  FnScanDevices scan_devices{};
  FnGetDeviceInfo get_device_info{};
  FnConnect connect{};
  FnGetCanChannelCount get_can_channel_count{};
  FnDisconnect disconnect{};
  FnConfigCan config_can{};
  FnConfigCanFd config_can_fd{};
  FnTransmitCan transmit_can{};
  FnTransmitCanFd transmit_can_fd{};
  FnTransmitCanSequence transmit_can_sequence{};
  FnTransmitCanFdSequence transmit_can_fd_sequence{};
  FnTransmitCanAsync transmit_can_async{};
  FnTransmitCanFdAsync transmit_can_fd_async{};
  FnReceiveCan receive_can{};
  FnReceiveCanFd receive_can_fd{};
  FnGetErrorDescription get_error_description{};

  std::string error_text(std::uint32_t code) const {
    char* description{};
    if (get_error_description &&
        get_error_description(code, &description) == kTscanSuccess &&
        description && *description) {
      return std::string(description) + " (status=" +
             std::to_string(code) + ')';
    }
    return "status=" + std::to_string(code);
  }
};

TosunCanAdapter::TosunCanAdapter() = default;
TosunCanAdapter::~TosunCanAdapter() { release(); }

CanVendor TosunCanAdapter::vendor() const noexcept {
  return CanVendor::Tosun;
}

void TosunCanAdapter::initialize() {
  {
    std::scoped_lock lock(state_mutex_);
    if (status_.initialized) return;
  }

  const auto candidates = library_candidates();
  auto implementation = std::make_unique<Impl>();
  auto& runtime = process_tscan_runtime();
  std::scoped_lock runtime_lock(runtime.mutex);
  if (!runtime.library) {
    for (const auto& candidate : candidates) {
      if (!std::filesystem::is_regular_file(candidate)) continue;
      runtime.library = LoadLibraryExW(
          candidate.c_str(), nullptr,
          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
      if (runtime.library) break;
    }
  }
  implementation->library = runtime.library;
  if (!implementation->library) {
    fail(CanAdapterErrorCode::DriverMissing,
         "TOSUN libTSCAN.dll was not found or could not be loaded; searched: " +
             library_search_detail(candidates));
  }

  const auto required = [&implementation](const char* name) -> FARPROC {
    const auto function = GetProcAddress(implementation->library, name);
    if (!function) {
      throw std::runtime_error(std::string("TOSUN API is missing: ") + name);
    }
    return function;
  };
  try {
    implementation->initialize =
        reinterpret_cast<Impl::FnInitialize>(required("initialize_lib_tscan"));
    implementation->finalize =
        reinterpret_cast<Impl::FnFinalize>(required("finalize_lib_tscan"));
    implementation->scan_devices =
        reinterpret_cast<Impl::FnScanDevices>(required("tscan_scan_devices"));
    implementation->get_device_info =
        reinterpret_cast<Impl::FnGetDeviceInfo>(
            GetProcAddress(implementation->library, "tscan_get_device_info"));
    implementation->connect =
        reinterpret_cast<Impl::FnConnect>(required("tscan_connect"));
    implementation->get_can_channel_count =
        reinterpret_cast<Impl::FnGetCanChannelCount>(
            GetProcAddress(implementation->library,
                           "tscan_get_can_channel_count"));
    implementation->disconnect =
        reinterpret_cast<Impl::FnDisconnect>(
            required("tscan_disconnect_by_handle"));
    implementation->config_can =
        reinterpret_cast<Impl::FnConfigCan>(
            required("tscan_config_can_by_baudrate"));
    implementation->config_can_fd =
        reinterpret_cast<Impl::FnConfigCanFd>(
            required("tscan_config_canfd_by_baudrate"));
    implementation->transmit_can =
        reinterpret_cast<Impl::FnTransmitCan>(
            required("tscan_transmit_can_sync"));
    implementation->transmit_can_fd =
        reinterpret_cast<Impl::FnTransmitCanFd>(
            required("tscan_transmit_canfd_sync"));
    implementation->transmit_can_sequence =
        reinterpret_cast<Impl::FnTransmitCanSequence>(
            GetProcAddress(implementation->library,
                           "tscan_transmit_can_sequence"));
    implementation->transmit_can_fd_sequence =
        reinterpret_cast<Impl::FnTransmitCanFdSequence>(
            GetProcAddress(implementation->library,
                           "tscan_transmit_canfd_sequence"));
    implementation->transmit_can_async =
        reinterpret_cast<Impl::FnTransmitCanAsync>(
            GetProcAddress(implementation->library,
                           "tscan_transmit_can_async"));
    implementation->transmit_can_fd_async =
        reinterpret_cast<Impl::FnTransmitCanFdAsync>(
            GetProcAddress(implementation->library,
                           "tscan_transmit_canfd_async"));
    implementation->receive_can =
        reinterpret_cast<Impl::FnReceiveCan>(
            required("tsfifo_receive_can_msgs"));
    implementation->receive_can_fd =
        reinterpret_cast<Impl::FnReceiveCanFd>(
            required("tsfifo_receive_canfd_msgs"));
    implementation->get_error_description =
        reinterpret_cast<Impl::FnGetErrorDescription>(
            GetProcAddress(implementation->library,
                           "tscan_get_error_description"));
  } catch (const std::exception& error) {
    fail(CanAdapterErrorCode::DriverMissing, error.what());
  }

  if (!runtime.initialized) {
    implementation->initialize(true, false, false);
    runtime.finalize = implementation->finalize;
    runtime.initialized = true;
  }
  implementation->initialized = true;
  std::scoped_lock lock(state_mutex_);
  impl_ = std::move(implementation);
  status_ = {CanVendor::Tosun, CanAdapterState::Ready, true, false, false};
  last_error_ = {};
}

void TosunCanAdapter::release() noexcept {
  close_device();
  std::unique_ptr<Impl> implementation;
  {
    std::scoped_lock lock(state_mutex_);
    implementation = std::move(impl_);
    status_ = {CanVendor::Tosun, CanAdapterState::Uninitialized,
               false, false, false};
  }
  // libTSCAN owns process-global state. ProcessTscanRuntime finalizes and
  // unloads it once at process exit, not between independent bus sessions.
}

std::vector<CanDeviceInfo> TosunCanAdapter::enumerate_devices() {
  initialize();
  Impl* implementation{};
  {
    std::scoped_lock lock(state_mutex_);
    implementation = impl_.get();
  }
  std::uint32_t count{};
  if (implementation->scan_devices(&count) != kTscanSuccess) {
    fail(CanAdapterErrorCode::VendorError, "TOSUN device scan failed");
  }
  std::vector<CanDeviceInfo> devices;
  devices.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    char* manufacturer{};
    char* product{};
    char* serial{};
    std::string display_name = "TOSUN CAN device " + std::to_string(index);
    std::string id;
    if (implementation->get_device_info &&
        implementation->get_device_info(static_cast<std::int32_t>(index),
                                        &manufacturer, &product,
                                        &serial) == kTscanSuccess) {
      if (manufacturer && *manufacturer) display_name = manufacturer;
      if (product && *product) {
        if (!display_name.empty()) display_name += " ";
        display_name += product;
      }
      if (serial && *serial) {
        id = "tosun:" + std::string(serial);
        display_name += " [" + std::string(serial) + "]";
      }
    }
    if (id.empty()) id = index == 0 ? "tosun:" : "tosun-index:" +
                                                        std::to_string(index);
    devices.push_back(
        {CanVendor::Tosun, std::move(id), std::move(display_name),
         1});
  }
  return devices;
}

void TosunCanAdapter::open_device(std::string_view device_id) {
  initialize();
  std::scoped_lock transmit_lock(transmit_mutex_);
  Impl* implementation{};
  {
    std::scoped_lock lock(state_mutex_);
    if (status_.device_open) return;
    implementation = impl_.get();
  }
  auto serial = normalize_serial(device_id);
  if (serial.empty() || serial.starts_with("tosun-index:")) {
    std::uint32_t count{};
    const auto scan_result = implementation->scan_devices(&count);
    if (scan_result != kTscanSuccess || count == 0 ||
        !implementation->get_device_info) {
      fail(CanAdapterErrorCode::DeviceNotFound,
           "TOSUN device serial could not be resolved before connect; " +
               implementation->error_text(scan_result));
    }
    std::uint32_t device_index{};
    if (serial.starts_with("tosun-index:")) {
      try {
        device_index = static_cast<std::uint32_t>(
            std::stoul(serial.substr(std::string("tosun-index:").size())));
      } catch (...) {
        fail(CanAdapterErrorCode::InvalidConfiguration,
             "TOSUN device index is invalid");
      }
    }
    if (device_index >= count) {
      fail(CanAdapterErrorCode::DeviceNotFound,
           "TOSUN device index is outside the scan result");
    }
    char* manufacturer{};
    char* product{};
    char* resolved_serial{};
    const auto info_result = implementation->get_device_info(
        static_cast<std::int32_t>(device_index), &manufacturer, &product,
        &resolved_serial);
    if (info_result != kTscanSuccess || !resolved_serial ||
        !*resolved_serial) {
      fail(CanAdapterErrorCode::DeviceNotFound,
           "TOSUN device scan did not provide a reconnectable serial; " +
               implementation->error_text(info_result));
    }
    serial = resolved_serial;
  }
  std::size_t handle{};
  std::uint32_t result{};
  wait_for_reconnect_window();
  for (std::size_t attempt = 0; attempt <= kReconnectBackoff.size();
       ++attempt) {
    if (attempt != 0) {
      std::this_thread::sleep_for(kReconnectBackoff[attempt - 1]);
      std::uint32_t ignored_count{};
      implementation->scan_devices(&ignored_count);
    }
    handle = 0;
    result = implementation->connect(serial.c_str(), &handle);
    if (result == kTscanSuccess || result == kTscanAlreadyConnected) break;
    note_transport_failure();
  }
  if (result != kTscanSuccess && result != kTscanAlreadyConnected) {
    fail(CanAdapterErrorCode::DeviceNotFound,
         "TOSUN device could not be connected after bounded recovery; " +
             implementation->error_text(result));
  }
  std::int32_t channel_count{};
  if (!implementation->get_can_channel_count ||
      implementation->get_can_channel_count(handle, &channel_count) !=
          kTscanSuccess ||
      channel_count <= 0) {
    channel_count = 32;
  }
  std::scoped_lock lock(state_mutex_);
  implementation->device = handle;
  implementation->device_connected = true;
  implementation->device_serial = serial;
  implementation->channel_count = static_cast<unsigned>(channel_count);
  status_.device_open = true;
  status_.state = CanAdapterState::DeviceOpen;
  last_error_ = {};
}

void TosunCanAdapter::close_device() noexcept {
  std::scoped_lock transmit_lock(transmit_mutex_);
  stop_channel();
  Impl* implementation{};
  std::size_t device{};
  bool connected{};
  {
    std::scoped_lock lock(state_mutex_);
    implementation = impl_.get();
    if (implementation) {
      device = implementation->device;
      connected = implementation->device_connected;
      implementation->device = 0;
      implementation->device_connected = false;
    }
    status_.device_open = false;
    status_.channel_started = false;
    if (status_.initialized) status_.state = CanAdapterState::Ready;
  }
  if (implementation && connected) implementation->disconnect(device);
}

void TosunCanAdapter::configure_channel(const CanChannelConfig& config) {
  std::scoped_lock transmit_lock(transmit_mutex_);
  Impl* implementation{};
  std::size_t device{};
  unsigned channel_count{};
  bool connected{};
  std::string serial;
  {
    std::scoped_lock lock(state_mutex_);
    implementation = impl_.get();
    device = implementation ? implementation->device : 0;
    channel_count = implementation ? implementation->channel_count : 0;
    connected = implementation && implementation->device_connected;
    serial = implementation ? implementation->device_serial : std::string{};
  }
  if (!implementation || !connected) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "TOSUN device must be connected before channel configuration");
  }
  if (config.channel == 0 || config.channel > channel_count ||
      config.nominal_bitrate == 0 ||
      (config.can_fd && config.data_bitrate == 0)) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "TOSUN CAN channel configuration is invalid");
  }
  const auto channel_index = config.channel - 1;
  const auto channel = static_cast<APP_CHANNEL>(channel_index);
  const auto apply_configuration = [&](std::size_t handle) {
    if (config.can_fd) {
      return implementation->config_can_fd(
          handle, channel,
          static_cast<double>(config.nominal_bitrate) / 1000.0,
          static_cast<double>(config.data_bitrate) / 1000.0, lfdtISOCAN,
          lfdmNormal, 0);
    }
    return implementation->config_can(
        handle, channel,
        static_cast<double>(config.nominal_bitrate) / 1000.0, 0);
  };
  auto result = apply_configuration(device);
  const auto initial_configuration_result = result;
  bool reconnect_failed{};
  bool have_connected_device = true;
  for (std::size_t attempt = 0;
       result != kTscanSuccess && attempt < kReconnectBackoff.size();
       ++attempt) {
    note_transport_failure();
    if (have_connected_device) implementation->disconnect(device);
    have_connected_device = false;
    {
      std::scoped_lock lock(state_mutex_);
      implementation->device = 0;
      implementation->device_connected = false;
      status_.device_open = false;
    }
    std::this_thread::sleep_for(kReconnectBackoff[attempt]);
    std::uint32_t ignored_count{};
    implementation->scan_devices(&ignored_count);
    device = 0;
    const auto connect_result =
        implementation->connect(serial.c_str(), &device);
    reconnect_failed =
        connect_result != kTscanSuccess &&
        connect_result != kTscanAlreadyConnected;
    if (reconnect_failed) {
      result = connect_result;
      continue;
    }
    have_connected_device = true;
    {
      std::scoped_lock lock(state_mutex_);
      implementation->device = device;
      implementation->device_connected = true;
      status_.device_open = true;
      status_.state = CanAdapterState::DeviceOpen;
    }
    result = apply_configuration(device);
    reconnect_failed = false;
  }
  if (result != kTscanSuccess) {
    note_transport_failure();
    const auto initial_error =
        implementation->error_text(initial_configuration_result);
    fail(CanAdapterErrorCode::VendorError,
         std::string(reconnect_failed
                         ? "TOSUN channel recovery reconnect failed after "
                           "initial configuration error " +
                               initial_error + "; "
                         : "TOSUN channel bitrate configuration failed after "
                           "bounded reconnect (initial error " +
                               initial_error + "); ") +
             implementation->error_text(result));
  }
  std::scoped_lock lock(state_mutex_);
  implementation->channel_index = channel_index;
  implementation->configured_fd = config.can_fd;
  status_.state = CanAdapterState::ChannelConfigured;
  status_.channel_started = false;
  last_error_ = {};
}

void TosunCanAdapter::start_channel() {
  {
    std::scoped_lock lock(state_mutex_);
    if (impl_ && status_.device_open &&
        status_.state == CanAdapterState::ChannelConfigured) {
      status_.state = CanAdapterState::ChannelStarted;
      status_.channel_started = true;
      last_error_ = {};
      return;
    }
  }
  {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "TOSUN channel must be configured before start");
  }
}

void TosunCanAdapter::stop_channel() noexcept {
  std::scoped_lock lock(state_mutex_);
  status_.channel_started = false;
  if (status_.device_open &&
      status_.state == CanAdapterState::ChannelStarted) {
    status_.state = CanAdapterState::ChannelConfigured;
  }
}

void TosunCanAdapter::send(const CanFrame& frame) {
  std::scoped_lock transmit_lock(transmit_mutex_);
  Impl* implementation{};
  std::size_t device{};
  unsigned channel_index{};
  bool connected{};
  {
    std::scoped_lock lock(state_mutex_);
    if (status_.channel_started) {
      implementation = impl_.get();
      device = implementation ? implementation->device : 0;
      channel_index = implementation ? implementation->channel_index : 0;
      connected = implementation && implementation->device_connected;
    }
  }
  if (!implementation || !connected) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "TOSUN channel is not started");
  }

  std::uint32_t result{};
  if (frame.fd) {
    if (frame.data.size() > 64) {
      fail(CanAdapterErrorCode::InvalidConfiguration,
           "TOSUN CAN FD payload exceeds 64 bytes");
    }
    const auto outgoing = make_fd_frame(frame, channel_index);
    result = implementation->transmit_can_fd_async
                 ? implementation->transmit_can_fd_async(device, &outgoing)
                 : implementation->transmit_can_fd(
                       device, &outgoing, kTransmitTimeoutMs);
  } else {
    if (frame.data.size() > 8) {
      fail(CanAdapterErrorCode::InvalidConfiguration,
           "TOSUN Classic CAN payload exceeds 8 bytes");
    }
    const auto outgoing = make_classic_frame(frame, channel_index);
    result = implementation->transmit_can_async
                 ? implementation->transmit_can_async(device, &outgoing)
                 : implementation->transmit_can(
                       device, &outgoing, kTransmitTimeoutMs);
  }
  if (result != kTscanSuccess) {
    note_transport_failure();
    fail(CanAdapterErrorCode::VendorError,
         "TOSUN CAN transmit failed; " +
             implementation->error_text(result));
  }
}

bool TosunCanAdapter::supports_batch_transmit() const noexcept {
  std::scoped_lock lock(state_mutex_);
  return impl_ &&
         (impl_->transmit_can_sequence || impl_->transmit_can_fd_sequence ||
          impl_->transmit_can_async || impl_->transmit_can_fd_async);
}

void TosunCanAdapter::send_batch(std::span<const CanFrame> frames) {
  if (frames.empty()) return;
  if (frames.size() >
      static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "TOSUN CAN transmit batch is too large");
  }

  std::scoped_lock transmit_lock(transmit_mutex_);
  Impl* implementation{};
  std::size_t device{};
  unsigned channel_index{};
  bool connected{};
  {
    std::scoped_lock lock(state_mutex_);
    if (status_.channel_started) {
      implementation = impl_.get();
      device = implementation ? implementation->device : 0;
      channel_index = implementation ? implementation->channel_index : 0;
      connected = implementation && implementation->device_connected;
    }
  }
  if (!implementation || !connected) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "TOSUN channel is not started");
  }

  const auto all_fd =
      std::all_of(frames.begin(), frames.end(),
                  [](const CanFrame& frame) { return frame.fd; });
  const auto all_classic =
      std::all_of(frames.begin(), frames.end(),
                  [](const CanFrame& frame) { return !frame.fd; });
  std::uint32_t result = kTscanSuccess;
  const auto batch_size = static_cast<std::int32_t>(frames.size());

  if (all_classic) {
    std::vector<TLIBCAN> outgoing;
    outgoing.reserve(frames.size());
    for (const auto& frame : frames) {
      if (frame.data.size() > 8) {
        fail(CanAdapterErrorCode::InvalidConfiguration,
             "TOSUN Classic CAN payload exceeds 8 bytes");
      }
      outgoing.push_back(make_classic_frame(frame, channel_index));
    }
    if (implementation->transmit_can_sequence) {
      // The controller serializes STmin=0 frames at the physical bus rate,
      // reproducing CANoe's wire cadence without a fixed host-side delay.
      result = implementation->transmit_can_sequence(
          device, outgoing.data(), batch_size);
    } else if (implementation->transmit_can_async) {
      for (const auto& frame : outgoing) {
        result = implementation->transmit_can_async(device, &frame);
        if (result != kTscanSuccess) break;
      }
    } else {
      for (const auto& frame : outgoing) {
        result = implementation->transmit_can(
            device, &frame, kTransmitTimeoutMs);
        if (result != kTscanSuccess) break;
      }
    }
  } else if (all_fd) {
    std::vector<TLIBCANFD> outgoing;
    outgoing.reserve(frames.size());
    for (const auto& frame : frames) {
      if (frame.data.size() > 64) {
        fail(CanAdapterErrorCode::InvalidConfiguration,
             "TOSUN CAN FD payload exceeds 64 bytes");
      }
      outgoing.push_back(make_fd_frame(frame, channel_index));
    }
    if (implementation->transmit_can_fd_sequence) {
      result = implementation->transmit_can_fd_sequence(
          device, outgoing.data(), batch_size);
    } else if (implementation->transmit_can_fd_async) {
      for (const auto& frame : outgoing) {
        result = implementation->transmit_can_fd_async(device, &frame);
        if (result != kTscanSuccess) break;
      }
    } else {
      for (const auto& frame : outgoing) {
        result = implementation->transmit_can_fd(
            device, &frame, kTransmitTimeoutMs);
        if (result != kTscanSuccess) break;
      }
    }
  } else {
    for (const auto& frame : frames) {
      if (frame.fd) {
        const auto outgoing = make_fd_frame(frame, channel_index);
        result = implementation->transmit_can_fd_async
                     ? implementation->transmit_can_fd_async(device, &outgoing)
                     : implementation->transmit_can_fd(
                           device, &outgoing, kTransmitTimeoutMs);
      } else {
        const auto outgoing = make_classic_frame(frame, channel_index);
        result = implementation->transmit_can_async
                     ? implementation->transmit_can_async(device, &outgoing)
                     : implementation->transmit_can(
                           device, &outgoing, kTransmitTimeoutMs);
      }
      if (result != kTscanSuccess) break;
    }
  }

  if (result != kTscanSuccess) {
    note_transport_failure();
    fail(CanAdapterErrorCode::VendorError,
         "TOSUN CAN batch transmit failed; " +
             implementation->error_text(result));
  }
}

std::optional<CanFrame> TosunCanAdapter::receive(
    std::chrono::milliseconds timeout) {
  Impl* implementation{};
  std::size_t device{};
  unsigned channel_index{};
  bool connected{};
  bool configured_fd{};
  {
    std::scoped_lock lock(state_mutex_);
    if (status_.channel_started) {
      implementation = impl_.get();
      device = implementation ? implementation->device : 0;
      channel_index = implementation ? implementation->channel_index : 0;
      connected = implementation && implementation->device_connected;
      configured_fd = implementation && implementation->configured_fd;
    }
  }
  if (!implementation || !connected) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "TOSUN channel is not started");
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::max(timeout, std::chrono::milliseconds::zero());
  do {
    if (configured_fd) {
      // On a CAN FD configured TSCAN channel this FIFO contains both Classic
      // CAN and CAN FD frames. Reading the Classic FIFO as well duplicates
      // Classic responses and can make a stale response satisfy the next UDS
      // request.
      TLIBCANFD incoming{};
      std::int32_t count = 1;
      const auto result = implementation->receive_can_fd(
          device, &incoming, &count, static_cast<u8>(channel_index),
          kReceiveOnly);
      if (result == kTscanSuccess && count > 0 &&
          !incoming.FProperties.bits.istx) {
        const auto fd = incoming.FFDProperties.bits.EDL != 0;
        const auto length =
            fd ? dlc_to_length(incoming.FDLC)
               : std::min<std::size_t>(incoming.FDLC, 8);
        return CanFrame{
            static_cast<std::uint32_t>(incoming.FIdentifier),
            {incoming.FData, incoming.FData + length},
            incoming.FProperties.bits.extframe != 0, fd,
            incoming.FFDProperties.bits.BRS != 0};
      }
    } else {
      TLIBCAN incoming{};
      std::int32_t count = 1;
      const auto result = implementation->receive_can(
          device, &incoming, &count, static_cast<u8>(channel_index),
          kReceiveOnly);
      if (result == kTscanSuccess && count > 0 &&
          !incoming.FProperties.bits.istx) {
        const auto length = std::min<std::size_t>(incoming.FDLC, 8);
        return CanFrame{
            static_cast<std::uint32_t>(incoming.FIdentifier),
            {incoming.FData, incoming.FData + length},
            incoming.FProperties.bits.extframe != 0, false, false};
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (true);
  return std::nullopt;
}

CanAdapterStatus TosunCanAdapter::status() const noexcept {
  std::scoped_lock lock(state_mutex_);
  return status_;
}

CanAdapterError TosunCanAdapter::last_error() const {
  std::scoped_lock lock(state_mutex_);
  return last_error_;
}

[[noreturn]] void TosunCanAdapter::fail(CanAdapterErrorCode code,
                                        std::string message) {
  remember_error(code, message);
  throw CanAdapterException({code, std::move(message)});
}

void TosunCanAdapter::remember_error(CanAdapterErrorCode code,
                                     std::string message) noexcept {
  try {
    std::scoped_lock lock(state_mutex_);
    last_error_ = {code, std::move(message)};
    status_.state = CanAdapterState::Error;
  } catch (...) {
  }
}

} // namespace uds
