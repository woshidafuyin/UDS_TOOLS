#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace uds {
struct ReportRow {
  std::chrono::system_clock::time_point timestamp{};
  std::string step;
  std::string verdict;
  std::string detail;
};
class HtmlReport {
public:
  void add(ReportRow row);
  void add_transcript(ReportRow row);
  std::filesystem::path write(const std::filesystem::path& directory,
                              const std::string& title) const;
private:
  static void append_row(std::vector<ReportRow>& rows, ReportRow row);
  std::vector<ReportRow> rows_;
  std::vector<ReportRow> transcript_rows_;
};
} // namespace uds
