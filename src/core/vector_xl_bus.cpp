#include "core/vector_xl_bus.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <thread>

namespace uds {
namespace {

#pragma pack(push, 8)
struct XlClassicCanMessage {
  unsigned id{};
  unsigned short flags{};
  unsigned short dlc{};
  unsigned long long reserved1{};
  unsigned char data[8]{};
  unsigned long long reserved2{};
};
union XlClassicEventData {
  XlClassicCanMessage message;
  unsigned char raw[32];
};
struct XlClassicEvent {
  unsigned char tag{};
  unsigned char channel_index{};
  unsigned short transaction_id{};
  unsigned short port_handle{};
  unsigned char flags{};
  unsigned char reserved{};
  unsigned long long timestamp{};
  XlClassicEventData data{};
};
#pragma pack(pop)

static_assert(sizeof(XlClassicCanMessage) == 32);
static_assert(sizeof(XlClassicEventData) == 32);
static_assert(offsetof(XlClassicEvent, data) == 16);
static_assert(sizeof(XlClassicEvent) == 48);

#pragma pack(push, 8)
struct XlCanFdConfig {
  unsigned arbitration_bitrate{};
  unsigned sjw_arbitration{};
  unsigned tseg1_arbitration{};
  unsigned tseg2_arbitration{};
  unsigned data_bitrate{};
  unsigned sjw_data{};
  unsigned tseg1_data{};
  unsigned tseg2_data{};
  unsigned char reserved{};
  unsigned char options{};
  unsigned char reserved1[2]{};
  unsigned reserved2{};
};
#pragma pack(pop)

constexpr unsigned kBusTypeCan = 1;
constexpr unsigned kInterfaceVersion = 3;
constexpr unsigned kInterfaceVersionFd = 4;
constexpr unsigned kActivateResetClock = 8;
constexpr unsigned char kClassicReceiveMessage = 1;
constexpr unsigned char kClassicTransmitMessage = 10;
constexpr unsigned short kClassicErrorFrame = 0x0001;
constexpr unsigned short kCanFdRxOk = 0x0400;
constexpr unsigned short kCanFdTxMessage = 0x0440;
constexpr unsigned kCanExtendedId = 0x80000000U;
constexpr unsigned kCanStandardIdMask = 0x7FFU;
constexpr unsigned kCanExtendedIdMask = 0x1FFFFFFFU;

/*
 * Classic CAN uses the legacy 48-byte XLevent ABI and its tag values are
 * XL_RECEIVE_MSG (1) / XL_TRANSMIT_MSG (10).  The 0x04xx tags below belong
 * only to the CAN FD xlCanReceive/xlCanTransmitEx API.
 */

struct XlCanFdRxMessage {
  unsigned can_id{};
  unsigned flags{};
  unsigned crc{};
  unsigned char reserved1[12]{};
  unsigned short total_bit_count{};
  unsigned char dlc{};
  unsigned char reserved[5]{};
  unsigned char data[64]{};
};
union XlCanFdRxData { XlCanFdRxMessage message; unsigned char raw[96]; };
struct XlCanFdRxEvent {
  int size{};
  unsigned short tag{};
  unsigned char channel_index{};
  unsigned char reserved{};
  int user_handle{};
  unsigned short chip_flags{};
  unsigned short reserved0{};
  unsigned long long reserved1{};
  unsigned long long timestamp{};
  XlCanFdRxData data{};
};
struct XlCanFdTxMessage {
  unsigned can_id{};
  unsigned flags{};
  unsigned char dlc{};
  unsigned char reserved[7]{};
  unsigned char data[64]{};
};
union XlCanFdTxData { XlCanFdTxMessage message; unsigned char raw[80]; };
struct XlCanFdTxEvent {
  unsigned short tag{};
  unsigned short transaction_id{};
  unsigned char channel_index{};
  unsigned char reserved[3]{};
  XlCanFdTxData data{};
};
static_assert(sizeof(XlCanFdConfig) == 40);
static_assert(sizeof(XlCanFdRxEvent) == 128);
static_assert(sizeof(XlCanFdTxEvent) == 88);

unsigned char length_to_dlc(std::size_t length) {
  if (length <= 8) return static_cast<unsigned char>(length);
  if (length <= 12) return 9; if (length <= 16) return 10; if (length <= 20) return 11;
  if (length <= 24) return 12; if (length <= 32) return 13; if (length <= 48) return 14;
  return 15;
}
std::size_t dlc_to_length(unsigned char dlc) {
  constexpr std::array<std::size_t,16> lengths{0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64};
  return lengths[std::min<unsigned>(dlc,15)];
}

std::string narrow(const std::wstring& value) {
  if (value.empty()) return {};
  const auto count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(count), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), count,
                      nullptr, nullptr);
  return out;
}

} // namespace

VectorXlBus::VectorXlBus(VectorBusConfig config) : config_(std::move(config)) {}
VectorXlBus::~VectorXlBus() { close(); }

void VectorXlBus::load_api() {
  if (dll_) return;
  dll_ = LoadLibraryW(L"vxlapi64.dll");
  if (!dll_) throw std::runtime_error("vxlapi64.dll not found; install Vector Driver");
  const auto proc = [this](const char* name) -> FARPROC {
    auto p = GetProcAddress(dll_, name);
    if (!p) throw std::runtime_error(std::string("Vector API missing: ") + name);
    return p;
  };
  xl_open_driver_ = reinterpret_cast<FnOpenDriver>(proc("xlOpenDriver"));
  xl_close_driver_ = reinterpret_cast<FnCloseDriver>(proc("xlCloseDriver"));
  xl_open_port_ = reinterpret_cast<FnOpenPort>(proc("xlOpenPort"));
  xl_close_port_ = reinterpret_cast<FnClosePort>(proc("xlClosePort"));
  xl_activate_ = reinterpret_cast<FnActivate>(proc("xlActivateChannel"));
  xl_deactivate_ = reinterpret_cast<FnDeactivate>(proc("xlDeactivateChannel"));
  xl_set_bitrate_ = reinterpret_cast<FnBitrate>(GetProcAddress(dll_, "xlCanSetChannelBitrate"));
  xl_transmit_ = reinterpret_cast<FnTransmit>(proc("xlCanTransmit"));
  xl_receive_ = reinterpret_cast<FnReceive>(proc("xlReceive"));
  xl_fd_config_ = reinterpret_cast<FnFdConfig>(GetProcAddress(dll_, "xlCanFdSetConfiguration"));
  xl_transmit_ex_ = reinterpret_cast<FnTransmitEx>(GetProcAddress(dll_, "xlCanTransmitEx"));
  xl_can_receive_ = reinterpret_cast<FnCanReceive>(GetProcAddress(dll_, "xlCanReceive"));
}

void VectorXlBus::check(int status, const char* api) const {
  if (status != 0) throw std::runtime_error(std::string(api) + " failed, XL status=" + std::to_string(status));
}

void VectorXlBus::open() {
  std::scoped_lock lock(mutex_);
  if (port_ >= 0) return;
  load_api();
  check(xl_open_driver_(), "xlOpenDriver");
  driver_open_ = true;
  access_mask_ = 1ULL << (std::max(1U, config_.channel) - 1U);
  auto permission = access_mask_;
  const auto app = narrow(config_.application_name);
  try {
    const auto interface_version = config_.fd ? kInterfaceVersionFd : kInterfaceVersion;
    check(xl_open_port_(&port_, app.c_str(), access_mask_, &permission, 262144,
                        interface_version, kBusTypeCan), "xlOpenPort");
    if (config_.fd && (!xl_fd_config_ || !xl_transmit_ex_ || !xl_can_receive_)) {
      throw std::runtime_error("Vector CAN FD API is unavailable");
    }
    if (config_.fd && (permission & access_mask_) != 0) {
      XlCanFdConfig timing{};
      timing.arbitration_bitrate = config_.bitrate;
      timing.sjw_arbitration = 16; timing.tseg1_arbitration = 63; timing.tseg2_arbitration = 16;
      timing.data_bitrate = config_.data_bitrate;
      timing.sjw_data = 4; timing.tseg1_data = 15; timing.tseg2_data = 4;
      check(xl_fd_config_(port_, access_mask_, &timing), "xlCanFdSetConfiguration");
    } else if (!config_.fd && xl_set_bitrate_ && (permission & access_mask_) != 0) {
      check(xl_set_bitrate_(port_, access_mask_, config_.bitrate), "xlCanSetChannelBitrate");
    }
    check(xl_activate_(port_, access_mask_, kBusTypeCan, kActivateResetClock), "xlActivateChannel");
  } catch (...) {
    if (port_ >= 0) xl_close_port_(port_);
    port_ = -1;
    xl_close_driver_();
    driver_open_ = false;
    throw;
  }
}

void VectorXlBus::close() noexcept {
  std::scoped_lock lock(mutex_);
  if (port_ >= 0) {
    if (xl_deactivate_) xl_deactivate_(port_, access_mask_);
    if (xl_close_port_) xl_close_port_(port_);
    port_ = -1;
  }
  if (driver_open_ && xl_close_driver_) xl_close_driver_();
  driver_open_ = false;
  if (dll_) FreeLibrary(dll_);
  dll_ = nullptr;
}

bool VectorXlBus::is_open() const noexcept {
  std::scoped_lock lock(mutex_);
  return port_ >= 0;
}

void VectorXlBus::send(const CanFrame& frame) {
  if (frame.data.size() > 64) {
    throw std::invalid_argument("CAN payload exceeds 64 bytes");
  }
  if ((!frame.extended && frame.id > kCanStandardIdMask) ||
      (frame.extended && frame.id > kCanExtendedIdMask)) {
    throw std::invalid_argument("CAN identifier is outside its configured range");
  }
  open();
  std::scoped_lock lock(mutex_);
  if (config_.fd) {
    XlCanFdTxEvent event{};
    event.tag = kCanFdTxMessage;
    event.transaction_id = 0xFFFF;
    event.channel_index = static_cast<unsigned char>(config_.channel - 1U);
    event.data.message.can_id =
        frame.id | (frame.extended ? kCanExtendedId : 0U);
    event.data.message.flags = (frame.fd ? 1U : 0U) | (frame.brs ? 2U : 0U);
    event.data.message.dlc = length_to_dlc(frame.data.size());
    std::copy(frame.data.begin(), frame.data.end(), event.data.message.data);
    unsigned sent = 0;
    check(xl_transmit_ex_(port_, access_mask_, 1, &sent, &event), "xlCanTransmitEx");
    if (sent != 1) throw std::runtime_error("Vector transmitted zero frames");
    return;
  }
  if (frame.data.size() > 8) throw std::invalid_argument("Classic CAN payload exceeds 8 bytes");
  XlClassicEvent event{};
  event.tag = kClassicTransmitMessage;
  event.data.message.id = frame.id | (frame.extended ? kCanExtendedId : 0U);
  event.data.message.dlc = static_cast<unsigned short>(frame.data.size());
  std::copy(frame.data.begin(), frame.data.end(), event.data.message.data);
  unsigned count = 1;
  check(xl_transmit_(port_, access_mask_, &count, &event), "xlCanTransmit");
  if (count != 1) throw std::runtime_error("Vector transmitted zero frames");
}

std::optional<CanFrame> VectorXlBus::receive(std::chrono::milliseconds timeout) {
  open();
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    int status{};
    if (config_.fd) {
      XlCanFdRxEvent event{};
      {
        std::scoped_lock lock(mutex_);
        status = xl_can_receive_(port_, &event);
      }
      if (status == 0 && event.tag == kCanFdRxOk) {
        const auto length = dlc_to_length(event.data.message.dlc);
        const bool fd = (event.data.message.flags & 1U) != 0;
        const bool brs = (event.data.message.flags & 2U) != 0;
        const bool extended =
            (event.data.message.can_id & kCanExtendedId) != 0;
        const auto id = event.data.message.can_id &
                        (extended ? kCanExtendedIdMask : kCanStandardIdMask);
        return CanFrame{id,
                        {event.data.message.data,
                         event.data.message.data + length},
                        extended, fd, brs};
      }
      if (status == 0) continue;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    XlClassicEvent event{};
    unsigned count = 1;
    {
      std::scoped_lock lock(mutex_);
      status = xl_receive_(port_, &count, &event);
    }
    if (status == 0 && event.tag == kClassicReceiveMessage &&
        (event.data.message.flags & kClassicErrorFrame) == 0) {
      const auto dlc = std::min<unsigned>(event.data.message.dlc, 8);
      const bool extended =
          (event.data.message.id & kCanExtendedId) != 0;
      const auto id = event.data.message.id &
                      (extended ? kCanExtendedIdMask : kCanStandardIdMask);
      return CanFrame{id,
                      {event.data.message.data,
                       event.data.message.data + dlc},
                      extended, false, false};
    }
    if (status == 0) continue;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return std::nullopt;
}

} // namespace uds
