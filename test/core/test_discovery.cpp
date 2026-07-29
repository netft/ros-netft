#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fenv.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <locale>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "detail/xml_config.hpp"
#include "netft/discovery.hpp"
#include "support/fake_http_server.hpp"

namespace {

constexpr std::string_view kValidXml = R"xml(
<netft><prodname>Ethernet Axia</prodname><cfgcpf>1000000</cfgcpf>
<cfgcpt>1000000</cfgcpt><scfgfu>N</scfgfu><scfgtu>Nm</scfgtu></netft>)xml";

std::string replace_value(std::string_view xml, std::string_view tag, std::string_view value) {
  std::string result{xml};
  const std::string opening = "<" + std::string{tag} + ">";
  const std::string closing = "</" + std::string{tag} + ">";
  const auto begin = result.find(opening) + opening.size();
  const auto end = result.find(closing, begin);
  result.replace(begin, end - begin, value);
  return result;
}

std::string remove_element(std::string_view xml, std::string_view tag) {
  std::string result{xml};
  const std::string opening = "<" + std::string{tag} + ">";
  const std::string closing = "</" + std::string{tag} + ">";
  const auto begin = result.find(opening);
  const auto end = result.find(closing, begin) + closing.size();
  result.erase(begin, end - begin);
  return result;
}

std::string duplicate_element(std::string_view xml, std::string_view tag) {
  std::string result{xml};
  const std::string opening = "<" + std::string{tag} + ">";
  const std::string closing = "</" + std::string{tag} + ">";
  const auto begin = result.find(opening);
  const auto end = result.find(closing, begin) + closing.size();
  result.insert(end, result.substr(begin, end - begin));
  return result;
}

std::string insert_after_root_open(std::string_view xml, std::string_view markup) {
  std::string result{xml};
  const auto position = result.find("<netft>") + std::string_view{"<netft>"}.size();
  result.insert(position, markup);
  return result;
}

class CommaDecimalPoint final : public std::numpunct<char> {
protected:
  char do_decimal_point() const override { return ','; }
};

class GlobalLocaleGuard {
public:
  explicit GlobalLocaleGuard(const std::locale &replacement)
      : original_{std::locale::global(replacement)} {}

  ~GlobalLocaleGuard() { std::locale::global(original_); }

  GlobalLocaleGuard(const GlobalLocaleGuard &) = delete;
  GlobalLocaleGuard &operator=(const GlobalLocaleGuard &) = delete;

private:
  std::locale original_;
};

class FloatingPointEnvironmentGuard {
public:
  FloatingPointEnvironmentGuard() : saved_{}, valid_{std::fegetenv(&saved_) == 0} {}

  ~FloatingPointEnvironmentGuard() {
    if (valid_) {
      static_cast<void>(std::fesetenv(&saved_));
    }
  }

  FloatingPointEnvironmentGuard(const FloatingPointEnvironmentGuard &) = delete;
  FloatingPointEnvironmentGuard &operator=(const FloatingPointEnvironmentGuard &) = delete;

private:
  std::fenv_t saved_;
  bool valid_;
};

#if defined(__linux__) && defined(__GLIBC__)
constexpr int kTrapSetupFailed = 1;
constexpr int kTrapControlUnsupported = 77;

constexpr int unavailable_trap_control_exit_code() {
#if defined(__aarch64__)
  return kTrapControlUnsupported;
#else
  return kTrapSetupFailed;
#endif
}
#endif

netft::DiscoveryOptions options_for(const FakeHttpServer &server) {
  netft::DiscoveryOptions options;
  options.sensor_host = server.host();
  options.http_port = server.port();
  options.connect_timeout = std::chrono::milliseconds{200};
  options.total_timeout = std::chrono::milliseconds{500};
  return options;
}

TEST(SensorDiscovery, RejectsInvalidPublicOptionsBeforeNetworkIo) {
  const std::vector<std::function<void(netft::DiscoveryOptions &)>> cases{
      [](auto &options) { options.sensor_host.clear(); },
      [](auto &options) { options.sensor_host = " \t\r\n"; },
      [](auto &options) { options.http_port = 0; },
      [](auto &options) { options.http_port = 65'536; },
      [](auto &options) { options.connect_timeout = std::chrono::duration<double>{0.0}; },
      [](auto &options) { options.connect_timeout = std::chrono::duration<double>{-1.0}; },
      [](auto &options) {
        options.connect_timeout =
            std::chrono::duration<double>{std::numeric_limits<double>::infinity()};
      },
      [](auto &options) {
        options.connect_timeout =
            std::chrono::duration<double>{std::numeric_limits<double>::quiet_NaN()};
      },
      [](auto &options) { options.total_timeout = std::chrono::duration<double>{0.0}; },
      [](auto &options) { options.total_timeout = std::chrono::duration<double>{-1.0}; },
      [](auto &options) {
        options.total_timeout =
            std::chrono::duration<double>{std::numeric_limits<double>::infinity()};
      },
      [](auto &options) {
        options.total_timeout =
            std::chrono::duration<double>{std::numeric_limits<double>::quiet_NaN()};
      },
  };

  for (const auto &configure : cases) {
    netft::DiscoveryOptions options;
    configure(options);
    EXPECT_THROW(netft::discover_sensor(options), netft::DiscoveryError);
  }
}

TEST(XmlConfiguration, ParsesTheRealSensorFixtureExactly) {
  const auto result = netft::detail::parse_sensor_configuration(kValidXml);

  EXPECT_EQ(result.product_name, "Ethernet Axia");
  EXPECT_DOUBLE_EQ(result.calibration.counts_per_force_unit, 1'000'000.0);
  EXPECT_DOUBLE_EQ(result.calibration.counts_per_torque_unit, 1'000'000.0);
  EXPECT_EQ(result.calibration.force_unit, netft::ForceUnit::Newton);
  EXPECT_EQ(result.calibration.torque_unit, netft::TorqueUnit::NewtonMeter);
  EXPECT_EQ(result.source, netft::CalibrationSource::Sensor);
  EXPECT_EQ(result.revision, 1U);
}

TEST(XmlConfiguration, ParsesDecimalAndScientificCountsExactly) {
  auto xml = replace_value(kValidXml, "cfgcpf", "1234.5");
  xml = replace_value(xml, "cfgcpt", "2.5e6");

  const auto result = netft::detail::parse_sensor_configuration(xml);

  EXPECT_DOUBLE_EQ(result.calibration.counts_per_force_unit, 1234.5);
  EXPECT_DOUBLE_EQ(result.calibration.counts_per_torque_unit, 2'500'000.0);
}

class RequiredXmlField : public ::testing::TestWithParam<const char *> {};

TEST_P(RequiredXmlField, RejectsMissingFields) {
  EXPECT_THROW(netft::detail::parse_sensor_configuration(remove_element(kValidXml, GetParam())),
               netft::DiscoveryError);
}

TEST_P(RequiredXmlField, RejectsDuplicateFields) {
  EXPECT_THROW(netft::detail::parse_sensor_configuration(duplicate_element(kValidXml, GetParam())),
               netft::DiscoveryError);
}

TEST_P(RequiredXmlField, RejectsEmptyFields) {
  EXPECT_THROW(
      netft::detail::parse_sensor_configuration(replace_value(kValidXml, GetParam(), " \t\r\n")),
      netft::DiscoveryError);
}

TEST_P(RequiredXmlField, RejectsFieldsLongerThan128Bytes) {
  EXPECT_THROW(netft::detail::parse_sensor_configuration(
                   replace_value(kValidXml, GetParam(), std::string(129, '1'))),
               netft::DiscoveryError);
}

INSTANTIATE_TEST_SUITE_P(EveryField, RequiredXmlField,
                         ::testing::Values("prodname", "cfgcpf", "cfgcpt", "scfgfu", "scfgtu"));

struct InvalidCountCase {
  const char *tag;
  const char *value;
};

class InvalidXmlCount : public ::testing::TestWithParam<InvalidCountCase> {};

TEST_P(InvalidXmlCount, RejectsInvalidCounts) {
  const auto &parameter = GetParam();
  EXPECT_THROW(netft::detail::parse_sensor_configuration(
                   replace_value(kValidXml, parameter.tag, parameter.value)),
               netft::DiscoveryError);
}

INSTANTIATE_TEST_SUITE_P(
    EveryInvalidForm, InvalidXmlCount,
    ::testing::Values(InvalidCountCase{"cfgcpf", "not-a-number"},
                      InvalidCountCase{"cfgcpf", "10remaining"}, InvalidCountCase{"cfgcpf", "0"},
                      InvalidCountCase{"cfgcpf", "-1"}, InvalidCountCase{"cfgcpf", "+1"},
                      InvalidCountCase{"cfgcpf", "NaN"}, InvalidCountCase{"cfgcpf", "inf"},
                      InvalidCountCase{"cfgcpf", "1e9999"}, InvalidCountCase{"cfgcpf", "1e-9999"},
                      InvalidCountCase{"cfgcpt", "not-a-number"},
                      InvalidCountCase{"cfgcpt", "10remaining"}, InvalidCountCase{"cfgcpt", "0"},
                      InvalidCountCase{"cfgcpt", "-1"}, InvalidCountCase{"cfgcpt", "NaN"},
                      InvalidCountCase{"cfgcpt", "infinity"}));

TEST(XmlConfiguration, ParsesCountsIndependentlyOfTheGlobalLocale) {
  const GlobalLocaleGuard locale_guard{std::locale{std::locale::classic(), new CommaDecimalPoint}};
  const auto xml = replace_value(kValidXml, "cfgcpf", "1234.5");

  const auto result = netft::detail::parse_sensor_configuration(xml);

  EXPECT_DOUBLE_EQ(result.calibration.counts_per_force_unit, 1234.5);
  EXPECT_THROW(
      netft::detail::parse_sensor_configuration(replace_value(kValidXml, "cfgcpf", "1234,5")),
      netft::DiscoveryError);
}

TEST(XmlConfiguration, UsesRoundToNearestAndRestoresTheCallersRoundingMode) {
  const FloatingPointEnvironmentGuard restore_environment;
  ASSERT_EQ(std::fesetround(FE_UPWARD), 0);
  const auto xml =
      replace_value(kValidXml, "cfgcpf", "1.00000000000000011102230246251565404236316680908203125");

  const auto result = netft::detail::parse_sensor_configuration(xml);

  EXPECT_EQ(result.calibration.counts_per_force_unit, 0x1p+0);
  EXPECT_EQ(std::fegetround(), FE_UPWARD);
}

TEST(XmlConfiguration, RestoresTheCallersRoundingModeWhenParsingThrows) {
  const FloatingPointEnvironmentGuard restore_environment;
  ASSERT_EQ(std::fesetround(FE_UPWARD), 0);

  EXPECT_THROW(
      netft::detail::parse_sensor_configuration(replace_value(kValidXml, "cfgcpf", "invalid")),
      netft::DiscoveryError);
  EXPECT_EQ(std::fegetround(), FE_UPWARD);
}

struct FloatingPointEnvironmentCase {
  int rounding_mode;
  const char *value;
  bool succeeds;
};

class FloatingPointEnvironment : public ::testing::TestWithParam<FloatingPointEnvironmentCase> {};

TEST_P(FloatingPointEnvironment, PreservesTheCallersModeAndExceptionFlags) {
  const FloatingPointEnvironmentGuard restore_environment;
  ASSERT_EQ(std::feclearexcept(FE_ALL_EXCEPT), 0);
  ASSERT_EQ(std::fesetround(GetParam().rounding_mode), 0);
  ASSERT_EQ(std::feraiseexcept(FE_DIVBYZERO), 0);
  const int expected_flags = std::fetestexcept(FE_ALL_EXCEPT);
  const auto xml = replace_value(kValidXml, "cfgcpf", GetParam().value);

  if (GetParam().succeeds) {
    const auto result = netft::detail::parse_sensor_configuration(xml);
    EXPECT_EQ(result.calibration.counts_per_force_unit, 0x1p+0);
  } else {
    EXPECT_THROW(netft::detail::parse_sensor_configuration(xml), netft::DiscoveryError);
  }

  EXPECT_EQ(std::fegetround(), GetParam().rounding_mode);
  EXPECT_EQ(std::fetestexcept(FE_ALL_EXCEPT), expected_flags);
}

INSTANTIATE_TEST_SUITE_P(
    EveryRoundingModeAndResult, FloatingPointEnvironment,
    ::testing::Values(
        FloatingPointEnvironmentCase{
            FE_TONEAREST, "1.00000000000000011102230246251565404236316680908203125", true},
        FloatingPointEnvironmentCase{FE_TONEAREST, "1e9999", false},
        FloatingPointEnvironmentCase{FE_TONEAREST, "1e-9999", false},
        FloatingPointEnvironmentCase{FE_TONEAREST, "+1", false},
        FloatingPointEnvironmentCase{
            FE_UPWARD, "1.00000000000000011102230246251565404236316680908203125", true},
        FloatingPointEnvironmentCase{FE_UPWARD, "1e9999", false},
        FloatingPointEnvironmentCase{FE_UPWARD, "1e-9999", false},
        FloatingPointEnvironmentCase{FE_UPWARD, "+1", false}));

#if defined(__linux__) && defined(__GLIBC__)
TEST(FloatingPointTrapPlatformPolicy, TreatsUnavailableTrapControlAsAnErrorWhereItIsSupported) {
#if defined(__aarch64__)
  EXPECT_EQ(unavailable_trap_control_exit_code(), kTrapControlUnsupported);
#else
  EXPECT_EQ(unavailable_trap_control_exit_code(), kTrapSetupFailed);
#endif
}

TEST(XmlConfiguration, MasksCallerOverflowTrapsWhileParsing) {
  const pid_t child = fork();
  ASSERT_NE(child, -1);
  if (child == 0) {
    if (std::feclearexcept(FE_ALL_EXCEPT) != 0) {
      _exit(1);
    }
    if (feenableexcept(FE_OVERFLOW) == -1) {
      _exit(unavailable_trap_control_exit_code());
    }
    try {
      static_cast<void>(
          netft::detail::parse_sensor_configuration(replace_value(kValidXml, "cfgcpf", "1e9999")));
    } catch (const netft::DiscoveryError &) {
      _exit((fegetexcept() & FE_OVERFLOW) != 0 ? 0 : 2);
    } catch (...) {
      _exit(3);
    }
    _exit(4);
  }

  int status{};
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  if (WEXITSTATUS(status) == kTrapControlUnsupported) {
    GTEST_SKIP() << "floating-point trap control is unavailable on this platform";
  }
  EXPECT_EQ(WEXITSTATUS(status), 0);
}
#endif

TEST(XmlConfiguration, TrimsAsciiWhitespaceAroundEveryValue) {
  auto xml = replace_value(kValidXml, "prodname", " \tEthernet Axia\r\n");
  xml = replace_value(xml, "cfgcpf", "\n 1000000 \t");
  xml = replace_value(xml, "cfgcpt", "\r1000000 ");
  xml = replace_value(xml, "scfgfu", " N\n");
  xml = replace_value(xml, "scfgtu", "\tNm ");

  const auto result = netft::detail::parse_sensor_configuration(xml);
  EXPECT_EQ(result.product_name, "Ethernet Axia");
  EXPECT_DOUBLE_EQ(result.calibration.counts_per_force_unit, 1'000'000.0);
  EXPECT_DOUBLE_EQ(result.calibration.counts_per_torque_unit, 1'000'000.0);
  EXPECT_EQ(result.calibration.force_unit, netft::ForceUnit::Newton);
  EXPECT_EQ(result.calibration.torque_unit, netft::TorqueUnit::NewtonMeter);
}

TEST(XmlConfiguration, AcceptsEveryApprovedForceUnitSpelling) {
  constexpr std::array<std::pair<std::string_view, netft::ForceUnit>, 5> cases{{
      {"lbf", netft::ForceUnit::PoundForce},
      {"N", netft::ForceUnit::Newton},
      {"klbf", netft::ForceUnit::KiloPoundForce},
      {"kN", netft::ForceUnit::KiloNewton},
      {"kgf", netft::ForceUnit::KilogramForce},
  }};

  for (const auto &[spelling, expected] : cases) {
    SCOPED_TRACE(spelling);
    const auto result =
        netft::detail::parse_sensor_configuration(replace_value(kValidXml, "scfgfu", spelling));
    EXPECT_EQ(result.calibration.force_unit, expected);
  }
}

TEST(XmlConfiguration, AcceptsEveryApprovedAtiTorqueUnitSpelling) {
  constexpr std::array<std::pair<std::string_view, netft::TorqueUnit>, 8> cases{{
      {"Nm", netft::TorqueUnit::NewtonMeter},
      {"N-m", netft::TorqueUnit::NewtonMeter},
      {"Nmm", netft::TorqueUnit::NewtonMillimeter},
      {"N-mm", netft::TorqueUnit::NewtonMillimeter},
      {"lbf-in", netft::TorqueUnit::PoundForceInch},
      {"lbf-ft", netft::TorqueUnit::PoundForceFoot},
      {"kg-cm", netft::TorqueUnit::KilogramForceCentimeter},
      {"kN-m", netft::TorqueUnit::KiloNewtonMeter},
  }};

  for (const auto &[spelling, expected] : cases) {
    SCOPED_TRACE(spelling);
    const auto result =
        netft::detail::parse_sensor_configuration(replace_value(kValidXml, "scfgtu", spelling));
    EXPECT_EQ(result.calibration.torque_unit, expected);
  }
}

TEST(XmlConfiguration, RejectsUnknownUnits) {
  EXPECT_THROW(
      netft::detail::parse_sensor_configuration(replace_value(kValidXml, "scfgfu", "newtons")),
      netft::DiscoveryError);
  EXPECT_THROW(netft::detail::parse_sensor_configuration(
                   replace_value(kValidXml, "scfgtu", "newton-metres")),
               netft::DiscoveryError);
}

TEST(XmlConfiguration, IgnoresRequiredElementTextInsideComments) {
  const auto xml = insert_after_root_open(kValidXml, "<!-- <prodname>Fake Sensor</prodname> -->");

  const auto result = netft::detail::parse_sensor_configuration(xml);

  EXPECT_EQ(result.product_name, "Ethernet Axia");
}

TEST(XmlConfiguration, RejectsAFieldThatAppearsOnlyInsideAComment) {
  const auto xml = insert_after_root_open(remove_element(kValidXml, "prodname"),
                                          "<!-- <prodname>Fake Sensor</prodname> -->");

  EXPECT_THROW(netft::detail::parse_sensor_configuration(xml), netft::DiscoveryError);
}

TEST(XmlConfiguration, IgnoresRequiredElementTextInsideCdata) {
  const auto xml =
      insert_after_root_open(kValidXml, "<![CDATA[<prodname>Fake Sensor</prodname>]]>");

  const auto result = netft::detail::parse_sensor_configuration(xml);

  EXPECT_EQ(result.product_name, "Ethernet Axia");
}

TEST(XmlConfiguration, RejectsAFieldThatAppearsOnlyInsideCdata) {
  const auto xml = insert_after_root_open(remove_element(kValidXml, "prodname"),
                                          "<![CDATA[<prodname>Fake Sensor</prodname>]]>");

  EXPECT_THROW(netft::detail::parse_sensor_configuration(xml), netft::DiscoveryError);
}

TEST(XmlConfiguration, RejectsUnterminatedCommentsAndCdata) {
  EXPECT_THROW(
      netft::detail::parse_sensor_configuration(std::string{kValidXml} + "<!-- unterminated"),
      netft::DiscoveryError);
  EXPECT_THROW(
      netft::detail::parse_sensor_configuration(std::string{kValidXml} + "<![CDATA[unterminated"),
      netft::DiscoveryError);
}

TEST(XmlConfiguration, RejectsOverlappingElementNesting) {
  const auto xml = replace_value(kValidXml, "prodname", "Ethernet Axia</wrapper>");
  const auto malformed = std::string{"<wrapper>"} + xml;

  EXPECT_THROW(netft::detail::parse_sensor_configuration(malformed), netft::DiscoveryError);
}

TEST(SensorDiscovery, FetchesTheRealSensorFixtureFromTheRequiredEndpoint) {
  FakeHttpServer server{std::string{kValidXml}};

  const auto result = netft::discover_sensor(options_for(server));

  EXPECT_EQ(result.product_name, "Ethernet Axia");
  EXPECT_DOUBLE_EQ(result.calibration.counts_per_force_unit, 1'000'000.0);
  EXPECT_DOUBLE_EQ(result.calibration.counts_per_torque_unit, 1'000'000.0);
  EXPECT_EQ(result.calibration.force_unit, netft::ForceUnit::Newton);
  EXPECT_EQ(result.calibration.torque_unit, netft::TorqueUnit::NewtonMeter);
  EXPECT_EQ(result.source, netft::CalibrationSource::Sensor);
  EXPECT_EQ(result.revision, 1U);
  EXPECT_EQ(server.request_count(), 1U);
}

TEST(SensorDiscovery, UsesTheServersCurrentSuccessfulResponse) {
  FakeHttpServer server{"unused", 500};
  server.set_response(replace_value(kValidXml, "prodname", "Updated Sensor"));

  const auto result = netft::discover_sensor(options_for(server));

  EXPECT_EQ(result.product_name, "Updated Sensor");
  EXPECT_EQ(server.request_count(), 1U);
}

TEST(SensorDiscovery, RejectsHttpErrorsWithoutExposingTheResponseBody) {
  constexpr std::string_view secret_body = "private sensor diagnostics";
  FakeHttpServer server{std::string{secret_body}, 500};

  try {
    static_cast<void>(netft::discover_sensor(options_for(server)));
    FAIL() << "expected discovery to reject HTTP 500";
  } catch (const netft::DiscoveryError &error) {
    EXPECT_EQ(std::string{error.what()}.find(secret_body), std::string::npos);
  }
  EXPECT_EQ(server.request_count(), 1U);
}

TEST(SensorDiscovery, RejectsRedirectResponses) {
  FakeHttpServer redirect_target{std::string{kValidXml}};
  FakeHttpServer redirecting_server{"redirecting", 302};
  redirecting_server.set_redirect_location("http://" + redirect_target.host() + ":" +
                                           std::to_string(redirect_target.port()) +
                                           "/netftapi2.xml");

  EXPECT_THROW(netft::discover_sensor(options_for(redirecting_server)), netft::DiscoveryError);
  EXPECT_EQ(redirecting_server.request_count(), 1U);
  EXPECT_EQ(redirect_target.request_count(), 0U);
}

TEST(SensorDiscovery, AcceptsAResponseAtThe64KiBBoundary) {
  std::string body{kValidXml};
  body.append(65'536 - body.size(), ' ');
  FakeHttpServer server{std::move(body)};

  EXPECT_NO_THROW(static_cast<void>(netft::discover_sensor(options_for(server))));
}

TEST(SensorDiscovery, RejectsAResponseLargerThan64KiB) {
  FakeHttpServer server{std::string(65'537, 'x')};
  EXPECT_THROW(netft::discover_sensor(options_for(server)), netft::DiscoveryError);
}

TEST(SensorDiscovery, EnforcesTheTotalTimeout) {
  FakeHttpServer server{std::string{kValidXml}};
  server.set_response_delay(std::chrono::milliseconds{250});
  auto options = options_for(server);
  options.total_timeout = std::chrono::milliseconds{50};

  const auto started = std::chrono::steady_clock::now();
  EXPECT_THROW(netft::discover_sensor(options), netft::DiscoveryError);
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds{1});
}

TEST(SensorDiscovery, ReportsConnectionFailure) {
  int closed_port{};
  {
    FakeHttpServer server{std::string{kValidXml}};
    closed_port = server.port();
  }
  netft::DiscoveryOptions options;
  options.sensor_host = "127.0.0.1";
  options.http_port = closed_port;
  options.connect_timeout = std::chrono::milliseconds{100};
  options.total_timeout = std::chrono::milliseconds{200};

  EXPECT_THROW(netft::discover_sensor(options), netft::DiscoveryError);
}

TEST(SensorDiscovery, RejectsHostsThatInjectUrlSyntax) {
  FakeHttpServer unintended_target{std::string{kValidXml}};
  constexpr std::array<std::string_view, 4> malicious_hosts{
      "ignored-userinfo@127.0.0.1", "127.0.0.1/ignored", "127.0.0.1?ignored", "127.0.0.1#ignored"};

  for (const auto host : malicious_hosts) {
    SCOPED_TRACE(host);
    auto options = options_for(unintended_target);
    options.sensor_host = host;
    EXPECT_THROW(netft::discover_sensor(options), netft::DiscoveryError);
  }
  EXPECT_EQ(unintended_target.request_count(), 0U);
}

TEST(SensorDiscovery, TreatsAnIpv6LiteralAsAHost) {
  netft::DiscoveryOptions options;
  options.sensor_host = "::1";
  options.http_port = 1;
  options.connect_timeout = std::chrono::milliseconds{50};
  options.total_timeout = std::chrono::milliseconds{100};

  EXPECT_THROW(netft::discover_sensor(options), netft::DiscoveryError);
}

TEST(SensorDiscovery, RejectsRoundedMillisecondsAboveLongMax) {
  netft::DiscoveryOptions options;
  options.sensor_host = "127.0.0.1";
  options.http_port = 1;
  options.connect_timeout = std::chrono::milliseconds{50};
  options.total_timeout =
      std::chrono::duration<double>{static_cast<double>(std::numeric_limits<long>::max()) / 1000.0};

  EXPECT_THROW(netft::discover_sensor(options), netft::DiscoveryError);
}

TEST(SensorDiscovery, AcceptsRoundedMillisecondsAtOrBelowLongMax) {
  FakeHttpServer server{std::string{kValidXml}};
  auto options = options_for(server);
  options.connect_timeout = std::chrono::milliseconds{50};
  const double boundary_seconds = static_cast<double>(std::numeric_limits<long>::max()) / 1000.0;
  options.total_timeout = std::chrono::duration<double>{std::nextafter(boundary_seconds, 0.0)};

  EXPECT_NO_THROW(static_cast<void>(netft::discover_sensor(options)));
}

TEST(FakeHttpServerTest, DestructionInterruptsAnIncompleteAcceptedRequest) {
  auto server = std::make_unique<FakeHttpServer>(std::string{kValidXml});
  const int client = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(client, 0);

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<std::uint16_t>(server->port()));
  ASSERT_EQ(::inet_pton(AF_INET, server->host().c_str(), &address.sin_addr), 1);
  ASSERT_EQ(::connect(client, reinterpret_cast<const sockaddr *>(&address), sizeof(address)), 0);
  ASSERT_EQ(::send(client, "GET /netftapi2.xml", 19, MSG_NOSIGNAL), 19);

  for (int attempt = 0; attempt < 100 && server->accepted_connection_count() == 0; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  ASSERT_EQ(server->accepted_connection_count(), 1U);

  const auto started = std::chrono::steady_clock::now();
  server.reset();
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds{500});
  ::close(client);
}

} // namespace
