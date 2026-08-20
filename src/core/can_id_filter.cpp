#include "core/can_id_filter.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <optional>

namespace uds {
namespace {

constexpr std::uint32_t kMaximumCanId = 0x1FFFFFFF;

bool is_hex(char character) {
  return std::isxdigit(static_cast<unsigned char>(character)) != 0;
}

std::optional<std::uint32_t> parse_hex(std::string_view token) {
  if (token.starts_with("0x") || token.starts_with("0X")) token.remove_prefix(2);
  if (token.empty() || token.size() > 8 ||
      !std::all_of(token.begin(), token.end(), is_hex)) {
    return std::nullopt;
  }
  std::uint32_t value{};
  const auto result = std::from_chars(token.data(), token.data() + token.size(),
                                      value, 16);
  if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
    return std::nullopt;
  }
  return value;
}

void replace_utf8_delimiter(std::string& text, std::string_view delimiter) {
  std::size_t offset{};
  while ((offset = text.find(delimiter, offset)) != std::string::npos) {
    text.replace(offset, delimiter.size(), " ");
  }
}

std::vector<std::string> tokenize(std::string_view expression) {
  std::string normalized(expression);
  replace_utf8_delimiter(normalized, "、");
  replace_utf8_delimiter(normalized, "，");
  std::replace(normalized.begin(), normalized.end(), ',', ' ');

  std::vector<std::string> tokens;
  std::size_t begin{};
  while (begin < normalized.size()) {
    while (begin < normalized.size() &&
           std::isspace(static_cast<unsigned char>(normalized[begin]))) {
      ++begin;
    }
    if (begin == normalized.size()) break;
    auto end = begin;
    while (end < normalized.size() &&
           !std::isspace(static_cast<unsigned char>(normalized[end]))) {
      ++end;
    }
    tokens.emplace_back(normalized.substr(begin, end - begin));
    begin = end;
  }
  return tokens;
}

CanIdFilterError error(CanIdFilterErrorCode code, std::size_t index,
                       std::string token) {
  return {code, index, std::move(token)};
}

} // namespace

bool CanIdFilter::Rule::matches(std::uint32_t can_id) const noexcept {
  switch (kind) {
  case RuleKind::exact:
    return can_id == first;
  case RuleKind::range:
    return can_id >= first && can_id <= second;
  case RuleKind::mask:
    return (can_id & second) == first;
  }
  return false;
}

bool CanIdFilter::matches(std::uint32_t can_id) const noexcept {
  if (can_id > kMaximumCanId) return false;
  const auto has_inclusion = std::any_of(
      rules_.begin(), rules_.end(), [](const Rule& rule) {
        return !rule.excluded;
      });
  bool included = !has_inclusion;
  for (const auto& rule : rules_) {
    if (!rule.matches(can_id)) continue;
    if (rule.excluded) return false;
    included = true;
  }
  return included;
}

CanIdFilterParseResult parse_can_id_filter(std::string_view expression) {
  CanIdFilter filter;
  const auto tokens = tokenize(expression);
  if (tokens.empty()) return filter;

  for (std::size_t index = 0; index < tokens.size(); ++index) {
    auto token = std::string_view(tokens[index]);
    bool excluded{};
    if (token.starts_with('!')) {
      excluded = true;
      token.remove_prefix(1);
      if (token.empty()) {
        return error(CanIdFilterErrorCode::missing_expression, index + 1,
                     tokens[index]);
      }
    }

    const auto hyphen = token.find('-');
    const auto contains_mask =
        token.find('x') != std::string_view::npos ||
        token.find('X') != std::string_view::npos;
    CanIdFilter::Rule rule;
    rule.excluded = excluded;

    if (hyphen != std::string_view::npos) {
      if (contains_mask || hyphen == 0 || hyphen + 1 == token.size() ||
          token.find('-', hyphen + 1) != std::string_view::npos) {
        return error(CanIdFilterErrorCode::invalid_range, index + 1,
                     tokens[index]);
      }
      const auto first = parse_hex(token.substr(0, hyphen));
      const auto second = parse_hex(token.substr(hyphen + 1));
      if (!first || !second) {
        return error(CanIdFilterErrorCode::invalid_hex, index + 1,
                     tokens[index]);
      }
      if (*first > kMaximumCanId || *second > kMaximumCanId) {
        return error(CanIdFilterErrorCode::identifier_out_of_range, index + 1,
                     tokens[index]);
      }
      if (*first > *second) {
        return error(CanIdFilterErrorCode::reversed_range, index + 1,
                     tokens[index]);
      }
      rule.kind = CanIdFilter::RuleKind::range;
      rule.first = *first;
      rule.second = *second;
    } else if (contains_mask) {
      if (token.starts_with("0x") || token.starts_with("0X")) {
        token.remove_prefix(2);
      }
      if (token.empty() || token.size() > 8 ||
          !std::all_of(token.begin(), token.end(), [](char character) {
            return is_hex(character) || character == 'x' || character == 'X';
          })) {
        return error(CanIdFilterErrorCode::invalid_mask, index + 1,
                     tokens[index]);
      }
      std::uint32_t value{};
      std::uint32_t mask{};
      for (const auto character : token) {
        value <<= 4U;
        mask <<= 4U;
        if (character == 'x' || character == 'X') continue;
        const auto nibble = parse_hex(std::string_view(&character, 1));
        value |= *nibble;
        mask |= 0xFU;
      }
      if ((value & ~kMaximumCanId) != 0) {
        return error(CanIdFilterErrorCode::identifier_out_of_range, index + 1,
                     tokens[index]);
      }
      rule.kind = CanIdFilter::RuleKind::mask;
      rule.first = value;
      rule.second = mask;
    } else {
      const auto value = parse_hex(token);
      if (!value) {
        return error(CanIdFilterErrorCode::invalid_hex, index + 1,
                     tokens[index]);
      }
      if (*value > kMaximumCanId) {
        return error(CanIdFilterErrorCode::identifier_out_of_range, index + 1,
                     tokens[index]);
      }
      rule.kind = CanIdFilter::RuleKind::exact;
      rule.first = *value;
    }
    filter.rules_.push_back(rule);
  }
  return filter;
}

} // namespace uds
