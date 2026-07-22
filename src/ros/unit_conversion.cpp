#include "ros/unit_conversion.hpp"

#include <stdexcept>

namespace netft_driver {

double force_scale_to_newtons(netft::ForceUnit unit)
{
  switch (unit) {
    case netft::ForceUnit::Newton:
      return 1.0;
    case netft::ForceUnit::KiloNewton:
      return 1000.0;
    case netft::ForceUnit::PoundForce:
      return 4.4482216152605;
    case netft::ForceUnit::KiloPoundForce:
      return 4448.2216152605;
    case netft::ForceUnit::KilogramForce:
      return 9.80665;
    case netft::ForceUnit::Unknown:
      throw std::invalid_argument{"Unknown force unit"};
  }

  throw std::invalid_argument{"Unknown force unit"};
}

double torque_scale_to_newton_metres(netft::TorqueUnit unit)
{
  switch (unit) {
    case netft::TorqueUnit::NewtonMeter:
      return 1.0;
    case netft::TorqueUnit::NewtonMillimeter:
      return 0.001;
    case netft::TorqueUnit::PoundForceInch:
      return 0.1129848290276167;
    case netft::TorqueUnit::PoundForceFoot:
      return 1.3558179483314004;
    case netft::TorqueUnit::KilogramForceCentimeter:
      return 0.0980665;
    case netft::TorqueUnit::KiloNewtonMeter:
      return 1000.0;
    case netft::TorqueUnit::Unknown:
      throw std::invalid_argument{"Unknown torque unit"};
  }

  throw std::invalid_argument{"Unknown torque unit"};
}

SiSample to_si_sample(const netft::Sample & sample)
{
  const auto force_scale = force_scale_to_newtons(sample.force_unit);
  const auto torque_scale = torque_scale_to_newton_metres(sample.torque_unit);

  SiSample si;
  si.rdt_sequence = sample.rdt_sequence;
  si.ft_sequence = sample.ft_sequence;
  si.status = sample.status;
  si.configuration_revision = sample.configuration_revision;
  si.received_at = sample.received_at;

  for (std::size_t index = 0; index < si.force.size(); ++index) {
    si.force[index] = sample.force[index] * force_scale;
    si.torque[index] = sample.torque[index] * torque_scale;
  }

  return si;
}

}  // namespace netft_driver
