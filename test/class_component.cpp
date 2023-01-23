#include "meld/core/framework_graph.hpp"
#include "meld/model/level_hierarchy.hpp"
#include "meld/model/product_store.hpp"
#include "meld/utilities/debug.hpp"

#include "catch2/catch.hpp"

#include <array>
#include <string>
#include <tuple>

using namespace std::string_literals;
using namespace meld;
using namespace meld::concurrency;

namespace {
  struct A {
    auto no_framework(int num, double temp, std::string const& name) const
    {
      return std::make_tuple(num, temp, name);
    }

    auto no_framework_all_refs(int const& num, double const& temp, std::string const& name) const
    {
      return std::make_tuple(num, temp, name);
    }

    auto no_framework_all_ptrs(int const* num, double const* temp, std::string const* name) const
    {
      return std::make_tuple(*num, *temp, *name);
    }

    auto one_framework_arg(handle<int> num, double temp, std::string const& name) const
    {
      return std::make_tuple(*num, temp, name);
    }

    auto all_framework_args(handle<int> const num,
                            handle<double> const temp,
                            handle<std::string> const name) const
    {
      return std::make_tuple(*num, *temp, *name);
    }
  };

  void verify_results(int number, double temperature, std::string const& name)
  {
    auto const expected = std::make_tuple(3, 98.5, "John");
    CHECK(std::tie(number, temperature, name) == expected);
  }
}

TEST_CASE("Call non-framework functions", "[programming model]")
{
  std::tuple const product_names{react_to("number"), react_to("temperature"), react_to("name")};
  std::array const oproduct_names{"onumber"s, "otemperature"s, "oname"s};

  level_hierarchy org;
  auto factory = org.make_factory({"event"});
  auto store = factory.make();
  store->add_product("number", 3);
  store->add_product("temperature", 98.5);
  store->add_product("name", std::string{"John"});

  framework_graph g{store};
  auto a_component = g.make<A>();
  SECTION("No framework")
  {
    a_component.declare_transform(&A::no_framework)
      .concurrency(unlimited)
      .input(product_names)
      .output(oproduct_names);
  }
  SECTION("No framework, all references")
  {
    a_component.declare_transform(&A::no_framework_all_refs)
      .concurrency(unlimited)
      .input(product_names)
      .output(oproduct_names);
  }
  SECTION("No framework, all pointers")
  {
    a_component.declare_transform(&A::no_framework_all_ptrs)
      .concurrency(unlimited)
      .input(product_names)
      .output(oproduct_names);
  }
  SECTION("One framework argument")
  {
    a_component.declare_transform(&A::one_framework_arg)
      .concurrency(unlimited)
      .input(product_names)
      .output(oproduct_names);
  }
  SECTION("All framework arguments")
  {
    a_component.declare_transform(&A::all_framework_args)
      .concurrency(unlimited)
      .input(product_names)
      .output(oproduct_names);
  }

  // The following is invoked for *each* section above
  g.declare_monitor(verify_results).concurrency(unlimited).input(product_names);

  g.execute("class_component_t.gv");
}
