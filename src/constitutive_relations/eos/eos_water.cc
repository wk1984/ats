/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/*
  ATS

  EOS for liquid water.  See the permafrost physical properties notes for
  references and documentation of this EOS at:

  http://software.lanl.gov/ats/trac

  Authors: Ethan Coon (ecoon@lanl.gov)
*/

#include "eos_water.hh"

namespace Amanzi {
namespace Relations {

EOSWater::EOSWater(Teuchos::ParameterList& eos_plist) :
    EOSConstantMolarMass(0.0180153),
    eos_plist_(eos_plist),

    ka_(999.915),
    kb_(0.0416516),
    kc_(-0.0100836),
    kd_(0.000206355),
    kT0_(273.15),

    kalpha_(5.0e-10),
    kp0_(1.0e5) {
};


double EOSWater::MassDensity(double T, double p) {
  double dT = T - kT0_;
  double rho1bar = ka_ + (kb_ + (kc_ + kd_*dT)*dT)*dT;
  return rho1bar * (1.0 + kalpha_*(p - kp0_));
};


double EOSWater::DMassDensityDT(double T, double p) {
  double dT = T - kT0_;
  double rho1bar = kb_ + (2.0*kc_ + 3.0*kd_*dT)*dT;
  return rho1bar * (1.0 + kalpha_*(p - kp0_));

};


double EOSWater::DMassDensityDp(double T, double p) {
  double dT = T - kT0_;
  double rho1bar = ka_ + (kb_ + (kc_ + kd_*dT)*dT)*dT;
  return rho1bar * kalpha_;
};

} // namespace
} // namespace
