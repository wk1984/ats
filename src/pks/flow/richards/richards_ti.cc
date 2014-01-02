/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/*
A base two-phase, thermal Richard's equation with water vapor.

Authors: Ethan Coon (ATS version) (ecoon@lanl.gov)
*/

#include "Epetra_FECrsMatrix.h"
#include "EpetraExt_RowMatrixOut.h"
#include "boost/math/special_functions/fpclassify.hpp"
#include "Mesh_MSTK.hh"

#include "richards.hh"

namespace Amanzi {
namespace Flow {

#define DEBUG_FLAG 1
#define DEBUG_RES_FLAG 0

// Richards is a BDFFnBase
// -----------------------------------------------------------------------------
// computes the non-linear functional g = g(t,u,udot)
// -----------------------------------------------------------------------------
void Richards::fun(double t_old,
                   double t_new,
                   Teuchos::RCP<TreeVector> u_old,
                   Teuchos::RCP<TreeVector> u_new,
                   Teuchos::RCP<TreeVector> g) {
  // VerboseObject stuff.
  Teuchos::OSTab tab = vo_->getOSTab();
  niter_++;

  double h = t_new - t_old;
  ASSERT(std::abs(S_inter_->time() - t_old) < 1.e-4*h);
  ASSERT(std::abs(S_next_->time() - t_new) < 1.e-4*h);

  // pointer-copy temperature into state and update any auxilary data
  solution_to_state(u_new, S_next_);
  Teuchos::RCP<CompositeVector> u = u_new->Data();

  if (dynamic_mesh_) matrix_->CreateMFDmassMatrices(K_.ptr());

#if DEBUG_FLAG
  if (vo_->os_OK(Teuchos::VERB_HIGH))
    *vo_->os() << "----------------------------------------------------------------" << std::endl
               << "Residual calculation: t0 = " << t_old
               << " t1 = " << t_new << " h = " << h << std::endl;

  // dump u_old, u_new
  db_->WriteCellInfo(true);
  std::vector<std::string> vnames;
  vnames.push_back("p_old"); vnames.push_back("p_new");
  std::vector< Teuchos::Ptr<const CompositeVector> > vecs;
  vecs.push_back(S_inter_->GetFieldData(key_).ptr()); vecs.push_back(u.ptr());
  db_->WriteVectors(vnames, vecs, true);
#endif

  // update boundary conditions
  bc_pressure_->Compute(t_new);
  bc_flux_->Compute(t_new);
  UpdateBoundaryConditions_();

  // zero out residual
  Teuchos::RCP<CompositeVector> res = g->Data();
  res->PutScalar(0.0);

  // diffusion term, treated implicitly
  ApplyDiffusion_(S_next_.ptr(), res.ptr());

#if DEBUG_FLAG
  // dump s_old, s_new
  vnames[0] = "sl_old"; vnames[1] = "sl_new";
  vecs[0] = S_inter_->GetFieldData("saturation_liquid").ptr();
  vecs[1] = S_next_->GetFieldData("saturation_liquid").ptr();

  if (S_next_->HasField("saturation_ice")) {
    vnames.push_back("si_old");
    vnames.push_back("si_new");
    vecs.push_back(S_inter_->GetFieldData("saturation_ice").ptr());
    vecs.push_back(S_next_->GetFieldData("saturation_ice").ptr());
  }

  vnames.push_back("k_rel");
  vecs.push_back(S_next_->GetFieldData("relative_permeability").ptr());
  vnames.push_back("uw_k_rel");
  vecs.push_back(S_next_->GetFieldData("numerical_rel_perm").ptr());
  db_->WriteVectors(vnames,vecs,true);

  db_->WriteVector("res (diff)", res.ptr(), true);
#endif

  // accumulation term
  AddAccumulation_(res.ptr());

#if DEBUG_FLAG
  db_->WriteVector("res (acc)", res.ptr(), true);
#endif

#if DEBUG_RES_FLAG
  if (niter_ < 23) {
    std::stringstream namestream;
    namestream << "flow_residual_" << niter_;
    *S_next_->GetFieldData(namestream.str(),name_) = *res;

    std::stringstream solnstream;
    solnstream << "flow_solution_" << niter_;
    *S_next_->GetFieldData(solnstream.str(),name_) = *u;
  }
#endif
};

// -----------------------------------------------------------------------------
// Apply the preconditioner to u and return the result in Pu.
// -----------------------------------------------------------------------------
void Richards::precon(Teuchos::RCP<const TreeVector> u, Teuchos::RCP<TreeVector> Pu) {
  Teuchos::OSTab tab = vo_->getOSTab();
  if (vo_->os_OK(Teuchos::VERB_HIGH))
    *vo_->os() << "Precon application:" << std::endl;

#if DEBUG_FLAG
  db_->WriteVector("p_res", u->Data().ptr(), true);
#endif

  // Apply the preconditioner
  mfd_preconditioner_->ApplyInverse(*u->Data(), *Pu->Data());

#if DEBUG_FLAG
  db_->WriteVector("PC*p_res", Pu->Data().ptr(), true);
#endif

  if (precon_wc_) {
    PreconWC_(u, Pu);
#if DEBUG_FLAG
    db_->WriteVector("PC_WC*p_res", Pu->Data().ptr(), true);
#endif
  }
};


// -----------------------------------------------------------------------------
// Update the preconditioner at time t and u = up
// -----------------------------------------------------------------------------
void Richards::update_precon(double t, Teuchos::RCP<const TreeVector> up, double h) {
  // VerboseObject stuff.
  Teuchos::OSTab tab = vo_->getOSTab();
  if (vo_->os_OK(Teuchos::VERB_HIGH))
    *vo_->os() << "Precon update at t = " << t << std::endl;

  // Recreate mass matrices
  if (dynamic_mesh_) {
    matrix_->CreateMFDmassMatrices(K_.ptr());
    mfd_preconditioner_->CreateMFDmassMatrices(K_.ptr());
  }

  // update state with the solution up.
  ASSERT(std::abs(S_next_->time() - t) <= 1.e-4*t);
  PKDefaultBase::solution_to_state(up, S_next_);

  // update the rel perm according to the scheme of choice
  UpdatePermeabilityData_(S_next_.ptr());

  // update boundary conditions
  bc_pressure_->Compute(S_next_->time());
  bc_flux_->Compute(S_next_->time());
  UpdateBoundaryConditions_();

  Teuchos::RCP<const CompositeVector> rel_perm =
      S_next_->GetFieldData("numerical_rel_perm");
  Teuchos::RCP<const CompositeVector> rho =
      S_next_->GetFieldData("mass_density_liquid");
  Teuchos::RCP<const Epetra_Vector> gvec =
      S_next_->GetConstantVectorData("gravity");

  // Update the preconditioner with darcy and gravity fluxes
  mfd_preconditioner_->CreateMFDstiffnessMatrices(rel_perm.ptr());
  mfd_preconditioner_->CreateMFDrhsVectors();
  AddGravityFluxes_(gvec.ptr(), rel_perm.ptr(), rho.ptr(), mfd_preconditioner_.ptr());

  // Update the preconditioner with accumulation terms.
  // -- update the accumulation derivatives
  S_next_->GetFieldEvaluator("water_content")
      ->HasFieldDerivativeChanged(S_next_.ptr(), name_, key_);

  // -- get the accumulation deriv
  const Epetra_MultiVector& dwc_dp =
      *S_next_->GetFieldData("dwater_content_d"+key_)->ViewComponent("cell",false);
  const Epetra_MultiVector& pres =
      *S_next_->GetFieldData(key_)->ViewComponent("cell",false);

#if DEBUG_FLAG
  db_->WriteVector("    dwc_dp", S_next_->GetFieldData("dwater_content_d"+key_).ptr());
#endif

  // -- update the cell-cell block
  std::vector<double>& Acc_cells = mfd_preconditioner_->Acc_cells();
  std::vector<double>& Fc_cells = mfd_preconditioner_->Fc_cells();

  unsigned int ncells = dwc_dp.MyLength();
  for (unsigned int c=0; c!=ncells; ++c) {
    Acc_cells[c] += dwc_dp[0][c] / h;
    Fc_cells[c] += pres[0][c] * dwc_dp[0][c] / h;
  }

  // Assemble and precompute the Schur complement for inversion.
  mfd_preconditioner_->ApplyBoundaryConditions(bc_markers_, bc_values_);

  if (assemble_preconditioner_) {
    if (vo_->os_OK(Teuchos::VERB_EXTREME))
      *vo_->os() << "  assembling..." << std::endl;
    mfd_preconditioner_->AssembleGlobalMatrices();
    mfd_preconditioner_->ComputeSchurComplement(bc_markers_, bc_values_);

    // dump the schur complement
    // Teuchos::RCP<Epetra_FECrsMatrix> sc = mfd_preconditioner_->Schur();
    // std::stringstream filename_s;
    // filename_s << "schur_" << S_next_->cycle() << ".txt";
    // EpetraExt::RowMatrixToMatlabFile(filename_s.str().c_str(), *sc);
    // *vo_->os() << "updated precon " << S_next_->cycle() << std::endl;

    mfd_preconditioner_->UpdatePreconditioner();
  }
};

double Richards::enorm(Teuchos::RCP<const TreeVector> u,
                       Teuchos::RCP<const TreeVector> du) {
  Teuchos::OSTab tab = vo_->getOSTab();

  // Calculate water content at the solution.
  S_next_->GetFieldEvaluator("water_content")->HasFieldChanged(S_next_.ptr(), name_);
  const Epetra_MultiVector& wc = *S_next_->GetFieldData("water_content")
      ->ViewComponent("cell",false);

  // Collect a typical flux value.
  const Epetra_MultiVector& flux = *S_next_->GetFieldData("darcy_flux")
      ->ViewComponent("face",false);
  double flux_max(0.);
  flux.NormInf(&flux_max);

  // Collect additional data.
  Teuchos::RCP<const CompositeVector> res = du->Data();
  const Epetra_MultiVector& res_c = *res->ViewComponent("cell",false);
  const Epetra_MultiVector& res_f = *res->ViewComponent("face",false);
  const Epetra_MultiVector& cv = *S_next_->GetFieldData("cell_volume")
      ->ViewComponent("cell",false);
  const Epetra_MultiVector& pres_f = *u->Data()->ViewComponent("face",false);
  double h = S_next_->time() - S_inter_->time();

  // Cell error is based upon error in mass conservation relative to
  // the current water content
  double enorm_cell(0.);
  int bad_cell = -1;
  unsigned int ncells = res_c.MyLength();
  for (unsigned int c=0; c!=ncells; ++c) {
    double tmp = std::abs(h*res_c[0][c]) / (atol_ * .25*.1*55000.*cv[0][c] + rtol_*std::abs(wc[0][c]));
    if (tmp > enorm_cell) {
      enorm_cell = tmp;
      bad_cell = c;
    }
  }

  // Face error is mismatch in flux, so relative to flux.
  double enorm_face(0.);
  int bad_face = -1;
  unsigned int nfaces = res_f.MyLength();
  for (unsigned int f=0; f!=nfaces; ++f) {
    double tmp = flux_tol_ * std::abs(res_f[0][f]) / (atol_ + rtol_*flux_max);
    if (tmp > enorm_face) {
      enorm_face = tmp;
      bad_face = f;
    }
  }

  // Write out Inf norms too.
  if (vo_->os_OK(Teuchos::VERB_MEDIUM)) {
    double infnorm_c(0.), infnorm_f(0.);
    res_c.NormInf(&infnorm_c);
    res_f.NormInf(&infnorm_f);

#ifdef HAVE_MPI
    double buf_c(enorm_cell), buf_f(enorm_face);
    MPI_Allreduce(&buf_c, &enorm_cell, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
    MPI_Allreduce(&buf_f, &enorm_face, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
#endif

    AmanziMesh::Entity_ID_List cells;
    mesh_->face_get_cells(bad_face, AmanziMesh::USED, &cells);
    *vo_->os() << "ENorm (cells) = " << enorm_cell << "[" << bad_cell << "] (" << infnorm_c << ")" << std::endl;

    if (cells.size() == 1) {
      *vo_->os() << "ENorm (faces) = " << enorm_face << "[" << cells[0] << "] (" << infnorm_f << ")" << std::endl;
    } else {
      *vo_->os() << "ENorm (faces) = " << enorm_face << "[" << cells[0] << "," << cells[1] << "] (" << infnorm_f << ")" << std::endl;
    }
  }

  // Communicate and take the max.
  double enorm_val(std::max<double>(enorm_face, enorm_cell));
#ifdef HAVE_MPI
  double buf = enorm_val;
  MPI_Allreduce(&buf, &enorm_val, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
#endif
  return enorm_val;
};


void Richards::PreconWC_(Teuchos::RCP<const TreeVector> u, Teuchos::RCP<TreeVector> Pu) {
  Teuchos::OSTab tab = vo_->getOSTab();
  if (vo_->os_OK(Teuchos::VERB_EXTREME))
    *vo_->os() << "  Apply precon variable switching to Liquid Saturation" << std::endl;

    // get old p, sat
  const Epetra_MultiVector& pres_prev =
      *S_next_->GetFieldData("pressure")->ViewComponent("cell",false);
  const Epetra_MultiVector& sat_prev =
      *S_next_->GetFieldData("saturation_liquid")->ViewComponent("cell",false);
  const double& patm = *S_next_->GetScalarData("atmospheric_pressure");

  // calculate ds/dp
  S_next_->GetFieldEvaluator("saturation_liquid")
      ->HasFieldDerivativeChanged(S_next_.ptr(), name_, key_);

  const Epetra_MultiVector& dsdp =
      *S_next_->GetFieldData("dsaturation_liquid_dpressure")->ViewComponent("cell",false);
  Epetra_MultiVector& dp = *Pu->Data()->ViewComponent("cell",false);

  Epetra_MultiVector s_new(dp);
  s_new.Multiply(1., dp, dsdp, 0.);
  s_new.Update(1., sat_prev, -1.); // s_new <-- s - ds

  AmanziMesh::Entity_ID ncells = s_new.MyLength();
  for (AmanziMesh::Entity_ID c=0; c!=ncells; ++c) {

    double p_prev = pres_prev[0][c];
    double p_standard = p_prev - dp[0][c];

    // cannot use if saturated, likely not useful if decreasing in saturation
    if (p_standard > p_prev && p_prev < patm && s_new[0][c] < 0.99) {
      double pc = wrms_->second[(*wrms_->first)[c]]->capillaryPressure(s_new[0][c]);
      double p_wc = patm - pc;
      dp[0][c] = p_prev - p_wc;
    }
  }

  // Now that we have monkeyed with cells, fix faces
  mfd_preconditioner_->UpdateConsistentFaceCorrection(*u->Data(), Pu->Data().ptr());
}

}  // namespace Flow
}  // namespace Amanzi



