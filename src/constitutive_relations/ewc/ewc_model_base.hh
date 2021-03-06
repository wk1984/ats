/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */
/* -------------------------------------------------------------------------
ATS

License: see $ATS_DIR/COPYRIGHT
Author: Ethan Coon

EWCModelBase provides some of the functionality of EWCModel for inverse
evaluating.

------------------------------------------------------------------------- */

#ifndef AMANZI_EWC_MODEL_BASE_HH_
#define AMANZI_EWC_MODEL_BASE_HH_

#include "Tensor.hh"
#include "Point.hh"

#include "ewc_model.hh"

namespace Amanzi {

class EWCModelBase : public EWCModel {
 public:
  EWCModelBase() {}
  
  virtual int Evaluate(double T, double p, double& energy, double& wc);
  virtual int InverseEvaluate(double energy, double wc, double& T, double& p, bool verbose=false);
  virtual int InverseEvaluateEnergy(double energy, double p, double& T);

 protected:

  virtual int EvaluateEnergyAndWaterContent_(double T, double p,
          AmanziGeometry::Point& result) = 0;

  virtual int EvaluateEnergyAndWaterContentAndJacobian_(double T, double p,
          AmanziGeometry::Point& result, WhetStone::Tensor& jac);

  int EvaluateEnergyAndWaterContentAndJacobian_FD_(double T, double p,
          AmanziGeometry::Point& result, WhetStone::Tensor& jac);
};

} // namespace

#endif
