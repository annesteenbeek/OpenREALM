/**
* This file is part of OpenREALM.
*
* Copyright (C) 2018 Alexander Kern <laxnpander at gmail dot com> (Braunschweig University of Technology)
* For more information see <https://github.com/laxnpander/OpenREALM>
*
* OpenREALM is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* OpenREALM is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with OpenREALM. If not, see <http://www.gnu.org/licenses/>.
*/

#include <realm_vslam_base/open_vslam.h>

#include <openvslam/config.h>
#include <openvslam/data/landmark.h>
#include <openvslam/publish/map_publisher.h>
#include <openvslam/publish/frame_publisher.h>

using namespace realm;

OpenVslam::OpenVslam(const VisualSlamSettings::Ptr &vslam_set, const CameraSettings::Ptr &cam_set)
 : _nrof_keyframes(0),
   _resizing((*vslam_set)["resizing"].toDouble()),
   _path_vocabulary((*vslam_set)["path_vocabulary"].toString())
{
  // ov: OpenVSLAM
  // or: OpenREALM
  std::cout << "test" << std::endl;
  LOG_F(INFO, "Test1");
  _settings["Camera.name"] = "cam";
  _settings["Camera.setup"] = "monocular";
  _settings["Camera.model"] = "perspective";
  _settings["Camera.fx"] = (*cam_set)["fx"].toDouble();
  _settings["Camera.fy"] = (*cam_set)["fy"].toDouble();
  _settings["Camera.cx"] = (*cam_set)["cx"].toDouble();
  _settings["Camera.cy"] = (*cam_set)["cy"].toDouble();
  _settings["Camera.k1"] = (*cam_set)["k1"].toDouble();
  _settings["Camera.k2"] = (*cam_set)["k2"].toDouble();
  _settings["Camera.p1"] = (*cam_set)["p1"].toDouble();
  _settings["Camera.p2"] = (*cam_set)["p2"].toDouble();
  _settings["Camera.k3"] = (*cam_set)["k3"].toDouble();
  _settings["Camera.fps"] = (*cam_set)["fps"].toDouble();
  _settings["Camera.cols"] = (*cam_set)["width"].toInt();
  _settings["Camera.rows"] = (*cam_set)["height"].toInt();
  _settings["Feature.max_num_keypoints"] = (*vslam_set)["nrof_features"].toInt();
  _settings["Feature.scale_factor"] = (*vslam_set)["scale_factor"].toFloat();
  _settings["Feature.ini_fast_threshold"] = (*vslam_set)["ini_th_FAST"].toInt();
  _settings["Feature.min_fast_threshold"] = (*vslam_set)["min_th_FAST"].toInt();

  LOG_F(INFO, "Test2");
  auto ov_config = std::make_shared<openvslam::config>(_settings, "");
  LOG_F(INFO, "Test3");
  _vslam = std::make_shared<openvslam::system>(ov_config, _path_vocabulary);
  LOG_F(INFO, "Test4");
  _frame_publisher = _vslam->get_frame_publisher();
  LOG_F(INFO, "Test5");
  _map_publisher = _vslam->get_map_publisher();

  LOG_F(INFO, "Test6");
  _vslam->startup();
}

VisualSlamIF::State OpenVslam::track(Frame::Ptr &frame, const cv::Mat &T_c2w_initial)
{
  // Set image resizing accoring to settings
  frame->setImageResizeFactor(_resizing);

  // ORB SLAM returns a transformation from the world to the camera frame (T_w2c). In case we provide an initial guess
  // of the current pose, we have to invert this before, because in OpenREALM the standard is defined as T_c2w.
  cv::Mat T_w2c;
  openvslam::Mat44_t T_w2c_eigen;
  if (T_c2w_initial.empty())
  {
    T_w2c_eigen = _vslam->feed_monocular_frame(frame->getResizedImageRaw(), frame->getTimestamp()*10e-9);
    T_w2c = convertToCv(T_w2c_eigen);
  }
  else
  {
    // prior not yet implemented
  }

  // Draw frame of tracked features
  _mutex_last_drawn_frame.lock();
  _last_drawn_frame = _frame_publisher->draw_frame();
  _mutex_last_drawn_frame.unlock();

  // In case tracking was successfull and slam not lost
  if (!T_w2c.empty())
  {
    // Pose definition as 3x4 matrix, calculated as 4x4 with last row (0, 0, 0, 1)
    // ORB SLAM 2 pose is defined as T_w2c, however the more intuitive way to describe
    // it for mapping is T_c2w (camera to world) therefore invert the pose matrix
    cv::Mat T_c2w = invertPose(T_w2c);

    // Remove last row of 0,0,0,1
    T_c2w.pop_back();
    frame->setVisualPose(T_c2w);

    // Get list of keyframes
    std::vector<openvslam::data::keyframe*> keyframes;
    unsigned int current_nrof_keyframes = _map_publisher->get_keyframes(keyframes);

    // Set last keyframe
    _mutex_last_keyframe.lock();
    _last_keyframe = keyframes.back();
    _mutex_last_keyframe.unlock();

    cv::Mat surface_pts = getTrackedMapPoints();
    frame->setSurfacePoints(surface_pts);

    // Check current state of the slam
    if (_nrof_keyframes == 0 && current_nrof_keyframes > 0)
    {
      _nrof_keyframes = current_nrof_keyframes;
      return State::INITIALIZED;
    }
    else if (current_nrof_keyframes != _nrof_keyframes)
    {
      _nrof_keyframes = current_nrof_keyframes;
      return State::KEYFRAME_INSERT;
    }
    else
    {
      return State::FRAME_INSERT;
    }
  }
  return State::LOST;
}

void OpenVslam::close()
{
  _vslam->shutdown();
}

void OpenVslam::reset()
{
  _vslam->request_reset();
}

cv::Mat OpenVslam::getTrackedMapPoints() const
{
  _mutex_last_keyframe.lock();
  std::vector<openvslam::data::landmark*> landmarks = _last_keyframe->get_landmarks();
  _mutex_last_keyframe.unlock();

  cv::Mat points(landmarks.size(), 3, CV_64F);
  for (int i = 0; i < points.rows; ++i)
  {
    openvslam::Vec3_t pos = landmarks[i]->get_pos_in_world();
    points.at<double>(i, 0) = pos[0];
    points.at<double>(i, 1) = pos[1];
    points.at<double>(i, 2) = pos[2];
  }
}

bool OpenVslam::drawTrackedImage(cv::Mat &img) const
{
  img = getLastDrawnFrame();
  return !img.empty();
}

cv::Mat OpenVslam::getLastDrawnFrame() const
{
  std::lock_guard<std::mutex> lock(_mutex_last_drawn_frame);
  return _last_drawn_frame.clone();
}

cv::Mat OpenVslam::invertPose(const cv::Mat &pose) const
{
  cv::Mat pose_inv = cv::Mat::eye(4, 4, pose.type());
  cv::Mat R_t = (pose.rowRange(0, 3).colRange(0, 3)).t();
  cv::Mat t = -R_t*pose.rowRange(0, 3).col(3);
  R_t.copyTo(pose_inv.rowRange(0, 3).colRange(0, 3));
  t.copyTo(pose_inv.rowRange(0, 3).col(3));
  return pose_inv;
}

cv::Mat OpenVslam::convertToCv(const openvslam::Mat44_t &mat_eigen) const
{
  cv::Mat mat_cv(4, 4, CV_64F);

  return mat_cv;
}

void OpenVslam::printSettingsToLog()
{

}