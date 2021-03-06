/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */
/* -------------------------------------------------------------------------

   ATS
   Author: Ethan Coon

   Self-registering factory for IEM implementations.
   ------------------------------------------------------------------------- */

#include "iem_factory.hh"

// explicity instantitate the static data of Factory<IEM>
template<> 
Amanzi::Utils::Factory<Amanzi::Energy::EnergyRelations::IEM>::map_type* 
Amanzi::Utils::Factory<Amanzi::Energy::EnergyRelations::IEM>::map_;

