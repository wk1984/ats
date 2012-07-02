/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/*
  A base two-phase, thermal Richard's equation with water vapor.

  License: BSD
  Authors: Ethan Coon (ATS version) (ecoon@lanl.gov)
*/

#ifndef PK_FLOW_OVERLAND_HH_
#define PK_FLOW_OVERLAND_HH_

#include <vector>
#include <cassert>

#include "Teuchos_RCP.hpp"
#include "Teuchos_ParameterList.hpp"

#include "composite_vector.hh"
#include "tree_vector.hh"
#include "state.hh"
#include "matrix_mfd.hh"
#include "boundary_function.hh"
#include "composite_vector_function.hh"

#include "PK.hh"
#include "pk_factory.hh"
#include "bdf_time_integrator.hh"

#include "wrm.hh"
#include "my_macro.hh"

namespace Amanzi {
namespace Flow {
#if 0
}}
#endif

class OverlandFlow : public PK {

public:
  OverlandFlow(Teuchos::ParameterList& flow_plist,
               const Teuchos::RCP<State>& S,
               const Teuchos::RCP<TreeVector>& solution);

  // main methods
  // -- Initialize owned (dependent) variables.
  virtual void initialize(const Teuchos::RCP<State>& S);

  // -- Choose a time step compatible with physics.
  virtual double get_dt() {
    return dt_;
  }

  // -- transfer operators -- pointer copy
  virtual void state_to_solution(const Teuchos::RCP<State>& S,
                                 const Teuchos::RCP<TreeVector>& soln);

  virtual void solution_to_state(const Teuchos::RCP<TreeVector>& soln,
                                 const Teuchos::RCP<State>& S);

  // -- Advance from state S to state S_next at time S0.time + dt.
  virtual bool advance(double dt);

  // -- Commit any secondary (dependent) variables.
  virtual void commit_state(double dt, const Teuchos::RCP<State>& S);

  // -- Update diagnostics for vis.
  virtual void calculate_diagnostics(const Teuchos::RCP<State>& S);

  // ConstantTemperature is a BDFFnBase
  // computes the non-linear functional g = g(t,u,udot)
  void fun(double t_old, double t_new, Teuchos::RCP<TreeVector> u_old,
           Teuchos::RCP<TreeVector> u_new, Teuchos::RCP<TreeVector> g);

  // applies preconditioner to u and returns the result in Pu
  virtual void precon(Teuchos::RCP<const TreeVector> u, Teuchos::RCP<TreeVector> Pu);

  // computes a norm on u-du and returns the result
  virtual double enorm(Teuchos::RCP<const TreeVector> u, Teuchos::RCP<const TreeVector> du);

  // updates the preconditioner
  virtual void update_precon(double t, Teuchos::RCP<const TreeVector> up, double h);

private:
  // boundary condition members
  virtual void UpdateBoundaryConditions_(const Teuchos::RCP<State>& S);
  virtual void ApplyBoundaryConditions_(const Teuchos::RCP<State>& S,
          const Teuchos::RCP<CompositeVector>& pres );

  // bdf needs help
  void DeriveFaceValuesFromCellValues_(const Teuchos::RCP<State>& S,
                                       const Teuchos::RCP<CompositeVector>& pres);

  // computational concerns in managing abs, rel perm
  // -- is abs perm changing?
  bool variable_abs_perm() { return variable_abs_perm_; }

  // -- builds tensor K, along with faced-based Krel if needed by the rel-perm method
  void UpdatePermeabilityData_(const Teuchos::RCP<State>& S);

  void CalculateRelativePermeabilityUpwindFlux_(
          const Teuchos::RCP<State>& S,
          const CompositeVector& flux,
          const CompositeVector& rel_perm_cells,
          const Teuchos::RCP<CompositeVector>& rel_perm_faces);


  // physical methods
  // -- diffusion term
  void ApplyDiffusion_(const Teuchos::RCP<State>& S, const Teuchos::RCP<CompositeVector>& g);

  // -- accumulation term
  void AddAccumulation_(const Teuchos::RCP<CompositeVector>& g);

  void AddLoadValue_(const Teuchos::RCP<State>& S,
                     const Teuchos::RCP<CompositeVector>& g) ;

  // -- add elevation to overland pressure
  void AddElevation_(const Teuchos::RCP<Amanzi::State>&) ;

  // -- update secondary variables from primary variables T,p
  void UpdateSecondaryVariables_(const Teuchos::RCP<State>& S);

  void UpdateLoadValue_(const Teuchos::RCP<State>& S);

  void UpdateConductivity_(const Teuchos::RCP<State>& S);
  void Conductivity_( const Teuchos::RCP<State>& S,
                      const CompositeVector& pressure,
                      const CompositeVector& manning_coef,
                      const CompositeVector& slope_magnitude,
                      const Teuchos::RCP<CompositeVector>& conductivity);

  // -- test 1, elevation methods
  void   TestOneSetUpElevationPars_() ;
  double TestOneElevation_( double x, double y ) ;
  void   TestOneSetElevation_( const Teuchos::RCP<State>& S ) ;
  
  // -- test 2, elevation methods
  int    TestTwoZoneFlag_ ( double x, double y ) ;
  void   TestTwoSetUpElevationPars_() ;
  double TestTwoElevation_( double x, double y ) ;
  void   TestTwoSetElevation_( const Teuchos::RCP<State>& S ) ;

  // -- setup of elevation pars
  void SetUpElevation_( const Teuchos::RCP<State>& S ) ;

private:
  // OVERLAND parameters
  std::vector<double> manning ;
  std::vector<double> slope_x ;
  std::vector<double> slope_y ;
  std::vector<double> zone_x  ;

  // control switches
  int internal_tests_;  // output information
  bool variable_abs_perm_;
  bool standalone_mode_; // domain mesh == surface mesh

  // time stuff
  int    niter_ ;
  double flow_time_;
  double dt_;
  double dt0_;

  // input parameter data
  Teuchos::ParameterList flow_plist_;

  // work data space
  std::vector<WhetStone::Tensor> K_;  // tensor of absolute permeability

  // rainfall flow rate model
  Teuchos::RCP<Functions::CompositeVectorFunction> rain_rate_function_;

  // Conductivity model
  Teuchos::RCP<Functions::CompositeVectorFunction> elevation_function_;
  Teuchos::RCP<Functions::CompositeVectorFunction> slope_function_;
  Teuchos::RCP<Functions::CompositeVectorFunction> manning_coef_function_;
  double manning_exp_;

  // mathematical operators
  Teuchos::RCP<Amanzi::BDFTimeIntegrator> time_stepper_;
  Teuchos::RCP<Operators::MatrixMFD> matrix_;
  Teuchos::RCP<Operators::MatrixMFD> preconditioner_;
  double atol_;
  double rtol_;
  double time_step_reduction_factor_;

  // boundary condition data
  Teuchos::RCP<Functions::BoundaryFunction> bc_pressure_;
  Teuchos::RCP<Functions::BoundaryFunction> bc_zero_gradient_;
  Teuchos::RCP<Functions::BoundaryFunction> bc_head_;
  Teuchos::RCP<Functions::BoundaryFunction> bc_flux_;
  std::vector<Operators::Matrix_bc> bc_markers_;
  std::vector<double> bc_values_;

  // factory registration
  static RegisteredPKFactory<OverlandFlow> reg_;

  // DEBUGGING STUFF
  void print_pressure( const Teuchos::RCP<State>& S, string prt_str="" ) const ;
  void print_vector ( const Teuchos::RCP<State>& S, const Teuchos::RCP<const CompositeVector>& p, string prt_str="" ) const ;
  void print_vector2( const Teuchos::RCP<State>& S,
                      const Teuchos::RCP<const CompositeVector>& pres,
                      const Teuchos::RCP<const CompositeVector>& elev,
                      string prt_str="" ) const ;

  void print_faceval( const Teuchos::RCP<State>& S,
                      const Teuchos::RCP<const CompositeVector>& vec,
                      string prt_str ) const ;

  // write flow rate on disk
  void output_flow_rate() ;

};

}  // namespace AmanziFlow
}  // namespace Amanzi

#endif