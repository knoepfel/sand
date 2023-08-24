#ifndef meld_core_declared_splitter_hpp
#define meld_core_declared_splitter_hpp

#include "meld/concurrency.hpp"
#include "meld/core/concepts.hpp"
#include "meld/core/detail/port_names.hpp"
#include "meld/core/end_of_message.hpp"
#include "meld/core/fwd.hpp"
#include "meld/core/message.hpp"
#include "meld/core/multiplexer.hpp"
#include "meld/core/products_consumer.hpp"
#include "meld/core/registrar.hpp"
#include "meld/core/store_counters.hpp"
#include "meld/model/handle.hpp"
#include "meld/model/level_id.hpp"
#include "meld/model/product_store.hpp"
#include "meld/utilities/sized_tuple.hpp"

#include "oneapi/tbb/concurrent_hash_map.h"
#include "oneapi/tbb/flow_graph.h"
#include "spdlog/spdlog.h"

#include <array>
#include <atomic>
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
#include <vector>

namespace meld {

  class generator {
  public:
    explicit generator(product_store_const_ptr const& parent,
                       std::string const& node_name,
                       std::string const& new_level_name);
    product_store_const_ptr flush_store() const;

    template <typename... Ts>
    product_store_const_ptr make_child_for(std::size_t const level_number, products new_products)
    {
      return make_child(level_number, std::move(new_products));
    }

  private:
    product_store_const_ptr make_child(std::size_t i, products new_products);
    product_store_ptr parent_;
    std::string const& node_name_;
    std::string const& new_level_name_;
    std::map<std::string, std::size_t> child_counts_;
  };

  class declared_splitter : public products_consumer {
  public:
    declared_splitter(std::string name,
                      std::vector<std::string> preceding_filters,
                      std::vector<std::string> receive_stores);
    virtual ~declared_splitter();

    virtual tbb::flow::sender<message>& to_output() = 0;
    virtual std::span<std::string const, std::dynamic_extent> output() const = 0;
    virtual void finalize(multiplexer::head_ports_t head_ports) = 0;
  };

  using declared_splitter_ptr = std::unique_ptr<declared_splitter>;
  using declared_splitters = std::map<std::string, declared_splitter_ptr>;

  // =====================================================================================

  template <typename Object, typename Predicate, typename Unfold, typename InputArgs>
  class partial_splitter {
    static constexpr std::size_t N = std::tuple_size_v<InputArgs>;

    template <std::size_t M>
    class complete_splitter;

  public:
    partial_splitter(registrar<declared_splitters> reg,
                     std::string name,
                     std::optional<std::size_t> concurrency,
                     std::vector<std::string> preceding_filters,
                     std::vector<std::string> receive_stores,
                     tbb::flow::graph& g,
                     Predicate&& predicate,
                     Unfold&& unfold,
                     InputArgs input_args,
                     std::array<std::string, N> product_names) :
      name_{std::move(name)},
      concurrency_{concurrency},
      preceding_filters_{std::move(preceding_filters)},
      receive_stores_{std::move(receive_stores)},
      graph_{g},
      predicate_{std::move(predicate)},
      unfold_{std::move(unfold)},
      input_args_{std::move(input_args)},
      product_names_{std::move(product_names)},
      reg_{std::move(reg)}
    {
    }

    template <std::size_t M>
    auto& into(std::array<std::string, M> output_product_names)
    {
      reg_.set([this, output = std::move(output_product_names)] {
        return std::make_unique<complete_splitter<M>>(std::move(name_),
                                                      concurrency_.value_or(concurrency::serial),
                                                      std::move(preceding_filters_),
                                                      std::move(receive_stores_),
                                                      graph_,
                                                      std::move(predicate_),
                                                      std::move(unfold_),
                                                      std::move(input_args_),
                                                      std::move(product_names_),
                                                      std::move(output),
                                                      std::move(new_level_name_));
      });
      return *this;
    }

    auto& into(std::convertible_to<std::string> auto&&... ts)
    {
      return into(std::array<std::string, sizeof...(ts)>{std::forward<decltype(ts)>(ts)...});
    }

    auto& within_domain(std::string new_level_name)
    {
      new_level_name_ = std::move(new_level_name);
      return *this;
    }

    auto& using_concurrency(std::size_t n)
    {
      if (!concurrency_) {
        concurrency_ = n;
      }
      return *this;
    }

  private:
    std::string name_;
    std::optional<std::size_t> concurrency_;
    std::vector<std::string> preceding_filters_;
    std::vector<std::string> receive_stores_;
    tbb::flow::graph& graph_;
    Predicate predicate_;
    Unfold unfold_;
    InputArgs input_args_;
    std::array<std::string, N> product_names_;
    std::string new_level_name_;
    registrar<declared_splitters> reg_;
  };

  // =====================================================================================

  template <typename Object, typename Predicate, typename Unfold, typename InputArgs>
  template <std::size_t M>
  class partial_splitter<Object, Predicate, Unfold, InputArgs>::complete_splitter :
    public declared_splitter,
    public detect_flush_flag {
    using stores_t = tbb::concurrent_hash_map<level_id::hash_type, product_store_ptr>;
    using accessor = stores_t::accessor;
    using const_accessor = stores_t::const_accessor;

  public:
    complete_splitter(std::string name,
                      std::size_t concurrency,
                      std::vector<std::string> preceding_filters,
                      std::vector<std::string> receive_stores,
                      tbb::flow::graph& g,
                      Predicate&& predicate,
                      Unfold&& unfold,
                      InputArgs input,
                      std::array<std::string, N> product_names,
                      std::array<std::string, M> output_products,
                      std::string new_level_name) :
      declared_splitter{std::move(name), std::move(preceding_filters), std::move(receive_stores)},
      product_names_{std::move(product_names)},
      input_{std::move(input)},
      output_{std::move(output_products)},
      new_level_name_{std::move(new_level_name)},
      multiplexer_{g},
      join_{make_join_or_none(g, std::make_index_sequence<N>{})},
      splitter_{
        g,
        concurrency,
        [this, p = std::move(predicate), ufold = std::move(unfold)](
          messages_t<N> const& messages) -> tbb::flow::continue_msg {
          auto const& msg = most_derived(messages);
          auto const& store = msg.store;
          if (store->is_flush()) {
            flag_accessor ca;
            flag_for(store->id()->hash(), ca).flush_received(msg.id);
          }
          else if (accessor a; stores_.insert(a, store->id()->hash())) {
            std::size_t const original_message_id{msg_counter_};
            generator g{msg.store, this->name(), new_level_name_};
            call(p, ufold, g, msg.eom, messages, std::make_index_sequence<N>{});
            multiplexer_.try_put({g.flush_store(), msg.eom, ++msg_counter_, original_message_id});
            flag_accessor ca;
            flag_for(store->id()->hash(), ca).mark_as_processed();
          }

          auto const id_hash = store->id()->hash();
          if (const_flag_accessor ca; flag_for(id_hash, ca) && ca->second->is_flush()) {
            stores_.erase(id_hash);
            erase_flag(ca);
          }
          return {};
        }},
      to_output_{g}
    {
      make_edge(join_, splitter_);
    }

    ~complete_splitter()
    {
      if (stores_.size() > 0ull) {
        spdlog::warn("Unfold {} has {} cached stores.", name(), stores_.size());
      }
      for (auto const& [_, store] : stores_) {
        spdlog::debug(" => ID: ", store->id()->to_string());
      }
    }

  private:
    tbb::flow::receiver<message>& port_for(std::string const& product_name) override
    {
      return receiver_for<N>(join_, product_names_, product_name);
    }
    std::vector<tbb::flow::receiver<message>*> ports() override { return input_ports<N>(join_); }

    tbb::flow::sender<message>& to_output() override { return to_output_; }

    std::span<std::string const, std::dynamic_extent> input() const override
    {
      return product_names_;
    }
    std::span<std::string const, std::dynamic_extent> output() const override { return output_; }

    void finalize(multiplexer::head_ports_t head_ports) override
    {
      multiplexer_.finalize(std::move(head_ports));
    }

    template <std::size_t... Is>
    void call(Predicate const& predicate,
              Unfold const& unfold,
              generator& g,
              end_of_message_ptr const& eom,
              messages_t<N> const& messages,
              std::index_sequence<Is...>)
    {
      ++calls_;
      Object obj(std::get<Is>(input_).retrieve(messages)...);
      std::size_t counter = 0;
      auto running_value = obj.initial_value();
      while (std::invoke(predicate, obj, running_value)) {
        auto [next_value, prods] = std::invoke(unfold, obj, running_value);
        products new_products;
        new_products.add_all(output_, prods);
        auto child = g.make_child_for(counter++, std::move(new_products));
        message const msg{child, eom->make_child(child->id()), ++msg_counter_};
        to_output_.try_put(msg);
        multiplexer_.try_put(msg);
        running_value = next_value;
      }
    }

    std::size_t num_calls() const final { return calls_.load(); }

    std::array<std::string, N> product_names_;
    InputArgs input_;
    std::array<std::string, M> output_;
    std::string new_level_name_;
    multiplexer multiplexer_;
    join_or_none_t<N> join_;
    tbb::flow::function_node<messages_t<N>> splitter_;
    tbb::flow::broadcast_node<message> to_output_;
    tbb::concurrent_hash_map<level_id::hash_type, product_store_ptr> stores_;
    std::atomic<std::size_t> msg_counter_{}; // Is this sufficient?  Probably not.
    std::atomic<std::size_t> calls_{};
  };
}

#endif /* meld_core_declared_splitter_hpp */
