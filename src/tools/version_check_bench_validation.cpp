#include "app/version_check_service.hpp"
#include "core/profile.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>

namespace {

struct Options {
  std::filesystem::path dist;
  std::filesystem::path log;
  std::wstring profile_id{L"changan_c857"};
  std::wstring target_id{L"secondary"};
};

Options parse_options(int argc, wchar_t** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::wstring argument = argv[index];
    if (argument == L"--dist" && index + 1 < argc) {
      options.dist = argv[++index];
    } else if (argument == L"--log" && index + 1 < argc) {
      options.log = argv[++index];
    } else if (argument == L"--profile" && index + 1 < argc) {
      options.profile_id = argv[++index];
    } else if (argument == L"--target" && index + 1 < argc) {
      options.target_id = argv[++index];
    } else {
      throw std::runtime_error(
          "usage: version_check_bench_validation --dist <dist-directory> "
          "--log <log-file> [--profile <profile-id>] [--target <target-id>]");
    }
  }
  if (options.dist.empty() || options.log.empty()) {
    throw std::runtime_error("--dist and --log are required");
  }
  options.dist = std::filesystem::absolute(options.dist).lexically_normal();
  options.log = std::filesystem::absolute(options.log).lexically_normal();
  return options;
}

std::string utf8(std::wstring_view text) {
  const auto encoded = std::filesystem::path(text).u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

std::string status_text(uds::app::VersionCheckStatus status) {
  switch (status) {
  case uds::app::VersionCheckStatus::pass:
    return "PASS";
  case uds::app::VersionCheckStatus::fail:
    return "FAIL";
  case uds::app::VersionCheckStatus::warning:
    return "WARN";
  case uds::app::VersionCheckStatus::info:
    return "INFO";
  case uds::app::VersionCheckStatus::error:
  default:
    return "ERROR";
  }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    const auto options = parse_options(argc, argv);
    std::filesystem::create_directories(options.log.parent_path());
    std::ofstream output(options.log);
    if (!output) throw std::runtime_error("cannot open validation log");
    const auto write = [&](const std::string& line) {
      std::cout << line << '\n';
      output << line << '\n';
      output.flush();
    };

    const auto profile_path =
        options.dist / "profiles" / (options.profile_id + L".ini");
    auto profile = uds::load_profile_ini(profile_path);
    const auto target =
        std::find_if(profile.targets.cbegin(), profile.targets.cend(),
                     [&](const uds::FlashTargetProfile& item) {
                       return item.id == options.target_id;
                     });
    if (target == profile.targets.cend()) {
      throw std::runtime_error("requested target is missing from profile");
    }

    uds::app::VersionCheckRequest request;
    request.profile = profile;
    request.profile_path = profile_path;
    request.target_id = options.target_id;
    request.channel = profile.channel;
    request.tx_id = target->tx_id;
    request.rx_id = target->rx_id;

    write("MODE=VERSION_CHECK");
    write("PROFILE=" + utf8(options.profile_id) + "; TARGET=" +
          utf8(options.target_id) + "; CH=" +
          std::to_string(request.channel) + "; TX=" +
          std::to_string(request.tx_id) + "; RX=" +
          std::to_string(request.rx_id));

    uds::app::VersionCheckService service;
    const auto result = service.run(
        request,
        {write, [&](int percent, const std::string& line) {
           write("PROGRESS=" + std::to_string(percent) + "; " + line);
         }},
        std::stop_token{});

    for (const auto& item : result.items) {
      write("ITEM=" + status_text(item.status) + "; NAME=" +
            utf8(item.name) + "; REQUEST=" + item.request_hex +
            "; RESPONSE=" + item.response_hex + "; EXPECTED=" +
            utf8(item.expected) + "; ACTUAL=" + utf8(item.actual) +
            "; ELAPSED_MS=" + std::to_string(item.elapsed_ms));
    }
    write("RESULT=" + result.message);
    return result.success ? 0 : 2;
  } catch (const std::exception& error) {
    std::cerr << "ERROR=" << error.what() << '\n';
    return 1;
  }
}
