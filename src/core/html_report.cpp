#include "core/html_report.hpp"

#include <chrono>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <sstream>

namespace uds {
namespace {
std::string escape(std::string value) {
  for (std::size_t pos = 0; (pos = value.find('&', pos)) != std::string::npos; pos += 5) value.replace(pos,1,"&amp;");
  for (std::size_t pos = 0; (pos = value.find('<', pos)) != std::string::npos; pos += 4) value.replace(pos,1,"&lt;");
  for (std::size_t pos = 0; (pos = value.find('>', pos)) != std::string::npos; pos += 4) value.replace(pos,1,"&gt;");
  return value;
}

std::string row_time(std::chrono::system_clock::time_point timestamp) {
  const auto time = std::chrono::system_clock::to_time_t(timestamp);
  std::tm local{};
  localtime_s(&local, &time);
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                          timestamp.time_since_epoch()) %
                      std::chrono::seconds(1);
  std::ostringstream value;
  value << std::put_time(&local, "%H:%M:%S.") << std::setfill('0')
        << std::setw(3) << millis.count();
  return value.str();
}
}
void HtmlReport::add(ReportRow row) {
  if (row.timestamp == std::chrono::system_clock::time_point{}) {
    row.timestamp = std::chrono::system_clock::now();
  }
  if (!rows_.empty() && row.timestamp < rows_.back().timestamp) {
    row.timestamp = rows_.back().timestamp;
  }
  rows_.push_back(std::move(row));
}

std::filesystem::path HtmlReport::write(const std::filesystem::path& directory,
                                         const std::string& title) const {
  std::filesystem::create_directories(directory);
  const auto now = std::chrono::system_clock::now();
  const auto now_time = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  localtime_s(&local, &now_time);
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch()) %
                      std::chrono::seconds(1);
  std::ostringstream timestamp;
  timestamp << std::put_time(&local, "%Y%m%d_%H%M%S_") << std::setfill('0')
            << std::setw(3) << millis.count();
  const auto failed = std::any_of(
      rows_.begin(), rows_.end(),
      [](const ReportRow& row) { return row.verdict == "FAIL"; });
  const auto result_class = failed ? "FAIL" : "PASS";
  const auto result_text = failed ? "失败（FAIL）" : "成功（PASS）";

  std::ostringstream html;
  html << "<!doctype html><html><head><meta charset='utf-8'><title>"
       << escape(title) << "</title>"
          "<style>body{font-family:Segoe UI;padding:24px;color:#222}"
          "table{border-collapse:collapse;width:100%;font-size:14px}"
          "th,td{border:1px solid #ddd;padding:7px;vertical-align:top}"
          "th{background:#f4f6f8;text-align:left}pre{margin:0;white-space:pre-wrap}"
          ".PASS{color:#087f23;font-weight:600}.FAIL{color:#c62828;font-weight:600}"
          ".WARN{color:#a15c00;font-weight:600}"
          ".result{padding:14px 16px;margin:12px 0 16px;border-radius:4px;font-size:18px}"
          ".result.PASS{background:#edf8f0;border-left:5px solid #087f23}"
          ".result.FAIL{background:#fff0f0;border-left:5px solid #c62828}"
          ".time{font-size:14px;color:#444;margin-top:6px;font-weight:400}"
          "</style></head><body><h2>"
       << escape(title) << "</h2><div class='result " << result_class
       << "'>本次流程结果：" << result_text << "<div class='time'>完成时间："
       << std::put_time(&local, "%Y-%m-%d %H:%M:%S")
       << "</div></div><table><tr><th>Time</th><th>Step</th><th>Verdict</th><th>Detail</th></tr>";
  for (const auto& row : rows_) {
    html << "<tr><td>" << row_time(row.timestamp) << "</td><td>"
         << escape(row.step)
         << "</td><td class='" << escape(row.verdict) << "'>"
         << escape(row.verdict) << "</td><td><pre>" << escape(row.detail)
         << "</pre></td></tr>";
  }
  html << "</table></body></html>";

  const auto write_file = [&](const std::filesystem::path& path) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
      throw std::runtime_error("cannot open HTML report for writing");
    }
    file << html.str();
    if (!file) throw std::runtime_error("cannot write HTML report");
  };
  const auto history = directory / ("report_" + timestamp.str() + ".html");
  const auto latest = directory / "latest_report.html";
  write_file(history);
  write_file(latest);
  return latest;
}
} // namespace uds
