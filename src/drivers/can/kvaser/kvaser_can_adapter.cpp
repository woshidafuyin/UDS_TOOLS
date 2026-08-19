#include "drivers/can/kvaser/kvaser_can_adapter.hpp"
#include "drivers/can/kvaser/kvaser_channel_catalog.hpp"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4828)
#endif
#include <canlib.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace uds {
namespace {

// A busy physical bus can briefly defer CANlib's transmit completion while the
// Xizhong NM keepalive and diagnostic traffic share one channel. 100 ms was
// shorter than the observed bench gap and caused canERR_TIMEOUT even though the
// channel recovered immediately. Keep one queued transmission and wait longer
// instead of retrying the same UDS request and risking a duplicate service.
constexpr unsigned long kTransmitCompletionTimeoutMs = 500;

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
      L"UDS_KVASER_DRIVER_DIR", configured.data(),
      static_cast<DWORD>(configured.size()));
  if (configured_length > 0 && configured_length < configured.size()) {
    candidates.emplace_back(std::filesystem::path(configured.data()) /
                            L"canlib32.dll");
  }
  const auto executable = executable_directory();
  if (!executable.empty()) {
    candidates.emplace_back(executable / L"drivers" / L"kvaser" /
                            L"canlib32.dll");
    candidates.emplace_back(executable / L"canlib32.dll");
  }
  candidates.emplace_back(
      std::filesystem::path(L"C:\\Windows\\System32\\canlib32.dll"));
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

long classic_bitrate(unsigned bitrate) {
  switch (bitrate) {
  case 1000000:
    return canBITRATE_1M;
  case 500000:
    return canBITRATE_500K;
  case 250000:
    return canBITRATE_250K;
  case 125000:
    return canBITRATE_125K;
  case 100000:
    return canBITRATE_100K;
  case 50000:
    return canBITRATE_50K;
  case 10000:
    return canBITRATE_10K;
  default:
    return static_cast<long>(bitrate);
  }
}

long fd_bitrate(unsigned bitrate) {
  switch (bitrate) {
  case 500000:
    return canFD_BITRATE_500K_80P;
  case 1000000:
    return canFD_BITRATE_1M_80P;
  case 2000000:
    return canFD_BITRATE_2M_80P;
  case 4000000:
    return canFD_BITRATE_4M_80P;
  case 8000000:
    return canFD_BITRATE_8M_80P;
  default:
    return static_cast<long>(bitrate);
  }
}

} // namespace

struct KvaserCanAdapter::Impl {
  using FnInitialize = void(CANLIBAPI*)();
  using FnGetNumberOfChannels = canStatus(CANLIBAPI*)(int*);
  using FnGetChannelData =
      canStatus(CANLIBAPI*)(int, int, void*, std::size_t);
  using FnOpenChannel = CanHandle(CANLIBAPI*)(int, int);
  using FnSetBusParams = canStatus(CANLIBAPI*)(
      CanHandle, long, unsigned, unsigned, unsigned, unsigned, unsigned);
  using FnSetBusParamsFd =
      canStatus(CANLIBAPI*)(CanHandle, long, unsigned, unsigned, unsigned);
  using FnBusOn = canStatus(CANLIBAPI*)(CanHandle);
  using FnBusOff = canStatus(CANLIBAPI*)(CanHandle);
  using FnClose = canStatus(CANLIBAPI*)(CanHandle);
  using FnWriteWait = canStatus(CANLIBAPI*)(
      CanHandle, long, void*, unsigned, unsigned, unsigned long);
  using FnReadWait = canStatus(CANLIBAPI*)(
      CanHandle, long*, void*, unsigned*, unsigned*, unsigned long*,
      unsigned long);
  using FnGetErrorText =
      canStatus(CANLIBAPI*)(canStatus, char*, unsigned);

  HMODULE library{};
  CanHandle handle{canINVALID_HANDLE};
  bool configured_fd{};
  std::vector<detail::KvaserChannelCatalogEntry> channels;
  std::string configured_channel_label;

  FnInitialize initialize{};
  FnGetNumberOfChannels get_number_of_channels{};
  FnGetChannelData get_channel_data{};
  FnOpenChannel open_channel{};
  FnSetBusParams set_bus_params{};
  FnSetBusParamsFd set_bus_params_fd{};
  FnBusOn bus_on{};
  FnBusOff bus_off{};
  FnClose close{};
  FnWriteWait write_wait{};
  FnReadWait read_wait{};
  FnGetErrorText get_error_text{};

  std::string error_text(canStatus code) const {
    std::array<char, 256> text{};
    if (get_error_text &&
        get_error_text(code, text.data(),
                       static_cast<unsigned>(text.size())) == canOK &&
        text.front() != '\0') {
      return text.data();
    }
    return "status=" + std::to_string(code);
  }

  template <typename Value>
  bool channel_value(int api_index, int item, Value& value) const {
    value = {};
    return get_channel_data &&
           get_channel_data(api_index, item, &value, sizeof(value)) >= canOK;
  }

  std::string channel_text(int api_index, int item) const {
    std::array<char, 256> value{};
    if (!get_channel_data ||
        get_channel_data(api_index, item, value.data(), value.size()) < canOK) {
      return {};
    }
    return value.data();
  }

  canStatus refresh_channels() {
    int channel_count{};
    const auto result = get_number_of_channels(&channel_count);
    if (result < canOK) return result;

    std::vector<detail::KvaserChannelCatalogEntry> discovered;
    discovered.reserve(static_cast<std::size_t>(std::max(0, channel_count)));
    for (int api_index = 0; api_index < channel_count; ++api_index) {
      detail::KvaserChannelCatalogEntry channel;
      channel.api_index = api_index;
      channel_value(api_index, canCHANNELDATA_CARD_TYPE, channel.card_type);
      channel_value(api_index, canCHANNELDATA_CHANNEL_CAP,
                    channel.capabilities);
      channel_value(api_index, canCHANNELDATA_CHAN_NO_ON_CARD,
                    channel.channel_on_card);
      channel_value(api_index, canCHANNELDATA_CARD_SERIAL_NO,
                    channel.serial_number);
      channel_value(api_index, canCHANNELDATA_CARD_UPC_NO,
                    channel.product_ean);
      channel.device_description =
          channel_text(api_index, canCHANNELDATA_DEVDESCR_ASCII);
      channel.channel_name =
          channel_text(api_index, canCHANNELDATA_CHANNEL_NAME);
      channel.virtual_channel =
          channel.card_type == canHWTYPE_VIRTUAL ||
          (channel.capabilities & canCHANNEL_CAP_VIRTUAL) != 0;
      discovered.push_back(std::move(channel));
    }
    detail::order_kvaser_channels(discovered);
    channels = std::move(discovered);
    return canOK;
  }
};

KvaserCanAdapter::KvaserCanAdapter() = default;
KvaserCanAdapter::~KvaserCanAdapter() { release(); }

CanVendor KvaserCanAdapter::vendor() const noexcept {
  return CanVendor::Kvaser;
}

void KvaserCanAdapter::initialize() {
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
         "Kvaser canlib32.dll was not found or could not be loaded; searched: " +
             library_search_detail(candidates));
  }

  const auto required = [&implementation](const char* name) -> FARPROC {
    const auto function = GetProcAddress(implementation->library, name);
    if (!function) {
      throw std::runtime_error(std::string("Kvaser API is missing: ") + name);
    }
    return function;
  };
  try {
    implementation->initialize =
        reinterpret_cast<Impl::FnInitialize>(
            required("canInitializeLibrary"));
    implementation->get_number_of_channels =
        reinterpret_cast<Impl::FnGetNumberOfChannels>(
            required("canGetNumberOfChannels"));
    implementation->get_channel_data =
        reinterpret_cast<Impl::FnGetChannelData>(
            required("canGetChannelData"));
    implementation->open_channel =
        reinterpret_cast<Impl::FnOpenChannel>(required("canOpenChannel"));
    implementation->set_bus_params =
        reinterpret_cast<Impl::FnSetBusParams>(
            required("canSetBusParams"));
    implementation->set_bus_params_fd =
        reinterpret_cast<Impl::FnSetBusParamsFd>(
            required("canSetBusParamsFd"));
    implementation->bus_on =
        reinterpret_cast<Impl::FnBusOn>(required("canBusOn"));
    implementation->bus_off =
        reinterpret_cast<Impl::FnBusOff>(required("canBusOff"));
    implementation->close =
        reinterpret_cast<Impl::FnClose>(required("canClose"));
    implementation->write_wait =
        reinterpret_cast<Impl::FnWriteWait>(required("canWriteWait"));
    implementation->read_wait =
        reinterpret_cast<Impl::FnReadWait>(required("canReadWait"));
    implementation->get_error_text =
        reinterpret_cast<Impl::FnGetErrorText>(required("canGetErrorText"));
  } catch (const std::exception& error) {
    FreeLibrary(implementation->library);
    implementation->library = nullptr;
    fail(CanAdapterErrorCode::DriverMissing, error.what());
  }

  implementation->initialize();
  const auto count_status = implementation->refresh_channels();
  if (count_status < canOK) {
    const auto detail = implementation->error_text(count_status);
    FreeLibrary(implementation->library);
    implementation->library = nullptr;
    fail(CanAdapterErrorCode::VendorError,
         "Kvaser channel enumeration failed: " + detail);
  }
  std::scoped_lock lock(state_mutex_);
  impl_ = std::move(implementation);
  status_ = {CanVendor::Kvaser, CanAdapterState::Ready,
             true, false, false};
  last_error_ = {};
}

void KvaserCanAdapter::release() noexcept {
  close_device();
  std::unique_ptr<Impl> implementation;
  {
    std::scoped_lock lock(state_mutex_);
    implementation = std::move(impl_);
    status_ = {CanVendor::Kvaser, CanAdapterState::Uninitialized,
               false, false, false};
  }
  if (implementation && implementation->library) {
    FreeLibrary(implementation->library);
  }
}

std::vector<CanDeviceInfo> KvaserCanAdapter::enumerate_devices() {
  initialize();
  Impl* implementation{};
  {
    std::scoped_lock lock(state_mutex_);
    implementation = impl_.get();
  }
  if (!implementation || implementation->channels.empty()) return {};

  std::ostringstream names;
  for (std::size_t index = 0; index < implementation->channels.size();
       ++index) {
    const auto& channel = implementation->channels[index];
    if (names.tellp() > 0) names << "; ";
    const auto& name = channel.device_description.empty()
                           ? channel.channel_name
                           : channel.device_description;
    names << "CH" << (index + 1) << '='
          << (name.empty() ? "Unnamed Kvaser channel" : name)
          << " {kind=" << (channel.virtual_channel ? "VIRTUAL" : "PHYSICAL")
          << ", api=" << channel.api_index
          << ", device_ch=" << (channel.channel_on_card + 1);
    if (!channel.virtual_channel) {
      names << ", serial=" << channel.serial_number << ", ean=0x" << std::hex
            << std::uppercase << channel.product_ean << std::dec;
    }
    names << '}';
  }
  auto display = std::string("Kvaser CANlib");
  if (names.tellp() > 0) display += " [" + names.str() + "]";
  return {{CanVendor::Kvaser, "kvaser:all", std::move(display),
           static_cast<unsigned>(implementation->channels.size())}};
}

void KvaserCanAdapter::open_device(std::string_view device_id) {
  initialize();
  if (!device_id.empty() && device_id != "kvaser:all") {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "Kvaser device id must be kvaser:all");
  }
  std::scoped_lock lock(state_mutex_);
  status_.device_open = true;
  status_.state = CanAdapterState::DeviceOpen;
  last_error_ = {};
}

void KvaserCanAdapter::close_device() noexcept {
  stop_channel();
  Impl* implementation{};
  CanHandle handle{canINVALID_HANDLE};
  {
    std::scoped_lock lock(state_mutex_);
    implementation = impl_.get();
    if (implementation) {
      handle = implementation->handle;
      implementation->handle = canINVALID_HANDLE;
    }
    status_.device_open = false;
    status_.channel_started = false;
    if (status_.initialized) status_.state = CanAdapterState::Ready;
  }
  if (implementation && handle >= 0) implementation->close(handle);
}

void KvaserCanAdapter::configure_channel(const CanChannelConfig& config) {
  Impl* implementation{};
  {
    std::scoped_lock lock(state_mutex_);
    implementation = impl_.get();
    if (!status_.device_open) implementation = nullptr;
  }
  if (!implementation) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "Kvaser CANlib must be opened before channel configuration");
  }
  if (config.channel == 0 ||
      config.channel > static_cast<unsigned>(implementation->channels.size()) ||
      config.nominal_bitrate == 0 ||
      (config.can_fd && config.data_bitrate == 0)) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "Kvaser CAN channel configuration is invalid");
  }

  const auto& selected = implementation->channels[config.channel - 1];
  if (config.can_fd &&
      (selected.capabilities & canCHANNEL_CAP_CAN_FD) == 0) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "Kvaser logical CH" + std::to_string(config.channel) +
             " does not report ISO CAN FD support");
  }

  auto flags = config.can_fd ? canOPEN_CAN_FD : 0;
  if (selected.virtual_channel) flags |= canOPEN_ACCEPT_VIRTUAL;
  const auto handle = implementation->open_channel(
      selected.api_index, flags);
  if (handle < 0) {
    fail(CanAdapterErrorCode::VendorError,
         "Kvaser canOpenChannel failed for logical CH" +
             std::to_string(config.channel) + " (CANlib api=" +
             std::to_string(selected.api_index) + "): " +
             implementation->error_text(
                 static_cast<canStatus>(handle)));
  }
  // CANlib has distinct preset timing constants for the arbitration phase of
  // CAN FD. Using the Classic-CAN 500 kbit/s preset opened the channel but gave
  // a different sample point; on the physical Xizhong bench that accumulated
  // error frames and eventually made the first post-10 02 request time out.
  const auto nominal_bitrate =
      config.can_fd ? fd_bitrate(config.nominal_bitrate)
                    : classic_bitrate(config.nominal_bitrate);
  const auto nominal_status = implementation->set_bus_params(
      handle, nominal_bitrate, 0, 0, 0, 0, 0);
  if (nominal_status < canOK) {
    const auto detail = implementation->error_text(nominal_status);
    implementation->close(handle);
    fail(CanAdapterErrorCode::VendorError,
         "Kvaser nominal bitrate configuration failed: " + detail);
  }
  if (config.can_fd) {
    const auto data_status = implementation->set_bus_params_fd(
        handle, fd_bitrate(config.data_bitrate), 0, 0, 0);
    if (data_status < canOK) {
      const auto detail = implementation->error_text(data_status);
      implementation->close(handle);
      fail(CanAdapterErrorCode::VendorError,
           "Kvaser data bitrate configuration failed: " + detail);
    }
  }

  std::scoped_lock lock(state_mutex_);
  implementation->handle = handle;
  implementation->configured_fd = config.can_fd;
  implementation->configured_channel_label =
      "logical CH" + std::to_string(config.channel) + " (CANlib api=" +
      std::to_string(selected.api_index) + ", device CH" +
      std::to_string(selected.channel_on_card + 1) + ", " +
      (selected.virtual_channel ? "virtual" : "physical") + ')';
  status_.state = CanAdapterState::ChannelConfigured;
  status_.channel_started = false;
  last_error_ = {};
}

void KvaserCanAdapter::start_channel() {
  Impl* implementation{};
  CanHandle handle{canINVALID_HANDLE};
  {
    std::scoped_lock lock(state_mutex_);
    if (status_.channel_started) return;
    implementation = impl_.get();
    handle = implementation ? implementation->handle : canINVALID_HANDLE;
  }
  if (!implementation || handle < 0) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "Kvaser channel must be configured before start");
  }
  const auto result = implementation->bus_on(handle);
  if (result < canOK) {
    fail(CanAdapterErrorCode::VendorError,
         "Kvaser canBusOn failed: " +
             implementation->error_text(result));
  }
  std::scoped_lock lock(state_mutex_);
  status_.state = CanAdapterState::ChannelStarted;
  status_.channel_started = true;
  last_error_ = {};
}

void KvaserCanAdapter::stop_channel() noexcept {
  Impl* implementation{};
  CanHandle handle{canINVALID_HANDLE};
  {
    std::scoped_lock lock(state_mutex_);
    implementation = impl_.get();
    handle = implementation ? implementation->handle : canINVALID_HANDLE;
    status_.channel_started = false;
    if (status_.device_open && handle >= 0) {
      status_.state = CanAdapterState::ChannelConfigured;
    }
  }
  if (implementation && handle >= 0) implementation->bus_off(handle);
}

void KvaserCanAdapter::send(const CanFrame& frame) {
  Impl* implementation{};
  CanHandle handle{canINVALID_HANDLE};
  {
    std::scoped_lock lock(state_mutex_);
    if (status_.channel_started) {
      implementation = impl_.get();
      handle = implementation ? implementation->handle : canINVALID_HANDLE;
    }
  }
  if (!implementation || handle < 0) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "Kvaser channel is not started");
  }
  if (frame.data.size() > (frame.fd ? 64U : 8U)) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "Kvaser CAN payload length is invalid");
  }
  auto flags = frame.extended ? canMSG_EXT : canMSG_STD;
  if (frame.fd) {
    flags |= canFDMSG_FDF;
    if (frame.brs) flags |= canFDMSG_BRS;
  }
  std::array<std::uint8_t, 64> data{};
  std::copy(frame.data.begin(), frame.data.end(), data.begin());
  const auto result = implementation->write_wait(
      handle, static_cast<long>(frame.id), data.data(),
      static_cast<unsigned>(frame.data.size()),
      static_cast<unsigned>(flags), kTransmitCompletionTimeoutMs);
  if (result < canOK) {
    fail(CanAdapterErrorCode::VendorError,
         "Kvaser CAN transmit failed: " +
             implementation->error_text(result));
  }
}

std::optional<CanFrame> KvaserCanAdapter::receive(
    std::chrono::milliseconds timeout) {
  Impl* implementation{};
  CanHandle handle{canINVALID_HANDLE};
  {
    std::scoped_lock lock(state_mutex_);
    if (status_.channel_started) {
      implementation = impl_.get();
      handle = implementation ? implementation->handle : canINVALID_HANDLE;
    }
  }
  if (!implementation || handle < 0) {
    fail(CanAdapterErrorCode::InvalidConfiguration,
         "Kvaser channel is not started");
  }

  long id{};
  std::array<std::uint8_t, 64> data{};
  unsigned dlc{};
  unsigned flags{};
  unsigned long timestamp{};
  const auto timeout_count = std::clamp<long long>(
      timeout.count(), 0, std::numeric_limits<unsigned long>::max());
  const auto result = implementation->read_wait(
      handle, &id, data.data(), &dlc, &flags, &timestamp,
      static_cast<unsigned long>(timeout_count));
  if (result == canERR_NOMSG || result == canERR_TIMEOUT) {
    return std::nullopt;
  }
  if (result < canOK) {
    fail(CanAdapterErrorCode::VendorError,
         "Kvaser CAN receive failed: " +
             implementation->error_text(result));
  }
  const auto fd = (flags & canFDMSG_FDF) != 0;
  const auto length = std::min<std::size_t>(dlc, fd ? 64U : 8U);
  return CanFrame{static_cast<std::uint32_t>(id),
                  std::vector<std::uint8_t>(
                      data.begin(), data.begin() + length),
                  (flags & canMSG_EXT) != 0,
                  fd,
                  (flags & canFDMSG_BRS) != 0};
}

CanAdapterStatus KvaserCanAdapter::status() const noexcept {
  std::scoped_lock lock(state_mutex_);
  return status_;
}

CanAdapterError KvaserCanAdapter::last_error() const {
  std::scoped_lock lock(state_mutex_);
  return last_error_;
}

[[noreturn]] void KvaserCanAdapter::fail(CanAdapterErrorCode code,
                                         std::string message) {
  remember_error(code, message);
  throw CanAdapterException({code, std::move(message)});
}

void KvaserCanAdapter::remember_error(CanAdapterErrorCode code,
                                      std::string message) noexcept {
  try {
    std::scoped_lock lock(state_mutex_);
    last_error_ = {code, std::move(message)};
    status_.state = CanAdapterState::Error;
  } catch (...) {
  }
}

} // namespace uds
