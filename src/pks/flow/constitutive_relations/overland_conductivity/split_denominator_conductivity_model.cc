/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/*
 Evaluates the conductivity of surface flow as a function of ponded
 depth using Manning's model. The denominator in the model is evaluated separately.
 
*/

#include "split_denominator_conductivity_model.hh"

namespace Amanzi {
namespace Flow {
namespace FlowRelations {

SplitDenominatorConductivityModel::SplitDenominatorConductivityModel(Teuchos::ParameterList& plist) :
    plist_(plist) {

  manning_exp_ = plist_.get<double>("Manning exponent");
}

double SplitDenominatorConductivityModel::Conductivity(double depth, double slope, double coef) {
  if (depth <= 0.) return 0.;
  double exponent = manning_exp_ + 1.0;
  return std::pow(std::max(depth,0.), exponent);
}

double SplitDenominatorConductivityModel::DConductivityDDepth(double depth, double slope, double coef) {
  if (depth <= 0.) return 0.;
  double exponent = manning_exp_ + 1.0;
  return std::pow(std::max(depth,0.), exponent - 1.) * exponent;
}


} // namespace
} // namespace
} // namespace
