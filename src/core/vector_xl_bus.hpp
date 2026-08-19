#pragma once

#include "core/can_bus.hpp"

#include <Windows.h>
#include <mutex>
#include <string>

namespace uds {

struct VectorBusConfig {
  unsigned channel{2};              // 1-based physical channel
  unsigned bitrate{500000};
  std::wstring application_name{L"UDSToolCpp"};
  bool fd{true};
  unsigned data_bitrate{2000000};
};

class VectorXlBus final : public ICanBus {
public:
  explicit VectorXlBus(VectorBusConfig config);
  ~VectorXlBus() override;
  VectorXlBus(const VectorXlBus&) = delete;
  VectorXlBus& operator=(const VectorXlBus&) = delete;

  void open() override;
  void close() noexcept override;
  bool is_open() const noexcept override;
  void send(const CanFrame& frame) override;
  std::optional<CanFrame> receive(std::chrono::milliseconds timeout) override;

private:
  void check(int status, const char* api) const;
  void load_api();

  VectorBusConfig config_;
  HMODULE dll_{};
  long port_{-1};
  unsigned long long access_mask_{};
  bool driver_open_{};
  mutable std::mutex mutex_;

  using FnOpenDriver = int(__stdcall*)();
  using FnCloseDriver = int(__stdcall*)();
  using FnOpenPort = int(__stdcall*)(long*, const char*, unsigned long long,
                                     unsigned long long*, unsigned, unsigned, unsigned);
  using FnClosePort = int(__stdcall*)(long);
  using FnActivate = int(__stdcall*)(long, unsigned long long, unsigned, unsigned);
  using FnDeactivate = int(__stdcall*)(long, unsigned long long);
  using FnBitrate = int(__stdcall*)(long, unsigned long long, unsigned long);
  using FnTransmit = int(__stdcall*)(long, unsigned long long, unsigned*, void*);
  using FnReceive = int(__stdcall*)(long, unsigned*, void*);
  using FnFdConfig = int(__stdcall*)(long, unsigned long long, void*);
  using FnTransmitEx = int(__stdcall*)(long, unsigned long long, unsigned, unsigned*, void*);
  using FnCanReceive = int(__stdcall*)(long, void*);

  FnOpenDriver xl_open_driver_{};
  FnCloseDriver xl_close_driver_{};
  FnOpenPort xl_open_port_{};
  FnClosePort xl_close_port_{};
  FnActivate xl_activate_{};
  FnDeactivate xl_deactivate_{};
  FnBitrate xl_set_bitrate_{};
  FnTransmit xl_transmit_{};
  FnReceive xl_receive_{};
  FnFdConfig xl_fd_config_{};
  FnTransmitEx xl_transmit_ex_{};
  FnCanReceive xl_can_receive_{};
};

} // namespace uds
