/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */

/*
  WRM which calls another WRM for saturation but sets 0 rel perm.

  Authors: Ethan Coon (ecoon@lanl.gov)
*/

#include "wrm_zero_relperm.hh"

namespace Amanzi {
namespace Flow {
namespace FlowRelations {

Utils::RegisteredFactory<WRM,WRMZeroRelPerm> WRMZeroRelPerm::factory_("zero rel perm");

} // namespace
} // namespace
} // namespace
