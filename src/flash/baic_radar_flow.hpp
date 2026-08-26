#pragma once

#include "core/uds_client.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace uds {

struct BaicRadarProtocol {
  std::string project_name;
  std::uint32_t driver_start{};
  std::uint32_t driver_length{};
  std::uint32_t app_start{};
  std::uint32_t app_length{};
  std::uint8_t security_seed_subfunction{0x11};
  std::size_t security_seed_length{4};
  std::size_t security_key_length{16};
};

struct BaicRadarImages {
  std::vector<std::uint8_t> driver;
  std::vector<std::uint8_t> app;
};

std::vector<std::uint8_t> baic_radar_request_download(
    std::uint32_t address, std::uint32_t length);
std::vector<std::uint8_t> baic_radar_erase_memory(
    std::uint32_t address, std::uint32_t length);
std::size_t baic_radar_max_block_length(
    std::span<const std::uint8_t> response);
std::uint32_t baic_radar_crc32(std::span<const std::uint8_t> data);

class BaicRadarFlow {
public:
  using Log = std::function<void(int, const std::string&)>;
  using KeyGenerator =
      std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t>)>;

  BaicRadarFlow(UdsClient& physical, UdsClient& functional,
                BaicRadarProtocol protocol, Log log,
                KeyGenerator key_generator);
  void run(const BaicRadarImages& images, std::stop_token stop = {});

private:
  UdsResponse expect(UdsClient& client,
                     std::span<const std::uint8_t> request,
                     std::span<const std::uint8_t> expected, int percent,
                     const std::string& name, bool exact = false);
  void send_cleanup(std::span<const std::uint8_t> request, int percent,
                    const std::string& name);
  void transfer_image(std::uint32_t address,
                      std::span<const std::uint8_t> image,
                      int begin_percent, int end_percent,
                      const std::string& label);
  void verify_crc(std::span<const std::uint8_t> image, int percent,
                  const std::string& label);
  void check_cancelled() const;
  static std::vector<std::uint8_t> fingerprint_f184();

  UdsClient& physical_;
  UdsClient& functional_;
  BaicRadarProtocol protocol_;
  Log log_;
  KeyGenerator key_generator_;
  std::stop_token stop_;
};

} // namespace uds
