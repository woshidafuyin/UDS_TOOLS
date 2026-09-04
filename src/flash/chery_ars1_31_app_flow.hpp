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
enum class CheryArs131FlashMode { app_only, cal_only, app_cal };

// Canonical E0Y normal-flow stages frozen from the CANoe Download/TC_7/TC_2
// entries.  Keeping the stage plan public and side-effect free lets tests lock
// protocol parity without coupling them to CAN hardware or transport timing.
enum class CheryE0yNormalStage {
  settle,
  functional_extended_session,
  programming_precondition,
  disable_dtc_setting,
  disable_communication,
  programming_session,
  security_access,
  update_public_key,
  write_fingerprint,
  download_driver,
  verify_driver,
  erase_app,
  download_app,
  verify_app,
  erase_cal,
  download_cal,
  verify_cal,
  check_programming_dependencies,
  hard_reset,
  functional_default_session,
  clear_dtc,
};

struct CheryArs131DownloadPlan {
  CheryArs131FlashMode mode;
  bool download_app;
  bool download_cal;
};

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
  std::chrono::milliseconds post_d004_delay;
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
  std::uint32_t cal_start{};
  std::uint32_t cal_length{};
};

struct CheryArs131AppImages {
  std::vector<std::uint8_t> driver;
  std::vector<std::uint8_t> app;
  std::vector<std::uint8_t> cal;
  std::vector<std::uint8_t> driver_signature;
  std::vector<std::uint8_t> app_signature;
  std::vector<std::uint8_t> cal_signature;
};

const CheryArs131AppSpec& chery_ars1_31_app_spec(
    CheryArs131Project project);
CheryArs131DownloadPlan resolve_chery_ars1_31_download_plan(
    CheryArs131Project project, std::wstring_view entry_mode);
std::vector<std::uint8_t> chery_ars1_31_request_download(
    std::uint32_t address, std::uint32_t length);
std::vector<std::uint8_t> chery_ars1_31_erase_memory(
    std::uint32_t address, std::uint32_t length);
std::vector<std::uint8_t> chery_e0y_update_public_key_request();
std::vector<CheryE0yNormalStage> chery_e0y_normal_stage_sequence(
    CheryArs131FlashMode mode, bool update_public_key);

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
           CheryArs131FlashMode mode, bool update_public_key,
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
  void write_fingerprint(int percent, std::uint16_t did,
                         std::size_t length);
  void write_public_key(int percent);
  void verify(std::uint16_t routine_id,
              std::span<const std::uint8_t> signature, int percent,
              const std::string& label);
  void transfer(std::uint32_t address,
                std::span<const std::uint8_t> image, int begin_percent,
                int end_percent, const std::string& label);
  void precondition(int percent);
  void run_app_only(const CheryArs131AppImages& images,
                    bool update_public_key);
  void run_cal_only(const CheryArs131AppImages& images,
                    bool update_public_key);
  void run_t22_cal_only(const CheryArs131AppImages& images);
  void run_app_cal(const CheryArs131AppImages& images,
                   bool update_public_key);
  void run_e0y_normal(const CheryArs131AppImages& images,
                      CheryArs131FlashMode mode, bool update_public_key);
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
