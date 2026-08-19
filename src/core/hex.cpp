#include "core/hex.hpp"

#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace uds {

std::string to_hex(std::span<const std::uint8_t> bytes, bool spaces) {
  std::ostringstream out;
  out << std::uppercase << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (spaces && i != 0) out << ' ';
    out << std::setw(2) << static_cast<unsigned>(bytes[i]);
  }
  return out.str();
}

std::vector<std::uint8_t> from_hex(const std::string& text) {
  std::string compact;
  for (const char ch : text) {
    if (!std::isspace(static_cast<unsigned char>(ch))) compact.push_back(ch);
  }
  if ((compact.size() % 2U) != 0U) throw std::invalid_argument("hex string has odd length");
  std::vector<std::uint8_t> result;
  result.reserve(compact.size() / 2U);
  for (std::size_t i = 0; i < compact.size(); i += 2U) {
    const auto value = std::stoul(compact.substr(i, 2), nullptr, 16);
    result.push_back(static_cast<std::uint8_t>(value));
  }
  return result;
}

} // namespace uds
