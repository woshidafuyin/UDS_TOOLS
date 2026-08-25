#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace uds {

struct SRecordSegment {
  std::uint32_t address{};
  std::vector<std::uint8_t> data;
};

struct SRecordImage {
  std::vector<SRecordSegment> segments;
  std::size_t payload_size{};
};

struct CbfSegment {
  std::uint32_t address{};
  std::vector<std::uint8_t> data;
};

struct CbfImage {
  std::string software_id;
  std::string software_version;
  std::string software_type;
  std::uint32_t ecu_address{};
  CbfSegment main;
  CbfSegment abt;
  std::vector<std::uint8_t> device_signature;
};

// Leapmotor OTA TMP packages contain a UTF-8 metadata block, one APP image
// descriptor, the complete APP bytes and the certificate bytes that are
// otherwise distributed as HEX-ASCII .asc files.
struct LeapmotorTmpPackage {
  std::string metadata_json;
  std::uint32_t app_crc32{};
  SRecordSegment app;
  std::vector<std::uint8_t> certificate;
};

// Parse all S1/S2/S3 data records and merge adjacent records into their
// natural address segments. Addresses and lengths come from the S-record
// contents; S7/S8/S9 execution-start records are deliberately not treated as
// download addresses.
SRecordImage load_srecord_image(const std::filesystem::path& path);
SRecordSegment load_single_srecord_segment(
    const std::filesystem::path& path);

std::vector<std::uint8_t> load_srecord_window(const std::filesystem::path& path,
                                               std::uint32_t start, std::size_t length);
std::vector<std::uint8_t> load_srecord_window_filtered(const std::filesystem::path& path,
                                                        std::uint32_t start, std::size_t length);
std::vector<std::uint8_t> load_hex_bytes(const std::filesystem::path& path,
                                         std::size_t take, std::size_t minimum);
std::vector<std::uint8_t> load_asc_hex(const std::filesystem::path& path,
                                       std::size_t take, std::size_t minimum);

LeapmotorTmpPackage load_leapmotor_tmp(
    const std::filesystem::path& path);

CbfImage load_chuneng_cbf(const std::filesystem::path& path);
} // namespace uds
