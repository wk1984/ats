/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/*
  Evaluates the Kr associated with the unfrozen fraction of water.

  Authors: Ethan Coon (ecoon@lanl.gov)
*/

#include "boost/math/constants/constants.hpp"
#include <cmath>

#include "dbc.hh"
#include "errors.hh"
#include "unfrozen_fraction_relperm_model.hh"

namespace Amanzi {
namespace Flow {
namespace FlowRelations {

UnfrozenFractionRelPermModel::UnfrozenFractionRelPermModel(Teuchos::ParameterList& plist) :
    plist_(plist),
    pi_(boost::math::constants::pi<double>()) {

  alpha_ = plist_.get<int>("unfrozen rel perm alpha", 4);
  if (alpha_ % 2 != 0) {
    Errors::Message message("Unfrozen Fraction Rel Perm: alpha must be an even integer");
    Exceptions::amanzi_throw(message);
  }
}

double
UnfrozenFractionRelPermModel::SurfaceRelPerm(double uf, double h) {
  return std::pow(std::sin(pi_ * uf / 2.), alpha_);
}


} // namespace
} // namespace
} // namespace
