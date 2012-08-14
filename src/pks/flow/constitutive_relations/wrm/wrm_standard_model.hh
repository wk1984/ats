/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/*
  This WRM model calls saturation and rel perm using a capillary pressure p_atm - pc.

  Authors: Ethan Coon (ecoon@lanl.gov)
*/

#ifndef AMANZI_FLOWRELATIONS_WRM_STANDARD_MODEL_
#define AMANZI_FLOWRELATIONS_WRM_STANDARD_MODEL_

#include "wrm_model.hh"

namespace Amanzi {
namespace Flow {
namespace FlowRelations {

class WRM; // forward declaration

class WRMStandardModel : public WRMModel {
 public:

  explicit
  WRMStandardModel(Teuchos::ParameterList& wrm_plist);

  WRMStandardModel(Teuchos::ParameterList& wrm_plist, const Teuchos::RCP<WRM>& wrm);

  WRMStandardModel(const WRMStandardModel& other);

  virtual Teuchos::RCP<FieldModel> Clone() const;

 protected:
  void InitializeFromPlist_();

  // Required methods from SecondaryVariableFieldModel
  virtual void EvaluateField_(const Teuchos::Ptr<State>& S,
          const std::vector<Teuchos::Ptr<CompositeVector> >& results);
  virtual void EvaluateFieldPartialDerivative_(const Teuchos::Ptr<State>& S,
          Key wrt_key, const std::vector<Teuchos::Ptr<CompositeVector> > & results);

 protected:
  Key pres_key_;
  bool calc_other_sat_;

};

} // namespae
} // namespae
} // namespae

#endif
