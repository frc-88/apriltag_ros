/**
 * Copyright (c) 2017, California Institute of Technology.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of the California Institute of
 * Technology.
 */

#include "apriltag_ros/continuous_depth_detector.h"

#include <pluginlib/class_list_macros.hpp>

PLUGINLIB_EXPORT_CLASS(apriltag_ros::ContinuousDepthDetector, nodelet::Nodelet);

namespace apriltag_ros
{
void ContinuousDepthDetector::onInit ()
{
  ros::NodeHandle& nh = getNodeHandle();
  ros::NodeHandle& pnh = getPrivateNodeHandle();

  tag_detector_ = std::shared_ptr<TagDetector>(new TagDetector(pnh));
  draw_tag_detections_image_ = getAprilTagOption<bool>(pnh, 
      "publish_tag_detections_image", false);
  it_ = std::shared_ptr<image_transport::ImageTransport>(
      new image_transport::ImageTransport(nh));

  std::string transport_hint;
  pnh.param<std::string>("transport_hint", transport_hint, "raw");
  std::string depth_transport_hint;
  pnh.param<std::string>("depth_transport_hint", depth_transport_hint, "raw");
  pnh.param<double>("depth_min_range", depth_min_range_, 0.0);
  pnh.param<double>("depth_max_range", depth_max_range_, 10.0);

  int queue_size;
  pnh.param<int>("queue_size", queue_size, 1);
  camera_image_subscriber_ =
      it_->subscribeCamera("color/image_rect", queue_size,
                          &ContinuousDepthDetector::imageCallback, this,
                          image_transport::TransportHints(transport_hint));
  depth_image_subscriber_ = 
    it_->subscribe("depth/image_rect", queue_size,
                  &ContinuousDepthDetector::depthCallback, this,
                  image_transport::TransportHints(depth_transport_hint));
  tag_detections_publisher_ =
      nh.advertise<AprilTagDetectionArray>("tag_detections", 1);
  if (draw_tag_detections_image_)
  {
    tag_detections_image_publisher_ = it_->advertise("tag_detections_image", 1);
  }

  refresh_params_service_ =
      pnh.advertiseService("refresh_tag_params", 
                          &ContinuousDepthDetector::refreshParamsCallback, this);
}

void ContinuousDepthDetector::refreshTagParameters()
{
  // Resetting the tag detector will cause a new param server lookup
  // So if the parameters have changed (by someone/something), 
  // they will be updated dynamically
  std::scoped_lock<std::mutex> lock(detection_mutex_);
  ros::NodeHandle& pnh = getPrivateNodeHandle();
  tag_detector_.reset(new TagDetector(pnh));
}

bool ContinuousDepthDetector::refreshParamsCallback(std_srvs::Empty::Request& req,
                                               std_srvs::Empty::Response& res)
{
  refreshTagParameters();
  return true;
}

void ContinuousDepthDetector::imageCallback (
    const sensor_msgs::ImageConstPtr& image_rect,
    const sensor_msgs::CameraInfoConstPtr& camera_info)
{
  std::scoped_lock<std::mutex> lock(detection_mutex_);
  // Lazy updates:
  // When there are no subscribers _and_ when tf is not published,
  // skip detection.
  if (tag_detections_publisher_.getNumSubscribers() == 0 &&
      tag_detections_image_publisher_.getNumSubscribers() == 0 &&
      !tag_detector_->get_publish_tf())
  {
    // ROS_INFO_STREAM("No subscribers and no tf publishing, skip processing.");
    return;
  }

  // Convert ROS's sensor_msgs::Image to cv_bridge::CvImagePtr in order to run
  // AprilTag 2 on the iamge
  try
  {
    cv_image_ = cv_bridge::toCvCopy(image_rect, image_rect->encoding);
  }
  catch (cv_bridge::Exception& e)
  {
    ROS_ERROR("cv_bridge exception: %s", e.what());
    return;
  }

  // Publish detected tags in the image by AprilTag 2
  if (cv_depth_ == NULL) {
    ROS_WARN_THROTTLE(2.0, "No depth image received. Can't find tags.");
    return;
  }
  tag_detections_publisher_.publish(
      tag_detector_->detectTags(cv_image_, cv_depth_, camera_info, depth_min_range_, depth_max_range_));

  // Publish the camera image overlaid by outlines of the detected tags and
  // their payload values
  if (draw_tag_detections_image_)
  {
    tag_detector_->drawDetections(cv_image_);
    tag_detections_image_publisher_.publish(cv_image_->toImageMsg());
  }
}
void ContinuousDepthDetector::depthCallback (
    const sensor_msgs::ImageConstPtr& depth_rect)
{
  // Convert ROS's sensor_msgs::Image to cv_bridge::CvImagePtr
  try
  {
    cv_depth_ = cv_bridge::toCvCopy(depth_rect);  // encoding: passthrough
    ROS_INFO_ONCE("Depth image for tags received.");
  }
  catch (cv_bridge::Exception& e)
  {
    ROS_ERROR("cv_bridge exception: %s", e.what());
    return;
  }
}

} // namespace apriltag_ros
