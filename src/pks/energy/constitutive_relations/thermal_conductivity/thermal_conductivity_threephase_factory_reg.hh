/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */
/* -------------------------------------------------------------------------

   ATS
   Author: Ethan Coon

   Self-registering factory for TC implementations.
   ------------------------------------------------------------------------- */

#include "thermal_conductivity_threephase_factory.hh"

// explicity instantitate the static data of Factory<EOS>
template<> 
Amanzi::Utils::Factory<Amanzi::Energy::EnergyRelations::ThermalConductivityThreePhase>::map_type* 
Amanzi::Utils::Factory<Amanzi::Energy::EnergyRelations::ThermalConductivityThreePhase>::map_;

