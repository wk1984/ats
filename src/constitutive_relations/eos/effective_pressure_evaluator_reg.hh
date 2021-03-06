/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/*
  EffectivePressureEvaluator evaluates p_eff = max(p_atm, p_liquid), which is used for EOS.

  License: BSD
  Authors: Ethan Coon (ecoon@lanl.gov)
*/

#include "effective_pressure_evaluator.hh"

namespace Amanzi {
namespace Relations {

// registry of method
Utils::RegisteredFactory<FieldEvaluator,EffectivePressureEvaluator> EffectivePressureEvaluator::factory_("effective_pressure");

}
}


