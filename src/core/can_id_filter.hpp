#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uds {

enum class CanIdFilterErrorCode {
  missing_expression,
  invalid_token,
  invalid_hex,
  identifier_out_of_range,
  invalid_range,
  reversed_range,
  invalid_mask,
};

struct CanIdFilterError {
  CanIdFilterErrorCode code{CanIdFilterErrorCode::invalid_token};
  std::size_t token_index{}; // One-based index after delimiter normalization.
  std::string token;
};

class CanIdFilter final {
public:
  [[nodiscard]] bool matches(std::uint32_t can_id) const noexcept;
  [[nodiscard]] bool empty() const noexcept { return rules_.empty(); }

private:
  enum class RuleKind { exact, range, mask };

  struct Rule {
    RuleKind kind{RuleKind::exact};
    bool excluded{};
    std::uint32_t first{};
    std::uint32_t second{};

    [[nodiscard]] bool matches(std::uint32_t can_id) const noexcept;
  };

  friend std::variant<CanIdFilter, CanIdFilterError>
  parse_can_id_filter(std::string_view expression);

  std::vector<Rule> rules_;
};

using CanIdFilterParseResult =
    std::variant<CanIdFilter, CanIdFilterError>;

// Grammar (case-insensitive hexadecimal): exact 772, range 700-7FF,
// nibble mask 18DAxxxx, exclusion !520. Tokens may be separated by comma,
// Chinese comma/dunhao, or ASCII whitespace.
[[nodiscard]] CanIdFilterParseResult
parse_can_id_filter(std::string_view expression);

} // namespace uds
