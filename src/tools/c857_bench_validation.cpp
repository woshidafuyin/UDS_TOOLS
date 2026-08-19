#include "app/flash_controller.hpp"
#include "app/probe_service.hpp"
#include "core/profile.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>

namespace {

struct Options {
  bool flash{};
  std::filesystem::path dist;
  std::filesystem::path log;
};

Options parse_options(int argc, wchar_t** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::wstring argument = argv[index];
    if (argument == L"--flash") {
      options.flash = true;
    } else if (argument == L"--probe") {
      options.flash = false;
    } else if (argument == L"--dist" && index + 1 < argc) {
      options.dist = argv[++index];
    } else if (argument == L"--log" && index + 1 < argc) {
      options.log = argv[++index];
    } else {
      throw std::runtime_error(
          "usage: c857_bench_validation [--probe|--flash] "
          "--dist <dist-directory> --log <log-file>");
    }
  }
  if (options.dist.empty() || options.log.empty()) {
    throw std::runtime_error("--dist and --log are required");
  }
  options.dist = std::filesystem::absolute(options.dist).lexically_normal();
  options.log = std::filesystem::absolute(options.log).lexically_normal();
  return options;
}

std::string utf8(const std::wstring& text) {
  const auto encoded = std::filesystem::path(text).u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

std::filesystem::path resolve(const std::filesystem::path& dist,
                              const std::filesystem::path& configured) {
  if (configured.empty()) return {};
  return configured.is_absolute()
             ? configured
             : (dist / configured).lexically_normal();
}

} // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    const auto options = parse_options(argc, argv);
    std::filesystem::create_directories(options.log.parent_path());
    std::ofstream log_file(options.log, std::ios::app);
    if (!log_file) throw std::runtime_error("cannot open validation log");
    const auto log = [&](const std::string& line) {
      std::cout << line << '\n';
      log_file << line << '\n';
      log_file.flush();
    };

    auto profile =
        uds::load_profile_ini(options.dist / "profiles" / "changan_c857.ini");
    const auto target = std::find_if(
        profile.targets.cbegin(), profile.targets.cend(),
        [](const uds::FlashTargetProfile& item) {
          return item.id == L"secondary";
        });
    if (target == profile.targets.cend()) {
      throw std::runtime_error("C857 secondary target is missing");
    }
    profile.tx_id = target->tx_id;
    profile.rx_id = target->rx_id;

    log("MODE=" + std::string(options.flash ? "FLASH" : "PROBE"));
    log("PROFILE=changan_c857; TARGET=secondary/ICRR; CH=" +
        std::to_string(profile.channel) + "; TX=0x760; RX=0x768; FUNC=0x7DF");

    uds::app::ProbeRequest probe_request;
    probe_request.profile = profile;
    probe_request.entry_mode = L"app";
    probe_request.channel = profile.channel;
    probe_request.tx_id = profile.tx_id;
    probe_request.rx_id = profile.rx_id;
    probe_request.nominal_bitrate = profile.nominal_bitrate;
    probe_request.data_bitrate = profile.data_bitrate;
    probe_request.padding = profile.padding;
    uds::app::ProbeService probe;
    const auto probe_result = probe.run(
        probe_request,
        {log, [&](int percent, const std::string& line) {
           log("PROGRESS=" + std::to_string(percent) + "; " + line);
         }},
        std::stop_token{});
    log("PROBE_RESULT=" + probe_result.message);
    if (!probe_result.success) return 2;
    if (!options.flash) return 0;

    uds::app::FlashRequest request;
    request.profile = profile;
    request.entry_mode = L"app";
    request.repeat_count = 1;
    request.executable_directory = options.dist;
    request.channel = profile.channel;
    request.tx_id = profile.tx_id;
    request.rx_id = profile.rx_id;
    request.functional_id = profile.functional_id;
    request.nominal_bitrate = profile.nominal_bitrate;
    request.data_bitrate = profile.data_bitrate;
    request.padding = profile.padding;
    request.driver_file = resolve(options.dist, profile.driver_file);
    request.app_file = resolve(
        options.dist,
        target->app_file.empty() ? profile.app_file : target->app_file);
    request.cal_file = resolve(options.dist, target->cal_file);
    request.driver_verify_file =
        resolve(options.dist, profile.driver_verify_file);
    request.app_verify_file = resolve(options.dist, target->app_verify_file);
    request.cal_verify_file = resolve(options.dist, target->cal_verify_file);
    request.security_dll = resolve(
        options.dist, target->security_dll.empty() ? profile.security_dll
                                                   : target->security_dll);

    uds::app::OperationState state;
    uds::app::FlashController controller(state);
    std::optional<uds::app::OperationResult> result;
    const auto started = controller.start(
        std::move(request),
        {log,
         [&](int percent, const std::string& line) {
           log("PROGRESS=" + std::to_string(percent) + "; " + line);
         },
         [&](uds::app::OperationResult completed) {
           result = std::move(completed);
         }});
    if (!started) throw std::runtime_error("flash controller did not start");
    controller.wait();
    if (!result) throw std::runtime_error("flash controller returned no result");
    log("FLASH_RESULT=" + utf8(result->message));
    log("REPORT=" + utf8(result->report_path.wstring()));
    return result->success ? 0 : 3;
  } catch (const std::exception& error) {
    std::cerr << "ERROR=" << error.what() << '\n';
    return 1;
  }
}
