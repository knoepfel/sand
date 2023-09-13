#ifndef meld_core_declared_transform_hpp
#define meld_core_declared_transform_hpp

#include "meld/core/concepts.hpp"
#include "meld/core/detail/port_names.hpp"
#include "meld/core/fwd.hpp"
#include "meld/core/message.hpp"
#include "meld/core/products_consumer.hpp"
#include "meld/core/registrar.hpp"
#include "meld/core/specified_label.hpp"
#include "meld/core/store_counters.hpp"
#include "meld/metaprogramming/type_deduction.hpp"
#include "meld/model/handle.hpp"
#include "meld/model/level_id.hpp"
#include "meld/model/product_store.hpp"

#include "oneapi/tbb/concurrent_hash_map.h"
#include "oneapi/tbb/flow_graph.h"
#include "spdlog/spdlog.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace meld {

  class declared_transform : public products_consumer {
  public:
    declared_transform(std::string name, std::vector<std::string> preceding_filters);
    virtual ~declared_transform();

    virtual tbb::flow::sender<message>& sender() = 0;
    virtual tbb::flow::sender<message>& to_output() = 0;
    virtual std::span<std::string const, std::dynamic_extent> output() const = 0;
  };

  using declared_transform_ptr = std::unique_ptr<declared_transform>;
  using declared_transforms = std::map<std::string, declared_transform_ptr>;

  // =====================================================================================

  template <is_transform_like FT, typename InputArgs>
  class pre_transform {
    static constexpr std::size_t N = std::tuple_size_v<InputArgs>;
    static constexpr std::size_t M = number_output_objects<FT>;
    using function_t = FT;

    template <std::size_t M>
    class total_transform;

  public:
    pre_transform(registrar<declared_transforms> reg,
                  std::string name,
                  std::size_t concurrency,
                  std::vector<std::string> preceding_filters,
                  tbb::flow::graph& g,
                  function_t&& f,
                  InputArgs input_args) :
      name_{std::move(name)},
      concurrency_{concurrency},
      preceding_filters_{std::move(preceding_filters)},
      graph_{g},
      ft_{std::move(f)},
      input_args_{std::move(input_args)},
      product_labels_{detail::port_names(input_args_)},
      reg_{std::move(reg)}
    {
    }

    auto& for_each(std::string const& domain)
    {
      for (auto& [_, allowed_domain] : product_labels_) {
        if (empty(allowed_domain)) {
          allowed_domain = domain;
        }
      }
      return *this;
    }

    template <std::size_t Msize>
    auto& to(std::array<std::string, Msize> output_keys)
    {
      static_assert(
        M == Msize,
        "The number of function parameters is not the same as the number of returned output "
        "objects.");
      reg_.set([this, outputs = std::move(output_keys)] { return create(std::move(outputs)); });
      return *this;
    }

    auto& to(std::convertible_to<std::string> auto&&... ts)
    {
      static_assert(
        M == sizeof...(ts),
        "The number of function parameters is not the same as the number of returned output "
        "objects.");
      return to(std::array<std::string, M>{std::forward<decltype(ts)>(ts)...});
    }

  private:
    declared_transform_ptr create(std::array<std::string, M> outputs)
    {
      return std::make_unique<total_transform<M>>(std::move(name_),
                                                  concurrency_,
                                                  std::move(preceding_filters_),
                                                  graph_,
                                                  std::move(ft_),
                                                  std::move(input_args_),
                                                  std::move(product_labels_),
                                                  std::move(outputs));
    }

    std::string name_;
    std::size_t concurrency_;
    std::vector<std::string> preceding_filters_;
    tbb::flow::graph& graph_;
    function_t ft_;
    InputArgs input_args_;
    std::array<specified_label, N> product_labels_;
    registrar<declared_transforms> reg_;
  };

  // =====================================================================================

  template <is_transform_like FT, typename InputArgs>
  template <std::size_t M>
  class pre_transform<FT, InputArgs>::total_transform :
    public declared_transform,
    private detect_flush_flag {
    using stores_t = tbb::concurrent_hash_map<level_id::hash_type, product_store_ptr>;
    using accessor = stores_t::accessor;
    using const_accessor = stores_t::const_accessor;

  public:
    total_transform(std::string name,
                    std::size_t concurrency,
                    std::vector<std::string> preceding_filters,
                    tbb::flow::graph& g,
                    function_t&& f,
                    InputArgs input,
                    std::array<specified_label, N> product_labels,
                    std::array<std::string, M> output) :
      declared_transform{std::move(name), std::move(preceding_filters)},
      product_labels_{std::move(product_labels)},
      input_{std::move(input)},
      output_{std::move(output)},
      join_{make_join_or_none(g, std::make_index_sequence<N>{})},
      transform_{
        g, concurrency, [this, ft = std::move(f)](messages_t<N> const& messages, auto& output) {
          auto const& msg = most_derived(messages);
          auto const& [store, message_eom, message_id] = std::tie(msg.store, msg.eom, msg.id);
          auto& [stay_in_graph, to_output] = output;
          if (store->is_flush()) {
            flag_accessor fa;
            flag_for(store->id()->hash(), fa).flush_received(msg.original_id);
            // spdlog::debug("Transform {} sending flush message {}/{}", this->name(), message_id, msg.original_id);
            stay_in_graph.try_put(msg);
          }
          else if (accessor a; needs_new(store, message_eom, message_id, stay_in_graph, a)) {
            auto result = call(ft, messages, std::make_index_sequence<N>{});
            products new_products;
            new_products.add_all(output_, result);
            a->second = store->make_continuation(this->name(), std::move(new_products));

            message const new_msg{a->second, msg.eom, message_id};
            stay_in_graph.try_put(new_msg);
            to_output.try_put(new_msg);
            flag_accessor fa;
            flag_for(store->id()->hash(), fa).mark_as_processed();
          }
          auto const id_hash = store->id()->hash();
          if (const_flag_accessor fa; flag_for(id_hash, fa) && fa->second->is_flush()) {
            stores_.erase(id_hash);
            erase_flag(fa);
          }
        }}
    {
      make_edge(join_, transform_);
    }

    ~total_transform()
    {
      if (stores_.size() > 0ull) {
        spdlog::warn("Transform {} has {} cached stores.", name(), stores_.size());
      }
      for (auto const& [hash, store] : stores_) {
        spdlog::debug(" => ID: {} (hash: {})", store->id()->to_string(), hash);
      }
    }

  private:
    tbb::flow::receiver<message>& port_for(specified_label const& product_label) override
    {
      return receiver_for<N>(join_, product_labels_, product_label);
    }

    std::vector<tbb::flow::receiver<message>*> ports() override { return input_ports<N>(join_); }

    tbb::flow::sender<message>& sender() override { return output_port<0>(transform_); }
    tbb::flow::sender<message>& to_output() override { return output_port<1>(transform_); }
    std::span<specified_label const, std::dynamic_extent> input() const override
    {
      return product_labels_;
    }
    std::span<std::string const, std::dynamic_extent> output() const override { return output_; }

    bool needs_new(product_store_const_ptr const& store,
                   end_of_message_ptr const& eom,
                   std::size_t message_id,
                   auto& stay_in_graph,
                   accessor& a)
    {
      if (store->is_flush()) {
        stay_in_graph.try_put({store, eom, message_id});
        return false;
      }

      if (const_accessor cached; stores_.find(cached, store->id()->hash())) {
        stay_in_graph.try_put({cached->second, eom, message_id});
        return false;
      }

      bool const new_insert = stores_.insert(a, store->id()->hash());
      if (!new_insert) {
        stay_in_graph.try_put({a->second, eom, message_id});
        return false;
      }
      return true;
    }

    template <std::size_t... Is>
    auto call(function_t const& ft, messages_t<N> const& messages, std::index_sequence<Is...>)
    {
      ++calls_;
      return std::invoke(ft, std::get<Is>(input_).retrieve(messages)...);
    }

    std::size_t num_calls() const final { return calls_.load(); }

    std::array<specified_label, N> product_labels_;
    InputArgs input_;
    std::array<std::string, M> output_;
    join_or_none_t<N> join_;
    tbb::flow::multifunction_node<messages_t<N>, messages_t<2u>> transform_;
    stores_t stores_;
    std::atomic<std::size_t> calls_;
  };

}

#endif /* meld_core_declared_transform_hpp */
