/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */
/* -------------------------------------------------------------------------
   ATS

   License: see $ATS_DIR/COPYRIGHT
   Author: Ethan Coon, Adam Atchley, Satish Karra

   DOCUMENT ME
   Surface Energy Balance for Snow Surface and Ground Surface
   Calculates Energy flux, rate or water, and water temperature
   entering through the surface skin.  Snow surface energy balance
   is calculated at equilibrium with ground/surface water and Air.

   0=(1-albedo)QswIn + Qlwin + QlwOut(Ts) + Qh(Ts) + Qc(Ts) + Qe(Ts)
   Qc = the energy delived to the subsurface
   The rate of water entering the surface skin occurs only when Ts > 0
   In which case Ts is set = 0 and the excess energy = Qm and the melt rate (Mr) is
   delivered to the surface skin via:
   Mr = Qm/(ROWw*Hf)
   ROWw = density of water
   Hf = latent heat of fusion
   The temperature of water in assumed to be 0 C

   In cases without snow the energy balance equations is:

   Qex = 0=(1-albedo)QswIn + Qlwin + QlwOut(Ts) + Qh(Ts) + Qe(Ts)
   Qex is the energy derived to the subsurface skin
   All water entering the surface skin is assumed to be precipitated
   or condensed on the surface and has a temperature of Air.
   ------------------------------------------------------------------------- */

#ifndef PKS_ENERGY_SEB_VPL_HH_
#define PKS_ENERGY_SEB_VPL_HH_

#include "PK_Factory.hh"
#include "pk_physical_default.hh"
#include "primary_variable_field_evaluator.hh"


namespace Amanzi {
namespace SurfaceBalance {

class SurfaceBalanceSEBVPL : public PK_Physical_Default {

 public:

  SurfaceBalanceSEBVPL(Teuchos::ParameterList& pk_tree,
                    const Teuchos::RCP<Teuchos::ParameterList>& global_list,
                    const Teuchos::RCP<State>& S,
                    const Teuchos::RCP<TreeVector>& solution);

  // Virtual destructor
  virtual ~SurfaceBalanceSEBVPL() {}

  // SurfaceBalanceSEBVPL is a PK
  // -- Setup data
  virtual void Setup(const Teuchos::Ptr<State>& S);

  // -- Initialize owned (dependent) variables.
  virtual void Initialize(const Teuchos::Ptr<State>& S);

  // -- provide a timestep size
  virtual double get_dt() {
    return dt_;
  }

  virtual void set_dt(double dt) {dt_ = dt;}

  virtual std::string name(){ return "SurfaceBalanceSEBVPL";}

  // -- Commit any secondary (dependent) variables.
  virtual void CommitStep(double t_old, double t_new, const Teuchos::RCP<State>& S) {}

  // -- Update diagnostics for vis.
  virtual void CalculateDiagnostics(const Teuchos::RCP<State>& S) {}

  // -- advance via one of a few methods
  virtual bool AdvanceStep(double t_old, double t_new, bool reinit);

 protected:
  // A few options for advance
  void CalculateSEB_();

 protected:

  // multiple primary variables
  Teuchos::RCP<PrimaryVariableFieldEvaluator> pvfe_esource_;
  Teuchos::RCP<PrimaryVariableFieldEvaluator> pvfe_wsource_;
  Teuchos::RCP<PrimaryVariableFieldEvaluator> pvfe_w_v_source_;
  Teuchos::RCP<PrimaryVariableFieldEvaluator> pvfe_wtemp_;

  double dt_;
  double min_wind_speed_;
  double albedo_trans_;
  double snow_ground_trans_;
  double no_snow_trans_;

  Teuchos::RCP<const AmanziMesh::Mesh> subsurf_mesh_;

 private:
  // factory registration
  static RegisteredPKFactory<SurfaceBalanceSEBVPL> reg_;
};

} // namespace
} // namespace

#endif
