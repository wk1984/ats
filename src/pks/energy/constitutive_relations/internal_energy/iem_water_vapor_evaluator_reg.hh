/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/*
  The WRM Evaluator simply calls the WRM with the correct arguments.

  Authors: Ethan Coon (ecoon@lanl.gov)
*/

#include "iem_water_vapor_evaluator.hh"

namespace Amanzi {
namespace Energy {
namespace EnergyRelations {

Utils::RegisteredFactory<FieldEvaluator,IEMWaterVaporEvaluator> IEMWaterVaporEvaluator::factory_("iem water vapor");

} //namespace
} //namespace
} //namespace
