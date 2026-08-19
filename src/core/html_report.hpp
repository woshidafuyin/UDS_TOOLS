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
  std::filesystem::path write(const std::filesystem::path& directory,
                              const std::string& title) const;
private:
  std::vector<ReportRow> rows_;
};
} // namespace uds
