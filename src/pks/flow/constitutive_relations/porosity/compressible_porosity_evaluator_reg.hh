/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/*
  Evaluates the porosity, given a small compressibility of rock.

  Authors: Ethan Coon (ecoon@lanl.gov)
*/

#include "compressible_porosity_evaluator.hh"

namespace Amanzi {
namespace Flow {
namespace FlowRelations {

// registry of method 
  Utils::RegisteredFactory<FieldEvaluator,CompressiblePorosityEvaluator> CompressiblePorosityEvaluator::fac_("compressible porosity"); 

} //namespace
} //namespace
} //namespace
