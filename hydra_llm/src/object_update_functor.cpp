#include "hydra_llm/object_update_functor.h"

#include <config_utilities/config.h>
#include <config_utilities/types/conversions.h>
#include <config_utilities/validation.h>
#include <glog/logging.h>
#include <hydra/common/dsg_types.h>
#include <hydra/utils/nearest_neighbor_utilities.h>
#include <hydra/utils/timing_utilities.h>
#include <khronos/common/utils/khronos_attribute_utils.h>

#include "hydra_llm/agglomerative_clustering.h"
#include "hydra_llm/probability_utilities.h"

namespace hydra::llm {

using config::VirtualConfig;
using timing::ScopedTimer;

bool isNodeActive(const SceneGraphNode& node,
                  const std::map<NodeId, size_t>& node_to_component,
                  const std::set<NodeId>& invalid) {
  return !node_to_component.count(node.id) && !invalid.count(node.id);
}

IdTracker::IdTracker(size_t start) : idx_(start) {}

size_t IdTracker::next() const {
  // default to assuming unused is empty
  size_t new_id = idx_;
  if (unused_.empty()) {
    ++idx_;
  } else {
    new_id = unused_.front();
    unused_.pop_front();
  }

  return new_id;
}

void IdTracker::markFree(size_t idx) const {
  // validate that ID could have come from existing object
  if (idx < idx_) {
    unused_.push_back(idx);
  }
}

OverlapIntersection::OverlapIntersection(const Config& config)
    : IntersectionPolicy(), config(config) {}

bool OverlapIntersection::call(const KhronosObjectAttributes& lhs,
                               const KhronosObjectAttributes& rhs) const {
  const auto& bbox_lhs = lhs.bounding_box;
  const auto& bbox_rhs = rhs.bounding_box;
  // TODO(nathan) get expanded bounding boxes
  Eigen::Array<float, 4, 3> combined;
  combined.row(0) = bbox_lhs.min;
  combined.row(1) = bbox_rhs.min;
  combined.row(2) = bbox_lhs.max;
  combined.row(3) = bbox_rhs.max;

  const auto min = combined.colwise().minCoeff();
  combined.rowwise() -= min;
  return (combined.row(0) <= combined.row(3)).all() &&
         (combined.row(2) >= combined.row(1)).all();
}

void declare_config(OverlapIntersection::Config& config) {
  using namespace config;
  name("OverlapIntersection::Config");
  field(config.tolerance, "tolerance");
}

ComponentInfo::ComponentInfo(const IBEdgeSelector::Config& config,
                             const EmbeddingGroup& tasks,
                             const EmbeddingDistance& metric,
                             const SceneGraphLayer& layer,
                             const std::vector<NodeId>& nodes,
                             double I_xy_full)
    : edge_selector(config), ws(layer, nodes), segments(nodes) {
  double delta_weight = computeDeltaWeight(layer, nodes);
  clusterAgglomerative(ws, tasks, edge_selector, metric, true, I_xy_full, delta_weight);
}

ObjectUpdateFunctor::ObjectUpdateFunctor(const Config& config)
    : config(config::checkValid(config)),
      edge_checker_(config.edge_checker.create()),
      tasks_(config.tasks.create()),
      metric_(config.metric.create()),
      next_node_id_(config.prefix, 0) {}

MergeMap ObjectUpdateFunctor::call(SharedDsgInfo& dsg, const UpdateInfo& info) const {
  ScopedTimer timer("backend/object_clustering", info.timestamp_ns);
  auto& graph = *dsg.graph;

  // repair broken edges between objects and places
  updateActiveParents(graph);
  // detect edges between segments (and active connected components)
  const auto active_components = addSegmentEdges(graph);
  // remove all previous components that are active
  clearActiveComponents(graph, active_components);
  // construct new components and cluster into objects
  detectObjects(graph);
  // we never have explict merges (clustering takes care of them)
  return {};
}

void ObjectUpdateFunctor::clearActiveComponents(DynamicSceneGraph& graph,
                                                const std::set<size_t>& active) const {
  auto iter = components_.begin();
  while (iter != components_.end()) {
    if (!active.count(iter->first)) {
      ++iter;
      continue;
    }

    for (const auto node_id : iter->second->segments) {
      node_to_component_.erase(node_id);
    }

    for (const auto& node_id : iter->second->objects) {
      graph.removeNode(node_id);
      active_.erase(node_id);
    }

    components_ids_.markFree(iter->first);
    iter = components_.erase(iter);
  }
}

std::set<size_t> ObjectUpdateFunctor::addSegmentEdges(DynamicSceneGraph& graph) const {
  const auto& segments = graph.getLayer(DsgLayers::SEGMENTS);

  std::set<size_t> active_components;
  for (auto&& [node_id, node] : segments.nodes()) {
    auto& attrs = node->attributes<KhronosObjectAttributes>();
    if (ignored_.count(node_id)) {
      continue;
    }

    if (node_to_component_.count(node_id)) {
      // only examine new nodes
      continue;
    }

    const Eigen::VectorXd feature = attrs.semantic_feature.rowwise().mean();
    const auto result = tasks_->getBestScore(*metric_, feature);
    if (result.score < config.min_segment_score) {
      VLOG(1) << "Skipping segment with score: " << result.score;
      ignored_.insert(node_id);
      attrs.is_active = false;
      continue;
    }

    // TODO(nathan) do something smarter than pairwise iteration
    for (auto&& [other_id, other_node] : segments.nodes()) {
      if (other_id == node_id) {
        continue;
      }

      const auto& other_attrs = other_node->attributes<KhronosObjectAttributes>();
      if (edge_checker_->call(attrs, other_attrs)) {
        graph.insertEdge(node_id, other_id);
        const auto iter = node_to_component_.find(other_id);
        if (iter != node_to_component_.end()) {
          active_components.insert(iter->second);
        }
      }
    }
  }

  return active_components;
}

NodeAttributes::Ptr getMergedAttributes(const DynamicSceneGraph& graph,
                                        const std::vector<NodeId>& nodes) {
  if (nodes.empty()) {
    return nullptr;
  }

  auto iter = nodes.begin();
  CHECK(graph.hasNode(*iter));
  const auto& node = graph.getNode(*iter)->get();
  ++iter;

  auto attrs_ptr = node.attributes().clone();
  auto& attrs = *CHECK_NOTNULL(dynamic_cast<KhronosObjectAttributes*>(attrs_ptr.get()));
  attrs.semantic_feature = attrs.semantic_feature.rowwise().mean().eval();

  while (iter != nodes.end()) {
    const auto& other = graph.getNode(*iter)->get();
    const auto& other_attrs = other.attributes<KhronosObjectAttributes>();
    attrs.position += other_attrs.position;
    attrs.semantic_feature += other_attrs.semantic_feature.rowwise().mean();
    // TODO(nathan) this is likely not correct
    khronos::mergeObjectAttributes(other_attrs, attrs);
    ++iter;
  }

  attrs.position /= nodes.size();
  attrs.semantic_feature /= nodes.size();
  return attrs_ptr;
}

std::optional<std::pair<NodeId, bool>> getBestParent(const DynamicSceneGraph& graph,
                                                     const std::vector<NodeId>& nodes) {
  std::vector<NodeId> active;
  std::vector<NodeId> archived;
  for (const auto node_id : nodes) {
    const SceneGraphNode& node = graph.getNode(node_id).value();
    const auto parent = node.getParent();
    if (!parent) {
      continue;
    }

    const SceneGraphNode& parent_node = graph.getNode(*parent).value();
    if (parent_node.attributes().is_active) {
      active.push_back(*parent);
    } else {
      archived.push_back(*parent);
    }
  }

  if (!archived.empty()) {
    return std::make_pair(archived.front(), false);
  }

  if (!active.empty()) {
    return std::make_pair(active.front(), true);
  }

  return std::nullopt;
}

void ObjectUpdateFunctor::detectObjects(DynamicSceneGraph& graph) const {
  const auto& segments = graph.getLayer(DsgLayers::SEGMENTS);

  const ClusteringWorkspace total_ws(segments);
  const auto py_all = computeIBpy(*tasks_);
  const auto px_all = computeIBpx(total_ws);
  const auto py_x_all =
      computeIBpyGivenX(total_ws, *tasks_, *metric_, config.selector.py_x);
  double I_xy_all = mutualInformation(py_all, px_all, py_x_all);

  // connected component search
  const auto new_components = graph_utilities::getConnectedComponents(
      segments,
      [&](const auto& n) { return isNodeActive(n, node_to_component_, ignored_); },
      [&](const auto& edge) {
        const auto source_active = isNodeActive(
            segments.getNode(edge.source).value(), node_to_component_, ignored_);
        const auto target_active = isNodeActive(
            segments.getNode(edge.target).value(), node_to_component_, ignored_);
        return source_active && target_active;
      });

  // reassign components
  for (const auto& nodes : new_components) {
    size_t new_id = components_ids_.next();
    auto new_component = std::make_unique<ComponentInfo>(
        config.selector, *tasks_, *metric_, segments, nodes, I_xy_all);
    for (const auto node_id : nodes) {
      node_to_component_[node_id] = new_id;
    }

    const auto clusters = new_component->ws.getClusters();
    for (const auto& cluster : clusters) {
      VLOG(5) << "Cluster: " << displayNodeSymbolContainer(cluster);

      auto attrs = getMergedAttributes(graph, cluster);
      if (!attrs) {
        LOG(ERROR) << "empty cluster!";
        continue;
      }

      const auto& feature =
          CHECK_NOTNULL(dynamic_cast<SemanticNodeAttributes*>(attrs.get()))
              ->semantic_feature;
      const auto result = tasks_->getBestScore(*metric_, feature);
      if (result.score < config.min_object_score) {
        VLOG(1) << "Skipping object with score: " << result.score;
        continue;
      }

      graph.emplaceNode(DsgLayers::OBJECTS, next_node_id_, std::move(attrs));
      new_component->objects.push_back(next_node_id_);

      const auto parent = getBestParent(graph, cluster);
      if (!parent) {
        LOG(WARNING) << "object '" << next_node_id_.getLabel() << "' without parent!";
        active_.insert(next_node_id_);
      } else {
        auto&& [parent_id, parent_active] = *parent;
        graph.insertEdge(next_node_id_, parent_id);
        if (parent_active) {
          active_.insert(next_node_id_);
        }
      }

      ++next_node_id_;
    }

    components_.emplace(new_id, std::move(new_component));
  }
}

void ObjectUpdateFunctor::updateActiveParents(DynamicSceneGraph& graph) const {
  std::vector<NodeId> place_ids;
  const auto& places = graph.getLayer(DsgLayers::PLACES);
  for (const auto& id_node_pair : graph.getLayer(DsgLayers::PLACES).nodes()) {
    place_ids.push_back(id_node_pair.first);
  }

  // TODO(nathan) drop this once edges start behaving
  for (const auto& id_node_pair : graph.getLayer(DsgLayers::OBJECTS).nodes()) {
    active_.insert(id_node_pair.first);
  }

  NearestNodeFinder places_finder(places, place_ids);
  auto iter = active_.begin();
  while (iter != active_.end()) {
    const SceneGraphNode& node = graph.getNode(*iter).value();
    const auto parent_id = node.getParent();
    if (parent_id) {
      const bool is_active = places.getNode(*parent_id)->get().attributes().is_active;
      if (is_active) {
        ++iter;
      } else {
        iter = active_.erase(iter);
      }

      continue;
    }

    bool found = false;
    places_finder.find(node.attributes().position,
                       1,
                       false,
                       [&](NodeId place_id, size_t, double distance) {
                         if (config.neighbor_max_distance > 0.0 &&
                             distance >= config.neighbor_max_distance) {
                           LOG(WARNING)
                               << "Discarding nearest neighbor '"
                               << NodeSymbol(place_id).getLabel() << "' for node '"
                               << NodeSymbol(node.id).getLabel() << "' with distance "
                               << distance << " >= " << config.neighbor_max_distance;
                         }

                         graph.insertEdge(place_id, node.id);
                         found = true;
                       });
    if (found) {
      iter = active_.erase(iter);
    } else {
      ++iter;
    }
  }
}

void declare_config(ObjectUpdateFunctor::Config& config) {
  using namespace config;
  name("ObjectUpdateFunctor::Config");
  field<CharConversion>(config.prefix, "prefix");
  config.edge_checker.setOptional();
  field(config.edge_checker, "edge_checker");
  field(config.tasks, "tasks");
  config.metric.setOptional();
  field(config.metric, "metric");
  field(config.selector, "selector");
  field(config.min_segment_score, "min_segment_score");
  field(config.min_object_score, "min_object_score");
  field(config.neighbor_max_distance, "neighbor_max_distance");
}

}  // namespace hydra::llm
