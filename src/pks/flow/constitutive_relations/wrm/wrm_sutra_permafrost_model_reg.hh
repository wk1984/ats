/*
Author: Ethan Coon

Painter's permafrost model with freezing point depression.

 */

#include "wrm.hh"
#include "wrm_sutra_permafrost_model.hh"

namespace Amanzi {
namespace Flow {
namespace FlowRelations {


// registry of method
Utils::RegisteredFactory<WRMPermafrostModel,WRMSutraPermafrostModel> WRMSutraPermafrostModel::factory_("sutra permafrost model");

} // namespace FlowRelations
} // namespace Flow
} // namespace Amanzi
