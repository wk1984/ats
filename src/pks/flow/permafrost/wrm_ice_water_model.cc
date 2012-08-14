/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/*
  This WRM model calls saturation and rel perm using a capillary pressure p_atm - pc.

  Authors: Ethan Coon (ecoon@lanl.gov)
*/

#include "wrm_ice_water_model.hh"


namespace Amanzi {
namespace Flow {
namespace FlowRelations {

WRMIceLiquidModel::WRMIceLiquidModel(Teuchos::ParameterList& wrm_plist) :
    WRMModel(wrm_plist) {
  InitializeFromPlist_();
}

WRMIceLiquidModel::WRMIceLiquidModel(Teuchos::ParameterList& wrm_plist,
        const Teuchos::RCP<WRM>& wrm) :
    WRMModel(wrm_plist, wrm) {
  InitializeFromPlist_();
}

WRMIceLiquidModel::WRMIceLiquidModel(const WRMIceLiquidModel& other) :
    WRMModel(other),
    temp_key_(other.temp_key_),
    dens_key_(other.dens_key_),
    calc_other_sat_(other.calc_other_sat_) {}

Teuchos::RCP<FieldModel>
WRMIceLiquidModel::Clone() const {
  return Teuchos::rcp(new WRMIceLiquidModel(*this));
}


void WRMIceLiquidModel::InitializeFromPlist_() {
  // my keys are for saturation and rel perm.
  my_keys_.push_back(wrm_plist_.get<string>("saturation key", "saturation_liquid"));

  calc_other_sat_ = wrm_plist_.get<bool>("calculate minor saturation", true);
  if (calc_other_sat_) {
    ASSERT(0);  // this is not implemented, as it physically doesn't make sense.
    //my_keys_.push_back(wrm_plist_.get<string>("other saturation key", "one_minus_sat_liq"));
  }

  // my dependencies are temperature and density
  temp_key_ = wrm_plist_.get<string>("temperature key", "temperature");
  dependencies_.insert(temp_key_);

  dens_key_ = wrm_plist_.get<string>("density key", "molar_density_liquid");
  dependencies_.insert(dens_key_);

  // get the physical model for P_{c-il}(T)
  ASSERT(wrm_plist_.isSublist("capillary pressure of ice-liquid"));
  Teuchos::ParameterList pc_plist = wrm_plist_.sublist("capillary pressure of ice-liquid");
  pc_ = Teuchos::rcp(new PCIceWater(pc_plist));

}


void WRMIceLiquidModel::EvaluateField_(const Teuchos::Ptr<State>& S,
        const std::vector<Teuchos::Ptr<CompositeVector> >& results) {
  Teuchos::Ptr<CompositeVector> sat = results[0];

  Teuchos::RCP<const CompositeVector> temp = S->GetFieldData(temp_key_);
  Teuchos::RCP<const CompositeVector> dens = S->GetFieldData(dens_key_);

  // Loop over names in the target and then owned entities in that name,
  // evaluating the model to calculate sat.
  for (CompositeVector::name_iterator comp=sat->begin();
       comp!=sat->end(); ++comp) {
    for (int id=0; id!=sat->size(*comp); ++id) {
      double pc = pc_->CapillaryPressure((*temp)(*comp, id), (*dens)(*comp, id));
      (*sat)(*comp, id) = wrm_->saturation(pc);
    }
  }
}


void WRMIceLiquidModel::EvaluateFieldPartialDerivative_(const Teuchos::Ptr<State>& S,
        Key wrt_key, const std::vector<Teuchos::Ptr<CompositeVector> > & results) {
  Teuchos::Ptr<CompositeVector> dsat = results[0];

  Teuchos::RCP<const CompositeVector> temp = S->GetFieldData(temp_key_);
  Teuchos::RCP<const CompositeVector> dens = S->GetFieldData(dens_key_);

  // Loop over names in the target and then owned entities in that name,
  // evaluating the model to calculate sat and rel perm.
  if (wrt_key == temp_key_) {
    for (CompositeVector::name_iterator comp=dsat->begin();
         comp!=dsat->end(); ++comp) {
      for (int id=0; id!=dsat->size(*comp); ++id) {
        double pc = pc_->CapillaryPressure((*temp)(*comp, id), (*dens)(*comp, id));
        (*dsat)(*comp, id) = wrm_->d_saturation(pc)
            * pc_->DCapillaryPressureDT((*temp)(*comp, id), (*dens)(*comp, id));
      }
    }
  } else if (wrt_key == dens_key_) {
    for (CompositeVector::name_iterator comp=dsat->begin();
         comp!=dsat->end(); ++comp) {
      for (int id=0; id!=dsat->size(*comp); ++id) {
        double pc = pc_->CapillaryPressure((*temp)(*comp, id), (*dens)(*comp, id));
        (*dsat)(*comp, id) = wrm_->d_saturation(pc)
            * pc_->DCapillaryPressureDRho((*temp)(*comp, id), (*dens)(*comp, id));
      }
    }
  } else {
    ASSERT(0);
  }
}

} // namespace
} // namespace
} // namespace



