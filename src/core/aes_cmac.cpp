#include "core/aes_cmac.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace uds {
namespace {
void checked(NTSTATUS status) {
  if (status < 0) throw std::runtime_error("AES-CMAC Windows crypto operation failed");
}

class AesEcb {
public:
  explicit AesEcb(std::span<const std::uint8_t, 16> secret) {
    try {
      checked(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_AES_ALGORITHM, nullptr, 0));
      checked(BCryptSetProperty(algorithm_, BCRYPT_CHAINING_MODE,
          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
          sizeof(BCRYPT_CHAIN_MODE_ECB), 0));
      checked(BCryptGenerateSymmetricKey(algorithm_, &key_, nullptr, 0,
          const_cast<PUCHAR>(secret.data()), 16, 0));
    } catch (...) {
      if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
      throw;
    }
  }
  ~AesEcb() {
    if (key_) BCryptDestroyKey(key_);
    if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
  }
  AesEcb(const AesEcb&) = delete;
  AesEcb& operator=(const AesEcb&) = delete;
  Aes128Block encrypt(Aes128Block input) const {
    Aes128Block out{};
    ULONG count{};
    checked(BCryptEncrypt(key_, input.data(), 16, nullptr, nullptr, 0,
                          out.data(), 16, &count, 0));
    if (count != 16) throw std::runtime_error("AES block length mismatch");
    return out;
  }
private:
  BCRYPT_ALG_HANDLE algorithm_{};
  BCRYPT_KEY_HANDLE key_{};
};

Aes128Block double_block(const Aes128Block& in) {
  Aes128Block out{};
  for (std::size_t i = 0; i < 16; ++i) {
    out[i] = static_cast<std::uint8_t>((in[i] << 1U) |
                                      (i < 15 ? in[i + 1] >> 7U : 0));
  }
  if ((in[0] & 0x80U) != 0) out[15] ^= 0x87U;
  return out;
}
} // namespace

Aes128Block aes128_cmac(std::span<const std::uint8_t, 16> key,
                       std::span<const std::uint8_t> message) {
  AesEcb aes(key);
  auto k1 = double_block(aes.encrypt({}));
  auto k2 = double_block(k1);
  const auto blocks = message.empty() ? 1U : 1U + (message.size() - 1U) / 16U;
  const bool full = !message.empty() && message.size() % 16U == 0;
  Aes128Block state{};
  for (std::size_t b = 0; b < blocks; ++b) {
    Aes128Block block{};
    const auto offset = b * 16U;
    const auto count = std::min<std::size_t>(16, message.size() - offset);
    std::copy_n(message.begin() + static_cast<std::ptrdiff_t>(offset), count, block.begin());
    if (b + 1U == blocks) {
      if (!full) block[count] = 0x80;
      for (std::size_t i = 0; i < 16; ++i) block[i] ^= full ? k1[i] : k2[i];
    }
    for (std::size_t i = 0; i < 16; ++i) block[i] ^= state[i];
    state = aes.encrypt(block);
  }
  SecureZeroMemory(k1.data(), k1.size());
  SecureZeroMemory(k2.data(), k2.size());
  return state;
}

std::filesystem::path default_oem_key_path() {
  std::vector<wchar_t> buffer(32768);
  const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
  if (size == 0 || size >= buffer.size()) return {};
  return std::filesystem::path(buffer.data()) / L"ChuHang" / L"DiagnosticStudio" /
         L"keys" / L"perodua_p02c_level4.key";
}

Aes128Block load_protected_aes128_key(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("OEM Key file missing; import the Perodua CPD Level 4 key first");
  const auto length = std::filesystem::file_size(path);
  if (length <= 6 || length > 16384) throw std::runtime_error("invalid protected OEM Key file size");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
  if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    throw std::runtime_error("cannot read protected OEM Key file");
  const std::string header(bytes.begin(), bytes.begin() + 6);
  if (header != "CHKEY1") throw std::runtime_error("OEM Key must be a CHKEY1 protected key file");
  DATA_BLOB encrypted{static_cast<DWORD>(bytes.size() - 6), bytes.data() + 6};
  DATA_BLOB decrypted{};
  if (!CryptUnprotectData(&encrypted, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &decrypted)) {
    throw std::runtime_error("cannot unlock OEM Key with this Windows user; import the key locally");
  }
  Aes128Block result{};
  const bool valid = decrypted.cbData == result.size();
  if (valid) std::copy_n(decrypted.pbData, result.size(), result.begin());
  SecureZeroMemory(decrypted.pbData, decrypted.cbData);
  LocalFree(decrypted.pbData);
  if (!valid) throw std::runtime_error("OEM Key must contain exactly 16 bytes");
  return result;
}
} // namespace uds
