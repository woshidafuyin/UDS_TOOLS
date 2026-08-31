#include "core/html_report.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace uds {
namespace {

enum class Stage {
  configuration,
  pre_flash_check,
  programming_session,
  security_access,
  driver_request_download,
  driver_transfer,
  driver_transfer_exit,
  driver_verification,
  app_request_download,
  app_transfer,
  app_transfer_exit,
  app_verification,
  dependency_check,
  ecu_reset_recovery,
  post_flash_verification,
  trace_evidence,
  cycle_overview,
  other,
};

struct CycleRef {
  unsigned index{1};
  unsigned total{1};
  bool explicit_cycle{};
};

struct ClassifiedRow {
  const ReportRow* row{};
  Stage stage{Stage::other};
  unsigned cycle{1};
};

struct StageStatus {
  std::string verdict{"NOT_RUN"};
  std::string symbol{"&mdash;"};
  std::string label{"未执行"};
  std::string css{"NOT_RUN"};
};

struct TransferBlock {
  std::string region;
  unsigned index{};
  unsigned total{};
  unsigned tx_count{};
  std::size_t payload_bytes{};
  unsigned sequence{};
  bool has_sequence{};
  bool completed{};
};

struct TransferStats {
  std::map<std::string, TransferBlock> blocks;
  std::map<std::string, unsigned> region_totals;
  std::set<std::string> regions;
  std::set<std::string> addresses;
  std::optional<std::size_t> max_block_length;
  unsigned pending_count{};
  bool rollover{};
  std::optional<unsigned> first_sequence;
  std::optional<unsigned> last_sequence;
  std::chrono::system_clock::time_point started{};
  std::chrono::system_clock::time_point finished{};
  StageStatus status;
};

struct TraceEvidence {
  unsigned cycle{1};
  std::string type;
  std::filesystem::path path;
  std::chrono::system_clock::time_point started{};
};

std::string escape(std::string value) {
  for (std::size_t pos = 0;
       (pos = value.find('&', pos)) != std::string::npos; pos += 5)
    value.replace(pos, 1, "&amp;");
  for (std::size_t pos = 0;
       (pos = value.find('<', pos)) != std::string::npos; pos += 4)
    value.replace(pos, 1, "&lt;");
  for (std::size_t pos = 0;
       (pos = value.find('>', pos)) != std::string::npos; pos += 4)
    value.replace(pos, 1, "&gt;");
  for (std::size_t pos = 0;
       (pos = value.find('"', pos)) != std::string::npos; pos += 6)
    value.replace(pos, 1, "&quot;");
  return value;
}

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool contains_any(std::string_view text,
                  std::initializer_list<std::string_view> needles) {
  return std::any_of(needles.begin(), needles.end(), [&](const auto needle) {
    return text.find(needle) != std::string_view::npos;
  });
}

std::string row_time(std::chrono::system_clock::time_point timestamp) {
  if (timestamp == std::chrono::system_clock::time_point{}) return "未记录";
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

std::string full_time(std::chrono::system_clock::time_point timestamp) {
  if (timestamp == std::chrono::system_clock::time_point{}) return "未记录";
  const auto time = std::chrono::system_clock::to_time_t(timestamp);
  std::tm local{};
  localtime_s(&local, &time);
  std::ostringstream value;
  value << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
  return value.str();
}

std::string join(const std::set<std::string>& values) {
  if (values.empty()) return "未记录";
  std::ostringstream result;
  bool first = true;
  for (const auto& value : values) {
    if (!first) result << "、";
    result << value;
    first = false;
  }
  return result.str();
}

std::string path_utf8(const std::filesystem::path& path) {
  const auto encoded = path.u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

CycleRef cycle_ref(std::string_view text) {
  const std::string source(text);
  std::smatch match;
  static const std::regex chinese(
      R"((?:\[)?第([0-9]+)/([0-9]+)次(?:\])?)");
  static const std::regex english(
      R"((?:Flash cycle|Cycle)\s+([0-9]+)/([0-9]+))",
      std::regex::icase);
  if (std::regex_search(source, match, chinese) ||
      std::regex_search(source, match, english)) {
    return {static_cast<unsigned>(std::stoul(match[1].str())),
            static_cast<unsigned>(std::stoul(match[2].str())), true};
  }
  return {};
}

unsigned repeat_count(const std::vector<ReportRow>& rows,
                      const std::vector<ReportRow>& transcript) {
  unsigned total = 1;
  const auto inspect = [&](const auto& source) {
    for (const auto& row : source) {
      const auto ref = cycle_ref(row.step + " " + row.detail);
      total = std::max(total, ref.total);
    }
  };
  inspect(rows);
  inspect(transcript);
  return total;
}

bool role_driver(std::string_view text) {
  return contains_any(text, {"driver", "sbl", "flashdrv", "flash driver"});
}

bool role_app(std::string_view text) {
  return contains_any(text, {"app", "application", "cal", "ess"});
}

Stage classify_direct(const ReportRow& row) {
  const auto text = lower_ascii(row.step + " " + row.detail);
  if (contains_any(text, {"asc + blf trace", "raw asc", "raw blf"}))
    return Stage::trace_evidence;
  if (contains_any(text, {"diagnostic ids", "flash target", "can configuration",
                          "flash file", "flash count"}))
    return Stage::configuration;
  if (contains_any(text, {"pre-flash qualification", "preflight",
                          "pre-flash check", "programmingprecondition"}))
    return Stage::pre_flash_check;
  if (text.find("flash cycle") != std::string::npos)
    return Stage::cycle_overview;
  if (text.find("download") != std::string::npos &&
      lower_ascii(row.verdict) == "fail")
    return Stage::cycle_overview;
  if (contains_any(text, {"post-reset", "post reset", "ecu reset", "ecureset",
                          "softreset", "defaultsession"}) ||
      text.find("11 0") != std::string::npos)
    return Stage::ecu_reset_recovery;
  if (contains_any(text, {"post-flash", "post flash", "version verification",
                          "after flash", "read version"}))
    return Stage::post_flash_verification;
  if (contains_any(text, {"dependency", "ff01", "ff 01", "02 05"}))
    return Stage::dependency_check;
  if (contains_any(text, {"securityaccess", "security access", "seed", " key",
                          "27 0", "27 service"}))
    return Stage::security_access;
  if (contains_any(text, {"programmingsession", "programming session",
                          "10 02", "10 service", "sessioncontrol"}))
    return Stage::programming_session;

  const auto driver = role_driver(text);
  const auto app = role_app(text) && !driver;
  const auto request_download =
      contains_any(text, {"requestdownload", "request download", "34 "});
  const auto transfer_exit = contains_any(
      text, {"requesttransferexit", "transfer exit", "transferexit", "37 "});
  const auto transfer =
      !transfer_exit && contains_any(text, {"transferdata", "36 ", "76 "});
  const auto verification = contains_any(
      text, {"verification", "verify", "integrity", "signature", "crc"});
  if (driver && request_download) return Stage::driver_request_download;
  if (driver && transfer_exit) return Stage::driver_transfer_exit;
  if (driver && transfer) return Stage::driver_transfer;
  if (driver && verification) return Stage::driver_verification;
  if (app && request_download) return Stage::app_request_download;
  if (app && transfer_exit) return Stage::app_transfer_exit;
  if (app && transfer) return Stage::app_transfer;
  if (app && verification) return Stage::app_verification;
  return Stage::other;
}

std::vector<ClassifiedRow> classify_summary(
    const std::vector<ReportRow>& rows) {
  std::vector<ClassifiedRow> result;
  result.reserve(rows.size());
  for (const auto& row : rows) {
    const auto ref = cycle_ref(row.step + " " + row.detail);
    result.push_back({&row, classify_direct(row), ref.index});
  }
  return result;
}

std::vector<ClassifiedRow> classify_transcript(
    const std::vector<ReportRow>& rows, unsigned total_cycles) {
  std::vector<ClassifiedRow> result;
  result.reserve(rows.size());
  std::map<unsigned, Stage> active_stage;
  for (const auto& row : rows) {
    const auto text = row.step + " " + row.detail;
    const auto ref = cycle_ref(text);
    const auto cycle = ref.explicit_cycle ? ref.index : 1U;
    auto stage = classify_direct(row);
    if (stage != Stage::other && stage != Stage::cycle_overview) {
      active_stage[cycle] = stage;
    } else {
      const auto lower = lower_ascii(text);
      const auto uds_line = contains_any(
          lower, {"tx [", "rx [", "nrc 0x", "response / timeout", "timeout"});
      if (uds_line && active_stage.contains(cycle)) stage = active_stage[cycle];
    }
    result.push_back({&row, stage, std::min(cycle, total_cycles)});
  }
  return result;
}

int verdict_rank(std::string verdict) {
  verdict = lower_ascii(std::move(verdict));
  if (verdict.empty() || verdict == "not_run") return 0;
  if (verdict == "fail" || verdict.find(" fail") != std::string::npos) return 4;
  if (verdict == "warn" || verdict.find("warn") != std::string::npos) return 3;
  if (verdict == "pass" || verdict.find("pass") != std::string::npos) return 2;
  if (verdict == "info" || !verdict.empty()) return 1;
  return 0;
}

StageStatus status_from_rank(int rank) {
  if (rank >= 4) return {"FAIL", "&#10005;", "FAIL", "FAIL"};
  if (rank == 3) return {"WARN", "&#9888;", "WARN", "WARN"};
  if (rank == 2) return {"PASS", "&#10003;", "PASS", "PASS"};
  if (rank == 1) return {"INFO", "&#9679;", "INFO", "INFO"};
  return {};
}

StageStatus stage_status(const std::vector<ClassifiedRow>& summary,
                         const std::vector<ClassifiedRow>& transcript,
                         Stage stage, unsigned cycle = 0) {
  int rank = 0;
  const auto inspect = [&](const auto& rows) {
    for (const auto& item : rows) {
      if (item.stage != stage || (cycle != 0 && item.cycle != cycle)) continue;
      rank = std::max(rank, verdict_rank(item.row->verdict));
    }
  };
  inspect(summary);
  inspect(transcript);
  return status_from_rank(rank);
}

std::string status_html(const StageStatus& status) {
  return "<span class='status " + status.css + "'><span class='symbol'>" +
         status.symbol + "</span>" + escape(status.label) + "</span>";
}

std::string cycle_id(unsigned cycle, std::string_view suffix) {
  return "cycle-" + std::to_string(cycle) + "-" + std::string(suffix);
}

std::vector<const ReportRow*> rows_for(const std::vector<ClassifiedRow>& rows,
                                       Stage stage, unsigned cycle = 0) {
  std::vector<const ReportRow*> result;
  for (const auto& item : rows) {
    if (item.stage == stage && (cycle == 0 || item.cycle == cycle))
      result.push_back(item.row);
  }
  return result;
}

void append_table(std::ostringstream& html,
                  const std::vector<const ReportRow*>& rows,
                  std::string_view empty_message = "本次未记录该阶段。") {
  if (rows.empty()) {
    html << "<p class='not-run'>" << escape(std::string(empty_message)) << "</p>";
    return;
  }
  html << "<div class='table-wrap'><table><thead><tr><th>时间</th><th>步骤</th>"
          "<th>判定</th><th>详细信息</th></tr></thead><tbody>";
  for (const auto* row : rows) {
    auto detail = row->detail;
    const std::string missing = "<not configured>";
    const auto missing_at = detail.find(missing);
    if (missing_at != std::string::npos) {
      detail.replace(missing_at, missing.size(),
                     "未配置（是否需要由本次刷写模式决定；未被流程要求时不影响结果）");
    }
    html << "<tr><td>" << row_time(row->timestamp) << "</td><td>"
         << escape(row->step) << "</td><td class='" << escape(row->verdict)
         << "'>" << escape(row->verdict) << "</td><td><pre>"
         << escape(detail) << "</pre></td></tr>";
  }
  html << "</tbody></table></div>";
}

void append_back_link(std::ostringstream& html) {
  html << "<a class='back-link' href='#report-navigation'>&uarr; 返回报告导航</a>";
}

std::string stage_title(std::string_view title, const StageStatus& status) {
  return "<span>" + escape(std::string(title)) + "</span>" +
         status_html(status);
}

void append_simple_stage(std::ostringstream& html, std::string_view id,
                         std::string_view title, const StageStatus& status,
                         const std::vector<const ReportRow*>& rows,
                         int heading = 3) {
  html << "<section class='report-section' id='" << id << "'><h" << heading
       << ">" << stage_title(title, status) << "</h" << heading << ">";
  append_table(html, rows);
  append_back_link(html);
  html << "</section>";
}

std::optional<std::pair<std::string, std::pair<unsigned, unsigned>>>
transfer_block_ref(std::string_view text) {
  const std::string source(text);
  std::smatch match;
  static const std::regex pattern(
      R"((?:36\s+|TransferData\s*\(0x36\)\s+)([A-Za-z0-9_.+\-]+)\s+block\s+([0-9]+)/([0-9]+))",
      std::regex::icase);
  if (!std::regex_search(source, match, pattern)) return std::nullopt;
  return std::make_pair(
      match[1].str(),
      std::make_pair(static_cast<unsigned>(std::stoul(match[2].str())),
                     static_cast<unsigned>(std::stoul(match[3].str()))));
}

std::optional<unsigned> parse_sequence(std::string_view text) {
  const std::string source(text);
  std::smatch match;
  static const std::regex pattern(R"(\]\s+36\s+([0-9A-Fa-f]{2})(?:\s|$))");
  if (!std::regex_search(source, match, pattern)) return std::nullopt;
  return static_cast<unsigned>(std::stoul(match[1].str(), nullptr, 16));
}

std::size_t parse_request_payload_bytes(std::string_view text) {
  const std::string source(text);
  std::smatch match;
  static const std::regex pattern(R"(\[([0-9]+) bytes\])",
                                  std::regex::icase);
  if (!std::regex_search(source, match, pattern)) return 0;
  const auto request_size = static_cast<std::size_t>(std::stoull(match[1].str()));
  return request_size >= 2 ? request_size - 2 : 0;
}

void collect_transfer_metadata(TransferStats& stats,
                               const std::vector<ClassifiedRow>& summary,
                               Stage transfer_stage, unsigned cycle) {
  static const std::regex address_pattern(
      R"((?:address|main)\s*=\s*(0x[0-9A-Fa-f]+)(?:\s*/\s*(0x[0-9A-Fa-f]+))?)",
      std::regex::icase);
  static const std::regex block_length_pattern(
      R"(block_length\s*=\s*(0x[0-9A-Fa-f]+|[0-9]+))",
      std::regex::icase);
  const auto driver = transfer_stage == Stage::driver_transfer;
  for (const auto& item : summary) {
    if (item.cycle != cycle) continue;
    const std::string text = item.row->step + " " + item.row->detail;
    const auto lowered = lower_ascii(text);
    if ((driver && !role_driver(lowered)) ||
        (!driver && (!role_app(lowered) || role_driver(lowered))))
      continue;
    for (std::sregex_iterator it(text.begin(), text.end(), address_pattern), end;
         it != end; ++it) {
      auto value = (*it)[1].str();
      if ((*it)[2].matched) value += "/" + (*it)[2].str();
      stats.addresses.insert(std::move(value));
    }
    std::smatch block_match;
    if (std::regex_search(text, block_match, block_length_pattern)) {
      const auto token = block_match[1].str();
      const auto value = static_cast<std::size_t>(
          std::stoull(token, nullptr, token.starts_with("0x") ? 16 : 10));
      stats.max_block_length =
          std::max(stats.max_block_length.value_or(0), value);
    }
  }
}

TransferStats transfer_stats(
    const std::vector<ClassifiedRow>& summary,
    const std::vector<ClassifiedRow>& transcript, Stage stage, unsigned cycle) {
  TransferStats stats;
  stats.status = stage_status(summary, transcript, stage, cycle);
  std::optional<std::string> active_block;
  std::optional<unsigned> previous_sequence;
  collect_transfer_metadata(stats, summary, stage, cycle);
  for (const auto& item : transcript) {
    if (item.stage != stage || item.cycle != cycle) continue;
    const auto& row = *item.row;
    const std::string text = row.step + " " + row.detail;
    if (stats.started == std::chrono::system_clock::time_point{} ||
        row.timestamp < stats.started)
      stats.started = row.timestamp;
    stats.finished = std::max(stats.finished, row.timestamp);
    if (const auto reference = transfer_block_ref(text)) {
      const auto& region = reference->first;
      const auto index = reference->second.first;
      const auto total = reference->second.second;
      const auto key = lower_ascii(region) + "#" + std::to_string(index);
      auto& block = stats.blocks[key];
      block.region = region;
      block.index = index;
      block.total = total;
      block.completed = block.completed || verdict_rank(row.verdict) == 2 ||
                        lower_ascii(text).find("pass") != std::string::npos;
      stats.regions.insert(region);
      stats.region_totals[lower_ascii(region)] =
          std::max(stats.region_totals[lower_ascii(region)], total);
      active_block = key;
    }
    if (const auto sequence = parse_sequence(text); sequence && active_block) {
      auto& block = stats.blocks[*active_block];
      ++block.tx_count;
      block.sequence = *sequence;
      block.has_sequence = true;
      block.payload_bytes =
          std::max(block.payload_bytes, parse_request_payload_bytes(text));
      if (!stats.first_sequence) stats.first_sequence = sequence;
      if (previous_sequence && *previous_sequence == 0xFFU && *sequence == 0U)
        stats.rollover = true;
      previous_sequence = sequence;
      stats.last_sequence = sequence;
    }
    if (lower_ascii(text).find("nrc 0x78") != std::string::npos &&
        active_block)
      ++stats.pending_count;
  }
  return stats;
}

std::string sequence_text(const std::optional<unsigned>& sequence) {
  if (!sequence) return "未记录";
  std::ostringstream value;
  value << "0x" << std::uppercase << std::hex << std::setfill('0')
        << std::setw(2) << *sequence;
  return value.str();
}

void append_metric(std::ostringstream& html, std::string_view label,
                   const std::string& value) {
  html << "<div class='metric'><span>" << escape(std::string(label))
       << "</span><strong>" << escape(value) << "</strong></div>";
}

void append_transfer_stage(
    std::ostringstream& html, std::string_view id, std::string_view role,
    const std::vector<ClassifiedRow>& summary,
    const std::vector<ClassifiedRow>& transcript, Stage stage, unsigned cycle) {
  const auto stats = transfer_stats(summary, transcript, stage, cycle);
  unsigned total_blocks{};
  for (const auto& item : stats.region_totals) total_blocks += item.second;
  const auto completed_blocks = static_cast<unsigned>(std::count_if(
      stats.blocks.begin(), stats.blocks.end(),
      [](const auto& item) { return item.second.completed; }));
  unsigned retries{};
  std::size_t total_bytes{}, max_payload{}, last_payload{};
  for (const auto& item : stats.blocks) {
    const auto& block = item.second;
    if (block.tx_count > 1) retries += block.tx_count - 1;
    total_bytes += block.payload_bytes;
    max_payload = std::max(max_payload, block.payload_bytes);
    if (!stats.last_sequence ||
        (block.has_sequence && block.sequence == *stats.last_sequence))
      last_payload = block.payload_bytes;
  }
  const auto duration =
      stats.started == std::chrono::system_clock::time_point{} ||
              stats.finished == std::chrono::system_clock::time_point{}
          ? std::string("未记录")
          : std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                               stats.finished - stats.started)
                               .count()) +
                " ms";
  const auto progress = total_blocks == 0
                            ? std::string("未记录")
                            : std::to_string(completed_blocks) + "/" +
                                  std::to_string(total_blocks) + " 块";
  html << "<section class='sub-stage transfer-stage' id='" << id << "'><h5>"
       << stage_title("36服务：" + std::string(role) + "数据传输（" +
                          progress + "）",
                      stats.status)
       << "</h5><div class='metrics'>";
  append_metric(html, "文件或区域名称", join(stats.regions));
  append_metric(html, "起始地址/区域", join(stats.addresses));
  append_metric(html, "总字节数",
                total_bytes == 0 ? "未记录" : std::to_string(total_bytes));
  append_metric(html, "总块数",
                total_blocks == 0 ? "未记录" : std::to_string(total_blocks));
  append_metric(html, "已完成块数", std::to_string(completed_blocks));
  append_metric(html, "单块最大长度",
                stats.max_block_length
                    ? std::to_string(*stats.max_block_length) + " bytes"
                    : "未记录");
  append_metric(html, "实际数据负载长度",
                max_payload == 0
                    ? "未记录"
                    : "最大 " + std::to_string(max_payload) +
                          " bytes；末块 " + std::to_string(last_payload) +
                          " bytes");
  append_metric(html, "起始块序号", sequence_text(stats.first_sequence));
  append_metric(html, "结束块序号", sequence_text(stats.last_sequence));
  append_metric(html, "FF → 00 回绕", stats.rollover ? "是" : "否");
  append_metric(html, "重试次数", std::to_string(retries));
  append_metric(html, "NRC 0x78 次数", std::to_string(stats.pending_count));
  append_metric(html, "开始时间", full_time(stats.started));
  append_metric(html, "结束时间", full_time(stats.finished));
  append_metric(html, "耗时", duration);
  append_metric(html, "最终结果", stats.status.label);
  html << "</div>";
  const auto raw_rows = rows_for(transcript, stage, cycle);
  html << "<details class='transfer-log'><summary>展开全部数据块日志（"
       << raw_rows.size() << " 条）</summary>";
  append_table(html, raw_rows, "本次未产生36/76数据块记录。");
  html << "</details>";
  append_back_link(html);
  html << "</section>";
}

std::vector<TraceEvidence> trace_evidence(
    const std::vector<ClassifiedRow>& summary) {
  std::vector<TraceEvidence> result;
  static const std::regex pattern(
      R"(raw\s+(ASC|BLF)\s+(PASS|FAIL):\s*([^;]+))",
      std::regex::icase);
  for (const auto& item : summary) {
    if (item.stage != Stage::trace_evidence) continue;
    const auto& detail = item.row->detail;
    for (std::sregex_iterator it(detail.begin(), detail.end(), pattern), end;
         it != end; ++it) {
      auto path = (*it)[3].str();
      while (!path.empty() &&
             std::isspace(static_cast<unsigned char>(path.back())))
        path.pop_back();
      const std::u8string encoded(
          reinterpret_cast<const char8_t*>(path.data()), path.size());
      result.push_back({item.cycle, (*it)[1].str(),
                        std::filesystem::path(encoded), item.row->timestamp});
    }
  }
  return result;
}

std::chrono::system_clock::time_point file_time(
    const std::filesystem::path& path) {
  std::error_code error;
  const auto value = std::filesystem::last_write_time(path, error);
  if (error) return {};
  return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      value - std::filesystem::file_time_type::clock::now() +
      std::chrono::system_clock::now());
}

StageStatus trace_status(const std::vector<TraceEvidence>& evidence) {
  if (evidence.empty()) return {};
  bool all_final = true;
  for (const auto& item : evidence) {
    std::error_code error;
    const auto exists = std::filesystem::is_regular_file(item.path, error);
    const auto size = exists && !error
                          ? std::filesystem::file_size(item.path, error)
                          : 0;
    all_final = all_final && exists && !error && size > 0;
  }
  return all_final ? status_from_rank(2) : status_from_rank(1);
}

void append_trace_section(std::ostringstream& html,
                          const std::vector<TraceEvidence>& evidence,
                          unsigned cycles) {
  const auto status = trace_status(evidence);
  html << "<section class='report-section' id='trace-evidence'><h3>"
       << stage_title("ASC与BLF证据文件", status)
       << "</h3><p class='section-note'>PASS仅表示报告生成时文件已存在且非空；若只能确认开始录制，则显示INFO。</p>";
  for (unsigned cycle = 1; cycle <= cycles; ++cycle)
    html << "<span class='anchor-alias' id='" << cycle_id(cycle, "trace-evidence")
         << "'></span>";
  if (evidence.empty()) {
    html << "<p class='not-run'>本次未记录ASC/BLF证据文件。</p>";
  } else {
    html << "<div class='table-wrap'><table><thead><tr><th>轮次</th><th>文件类型</th>"
            "<th>文件名称</th><th>路径</th><th>是否存在</th><th>文件大小</th>"
            "<th>录制开始</th><th>录制结束</th><th>最终状态</th></tr></thead><tbody>";
    for (const auto& item : evidence) {
      std::error_code error;
      const auto exists = std::filesystem::is_regular_file(item.path, error);
      const auto size = exists && !error
                            ? std::filesystem::file_size(item.path, error)
                            : 0;
      const auto finalized = exists && !error && size > 0;
      const auto item_status = finalized ? status_from_rank(2)
                                         : status_from_rank(1);
      html << "<tr><td>" << item.cycle << "</td><td>" << escape(item.type)
           << "</td><td>" << escape(path_utf8(item.path.filename()))
           << "</td><td><pre>" << escape(path_utf8(item.path))
           << "</pre></td><td>" << (exists && !error ? "是" : "否")
           << "</td><td>"
           << (finalized ? std::to_string(size) + " bytes" : "未记录")
           << "</td><td>" << full_time(item.started) << "</td><td>"
           << (finalized ? full_time(file_time(item.path)) : "未确认关闭")
           << "</td><td>" << status_html(item_status) << "</td></tr>";
    }
    html << "</tbody></table></div>";
  }
  append_back_link(html);
  html << "</section>";
}

void append_nav_link(std::ostringstream& html, std::string_view href,
                     std::string_view label, const StageStatus& status,
                     bool child = false) {
  html << "<a class='nav-link" << (child ? " child" : "") << "' href='#"
       << href << "'>" << status_html(status) << "<span>"
       << escape(std::string(label)) << "</span></a>";
}

StageStatus aggregate_status(const std::vector<ClassifiedRow>& summary,
                             const std::vector<ClassifiedRow>& transcript,
                             std::initializer_list<Stage> stages,
                             unsigned cycle = 0) {
  int rank = 0;
  for (const auto stage : stages)
    rank = std::max(rank, verdict_rank(
                              stage_status(summary, transcript, stage, cycle)
                                  .verdict));
  return status_from_rank(rank);
}

void append_navigation(std::ostringstream& html,
                       const std::vector<ClassifiedRow>& summary,
                       const std::vector<ClassifiedRow>& transcript,
                       const std::vector<TraceEvidence>& traces,
                       unsigned cycles) {
  const auto one = [&](Stage stage) {
    return stage_status(summary, transcript, stage);
  };
  const auto driver = aggregate_status(
      summary, transcript,
      {Stage::driver_request_download, Stage::driver_transfer,
       Stage::driver_transfer_exit, Stage::driver_verification});
  const auto app = aggregate_status(
      summary, transcript,
      {Stage::app_request_download, Stage::app_transfer,
       Stage::app_transfer_exit, Stage::app_verification});
  html << "<nav class='report-nav' id='report-navigation' aria-label='报告导航'>"
          "<div class='nav-title'>报告导航</div><div class='nav-grid'>";
  append_nav_link(html, "configuration", "1. 基本配置与文件检查",
                  one(Stage::configuration));
  append_nav_link(html, "pre-flash-check", "2. 刷写前条件检查",
                  one(Stage::pre_flash_check));
  append_nav_link(html, "programming-session", "3. 进入编程会话",
                  one(Stage::programming_session));
  append_nav_link(html, "programming-session", "10服务：会话控制",
                  one(Stage::programming_session), true);
  append_nav_link(html, "security-access", "27服务：安全访问",
                  one(Stage::security_access), true);
  append_nav_link(html, "driver-download", "4. Driver下载", driver);
  append_nav_link(html, "driver-request-download", "34服务：请求下载",
                  one(Stage::driver_request_download), true);
  append_nav_link(html, "driver-transfer", "36服务：数据传输",
                  one(Stage::driver_transfer), true);
  append_nav_link(html, "driver-transfer-exit", "37服务：退出传输",
                  one(Stage::driver_transfer_exit), true);
  append_nav_link(html, "driver-verification", "完整性校验",
                  one(Stage::driver_verification), true);
  append_nav_link(html, "app-download", "5. APP下载", app);
  append_nav_link(html, "app-request-download", "34服务：请求下载",
                  one(Stage::app_request_download), true);
  append_nav_link(html, "app-transfer", "36服务：数据传输",
                  one(Stage::app_transfer), true);
  append_nav_link(html, "app-transfer-exit", "37服务：退出传输",
                  one(Stage::app_transfer_exit), true);
  append_nav_link(html, "app-verification", "完整性校验",
                  one(Stage::app_verification), true);
  append_nav_link(html, "dependency-check", "6. 依赖性检查",
                  one(Stage::dependency_check));
  append_nav_link(html, "ecu-reset-recovery", "7. ECU复位与恢复",
                  one(Stage::ecu_reset_recovery));
  append_nav_link(html, "post-flash-verification", "8. 刷写后验证",
                  one(Stage::post_flash_verification));
  append_nav_link(html, "trace-evidence", "9. ASC与BLF证据文件",
                  trace_status(traces));
  append_nav_link(html, "raw-log", "10. 完整原始日志",
                  transcript.empty() ? status_from_rank(0) : status_from_rank(1));
  html << "</div>";
  if (cycles > 1) {
    html << "<div class='cycle-nav'><strong>按刷写轮次定位</strong>";
    for (unsigned cycle = 1; cycle <= cycles; ++cycle) {
      html << "<div class='cycle-nav-row'><a class='cycle-link' href='#cycle-"
           << cycle << "'>第" << cycle << "/" << cycles << "轮</a>";
      append_nav_link(html, cycle_id(cycle, "programming-session"),
                      "10会话", stage_status(summary, transcript,
                                                   Stage::programming_session,
                                                   cycle), true);
      append_nav_link(html, cycle_id(cycle, "security-access"), "27安全访问",
                      stage_status(summary, transcript, Stage::security_access,
                                   cycle), true);
      append_nav_link(html, cycle_id(cycle, "driver-transfer"), "Driver 36传输",
                      stage_status(summary, transcript, Stage::driver_transfer,
                                   cycle), true);
      append_nav_link(html, cycle_id(cycle, "app-transfer"), "APP 36传输",
                      stage_status(summary, transcript, Stage::app_transfer,
                                   cycle), true);
      append_nav_link(html, cycle_id(cycle, "ecu-reset-recovery"), "ECU恢复",
                      stage_status(summary, transcript,
                                   Stage::ecu_reset_recovery, cycle), true);
      html << "</div>";
    }
    html << "</div>";
  }
  html << "</nav>";
}

void append_first_cycle_aliases(std::ostringstream& html) {
  for (const auto id : {"programming-session", "security-access",
                        "driver-download", "driver-request-download",
                        "driver-transfer", "driver-transfer-exit",
                        "driver-verification", "app-download",
                        "app-request-download", "app-transfer",
                        "app-transfer-exit", "app-verification",
                        "dependency-check", "ecu-reset-recovery",
                        "post-flash-verification"})
    html << "<span class='anchor-alias' id='" << id << "'></span>";
}

void append_cycle(std::ostringstream& html,
                  const std::vector<ClassifiedRow>& summary,
                  const std::vector<ClassifiedRow>& transcript,
                  unsigned cycle, unsigned cycles) {
  if (cycle == 1) append_first_cycle_aliases(html);
  html << "<section class='cycle-section' id='cycle-" << cycle << "'><h3>"
       << stage_title("第" + std::to_string(cycle) + "/" +
                          std::to_string(cycles) + "轮刷写",
                      stage_status(summary, transcript, Stage::cycle_overview,
                                   cycle))
       << "</h3>";
  append_table(html, rows_for(summary, Stage::cycle_overview, cycle),
               "本轮没有独立概要行，请查看各阶段与完整原始日志。");
  append_simple_stage(
      html, cycle_id(cycle, "programming-session"), "10服务：会话控制",
      stage_status(summary, transcript, Stage::programming_session, cycle),
      rows_for(summary, Stage::programming_session, cycle), 4);
  append_simple_stage(
      html, cycle_id(cycle, "security-access"), "27服务：安全访问",
      stage_status(summary, transcript, Stage::security_access, cycle),
      rows_for(summary, Stage::security_access, cycle), 4);

  const auto driver_status = aggregate_status(
      summary, transcript,
      {Stage::driver_request_download, Stage::driver_transfer,
       Stage::driver_transfer_exit, Stage::driver_verification}, cycle);
  html << "<section class='download-section' id='"
       << cycle_id(cycle, "driver-download") << "'><h4>"
       << stage_title("Driver下载", driver_status) << "</h4>";
  append_simple_stage(
      html, cycle_id(cycle, "driver-request-download"), "34服务：请求下载",
      stage_status(summary, transcript, Stage::driver_request_download, cycle),
      rows_for(summary, Stage::driver_request_download, cycle), 5);
  append_transfer_stage(html, cycle_id(cycle, "driver-transfer"), "Driver",
                        summary, transcript, Stage::driver_transfer, cycle);
  append_simple_stage(
      html, cycle_id(cycle, "driver-transfer-exit"), "37服务：退出传输",
      stage_status(summary, transcript, Stage::driver_transfer_exit, cycle),
      rows_for(summary, Stage::driver_transfer_exit, cycle), 5);
  append_simple_stage(
      html, cycle_id(cycle, "driver-verification"), "Driver完整性校验",
      stage_status(summary, transcript, Stage::driver_verification, cycle),
      rows_for(summary, Stage::driver_verification, cycle), 5);
  html << "</section>";

  const auto app_status = aggregate_status(
      summary, transcript,
      {Stage::app_request_download, Stage::app_transfer,
       Stage::app_transfer_exit, Stage::app_verification}, cycle);
  html << "<section class='download-section' id='"
       << cycle_id(cycle, "app-download") << "'><h4>"
       << stage_title("APP下载", app_status) << "</h4>";
  append_simple_stage(
      html, cycle_id(cycle, "app-request-download"), "34服务：请求下载",
      stage_status(summary, transcript, Stage::app_request_download, cycle),
      rows_for(summary, Stage::app_request_download, cycle), 5);
  append_transfer_stage(html, cycle_id(cycle, "app-transfer"), "APP", summary,
                        transcript, Stage::app_transfer, cycle);
  append_simple_stage(
      html, cycle_id(cycle, "app-transfer-exit"), "37服务：退出传输",
      stage_status(summary, transcript, Stage::app_transfer_exit, cycle),
      rows_for(summary, Stage::app_transfer_exit, cycle), 5);
  append_simple_stage(
      html, cycle_id(cycle, "app-verification"), "APP完整性校验",
      stage_status(summary, transcript, Stage::app_verification, cycle),
      rows_for(summary, Stage::app_verification, cycle), 5);
  html << "</section>";
  append_simple_stage(
      html, cycle_id(cycle, "dependency-check"), "依赖性检查",
      stage_status(summary, transcript, Stage::dependency_check, cycle),
      rows_for(summary, Stage::dependency_check, cycle), 4);
  append_simple_stage(
      html, cycle_id(cycle, "ecu-reset-recovery"), "ECU复位与恢复",
      stage_status(summary, transcript, Stage::ecu_reset_recovery, cycle),
      rows_for(summary, Stage::ecu_reset_recovery, cycle), 4);
  append_simple_stage(
      html, cycle_id(cycle, "post-flash-verification"), "刷写后验证",
      stage_status(summary, transcript, Stage::post_flash_verification, cycle),
      rows_for(summary, Stage::post_flash_verification, cycle), 4);
  append_back_link(html);
  html << "</section>";
}

} // namespace

void HtmlReport::append_row(std::vector<ReportRow>& rows, ReportRow row) {
  if (row.timestamp == std::chrono::system_clock::time_point{})
    row.timestamp = std::chrono::system_clock::now();
  if (!rows.empty() && row.timestamp < rows.back().timestamp)
    row.timestamp = rows.back().timestamp;
  rows.push_back(std::move(row));
}

void HtmlReport::add(ReportRow row) { append_row(rows_, std::move(row)); }

void HtmlReport::add_transcript(ReportRow row) {
  append_row(transcript_rows_, std::move(row));
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

  const auto cycles = repeat_count(rows_, transcript_rows_);
  const auto summary = classify_summary(rows_);
  const auto transcript = classify_transcript(transcript_rows_, cycles);
  const auto traces = trace_evidence(summary);
  int overall_rank = 0;
  for (const auto& row : rows_)
    overall_rank = std::max(overall_rank, verdict_rank(row.verdict));
  const auto has_failure = overall_rank >= 4;
  const auto has_warning = overall_rank == 3;
  const auto result_class = has_failure ? "FAIL" : has_warning ? "WARN" : "PASS";
  const auto result_text = has_failure
                               ? "失败（FAIL）"
                               : has_warning
                                     ? "成功，但存在警告（PASS with warnings）"
                                     : "成功（PASS）";

  std::ostringstream html;
  html << "<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
          "<meta name='viewport' content='width=device-width,initial-scale=1'>"
          "<title>"
       << escape(title) << "</title><style>"
          ":root{--pass:#087f23;--warn:#a15c00;--fail:#c62828;--info:#456579;"
          "--muted:#6b7280;--line:#d9dee3;--nav-offset:150px}"
          "*{box-sizing:border-box}html{scroll-behavior:smooth}"
          "body{font-family:'Segoe UI','Microsoft YaHei',sans-serif;margin:0;padding:24px;"
          "color:#222;background:#fff;overflow-wrap:anywhere}h2{margin-top:0}"
          "h3,h4,h5{display:flex;align-items:center;justify-content:space-between;gap:12px;"
          "scroll-margin-top:var(--nav-offset)}section,.anchor-alias,nav{scroll-margin-top:var(--nav-offset)}"
          ".anchor-alias{display:block;height:0;position:relative;top:-4px}"
          "table{border-collapse:collapse;width:100%;font-size:14px;min-width:720px}"
          "th,td{border:1px solid #ddd;padding:7px;vertical-align:top}"
          "th{background:#f4f6f8;text-align:left}pre{font-family:Consolas,'Cascadia Mono',monospace;"
          "margin:0;white-space:pre-wrap;word-break:break-word}.table-wrap{width:100%;overflow-x:auto}"
          ".PASS{color:var(--pass);font-weight:600}.FAIL{color:var(--fail);font-weight:600}"
          ".WARN{color:var(--warn);font-weight:600}.INFO{color:var(--info);font-weight:600}"
          ".NOT_RUN{color:var(--muted);font-weight:600}.result{padding:14px 16px;margin:12px 0 16px;"
          "border-radius:5px;font-size:18px}.result.PASS{background:#edf8f0;border-left:5px solid var(--pass)}"
          ".result.WARN{background:#fff7e8;border-left:5px solid var(--warn)}"
          ".result.FAIL{background:#fff0f0;border-left:5px solid var(--fail)}"
          ".time{font-size:14px;color:#444;margin-top:6px;font-weight:400}"
          ".section-note{color:#555;margin:6px 0 12px}.not-run{color:var(--muted);background:#f5f6f7;"
          "border-left:4px solid #aeb5bd;padding:9px 11px}.report-nav{position:sticky;top:0;z-index:20;"
          "background:rgba(255,255,255,.97);border:1px solid var(--line);border-radius:7px;padding:12px;"
          "margin:14px 0 18px;box-shadow:0 2px 8px rgba(0,0,0,.08);max-height:46vh;overflow:auto}"
          ".nav-title{font-size:18px;font-weight:700;margin-bottom:8px}.nav-grid{display:flex;flex-wrap:wrap;gap:6px}"
          ".nav-link,.cycle-link{display:inline-flex;align-items:center;gap:5px;padding:6px 8px;"
          "border:1px solid #d7dce1;border-radius:5px;color:#24323d;background:#fff;text-decoration:none;"
          "transition:.15s ease}.nav-link:hover,.nav-link:focus,.cycle-link:hover,.cycle-link:focus{"
          "background:#eaf3fb;border-color:#5f91b4;outline:none}.nav-link.child{font-size:13px;background:#f8fafb}"
          ".status{display:inline-flex;gap:4px;align-items:center;white-space:nowrap;font-size:12px}"
          ".symbol{font-size:14px}.cycle-nav{margin-top:10px;border-top:1px solid var(--line);padding-top:8px}"
          ".cycle-nav-row{display:flex;flex-wrap:wrap;gap:5px;margin-top:6px}.cycle-link{font-weight:700}"
          ".report-section,.cycle-section,.download-section,.sub-stage{margin:20px 0;padding:14px;"
          "border:1px solid var(--line);border-radius:7px;background:#fff}.cycle-section{background:#fbfcfd;"
          "border-color:#bac7d1}.download-section{background:#f9fbfc}.sub-stage{background:#fff}"
          ".back-link{display:inline-block;margin-top:10px;color:#1769aa;text-decoration:none}"
          ".back-link:hover{text-decoration:underline}.metrics{display:grid;grid-template-columns:"
          "repeat(auto-fit,minmax(210px,1fr));gap:8px;margin:10px 0}.metric{border:1px solid #dce2e7;"
          "border-radius:5px;padding:8px;background:#f8fafb}.metric span{display:block;color:#5f6b75;"
          "font-size:12px;margin-bottom:3px}.metric strong{font-size:14px}details{margin-top:12px;"
          "border:1px solid var(--line);border-radius:5px;padding:10px 12px}summary{cursor:pointer;"
          "font-size:16px;font-weight:600}details>.table-wrap{margin-top:12px}"
          "@media(max-width:760px){body{padding:12px;--nav-offset:190px}.report-nav{max-height:42vh}"
          ".nav-link{flex:1 1 220px}h3,h4,h5{align-items:flex-start;flex-direction:column}}"
          "@media print{.report-nav{position:static;max-height:none;box-shadow:none}.back-link{display:none}}"
          "</style></head><body><h2>"
       << escape(title) << "</h2><div class='result " << result_class
       << "' id='summary'>本次流程结果：" << result_text
       << "<div class='time'>完成时间：" << std::put_time(&local, "%Y-%m-%d %H:%M:%S")
       << "</div></div>";

  append_navigation(html, summary, transcript, traces, cycles);
  html << "<h3>执行摘要与证据索引</h3>"
          "<p class='section-note'>摘要行按真实执行记录归入各阶段；未执行显示“—”，不会伪装成PASS。完整原始日志在报告底部按原顺序保留。</p>";
  append_simple_stage(html, "configuration", "基本配置与文件检查",
                      stage_status(summary, transcript, Stage::configuration),
                      rows_for(summary, Stage::configuration));
  append_simple_stage(
      html, "pre-flash-check", "刷写前条件检查",
      stage_status(summary, transcript, Stage::pre_flash_check),
      rows_for(summary, Stage::pre_flash_check));
  for (unsigned cycle = 1; cycle <= cycles; ++cycle)
    append_cycle(html, summary, transcript, cycle, cycles);
  append_trace_section(html, traces, cycles);
  const auto other_rows = rows_for(summary, Stage::other);
  if (!other_rows.empty())
    append_simple_stage(html, "other-records", "其他执行摘要",
                        status_from_rank(1), other_rows);

  html << "<section class='report-section' id='raw-log'><h3>"
       << stage_title("完整原始日志",
                      transcript_rows_.empty() ? status_from_rank(0)
                                               : status_from_rank(1))
       << "</h3><details class='raw-log-details'><summary>展开完整原始日志（"
       << transcript_rows_.size()
       << " 条）</summary><p class='section-note'>保留工作流原始记录的时间、方向、CAN ID、请求、响应、NRC、数据长度、执行顺序和最终判定。</p>"
          "<div class='table-wrap'><table><thead><tr><th>时间</th><th>类型</th>"
          "<th>判定</th><th>原始内容</th></tr></thead><tbody>";
  for (const auto& row : transcript_rows_)
    html << "<tr><td>" << row_time(row.timestamp) << "</td><td>"
         << escape(row.step) << "</td><td class='" << escape(row.verdict)
         << "'>" << escape(row.verdict) << "</td><td><pre>"
         << escape(row.detail) << "</pre></td></tr>";
  html << "</tbody></table></div></details>";
  append_back_link(html);
  html << "</section></body></html>";

  const auto write_file = [&](const std::filesystem::path& path) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("cannot open HTML report for writing");
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
