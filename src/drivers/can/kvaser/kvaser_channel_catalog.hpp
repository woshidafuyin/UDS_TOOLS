#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace uds::detail {

// CANlib exposes one process-wide channel index that contains both physical and
// virtual channels.  Keep that SDK-specific indexing inside the Kvaser adapter
// and expose a stable, 1-based logical order to the rest of the application.
struct KvaserChannelCatalogEntry {
  int api_index{};
  std::uint32_t card_type{};
  std::uint32_t capabilities{};
  std::uint32_t channel_on_card{};
  std::uint64_t serial_number{};
  std::uint64_t product_ean{};
  bool virtual_channel{};
  std::string device_description;
  std::string channel_name;
};

inline void order_kvaser_channels(
    std::vector<KvaserChannelCatalogEntry>& channels) {
  std::stable_partition(
      channels.begin(), channels.end(),
      [](const KvaserChannelCatalogEntry& channel) {
        return !channel.virtual_channel;
      });
}

} // namespace uds::detail
