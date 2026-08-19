#include "core/sha256.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace uds {
namespace {

void check_status(NTSTATUS status, const char* operation) {
  if (status < 0) {
    throw std::runtime_error(std::string(operation) +
                             " failed, NTSTATUS=" +
                             std::to_string(static_cast<long>(status)));
  }
}

} // namespace

std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> data) {
  BCRYPT_ALG_HANDLE algorithm{};
  BCRYPT_HASH_HANDLE hash{};
  try {
    check_status(
        BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0),
        "BCryptOpenAlgorithmProvider(SHA256)");

    ULONG object_length{};
    ULONG result_length{};
    check_status(
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_length),
                          sizeof(object_length), &result_length, 0),
        "BCryptGetProperty(BCRYPT_OBJECT_LENGTH)");
    std::vector<std::uint8_t> hash_object(object_length);
    check_status(
        BCryptCreateHash(algorithm, &hash, hash_object.data(),
                         static_cast<ULONG>(hash_object.size()), nullptr, 0, 0),
        "BCryptCreateHash(SHA256)");

    std::size_t offset{};
    while (offset < data.size()) {
      const auto count = std::min<std::size_t>(
          data.size() - offset, std::numeric_limits<ULONG>::max());
      check_status(
          BCryptHashData(
              hash,
              const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(
                  data.data() + static_cast<std::ptrdiff_t>(offset))),
              static_cast<ULONG>(count), 0),
          "BCryptHashData(SHA256)");
      offset += count;
    }

    std::array<std::uint8_t, 32> digest{};
    check_status(BCryptFinishHash(hash, digest.data(),
                                  static_cast<ULONG>(digest.size()), 0),
                 "BCryptFinishHash(SHA256)");
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return digest;
  } catch (...) {
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    throw;
  }
}

} // namespace uds
