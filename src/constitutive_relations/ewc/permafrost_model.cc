/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/*
  Ugly hackjob to enable direct evaluation of the full model, on a single
  WRM/region.  This is bypassing much of the "niceness" of the framework, but
  seems necessary for solving a cell-wise correction equation.

  Uses intensive, not extensive, forms.

  Authors: Ethan Coon (ecoon@lanl.gov)
*/

#include "exceptions.hh"
#include "State.hh"

#include "eos_evaluator.hh"
#include "eos.hh"
#include "wrm_partition.hh"
#include "wrm_permafrost_evaluator.hh"
#include "wrm_permafrost_model.hh"
#include "molar_fraction_gas_evaluator.hh"
#include "vapor_pressure_relation.hh"
#include "pc_liquid_evaluator.hh"
#include "pc_ice_evaluator.hh"
#include "pc_ice_water.hh"
#include "pc_liq_atm.hh"
#include "iem_evaluator.hh"
#include "iem.hh"
#include "iem_water_vapor_evaluator.hh"
#include "iem_water_vapor.hh"
#include "compressible_porosity_evaluator.hh"
#include "compressible_porosity_model.hh"
#include "compressible_porosity_leijnse_evaluator.hh"
#include "compressible_porosity_leijnse_model.hh"
#include "permafrost_model.hh"

namespace Amanzi {

#define DEBUG_FLAG 0

void PermafrostModel::InitializeModel(const Teuchos::Ptr<State>& S,
                                      Teuchos::ParameterList& plist) {
  // these are not yet initialized
  rho_rock_ = -1.;
  p_atm_ = -1.e12;
  domain = plist.get<std::string>("domain key", "");
  if (!domain.empty()) {
    mesh_ = S->GetMesh(domain);
  } else {
    mesh_ = S->GetMesh("domain");
  }
  
  // Grab the models.
  // get the WRM models and their regions

  Teuchos::RCP<FieldEvaluator> me = S->GetFieldEvaluator(getKey(domain, "saturation_gas"));
  
  Teuchos::RCP<Flow::FlowRelations::WRMPermafrostEvaluator> wrm_me =
      Teuchos::rcp_dynamic_cast<Flow::FlowRelations::WRMPermafrostEvaluator>(me);
  ASSERT(wrm_me != Teuchos::null);
  wrms_ = wrm_me->get_WRMPermafrostModels();
  
  // -- liquid EOS
  me = S->GetFieldEvaluator(getKey(domain, "molar_density_liquid"));
  Teuchos::RCP<Relations::EOSEvaluator> eos_liquid_me =
      Teuchos::rcp_dynamic_cast<Relations::EOSEvaluator>(me);
  ASSERT(eos_liquid_me != Teuchos::null);
  liquid_eos_ = eos_liquid_me->get_EOS();

  // -- ice EOS
  me = S->GetFieldEvaluator(getKey(domain, "molar_density_ice"));
  Teuchos::RCP<Relations::EOSEvaluator> eos_ice_me =
      Teuchos::rcp_dynamic_cast<Relations::EOSEvaluator>(me);
  ASSERT(eos_ice_me != Teuchos::null);
  ice_eos_ = eos_ice_me->get_EOS();

  // -- gas EOS
  me = S->GetFieldEvaluator(getKey(domain, "molar_density_gas"));
  Teuchos::RCP<Relations::EOSEvaluator> eos_gas_me =
      Teuchos::rcp_dynamic_cast<Relations::EOSEvaluator>(me);
  ASSERT(eos_gas_me != Teuchos::null);
  gas_eos_ = eos_gas_me->get_EOS();

  // -- gas vapor pressure
  me = S->GetFieldEvaluator(getKey(domain, "mol_frac_gas"));
  Teuchos::RCP<Relations::MolarFractionGasEvaluator> mol_frac_me =
    Teuchos::rcp_dynamic_cast<Relations::MolarFractionGasEvaluator>(me);
  ASSERT(mol_frac_me != Teuchos::null);
  vpr_ = mol_frac_me->get_VaporPressureRelation();

  // -- capillary pressure for ice/water
  me = S->GetFieldEvaluator(getKey(domain, "capillary_pressure_liq_ice"));
  Teuchos::RCP<Flow::FlowRelations::PCIceEvaluator> pc_ice_me =
    Teuchos::rcp_dynamic_cast<Flow::FlowRelations::PCIceEvaluator>(me);
  ASSERT(pc_ice_me != Teuchos::null);
  pc_i_ = pc_ice_me->get_PCIceWater();

  // -- capillary pressure for liq/gas
  me = S->GetFieldEvaluator(getKey(domain, "capillary_pressure_gas_liq"));
  Teuchos::RCP<Flow::FlowRelations::PCLiquidEvaluator> pc_liq_me =
    Teuchos::rcp_dynamic_cast<Flow::FlowRelations::PCLiquidEvaluator>(me);
  ASSERT(pc_liq_me != Teuchos::null);
  pc_l_ = pc_liq_me->get_PCLiqAtm();
  
  // -- iem for liquid
  me = S->GetFieldEvaluator(getKey(domain, "internal_energy_liquid"));
  Teuchos::RCP<Energy::EnergyRelations::IEMEvaluator> iem_liquid_me =
      Teuchos::rcp_dynamic_cast<Energy::EnergyRelations::IEMEvaluator>(me);
  ASSERT(iem_liquid_me != Teuchos::null);
  liquid_iem_ = iem_liquid_me->get_IEM();

  // -- iem for ice
  me = S->GetFieldEvaluator(getKey(domain, "internal_energy_ice"));
  Teuchos::RCP<Energy::EnergyRelations::IEMEvaluator> iem_ice_me =
      Teuchos::rcp_dynamic_cast<Energy::EnergyRelations::IEMEvaluator>(me);
  ASSERT(iem_ice_me != Teuchos::null);
  ice_iem_ = iem_ice_me->get_IEM();

  // -- iem for gas
  me = S->GetFieldEvaluator(getKey(domain, "internal_energy_gas"));
  Teuchos::RCP<Energy::EnergyRelations::IEMWaterVaporEvaluator> iem_gas_me =
      Teuchos::rcp_dynamic_cast<Energy::EnergyRelations::IEMWaterVaporEvaluator>(me);
  ASSERT(iem_gas_me != Teuchos::null);
  gas_iem_ = iem_gas_me->get_IEM();

  // -- iem for rock
  me = S->GetFieldEvaluator(getKey(domain, "internal_energy_rock"));
  Teuchos::RCP<Energy::EnergyRelations::IEMEvaluator> iem_rock_me =
      Teuchos::rcp_dynamic_cast<Energy::EnergyRelations::IEMEvaluator>(me);
  ASSERT(iem_rock_me != Teuchos::null);
  rock_iem_ = iem_rock_me->get_IEM();

  // -- porosity

  poro_leij_ = plist.get<bool>("porosity leijnse model", false);
  me = S->GetFieldEvaluator(getKey(domain, "porosity"));
  if(!poro_leij_){
    Teuchos::RCP<Flow::FlowRelations::CompressiblePorosityEvaluator> poro_me =
      Teuchos::rcp_dynamic_cast<Flow::FlowRelations::CompressiblePorosityEvaluator>(me);
    ASSERT(poro_me != Teuchos::null);
    poro_models_ = poro_me->get_Models();
  }
  else{
    Teuchos::RCP<Flow::FlowRelations::CompressiblePorosityLeijnseEvaluator> poro_me =
      Teuchos::rcp_dynamic_cast<Flow::FlowRelations::CompressiblePorosityLeijnseEvaluator>(me);
    ASSERT(poro_me != Teuchos::null);
    poro_leij_models_ = poro_me->get_Models();
  }
  

}


void PermafrostModel::UpdateModel(const Teuchos::Ptr<State>& S, int c) {
  // update scalars
  p_atm_ = *S->GetScalarData("atmospheric_pressure");
  rho_rock_ = (*S->GetFieldData(getKey(domain,"density_rock"))->ViewComponent("cell"))[0][c];
  poro_ = (*S->GetFieldData(getKey(domain,"base_porosity"))->ViewComponent("cell"))[0][c];
  wrm_ = wrms_->second[(*wrms_->first)[c]];
  if(!poro_leij_)
    poro_model_ = poro_models_->second[(*poro_models_->first)[c]];
  else
    poro_leij_model_ = poro_leij_models_->second[(*poro_leij_models_->first)[c]];
    
  ASSERT(IsSetUp_());
}

bool PermafrostModel::IsSetUp_() {
  if (wrm_ == Teuchos::null) return false;
  if (!poro_leij_) {
    if (poro_model_ == Teuchos::null) return false;
  }
  else {
    if (poro_leij_model_ == Teuchos::null) return false;
  }
  if (liquid_eos_ == Teuchos::null) return false;
  if (gas_eos_ == Teuchos::null) return false;
  if (ice_eos_ == Teuchos::null) return false;
  if (pc_i_ == Teuchos::null) return false;
  if (pc_l_ == Teuchos::null) return false;
  if (vpr_ == Teuchos::null) return false;
  if (liquid_iem_ == Teuchos::null) return false;
  if (gas_iem_ == Teuchos::null) return false;
  if (ice_iem_ == Teuchos::null) return false;
  if (rock_iem_ == Teuchos::null) return false;
  if (rho_rock_ < 0.) return false;
  if (p_atm_ < -1.e10) return false;
  return true;
}


bool 
PermafrostModel::Freezing(double T, double p) {
  double eff_p = std::max(p_atm_, p);
  double rho_l = liquid_eos_->MolarDensity(T,eff_p);
  double mass_rho_l = liquid_eos_->MassDensity(T,eff_p);

  double pc_l = pc_l_->CapillaryPressure(p,p_atm_);
  double pc_i;
  if (pc_i_->IsMolarBasis()) {
    pc_i = pc_i_->CapillaryPressure(T, rho_l);
  } else {
    double mass_rho_l = liquid_eos_->MassDensity(T,eff_p);
    pc_i = pc_i_->CapillaryPressure(T, mass_rho_l);
  }

  return wrm_->freezing(T,pc_l,pc_i);
}


int PermafrostModel::EvaluateSaturations(double T, double p, double& s_gas, double& s_liq, double& s_ice) {
  int ierr = 0;
  try {
    double eff_p = std::max(p_atm_, p);
    double rho_l = liquid_eos_->MolarDensity(T,eff_p);
    double mass_rho_l = liquid_eos_->MassDensity(T,eff_p);

    double pc_l = pc_l_->CapillaryPressure(p, p_atm_);
    double pc_i;
    if (pc_i_->IsMolarBasis()) {
      pc_i = pc_i_->CapillaryPressure(T, rho_l);
    } else {
      double mass_rho_l = liquid_eos_->MassDensity(T,eff_p);
      pc_i = pc_i_->CapillaryPressure(T, mass_rho_l);
    }

    double sats[3];
    wrm_->saturations(pc_l, pc_i, sats);
    s_gas = sats[0];
    s_liq = sats[1];
    s_ice = sats[2];

  } catch (const Exceptions::Amanzi_exception& e) {
    if (e.what() == std::string("Cut time step")) {
      ierr = 1;
    }
  }

  return ierr;
}

int PermafrostModel::EvaluateEnergyAndWaterContent_(double T, double p, AmanziGeometry::Point& result) {
  if (T < 100.0 || T > 373.0) {
    return 1; // invalid temperature
  }
  int ierr = 0;
  try {
    double poro;
    if (!poro_leij_)
      poro = poro_model_->Porosity(poro_, p, p_atm_);
    else
      poro = poro_leij_model_->Porosity(poro_, p, p_atm_);
    
    double eff_p = std::max(p_atm_, p);

    double rho_l = liquid_eos_->MolarDensity(T,eff_p);
    double mass_rho_l = liquid_eos_->MassDensity(T,eff_p);
    double rho_i = ice_eos_->MolarDensity(T,eff_p);
    double rho_g = gas_eos_->MolarDensity(T,eff_p);
    double omega = vpr_->SaturatedVaporPressure(T)/p_atm_;

    double pc_i;
    if (pc_i_->IsMolarBasis()) {
      pc_i = pc_i_->CapillaryPressure(T, rho_l);
    } else {
      double mass_rho_l = liquid_eos_->MassDensity(T,eff_p);
      pc_i = pc_i_->CapillaryPressure(T, mass_rho_l);
    }

    double pc_l = pc_l_->CapillaryPressure(p, p_atm_);

    double sats[3];
    wrm_->saturations(pc_l, pc_i, sats);
    double s_g = sats[0];
    double s_l = sats[1];
    double s_i = sats[2];

    double u_l = liquid_iem_->InternalEnergy(T);
    double u_g = gas_iem_->InternalEnergy(T, omega);
    double u_i = ice_iem_->InternalEnergy(T);

    double u_rock = rock_iem_->InternalEnergy(T);

    // water content
    result[1] = poro * (rho_l * s_l + rho_i * s_i + rho_g * s_g * omega);

    // energy
    result[0] = poro * (u_l * rho_l * s_l + u_i * rho_i * s_i + u_g * rho_g * s_g)
        + (1.0 - poro_) * (rho_rock_ * u_rock);
  } catch (const Exceptions::Amanzi_exception& e) {
    if (e.what() == std::string("Cut time step")) {
      ierr = 1;
    }
  }

  return ierr;
}

}
