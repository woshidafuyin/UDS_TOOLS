#pragma once

#include "core/uds_client.hpp"
#include "core/vbf.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace uds {

struct GeelyGeea2Image {
  std::string label;
  VbfFile file;
  bool secondary_bootloader{};
};

struct GeelyGeea2Protocol {
  std::uint8_t security_seed_subfunction{0x01};
  std::vector<std::uint8_t> programming_precondition{0x31, 0x01, 0x02, 0x06};
  std::vector<std::uint8_t> complete_and_compatible{0x31, 0x01, 0x02, 0x05};
  std::vector<std::uint8_t> report_dtc{0x19, 0x02, 0x08};
};

std::vector<std::uint8_t> geely_geea2_request_download(
    std::uint8_t data_format_identifier, std::uint32_t address,
    std::uint32_t length);
std::vector<std::uint8_t> geely_geea2_erase_memory(
    std::uint32_t address, std::uint32_t length);
std::vector<std::uint8_t> geely_geea2_check_memory(
    std::span<const std::uint8_t> signature);
std::vector<std::uint8_t> geely_geea2_activate_sbl(std::uint32_t address);
std::size_t geely_geea2_transfer_chunk_size(
    std::span<const std::uint8_t> request_download_response);
void validate_geely_geea2_images(const std::vector<GeelyGeea2Image>& images);

class GeelyGeea2Flow {
public:
  using Log = std::function<void(int, const std::string&)>;
  using Keygen = std::function<std::vector<std::uint8_t>(
      std::span<const std::uint8_t>, unsigned)>;

  GeelyGeea2Flow(UdsClient& physical, Log log, Keygen keygen,
                 GeelyGeea2Protocol protocol = {});

  void run(const std::vector<GeelyGeea2Image>& images,
           std::stop_token stop = {});
  [[nodiscard]] bool core_programming_completed() const noexcept;

private:
  UdsResponse expect(std::span<const std::uint8_t> request,
                     std::span<const std::uint8_t> prefix, int percent,
                     const std::string& name);
  void check_cancelled() const;
  void unlock();
  void transfer_file(const GeelyGeea2Image& image, int begin_percent,
                     int end_percent);
  void erase_file(const GeelyGeea2Image& image, int percent);
  void verify_file(const GeelyGeea2Image& image, int percent);

  UdsClient& physical_;
  Log log_;
  Keygen keygen_;
  GeelyGeea2Protocol protocol_;
  std::stop_token stop_;
  bool core_programming_completed_{};
};

} // namespace uds
