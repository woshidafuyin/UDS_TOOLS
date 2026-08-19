#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace uds::app {

// Converts the payload behind a positive UDS response prefix into the
// project-facing value shown by the version-read UI.
std::wstring decode_version_value(std::span<const std::uint8_t> payload,
                                  std::wstring_view decoder);

} // namespace uds::app
