#include "detail/xml_config.hpp"

#include <array>
#include <cfenv>
#include <cmath>
#include <exception>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

#include "netft/discovery.hpp"

namespace netft::detail {
namespace {

constexpr std::size_t kMaximumFieldLength = 128;
constexpr std::array<std::string_view, 5> kRequiredTags{"prodname", "cfgcpf", "cfgcpt", "scfgfu",
                                                        "scfgtu"};

bool is_ascii_whitespace(char character) noexcept {
  return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
         character == '\f' || character == '\v';
}

std::string_view trim_ascii_whitespace(std::string_view value) noexcept {
  while (!value.empty() && is_ascii_whitespace(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && is_ascii_whitespace(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

int required_tag_index(std::string_view name) noexcept {
  for (std::size_t index = 0; index < kRequiredTags.size(); ++index) {
    if (name == kRequiredTags[index]) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

std::size_t find_tag_end(std::string_view xml, std::size_t start) {
  char quote{};
  for (std::size_t position = start; position < xml.size(); ++position) {
    const char character = xml[position];
    if (quote != '\0') {
      if (character == quote) {
        quote = '\0';
      }
    } else if (character == '\'' || character == '"') {
      quote = character;
    } else if (character == '>') {
      return position;
    }
  }
  throw DiscoveryError("sensor configuration contains unterminated markup");
}

struct OpenElement {
  std::string_view name;
  int required_index{-1};
};

using RequiredFields = std::array<std::string, kRequiredTags.size()>;

class ScopedRoundToNearest {
public:
  ScopedRoundToNearest() {
    if (std::feholdexcept(&environment_) != 0) {
      throw DiscoveryError("failed to hold the floating-point environment");
    }
    active_ = true;

    const int rounding_mode = std::fegetround();
    if (rounding_mode == -1) {
      restore_environment_or_terminate();
      throw DiscoveryError("failed to inspect the floating-point rounding mode");
    }
    if (rounding_mode != FE_TONEAREST && std::fesetround(FE_TONEAREST) != 0) {
      restore_environment_or_terminate();
      throw DiscoveryError("failed to select round-to-nearest floating-point parsing");
    }
  }

  ~ScopedRoundToNearest() {
    if (active_) {
      restore_environment_or_terminate();
    }
  }

  ScopedRoundToNearest(const ScopedRoundToNearest &) = delete;
  ScopedRoundToNearest &operator=(const ScopedRoundToNearest &) = delete;

private:
  void restore_environment_or_terminate() noexcept {
    active_ = false;
    if (std::fesetenv(&environment_) != 0) {
      std::terminate();
    }
  }

  std::fenv_t environment_{};
  bool active_{false};
};

void append_required_text(const std::vector<OpenElement> &elements, RequiredFields &fields,
                          std::string_view text) {
  if (!elements.empty() && elements.back().required_index >= 0) {
    fields[static_cast<std::size_t>(elements.back().required_index)].append(text);
  }
}

RequiredFields extract_required_fields(std::string_view xml) {
  RequiredFields fields;
  std::array<bool, kRequiredTags.size()> seen{};
  std::vector<OpenElement> elements;
  std::size_t position{};

  while (position < xml.size()) {
    const auto markup = xml.find('<', position);
    if (markup == std::string_view::npos) {
      append_required_text(elements, fields, xml.substr(position));
      position = xml.size();
      break;
    }
    append_required_text(elements, fields, xml.substr(position, markup - position));

    if (xml.substr(markup, 4) == "<!--") {
      const auto end = xml.find("-->", markup + 4);
      if (end == std::string_view::npos) {
        throw DiscoveryError("sensor configuration contains an unterminated comment");
      }
      position = end + 3;
      continue;
    }
    if (xml.substr(markup, 9) == "<![CDATA[") {
      const auto end = xml.find("]]>", markup + 9);
      if (end == std::string_view::npos) {
        throw DiscoveryError("sensor configuration contains unterminated CDATA");
      }
      append_required_text(elements, fields, xml.substr(markup + 9, end - markup - 9));
      position = end + 3;
      continue;
    }
    if (xml.substr(markup, 2) == "<?") {
      const auto end = xml.find("?>", markup + 2);
      if (end == std::string_view::npos) {
        throw DiscoveryError("sensor configuration contains unterminated markup");
      }
      position = end + 2;
      continue;
    }
    if (xml.substr(markup, 2) == "<!") {
      throw DiscoveryError("sensor configuration contains unsupported markup");
    }

    const auto end = find_tag_end(xml, markup + 1);
    auto tag = trim_ascii_whitespace(xml.substr(markup + 1, end - markup - 1));
    if (tag.empty()) {
      throw DiscoveryError("sensor configuration contains malformed markup");
    }

    if (tag.front() == '/') {
      const auto name = trim_ascii_whitespace(tag.substr(1));
      if (name.empty() || name.find_first_of(" \t\r\n\f\v/") != std::string_view::npos ||
          elements.empty() || elements.back().name != name) {
        throw DiscoveryError("sensor configuration contains malformed element nesting");
      }
      elements.pop_back();
      position = end + 1;
      continue;
    }

    bool self_closing = false;
    if (tag.back() == '/') {
      self_closing = true;
      tag = trim_ascii_whitespace(tag.substr(0, tag.size() - 1));
    }
    const auto name_end = tag.find_first_of(" \t\r\n\f\v");
    const auto name = tag.substr(0, name_end);
    if (name.empty() || name.front() == '/') {
      throw DiscoveryError("sensor configuration contains malformed markup");
    }

    const int required_index = required_tag_index(name);
    if (!elements.empty() && elements.back().required_index >= 0) {
      throw DiscoveryError("sensor configuration fields must contain text only");
    }
    if (required_index >= 0) {
      const auto index = static_cast<std::size_t>(required_index);
      if (seen[index]) {
        throw DiscoveryError("sensor configuration field '" + std::string{kRequiredTags[index]} +
                             "' must appear exactly once");
      }
      seen[index] = true;
    }
    if (!self_closing) {
      elements.push_back(OpenElement{name, required_index});
    }
    position = end + 1;
  }

  if (!elements.empty()) {
    throw DiscoveryError("sensor configuration contains malformed element nesting");
  }
  for (std::size_t index = 0; index < kRequiredTags.size(); ++index) {
    if (!seen[index]) {
      throw DiscoveryError("sensor configuration field '" + std::string{kRequiredTags[index]} +
                           "' must appear exactly once");
    }
    const auto value = trim_ascii_whitespace(fields[index]);
    if (value.empty()) {
      throw DiscoveryError("sensor configuration field '" + std::string{kRequiredTags[index]} +
                           "' is empty");
    }
    if (value.size() > kMaximumFieldLength) {
      throw DiscoveryError("sensor configuration field '" + std::string{kRequiredTags[index]} +
                           "' is too long");
    }
    fields[index] = std::string{value};
  }
  return fields;
}

double parse_positive_count(std::string_view value, std::string_view tag) {
  const ScopedRoundToNearest rounding_mode;
  double result{};
  std::istringstream input{std::string{value}};
  input.imbue(std::locale::classic());
  input >> std::noskipws >> result;
  if (value.empty() || value.front() == '+' || !input ||
      input.peek() != std::char_traits<char>::eof() || !std::isfinite(result) || result <= 0.0) {
    throw DiscoveryError("sensor configuration field '" + std::string{tag} +
                         "' must be a finite positive number");
  }
  return result;
}

std::string_view normalize_torque_spelling(std::string_view value) noexcept {
  if (value == "Nm") {
    return "N-m";
  }
  if (value == "Nmm") {
    return "N-mm";
  }
  if (value == "kg-cm") {
    return "kgf-cm";
  }
  return value;
}

ForceUnit parse_force_unit(std::string_view value) {
  const auto unit = force_unit_from_string(value);
  if (!unit.has_value() || *unit == ForceUnit::Unknown) {
    throw DiscoveryError("sensor configuration contains an unknown force unit");
  }
  return *unit;
}

TorqueUnit parse_torque_unit(std::string_view value) {
  const auto unit = torque_unit_from_string(normalize_torque_spelling(value));
  if (!unit.has_value() || *unit == TorqueUnit::Unknown) {
    throw DiscoveryError("sensor configuration contains an unknown torque unit");
  }
  return *unit;
}

} // namespace

SensorConfiguration parse_sensor_configuration(std::string_view xml) {
  const auto fields = extract_required_fields(xml);
  SensorConfiguration configuration;
  configuration.product_name = fields[0];
  configuration.calibration.counts_per_force_unit = parse_positive_count(fields[1], "cfgcpf");
  configuration.calibration.counts_per_torque_unit = parse_positive_count(fields[2], "cfgcpt");
  configuration.calibration.force_unit = parse_force_unit(fields[3]);
  configuration.calibration.torque_unit = parse_torque_unit(fields[4]);
  configuration.source = CalibrationSource::Sensor;
  configuration.revision = 1;
  return configuration;
}

} // namespace netft::detail
