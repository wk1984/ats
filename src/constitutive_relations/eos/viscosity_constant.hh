/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/*
  ATS

  Constant prescribed viscosity.

  http://software.lanl.gov/ats/trac

  Authors: Ethan Coon (ecoon@lanl.gov)
*/


#ifndef AMANZI_RELATIONS_VISCOSITY_CONSTANT_HH_
#define AMANZI_RELATIONS_VISCOSITY_CONSTANT_HH_

#include "Teuchos_ParameterList.hpp"
#include "factory.hh"
#include "dbc.hh"
#include "viscosity_relation.hh"

namespace Amanzi {
namespace Relations {

// Equation of State model
class ViscosityConstant : public ViscosityRelation {

public:
  explicit
  ViscosityConstant(Teuchos::ParameterList& visc_plist);

  virtual double Viscosity(double T) { return visc_; }
  virtual double DViscosityDT(double T) { return 0.; }

protected:

  virtual void InitializeFromPlist_();

  Teuchos::ParameterList visc_plist_;
  double visc_;

 private:
  static Utils::RegisteredFactory<ViscosityRelation,ViscosityConstant> factory_;

};

} // namespace
} // namespace

#endif
