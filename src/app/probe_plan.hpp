#pragma once

#include "app/probe_service.hpp"

#include <cstdint>
#include <string>

namespace uds::app::probe_detail {

struct ProbePlan {
  ProbeRequest request;
  bool ft_probe{};
  bool boot_probe{};
  bool power_managed{};
  bool chuneng_arc331{};
  bool shidaixinan_hjzj{};
  bool lingpao_radar{};
  bool geely_p416{};
  bool xizhong{};
  bool ars131_app{};
  bool chery_e0y{};
  bool secondary_target{};
  bool expected_profile_ids{};
  std::uint32_t app_tx_id{};
  std::uint32_t app_rx_id{};
  std::uint32_t probe_tx_id{};
  std::uint8_t session{};
  int attempt_count{1};
};

struct ProbePlanResolution {
  bool valid{};
  ProbePlan plan;
  std::string message;
};

ProbePlanResolution resolve_probe_plan(const ProbeRequest& requested);
std::string probe_start_description(const ProbePlan& plan);
std::string probe_session_description(const ProbePlan& plan);

} // namespace uds::app::probe_detail
