#include "meld/module.hpp"
#include "test/plugins/add.hpp"

using namespace meld::concurrency;

// TODO: Option to select which algorithm to run via configuration?

DEFINE_MODULE(m)
{
  m.declare_transform("add", test::add).concurrency(unlimited).input("i", "j").output("sum");
  m.declare_monitor("verify_zero", test::verify).concurrency(unlimited).input("sum", meld::use(0));
}
