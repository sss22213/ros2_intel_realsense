// Copyright (c) 2019 Intel Corporation. All Rights Reserved
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

#include <sstream>
#include "realsense/rs_base.hpp"

namespace realsense
{
using namespace std::chrono_literals;

RealSenseBase::RealSenseBase(rs2::context ctx, rs2::device dev, rclcpp::Node & node)
: node_(node),
  ctx_(ctx),
  dev_(dev)
{ 
  pipeline_ = rs2::pipeline(ctx_);
  node_.set_on_parameters_set_callback(std::bind(&RealSenseBase::paramChangeCallback, this, std::placeholders::_1));
}

RealSenseBase::~RealSenseBase()
{
  pipeline_.stop();
}

void RealSenseBase::startPipeline()
{
  auto p_profile = cfg_.resolve(pipeline_);
  auto active_profiles = p_profile.get_streams();
  for (auto & profile : active_profiles) {
    if (profile.is<rs2::video_stream_profile>()) {
      updateVideoStreamCalibData(profile.as<rs2::video_stream_profile>());
    }
  }
  pipeline_.start(cfg_, std::bind(&RealSenseBase::publishTopicsCallback, this, std::placeholders::_1));
}

void RealSenseBase::setupStream(const stream_index_pair & stream)
{
  std::ostringstream os;
  os << STREAM_NAME.at(stream.first) << stream.second << ".enabled";
  bool enable = node_.declare_parameter(os.str(), false);

  if (stream == ACCEL || stream == GYRO) {
    imu_pub_.insert(std::pair<stream_index_pair, rclcpp::Publisher<sensor_msgs::msg::Imu>::SharedPtr>
      (stream, node_.create_publisher<sensor_msgs::msg::Imu>(SAMPLE_TOPIC.at(stream), rclcpp::QoS(1))));
    imu_info_pub_.insert(std::pair<stream_index_pair, rclcpp::Publisher<realsense_msgs::msg::IMUInfo>::SharedPtr>
      (stream, node_.create_publisher<realsense_msgs::msg::IMUInfo>(INFO_TOPIC.at(stream), rclcpp::QoS(1))));
    if (enable == true) {
      enable_[stream] = true;
      cfg_.enable_stream(stream.first, stream.second);
    }
  } else if (stream == POSE) {
    odom_pub_ = node_.create_publisher<nav_msgs::msg::Odometry>(SAMPLE_TOPIC.at(stream), rclcpp::QoS(1));
    if (enable == true) {
      enable_[stream] = true;
      cfg_.enable_stream(stream.first, stream.second);
    }
  } else {
    std::vector<int64_t> res;
    int fps;
    if (stream == COLOR || stream == DEPTH || stream == INFRA1 || stream == INFRA2) {
      os.str("");
      os << STREAM_NAME.at(stream.first) << stream.second << ".resolution";
      res = node_.declare_parameter(os.str(), rclcpp::ParameterValue(DEFAULT_IMAGE_RESOLUTION)).get<rclcpp::PARAMETER_INTEGER_ARRAY>();
      os.str("");
      os << STREAM_NAME.at(stream.first) << stream.second << ".fps";
      fps = node_.declare_parameter(os.str(), DEFAULT_IMAGE_FPS);
    } else if (stream == FISHEYE1 || stream == FISHEYE2) {
      auto param_desc = rcl_interfaces::msg::ParameterDescriptor();
      param_desc.read_only = true;
      os.str("");
      os << STREAM_NAME.at(stream.first) << stream.second << ".resolution";
      res = node_.declare_parameter(os.str(), rclcpp::ParameterValue(FISHEYE_RESOLUTION), param_desc).get<rclcpp::PARAMETER_INTEGER_ARRAY>();
      os.str("");
      os << STREAM_NAME.at(stream.first) << stream.second << ".fps";
      fps = node_.declare_parameter(os.str(), DEFAULT_IMAGE_FPS, param_desc);
    }

    VideoStreamInfo info(static_cast<int>(res[0]), static_cast<int>(res[1]), fps);

    stream_info_.insert(std::pair<stream_index_pair, VideoStreamInfo>(stream, info));
    image_pub_.insert(std::pair<stream_index_pair, rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr>
      (stream, node_.create_publisher<sensor_msgs::msg::Image>(SAMPLE_TOPIC.at(stream), rclcpp::QoS(1))));
    camera_info_pub_.insert(std::pair<stream_index_pair, rclcpp::Publisher<sensor_msgs::msg::CameraInfo>::SharedPtr>
      (stream, node_.create_publisher<sensor_msgs::msg::CameraInfo>(INFO_TOPIC.at(stream), rclcpp::QoS(1))));
    if (enable == true) {
      enable_[stream] = true;
      cfg_.enable_stream(stream.first, stream.second, info.width, info.height, STREAM_FORMAT.at(stream.first), info.fps);
    }
  } 
}

void RealSenseBase::publishImageTopic(const rs2::frame & frame, const rclcpp::Time & time)
{
  auto v_frame = frame.as<rs2::video_frame>();
  auto type = v_frame.get_profile().stream_type();
  auto index = v_frame.get_profile().stream_index();
  auto type_index = std::pair<rs2_stream, int>(type, index);
  auto width = v_frame.get_width();
  auto height = v_frame.get_height();

  cv::Mat cv_image = cv::Mat(height, width, CV_FORMAT.at(type));
  cv_image.data = const_cast<uchar *>(reinterpret_cast<const uchar *>(v_frame.get_data()));

  if (!node_.get_node_options().use_intra_process_comms()) {
    sensor_msgs::msg::Image::SharedPtr img;
    img = cv_bridge::CvImage(std_msgs::msg::Header(), MSG_ENCODING.at(type), cv_image).toImageMsg();
    //debug
    //RCLCPP_INFO(node_.get_logger(), "non-intra: timestamp: %f, address: %p", time.seconds(), reinterpret_cast<std::uintptr_t>(img.get()));
    //
    img->header.frame_id = OPTICAL_FRAME_ID.at(type_index);
    img->header.stamp = time;
    image_pub_[type_index]->publish(*img);
  } else {
    auto img = std::make_unique<sensor_msgs::msg::Image>();
    cv_bridge::CvImage(std_msgs::msg::Header(), MSG_ENCODING.at(type), cv_image).toImageMsg(*img);
    //debug
    //RCLCPP_INFO(node_.get_logger(), "intra: timestamp: %f, address: %p", time.seconds(), reinterpret_cast<std::uintptr_t>(img.get()));
    //
    img->header.frame_id = OPTICAL_FRAME_ID.at(type_index);
    img->header.stamp = time;
    image_pub_[type_index]->publish(std::move(img));
  }
  //TODO: need to update calibration data if anything is changed dynamically.
  camera_info_[type_index].header.stamp = time;
  camera_info_pub_[type_index]->publish(camera_info_[type_index]);
}

void RealSenseBase::updateVideoStreamCalibData(const rs2::video_stream_profile & video_profile)
{
  stream_index_pair type_index{video_profile.stream_type(), video_profile.stream_index()};
  auto intrinsic = video_profile.get_intrinsics();
  camera_info_[type_index].width = intrinsic.width;
  camera_info_[type_index].height = intrinsic.height;
  camera_info_[type_index].header.frame_id = OPTICAL_FRAME_ID.at(type_index);

  camera_info_[type_index].k.at(0) = intrinsic.fx;
  camera_info_[type_index].k.at(2) = intrinsic.ppx;
  camera_info_[type_index].k.at(4) = intrinsic.fy;
  camera_info_[type_index].k.at(5) = intrinsic.ppy;
  camera_info_[type_index].k.at(8) = 1;

  camera_info_[type_index].p.at(0) = camera_info_[type_index].k.at(0);
  camera_info_[type_index].p.at(1) = 0;
  camera_info_[type_index].p.at(2) = camera_info_[type_index].k.at(2);
  camera_info_[type_index].p.at(3) = 0;
  camera_info_[type_index].p.at(4) = 0;
  camera_info_[type_index].p.at(5) = camera_info_[type_index].k.at(4);
  camera_info_[type_index].p.at(6) = camera_info_[type_index].k.at(5);
  camera_info_[type_index].p.at(7) = 0;
  camera_info_[type_index].p.at(8) = 0;
  camera_info_[type_index].p.at(9) = 0;
  camera_info_[type_index].p.at(10) = 1;
  camera_info_[type_index].p.at(11) = 0;

  camera_info_[type_index].distortion_model = "plumb_bob";

  // set R (rotation matrix) values to identity matrix
  camera_info_[type_index].r.at(0) = 1.0;
  camera_info_[type_index].r.at(1) = 0.0;
  camera_info_[type_index].r.at(2) = 0.0;
  camera_info_[type_index].r.at(3) = 0.0;
  camera_info_[type_index].r.at(4) = 1.0;
  camera_info_[type_index].r.at(5) = 0.0;
  camera_info_[type_index].r.at(6) = 0.0;
  camera_info_[type_index].r.at(7) = 0.0;
  camera_info_[type_index].r.at(8) = 1.0;

  camera_info_[type_index].d.resize(5);
  for (int i = 0; i < 5; i++) {
    camera_info_[type_index].d.at(i) = intrinsic.coeffs[i];
  }

  if (type_index == DEPTH && enable_[DEPTH] && enable_[COLOR]) {
      camera_info_[type_index].p.at(3) = 0;     // Tx
      camera_info_[type_index].p.at(7) = 0;     // Ty
  }
}

void RealSenseBase::printDeviceInfo()
{
  auto camera_name = dev_.get_info(RS2_CAMERA_INFO_NAME);
  RCLCPP_INFO(node_.get_logger(), "+++++++++++++++++++++");
  RCLCPP_INFO(node_.get_logger(), "Device Name: %s", camera_name);
  auto serial_no = dev_.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER);
  RCLCPP_INFO(node_.get_logger(), "Device Serial No: %s", serial_no);
  auto fw_ver = dev_.get_info(RS2_CAMERA_INFO_FIRMWARE_VERSION);
  RCLCPP_INFO(node_.get_logger(), "Device FW Version: %s", fw_ver);
  auto pid = dev_.get_info(RS2_CAMERA_INFO_PRODUCT_ID);
  RCLCPP_INFO(node_.get_logger(), "Device Product ID: 0x%s", pid);
  RCLCPP_INFO(node_.get_logger(), "+++++++++++++++++++++");
}

void RealSenseBase::printSupportedStreamProfiles()
{
  auto sensor_list = dev_.query_sensors();
  
  for(auto sensor : sensor_list) {
    RCLCPP_INFO(node_.get_logger(), "Sensor Name: %s", sensor.get_info(RS2_CAMERA_INFO_NAME));
    auto profile_list = sensor.get_stream_profiles();
    printStreamProfiles(profile_list);
  }
}

void RealSenseBase::printActiveStreamProfiles()
{
  auto p_profile = pipeline_.get_active_profile();
  auto profile_list = p_profile.get_streams();
  printStreamProfiles(profile_list);
}

void RealSenseBase::printStreamProfiles(const std::vector<rs2::stream_profile> & profile_list)
{
  for (auto profile : profile_list) {
      auto p = profile.as<rs2::video_stream_profile>();
      RCLCPP_INFO(node_.get_logger(), "+++++++++++++++++++++");
      RCLCPP_INFO(node_.get_logger(), "Stream Name: %s", p.stream_name().c_str());
      RCLCPP_INFO(node_.get_logger(), "Type: %s", rs2_stream_to_string(p.stream_type()));
      RCLCPP_INFO(node_.get_logger(), "Index: %d", p.stream_index());
      RCLCPP_INFO(node_.get_logger(), "Unique id: %d", p.unique_id());
      RCLCPP_INFO(node_.get_logger(), "Format: %s", rs2_format_to_string(p.format()));
      RCLCPP_INFO(node_.get_logger(), "Width: %d", p.width());
      RCLCPP_INFO(node_.get_logger(), "Height: %d", p.height());
      RCLCPP_INFO(node_.get_logger(), "FPS: %d", p.fps());
  }
}

Result RealSenseBase::toggleStream(const stream_index_pair & stream, const rclcpp::Parameter & param)
{
  auto result = Result();
  result.successful = true;
  if (param.get_type() != rclcpp::ParameterType::PARAMETER_BOOL) {
    result.successful = false;
    result.reason = "Type should be boolean.";
    return result;
  }
  if (param.as_bool() == true && enable_[stream] == false) {
    if (stream == ACCEL || stream == GYRO || stream == POSE) {
      cfg_.enable_stream(stream.first, stream.second);
    } else {
      cfg_.enable_stream(stream.first, stream.second, stream_info_[stream].width, stream_info_[stream].height,
      STREAM_FORMAT.at(stream.first), stream_info_[stream].fps);
    }
    pipeline_.stop();
    rclcpp::sleep_for(200ms);
    pipeline_.start(cfg_, std::bind(&RealSenseBase::publishTopicsCallback, this, std::placeholders::_1));
    enable_[stream] = true;
    RCLCPP_INFO(node_.get_logger(), "%s stream is enabled.", STREAM_NAME.at(stream.first).c_str());
  } else if (param.as_bool() == false && enable_[stream] == true) {
    cfg_.disable_stream(stream.first, stream.second);
    enable_[stream] = false;
    RCLCPP_INFO(node_.get_logger(), "%s stream is disabled.", STREAM_NAME.at(stream.first).c_str());
  } else {
    result.successful = false;
    result.reason = "Parameter is equal to the previous value. Do nothing.";
  }
  return result;
}

Result RealSenseBase::changeResolution(const stream_index_pair & stream, const rclcpp::Parameter & param)
{
  auto result = Result();
  result.successful = true;
  if (param.get_type() != rclcpp::ParameterType::PARAMETER_INTEGER_ARRAY) {
    result.successful = false;
    result.reason = "Type should be integer array.";
    return result;
  }
  auto res = param.as_integer_array();
  cfg_.enable_stream(stream.first, stream.second, res[0], res[1], STREAM_FORMAT.at(stream.first), stream_info_[stream].fps);
  if (cfg_.can_resolve(pipeline_)) {
    if (enable_[stream] == true) {
      pipeline_.stop();
      pipeline_.start(cfg_, std::bind(&RealSenseBase::publishTopicsCallback, this, std::placeholders::_1));
    }
    stream_info_[stream].width = static_cast<int>(res[0]);
    stream_info_[stream].height = static_cast<int>(res[1]);
  } else {
    result.successful = false;
    result.reason = "Unsupported resolution.";
  }
  return result;
}

Result RealSenseBase::changeFPS(const stream_index_pair & stream, const rclcpp::Parameter & param)
{
  auto result = Result();
  result.successful = true;
  if (param.get_type() != rclcpp::ParameterType::PARAMETER_INTEGER) {
    result.successful = false;
    result.reason = "Type should be integer.";
    return result;
  }
  int fps = param.as_int();
  cfg_.enable_stream(stream.first, stream.second, stream_info_[stream].width, stream_info_[stream].height,
        STREAM_FORMAT.at(stream.first), fps);
  if (cfg_.can_resolve(pipeline_)) {
    if (enable_[stream] == true) {
      pipeline_.stop();
      pipeline_.start(cfg_, std::bind(&RealSenseBase::publishTopicsCallback, this, std::placeholders::_1));
    }
    stream_info_[stream].fps = fps;
  } else {
    result.successful = false;
    result.reason = "Unsupported configuration.";
  }
  return result;
}
}  // namespace realsense