/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/*
  Source term evaluator for enthalpy of mass source.

  Authors: Ethan Coon (ecoon@lanl.gov)
*/

#include "advected_energy_source_evaluator.hh"

namespace Amanzi {
namespace Energy {
namespace EnergyRelations {

Utils::RegisteredFactory<FieldEvaluator,AdvectedEnergySourceEvaluator> AdvectedEnergySourceEvaluator::factory_("advected energy source");

} //namespace
} //namespace
} //namespace

