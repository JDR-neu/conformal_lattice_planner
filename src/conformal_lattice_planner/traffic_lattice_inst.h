/*
 * Copyright [2019] [Ke Sun]
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cmath>
#include <deque>
#include <unordered_map>
#include <stdexcept>
#include <conformal_lattice_planner/traffic_lattice.h>

namespace planner {

template<typename Router>
TrafficLattice<Router>::TrafficLattice(
    const std::vector<VehicleTuple>& vehicles,
    const boost::shared_ptr<CarlaMap>& map,
    const boost::shared_ptr<Router>& router) : map_(map) {

  this->router_ = router;

  // Find the start waypoint and range of the lattice based
  // on the given vehicles.
  boost::shared_ptr<CarlaWaypoint> start_waypoint = nullptr;
  double range = 0.0;
  latticeStartAndRange(vehicles, start_waypoint, range);

  // Now we can construct the lattice.
  // FIXME: The following is just a copy of the Lattice custom constructor.
  //        Can we avoid this code duplication?
  this->longitudinal_resolution_ = 1.0;
  baseConstructor(start_waypoint, range, 1.0, router);

  // Register the vehicles onto the lattice nodes.
  if(!registerVehicles(vehicles)) {
    throw std::runtime_error("Collisions detected within the input vehicles.");
  }

  return;
}

template<typename Router>
TrafficLattice<Router>::TrafficLattice(
    const std::vector<boost::shared_ptr<const CarlaVehicle>>& vehicles,
    const boost::shared_ptr<CarlaMap>& map,
    const boost::shared_ptr<Router>& router) : map_(map) {

  this->router_ = router;

  std::vector<VehicleTuple> vehicle_tuples;
  for (const auto& vehicle : vehicles) {
    vehicle_tuples.push_back(std::make_tuple(
          vehicle->GetId(),
          vehicle->GetTransform(),
          vehicle->GetBoundingBox()));
  }

  // Find the start waypoint and range of the lattice based
  // on the given vehicles.
  boost::shared_ptr<CarlaWaypoint> start_waypoint = nullptr;
  double range = 0.0;
  latticeStartAndRange(vehicle_tuples, start_waypoint, range);

  // Now we can construct the lattice.
  // FIXME: The following is just a copy of the Lattice custom constructor.
  //        Can we avoid this code duplication?
  this->longitudinal_resolution_ = 1.0;
  baseConstructor(start_waypoint, range, 1.0, router);

  // Register the vehicles onto the lattice nodes.
  if(!registerVehicles(vehicle_tuples)) {
    throw std::runtime_error("Collisions detected within the input vehicles.");
  }

  return;
}

template<typename Router>
void TrafficLattice<Router>::baseConstructor(
    const boost::shared_ptr<CarlaWaypoint>& start,
    const double range,
    const double longitudinal_resolution,
    const boost::shared_ptr<Router>& router) {

  this->longitudinal_resolution_ = longitudinal_resolution;
  this->router_ = router;

  if (range <= this->longitudinal_resolution_) {
    throw std::runtime_error(
        (boost::format("The given range [%1%] is too small."
                       "Range should be at least 1xlongitudinal_resolution.") % range).str());
  }

  // Create the start node.
  boost::shared_ptr<Node> start_node = boost::make_shared<Node>(start);
  start_node->distance() = 0.0;
  this->lattice_entry_ = start_node;
  this->lattice_exit_ = start_node;

  this->augmentWaypointToNodeTable(start->GetId(), start_node);
  this->augmentRoadlaneToWaypointsTable(start);

  // Construct the lattice.
  this->extend(range);

  return;
}

template<typename Router>
void TrafficLattice<Router>::latticeStartAndRange(
    const std::vector<VehicleTuple>& vehicles,
    boost::shared_ptr<CarlaWaypoint>& start,
    double& range) const {

  // Arrange the vehicles by id.
  //std::printf("Arrange the vehicles by IDs.\n");
  std::unordered_map<size_t, CarlaTransform> vehicle_transforms;
  std::unordered_map<size_t, CarlaBoundingBox> vehicle_bounding_boxes;
  for (const auto& vehicle : vehicles) {
    size_t id; CarlaTransform transform; CarlaBoundingBox bounding_box;
    std::tie(id, transform, bounding_box) = vehicle;

    vehicle_transforms[id] = transform;
    vehicle_bounding_boxes[id] = bounding_box;
  }

  // Arrange the vehicles by roads.
  //std::printf("Arrange the vehicles by roads.\n");
  std::unordered_map<size_t, std::vector<size_t>> road_to_vehicles_table;
  for (const auto& vehicle : vehicle_transforms) {
    const size_t road = vehicleWaypoint(vehicle.second)->GetRoadId();
    // Initialize the vector if necessary.
    if (road_to_vehicles_table.count(road) == 0)
      road_to_vehicles_table[road] = std::vector<size_t>();
    road_to_vehicles_table[road].push_back(vehicle.first);
  }

  // Sort the vehicles on each road based on its distance.
  // Vehicles with smaller distance are at the beginning of the vector.
  //std::printf("Sort the vehicles on each road.\n");
  for (auto& road : road_to_vehicles_table) {
    std::sort(road.second.begin(), road.second.end(),
        [this, &vehicle_transforms](const size_t v0, const size_t v1)->bool{
          const double d0 = waypointToRoadStartDistance(vehicleWaypoint(vehicle_transforms[v0]));
          const double d1 = waypointToRoadStartDistance(vehicleWaypoint(vehicle_transforms[v1]));
          return d0 < d1;
        });
  }

  // Connect the roads into a chain.
  //std::printf("Connect the roads into a chain.\n");
  std::unordered_set<size_t> roads;
  for (const auto& road : road_to_vehicles_table)
    roads.insert(road.first);

  std::deque<size_t> sorted_roads = sortRoads(roads);

  // Find the first (minimum distance) and last (maximum distance)
  // waypoint of the input vehicles.
  //std::printf("Find the first and last vehicle and their waypoints.\n");
  const size_t first_vehicle = road_to_vehicles_table[sorted_roads.front()].front();
  const size_t last_vehicle  = road_to_vehicles_table[sorted_roads.back()].back();

  boost::shared_ptr<CarlaWaypoint> first_waypoint = vehicleRearWaypoint(
      vehicle_transforms[first_vehicle], vehicle_bounding_boxes[first_vehicle]);
  boost::shared_ptr<CarlaWaypoint> last_waypoint = vehicleHeadWaypoint(
      vehicle_transforms[last_vehicle], vehicle_bounding_boxes[last_vehicle]);

  // Set the output start.
  start = first_waypoint;

  // Find the range of the traffic lattice
  // (the distance between the rear of the first vehicle and
  //  the front of the last vehicle).
  //
  // Some special care is required since the first and last waypoints
  // may not be on the existing roads. If not, just extend the range
  // a bit (5m in this case).
  //std::printf("Find the range of the traffic lattice.\n");
  range = 0.0;
  for (const size_t id : sorted_roads) {
    range += map_->GetMap().GetMap().GetRoad(id).GetLength();
  }

  if (first_waypoint->GetRoadId() == sorted_roads.front()) {
    range -= waypointToRoadStartDistance(first_waypoint);
  } else {
    range+= 5.0;
  }

  if (last_waypoint->GetRoadId() == sorted_roads.back()) {
    range -= map_->GetMap().GetMap().GetRoad(sorted_roads.back()).GetLength() -
             waypointToRoadStartDistance(last_waypoint);
  } else {
    range += 5.0;
  }

  return;
}

template<typename Router>
bool TrafficLattice<Router>::registerVehicles(
    const std::vector<VehicleTuple>& vehicles) {

  // Clear the \c vehicle_to_node_table_.
  vehicle_to_nodes_table_.clear();

  for (const auto& vehicle : vehicles) {

    // Extract the stuff in the tuple
    size_t id; CarlaTransform transform; CarlaBoundingBox bounding_box;
    std::tie(id, transform, bounding_box) = vehicle;

    // Find the waypoints (head and rear) of this vehicle.
    boost::shared_ptr<const CarlaWaypoint> head_waypoint =
      vehicleHeadWaypoint(transform, bounding_box);
    boost::shared_ptr<const CarlaWaypoint> rear_waypoint =
      vehicleRearWaypoint(transform, bounding_box);

    // Find the nodes occupied by this vehicle.
    boost::shared_ptr<Node> head_node = this->closestNode(
        head_waypoint, this->longitudinal_resolution_/2.0);
    boost::shared_ptr<Node> rear_node = this->closestNode(
        rear_waypoint, this->longitudinal_resolution_/2.0);
    if (!head_node || !rear_node)  {
      throw std::runtime_error("Cannot find nodes on lattice close to the vehicle.");
    }

    std::vector<boost::weak_ptr<Node>> nodes;
    boost::shared_ptr<Node> next_node = rear_node;
    while (next_node->waypoint()->GetId() != head_node->waypoint()->GetId()) {
      nodes.emplace_back(next_node);
      if (next_node->front().lock())
        next_node = next_node->front().lock();
      else
        throw std::runtime_error("The head and rear nodes for the vehicle are not connected in the lattice.");
    }
    nodes.emplace_back(head_node);

    // If there is already a vehicle on any of the found nodes,
    // it indicates there is a collision. Otherwise, we register
    // the vehicle onto this node.
    for (auto& node : nodes) {
      if (node.lock()->vehicle()) return false;
      else node.lock()->vehicle() = id;
    }

    // Update the \c vehicle_to_nodes_table_.
    vehicle_to_nodes_table_[id] = nodes;
  }

  return true;
}



template<typename Router>
std::deque<size_t> TrafficLattice<Router>::sortRoads(
    const std::unordered_set<size_t>& roads) const {

  // Keep track of the road IDs we have not dealt with.
  std::unordered_set<size_t> remaining_roads(roads);

  // Keep track of the sorted roads.
  std::deque<size_t> sorted_roads;

  // Start from a random road in the given set.
  sorted_roads.push_back(*(remaining_roads.begin()));
  remaining_roads.erase(remaining_roads.begin());

  // We will only expand 5 times.
  for (size_t i = 0; i < 5; ++i) {
    // Current first and last road in the chain.
    const size_t first_road = sorted_roads.front();
    const size_t last_road = sorted_roads.back();

    // New first and last road in the chain.
    boost::optional<size_t> new_first_road = this->router_->prevRoad(first_road);
    boost::optional<size_t> new_last_road = this->router_->nextRoad(last_road);

    if (new_first_road) {
      sorted_roads.push_front(*new_first_road);
      if (!remaining_roads.empty()) remaining_roads.erase(*new_first_road);
    }
    if (new_last_road) {
      sorted_roads.push_back(*new_last_road);
      if (!remaining_roads.empty()) remaining_roads.erase(*new_last_road);
    }
    if (remaining_roads.empty()) break;
  }

  // If for some weired reason, there is still some road remaining
  // which cannot be sorted, throw a runtime error.
  if (!remaining_roads.empty()) {
    throw std::runtime_error(
        "The given roads cannot be sorted."
        "This is probably because the given vehicles does not construct a local traffic.");
  }

  // Trim the sorted road vector so that both the first and last road
  // in the vector are within the given roads.
  while (roads.count(sorted_roads.front()) == 0)
    sorted_roads.pop_front();
  while (roads.count(sorted_roads.back()) == 0)
    sorted_roads.pop_back();

  return sorted_roads;
}

template<typename Router>
boost::shared_ptr<typename TrafficLattice<Router>::CarlaWaypoint>
  TrafficLattice<Router>::vehicleHeadWaypoint(
    const CarlaTransform& transform,
    const CarlaBoundingBox& bounding_box) const {

  const double sin = std::sin(transform.rotation.yaw/180.0*M_PI);
  const double cos = std::cos(transform.rotation.yaw/180.0*M_PI);

  // Be careful here! We are dealing with the left hand coordinates.
  // Do not really care about the z-axis.
  carla::geom::Location waypoint_location;
  waypoint_location.x = cos*bounding_box.extent.x + transform.location.x;
  waypoint_location.y = sin*bounding_box.extent.x + transform.location.y;
  waypoint_location.z = transform.location.z;

  //std::printf("vehicle waypoint location: x:%f y:%f z:%f\n",
  //    transform.location.x, transform.location.y, transform.location.z);
  //std::printf("head waypoint location: x:%f y:%f z:%f\n",
  //    waypoint_location.x, waypoint_location.y, waypoint_location.z);

  return map_->GetWaypoint(waypoint_location);
}

template<typename Router>
boost::shared_ptr<typename TrafficLattice<Router>::CarlaWaypoint>
  TrafficLattice<Router>::vehicleRearWaypoint(
    const CarlaTransform& transform,
    const CarlaBoundingBox& bounding_box) const {

  const double sin = std::sin(transform.rotation.yaw/180.0*M_PI);
  const double cos = std::cos(transform.rotation.yaw/180.0*M_PI);

  // Be careful here! We are dealing with the left hand coordinates.
  // Do not really care about the z-axis.
  carla::geom::Location waypoint_location;
  waypoint_location.x = -cos*bounding_box.extent.x + transform.location.x;
  waypoint_location.y = -sin*bounding_box.extent.x + transform.location.y;
  waypoint_location.z = transform.location.z;

  //std::printf("vehicle waypoint location: x:%f y:%f z:%f\n",
  //    transform.location.x, transform.location.y, transform.location.z);
  //std::printf("rear waypoint location: x:%f y:%f z:%f\n",
  //    waypoint_location.x, waypoint_location.y, waypoint_location.z);
  return map_->GetWaypoint(waypoint_location);
}
} // End namespace planner.