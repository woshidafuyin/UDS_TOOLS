#pragma once

#include "core/flash_data.hpp"
#include "core/uds_client.hpp"

#include <ctime>
#include <functional>
#include <string>

namespace uds {
enum class PeroduaEndpoint { ecu, gateway, functional };
enum class PeroduaMode { app, cal, app_cal };

struct PeroduaMemoryBlock {
  std::string name;
  std::uint32_t address{};
  std::uint32_t length{};
  SRecordImage image;
};
struct PeroduaImages {
  PeroduaMemoryBlock driver;
  std::vector<PeroduaMemoryBlock> modules;
};

// The flow owns UDS semantics; the workflow owns hardware, transport and keys.
// Tests inject a protocol peer here without using physical hardware or sleeps.
struct PeroduaIo {
  std::function<UdsResponse(PeroduaEndpoint, std::span<const std::uint8_t>,
      std::chrono::milliseconds, std::chrono::milliseconds)> request;
  std::function<void(std::span<const std::uint8_t>)> functional_send;
  std::function<void(std::chrono::milliseconds)> wait;
  std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t>)> key;
  std::function<void(int, const std::string&)> progress;
  std::function<void()> health_check;
};

PeroduaMode perodua_mode(std::wstring_view entry);
std::vector<std::uint8_t> perodua_fingerprint(const std::tm& date,
                                             std::wstring_view tester_identity);
std::uint32_t perodua_crc32(std::span<const std::uint8_t> bytes, bool reflected = true);
std::size_t perodua_max_block_length(std::span<const std::uint8_t> response);
void validate_perodua_images(const PeroduaImages& images);
SRecordImage load_perodua_image(const std::filesystem::path& file,
                                std::uint32_t binary_address);

class PeroduaP02cFlow {
public:
  explicit PeroduaP02cFlow(PeroduaIo io, bool crc_reflected = true);
  void run(const PeroduaImages& images,
           std::span<const std::uint8_t> fingerprint,
           std::stop_token stop = {});
  bool programming_completed() const noexcept { return programming_completed_; }
private:
  void check() const;
  void progress(int percent, const std::string& message) const;
  std::vector<std::uint8_t> exchange(PeroduaEndpoint endpoint,
      const std::vector<std::uint8_t>& request,
      const std::vector<std::uint8_t>& prefix, const std::string& label,
      bool retry_timeout = false);
  void functional(std::initializer_list<std::uint8_t> request);
  void routine(const std::vector<std::uint8_t>& request, const std::string& label);
  void transfer(const SRecordSegment& segment, const std::string& label,
                int begin, int end);
  PeroduaIo io_;
  std::stop_token stop_;
  bool programming_completed_{};
  bool crc_reflected_{};
};
} // namespace uds
