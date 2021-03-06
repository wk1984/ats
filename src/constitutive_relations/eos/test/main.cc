
#include "Teuchos_GlobalMPISession.hpp"

#include "Teuchos_ParameterList.hpp"
#include "Teuchos_RCP.hpp"
#include "Teuchos_XMLParameterListHelpers.hpp"

#include <string>
#include "eos_factory.hh"
#include "eos.hh"

int main( int argc, char *argv[] )
{
  Teuchos::GlobalMPISession mpiSession(&argc, &argv);

  Teuchos::ParameterList parameter_list;
  std::string xmlFileName = "test/eos_test.xml";
  updateParametersFromXmlFile(xmlFileName, &parameter_list);

  Teuchos::ParameterList eos1_plist = parameter_list.sublist("EOS 1");
  Teuchos::ParameterList eos2_plist = parameter_list.sublist("EOS 2");

  Amanzi::Flow::FlowRelations::EOSFactory eosfactory;

  Teuchos::RCP<Amanzi::Flow::FlowRelations::EOS> eos1 = eosfactory.createEOS(eos1_plist);
  Teuchos::RCP<Amanzi::Flow::FlowRelations::EOS> eos2 = eosfactory.createEOS(eos2_plist);
}
