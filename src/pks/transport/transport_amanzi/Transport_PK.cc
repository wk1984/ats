/*
  Transport PK 

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include <algorithm>
#include <vector>

#include "boost/algorithm/string.hpp"
#include "Epetra_Vector.h"
#include "Epetra_IntVector.h"
#include "Epetra_MultiVector.h"
#include "Epetra_Import.h"
#include "Teuchos_RCP.hpp"

#include "BCs.hh"
#include "errors.hh"
#include "Explicit_TI_RK.hh"
#include "FieldEvaluator.hh"
#include "GMVMesh.hh"
#include "LinearOperatorDefs.hh"
#include "LinearOperatorFactory.hh"
#include "Mesh.hh"
#include "OperatorDefs.hh"
#include "OperatorDiffusionFactory.hh"
#include "OperatorDiffusion.hh"
#include "OperatorAccumulation.hh"
#include "PK_DomainFunctionFactory.hh"
#include "PK_Utils.hh"

#include "MultiscaleTransportPorosityFactory.hh"
#include "Transport_PK_ATS.hh"
#include "TransportDomainFunction.hh"
#include "TransportBoundaryFunction_Alquimia.hh"
#include "TransportSourceFunction_Alquimia.hh"

namespace Amanzi {
namespace Transport {

/* ******************************************************************
* New constructor compatible with new MPC framework.
****************************************************************** */
Transport_PK_ATS::Transport_PK_ATS(Teuchos::ParameterList& pk_tree,
                           const Teuchos::RCP<Teuchos::ParameterList>& glist,
                           const Teuchos::RCP<State>& S,
                           const Teuchos::RCP<TreeVector>& soln) :
  S_(S),
  soln_(soln)
{
  std::string pk_name = pk_tree.name();
  const char* result = pk_name.data();

  boost::iterator_range<std::string::iterator> res = boost::algorithm::find_last(pk_name,"->"); 
  if (res.end() - pk_name.end() != 0) boost::algorithm::erase_head(pk_name,  res.end() - pk_name.begin());

  
// Create miscaleneous lists.
  Teuchos::RCP<Teuchos::ParameterList> pk_list = Teuchos::sublist(glist, "PKs", true);
  //std::cout<<*pk_list;
  tp_list_ = Teuchos::sublist(pk_list, pk_name, true);



  if (tp_list_->isParameter("component names")) {
    component_names_ = tp_list_->get<Teuchos::Array<std::string> >("component names").toVector();
    mol_masses_ = tp_list_->get<Teuchos::Array<double> >("component molar masses").toVector();
  }
  else if (glist->isSublist("Cycle Driver")) {
    if (glist->sublist("Cycle Driver").isParameter("component names")) {
      // grab the component names
      component_names_ = glist->sublist("Cycle Driver")
        .get<Teuchos::Array<std::string> >("component names").toVector();
    } else {
      Errors::Message msg("Transport PK: parameter component names is missing.");
      Exceptions::amanzi_throw(msg);
    }
  } else {
    Errors::Message msg("Transport PK: sublist Cycle Driver or parameter component names is missing.");
    Exceptions::amanzi_throw(msg);
  }
  
  preconditioner_list_ = Teuchos::sublist(glist, "Preconditioners");
  linear_solver_list_ = Teuchos::sublist(glist, "Solvers");
  nonlinear_solver_list_ = Teuchos::sublist(glist, "Nonlinear solvers");

  subcycling_ = tp_list_->get<bool>("transport subcycling", true);
   
  // initialize io
  Teuchos::RCP<Teuchos::ParameterList> units_list = Teuchos::sublist(glist, "units");
  units_.Init(*units_list);

  vo_ = Teuchos::null;
}


/* ******************************************************************
* Old constructor for unit tests.
****************************************************************** */
Transport_PK_ATS::Transport_PK_ATS(const Teuchos::RCP<Teuchos::ParameterList>& glist,
                           Teuchos::RCP<State> S, 
                           const std::string& pk_list_name,
                           std::vector<std::string>& component_names) :
    S_(S),
    component_names_(component_names)
{
  // Create miscaleneous lists.
  Teuchos::RCP<Teuchos::ParameterList> pk_list = Teuchos::sublist(glist, "PKs", true);
  tp_list_ = Teuchos::sublist(pk_list, pk_list_name, true);

  preconditioner_list_ = Teuchos::sublist(glist, "Preconditioners");
  linear_solver_list_ = Teuchos::sublist(glist, "Solvers");
  nonlinear_solver_list_ = Teuchos::sublist(glist, "Nonlinear solvers");

  vo_ = Teuchos::null;
}


/* ******************************************************************
* Routine processes parameter list. It needs to be called only once
* on each processor.                                                     
****************************************************************** */
Transport_PK_ATS::~Transport_PK_ATS()
{ 
  if (vo_ != Teuchos::null) {
    vo_ = Teuchos::null;
  }

}


/* ******************************************************************
* Setup for Alquimia.
****************************************************************** */
#ifdef ALQUIMIA_ENABLED
void Transport_PK_ATS::SetupAlquimia(Teuchos::RCP<AmanziChemistry::Alquimia_PK> chem_pk,
                                 Teuchos::RCP<AmanziChemistry::ChemistryEngine> chem_engine)
{
  chem_pk_ = chem_pk;
  chem_engine_ = chem_engine;

  if (chem_engine_ != Teuchos::null) {
    // Retrieve the component names (primary and secondary) from the chemistry 
    // engine.
    std::vector<std::string> component_names;
    chem_engine_->GetPrimarySpeciesNames(component_names);
    component_names_ = component_names;
    for (int i = 0; i < chem_engine_->NumAqueousComplexes(); ++i) {
      char secondary_name[128];
      snprintf(secondary_name, 127, "secondary_%d", i);
      component_names_.push_back(secondary_name);
    }
  }
}
#endif

void Transport_PK_ATS::set_states(const Teuchos::RCP<const State>& S,
                                  const Teuchos::RCP<State>& S_inter,
                                  const Teuchos::RCP<State>& S_next) {

    //S_ = S;
  S_inter_ = S_inter;
  S_next_ = S_next;
}


/* ******************************************************************
* Define structure of this PK.
****************************************************************** */
void Transport_PK_ATS::Setup(const Teuchos::Ptr<State>& S)
{
  passwd_ = "state";  // owner's password


  domain_name_ = tp_list_->get<std::string>("domain name", "domain");

  saturation_key_ = tp_list_->get<std::string>("saturation_key", 
                                               getKey(domain_name_, "saturation_liquid"));
  prev_saturation_key_ = tp_list_->get<std::string>("prev_saturation_key", 
                                                    getKey(domain_name_, "prev_saturation_liquid"));
  flux_key_ = tp_list_->get<std::string>("flux_key", 
                                         getKey(domain_name_, "mass_flux"));
  darcy_flux_key_ = tp_list_->get<std::string>("darcy_flux_key", 
                                               getKey(domain_name_, "mass_flux"));
  permeability_key_ = tp_list_->get<std::string>("permeability_key", 
                                                 getKey(domain_name_, "permeability"));
  tcc_key_ = tp_list_->get<std::string>("concentration_key", 
                                        getKey(domain_name_, "total_component_concentration"));
  porosity_key_ = tp_list_->get<std::string>("porosity_key", getKey(domain_name_, "porosity"));
  molar_density_key_ = tp_list_->get<std::string>("molar_density_key", getKey(domain_name_, "molar_density_liquid"));
  tcc_matrix_key_ = tp_list_->get<std::string>("tcc_matrix_key", 
                                               getKey(domain_name_, "total_component_concentraion_matrix"));


  // molar_density_key_ = tp_list_->get<std::string>("molar_density_key_", getKey(domain_name_, "molar_density")); 

  mesh_ = S->GetMesh(domain_name_);
  dim = mesh_->space_dimension();

  // cross-coupling of PKs
  Teuchos::RCP<Teuchos::ParameterList> physical_models =
      Teuchos::sublist(tp_list_, "physical models and assumptions");
  bool abs_perm = physical_models->get<bool>("permeability field is required", false);
  vol_flux_conversion_ = tp_list_->get<bool>("vol_flux conversion is required", false);
  std::string multiscale_model = physical_models->get<std::string>("multiscale model", "single porosity");


  // require state fields when Flow PK is off
  if (!S->HasField(permeability_key_) && abs_perm) {
    S->RequireField(permeability_key_)->SetMesh(mesh_)->SetGhosted(true)
      ->AddComponent("cell", AmanziMesh::CELL, dim);
  }

  if (!S->HasField(flux_key_)) {
    S->RequireField(flux_key_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("face", AmanziMesh::FACE, 1);
  }

  if (!S->HasField(saturation_key_)) {
    S->RequireField(saturation_key_)->SetMesh(mesh_)->SetGhosted(true)
      ->AddComponent("cell", AmanziMesh::CELL, 1);
  }
  if (!S->HasField(prev_saturation_key_)) {
    S->RequireField(prev_saturation_key_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, 1);
    S->GetField(prev_saturation_key_, passwd_)->set_io_vis(false);
  }

  //S->RequireFieldEvaluator(flux_key_);

  // require state fields when Transport PK is on
  if (component_names_.size() == 0) {
    Errors::Message msg;
    msg << "Transport PK: list of solutes is empty.\n";
    Exceptions::amanzi_throw(msg);  
  }

  int ncomponents = component_names_.size();
  if (!S->HasField(tcc_key_)) {
    std::vector<std::vector<std::string> > subfield_names(1);
    subfield_names[0] = component_names_;

    S->RequireField(tcc_key_, passwd_, subfield_names)
      ->SetMesh(mesh_)->SetGhosted(true)
      ->AddComponent("cell", AmanziMesh::CELL, ncomponents);
  }


  //testing evaluators
  if (!S->HasField(porosity_key_)) {
    S->RequireField(porosity_key_, porosity_key_)->SetMesh(mesh_)->SetGhosted(true)
      ->SetComponent("cell", AmanziMesh::CELL, 1);
    S->RequireFieldEvaluator(porosity_key_);
  }

  // if (!S->HasField(molar_density_key_)){
  //   S->RequireField(molar_density_key_, molar_density_key_)->SetMesh(mesh_)->SetGhosted(true)
  //     ->SetComponent("cell", AmanziMesh::CELL, 1);
  //   S->RequireFieldEvaluator(molar_density_key_);
  // }

  // require multiscale fields
  multiscale_porosity_ = false;
  if (multiscale_model == "dual porosity") {
    multiscale_porosity_ = true;
    Teuchos::RCP<Teuchos::ParameterList>
        msp_list = Teuchos::sublist(tp_list_, "multiscale models", true);
    msp_ = CreateMultiscaleTransportPorosityPartition(mesh_, msp_list);

    if (!S->HasField(tcc_matrix_key_)) {
      std::vector<std::vector<std::string> > subfield_names(1);
      subfield_names[0] = component_names_;

      S->RequireField(tcc_matrix_key_, passwd_, subfield_names)
        ->SetMesh(mesh_)->SetGhosted(false)
        ->SetComponent("cell", AmanziMesh::CELL, ncomponents);
    }
  }
}


/* ******************************************************************
* Routine processes parameter list. It needs to be called only once
* on each processor.                                                     
****************************************************************** */
void Transport_PK_ATS::Initialize(const Teuchos::Ptr<State>& S)
{
  // Set initial values for transport variables.
  dt_ = dt_debug_ = t_physics_ = 0.0;
  double time = S->time();
  if (time >= 0.0) t_physics_ = time;

  
  dispersion_preconditioner = "identity";

  internal_tests = 0;
  tests_tolerance = TRANSPORT_CONCENTRATION_OVERSHOOT;

  bc_scaling = 0.0;

  // Create verbosity object.
  Teuchos::ParameterList vlist;
  vlist.sublist("verbose object") = tp_list_->sublist("verbose object");
  vo_ =  Teuchos::rcp(new VerboseObject("TransportPK", vlist)); 

  

  MyPID = mesh_->get_comm()->MyPID();

  // initialize missed fields
  InitializeFields_(S);

  //create copies
  S->RequireFieldCopy(tcc_key_, "subcycling", passwd_);
  tcc_tmp = S->GetField(tcc_key_, passwd_)->GetCopy("subcycling", passwd_)->GetFieldData();

  S->RequireFieldCopy(saturation_key_, "subcycle_start", passwd_);
  ws_subcycle_start = S->GetFieldCopyData(saturation_key_, "subcycle_start",passwd_)
    ->ViewComponent("cell");

  S->RequireFieldCopy(saturation_key_, "subcycle_end", passwd_);
  ws_subcycle_end = S->GetFieldCopyData(saturation_key_, "subcycle_end", passwd_)
    ->ViewComponent("cell");

  S->RequireFieldCopy(molar_density_key_, "subcycle_start", passwd_);
  mol_dens_subcycle_start = S->GetFieldCopyData(molar_density_key_, "subcycle_start",passwd_)->ViewComponent("cell");

  S->RequireFieldCopy(molar_density_key_, "subcycle_end", passwd_);
  mol_dens_subcycle_end = S->GetFieldCopyData(molar_density_key_, "subcycle_end", passwd_)->ViewComponent("cell");

  // S->RequireFieldCopy(saturation_key_, "subcycle_start", passwd_);
  // ws_subcycle_start = S->GetFieldCopyData(saturation_key_, "subcycle_start",passwd_)
  //   ->ViewComponent("cell");

  // S->RequireFieldCopy(saturation_key_, "subcycle_end", passwd_);
  // ws_subcycle_end = S->GetFieldCopyData(saturation_key_, "subcycle_end", passwd_)
  //   ->ViewComponent("cell");

  S->RequireFieldCopy(flux_key_, "next_timestep", passwd_);


  // Check input parameters. Due to limited amount of checks, we can do it earlier.
  Policy(S.ptr());

  ncells_owned = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  ncells_wghost = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::USED);

  nfaces_owned = mesh_->num_entities(AmanziMesh::FACE, AmanziMesh::OWNED);
  nfaces_wghost = mesh_->num_entities(AmanziMesh::FACE, AmanziMesh::USED);
  nnodes_wghost = mesh_->num_entities(AmanziMesh::NODE, AmanziMesh::USED);

  // extract control parameters
  InitializeAll_();
 
  // state pre-prosessing
  Teuchos::RCP<const CompositeVector> cv;

  ws_ = S->GetFieldData(saturation_key_)->ViewComponent("cell", false);
  
  ws_prev_ = S -> GetFieldData(prev_saturation_key_) -> ViewComponent("cell", false);

  phi_ = S->GetFieldData(porosity_key_) -> ViewComponent("cell", false);

  mol_dens_ = S_->GetFieldData(molar_density_key_) -> ViewComponent("cell", false);
  mol_dens_prev_ = S_->GetFieldData(molar_density_key_) -> ViewComponent("cell", false);
  

  tcc = S->GetFieldData(tcc_key_, passwd_);

  flux = S->GetFieldData(flux_key_)->ViewComponent("face", true);

  //create vector of conserved quatities
  conserve_qty_ = Teuchos::rcp(new Epetra_MultiVector(*(S->GetFieldData(tcc_key_)->ViewComponent("cell", true))));

  // memory for new components
  // tcc_tmp = Teuchos::rcp(new CompositeVector(*(S->GetFieldData(tcc_key_))));
  // *tcc_tmp = *tcc;

  // upwind 
  const Epetra_Map& fmap_wghost = mesh_->face_map(true);
  upwind_cell_ = Teuchos::rcp(new Epetra_IntVector(fmap_wghost));
  downwind_cell_ = Teuchos::rcp(new Epetra_IntVector(fmap_wghost));

  IdentifyUpwindCells();

  // advection block initialization
  current_component_ = -1;

  const Epetra_Map& cmap_owned = mesh_->cell_map(false);
 
  // reconstruction initialization
  const Epetra_Map& cmap_wghost = mesh_->cell_map(true);
  lifting_ = Teuchos::rcp(new Operators::ReconstructionCell(mesh_));

  // mechanical dispersion
  flag_dispersion_ = false;
  if (tp_list_->isSublist("material properties")) {
    Teuchos::RCP<Teuchos::ParameterList>
        mdm_list = Teuchos::sublist(tp_list_, "material properties");
    mdm_ = CreateMDMPartition(mesh_, mdm_list, flag_dispersion_);
    if (flag_dispersion_) CalculateAxiSymmetryDirection();
  }


  // create boundary conditions
  if (tp_list_->isSublist("boundary conditions")) {
    // -- try tracer-type conditions
    PK_DomainFunctionFactory<TransportDomainFunction> factory(mesh_);
    Teuchos::ParameterList& clist = tp_list_->sublist("boundary conditions").sublist("concentration");

    for (Teuchos::ParameterList::ConstIterator it = clist.begin(); it != clist.end(); ++it) {
      std::string name = it->first;
      if (clist.isSublist(name)) {
        Teuchos::ParameterList& bc_list = clist.sublist(name);
        if (name=="coupling") {
          Teuchos::ParameterList::ConstIterator it1 = bc_list.begin();
          std::string specname = it1->first;
          Teuchos::ParameterList& spec = bc_list.sublist(specname);
          Teuchos::RCP<TransportDomainFunction> 
            bc = factory.Create(spec, "boundary concentration", AmanziMesh::FACE, Kxy);

          for (int i = 0; i < component_names_.size(); i++){
            bc->tcc_names().push_back(component_names_[i]);
            bc->tcc_index().push_back(i);
          }
          //          std::cout << "tcc_names "<<tcc_names<<"\n";
          //std::cout << "tcc_index "<<tcc_index<<"\n";
          
          bc->set_state(S_);
          bcs_.push_back(bc);

        }else{
          for (Teuchos::ParameterList::ConstIterator it1 = bc_list.begin(); it1 != bc_list.end(); ++it1) {
            std::string specname = it1->first;
            Teuchos::ParameterList& spec = bc_list.sublist(specname);
            Teuchos::RCP<TransportDomainFunction> 
              bc = factory.Create(spec, "boundary concentration", AmanziMesh::FACE, Kxy);

            std::vector<int>& tcc_index = bc->tcc_index();
            std::vector<std::string>& tcc_names = bc->tcc_names();
            bc->set_state(S_);

            tcc_names.push_back(name);
            tcc_index.push_back(FindComponentNumber(name));

            bcs_.push_back(bc);          
          }
        }
      }
    }
#ifdef ALQUIMIA_ENABLED
    // -- try geochemical conditions
    Teuchos::ParameterList& glist = tp_list_->sublist("boundary conditions").sublist("geochemical");

    for (Teuchos::ParameterList::ConstIterator it = glist.begin(); it != glist.end(); ++it) {
      std::string specname = it->first;
      Teuchos::ParameterList& spec = glist.sublist(specname);

      Teuchos::RCP<TransportBoundaryFunction_Alquimia> 
        bc = Teuchos::rcp(new TransportBoundaryFunction_Alquimia(spec, mesh_, chem_pk_, chem_engine_));

      std::vector<int>& tcc_index = bc->tcc_index();
      std::vector<std::string>& tcc_names = bc->tcc_names();
      
      for (int i = 0; i < tcc_names.size(); i++) {
        tcc_index.push_back(FindComponentNumber(tcc_names[i]));
      }

      bcs_.push_back(bc);
    }
#endif
  } else {
    if (vo_->getVerbLevel() > Teuchos::VERB_NONE) {
      *vo_->os() << vo_->color("yellow") << "No BCs were specified." << vo_->reset() << std::endl;
    }
  }


  // boundary conditions initialization
  time = t_physics_;
  for (int i = 0; i < bcs_.size(); i++) {
    bcs_[i]->Compute(time, time);
  }

  VV_CheckInfluxBC();

  // source term initialization: so far only "concentration" is available.
  if (tp_list_->isSublist("source terms")) {
    PK_DomainFunctionFactory<TransportDomainFunction> factory(mesh_);
    //if (domain_name_ == "domain")  PKUtils_CalculatePermeabilityFactorInWell(S_.ptr(), Kxy);

    Teuchos::ParameterList& clist = tp_list_->sublist("source terms").sublist("concentration");
    for (Teuchos::ParameterList::ConstIterator it = clist.begin(); it != clist.end(); ++it) {
      std::string name = it->first;
      if (clist.isSublist(name)) {
        Teuchos::ParameterList& src_list = clist.sublist(name);
        if (name=="coupling") {
          Teuchos::ParameterList::ConstIterator it1 = src_list.begin();
          std::string specname = it1->first;
          Teuchos::ParameterList& spec = src_list.sublist(specname);
          Teuchos::RCP<TransportDomainFunction> src = 
              factory.Create(spec, "sink", AmanziMesh::CELL, Kxy);

          for (int i = 0; i < component_names_.size(); i++){
            src->tcc_names().push_back(component_names_[i]);
            src->tcc_index().push_back(i);
          }
          src->set_state(S_);
          srcs_.push_back(src);
          
        }else{
          for (Teuchos::ParameterList::ConstIterator it1 = src_list.begin(); it1 != src_list.end(); ++it1) {
            std::string specname = it1->first;
            Teuchos::ParameterList& spec = src_list.sublist(specname);
            Teuchos::RCP<TransportDomainFunction> src = 
              factory.Create(spec, "sink", AmanziMesh::CELL, Kxy);

            src->tcc_names().push_back(name);
            src->tcc_index().push_back(FindComponentNumber(name));
          
            src->set_state(S_);
            srcs_.push_back(src);
          }
        }
      }
    }
#ifdef ALQUIMIA_ENABLED
    // -- try geochemical conditions
    Teuchos::ParameterList& glist = tp_list_->sublist("source terms").sublist("geochemical");

    for (auto it = glist.begin(); it != glist.end(); ++it) {
      std::string specname = it->first;
      Teuchos::ParameterList& spec = glist.sublist(specname);

      Teuchos::RCP<TransportSourceFunction_Alquimia> 
          src = Teuchos::rcp(new TransportSourceFunction_Alquimia(spec, mesh_, chem_pk_, chem_engine_));

      std::vector<int>& tcc_index = src->tcc_index();
      std::vector<std::string>& tcc_names = src->tcc_names();

      for (int i = 0; i < tcc_names.size(); i++) {
        tcc_index.push_back(FindComponentNumber(tcc_names[i]));
      }

      srcs_.push_back(src);
    }
#endif
  }

  // Temporarily Transport hosts Henry law.
  PrepareAirWaterPartitioning_();

  if (vo_->getVerbLevel() >= Teuchos::VERB_MEDIUM) {
    Teuchos::OSTab tab = vo_->getOSTab();
    *vo_->os() << "Number of components: " << tcc->size() << std::endl
               << "cfl=" << cfl_ << " spatial/temporal discretization: " 
               << spatial_disc_order << " " << temporal_disc_order << std::endl;
    *vo_->os() << vo_->color("green") << "Initalization of PK is complete." 
               << vo_->reset() << std::endl << std::endl;
  }


}


/* ******************************************************************
* Initalized fields left by State and other PKs.
****************************************************************** */
void Transport_PK_ATS::InitializeFields_(const Teuchos::Ptr<State>& S)
{
  Teuchos::OSTab tab = vo_->getOSTab();

  // set popular default values when flow PK is off
  if (S->HasField(saturation_key_)) {
    if (S->GetField(saturation_key_)->owner() == passwd_) {
      if (!S->GetField(saturation_key_, passwd_)->initialized()) {
        S->GetFieldData(saturation_key_, passwd_)->PutScalar(1.0);
        S->GetField(saturation_key_, passwd_)->set_initialized();

        if (vo_->getVerbLevel() >= Teuchos::VERB_MEDIUM)
            *vo_->os() << "initilized saturation_liquid to value 1.0" << std::endl;  
      }
      InitializeFieldFromField_(prev_saturation_key_, saturation_key_, S, true, true);
    }
    else {
      if (S->GetField(prev_saturation_key_)->owner() == passwd_) {
        if (!S->GetField(prev_saturation_key_, passwd_)->initialized()) {
          // S->GetFieldData(prev_saturation_key_, passwd_)->PutScalar(1.0);
          // S->GetField(prev_saturation_key_, passwd_)->set_initialized();
          // if (S->HasFieldEvaluator(getKey(domain_,saturation_key_))){
          //   S->GetFieldEvaluator(getKey(domain_,saturation_key_))->HasFieldChanged(S.ptr(), "transport");
          // }
          InitializeFieldFromField_(prev_saturation_key_, saturation_key_, S, true, true);
          S->GetField(prev_saturation_key_, passwd_)->set_initialized();

          if (vo_->getVerbLevel() >= Teuchos::VERB_MEDIUM)
            *vo_->os() << "initilized prev_saturation_liquid from saturation" << std::endl;
        }
      }
    }
  }

  InitializeFieldFromField_(tcc_matrix_key_, tcc_key_, S, false, false);

  //InitializeFieldFromField_(flux_key_, darcy_flux_key_, S, false, false);

  // if (vol_flux_conversion_){
  //   vol_flux = S->GetFieldData(flux_key_, passwd_)->ViewComponent("face", true);
  //   ComputeVolumeDarcyFlux(S->GetFieldData(darcy_flux_key_)->ViewComponent("face", true),
  //                          S->GetFieldData(molar_density_key_)->ViewComponent("cell", true),
  //                          vol_flux);
  // }

}


/* ****************************************************************
* Auxiliary initialization technique.
**************************************************************** */
void Transport_PK_ATS::InitializeFieldFromField_(const std::string& field0, 
                                                 const std::string& field1, 
                                                 const Teuchos::Ptr<State>& S,
                                                 bool call_evaluator,
                                                 bool overwrite)
{
  if (S->HasField(field0)) {
    if (S->GetField(field0)->owner() == passwd_) {
      if ((!S->GetField(field0, passwd_)->initialized())||(overwrite)) {
        if (call_evaluator)
            S->GetFieldEvaluator(field1)->HasFieldChanged(S.ptr(), passwd_);

        const CompositeVector& f1 = *S->GetFieldData(field1);
        CompositeVector& f0 = *S->GetFieldData(field0, passwd_);
        
        double vmin0, vmax0, vavg0;
        double vmin1, vmax1, vavg1;
        
        // f0.ViewComponent("cell")->MinValue(&vmin0);
        // f1.ViewComponent("cell")->MinValue(&vmin1);
 
        // std::cout<<field0<<" vmin "<<vmin0<<"\n"; 
        // std::cout<<field1<<" vmin "<<vmin1<<"\n";

        f0 = f1;

        // f0.ViewComponent("cell")->MinValue(&vmin0);
        // f1.ViewComponent("cell")->MinValue(&vmin1);
 
        // std::cout<<field0<<" vmin "<<vmin0<<"\n"; 
        // std::cout<<field1<<" vmin "<<vmin1<<"\n";
        // *vo_->os() << "initiliazed " << field0 << " to " << field1 << std::endl;
      
        S->GetField(field0, passwd_)->set_initialized();
        if ((vo_->getVerbLevel() >= Teuchos::VERB_MEDIUM)&&(!overwrite)){
          *vo_->os() << "initiliazed " << field0 << " to " << field1 << std::endl;
        }
      }
    }
  }
}


/* *******************************************************************
* Estimation of the time step based on T.Barth (Lecture Notes   
* presented at VKI Lecture Series 1994-05, Theorem 4.2.2.       
* Routine must be called every time we update a flow field.
*
* Warning: Barth calculates influx, we calculate outflux. The methods
* are equivalent for divergence-free flows and gurantee EMP. Outflux 
* takes into account sinks and sources but preserves only positivity
* of an advected mass.
* ***************************************************************** */
double Transport_PK_ATS::StableTimeStep()
{
  S_next_->GetFieldData(flux_key_)->ScatterMasterToGhosted("face");

  // if (vol_flux_conversion_){
  //   vol_flux = S_next_->GetFieldData(flux_key_, passwd_)->ViewComponent("face", true);
  //   ComputeVolumeDarcyFlux(S_next_->GetFieldData(darcy_flux_key_)->ViewComponent("face", true),
  //                          S_next_->GetFieldData(molar_density_key_)->ViewComponent("cell", true),
  //                          vol_flux);

  // }
  
  flux = S_next_->GetFieldData(flux_key_)->ViewComponent("face", true);

  IdentifyUpwindCells();

  tcc = S_->GetFieldData(tcc_key_, passwd_);
  Epetra_MultiVector& tcc_prev = *tcc->ViewComponent("cell");

  // loop over faces and accumulate upwinding fluxes
  std::vector<double> total_outflux(ncells_wghost, 0.0);

  for (int f = 0; f < nfaces_wghost; f++) {
    int c = (*upwind_cell_)[f];
    if (c >= 0) total_outflux[c] += fabs((*flux)[0][f]);
  }

  Sinks2TotalOutFlux(tcc_prev, total_outflux, 0, num_aqueous - 1);



  // modify estimate for other models
  if (multiscale_porosity_) {
    const Epetra_MultiVector& wcm_prev = *S_next_->GetFieldData("prev_water_content_matrix")->ViewComponent("cell");
    const Epetra_MultiVector& wcm = *S_next_->GetFieldData("water_content_matrix")->ViewComponent("cell");

    double dtg = S_->final_time() - S_->initial_time();
    for (int c = 0; c < ncells_owned; ++c) {
      double flux_liquid = (wcm[0][c] - wcm_prev[0][c]) / dtg;
      msp_->second[(*msp_->first)[c]]->UpdateStabilityOutflux(flux_liquid, &total_outflux[c]);
    }
  }

  // loop over cells and calculate minimal time step
  double vol, outflux, dt_cell;
  vol=0;
  dt_ = dt_cell = TRANSPORT_LARGE_TIME_STEP;
  int cmin_dt = 0;
  for (int c = 0; c < ncells_owned; c++) {
    outflux = total_outflux[c];
    if ((outflux > 0) && ((*ws_prev_)[0][c]>0) && ((*ws_)[0][c]>0) ) {
      vol = mesh_->cell_volume(c);
      dt_cell = vol * (*mol_dens_)[0][c] * (*phi_)[0][c] * std::min( (*ws_prev_)[0][c], (*ws_)[0][c] ) / outflux;
    }
    if (dt_cell < dt_) {
      dt_ = dt_cell;
      cmin_dt = c;
    }
  }

  if (spatial_disc_order == 2) dt_ /= 2;

  // communicate global time step
  double dt_tmp = dt_;
#ifdef HAVE_MPI
  const Epetra_Comm& comm = ws_prev_->Comm();
  comm.MinAll(&dt_tmp, &dt_, 1);
#endif

  // incorporate developers and CFL constraints
  dt_ = std::min(dt_, dt_debug_);
  dt_ *= cfl_;

  // print optional diagnostics using maximum cell id as the filter
 //  if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH) {
//     int cmin_dt_unique = (fabs(dt_tmp * cfl_ - dt_) < 1e-6 * dt_) ? cmin_dt : -1;
 
// #ifdef HAVE_MPI
//     int cmin_dt_tmp = cmin_dt_unique;
//     comm.MaxAll(&cmin_dt_tmp, &cmin_dt_unique, 1);
// #endif
//     if (cmin_dt == cmin_dt_unique) {
//       const AmanziGeometry::Point& p = mesh_->cell_centroid(cmin_dt);

//       Teuchos::OSTab tab = vo_->getOSTab();
//       *vo_->os() << "cell " << cmin_dt << " has smallest dt, (" << p[0] << ", " << p[1];
//       if (p.dim() == 3) *vo_->os() << ", " << p[2];
//       *vo_->os() << ")" << std::endl;
//     }
//   }
  return dt_;
}


/* ******************************************************************* 
* Estimate returns last time step unless it is zero.     
******************************************************************* */
double Transport_PK_ATS::get_dt()
{
  if (subcycling_) {
    return 1e+99;
  } else {
    StableTimeStep();
    return dt_;
  }
}


/* ******************************************************************* 
* MPC will call this function to advance the transport state.
* Efficient subcycling requires to calculate an intermediate state of
* saturation only once, which leads to a leap-frog-type algorithm.
******************************************************************* */
bool Transport_PK_ATS::AdvanceStep(double t_old, double t_new, bool reinit)
{ 
  bool failed = false;
  double dt_MPC = t_new - t_old;

  // if (vol_flux_conversion_){
  //     vol_flux = S_next_->GetFieldData(flux_key_, passwd_)->ViewComponent("face", true);
  //   ComputeVolumeDarcyFlux(S_next_->GetFieldData(darcy_flux_key_)->ViewComponent("face", true),
  //                          S_next_->GetFieldData(molar_density_key_)->ViewComponent("cell", true), vol_flux);
  // }

  flux = S_next_->GetFieldData(flux_key_)->ViewComponent("face", true);

  ws_ = S_next_->GetFieldData(saturation_key_)->ViewComponent("cell", false);
  mol_dens_ = S_next_->GetFieldData(molar_density_key_)->ViewComponent("cell", false);

  // We use original tcc and make a copy of it later if needed.
  tcc = S_->GetFieldData(tcc_key_, passwd_);
  Epetra_MultiVector& tcc_prev = *tcc->ViewComponent("cell");


  for (int c = 0; c < ncells_owned; c++) {
    double vol_phi_ws_den;
    vol_phi_ws_den = mesh_->cell_volume(c) * (*phi_)[0][c] * (*ws_prev_)[0][c] * (*mol_dens_prev_)[0][c];
    for (int i=0; i<num_aqueous + num_gaseous; i++){
      mass_solutes_stepstart_[i] = tcc_prev[i][c] * vol_phi_ws_den;
    }
  }


  // calculate stable time step    
  double dt_shift = 0.0, dt_global = dt_MPC;
  double time = S_->intermediate_time();
  if (time >= 0.0) { 
    t_physics_ = time;
    dt_shift = time - S_->initial_time();
    dt_global = S_->final_time() - S_->initial_time();
  }

  StableTimeStep();
  double dt_original = dt_;  // advance routines override dt_
  int interpolate_ws = (dt_ < dt_global) ? 1 : 0;

  //if ((t_old > S_->initial_time())||(t_new < S_->final_time())) interpolate_ws = 1;


  // start subcycling
  double dt_sum = 0.0;
  double dt_cycle;
  if (interpolate_ws) {        
    dt_cycle = std::min(dt_original, dt_MPC);
    InterpolateCellVector(*ws_prev_, *ws_, dt_shift, dt_global, *ws_subcycle_start);
    InterpolateCellVector(*mol_dens_prev_, *mol_dens_, dt_shift, dt_global, *mol_dens_subcycle_start);
  } else {
    dt_cycle = dt_MPC;
    ws_start = ws_prev_;
    ws_end = ws_;
    mol_dens_start = mol_dens_prev_;
    mol_dens_end = mol_dens_;
  }

  int ncycles = 0, swap = 1;
  while (dt_sum < dt_MPC) {
    // update boundary conditions
    time = t_physics_ + dt_cycle / 2;
    for (int i = 0; i < bcs_.size(); i++){
      bcs_[i]->Compute(time, time);
    }
    
    double dt_try = dt_MPC - dt_sum;
    double tol = 1e-14 * (dt_try + dt_original); 
    bool final_cycle = false;
    if (dt_try >= 2 * dt_original) {
      dt_cycle = dt_original;
    } else if (dt_try > dt_original + tol) { 
      dt_cycle = dt_try / 2; 
    } else {
      dt_cycle = dt_try;
      final_cycle = true;
    }

    t_physics_ += dt_cycle;
    dt_sum += dt_cycle;

    if (interpolate_ws) {
      if (swap) {  // Initial water saturation is in 'start'.
        ws_start = ws_subcycle_start;
        ws_end = ws_subcycle_end;
        mol_dens_start = mol_dens_subcycle_start;
        mol_dens_end = mol_dens_subcycle_end;
                
        double dt_int = dt_sum + dt_shift;
        InterpolateCellVector(*ws_prev_, *ws_, dt_int, dt_global, *ws_subcycle_end);
        InterpolateCellVector(*mol_dens_prev_, *mol_dens_, dt_int, dt_global, *mol_dens_subcycle_end);
      } else {  // Initial water saturation is in 'end'.
        ws_start = ws_subcycle_end;
        ws_end = ws_subcycle_start;
        mol_dens_start = mol_dens_subcycle_end;
        mol_dens_end = mol_dens_subcycle_start;

        double dt_int = dt_sum + dt_shift;
        InterpolateCellVector(*ws_prev_, *ws_, dt_int, dt_global, *ws_subcycle_start);
        InterpolateCellVector(*mol_dens_prev_, *mol_dens_, dt_int, dt_global, *mol_dens_subcycle_start);
      }
      swap = 1 - swap;
    }

  // if (domain_name_ == "surface"){
  //   std::cout<<"ws "<<(*ws_)[0][49]<<" ws_prev_ "<<(*ws_prev_)[0][49]<<" ws_end "<<(*ws_end)[0][49]<<
  //     " ws_start "<<(*ws_start)[0][49]<<"\n";
  // }


    if (spatial_disc_order == 1) {  // temporary solution (lipnikov@lanl.gov)
      AdvanceDonorUpwind(dt_cycle);
    } else if (spatial_disc_order == 2 && temporal_disc_order == 1) {
      AdvanceSecondOrderUpwindRK1(dt_cycle);
    } else if (spatial_disc_order == 2 && temporal_disc_order == 2) {
      AdvanceSecondOrderUpwindRK2(dt_cycle);
    }

    // add multiscale model
    if (multiscale_porosity_) {
      double t_int1 = t_old + dt_sum - dt_cycle;
      double t_int2 = t_old + dt_sum;
      AddMultiscalePorosity_(t_old, t_new, t_int1, t_int2);
    }

    if (! final_cycle) {  // rotate concentrations (we need new memory for tcc)
      tcc = Teuchos::RCP<CompositeVector>(new CompositeVector(*tcc_tmp));
    }

    ncycles++;
  }

  dt_ = dt_original;  // restore the original time step (just in case)

  Epetra_MultiVector& tcc_next = *tcc_tmp->ViewComponent("cell", false);

  Advance_Dispersion_Diffusion(t_old, t_new);

  // optional Henry Law for the case of gas diffusion
  if (henry_law_) {
    MakeAirWaterPartitioning_();
  }

  // statistics output
  nsubcycles = ncycles;
  if (vo_->getVerbLevel() >= Teuchos::VERB_MEDIUM) {
    Teuchos::OSTab tab = vo_->getOSTab();
    *vo_->os() << ncycles << " sub-cycles, dt_stable=" << units_.OutputTime(dt_original)
               << " [sec]  dt_MPC=" << units_.OutputTime(dt_MPC) << " [sec]" << std::endl;

    VV_PrintSoluteExtrema(tcc_next, dt_MPC);
  }

  // if (domain_name_ == "surface") {
  // //   Key surface_tcc_key = "surface-total_component_concentration";
  // //   Teuchos::RCP<const Field> fld = S_->GetField("surface-total_component_concentration");
  // //   Teuchos::RCP<const Field> copy = fld->GetCopy("subcycling");
  // //   Teuchos::RCP<const CompositeVector> surf = copy->GetFieldData();
  // //   const Epetra_MultiVector& surf_c = *surf->ViewComponent("cell",false);
   
  // //   std::cout<<"End Advance\n";
  // //   std::cout<<"S_"<<S_<<"\n";
  // //   //std::cout<<surf_c<<"\n";
  //   for (int i=35;i<50;i++)
  //     std::cout<<(*tcc_tmp->ViewComponent("cell",false))[0][i]<<" ws "<<
  //       (*ws_end)[0][i]<<"\n";
  // }

  return failed;
}


void Transport_PK_ATS :: Advance_Dispersion_Diffusion(double t_old, double t_new) {

  double dt_MPC = t_new - t_old;
  // We define tracer as the species #0 as calculate some statistics.
  Epetra_MultiVector& tcc_next = *tcc_tmp->ViewComponent("cell", false);
  Epetra_MultiVector& tcc_prev = *tcc->ViewComponent("cell");
  int num_components = tcc_prev.NumVectors();

  bool flag_diffusion(false);
  for (int i = 0; i < 2; i++) {
    if (diffusion_phase_[i] != Teuchos::null) {
      if (diffusion_phase_[i]->values().size() != 0) flag_diffusion = true;
    }
  }
  if (flag_diffusion) {
    // no molecular diffusion if all tortuosities are zero.
    double tau(0.0);
    for (int i = 0; i < mat_properties_.size(); i++) {
      tau += mat_properties_[i]->tau[0] + mat_properties_[i]->tau[1];
    }
    if (tau == 0.0) flag_diffusion = false;
  }

  if (flag_dispersion_ || flag_diffusion) {
    Teuchos::ParameterList& op_list = 
        tp_list_->sublist("operators").sublist("diffusion operator").sublist("matrix");

    // default boundary conditions (none inside domain and Neumann on its boundary)
    std::vector<int> bc_model(nfaces_wghost, Operators::OPERATOR_BC_NONE);
    std::vector<double> bc_value(nfaces_wghost, 0.0);
    std::vector<double> bc_mixed;
    PopulateBoundaryData(bc_model, bc_value, -1);

    Teuchos::RCP<Operators::BCs> bc_dummy = 
        Teuchos::rcp(new Operators::BCs(Operators::OPERATOR_BC_TYPE_FACE, bc_model, bc_value, bc_mixed));

    Operators::OperatorDiffusionFactory opfactory;
    Teuchos::RCP<Operators::OperatorDiffusion> op1 = opfactory.Create(op_list, mesh_, bc_dummy);
    op1->SetBCs(bc_dummy, bc_dummy);
    Teuchos::RCP<Operators::Operator> op = op1->global_operator();
    Teuchos::RCP<Operators::OperatorAccumulation> op2 =
        Teuchos::rcp(new Operators::OperatorAccumulation(AmanziMesh::CELL, op));

    const CompositeVectorSpace& cvs = op1->global_operator()->DomainMap();
    CompositeVector sol(cvs), factor(cvs), factor0(cvs), source(cvs), zero(cvs);
    zero.PutScalar(0.0);
  
    // instantiale solver
    AmanziSolvers::LinearOperatorFactory<Operators::Operator, CompositeVector, CompositeVectorSpace> sfactory;
    Teuchos::RCP<AmanziSolvers::LinearOperator<Operators::Operator, CompositeVector, CompositeVectorSpace> >
        solver = sfactory.Create(dispersion_solver, *linear_solver_list_, op);

    solver->add_criteria(AmanziSolvers::LIN_SOLVER_MAKE_ONE_ITERATION);  // Make at least one iteration

    // populate the dispersion operator (if any)
    if (flag_dispersion_) {
      CalculateDispersionTensor_(*flux, *phi_, *ws_, *mol_dens_);
    }

    int phase, num_itrs(0);
    bool flag_op1(true);
    double md_change, md_old(0.0), md_new, residual(0.0);

    // Disperse and diffuse aqueous components
    for (int i = 0; i < num_aqueous; i++) {
      FindDiffusionValue(component_names_[i], &md_new, &phase);
      md_change = md_new - md_old;
      md_old = md_new;

      if (md_change != 0.0) {
        CalculateDiffusionTensor_(md_change, phase, *phi_, *ws_, *mol_dens_);
        flag_op1 = true;
      }

      // set initial guess
      Epetra_MultiVector& sol_cell = *sol.ViewComponent("cell");
      for (int c = 0; c < ncells_owned; c++) {
        sol_cell[0][c] = tcc_next[i][c];
      }
      if (sol.HasComponent("face")) {
        sol.ViewComponent("face")->PutScalar(0.0);
      }

      if (flag_op1) {
        op->Init();
        Teuchos::RCP<std::vector<WhetStone::Tensor> > Dptr = Teuchos::rcpFromRef(D_);
        op1->Setup(Dptr, Teuchos::null, Teuchos::null);
        op1->UpdateMatrices(Teuchos::null, Teuchos::null);

        // add accumulation term
        Epetra_MultiVector& fac = *factor.ViewComponent("cell");
        for (int c = 0; c < ncells_owned; c++) {
          fac[0][c] = (*phi_)[0][c] * (*ws_)[0][c] * (*mol_dens_)[0][c];
        }
        op2->AddAccumulationTerm(sol, factor, dt_MPC, "cell");
 
        op1->ApplyBCs(true, true);
        op->SymbolicAssembleMatrix();
        op->AssembleMatrix();
        op->InitPreconditioner(dispersion_preconditioner, *preconditioner_list_);
      } else {
        Epetra_MultiVector& rhs_cell = *op->rhs()->ViewComponent("cell");
        for (int c = 0; c < ncells_owned; c++) {
          double tmp = mesh_->cell_volume(c) * (*ws_)[0][c] * (*phi_)[0][c] * (*mol_dens_)[0][c]/ dt_MPC;
          rhs_cell[0][c] = tcc_next[i][c] * tmp;
        }
      }
  
      CompositeVector& rhs = *op->rhs();
      int ierr = solver->ApplyInverse(rhs, sol);

      if (ierr < 0) {
        Errors::Message msg;
        msg = solver->DecodeErrorCode(ierr);
        Exceptions::amanzi_throw(msg);
      }

      residual += solver->residual();
      num_itrs += solver->num_itrs();

      for (int c = 0; c < ncells_owned; c++) {
        tcc_next[i][c] = sol_cell[0][c];
      }
    }

    // Diffuse gaseous components. We ignore dispersion 
    // tensor (D is reset). Inactive cells (s[c] = 1 and D_[c] = 0) 
    // are treated with a hack of the accumulation term.
    D_.clear();
    md_old = 0.0;
    for (int i = num_aqueous; i < num_components; i++) {
      FindDiffusionValue(component_names_[i], &md_new, &phase);
      md_change = md_new - md_old;
      md_old = md_new;

      if (md_change != 0.0 || i == num_aqueous) {
        CalculateDiffusionTensor_(md_change, phase, *phi_, *ws_, *mol_dens_);
      }

      // set initial guess
      Epetra_MultiVector& sol_cell = *sol.ViewComponent("cell");
      for (int c = 0; c < ncells_owned; c++) {
        sol_cell[0][c] = tcc_next[i][c];
      }
      if (sol.HasComponent("face")) {
        sol.ViewComponent("face")->PutScalar(0.0);
      }

      op->Init();
      Teuchos::RCP<std::vector<WhetStone::Tensor> > Dptr = Teuchos::rcpFromRef(D_);
      op1->Setup(Dptr, Teuchos::null, Teuchos::null);
      op1->UpdateMatrices(Teuchos::null, Teuchos::null);

      // add boundary conditions and sources for gaseous components
      PopulateBoundaryData(bc_model, bc_value, i);

      Epetra_MultiVector& rhs_cell = *op->rhs()->ViewComponent("cell");
      ComputeAddSourceTerms(t_new, 1.0, rhs_cell, i, i);
      op1->ApplyBCs(true, true);

      // add accumulation term
      Epetra_MultiVector& fac1 = *factor.ViewComponent("cell");
      Epetra_MultiVector& fac0 = *factor0.ViewComponent("cell");

      for (int c = 0; c < ncells_owned; c++) {
        fac1[0][c] = (*phi_)[0][c] * (1.0 - (*ws_)[0][c]) * (*mol_dens_)[0][c];
        fac0[0][c] = (*phi_)[0][c] * (1.0 - (*ws_prev_)[0][c]) * (*mol_dens_prev_)[0][c];
        if ((*ws_)[0][c] == 1.0) fac1[0][c] = 1.0 * (*mol_dens_)[0][c];  // hack so far
      }
      op2->AddAccumulationTerm(sol, factor0, factor, dt_MPC, "cell");
 
      op->SymbolicAssembleMatrix();
      op->AssembleMatrix();
      op->InitPreconditioner(dispersion_preconditioner, *preconditioner_list_);
  
      CompositeVector& rhs = *op->rhs();
      int ierr = solver->ApplyInverse(rhs, sol);

      if (ierr < 0) {
        Errors::Message msg;
        msg = solver->DecodeErrorCode(ierr);
        Exceptions::amanzi_throw(msg);
      }

      residual += solver->residual();
      num_itrs += solver->num_itrs();

      for (int c = 0; c < ncells_owned; c++) {
        tcc_next[i][c] = sol_cell[0][c];
      }
    }

    if (vo_->getVerbLevel() >= Teuchos::VERB_MEDIUM) {
      Teuchos::OSTab tab = vo_->getOSTab();
      *vo_->os() << "dispersion solver (" << solver->name() 
                 << ") ||r||=" << residual / num_components
                 << " itrs=" << num_itrs / num_components << std::endl;
    }
  }

}





/* ******************************************************************* 
* Add multiscale porosity model on sub interval [t_int1, t_int2]:
*   d(VWC_f)/dt -= G_s, d(VWC_m) = G_s 
*   G_s = G_w C^* + omega_s (C_f - C_m).
******************************************************************* */
void Transport_PK_ATS::AddMultiscalePorosity_(
    double t_old, double t_new, double t_int1, double t_int2)
{
  const Epetra_MultiVector& tcc_prev = *tcc->ViewComponent("cell");
  Epetra_MultiVector& tcc = *tcc_tmp->ViewComponent("cell");
  Epetra_MultiVector& tcc_matrix = 
     *S_->GetFieldData(tcc_matrix_key_, passwd_)->ViewComponent("cell");

  const Epetra_MultiVector& wcf_prev = *S_next_->GetFieldData("prev_water_content")->ViewComponent("cell");
  const Epetra_MultiVector& wcf = *S_next_->GetFieldData("water_content")->ViewComponent("cell");

  const Epetra_MultiVector& wcm_prev = *S_next_->GetFieldData("prev_water_content_matrix")->ViewComponent("cell");
  const Epetra_MultiVector& wcm = *S_next_->GetFieldData("water_content_matrix")->ViewComponent("cell");

  double flux_solute, flux_liquid, f1, f2, f3;
  std::vector<AmanziMesh::Entity_ID> block;

  double dtg, dts, t1, t2, wcm0, wcm1, wcf0, wcf1,tmp0, tmp1, a, b;
  dtg = t_new - t_old;
  dts = t_int2 - t_int1;
  t1 = t_int1 - t_old;
  t2 = t_int2 - t_old;

  for (int c = 0; c < ncells_owned; ++c) {
    wcm0 = wcm_prev[0][c];
    wcm1 = wcm[0][c];
    flux_liquid = (wcm1 - wcm0) / dtg;
  
    wcf0 = wcf_prev[0][c];
    wcf1 = wcf[0][c];
  
    a = t2 / dtg;
    tmp1 = a * wcf1 + (1.0 - a) * wcf0;
    f1 = dts / tmp1;

    b = t1 / dtg;
    tmp0 = b * wcm1 + (1.0 - b) * wcm0;
    tmp1 = a * wcm1 + (1.0 - a) * wcm0;

    f2 = dts / tmp1;
    f3 = tmp0 / tmp1;

    for (int i = 0; i < num_aqueous; ++i) {
      flux_solute = msp_->second[(*msp_->first)[c]]->ComputeSoluteFlux(
          flux_liquid, tcc_prev[i][c], tcc_matrix[i][c]);
      tcc[i][c] -= flux_solute * f1;
      tcc_matrix[i][c] = tcc_matrix[i][c] * f3 + flux_solute * f2;
    }
  }
}


/* ******************************************************************* 
* Copy the advected tcc field to the state.
******************************************************************* */
void Transport_PK_ATS::CommitStep(double t_old, double t_new, const Teuchos::RCP<State>& S)
{
  Teuchos::RCP<CompositeVector> tcc;
  tcc = S->GetFieldData(tcc_key_, passwd_);
  *tcc = *tcc_tmp;
  InitializeFieldFromField_(prev_saturation_key_, saturation_key_, S.ptr(), false, true);

  // Copy to S_ as well
  tcc = S_->GetFieldData(tcc_key_, passwd_);
  *tcc = *tcc_tmp;
}


/* ******************************************************************* 
 * A simple first-order transport method 
 ****************************************************************** */
void Transport_PK_ATS::AdvanceDonorUpwind(double dt_cycle)
{
  dt_ = dt_cycle;  // overwrite the maximum stable transport step
  mass_solutes_source_.assign(num_aqueous + num_gaseous, 0.0);
  mass_solutes_bc_.assign(num_aqueous + num_gaseous, 0.0);

  // populating next state of concentrations
  tcc->ScatterMasterToGhosted("cell");
  Epetra_MultiVector& tcc_prev = *tcc->ViewComponent("cell", true);
  Epetra_MultiVector& tcc_next = *tcc_tmp->ViewComponent("cell", true);

  //if (domain_name_=="surface") std::cout<<"DonorUpwind start\n"<<tcc_prev<<"\n";


  // prepare conservative state in master and slave cells
  double vol_phi_ws_den, tcc_flux;
  double mass_start = 0., tmp1;

  // We advect only aqueous components.
  int num_advect = num_aqueous;

  for (int c = 0; c < ncells_owned; c++) {
    vol_phi_ws_den = mesh_->cell_volume(c) * (*phi_)[0][c] * (*ws_start)[0][c] * (*mol_dens_start)[0][c];
     // if (domain_name_=="surface") 
    for (int i = 0; i < num_advect; i++){
      // if (c<5) std::cout<<c<<" vol "<<mesh_->cell_volume(c)<<" phi "<<(*phi_)[0][c]<<" ws0 "<<(*ws_start)[0][c]<<" ws1 "<< (*ws_end)[0][c]<<" den "<<(*mol_dens_start)[0][c]<<" tcc "<<tcc_prev[i][c]<<"\n";

      (*conserve_qty_)[i][c] = tcc_prev[i][c] * vol_phi_ws_den;   
      // if (c<5) std::cout<<c<<" "<<vol_phi_ws_den<<" "<<tcc_prev[i][c]<<" "<<(*conserve_qty_)[i][c]<<"\n";
      mass_start += (*conserve_qty_)[i][c];
    }
  }

  //mass_start /= units_.concentration_factor();

  if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH){
    if (domain_name_ == "surface") std::cout<<"Overland mass start "<<mass_start<<"\n";
    else std::cout<<"Subsurface mass start "<<mass_start<<"\n";
  }


  // advance all components at once
  for (int f = 0; f < nfaces_wghost; f++) {  // loop over master and slave faces
    int c1 = (*upwind_cell_)[f];
    int c2 = (*downwind_cell_)[f];

    double u = fabs((*flux)[0][f]);

    if (c1 >=0 && c1 < ncells_owned && c2 >= 0 && c2 < ncells_owned) {
      for (int i = 0; i < num_advect; i++) {
        tcc_flux = dt_ * u * tcc_prev[i][c1];
        (*conserve_qty_)[i][c1] -= tcc_flux;
        (*conserve_qty_)[i][c2] += tcc_flux;
      }

    } else if (c1 >=0 && c1 < ncells_owned && (c2 >= ncells_owned || c2 < 0)) {
      for (int i = 0; i < num_advect; i++) {
        tcc_flux = dt_ * u * tcc_prev[i][c1];
        (*conserve_qty_)[i][c1] -= tcc_flux;
        if (c2 < 0) mass_solutes_bc_[i] -= tcc_flux;
      }

    } else if (c1 >= ncells_owned && c2 >= 0 && c2 < ncells_owned) {
      for (int i = 0; i < num_advect; i++) {
        tcc_flux = dt_ * u * tcc_prev[i][c1];
        (*conserve_qty_)[i][c2] += tcc_flux;
      }
    }
  }

  // loop over exterior boundary sets
  for (int m = 0; m < bcs_.size(); m++) {
    std::vector<int>& tcc_index = bcs_[m]->tcc_index();
    int ncomp = tcc_index.size();

    for (auto it = bcs_[m]->begin(); it != bcs_[m]->end(); ++it) {
      int f = it->first;
      std::vector<double>& values = it->second;
      int c2 = (*downwind_cell_)[f];
      if (c2 >= 0) {
        double u = fabs((*flux)[0][f]);
        for (int i = 0; i < ncomp; i++) {
          int k = tcc_index[i];
          if (k < num_advect) {
            tcc_flux = dt_ * u * values[i];
            //if (tcc_flux > 0) std::cout <<domain_name_<<" "<<"from BC cell "<<c2<<" flux "<< u<<" dt "<<dt_<<" value "<<values[i]<<" + "<<tcc_flux<<"\n";
            (*conserve_qty_)[k][c2] += tcc_flux;
            mass_solutes_bc_[k] += tcc_flux;
          }
        }
      } 
    }
  }



  // if (domain_name_ == "surface"){
  //   std::cout<<"Before ComputeAddSourceTerms\n";
  //   std::cout<<(*conserve_qty_)<<"\n";
  // }

  // process external sources
  if (srcs_.size() != 0) {
    double time = t_physics_;
    ComputeAddSourceTerms(time, dt_, *conserve_qty_, 0, num_advect - 1);
  }

  // if (domain_name_ == "surface"){
  //   std::cout<<"After ComputeAddSourceTerms\n";
  //   std::cout<<*conserve_qty_<<"\n";
  // }
  //if (domain_name_ == "surface") std::cout<<"ws_end\n"<<*ws_end<<"\n";

  



  // recover concentration from new conservative state
  for (int c = 0; c < ncells_owned; c++) {
    vol_phi_ws_den = mesh_->cell_volume(c) * (*phi_)[0][c] * (*ws_end)[0][c] * (*mol_dens_end)[0][c];
    for (int i = 0; i < num_advect; i++) {
      if (vol_phi_ws_den > 0 ) tcc_next[i][c] = (*conserve_qty_)[i][c] / vol_phi_ws_den;
      else  {
        tcc_next[i][c] = 0.;
      }
      // if (c<5) std::cout<<c<<" vol "<<mesh_->cell_volume(c)<<" phi "<<(*phi_)[0][c]<<" ws0 "<<(*ws_start)[0][c]<<" ws1 "<< (*ws_end)[0][c]<<" den "<<(*mol_dens_end)[0][c]<<" tcc "<<tcc_next[i][c]<<"\n";
      // if (c<5) std::cout<<c<<" "<<vol_phi_ws_den<<" "<<tcc_next[i][c]<<" "<<(*conserve_qty_)[i][c]<<"\n";
    }
  }

  double mass_final = 0;
  for (int c = 0; c < ncells_owned; c++) {
    vol_phi_ws_den = mesh_->cell_volume(c) * (*phi_)[0][c] * (*ws_end)[0][c] * (*mol_dens_end)[0][c];
    for (int i = 0; i < num_advect; i++){        
      mass_final += tcc_next[i][c]*vol_phi_ws_den;
    }    
  }
  //mass_final /= units_.concentration_factor();

  tmp1 = mass_final;
  mesh_->get_comm()->SumAll(&tmp1, &mass_final, 1);
  tmp1 = mass_start;
  mesh_->get_comm()->SumAll(&tmp1, &mass_start, 1);



  //if (domain_name_ == "surface") std::cout<<"DonorUpwind end\n"<<tcc_next<<"\n";


  // update mass balance
  for (int i = 0; i < mass_solutes_exact_.size(); i++) {
    mass_solutes_exact_[i] += mass_solutes_source_[i] * dt_;
    if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH){
      tmp1 = mass_solutes_bc_[i];
      mesh_->get_comm()->SumAll(&tmp1, &mass_solutes_bc_[i], 1);
      std::cout<<"*****************\n";
      if (domain_name_ == "surface") std::cout<<"Overland mass BC "<<mass_solutes_bc_[i]<<"\n";
      else std::cout<<"Subsurface mass BC "<<mass_solutes_bc_[i]<<"\n";
      tmp1 = mass_solutes_source_[i];
      mesh_->get_comm()->SumAll(&tmp1, &mass_solutes_source_[i], 1);
      if (domain_name_ == "surface") std::cout<<"Overland mass_solutes source "<<mass_solutes_source_[i]*dt_<<"\n";
      else std::cout<<"Subsurface mass_solutes source "<<mass_solutes_source_[i]*dt_<<"\n";
      std::cout<<"*****************\n";
    }
  }

  if (internal_tests) {
    VV_CheckGEDproperty(*tcc_tmp->ViewComponent("cell"));
  }

  if (vo_->getVerbLevel() >= Teuchos::VERB_HIGH){
    if (domain_name_ == "surface") std::cout<<"Overland mass final "<<mass_final<<"\n";
    else std::cout<<"Subsurface mass final "<<mass_final<<"\n";
  }

  //if (abs(mass_final - (mass_start + mass_solutes_bc_[0] + mass_solutes_source_[0]*dt_) )/mass_final > 1e-4) exit(-1);
  
}


/* ******************************************************************* 
 * We have to advance each component independently due to different
 * reconstructions. We use tcc when only owned data are needed and 
 * tcc_next when owned and ghost data. This is a special routine for 
 * transient flow and uses first-order time integrator. 
 ****************************************************************** */
void Transport_PK_ATS::AdvanceSecondOrderUpwindRK1(double dt_cycle)
{
  dt_ = dt_cycle;  // overwrite the maximum stable transport step
  mass_solutes_source_.assign(num_aqueous + num_gaseous, 0.0);

  // work memory
  const Epetra_Map& cmap_wghost = mesh_->cell_map(true);
  Epetra_Vector f_component(cmap_wghost);

  // distribute vector of concentrations
  S_->GetFieldData(tcc_key_)->ScatterMasterToGhosted("cell");
  Epetra_MultiVector& tcc_prev = *tcc->ViewComponent("cell", true);
  Epetra_MultiVector& tcc_next = *tcc_tmp->ViewComponent("cell", true);

  // We advect only aqueous components.
  int num_advect = num_aqueous;

  for (int i = 0; i < num_advect; i++) {
    current_component_ = i;  // needed by BJ 

    double T = t_physics_;
    Epetra_Vector*& component = tcc_prev(i);
    Functional(T, *component, f_component);

    double ws_ratio;
    for (int c = 0; c < ncells_owned; c++) {
      ws_ratio = (*ws_start)[0][c] / (*ws_end)[0][c];
      tcc_next[i][c] = (tcc_prev[i][c] + dt_ * f_component[c]) * ws_ratio;
    }
  }

  // update mass balance
  for (int i = 0; i < num_aqueous + num_gaseous; i++) {
    mass_solutes_exact_[i] += mass_solutes_source_[i] * dt_;
  }

  if (internal_tests) {
    VV_CheckGEDproperty(*tcc_tmp->ViewComponent("cell"));
  }
}


/* ******************************************************************* 
 * We have to advance each component independently due to different
 * reconstructions. This is a special routine for transient flow and 
 * uses second-order predictor-corrector time integrator. 
 ****************************************************************** */
void Transport_PK_ATS::AdvanceSecondOrderUpwindRK2(double dt_cycle)
{
  dt_ = dt_cycle;  // overwrite the maximum stable transport step
  mass_solutes_source_.assign(num_aqueous + num_gaseous, 0.0);

  // work memory
  const Epetra_Map& cmap_wghost = mesh_->cell_map(true);
  Epetra_Vector f_component(cmap_wghost);

  // distribute old vector of concentrations
  S_->GetFieldData(tcc_key_)->ScatterMasterToGhosted("cell");
  Epetra_MultiVector& tcc_prev = *tcc->ViewComponent("cell", true);
  Epetra_MultiVector& tcc_next = *tcc_tmp->ViewComponent("cell", true);

  Epetra_Vector ws_ratio(Copy, *ws_start, 0);
  for (int c = 0; c < ncells_owned; c++) ws_ratio[c] /= (*ws_end)[0][c];

  // We advect only aqueous components.
  int num_advect = num_aqueous;

  // predictor step
  for (int i = 0; i < num_advect; i++) {
    current_component_ = i;  // needed by BJ 

    double T = t_physics_;
    Epetra_Vector*& component = tcc_prev(i);
    Functional(T, *component, f_component);

    for (int c = 0; c < ncells_owned; c++) {
      tcc_next[i][c] = (tcc_prev[i][c] + dt_ * f_component[c]) * ws_ratio[c];
    }
  }

  tcc_tmp->ScatterMasterToGhosted("cell");

  // corrector step
  for (int i = 0; i < num_advect; i++) {
    current_component_ = i;  // needed by BJ 

    double T = t_physics_;
    Epetra_Vector*& component = tcc_next(i);
    Functional(T, *component, f_component);

    for (int c = 0; c < ncells_owned; c++) {
      double value = (tcc_prev[i][c] + dt_ * f_component[c]) * ws_ratio[c];
      tcc_next[i][c] = (tcc_next[i][c] + value) / 2;
    }
  }

  // update mass balance
  for (int i = 0; i < num_aqueous + num_gaseous; i++) {
    mass_solutes_exact_[i] += mass_solutes_source_[i] * dt_ / 2;
  }

  if (internal_tests) {
    VV_CheckGEDproperty(*tcc_tmp->ViewComponent("cell"));
  }
}


/* ******************************************************************* 
 * We have to advance each component independently due to different
 * reconstructions. We use tcc when only owned data are needed and 
 * tcc_next when owned and ghost data.
 *
 * Data flow: loop over components and apply the generic RK2 method.
 * The generic means that saturation is constant during time step. 
 ****************************************************************** */
/*
void Transport_PK_ATS::AdvanceSecondOrderUpwindGeneric(double dt_cycle)
{
  dt_ = dt_cycle;  // overwrite the maximum stable transport step

  Teuchos::RCP<Epetra_MultiVector> tcc = TS->total_component_concentration();
  Teuchos::RCP<Epetra_MultiVector> tcc_next = TS_nextBIG->total_component_concentration();

  // define time integration method
  Explicit_TI::RK::method_t ti_method = Explicit_TI::RK::forward_euler;
  if (temporal_disc_order == 2) {
    ti_method = Explicit_TI::RK::heun_euler;
  }
  Explicit_TI::RK TVD_RK(*this, ti_method, *component_);

  // We advect only aqueous components.
  // int ncomponents = tcc_next.NumVectors();
  int ncomponents = num_aqueous;

  for (int i = 0; i < ncomponents; i++) {
    current_component_ = i;  // it is needed in BJ called inside RK:fun

    Epetra_Vector*& tcc_component = (*tcc)(i);
    TS_nextBIG->CopyMasterCell2GhostCell(*tcc_component, *component_);

    double T = 0.0;  // requires fixes (lipnikov@lanl.gov)
    TVD_RK.step(T, dt_, *component_, *component_next_);

    for (int c = 0; c < ncells_owned; c++) (*tcc_next)[i][c] = (*component_next_)[c];
  }
}
*/


/* ******************************************************************
* Computes source and sink terms and adds them to vector tcc.
* Returns mass rate for the tracer.
* The routine treats two cases of tcc with one and all components.
****************************************************************** */
void Transport_PK_ATS::ComputeAddSourceTerms(double tp, double dtp, 
                                         Epetra_MultiVector& tcc, int n0, int n1)
{
  int num_vectors = tcc.NumVectors();
  int nsrcs = srcs_.size();

  //std::cout<<"Time "<<tp<<"\n";
  for (int m = 0; m < nsrcs; m++) {
    double t0 = tp - dtp;
    srcs_[m]->Compute(t0, tp); 

    std::vector<int> tcc_index = srcs_[m]->tcc_index();
    for (auto it = srcs_[m]->begin(); it != srcs_[m]->end(); ++it) {
      int c = it->first;
      std::vector<double>& values = it->second;

      //std::cout<<c<<": "<<values[0]<<"\n";

      for (int k = 0; k < tcc_index.size(); ++k) {
        int i = tcc_index[k];
        if (i < n0 || i > n1) continue;

        int imap = i;
        if (num_vectors == 1) imap = 0;

        double value;   
        if (srcs_[m]->name() == "domain coupling") {
          value = values[k];
        } else {
          value =  mesh_->cell_volume(c) * values[k];
          // correction for non-SI concentration units
          // if (srcs_[m]->name() == "volume" || srcs_[m]->name() == "weight")
          //     value /= units_.concentration_factor();
        }

        tcc[imap][c] += dtp * value;
        mass_solutes_source_[i] += value;

         // if (value != 0) std::cout<<"Source name "<<srcs_[m]->name()<<" from Source cell "<<c
         //                         <<" dt "<<dtp<<"  + "<<dtp * value<<" "<<values[k]<<"\n";
      }
    }
  }

}

void Transport_PK_ATS::Sinks2TotalOutFlux(Epetra_MultiVector& tcc,
                                          std::vector<double>& total_outflux, int n0, int n1){

  std::vector<double> sink_add(ncells_wghost, 0.0);
  //Assumption that there is only one sink per component per cell
  double t0 = S_->intermediate_time();
  int num_vectors = tcc.NumVectors();
  int nsrcs = srcs_.size();

  for (int m = 0; m < nsrcs; m++) {    
    srcs_[m]->Compute(t0, t0); 
    std::vector<int> index = srcs_[m]->tcc_index();

    for (auto it = srcs_[m]->begin(); it != srcs_[m]->end(); ++it) {
      int c = it->first;
      std::vector<double>& values = it->second;

      double val = 0;
      for (int k = 0; k < index.size(); ++k) {
        int i = index[k];
        if (i < n0 || i > n1) continue;

        int imap = i;
        if (num_vectors == 1) imap = 0;

        if ((values[k] < 0)&&(tcc[imap][c]>0)) {
          if (srcs_[m]->name() == "domain coupling") {
            val = std::max(val, fabs(values[k])/tcc[imap][c]);
          }
          // else if (srcs_[m]->name() == "volume" || srcs_[m]->name() == "weight"){
          //   val = std::max(val, fabs(values[k])* mesh_->cell_volume(c) * units_.concentration_factor());
          // }
          // else {
          //   val = std::max(val, fabs(values[k])* mesh_->cell_volume(c));
          // }
        }                              
      }
      sink_add[c] = std::max(sink_add[c], val);              
    }
  }

  for (int c=0;c<ncells_wghost; c++) total_outflux[c] += sink_add[c];

}



/* *******************************************************************
* Populates operators' boundary data for given component.
* Returns true if at least one face was populated.
******************************************************************* */
bool Transport_PK_ATS::PopulateBoundaryData(
    std::vector<int>& bc_model, std::vector<double>& bc_value, int component)
{
  bool flag = false;

  for (int i = 0; i < bc_model.size(); i++) {
    bc_model[i] = Operators::OPERATOR_BC_NONE;
    bc_value[i] = 0.0;
  }

  AmanziMesh::Entity_ID_List cells;
  for (int f = 0; f < nfaces_wghost; f++) {
    mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
    if (cells.size() == 1) bc_model[f] = Operators::OPERATOR_BC_NEUMANN;
  }

  for (int m = 0; m < bcs_.size(); m++) {
    std::vector<int>& tcc_index = bcs_[m]->tcc_index();
    int ncomp = tcc_index.size();

    for (auto it = bcs_[m]->begin(); it != bcs_[m]->end(); ++it) {
      int f = it->first;
      std::vector<double>& values = it->second;

      for (int i = 0; i < ncomp; i++) {
        int k = tcc_index[i];
        if (k == component) {
          bc_model[f] = Operators::OPERATOR_BC_DIRICHLET;
          bc_value[f] = values[i];
          flag = true;
        }
      }
    }
  }

  return flag;
}


/* *******************************************************************
* Identify flux direction based on orientation of the face normal 
* and sign of the  Darcy velocity.                               
******************************************************************* */
void Transport_PK_ATS::IdentifyUpwindCells()
{
  for (int f = 0; f < nfaces_wghost; f++) {
    (*upwind_cell_)[f] = -1;  // negative value indicates boundary
    (*downwind_cell_)[f] = -1;

    }
  AmanziMesh::Entity_ID_List faces;
  std::vector<int> dirs;

  for (int c = 0; c < ncells_wghost; c++) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);

    for (int i = 0; i < faces.size(); i++) {
      int f = faces[i];
      double tmp = (*flux)[0][f] * dirs[i];
      if (tmp > 0.0) {
        (*upwind_cell_)[f] = c;
      } else if (tmp < 0.0) {
        (*downwind_cell_)[f] = c;
      } else if (dirs[i] > 0) {
        (*upwind_cell_)[f] = c;
      } else {
        (*downwind_cell_)[f] = c;
      }
    }
  }
}

void Transport_PK_ATS::ComputeVolumeDarcyFlux(Teuchos::RCP<const Epetra_MultiVector> flux,
                                              Teuchos::RCP<const Epetra_MultiVector> molar_density,
                                              Teuchos::RCP<Epetra_MultiVector>& vol_darcy_flux){

  int nfaces_wghost = mesh_->num_entities(AmanziMesh::FACE, AmanziMesh::USED);
  AmanziMesh::Entity_ID_List cells;

  for (int f = 0; f < nfaces_wghost ; f++){
    mesh_->face_get_cells(f, AmanziMesh::USED, &cells);
    double n_liq=0.;
    for (int c=0; c<cells.size();c++) n_liq += (*molar_density)[0][c];
    n_liq /= cells.size();
    if (n_liq > 0) (*vol_darcy_flux)[0][f] = (*flux)[0][f]/n_liq;
    else (*vol_darcy_flux)[0][f] = 0.;
  }

}



/* *******************************************************************
* Interpolate linearly in time between two values v0 and v1. The time 
* is measuared relative to value v0; so that v1 is at time dt. The
* interpolated data are at time dt_int.            
******************************************************************* */
void Transport_PK_ATS::InterpolateCellVector(
    const Epetra_MultiVector& v0, const Epetra_MultiVector& v1, 
    double dt_int, double dt, Epetra_MultiVector& v_int) 
{
  double a = dt_int / dt;
  double b = 1.0 - a;
  v_int.Update(b, v0, a, v1, 0.);
}

}  // namespace Transport
}  // namespace Amanzi

