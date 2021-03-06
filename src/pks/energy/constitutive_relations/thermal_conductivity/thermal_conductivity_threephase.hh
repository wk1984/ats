/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/* -------------------------------------------------------------------------
ATS

License: see $ATS_DIR/COPYRIGHT
Author: Ethan Coon

Base class of a three-phase Thermal Conductivity relation.

UNITS: ????
------------------------------------------------------------------------- */

#ifndef PK_ENERGY_RELATIONS_TC_THREEPHASE_HH_
#define PK_ENERGY_RELATIONS_TC_THREEPHASE_HH_

#include "dbc.hh"

namespace Amanzi {
namespace Energy {
namespace EnergyRelations {

class ThermalConductivityThreePhase {

public:
  virtual ~ThermalConductivityThreePhase() {}

  virtual double ThermalConductivity(double porosity, double sat_liq, double sat_ice, double temp) = 0;
  virtual double DThermalConductivity_DPorosity(double porosity, double sat_liq, double sat_ice, double temp) {
    ASSERT(false);
    return 0.;
  }
  virtual double DThermalConductivity_DSaturationLiquid(double porosity, double sat_liq, double sat_ice, double temp) {
    ASSERT(false);
    return 0.;
  }
  virtual double DThermalConductivity_DSaturationIce(double porosity, double sat_liq, double sat_ice, double temp) {
    ASSERT(false);
    return 0.;
  }
  virtual double DThermalConductivity_DTemperature(double porosity, double sat_liq, double sat_ice, double temp) {
    ASSERT(false);
    return 0.;
  }
};

} // namespace
} // namespace
} // namespace

#endif
