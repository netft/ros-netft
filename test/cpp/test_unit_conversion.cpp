#include "ros/unit_conversion.hpp"

#include <gtest/gtest.h>

namespace {

TEST(UnitConversion, ConvertsEveryForceUnitToNewtons)
{
  EXPECT_DOUBLE_EQ(netft_driver::force_scale_to_newtons(netft::ForceUnit::Newton), 1.0);
  EXPECT_DOUBLE_EQ(netft_driver::force_scale_to_newtons(netft::ForceUnit::KiloNewton), 1000.0);
  EXPECT_DOUBLE_EQ(netft_driver::force_scale_to_newtons(netft::ForceUnit::PoundForce),
                   4.4482216152605);
  EXPECT_DOUBLE_EQ(netft_driver::force_scale_to_newtons(netft::ForceUnit::KiloPoundForce),
                   4448.2216152605);
  EXPECT_DOUBLE_EQ(netft_driver::force_scale_to_newtons(netft::ForceUnit::KilogramForce),
                   9.80665);
  EXPECT_THROW(netft_driver::force_scale_to_newtons(netft::ForceUnit::Unknown),
               std::invalid_argument);
}

TEST(UnitConversion, ConvertsEveryTorqueUnitToNewtonMetres)
{
  EXPECT_DOUBLE_EQ(netft_driver::torque_scale_to_newton_metres(netft::TorqueUnit::NewtonMeter),
                   1.0);
  EXPECT_DOUBLE_EQ(netft_driver::torque_scale_to_newton_metres(
                     netft::TorqueUnit::NewtonMillimeter), 0.001);
  EXPECT_DOUBLE_EQ(netft_driver::torque_scale_to_newton_metres(
                     netft::TorqueUnit::PoundForceInch), 0.1129848290276167);
  EXPECT_DOUBLE_EQ(netft_driver::torque_scale_to_newton_metres(
                     netft::TorqueUnit::PoundForceFoot), 1.3558179483314004);
  EXPECT_DOUBLE_EQ(netft_driver::torque_scale_to_newton_metres(
                     netft::TorqueUnit::KilogramForceCentimeter), 0.0980665);
  EXPECT_DOUBLE_EQ(netft_driver::torque_scale_to_newton_metres(
                     netft::TorqueUnit::KiloNewtonMeter), 1000.0);
  EXPECT_THROW(netft_driver::torque_scale_to_newton_metres(netft::TorqueUnit::Unknown),
               std::invalid_argument);
}

TEST(UnitConversion, ConvertsSampleAndPreservesMetadata)
{
  netft::Sample native;
  native.rdt_sequence = 10;
  native.ft_sequence = 20;
  native.status = 30;
  native.force = {1.0, -2.0, 3.0};
  native.torque = {4.0, -5.0, 6.0};
  native.force_unit = netft::ForceUnit::KiloNewton;
  native.torque_unit = netft::TorqueUnit::NewtonMillimeter;
  native.configuration_revision = 7;
  native.received_at = std::chrono::steady_clock::now();

  const auto si = netft_driver::to_si_sample(native);

  EXPECT_EQ(si.rdt_sequence, 10U);
  EXPECT_EQ(si.ft_sequence, 20U);
  EXPECT_EQ(si.status, 30U);
  EXPECT_EQ(si.configuration_revision, 7U);
  EXPECT_EQ(si.received_at, native.received_at);
  EXPECT_DOUBLE_EQ(si.force[0], 1000.0);
  EXPECT_DOUBLE_EQ(si.force[1], -2000.0);
  EXPECT_DOUBLE_EQ(si.force[2], 3000.0);
  EXPECT_DOUBLE_EQ(si.torque[0], 0.004);
  EXPECT_DOUBLE_EQ(si.torque[1], -0.005);
  EXPECT_DOUBLE_EQ(si.torque[2], 0.006);
}

}  // namespace
