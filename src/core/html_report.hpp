#pragma once

#include "core/flash_event.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace uds {

struct ReportRow {
  std::chrono::system_clock::time_point timestamp{};
  std::string step;
  std::string verdict;
  std::string detail;
  FlashStage stage{FlashStage::unspecified};
  unsigned cycle{};
  std::optional<std::uint8_t> uds_service;
  FlashImageRole image_role{FlashImageRole::none};
};
class HtmlReport {
public:
  void add(ReportRow row);
  void add_event(FlashEvent event);
  void add_transcript(ReportRow row);
  void add_transcript_event(FlashEvent event);
  std::filesystem::path write(const std::filesystem::path& directory,
                              const std::string& title) const;
private:
  static void append_row(std::vector<ReportRow>& rows, ReportRow row);
  std::vector<ReportRow> rows_;
  std::vector<ReportRow> transcript_rows_;
};
} // namespace uds
