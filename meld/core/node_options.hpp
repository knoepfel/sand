#ifndef meld_core_node_options_hpp
#define meld_core_node_options_hpp

// =======================================================================================
// The facilities provided here will become simpler whenever "deducing this" is available
// in C++23 (which will largely replace CRTP).
// =======================================================================================

#include "meld/configuration.hpp"

#include <optional>
#include <string>
#include <vector>

namespace meld {

  template <typename T>
  class node_options {
  public:
    T& filtered_by(std::vector<std::string> preceding_filters)
    {
      if (!preceding_filters_) {
        preceding_filters_ = std::move(preceding_filters);
      }
      return self();
    }

    T& filtered_by(std::convertible_to<std::string> auto&&... names)
    {
      return filtered_by({std::forward<decltype(names)>(names)...});
    }

  protected:
    explicit node_options(configuration const* config)
    {
      if (!config) {
        return;
      }
      preceding_filters_ = config->get_if_present<std::vector<std::string>>("filtered_by");
    }

    std::vector<std::string> release_preceding_filters()
    {
      return std::move(preceding_filters_).value_or(std::vector<std::string>{});
    }

  private:
    auto& self() { return *static_cast<T*>(this); }
    std::optional<std::vector<std::string>> preceding_filters_{};
  };
}

#endif /* meld_core_node_options_hpp */
