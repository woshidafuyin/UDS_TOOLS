#pragma once

#include "core/uds_client.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace uds {

enum class CheryArs131Project { t1ej, t22, e0y };
enum class CheryArs131D004Mode { none, routine_only, app_signature };

struct CheryArs131AppSpec {
  CheryArs131Project project;
  std::string name;
  std::uint32_t tx_id;
  std::uint32_t rx_id;
  std::uint8_t seed_subfunction;
  std::size_t seed_length;
  std::size_t key_length;
  std::uint16_t precondition_routine;
  std::uint16_t verification_routine;
  std::uint16_t fingerprint_did;
  std::size_t fingerprint_length;
  CheryArs131D004Mode d004_mode;
  bool initial_physical_extended_session;
  bool precondition_before_network_disable;
  bool install_d005;
  bool restore_default_session;
};

struct CheryArs131AppLayout {
  std::uint32_t driver_start{0x08000000};
  std::uint32_t driver_length{0x400};
  std::uint32_t app_start{0xC0080000};
  std::uint32_t app_length{0xF5000};
};

struct CheryArs131AppImages {
  std::vector<std::uint8_t> driver;
  std::vector<std::uint8_t> app;
  std::vector<std::uint8_t> driver_signature;
  std::vector<std::uint8_t> app_signature;
};

const CheryArs131AppSpec& chery_ars1_31_app_spec(
    CheryArs131Project project);
std::vector<std::uint8_t> chery_ars1_31_request_download(
    std::uint32_t address, std::uint32_t length);
std::vector<std::uint8_t> chery_ars1_31_erase_memory(
    std::uint32_t address, std::uint32_t length);

class CheryArs131AppFlow {
public:
  using Log = std::function<void(int, const std::string&)>;
  using KeyGenerator = std::function<std::vector<std::uint8_t>(
      std::span<const std::uint8_t>, unsigned)>;

  CheryArs131AppFlow(UdsClient& physical, UdsClient& functional,
                     CheryArs131AppSpec spec,
                     CheryArs131AppLayout layout, Log log,
                     KeyGenerator key_generator);

  void run(const CheryArs131AppImages& images,
           std::stop_token stop = {});

private:
  UdsResponse expect(UdsClient& client,
                     std::span<const std::uint8_t> request,
                     std::span<const std::uint8_t> prefix, int percent,
                     const std::string& name);
  UdsResponse routine(std::span<const std::uint8_t> request,
                      std::uint16_t routine_id, int percent,
                      const std::string& name, bool allow_status_one = false);
  void functional_send(std::span<const std::uint8_t> request, int percent,
                       const std::string& name);
  void unlock(int percent);
  void write_fingerprint(int percent);
  void verify(std::uint16_t routine_id,
              std::span<const std::uint8_t> signature, int percent,
              const std::string& label);
  void transfer(std::uint32_t address,
                std::span<const std::uint8_t> image, int begin_percent,
                int end_percent, const std::string& label);
  void precondition(int percent);
  void wait(std::chrono::milliseconds duration) const;
  void cancelled() const;

  UdsClient& physical_;
  UdsClient& functional_;
  CheryArs131AppSpec spec_;
  CheryArs131AppLayout layout_;
  Log log_;
  KeyGenerator key_generator_;
  std::stop_token stop_;
};

} // namespace uds
