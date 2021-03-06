/*
  The richards water content with vapor evaluator is an algebraic evaluator of a given model.
Richards water content with vapor.  
  Generated via evaluator_generator.
*/

#include "richards_water_content_with_vapor_evaluator.hh"
#include "richards_water_content_with_vapor_model.hh"

namespace Amanzi {
namespace Flow {
namespace Relations {

// Constructor from ParameterList
RichardsWaterContentWithVaporEvaluator::RichardsWaterContentWithVaporEvaluator(Teuchos::ParameterList& plist) :
    SecondaryVariableFieldEvaluator(plist)
{
  Teuchos::ParameterList& sublist = plist_.sublist("richards_water_content_with_vapor parameters");
  model_ = Teuchos::rcp(new RichardsWaterContentWithVaporModel(sublist));
  InitializeFromPlist_();
}


// Copy constructor
RichardsWaterContentWithVaporEvaluator::RichardsWaterContentWithVaporEvaluator(const RichardsWaterContentWithVaporEvaluator& other) :
    SecondaryVariableFieldEvaluator(other),
    phi_key_(other.phi_key_),
    sl_key_(other.sl_key_),
    sg_key_(other.sg_key_),
    nl_key_(other.nl_key_),
    ng_key_(other.ng_key_),
    omega_key_(other.omega_key_),
    cv_key_(other.cv_key_),    
    model_(other.model_) {}


// Virtual copy constructor
Teuchos::RCP<FieldEvaluator>
RichardsWaterContentWithVaporEvaluator::Clone() const
{
  return Teuchos::rcp(new RichardsWaterContentWithVaporEvaluator(*this));
}


// Initialize by setting up dependencies
void
RichardsWaterContentWithVaporEvaluator::InitializeFromPlist_()
{
  // Set up my dependencies
  // - defaults to prefixed via domain
  Key domain_name = getDomainPrefix(my_key_);

  // - pull Keys from plist
  // dependency: porosity
  phi_key_ = plist_.get<std::string>("porosity key",
          domain_name+"porosity");
  dependencies_.insert(phi_key_);

  // dependency: saturation_liquid
  sl_key_ = plist_.get<std::string>("saturation liquid key",
          domain_name+"saturation_liquid");
  dependencies_.insert(sl_key_);

  // dependency: saturation_gas
  sg_key_ = plist_.get<std::string>("saturation gas key",
          domain_name+"saturation_gas");
  dependencies_.insert(sg_key_);

  // dependency: molar_density_liquid
  nl_key_ = plist_.get<std::string>("molar density liquid key",
          domain_name+"molar_density_liquid");
  dependencies_.insert(nl_key_);

  // dependency: molar_density_gas
  ng_key_ = plist_.get<std::string>("molar density gas key",
          domain_name+"molar_density_gas");
  dependencies_.insert(ng_key_);

  // dependency: mol_fraction_vapor_in_gas
  omega_key_ = plist_.get<std::string>("mol fraction vapor in gas key",
          domain_name+"mol_fraction_vapor_in_gas");
  dependencies_.insert(omega_key_);

  // dependency: cell_volume
  cv_key_ = plist_.get<std::string>("cell volume key",
          domain_name+"cell_volume");
  dependencies_.insert(cv_key_);
}


void
RichardsWaterContentWithVaporEvaluator::EvaluateField_(const Teuchos::Ptr<State>& S,
        const Teuchos::Ptr<CompositeVector>& result)
{
Teuchos::RCP<const CompositeVector> phi = S->GetFieldData(phi_key_);
Teuchos::RCP<const CompositeVector> sl = S->GetFieldData(sl_key_);
Teuchos::RCP<const CompositeVector> sg = S->GetFieldData(sg_key_);
Teuchos::RCP<const CompositeVector> nl = S->GetFieldData(nl_key_);
Teuchos::RCP<const CompositeVector> ng = S->GetFieldData(ng_key_);
Teuchos::RCP<const CompositeVector> omega = S->GetFieldData(omega_key_);
Teuchos::RCP<const CompositeVector> cv = S->GetFieldData(cv_key_);

  for (CompositeVector::name_iterator comp=result->begin();
       comp!=result->end(); ++comp) {
    const Epetra_MultiVector& phi_v = *phi->ViewComponent(*comp, false);
    const Epetra_MultiVector& sl_v = *sl->ViewComponent(*comp, false);
    const Epetra_MultiVector& sg_v = *sg->ViewComponent(*comp, false);
    const Epetra_MultiVector& nl_v = *nl->ViewComponent(*comp, false);
    const Epetra_MultiVector& ng_v = *ng->ViewComponent(*comp, false);
    const Epetra_MultiVector& omega_v = *omega->ViewComponent(*comp, false);
    const Epetra_MultiVector& cv_v = *cv->ViewComponent(*comp, false);
    Epetra_MultiVector& result_v = *result->ViewComponent(*comp,false);

    int ncomp = result->size(*comp, false);
    for (int i=0; i!=ncomp; ++i) {
      result_v[0][i] = model_->WaterContent(phi_v[0][i], sl_v[0][i], sg_v[0][i], nl_v[0][i], ng_v[0][i], omega_v[0][i], cv_v[0][i]);
    }
  }
}


void
RichardsWaterContentWithVaporEvaluator::EvaluateFieldPartialDerivative_(const Teuchos::Ptr<State>& S,
        Key wrt_key, const Teuchos::Ptr<CompositeVector>& result)
{
Teuchos::RCP<const CompositeVector> phi = S->GetFieldData(phi_key_);
Teuchos::RCP<const CompositeVector> sl = S->GetFieldData(sl_key_);
Teuchos::RCP<const CompositeVector> sg = S->GetFieldData(sg_key_);
Teuchos::RCP<const CompositeVector> nl = S->GetFieldData(nl_key_);
Teuchos::RCP<const CompositeVector> ng = S->GetFieldData(ng_key_);
Teuchos::RCP<const CompositeVector> omega = S->GetFieldData(omega_key_);
Teuchos::RCP<const CompositeVector> cv = S->GetFieldData(cv_key_);

  if (wrt_key == phi_key_) {
    for (CompositeVector::name_iterator comp=result->begin();
         comp!=result->end(); ++comp) {
      const Epetra_MultiVector& phi_v = *phi->ViewComponent(*comp, false);
      const Epetra_MultiVector& sl_v = *sl->ViewComponent(*comp, false);
      const Epetra_MultiVector& sg_v = *sg->ViewComponent(*comp, false);
      const Epetra_MultiVector& nl_v = *nl->ViewComponent(*comp, false);
      const Epetra_MultiVector& ng_v = *ng->ViewComponent(*comp, false);
      const Epetra_MultiVector& omega_v = *omega->ViewComponent(*comp, false);
      const Epetra_MultiVector& cv_v = *cv->ViewComponent(*comp, false);
      Epetra_MultiVector& result_v = *result->ViewComponent(*comp,false);

      int ncomp = result->size(*comp, false);
      for (int i=0; i!=ncomp; ++i) {
        result_v[0][i] = model_->DWaterContentDPorosity(phi_v[0][i], sl_v[0][i], sg_v[0][i], nl_v[0][i], ng_v[0][i], omega_v[0][i], cv_v[0][i]);
      }
    }

  } else if (wrt_key == sl_key_) {
    for (CompositeVector::name_iterator comp=result->begin();
         comp!=result->end(); ++comp) {
      const Epetra_MultiVector& phi_v = *phi->ViewComponent(*comp, false);
      const Epetra_MultiVector& sl_v = *sl->ViewComponent(*comp, false);
      const Epetra_MultiVector& sg_v = *sg->ViewComponent(*comp, false);
      const Epetra_MultiVector& nl_v = *nl->ViewComponent(*comp, false);
      const Epetra_MultiVector& ng_v = *ng->ViewComponent(*comp, false);
      const Epetra_MultiVector& omega_v = *omega->ViewComponent(*comp, false);
      const Epetra_MultiVector& cv_v = *cv->ViewComponent(*comp, false);
      Epetra_MultiVector& result_v = *result->ViewComponent(*comp,false);

      int ncomp = result->size(*comp, false);
      for (int i=0; i!=ncomp; ++i) {
        result_v[0][i] = model_->DWaterContentDSaturationLiquid(phi_v[0][i], sl_v[0][i], sg_v[0][i], nl_v[0][i], ng_v[0][i], omega_v[0][i], cv_v[0][i]);
      }
    }

  } else if (wrt_key == sg_key_) {
    for (CompositeVector::name_iterator comp=result->begin();
         comp!=result->end(); ++comp) {
      const Epetra_MultiVector& phi_v = *phi->ViewComponent(*comp, false);
      const Epetra_MultiVector& sl_v = *sl->ViewComponent(*comp, false);
      const Epetra_MultiVector& sg_v = *sg->ViewComponent(*comp, false);
      const Epetra_MultiVector& nl_v = *nl->ViewComponent(*comp, false);
      const Epetra_MultiVector& ng_v = *ng->ViewComponent(*comp, false);
      const Epetra_MultiVector& omega_v = *omega->ViewComponent(*comp, false);
      const Epetra_MultiVector& cv_v = *cv->ViewComponent(*comp, false);
      Epetra_MultiVector& result_v = *result->ViewComponent(*comp,false);

      int ncomp = result->size(*comp, false);
      for (int i=0; i!=ncomp; ++i) {
        result_v[0][i] = model_->DWaterContentDSaturationGas(phi_v[0][i], sl_v[0][i], sg_v[0][i], nl_v[0][i], ng_v[0][i], omega_v[0][i], cv_v[0][i]);
      }
    }

  } else if (wrt_key == nl_key_) {
    for (CompositeVector::name_iterator comp=result->begin();
         comp!=result->end(); ++comp) {
      const Epetra_MultiVector& phi_v = *phi->ViewComponent(*comp, false);
      const Epetra_MultiVector& sl_v = *sl->ViewComponent(*comp, false);
      const Epetra_MultiVector& sg_v = *sg->ViewComponent(*comp, false);
      const Epetra_MultiVector& nl_v = *nl->ViewComponent(*comp, false);
      const Epetra_MultiVector& ng_v = *ng->ViewComponent(*comp, false);
      const Epetra_MultiVector& omega_v = *omega->ViewComponent(*comp, false);
      const Epetra_MultiVector& cv_v = *cv->ViewComponent(*comp, false);
      Epetra_MultiVector& result_v = *result->ViewComponent(*comp,false);

      int ncomp = result->size(*comp, false);
      for (int i=0; i!=ncomp; ++i) {
        result_v[0][i] = model_->DWaterContentDMolarDensityLiquid(phi_v[0][i], sl_v[0][i], sg_v[0][i], nl_v[0][i], ng_v[0][i], omega_v[0][i], cv_v[0][i]);
      }
    }

  } else if (wrt_key == ng_key_) {
    for (CompositeVector::name_iterator comp=result->begin();
         comp!=result->end(); ++comp) {
      const Epetra_MultiVector& phi_v = *phi->ViewComponent(*comp, false);
      const Epetra_MultiVector& sl_v = *sl->ViewComponent(*comp, false);
      const Epetra_MultiVector& sg_v = *sg->ViewComponent(*comp, false);
      const Epetra_MultiVector& nl_v = *nl->ViewComponent(*comp, false);
      const Epetra_MultiVector& ng_v = *ng->ViewComponent(*comp, false);
      const Epetra_MultiVector& omega_v = *omega->ViewComponent(*comp, false);
      const Epetra_MultiVector& cv_v = *cv->ViewComponent(*comp, false);
      Epetra_MultiVector& result_v = *result->ViewComponent(*comp,false);

      int ncomp = result->size(*comp, false);
      for (int i=0; i!=ncomp; ++i) {
        result_v[0][i] = model_->DWaterContentDMolarDensityGas(phi_v[0][i], sl_v[0][i], sg_v[0][i], nl_v[0][i], ng_v[0][i], omega_v[0][i], cv_v[0][i]);
      }
    }

  } else if (wrt_key == omega_key_) {
    for (CompositeVector::name_iterator comp=result->begin();
         comp!=result->end(); ++comp) {
      const Epetra_MultiVector& phi_v = *phi->ViewComponent(*comp, false);
      const Epetra_MultiVector& sl_v = *sl->ViewComponent(*comp, false);
      const Epetra_MultiVector& sg_v = *sg->ViewComponent(*comp, false);
      const Epetra_MultiVector& nl_v = *nl->ViewComponent(*comp, false);
      const Epetra_MultiVector& ng_v = *ng->ViewComponent(*comp, false);
      const Epetra_MultiVector& omega_v = *omega->ViewComponent(*comp, false);
      const Epetra_MultiVector& cv_v = *cv->ViewComponent(*comp, false);
      Epetra_MultiVector& result_v = *result->ViewComponent(*comp,false);

      int ncomp = result->size(*comp, false);
      for (int i=0; i!=ncomp; ++i) {
        result_v[0][i] = model_->DWaterContentDMolFractionVaporInGas(phi_v[0][i], sl_v[0][i], sg_v[0][i], nl_v[0][i], ng_v[0][i], omega_v[0][i], cv_v[0][i]);
      }
    }

  } else if (wrt_key == cv_key_) {
    for (CompositeVector::name_iterator comp=result->begin();
         comp!=result->end(); ++comp) {
      const Epetra_MultiVector& phi_v = *phi->ViewComponent(*comp, false);
      const Epetra_MultiVector& sl_v = *sl->ViewComponent(*comp, false);
      const Epetra_MultiVector& sg_v = *sg->ViewComponent(*comp, false);
      const Epetra_MultiVector& nl_v = *nl->ViewComponent(*comp, false);
      const Epetra_MultiVector& ng_v = *ng->ViewComponent(*comp, false);
      const Epetra_MultiVector& omega_v = *omega->ViewComponent(*comp, false);
      const Epetra_MultiVector& cv_v = *cv->ViewComponent(*comp, false);
      Epetra_MultiVector& result_v = *result->ViewComponent(*comp,false);

      int ncomp = result->size(*comp, false);
      for (int i=0; i!=ncomp; ++i) {
        result_v[0][i] = model_->DWaterContentDCellVolume(phi_v[0][i], sl_v[0][i], sg_v[0][i], nl_v[0][i], ng_v[0][i], omega_v[0][i], cv_v[0][i]);
      }
    }

  } else {
    ASSERT(0);
  }
}


} //namespace
} //namespace
} //namespace
