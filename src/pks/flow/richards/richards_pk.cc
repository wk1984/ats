/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/* -------------------------------------------------------------------------
This is the flow component of the Amanzi code.
License: BSD
Authors: Neil Carlson (version 1)
         Konstantin Lipnikov (version 2) (lipnikov@lanl.gov)
         Ethan Coon (ATS version) (ecoon@lanl.gov)
------------------------------------------------------------------------- */
#include "boost/math/special_functions/fpclassify.hpp"

#include "Epetra_Import.h"

#include "flow_bc_factory.hh"

#include "Point.hh"

#include "upwind_cell_centered.hh"
#include "upwind_arithmetic_mean.hh"
#include "upwind_total_flux.hh"
#include "upwind_gravity_flux.hh"

#include "CompositeVectorFunction.hh"
#include "CompositeVectorFunctionFactory.hh"

#include "predictor_delegate_bc_flux.hh"
#include "wrm_evaluator.hh"
#include "rel_perm_evaluator.hh"
#include "richards_water_content_evaluator.hh"
#include "OperatorDefs.hh"

#include "richards.hh"

#define DEBUG_RES_FLAG 0


namespace Amanzi {
namespace Flow {

// -------------------------------------------------------------
// Constructor
// -------------------------------------------------------------
Richards::Richards(Teuchos::ParameterList& pk_tree,
                   const Teuchos::RCP<Teuchos::ParameterList>& glist,
                   const Teuchos::RCP<State>& S,
                   const Teuchos::RCP<TreeVector>& solution) :
    PK(pk_tree, glist,  S, solution),
    PK_PhysicalBDF_Default(pk_tree, glist,  S, solution),
    coupled_to_surface_via_head_(false),
    coupled_to_surface_via_flux_(false),
    infiltrate_only_if_unfrozen_(false),
    modify_predictor_with_consistent_faces_(false),
    modify_predictor_wc_(false),
    modify_predictor_bc_flux_(false),
    modify_predictor_first_bc_flux_(false),
    upwind_from_prev_flux_(false),
    precon_wc_(false),
    dynamic_mesh_(false),
    clobber_surf_kr_(false),
    clobber_boundary_flux_dir_(false),
    vapor_diffusion_(false),
    perm_scale_(1.)
{
  if (!plist_->isParameter("conserved quantity suffix"))
    plist_->set("conserved quantity suffix", "water_content");
  
  // set a default absolute tolerance
  if (!plist_->isParameter("absolute error tolerance"))
    plist_->set("absolute error tolerance", .5 * .1 * 55000.); // phi * s * nl
}

// -------------------------------------------------------------
// Setup data
// -------------------------------------------------------------
void Richards::Setup(const Teuchos::Ptr<State>& S) {
  
  PK_PhysicalBDF_Default::Setup(S);
  
  SetupRichardsFlow_(S);
  SetupPhysicalEvaluators_(S);

  flux_tol_ = plist_->get<double>("flux tolerance", 1.);
};


// -------------------------------------------------------------
// Pieces of the construction process that are common to all
// Richards-like PKs.
// -------------------------------------------------------------
void Richards::SetupRichardsFlow_(const Teuchos::Ptr<State>& S) {
  if (mass_dens_key_.empty()) {
    mass_dens_key_ = plist_->get<std::string>("mass density key",
            getKey(domain_, "mass_density_liquid"));
  }
  if (molar_dens_key_.empty()) {
    molar_dens_key_ = plist_->get<std::string>("molar density key",
            getKey(domain_, "molar_density_liquid"));
  }
  if (perm_key_.empty()) {
    perm_key_ = plist_->get<std::string>("permeability key",
            getKey(domain_, "permeability"));
  }
  if (coef_key_.empty()) {
    coef_key_ = plist_->get<std::string>("conductivity key",
            getKey(domain_, "relative_permeability"));
  }
  
  if (uw_coef_key_.empty()) {
    uw_coef_key_ = plist_->get<std::string>("upwinded conductivity key",
            getKey(domain_, "upwind_relative_permeability"));
  }
  if (flux_key_.empty()) {
    flux_key_ = plist_->get<std::string>("darcy flux key",
            getKey(domain_, "mass_flux"));
  }
  if (flux_dir_key_.empty()) {
    flux_dir_key_ = plist_->get<std::string>("darcy flux direction key",
            getKey(domain_, "mass_flux_direction")); 
  }
  if (velocity_key_.empty()) {
    velocity_key_ = plist_->get<std::string>("darcy velocity key",
            getKey(domain_, "darcy_velocity"));
  }
  if (sat_key_.empty()) {
    sat_key_ = plist_->get<std::string>("saturation key",
            getKey(domain_, "saturation_liquid"));
  }
  if (sat_ice_key_.empty()) {
    sat_ice_key_ = plist_->get<std::string>("saturation ice key",
            getKey(domain_, "saturation_ice"));
  }
  
  // Get data for special-case entities.
  S->RequireField(cell_vol_key_)->SetMesh(mesh_)
      ->AddComponent("cell", AmanziMesh::CELL, 1);

  S->RequireFieldEvaluator(cell_vol_key_);
  S->RequireGravity();
  S->RequireScalar("atmospheric_pressure");

  // Set up Operators
  // -- boundary conditions
  Teuchos::ParameterList bc_plist = plist_->sublist("boundary conditions", true);
  FlowBCFactory bc_factory(mesh_, bc_plist);
  bc_pressure_ = bc_factory.CreatePressure();
  bc_head_ = bc_factory.CreateHead();
  bc_flux_ = bc_factory.CreateMassFlux();
  bc_seepage_ = bc_factory.CreateSeepageFacePressure();
  bc_seepage_->Compute(0.); // compute at t=0 to set up
  bc_seepage_infilt_ = bc_factory.CreateSeepageFacePressureWithInfiltration();
  bc_seepage_infilt_->Compute(0.); // compute at t=0 to set up

  int nfaces = mesh_->num_entities(AmanziMesh::FACE, AmanziMesh::USED);
  bc_markers_.resize(nfaces, Operators::OPERATOR_BC_NONE);
  bc_values_.resize(nfaces, 0.0);
  std::vector<double> mixed;
  bc_ = Teuchos::rcp(new Operators::BCs(Operators::OPERATOR_BC_TYPE_FACE, bc_markers_, bc_values_, mixed));

  // -- linear tensor coefficients
  unsigned int c_owned = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  K_ = Teuchos::rcp(new std::vector<WhetStone::Tensor>(c_owned));
  for (unsigned int c=0; c!=c_owned; ++c) {
    (*K_)[c].Init(mesh_->space_dimension(),1);
  }
  // scaling for permeability
  perm_scale_ = plist_->get<double>("permeability rescaling", 1.0);
  
  // -- nonlinear coefficients/upwinding
  Teuchos::ParameterList& wrm_plist = plist_->sublist("water retention evaluator");
  clobber_surf_kr_ = plist_->get<bool>("clobber surface rel perm", false);
  clobber_boundary_flux_dir_ = plist_->get<bool>("clobber boundary flux direction for upwinding", false);

  std::string method_name = plist_->get<std::string>("relative permeability method", "upwind with Darcy flux");

  if (method_name == "upwind with gravity") {
    upwinding_ = Teuchos::rcp(new Operators::UpwindGravityFlux(name_,
            coef_key_, uw_coef_key_, K_));
    Krel_method_ = Operators::UPWIND_METHOD_GRAVITY;
  } else if (method_name == "cell centered") {
    upwinding_ = Teuchos::rcp(new Operators::UpwindCellCentered(name_,
            coef_key_, uw_coef_key_));
    Krel_method_ = Operators::UPWIND_METHOD_CENTERED;
  } else if (method_name == "upwind with Darcy flux") {
    upwinding_ = Teuchos::rcp(new Operators::UpwindTotalFlux(name_,
            coef_key_, uw_coef_key_, flux_dir_key_, 1.e-5));
    Krel_method_ = Operators::UPWIND_METHOD_TOTAL_FLUX;
  } else if (method_name == "arithmetic mean") {
    upwinding_ = Teuchos::rcp(new Operators::UpwindArithmeticMean(name_,
            coef_key_, uw_coef_key_));
    Krel_method_ = Operators::UPWIND_METHOD_ARITHMETIC_MEAN;
  } else {
    std::stringstream messagestream;
    messagestream << "Richards Flow PK has no upwinding method named: " << method_name;
    Errors::Message message(messagestream.str());
    Exceptions::amanzi_throw(message);
  }

  // -- require the data on appropriate locations
  std::string coef_location = upwinding_->CoefficientLocation();
  if (coef_location == "upwind: face") {  
    S->RequireField(uw_coef_key_, name_)->SetMesh(mesh_)
        ->SetGhosted()->SetComponent("face", AmanziMesh::FACE, 1);
  } else if (coef_location == "standard: cell") {
    S->RequireField(uw_coef_key_, name_)->SetMesh(mesh_)
        ->SetGhosted()->SetComponent("cell", AmanziMesh::CELL, 1);
  } else {
    Errors::Message message("Unknown upwind coefficient location in Richards flow.");
    Exceptions::amanzi_throw(message);
  }
  S->GetField(uw_coef_key_,name_)->set_io_vis(false);

  // -- create the forward operator for the diffusion term
  Teuchos::ParameterList& mfd_plist = plist_->sublist("diffusion");
  mfd_plist.set("nonlinear coefficient", coef_location);
  mfd_plist.set("gravity", true);
  
  Operators::OperatorDiffusionFactory opfactory;
  matrix_diff_ = opfactory.CreateWithGravity(mfd_plist, mesh_, bc_);
  matrix_ = matrix_diff_->global_operator();

  // -- create the operator, data for flux directions
  Teuchos::ParameterList face_diff_list(mfd_plist);
  face_diff_list.set("nonlinear coefficient", "none");
  face_matrix_diff_ = opfactory.CreateWithGravity(face_diff_list, mesh_, bc_);

  S->RequireField(flux_dir_key_, name_)->SetMesh(mesh_)->SetGhosted()
      ->SetComponent("face", AmanziMesh::FACE, 1);

  // -- create the operators for the preconditioner
  //    diffusion
  Teuchos::ParameterList& mfd_pc_plist = plist_->sublist("diffusion preconditioner");
  mfd_pc_plist.set("nonlinear coefficient", coef_location);
  mfd_pc_plist.set("gravity", true);
  if (!mfd_pc_plist.isParameter("discretization primary"))
    mfd_pc_plist.set("discretization primary", mfd_plist.get<std::string>("discretization primary"));
  if (!mfd_pc_plist.isParameter("discretization secondary") && mfd_plist.isParameter("discretization secondary"))
    mfd_pc_plist.set("discretization secondary", mfd_plist.get<std::string>("discretization secondary"));
  if (!mfd_pc_plist.isParameter("schema") && mfd_plist.isParameter("schema"))
    mfd_pc_plist.set("schema", mfd_plist.get<Teuchos::Array<std::string> >("schema"));

  preconditioner_diff_ = opfactory.CreateWithGravity(mfd_pc_plist, mesh_, bc_);
  preconditioner_ = preconditioner_diff_->global_operator();
  
  //    If using approximate Jacobian for the preconditioner, we also need derivative information.
  //    For now this means upwinding the derivative.
  jacobian_ = mfd_pc_plist.get<std::string>("Newton correction", "none") != "none";
  if (jacobian_) {
    if (preconditioner_->RangeMap().HasComponent("face")) {
      // MFD -- upwind required
      dcoef_key_ = getDerivKey(coef_key_, key_);
      duw_coef_key_ = getDerivKey(uw_coef_key_, key_);
        
      S->RequireField(duw_coef_key_, name_)
        ->SetMesh(mesh_)->SetGhosted()
        ->SetComponent("face", AmanziMesh::FACE, 1);

      upwinding_deriv_ = Teuchos::rcp(new Operators::UpwindTotalFlux(name_,
                                      dcoef_key_, duw_coef_key_, flux_dir_key_, 1.e-8));

    } else {
      // FV -- no upwinding
      dcoef_key_ = getDerivKey(coef_key_, key_);
      duw_coef_key_ = std::string();
    }
  }
  
  // -- accumulation terms
  Teuchos::ParameterList& acc_pc_plist = plist_->sublist("Accumulation PC");
  acc_pc_plist.set("entity kind", "cell");
  preconditioner_acc_ = Teuchos::rcp(new Operators::OperatorAccumulation(acc_pc_plist, preconditioner_));

  // // -- vapor diffusion terms
  // vapor_diffusion_ = plist_->get<bool>("include vapor diffusion", false);
  // if (vapor_diffusion_){
  //   ASSERT(0); // untested!
    
  //   // Create the vapor diffusion vectors
  //   S->RequireField("vapor_diffusion_pressure", name_)->SetMesh(mesh_)->SetGhosted()
  //       ->SetComponent("cell", AmanziMesh::CELL, 1);
  //   S->GetField("vapor_diffusion_pressure",name_)->set_io_vis(true);

  //   S->RequireField("vapor_diffusion_temperature", name_)->SetMesh(mesh_)->SetGhosted()
  //     ->SetComponent("cell", AmanziMesh::CELL, 1);
  //   S->GetField("vapor_diffusion_temperature",name_)->set_io_vis(true);

  //   // operator for the vapor diffusion terms
  //   matrix_vapor_ = Operators::CreateMatrixMFD(mfd_plist, mesh_);
  // }

  //    symbolic assemble
  precon_used_ = plist_->isSublist("preconditioner");
  if (precon_used_) {
    preconditioner_->SymbolicAssembleMatrix();
  
    //    Potentially create a linear solver
    if (plist_->isSublist("linear solver")) {
      Teuchos::ParameterList linsolve_sublist = plist_->sublist("linear solver");
      AmanziSolvers::LinearOperatorFactory<Operators::Operator,CompositeVector,CompositeVectorSpace> fac;
      lin_solver_ = fac.Create(linsolve_sublist, preconditioner_);
    } else {
      lin_solver_ = preconditioner_;
    }
  }

  // -- PC control
  precon_wc_ = plist_->get<bool>("precondition using WC", false);
  
  // source terms
  is_source_term_ = plist_->get<bool>("source term", false);
  if (is_source_term_) {
    if (source_key_.empty()) {
      source_key_ = plist_->get<std::string>("mass source key",
              getKey(domain_, "mass_source"));
    }

    source_term_is_differentiable_ =
        plist_->get<bool>("source term is differentiable", true);
    explicit_source_ = plist_->get<bool>("explicit source term", false);
    S->RequireField(source_key_)->SetMesh(mesh_)
        ->AddComponent("cell", AmanziMesh::CELL, 1);
    S->RequireFieldEvaluator(source_key_);
  }

  // coupling
  // -- coupling done by a Neumann condition
  coupled_to_surface_via_flux_ = plist_->get<bool>("coupled to surface via flux", false);
  if (coupled_to_surface_via_flux_) {
    if (ss_flux_key_.empty()) {
      ss_flux_key_ = plist_->get<std::string>("surface-subsurface flux key",
              getKey(domain_, "surface_subsurface_flux"));
    }

    S->RequireField(ss_flux_key_)
        ->SetMesh(S->GetMesh("surface"))
        ->AddComponent("cell", AmanziMesh::CELL, 1);
  }

  // -- coupling done by a Dirichlet condition
  coupled_to_surface_via_head_ = plist_->get<bool>("coupled to surface via head", false);
  if (coupled_to_surface_via_head_) {
    S->RequireField("surface_pressure");
  }

  // -- Make sure coupling isn't flagged multiple ways.
  if (coupled_to_surface_via_flux_ && coupled_to_surface_via_head_) {
    Errors::Message message("Richards PK requested both flux and head coupling -- choose one.");
    Exceptions::amanzi_throw(message);
  }

  // predictors for time integration
  modify_predictor_with_consistent_faces_ =
    plist_->get<bool>("modify predictor with consistent faces", false);
  modify_predictor_bc_flux_ =
    plist_->get<bool>("modify predictor for flux BCs", false);
  modify_predictor_first_bc_flux_ =
    plist_->get<bool>("modify predictor for initial flux BCs", false);
  modify_predictor_wc_ =
    plist_->get<bool>("modify predictor via water content", false);

  // correctors
  p_limit_ = plist_->get<double>("limit correction to pressure change [Pa]", -1.);
  patm_limit_ = plist_->get<double>("limit correction to pressure change when crossing atmospheric [Pa]", -1.);

  // valid step controls
  sat_change_limit_ = plist_->get<double>("max valid change in saturation in a time step [-]", -1.);
  sat_ice_change_limit_ = plist_->get<double>("max valid change in ice saturation in a time step [-]", -1.);

  // Require fields and evaluators for those fields.
  // -- primary variables
  S->RequireField(key_, name_)->Update(matrix_->RangeMap())->SetGhosted();

  // -- secondary variables, with no evaluator used
  S->RequireField(flux_key_, name_)->SetMesh(mesh_)->SetGhosted()
                                ->SetComponent("face", AmanziMesh::FACE, 1);
  S->RequireField(velocity_key_, name_)->SetMesh(mesh_)->SetGhosted()
                                ->SetComponent("cell", AmanziMesh::CELL, 3);

  
}


// -------------------------------------------------------------
// Create the physical evaluators for water content, water
// retention, rel perm, etc, that are specific to Richards.
// -------------------------------------------------------------
void Richards::SetupPhysicalEvaluators_(const Teuchos::Ptr<State>& S) {
  // -- Absolute permeability.
  //       For now, we assume scalar permeability.  This will change.
  S->RequireField(perm_key_)->SetMesh(mesh_)->SetGhosted()
      ->AddComponent("cell", AmanziMesh::CELL, 1);
  S->RequireFieldEvaluator(perm_key_);

  // -- water content, and evaluator
  S->RequireField(conserved_key_)->SetMesh(mesh_)->SetGhosted()
      ->AddComponent("cell", AmanziMesh::CELL, 1);
  S->RequireFieldEvaluator(conserved_key_);

  // -- Water retention evaluators
  // -- saturation
  Teuchos::ParameterList& wrm_plist = plist_->sublist("water retention evaluator");
  Teuchos::RCP<FlowRelations::WRMEvaluator> wrm =
      Teuchos::rcp(new FlowRelations::WRMEvaluator(wrm_plist));
  S->SetFieldEvaluator(getKey(domain_,"saturation_liquid"), wrm);
  S->SetFieldEvaluator(getKey(domain_,"saturation_gas"), wrm);

  // -- rel perm
  std::vector<AmanziMesh::Entity_kind> locations2(2);
  std::vector<std::string> names2(2);
  std::vector<int> num_dofs2(2,1);
  locations2[0] = AmanziMesh::CELL;
  locations2[1] = AmanziMesh::BOUNDARY_FACE;
  names2[0] = "cell";
  names2[1] = "boundary_face";

  S->RequireField(coef_key_)->SetMesh(mesh_)->SetGhosted()
      ->AddComponents(names2, locations2, num_dofs2);
  wrm_plist.set<double>("permeability rescaling", perm_scale_);
  Teuchos::RCP<FlowRelations::RelPermEvaluator> rel_perm_evaluator =
      Teuchos::rcp(new FlowRelations::RelPermEvaluator(wrm_plist, wrm->get_WRMs()));
  S->SetFieldEvaluator(coef_key_, rel_perm_evaluator);
  wrms_ = wrm->get_WRMs();

  // -- Liquid density and viscosity for the transmissivity.
  S->RequireField(molar_dens_key_)->SetMesh(mesh_)->SetGhosted()
      ->AddComponent("cell", AmanziMesh::CELL, 1);
  S->RequireFieldEvaluator(molar_dens_key_);

  // -- liquid mass density for the gravity fluxes
  S->RequireField(mass_dens_key_)->SetMesh(mesh_)->SetGhosted()
      ->AddComponent("cell", AmanziMesh::CELL, 1);
  S->RequireFieldEvaluator(mass_dens_key_); // simply picks up the molar density one.


}
    

// -------------------------------------------------------------
// Initialize PK
// -------------------------------------------------------------
void Richards::Initialize(const Teuchos::Ptr<State>& S) {

  // Initialize BDF stuff and physical domain stuff.
  PK_PhysicalBDF_Default::Initialize(S);


  // debugggin cruft
#if DEBUG_RES_FLAG
  for (unsigned int i=1; i!=23; ++i) {
    std::stringstream namestream;
    namestream << "flow_residual_" << i;
    S->GetFieldData(namestream.str(),name_)->PutScalar(0.);
    S->GetField(namestream.str(),name_)->set_initialized();

    std::stringstream solnstream;
    solnstream << "flow_solution_" << i;
    S->GetFieldData(solnstream.str(),name_)->PutScalar(0.);
    S->GetField(solnstream.str(),name_)->set_initialized();
  }
#endif

  // check whether this is a dynamic mesh problem
  if (S->HasField("vertex coordinate")) dynamic_mesh_ = true;

  // Set extra fields as initialized -- these don't currently have evaluators,
  // and will be initialized in the call to commit_state()
  S->GetFieldData(uw_coef_key_,name_)->PutScalar(1.0);
  S->GetField(uw_coef_key_,name_)->set_initialized();

  if (!duw_coef_key_.empty()) {
    S->GetFieldData(duw_coef_key_,name_)->PutScalar(1.0);
    S->GetField(duw_coef_key_,name_)->set_initialized();
  }

  // if (vapor_diffusion_){
  //   S->GetFieldData("vapor_diffusion_pressure",name_)->PutScalar(1.0);
  //   S->GetField("vapor_diffusion_pressure",name_)->set_initialized();
  //   S->GetFieldData("vapor_diffusion_temperature",name_)->PutScalar(1.0);
  //   S->GetField("vapor_diffusion_temperature",name_)->set_initialized();
  // }

  S->GetFieldData(flux_key_, name_)->PutScalar(0.0);
  S->GetField(flux_key_, name_)->set_initialized();
  S->GetFieldData(flux_dir_key_, name_)->PutScalar(0.0);
  S->GetField(flux_dir_key_, name_)->set_initialized();
  S->GetFieldData(velocity_key_, name_)->PutScalar(0.0);
  S->GetField(velocity_key_, name_)->set_initialized();

  // absolute perm
  SetAbsolutePermeabilityTensor_(S);

  // operators
  Teuchos::RCP<const Epetra_Vector> gvec = S->GetConstantVectorData("gravity");
  AmanziGeometry::Point g(3);
  g[0] = (*gvec)[0]; g[1] = (*gvec)[1]; g[2] = (*gvec)[2];

  matrix_diff_->SetGravity(g);
  matrix_diff_->SetBCs(bc_, bc_);
  matrix_diff_->SetTensorCoefficient(K_);


  preconditioner_diff_->SetGravity(g);
  preconditioner_diff_->SetBCs(bc_, bc_);
  preconditioner_diff_->SetTensorCoefficient(K_);
  preconditioner_->SymbolicAssembleMatrix();

  face_matrix_diff_->SetGravity(g);
  face_matrix_diff_->SetBCs(bc_, bc_);
  face_matrix_diff_->SetTensorCoefficient(K_);
  face_matrix_diff_->SetScalarCoefficient(Teuchos::null, Teuchos::null);

  // if (vapor_diffusion_){
  //   //vapor diffusion
  //   matrix_vapor_->CreateMFDmassMatrices(Teuchos::null);
  //   // residual vector for vapor diffusion
  //   res_vapor = Teuchos::rcp(new CompositeVector(*S->GetFieldData(key_))); 
  // }

};


// -----------------------------------------------------------------------------
// Update any secondary (dependent) variables given a solution.
//
//   After a timestep is evaluated (or at ICs), there is no way of knowing if
//   secondary variables have been updated to be consistent with the new
//   solution.
// -----------------------------------------------------------------------------
  void Richards::CommitStep(double t_old, double t_new, const Teuchos::RCP<State>& S) {

    double dt = t_new - t_old;
    Teuchos::OSTab tab = vo_->getOSTab();
    if (vo_->os_OK(Teuchos::VERB_EXTREME))
      *vo_->os() << "Commiting state." << std::endl;

    PK_PhysicalBDF_Default::CommitStep(t_old, t_new, S);
  
  // update BCs, rel perm
  UpdateBoundaryConditions_(S.ptr());
  bool update = UpdatePermeabilityData_(S.ptr());

  update |= S->GetFieldEvaluator(key_)->HasFieldChanged(S.ptr(), name_);
  update |= S->GetFieldEvaluator(mass_dens_key_)->HasFieldChanged(S.ptr(), name_);

  if (update) {
    // update the stiffness matrix
    Teuchos::RCP<const CompositeVector> rel_perm =
      S->GetFieldData(uw_coef_key_);
    Teuchos::RCP<const CompositeVector> rho = S->GetFieldData(mass_dens_key_);
    matrix_->Init();
    matrix_diff_->SetDensity(rho);
    matrix_diff_->SetScalarCoefficient(rel_perm, Teuchos::null);
    matrix_diff_->UpdateMatrices(Teuchos::null, Teuchos::null);

    // derive fluxes
    Teuchos::RCP<const CompositeVector> pres = S->GetFieldData(key_);
    Teuchos::RCP<CompositeVector> flux = S->GetFieldData(flux_key_, name_);
    matrix_diff_->UpdateFlux(*pres, *flux);
  }

  // As a diagnostic, calculate the mass balance error
// #if DEBUG_FLAG
//   if (S_next_ != Teuchos::null) {
//     Teuchos::RCP<const CompositeVector> wc1 = S_next_->GetFieldData(conserved_key_);
//     Teuchos::RCP<const CompositeVector> wc0 = S_->GetFieldData(conserved_key_);
//     Teuchos::RCP<const CompositeVector> mass_flux = S->GetFieldData(flux_key_, name_);
//     CompositeVector error(*wc1);

//     for (unsigned int c=0; c!=error.size("cell"); ++c) {
//       error("cell",c) = (*wc1)("cell",c) - (*wc0)("cell",c);

//       AmanziMesh::Entity_ID_List faces;
//       std::vector<int> dirs;
//       mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
//       for (unsigned int n=0; n!=faces.size(); ++n) {
//         error("cell",c) += (*mass_flux)("face",faces[n]) * dirs[n] * dt;
//       }
//     }

//     double einf(0.0);
//     error.NormInf(&einf);

//     // VerboseObject stuff.
//     Teuchos::OSTab tab = vo_->getOSTab();
//     *vo_->os() << "Final Mass Balance Error: " << einf << std::endl;
//   }
// #endif
};


// -----------------------------------------------------------------------------
// Check for controls on saturation
// -----------------------------------------------------------------------------
bool
Richards::ValidStep() {
  Teuchos::OSTab tab = vo_->getOSTab();
  if (vo_->os_OK(Teuchos::VERB_EXTREME))
    *vo_->os() << "Validating time step." << std::endl;

  if (sat_change_limit_ > 0.0) {
    const Epetra_MultiVector& sl_new = *S_next_->GetFieldData(sat_key_)
        ->ViewComponent("cell",false);
    const Epetra_MultiVector& sl_old = *S_->GetFieldData(sat_key_)
        ->ViewComponent("cell",false);
    Epetra_MultiVector dsl(sl_new);
    dsl.Update(-1., sl_old, 1.);
    double change = 0.;
    dsl.NormInf(&change);

    if (change > sat_change_limit_) {
      if (vo_->os_OK(Teuchos::VERB_MEDIUM))
        *vo_->os() << "Invalid time step, max sl change="
                   << change << " > limit=" << sat_change_limit_ << std::endl;
      return false;
    }
  }
  if (sat_ice_change_limit_ > 0.0) {
    const Epetra_MultiVector& si_new = *S_next_->GetFieldData(sat_ice_key_)
        ->ViewComponent("cell",false);
    const Epetra_MultiVector& si_old = *S_->GetFieldData(sat_ice_key_)
        ->ViewComponent("cell",false);
    Epetra_MultiVector dsi(si_new);
    dsi.Update(-1., si_old, 1.);
    double change = 0.;
    dsi.NormInf(&change);

    if (change > sat_ice_change_limit_) {
      if (vo_->os_OK(Teuchos::VERB_MEDIUM))
        *vo_->os() << "Invalid time step, max si change="
                   << change << " > limit=" << sat_ice_change_limit_ << std::endl;
      return false;
    }
  }

  return PK_PhysicalBDF_Default::ValidStep();
}


// -----------------------------------------------------------------------------
// Update any diagnostic variables prior to vis (in this case velocity field).
// -----------------------------------------------------------------------------
void Richards::CalculateDiagnostics(const Teuchos::RCP<State>& S) {
  Teuchos::OSTab tab = vo_->getOSTab();
  if (vo_->os_OK(Teuchos::VERB_EXTREME))
    *vo_->os() << "Calculating diagnostic variables." << std::endl;

  // update the cell velocities
  UpdateBoundaryConditions_(S.ptr());

  Teuchos::RCP<const CompositeVector> rel_perm =
      S->GetFieldData(uw_coef_key_);
  Teuchos::RCP<const CompositeVector> rho =
      S->GetFieldData(mass_dens_key_);
  // update the stiffness matrix
  matrix_diff_->SetDensity(rho);
  matrix_diff_->SetScalarCoefficient(rel_perm, Teuchos::null);
  matrix_diff_->UpdateMatrices(Teuchos::null, Teuchos::null);

  // derive fluxes
  Teuchos::RCP<CompositeVector> flux = S->GetFieldData(flux_key_, name_);
  Teuchos::RCP<const CompositeVector> pres = S->GetFieldData(key_);
  matrix_diff_->UpdateFlux(*pres, *flux);

  UpdateVelocity_(S.ptr());
};


// -----------------------------------------------------------------------------
// Use the physical rel perm (on cells) to update a work vector for rel perm.
//
//   This deals with upwinding, etc.
// -----------------------------------------------------------------------------
bool Richards::UpdatePermeabilityData_(const Teuchos::Ptr<State>& S) {
  Teuchos::OSTab tab = vo_->getOSTab();
  if (vo_->os_OK(Teuchos::VERB_EXTREME))
    *vo_->os() << "  Updating permeability?";

  Teuchos::RCP<CompositeVector> uw_rel_perm = S->GetFieldData(uw_coef_key_, name_);
  Teuchos::RCP<const CompositeVector> rel_perm = S->GetFieldData(coef_key_);
  bool update_perm = S->GetFieldEvaluator(coef_key_)
      ->HasFieldChanged(S, name_);

  // requirements due to the upwinding method
  if (Krel_method_ == Operators::UPWIND_METHOD_TOTAL_FLUX) {
    bool update_dir = S->GetFieldEvaluator(mass_dens_key_)
        ->HasFieldChanged(S, name_);
    update_dir |= S->GetFieldEvaluator(key_)->HasFieldChanged(S, name_);

    if (update_dir) {
      // update the direction of the flux -- note this is NOT the flux
      Teuchos::RCP<const CompositeVector> rho = S->GetFieldData(mass_dens_key_);
      face_matrix_diff_->SetDensity(rho);
      face_matrix_diff_->UpdateMatrices(Teuchos::null, Teuchos::null);

      Teuchos::RCP<CompositeVector> flux_dir =
          S->GetFieldData(flux_dir_key_, name_);
      Teuchos::RCP<const CompositeVector> pres = S->GetFieldData(key_);

      if (!pres->HasComponent("face"))
        face_matrix_diff_->ApplyBCs(true, true);

      face_matrix_diff_->UpdateFlux(*pres, *flux_dir);

      if (clobber_boundary_flux_dir_) {
        Epetra_MultiVector& flux_dir_f = *flux_dir->ViewComponent("face",false);
        for (int f=0; f!=bc_markers_.size(); ++f) {
          if (bc_markers_[f] == Operators::OPERATOR_BC_NEUMANN) {
            AmanziMesh::Entity_ID_List cells;
            mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
            ASSERT(cells.size() == 1);
            int c = cells[0];
            AmanziMesh::Entity_ID_List faces;
            std::vector<int> dirs;
            mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
            int i = std::find(faces.begin(), faces.end(), f) - faces.begin();
            
            flux_dir_f[0][f] = bc_values_[f]*dirs[i];
          }
        }
      }      
    }

    update_perm |= update_dir;
  }

  if (update_perm) {
    // Move rel perm on boundary_faces into uw_rel_perm on faces
    const Epetra_Import& vandelay = mesh_->exterior_face_importer();
    const Epetra_MultiVector& rel_perm_bf =
        *rel_perm->ViewComponent("boundary_face",false);
    {
      Epetra_MultiVector& uw_rel_perm_f = *uw_rel_perm->ViewComponent("face",false);
      uw_rel_perm_f.Export(rel_perm_bf, vandelay, Insert);
    }

    // Upwind, only overwriting boundary faces if the wind says to do so.
    upwinding_->Update(S);

    if (clobber_surf_kr_) {
      Epetra_MultiVector& uw_rel_perm_f = *uw_rel_perm->ViewComponent("face",false);
      uw_rel_perm_f.Export(rel_perm_bf, vandelay, Insert);
    }

    if (uw_rel_perm->HasComponent("face"))
      uw_rel_perm->ScatterMasterToGhosted("face");
  }

  // debugging
  if (vo_->os_OK(Teuchos::VERB_EXTREME)) {
    *vo_->os() << " " << update_perm << std::endl;
  }
  return update_perm;
};


bool Richards::UpdatePermeabilityDerivativeData_(const Teuchos::Ptr<State>& S) {
  Teuchos::OSTab tab = vo_->getOSTab();
  if (vo_->os_OK(Teuchos::VERB_EXTREME))
    *vo_->os() << "  Updating permeability derivatives?";

  bool update_perm = S->GetFieldEvaluator(coef_key_)->HasFieldDerivativeChanged(S, name_, key_);
  Teuchos::RCP<const CompositeVector> drel_perm = S->GetFieldData(dcoef_key_);

  if (update_perm) {
    if (!duw_coef_key_.empty()) {
      Teuchos::RCP<CompositeVector> duw_rel_perm = S->GetFieldData(duw_coef_key_, name_);
      duw_rel_perm->PutScalar(0.);

      // Upwind, only overwriting boundary faces if the wind says to do so.
      upwinding_deriv_->Update(S);

      duw_rel_perm->ScatterMasterToGhosted("face");
    } else {
      drel_perm->ScatterMasterToGhosted("cell");
    }
  }

  // debugging
  if (vo_->os_OK(Teuchos::VERB_EXTREME)) {
    *vo_->os() << " " << update_perm << std::endl;
  }
  return update_perm;
};




// -----------------------------------------------------------------------------
// Evaluate boundary conditions at the current time.
// -----------------------------------------------------------------------------
void Richards::UpdateBoundaryConditions_(const Teuchos::Ptr<State>& S, bool kr) {
  Teuchos::OSTab tab = vo_->getOSTab();
  if (vo_->os_OK(Teuchos::VERB_EXTREME))
    *vo_->os() << "  Updating BCs." << std::endl;

  // initialize all to 0
  for (unsigned int n=0; n!=bc_markers_.size(); ++n) {
    bc_markers_[n] = Operators::OPERATOR_BC_NONE;
    bc_values_[n] = 0.0;
  }

  std::vector<int> bc_counts;
  std::vector<std::string> bc_names;

  // Dirichlet boundary conditions
  Functions::BoundaryFunction::Iterator bc;
  bc_counts.push_back(bc_pressure_->size());
  bc_names.push_back("pressure");
  for (bc=bc_pressure_->begin(); bc!=bc_pressure_->end(); ++bc) {
    int f = bc->first;
#ifdef ENABLE_DBC
    AmanziMesh::Entity_ID_List cells;
    mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
    ASSERT(cells.size() == 1);
#endif
    
    bc_markers_[f] = Operators::OPERATOR_BC_DIRICHLET;
    bc_values_[f] = bc->second;
  }

  bc_counts.push_back(bc_head_->size());
  bc_names.push_back("head");
  for (bc=bc_head_->begin(); bc!=bc_head_->end(); ++bc) {
    int f = bc->first;
#ifdef ENABLE_DBC
    AmanziMesh::Entity_ID_List cells;
    mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
    ASSERT(cells.size() == 1);
#endif
    
    bc_markers_[f] = Operators::OPERATOR_BC_DIRICHLET;
    bc_values_[f] = bc->second;
  }


  const Epetra_MultiVector& rel_perm = 
    *S->GetFieldData(uw_coef_key_)->ViewComponent("face",false);

  bc_counts.push_back(bc_flux_->size());
  bc_names.push_back("flux");
  if (!infiltrate_only_if_unfrozen_) {
    // Standard Neuman boundary conditions
    for (bc=bc_flux_->begin(); bc!=bc_flux_->end(); ++bc) {
      int f = bc->first;
#ifdef ENABLE_DBC
    AmanziMesh::Entity_ID_List cells;
    mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
    ASSERT(cells.size() == 1);
#endif
      bc_markers_[f] = Operators::OPERATOR_BC_NEUMANN;
      bc_values_[f] = bc->second;
      if (!kr && rel_perm[0][f] > 0.) bc_values_[f] /= rel_perm[0][f];
    }
  } else {
    // Neumann boundary conditions that turn off if temp < freezing
    const Epetra_MultiVector& temp = *S->GetFieldData(getKey(domain_,"temperature"))->ViewComponent("face");
    for (bc=bc_flux_->begin(); bc!=bc_flux_->end(); ++bc) {
      int f = bc->first;
#ifdef ENABLE_DBC
    AmanziMesh::Entity_ID_List cells;
    mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
    ASSERT(cells.size() == 1);
#endif
      bc_markers_[f] = Operators::OPERATOR_BC_NEUMANN;
      if (temp[0][f] > 273.15) {
        bc_values_[f] = bc->second;
        if (!kr && rel_perm[0][f] > 0.) {
          bc_values_[f] /= rel_perm[0][f];
        }
      } else {
        bc_values_[f] = 0.;
      }
    }
  }

  // seepage face -- pressure <= p_atm, outward mass flux >= 0
  S->GetFieldData(flux_key_)->ScatterMasterToGhosted("face");
  const Epetra_MultiVector& flux = *S->GetFieldData(flux_key_)->ViewComponent("face", true);
  const double& p_atm = *S->GetScalarData("atmospheric_pressure");
  Teuchos::RCP<const CompositeVector> u = S->GetFieldData(key_);
  double seepage_tol = p_atm * 1e-14;

  bc_counts.push_back(bc_seepage_->size());
  bc_names.push_back("standard seepage");
  for (bc=bc_seepage_->begin(); bc!=bc_seepage_->end(); ++bc) {
    int f = bc->first;
#ifdef ENABLE_DBC
    AmanziMesh::Entity_ID_List cells;
    mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
    ASSERT(cells.size() == 1);
#endif
    double boundary_pressure = BoundaryValue(u, f);
    double boundary_flux = flux[0][f]*BoundaryDirection(f);
    if (boundary_pressure < bc->second - seepage_tol) {
      bc_markers_[f] = Operators::OPERATOR_BC_NEUMANN;
      bc_values_[f] = 0.;
    } else if (boundary_flux > 0.) {
      bc_markers_[f] = Operators::OPERATOR_BC_DIRICHLET;
      bc_values_[f] = bc->second;
    } else {
      bc_markers_[f] = Operators::OPERATOR_BC_NEUMANN;
      bc_values_[f] = 0.;
    }
  }

  // seepage face -- pressure <= p_atm, outward mass flux is specified
  bc_counts.push_back(bc_seepage_infilt_->size());
  bc_names.push_back("seepage with infiltration");
  for (bc=bc_seepage_infilt_->begin(); bc!=bc_seepage_infilt_->end(); ++bc) {
    int f = bc->first;
#ifdef ENABLE_DBC
    AmanziMesh::Entity_ID_List cells;
    mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
    ASSERT(cells.size() == 1);
#endif
    double flux_seepage_tol = std::abs(bc->second) * .001;
    
    double boundary_pressure = BoundaryValue(u, f);
    double boundary_flux = flux[0][f]*BoundaryDirection(f);

    //    std::cout << "BFlux = " << boundary_flux << " with constraint = " << bc->second - flux_seepage_tol << std::endl;
    if (boundary_flux < bc->second - flux_seepage_tol &&
        boundary_pressure > p_atm + seepage_tol) {
      // both constraints are violated, either option should push things in the right direction
      bc_markers_[f] = Operators::OPERATOR_BC_DIRICHLET;
      bc_values_[f] = p_atm;
      //      std::cout << "BC PRESSURE ON SEEPAGE = " << boundary_pressure << " with flux " << flux[0][f]*BoundaryDirection(f) << " resulted in DIRICHLET pressure " << p_atm << std::endl;

    } else if (boundary_flux >= bc->second - flux_seepage_tol &&
        boundary_pressure > p_atm - seepage_tol) {
      // max pressure condition violated
      bc_markers_[f] = Operators::OPERATOR_BC_DIRICHLET;
      bc_values_[f] = p_atm;
      //      std::cout << "BC PRESSURE ON SEEPAGE = " << boundary_pressure << " with flux " << flux[0][f]*BoundaryDirection(f) << " resulted in DIRICHLET pressure " << p_atm << std::endl;

    } else if (boundary_flux < bc->second - flux_seepage_tol &&
        boundary_pressure <= p_atm + seepage_tol) {
      // max infiltration violated
      bc_markers_[f] = Operators::OPERATOR_BC_NEUMANN;
      bc_values_[f] = bc->second;
      //      std::cout << "BC PRESSURE ON SEEPAGE = " << boundary_pressure << " with flux " << flux[0][f]*BoundaryDirection(f) << " resulted in NEUMANN flux " << bc->second << std::endl;

    } else if (boundary_flux >= bc->second - flux_seepage_tol &&
        boundary_pressure <= p_atm - seepage_tol) {
      // both conditions are valid
      bc_markers_[f] = Operators::OPERATOR_BC_NEUMANN;
      bc_values_[f] = bc->second;
      //      std::cout << "BC PRESSURE ON SEEPAGE = " << boundary_pressure << " with flux " << flux[0][f]*BoundaryDirection(f) << " resulted in NEUMANN flux " << bc->second << std::endl;

    } else {
      ASSERT(0);
    }

  }

  // surface coupling
  bc_counts.push_back(0);
  bc_names.push_back("surface coupling (head)");
  if (coupled_to_surface_via_head_) {
    // Face is Dirichlet with value of surface head
    Teuchos::RCP<const AmanziMesh::Mesh> surface = S->GetMesh("surface");
    const Epetra_MultiVector& head = *S->GetFieldData("surface_pressure")
        ->ViewComponent("cell",false);

    unsigned int ncells_surface = head.MyLength();
    bc_counts[bc_counts.size()-1] = ncells_surface;
    for (unsigned int c=0; c!=ncells_surface; ++c) {
      // -- get the surface cell's equivalent subsurface face
      AmanziMesh::Entity_ID f =
        surface->entity_get_parent(AmanziMesh::CELL, c);
#ifdef ENABLE_DBC
      AmanziMesh::Entity_ID_List cells;
      mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
      ASSERT(cells.size() == 1);
#endif

      // -- set that value to dirichlet
      bc_markers_[f] = Operators::OPERATOR_BC_DIRICHLET;
      bc_values_[f] = head[0][c];
    }
  }

  // surface coupling
  bc_counts.push_back(0);
  bc_names.push_back("surface coupling (flux)");
  if (coupled_to_surface_via_flux_) {
    // Face is Neumann with value of surface residual
    Teuchos::RCP<const AmanziMesh::Mesh> surface = S->GetMesh("surface");
    const Epetra_MultiVector& flux = *S->GetFieldData(ss_flux_key_)
        ->ViewComponent("cell",false);

    unsigned int ncells_surface = flux.MyLength();
    bc_counts[bc_counts.size()-1] = ncells_surface;
    for (unsigned int c=0; c!=ncells_surface; ++c) {
      // -- get the surface cell's equivalent subsurface face
      AmanziMesh::Entity_ID f =
        surface->entity_get_parent(AmanziMesh::CELL, c);
#ifdef ENABLE_DBC
      AmanziMesh::Entity_ID_List cells;
      mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
      ASSERT(cells.size() == 1);
#endif

      // -- set that value to Neumann
      bc_markers_[f] = Operators::OPERATOR_BC_NEUMANN;
      bc_values_[f] = flux[0][c] / mesh_->face_area(f);
      if (!kr && rel_perm[0][f] > 0.) bc_values_[f] /= rel_perm[0][f];

      if ((surface->cell_map(false).GID(c) == 0) && vo_->os_OK(Teuchos::VERB_HIGH)) {
        *vo_->os() << "  bc for coupled surface: val=" << bc_values_[f] << std::endl;
      }
      
      // NOTE: flux[0][c] is in units of mols / s, where as Neumann BCs are in
      //       units of mols / s / A.  The right A must be chosen, as it is
      //       the subsurface mesh's face area, not the surface mesh's cell
      //       area.
    }
  }

  // mark all remaining boundary conditions as zero flux conditions
  AmanziMesh::Entity_ID_List cells;
  int n_default = 0;
  int nfaces_owned = mesh_->num_entities(AmanziMesh::FACE, AmanziMesh::OWNED);
  for (int f = 0; f < nfaces_owned; f++) {
    if (bc_markers_[f] == Operators::OPERATOR_BC_NONE) {
      mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
      int ncells = cells.size();

      if (ncells == 1) {
        n_default++;
        bc_markers_[f] = Operators::OPERATOR_BC_NEUMANN;
        bc_values_[f] = 0.0;

        // BEGIN DEBUG CRUFT        
        /*
        AmanziGeometry::Point normal = mesh_->face_normal(f);
        normal /= AmanziGeometry::norm(normal);
        if (std::abs(normal[2]) > 0.0001) {
          std::cout << "bottom face: " << f << std::endl;
        }
        */

        // ENDDEBUG CRUFT        
      }
    }
  }
  bc_names.push_back("default (zero flux)");
  bc_counts.push_back(n_default);

  // report on counts
  if (vo_->os_OK(Teuchos::VERB_EXTREME)) {
    std::vector<int> bc_counts_global(bc_counts.size(), 0);
    mesh_->get_comm()->SumAll(&bc_counts[0], &bc_counts_global[0], bc_counts.size());

    *vo_->os() << "  BCs applied:" << std::endl;

    for (int i=0; i!=bc_counts_global.size(); ++i) {
      *vo_->os() << "    " << bc_names[i] << ": " << bc_counts_global[i] << std::endl;
    }
  }
};


bool Richards::ModifyPredictor(double h, Teuchos::RCP<const TreeVector> u0,
        Teuchos::RCP<TreeVector> u) {
  Teuchos::OSTab tab = vo_->getOSTab();
  if (vo_->os_OK(Teuchos::VERB_EXTREME))
    *vo_->os() << "Modifying predictor:" << std::endl;

  // update boundary conditions
  bc_pressure_->Compute(S_next_->time());
  bc_head_->Compute(S_next_->time());
  bc_flux_->Compute(S_next_->time());
  UpdateBoundaryConditions_(S_next_.ptr(), false); // without rel perm
  
  // push Dirichlet data into predictor
  if (u->Data()->HasComponent("boundary_cell")) {
    ApplyBoundaryConditions_(u->Data().ptr());
  }
  
  bool changed(false);
  if (modify_predictor_bc_flux_ ||
      (modify_predictor_first_bc_flux_ && 
       ((S_next_->cycle() == 0) || (S_next_->cycle() == 1)))) {
    changed |= ModifyPredictorFluxBCs_(h,u);
  }

  if (modify_predictor_wc_) {
    changed |= ModifyPredictorWC_(h,u);
  }

  if (modify_predictor_with_consistent_faces_) {
    changed |= ModifyPredictorConsistentFaces_(h,u);
  }
  return changed;
}

bool Richards::ModifyPredictorFluxBCs_(double h, Teuchos::RCP<TreeVector> u) {
  if (!u->Data()->HasComponent("face")) return false;

  Teuchos::OSTab tab = vo_->getOSTab();
  if (vo_->os_OK(Teuchos::VERB_EXTREME))
    *vo_->os() << "  modifications to deal with nonlinearity at flux BCs" << std::endl;

  if (flux_predictor_ == Teuchos::null) {
    flux_predictor_ = Teuchos::rcp(new PredictorDelegateBCFlux(S_next_, mesh_, matrix_diff_,
            wrms_, &bc_markers_, &bc_values_));
  }

  UpdatePermeabilityData_(S_next_.ptr());
  Teuchos::RCP<const CompositeVector> rel_perm =
    S_next_->GetFieldData(uw_coef_key_);

  matrix_->Init();
  matrix_diff_->SetScalarCoefficient(rel_perm, Teuchos::null);
  Teuchos::RCP<const CompositeVector> rho = S_next_->GetFieldData(mass_dens_key_);
  matrix_diff_->SetDensity(rho);
  matrix_diff_->UpdateMatrices(Teuchos::null, Teuchos::null);
  matrix_diff_->ApplyBCs(true, true);

  flux_predictor_->ModifyPredictor(h, u);
  ChangedSolution(); // mark the solution as changed, as modifying with
                      // consistent faces will then get the updated boundary
                      // conditions
  return true;
}

bool Richards::ModifyPredictorConsistentFaces_(double h, Teuchos::RCP<TreeVector> u) {
  Teuchos::OSTab tab = vo_->getOSTab();
  if (vo_->os_OK(Teuchos::VERB_EXTREME))
    *vo_->os() << "  modifications for consistent face pressures." << std::endl;
  
  CalculateConsistentFaces(u->Data().ptr());

  return true;
}

bool Richards::ModifyPredictorWC_(double h, Teuchos::RCP<TreeVector> u) {
  ASSERT(0);
  return false;
}


// void Richards::CalculateConsistentFacesForInfiltration_(
//     const Teuchos::Ptr<CompositeVector>& u) {
//   if (vo_->os_OK(Teuchos::VERB_EXTREME))
//     *vo_->os() << "  modifications to deal with nonlinearity at flux BCs" << std::endl;

//   if (flux_predictor_ == Teuchos::null) {
//     flux_predictor_ = Teuchos::rcp(new PredictorDelegateBCFlux(S_next_, mesh_, matrix_,
//             wrms_, &bc_markers_, &bc_values_));
//   }

//   // update boundary conditions
//   bc_pressure_->Compute(S_next_->time());
//   bc_flux_->Compute(S_next_->time());
//   UpdateBoundaryConditions_(S_next_.ptr());

//   bool update = UpdatePermeabilityData_(S_next_.ptr());
//   Teuchos::RCP<const CompositeVector> rel_perm =
//       S_next_->GetFieldData(uw_coef_key_);
//   matrix_->CreateMFDstiffnessMatrices(rel_perm.ptr());
//   matrix_->CreateMFDrhsVectors();
//   Teuchos::RCP<const CompositeVector> rho = S_next_->GetFieldData(mass_dens_key_);
//   Teuchos::RCP<const Epetra_Vector> gvec = S_next_->GetConstantVectorData("gravity");
//   AddGravityFluxes_(gvec.ptr(), rel_perm.ptr(), rho.ptr(), matrix_.ptr());
//   matrix_->ApplyBoundaryConditions(bc_markers_, bc_values_);

//   flux_predictor_->ModifyPredictor(u);
// }

void Richards::CalculateConsistentFaces(const Teuchos::Ptr<CompositeVector>& u) {
  if (!u->HasComponent("face")) return; // no need

  // VerboseObject stuff.
  Teuchos::OSTab tab = vo_->getOSTab();
  if (vo_->os_OK(Teuchos::VERB_EXTREME))
    *vo_->os() << "  Modifying predictor for consistent faces" << std::endl;

  // average cells to faces to give a reasonable initial guess
  u->ScatterMasterToGhosted("cell");
  const Epetra_MultiVector& u_c = *u->ViewComponent("cell",true);
  Epetra_MultiVector& u_f = *u->ViewComponent("face",false);

  int f_owned = u_f.MyLength();
  for (int f=0; f!=f_owned; ++f) {
    AmanziMesh::Entity_ID_List cells;
    mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
    int ncells = cells.size();

    double face_value = 0.0;
    for (int n=0; n!=ncells; ++n) {
      face_value += u_c[0][cells[n]];
    }
    u_f[0][f] = face_value / ncells;
  }
  ChangedSolution();
  
  // Using the old BCs, so should use the old rel perm?
  // update the rel perm according to the scheme of choice
  //  UpdatePermeabilityData_(S_next_.ptr());

  Teuchos::RCP<const CompositeVector> rel_perm = 
    S_next_->GetFieldData(uw_coef_key_);
  S_next_->GetFieldEvaluator(mass_dens_key_)
      ->HasFieldChanged(S_next_.ptr(), name_);
  Teuchos::RCP<const CompositeVector> rho =
      S_next_->GetFieldData(mass_dens_key_);

  // Update the preconditioner with darcy and gravity fluxes
  matrix_->Init();
  matrix_diff_->SetDensity(rho);
  matrix_diff_->SetScalarCoefficient(rel_perm, Teuchos::null);
  matrix_diff_->UpdateMatrices(Teuchos::null, Teuchos::null);
  matrix_diff_->ApplyBCs(true, true);

  // derive the consistent faces, involves a solve
  db_->WriteVector(" p_cf guess:", u.ptr(), true);
  matrix_diff_->UpdateConsistentFaces(*u);
  db_->WriteVector(" p_cf soln:", u.ptr(), true);
}

// -----------------------------------------------------------------------------
// Check admissibility of the solution guess.
// -----------------------------------------------------------------------------
bool Richards::IsAdmissible(Teuchos::RCP<const TreeVector> up) {
  Teuchos::OSTab tab = vo_->getOSTab();
  if (vo_->os_OK(Teuchos::VERB_EXTREME))
    *vo_->os() << "  Checking admissibility..." << std::endl;

  // For some reason, wandering PKs break most frequently with an unreasonable
  // pressure.  This simply tries to catch that before it happens.
  Teuchos::RCP<const CompositeVector> pres = up->Data();
  double minT, maxT;

  const Epetra_MultiVector& pres_c = *pres->ViewComponent("cell",false);
  double minT_c(1.e15), maxT_c(-1.e15);
  int min_c(-1), max_c(-1);
  for (int c=0; c!=pres_c.MyLength(); ++c) {
    if (pres_c[0][c] < minT_c) {
      minT_c = pres_c[0][c];
      min_c = c;
    }
    if (pres_c[0][c] > maxT_c) {
      maxT_c = pres_c[0][c];
      max_c = c;
    }
  }

  double minT_f(1.e15), maxT_f(-1.e15);
  int min_f(-1), max_f(-1);
  if (pres->HasComponent("face")) {
    const Epetra_MultiVector& pres_f = *pres->ViewComponent("face",false);
    for (int f=0; f!=pres_f.MyLength(); ++f) {
      if (pres_f[0][f] < minT_f) {
        minT_f = pres_f[0][f];
        min_f = f;
      } 
      if (pres_f[0][f] > maxT_f) {
        maxT_f = pres_f[0][f];
        max_f = f;
      }
    }
    minT = std::min(minT_c, minT_f);
    maxT = std::max(maxT_c, maxT_f);

  } else {
    minT = minT_c;
    maxT = maxT_c;
  }

  double minT_l = minT;
  double maxT_l = maxT;
  mesh_->get_comm()->MaxAll(&maxT_l, &maxT, 1);
  mesh_->get_comm()->MinAll(&minT_l, &minT, 1);
  
  if (vo_->os_OK(Teuchos::VERB_HIGH)) {
    *vo_->os() << "    Admissible p? (min/max): " << minT << ",  " << maxT << std::endl;
  }
  
  if (minT < -1.e9 || maxT > 1.e8) {
    if (vo_->os_OK(Teuchos::VERB_MEDIUM)) {
      *vo_->os() << " is not admissible, as it is not within bounds of constitutive models:" << std::endl;
      ENorm_t global_minT_c, local_minT_c;
      ENorm_t global_maxT_c, local_maxT_c;

      local_minT_c.value = minT_c;
      local_minT_c.gid = pres_c.Map().GID(min_c);
      local_maxT_c.value = maxT_c;
      local_maxT_c.gid = pres_c.Map().GID(max_c);

      MPI_Allreduce(&local_minT_c, &global_minT_c, 1, MPI_DOUBLE_INT, MPI_MINLOC, MPI_COMM_WORLD);
      MPI_Allreduce(&local_maxT_c, &global_maxT_c, 1, MPI_DOUBLE_INT, MPI_MAXLOC, MPI_COMM_WORLD);
      *vo_->os() << "   cells (min/max): [" << global_minT_c.gid << "] " << global_minT_c.value
                 << ", [" << global_maxT_c.gid << "] " << global_maxT_c.value << std::endl;

      if (pres->HasComponent("face")) {
        const Epetra_MultiVector& pres_f = *pres->ViewComponent("face",false);
        ENorm_t global_minT_f, local_minT_f;
        ENorm_t global_maxT_f, local_maxT_f;

        local_minT_f.value = minT_f;
        local_minT_f.gid = pres_f.Map().GID(min_f);
        local_maxT_f.value = maxT_f;
        local_maxT_f.gid = pres_f.Map().GID(max_f);
        
        MPI_Allreduce(&local_minT_f, &global_minT_f, 1, MPI_DOUBLE_INT, MPI_MINLOC, MPI_COMM_WORLD);
        MPI_Allreduce(&local_maxT_f, &global_maxT_f, 1, MPI_DOUBLE_INT, MPI_MAXLOC, MPI_COMM_WORLD);
        *vo_->os() << "   cells (min/max): [" << global_minT_f.gid << "] " << global_minT_f.value
                   << ", [" << global_maxT_f.gid << "] " << global_maxT_f.value << std::endl;
      }
    }
    return false;
  }
  return true;
}


AmanziSolvers::FnBaseDefs::ModifyCorrectionResult
Richards::ModifyCorrection(double h, Teuchos::RCP<const TreeVector> res,
                 Teuchos::RCP<const TreeVector> u,
                 Teuchos::RCP<TreeVector> du) {
  Teuchos::OSTab tab = vo_->getOSTab();

  // if the primary variable has boundary face, this is for upwinding rel
  // perms and is never actually used.  Make sure it does not go to undefined
  // pressures.
  if (du->Data()->HasComponent("boundary_face")) {
    du->Data()->ViewComponent("boundary_face")->PutScalar(0.);
  }

  // debugging -- remove me! --etc
  for (CompositeVector::name_iterator comp=du->Data()->begin();
       comp!=du->Data()->end(); ++comp) {
    Epetra_MultiVector& du_c = *du->Data()->ViewComponent(*comp,false);
    double max, l2;
    du_c.NormInf(&max);
    du_c.Norm2(&l2);
    if (vo_->os_OK(Teuchos::VERB_HIGH)) {
      *vo_->os() << "Linf, L2 pressure correction (" << *comp << ") = " << max << ", " << l2 << std::endl;
    }
  }
  
  // limit by capping corrections when they cross atmospheric pressure
  // (where pressure derivatives are discontinuous)
  int my_limited = 0;
  int n_limited_spurt = 0;
  if (patm_limit_ > 0.) {
    double patm = *S_next_->GetScalarData("atmospheric_pressure");
    for (CompositeVector::name_iterator comp=du->Data()->begin();
         comp!=du->Data()->end(); ++comp) {
      Epetra_MultiVector& du_c = *du->Data()->ViewComponent(*comp,false);
      const Epetra_MultiVector& u_c = *u->Data()->ViewComponent(*comp,false);

      for (int c=0; c!=du_c.MyLength(); ++c) {
        if ((u_c[0][c] < patm) &&
            (u_c[0][c] - du_c[0][c] > patm + patm_limit_)) {
          du_c[0][c] = u_c[0][c] - (patm + patm_limit_);          
          my_limited++;
        } else if ((u_c[0][c] > patm) &&
                   (u_c[0][c] - du_c[0][c] < patm - patm_limit_)) {
          du_c[0][c] = u_c[0][c] - (patm - patm_limit_);          
          my_limited++;
        }
      }
    }
    mesh_->get_comm()->MaxAll(&my_limited, &n_limited_spurt, 1);
  }

  if (n_limited_spurt > 0) {
    if (vo_->os_OK(Teuchos::VERB_HIGH)) {
      *vo_->os() << "  limiting the spurt." << std::endl;
    }
  }

  // debugging -- remove me! --etc
  for (CompositeVector::name_iterator comp=du->Data()->begin();
       comp!=du->Data()->end(); ++comp) {
    Epetra_MultiVector& du_c = *du->Data()->ViewComponent(*comp,false);
    double max, l2;
    du_c.NormInf(&max);
    du_c.Norm2(&l2);
    if (vo_->os_OK(Teuchos::VERB_HIGH)) {
      *vo_->os() << "Linf, L2 pressure correction (" << *comp << ") = " << max << ", " << l2 << std::endl;
    }
  }
  
  // Limit based on a max pressure change
  my_limited = 0;
  int n_limited_change = 0;
  if (p_limit_ >= 0.) {
    for (CompositeVector::name_iterator comp=du->Data()->begin();
         comp!=du->Data()->end(); ++comp) {
      Epetra_MultiVector& du_c = *du->Data()->ViewComponent(*comp,false);

      double max;
      du_c.NormInf(&max);
      if (vo_->os_OK(Teuchos::VERB_HIGH)) {
        *vo_->os() << "Max pressure correction (" << *comp << ") = " << max << std::endl;
      }
      
      for (int c=0; c!=du_c.MyLength(); ++c) {
        if (std::abs(du_c[0][c]) > p_limit_) {
          du_c[0][c] = ((du_c[0][c] > 0) - (du_c[0][c] < 0)) * p_limit_;
          my_limited++;
        }
      }
    }
    
    mesh_->get_comm()->MaxAll(&my_limited, &n_limited_change, 1);
  }

  // debugging -- remove me! --etc
  for (CompositeVector::name_iterator comp=du->Data()->begin();
       comp!=du->Data()->end(); ++comp) {
    Epetra_MultiVector& du_c = *du->Data()->ViewComponent(*comp,false);
    double max, l2;
    du_c.NormInf(&max);
    du_c.Norm2(&l2);
    if (vo_->os_OK(Teuchos::VERB_HIGH)) {
      *vo_->os() << "Linf, L2 pressure correction (" << *comp << ") = " << max << ", " << l2 << std::endl;
    }
  }
  
  if (n_limited_change > 0) {
    if (vo_->os_OK(Teuchos::VERB_HIGH)) {
      *vo_->os() << "  limited by pressure." << std::endl;
    }
  }

  if (n_limited_spurt > 0) {
    return AmanziSolvers::FnBaseDefs::CORRECTION_MODIFIED_LAG_BACKTRACKING;
  } else if (n_limited_change > 0) {
    return AmanziSolvers::FnBaseDefs::CORRECTION_MODIFIED;
  }
  return AmanziSolvers::FnBaseDefs::CORRECTION_NOT_MODIFIED;
}


} // namespace
} // namespace
