/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */
/* -------------------------------------------------------------------------

   ATS
   Author: Ethan Coon

   Self-registering factory for IEM implementations.
   ------------------------------------------------------------------------- */

#include <string>
#include "iem_factory.hh"

namespace Amanzi {
namespace Energy {
namespace EnergyRelations {

// method for instantiating IEM implementations
Teuchos::RCP<IEM> IEMFactory::createIEM(Teuchos::ParameterList& plist) {
  std::string iem_typename = plist.get<std::string>("IEM type");
  return Teuchos::rcp(CreateInstance(iem_typename, plist));
};

} // namespace
} // namespace
} // namespace

