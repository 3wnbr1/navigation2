// Copyright (c) 2021 Samsung Research America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NAV2_BT_NAVIGATOR__NAVIGATOR_HPP_
#define NAV2_BT_NAVIGATOR__NAVIGATOR_HPP_

#include <memory>
#include <string>
#include <vector>
#include <mutex>

#include "tf2_ros/buffer.h"
#include "rclcpp/rclcpp.hpp"
#include "rclcpp_lifecycle/lifecycle_node.hpp"
#include "pluginlib/class_loader.hpp"
#include "nav2_behavior_tree/bt_action_server.hpp"

namespace nav2_bt_navigator
{

/**
 * @struct FeedbackUtils
 * @brief Navigator feedback utilities required to get transforms and reference frames.
 */
struct FeedbackUtils
{
  std::string robot_frame;
  std::string global_frame;
  double transform_tolerance;
  std::shared_ptr<tf2_ros::Buffer> tf;
};

/**
 * @class NavigatorMuxer
 * @brief A class to control the state of the BT navigator by allowing only a single
 * plugin to be processed at a time.
 */
class NavigatorMuxer
{
public:
  /**
   * @brief A Navigator Muxer constructor
   */
  NavigatorMuxer() : current_navigator_(std::string("")) {};

  /**
   * @brief Get the navigator muxer state
   * @return bool If a navigator is in progress
   */
  bool isNavigating() {
    std::scoped_lock l(mutex_);
    return !current_navigator_.empty();
  }

  /**
   * @brief Start navigating with a given navigator
   * @param string Name of the navigator to start
   */
  void startNavigating(const std::string & navigator_name) {
    std::scoped_lock l(mutex_);
    if (!current_navigator_.empty()) {
      RCLCPP_ERROR(
        rclcpp::get_logger("NavigatorMutex"),
        "Major error! Navigation requested while another navigation"
        " task is in progress! This likely occurred from an incorrect"
        "implementation of a navigator plugin.");
    }
    current_navigator_ = navigator_name;
  }

  /**
   * @brief Stop navigating with a given navigator
   * @param string Name of the navigator ending task
   */
  void stopNavigating(const std::string & navigator_name) {
    std::scoped_lock l(mutex_);
    if (current_navigator_ != navigator_name) {
      RCLCPP_ERROR(
        rclcpp::get_logger("NavigatorMutex"),
        "Major error! Navigation stopped while another navigation"
        " task is in progress! This likely occurred from an incorrect"
        "implementation of a navigator plugin.");
    } else {
      current_navigator_ = std::string("");      
    }
  }

protected:
  std::string current_navigator_;
  std::mutex mutex_;
};

/**
 * @class Navigator
 * @brief Navigator interface that acts as a base class for all BT-based Navigator action's plugins
 */
template<class ActionT>
class Navigator
{
public:
  using Ptr = std::shared_ptr<nav2_bt_navigator::Navigator<ActionT>>;

  Navigator()
  {
    plugin_muxer_ = nullptr;
  }

  /**
   * @brief Virtual destructor
   */
  virtual ~Navigator() = default;

  bool on_configure(
    rclcpp_lifecycle::LifecycleNode::WeakPtr parent_node,
    const std::vector<std::string> & plugin_lib_names,
    const FeedbackUtils & feedback_utils,
    nav2_bt_navigator::NavigatorMuxer * plugin_muxer)
  {
    auto node = parent_node.lock();
    logger_ = node->get_logger();
    clock_ = node->get_clock();
    feedback_utils_ = feedback_utils;
    plugin_muxer_ = plugin_muxer;

    // do some stuff
    bt_action_server_ = std::make_unique<nav2_behavior_tree::BtActionServer<ActionT>>(
    node,
    getName(),
    plugin_lib_names,
    std::bind(&Navigator::onGoalReceived, this, std::placeholders::_1),
    std::bind(&Navigator::onLoop, this),
    std::bind(&Navigator::onPreempt, this),
    std::bind(&Navigator::onCompletion, this, std::placeholders::_1));

    bool ok = true;
    if (!bt_action_server_->on_configure()) {
      ok = false;
    }

    BT::Blackboard::Ptr blackboard = bt_action_server_->getBlackboard();
    blackboard->set<std::shared_ptr<tf2_ros::Buffer>>("tf_buffer", feedback_utils.tf);  // NOLINT
    blackboard->set<bool>("initial_pose_received", false);  // NOLINT
    blackboard->set<int>("number_recoveries", 0);  // NOLINT

    return configure(parent_node) && ok;
  }

  bool on_activate()
  {
    bool ok = true;

    if (!bt_action_server_->on_activate()) {
      ok = false;
    }

    return activate() && ok;
  }

  bool on_deactivate()
  {
    bool ok = true;
    if (!bt_action_server_->on_deactivate()) {
      ok = false;
    }

    return deactivate() && ok;
  }

  bool on_cleanup()
  {
    bool ok = true;
    if (!bt_action_server_->on_cleanup()) {
      ok = false;
    }

    bt_action_server_.reset();

    return cleanup() && ok;
  }

  bool on_shutdown()
  {
    bool ok = true;
    if (!bt_action_server_->on_shutdown()) {
      ok = false;
    }

    return shutdown() && ok;
  }

  virtual std::string getName() = 0;

protected:
  /**
   * @brief An intermediate goal reception function to mux navigators.
   */
  bool onGoalReceived(typename ActionT::Goal::ConstSharedPtr goal) {
    if (plugin_muxer_->isNavigating()) {
      return false;
    }

    plugin_muxer_->startNavigating(getName());

    return goalReceived(goal);
  }

  /**
   * @brief An intermediate compution function to mux navigators
   */
  void onCompletion(typename ActionT::Result::SharedPtr result) {
    plugin_muxer_->stopNavigating(getName());
    goalCompleted(result);
  }

  /**
   * @brief A callback to be called when a new goal is received by the BT action server
   * Can be used to check if goal is valid and put values on
   * the blackboard which depend on the received goal
   */
  virtual bool goalReceived(typename ActionT::Goal::ConstSharedPtr goal) = 0;

  /**
   * @brief A callback that defines execution that happens on one iteration through the BT
   * Can be used to publish action feedback
   */
  virtual void onLoop() = 0;

  /**
   * @brief A callback that is called when a preempt is requested
   */
  virtual void onPreempt() = 0;

  /**
   * @brief A callback that is called when a the action is completed, can fill in
   * action result message or indicate that this action is done.
   */
  virtual void goalCompleted(typename ActionT::Result::SharedPtr result) = 0;

  /**
   * @param Method to configure resources.
   */
  virtual bool configure(rclcpp_lifecycle::LifecycleNode::WeakPtr /*node*/) {return true;};

  /**
   * @brief Method to cleanup resources.
   */
  virtual bool cleanup() {return true;};

  /**
   * @brief Method to active and any threads involved in execution.
   */
  virtual bool activate() {return true;};

  /**
   * @brief Method to deactive and any threads involved in execution.
   */
  virtual bool deactivate() {return true;};

  /**
   * @brief Method to deactive and any threads involved in execution.
   */
  virtual bool shutdown() {return true;};

  std::unique_ptr<nav2_behavior_tree::BtActionServer<ActionT>> bt_action_server_;
  rclcpp::Logger logger_{rclcpp::get_logger("Navigator")};
  rclcpp::Clock::SharedPtr clock_;
  FeedbackUtils feedback_utils_;
  NavigatorMuxer * plugin_muxer_;
};

}  // namespace nav2_bt_navigator

#endif  // nav2_bt_navigator__NAVIGATOR_HPP_
