#include "lvio_fusion/frontend.h"
#include "lvio_fusion/backend.h"
#include "lvio_fusion/ceres/imu_error.hpp"
#include "lvio_fusion/ceres/visual_error.hpp"
#include "lvio_fusion/map.h"
#include "lvio_fusion/utility.h"
#include "lvio_fusion/visual/camera.h"
#include "lvio_fusion/visual/feature.h"
#include "lvio_fusion/visual/landmark.h"
#include <opencv2/core/eigen.hpp>

namespace lvio_fusion
{

Frontend::Frontend(int num_features, int init, int tracking, int tracking_bad, int need_for_keyframe)
    : num_features_(num_features), num_features_init_(init), num_features_tracking_bad_(tracking_bad),
      num_features_needed_for_keyframe_(need_for_keyframe),
      matcher_(ORBMatcher(tracking_bad))
{
}

bool Frontend::AddFrame(lvio_fusion::Frame::Ptr frame)
{
    std::unique_lock<std::mutex> lock(mutex);
    //IMU
    if (Imu::Num())
    {
        if (last_frame)
            frame->SetNewBias(last_frame->GetImuBias());
        frame->preintegration = nullptr;
        frame->preintegrationFrame = nullptr;
    }
    //IMUEND
    current_frame = frame;

    switch (status)
    {
    case FrontendStatus::BUILDING:
    case FrontendStatus::LOST:
        // Reset();
        InitMap();
        break;
    case FrontendStatus::INITIALIZING:
    case FrontendStatus::TRACKING_GOOD:
    case FrontendStatus::TRACKING_TRY:
        Track();
        //NOTE: semantic map
        if (!current_frame->objects.empty())
        {
            current_frame->UpdateLabel();
        }
        break;
    }
    last_frame = current_frame;
    last_frame_pose_cache_ = last_frame->pose;
    return true;
}

void Frontend::AddImu(double time, Vector3d acc, Vector3d gyr)
{
    //IMU
    imuPoint imuMeas(acc, gyr, time);
    imuData_buf.push_back(imuMeas);
    //IMUEND
}
//IMU
void Frontend::PreintegrateIMU()
{
    if (last_frame)
    {
        if (backend_.lock()->GetInitializer()->initialized)
        {
            current_frame->bImu = true;
        }
        std::vector<imuPoint> imuDatafromlastframe;
        while (true)
        {
            if (imuData_buf.empty())
            {
                usleep(500);
                continue;
            }
            imuPoint imudata = imuData_buf.front();
            if (imudata.t < last_frame->time - 0.001)
            {
                imuData_buf.pop_front();
            }
            else if (imudata.t < current_frame->time - 0.001)
            {
                imuDatafromlastframe.push_back(imudata);
                imuData_buf.pop_front();
            }
            else
            {
                imuDatafromlastframe.push_back(imudata);
                break;
            }
        }
        const int n = imuDatafromlastframe.size() - 1;
        imu::Preintegration::Ptr ImuPreintegratedFromLastFrame = imu::Preintegration::Create(last_frame->GetImuBias());
        if (ImuPreintegratedFromLastKF == nullptr)
            ImuPreintegratedFromLastKF = imu::Preintegration::Create(last_frame->GetImuBias());

        if (imuDatafromlastframe[0].t - last_frame->time > 0.015) //freq*1.5
        {
            validtime = imuDatafromlastframe[0].t;
            if (backend_.lock()->GetInitializer()->initialized)
            {
                backend_.lock()->GetInitializer()->initialized = false;
                status = FrontendStatus::INITIALIZING;
            }
            ImuPreintegratedFromLastKF->isBad = true;
            ImuPreintegratedFromLastFrame->isBad = true;
        }
        else
        {
            for (int i = 0; i < n; i++)
            {
                if (imuDatafromlastframe[i + 1].t - imuDatafromlastframe[i].t > 0.015)
                {
                    validtime = imuDatafromlastframe[i + 1].t;
                    if (backend_.lock()->GetInitializer()->initialized)
                    {
                        backend_.lock()->GetInitializer()->initialized = false;
                        status = FrontendStatus::INITIALIZING;
                    }
                    ImuPreintegratedFromLastKF->isBad = true;
                    ImuPreintegratedFromLastFrame->isBad = true;
                    break;
                }
                double tstep;
                double tab;
                Vector3d acc0, angVel0; //第一帧
                Vector3d acc, angVel;   //第二帧
                if ((i == 0) && (i < (n - 1)))
                {
                    tab = imuDatafromlastframe[i + 1].t - imuDatafromlastframe[i].t;
                    double tini = imuDatafromlastframe[i].t - last_frame->time;
                    acc = imuDatafromlastframe[i + 1].a;
                    acc0 = (imuDatafromlastframe[i].a -
                            (imuDatafromlastframe[i + 1].a - imuDatafromlastframe[i].a) * (tini / tab));
                    angVel = imuDatafromlastframe[i + 1].w;
                    angVel0 = (imuDatafromlastframe[i].w -
                               (imuDatafromlastframe[i + 1].w - imuDatafromlastframe[i].w) * (tini / tab));
                    tstep = imuDatafromlastframe[i + 1].t - last_frame->time;
                }
                else if (i < (n - 1))
                {
                    acc = (imuDatafromlastframe[i + 1].a);
                    acc0 = (imuDatafromlastframe[i].a);
                    angVel = (imuDatafromlastframe[i + 1].w);
                    angVel0 = (imuDatafromlastframe[i].w);
                    tstep = imuDatafromlastframe[i + 1].t - imuDatafromlastframe[i].t;
                }
                else if ((i > 0) && (i == (n - 1)))
                {
                    tab = imuDatafromlastframe[i + 1].t - imuDatafromlastframe[i].t;
                    double tend = imuDatafromlastframe[i + 1].t - current_frame->time;

                    acc = (imuDatafromlastframe[i + 1].a -
                           (imuDatafromlastframe[i + 1].a - imuDatafromlastframe[i].a) * (tend / tab));
                    acc0 = imuDatafromlastframe[i].a;
                    angVel = (imuDatafromlastframe[i + 1].w -
                              (imuDatafromlastframe[i + 1].w - imuDatafromlastframe[i].w) * (tend / tab));
                    angVel0 = imuDatafromlastframe[i].w;
                    tstep = current_frame->time - imuDatafromlastframe[i].t;
                }
                else if ((i == 0) && (i == (n - 1)))
                {
                    tab = imuDatafromlastframe[i + 1].t - imuDatafromlastframe[i].t;
                    double tini = imuDatafromlastframe[i].t - last_frame->time;
                    double tend = imuDatafromlastframe[i + 1].t - current_frame->time;
                    acc = (imuDatafromlastframe[i + 1].a -
                           (imuDatafromlastframe[i + 1].a - imuDatafromlastframe[i].a) * (tend / tab));
                    acc0 = (imuDatafromlastframe[i].a -
                            (imuDatafromlastframe[i + 1].a - imuDatafromlastframe[i].a) * (tini / tab));
                    angVel = (imuDatafromlastframe[i + 1].w -
                              (imuDatafromlastframe[i + 1].w - imuDatafromlastframe[i].w) * (tend / tab));
                    angVel0 = (imuDatafromlastframe[i].w -
                               (imuDatafromlastframe[i + 1].w - imuDatafromlastframe[i].w) * (tini / tab));
                    tstep = current_frame->time - last_frame->time;
                }
                if (tab == 0)
                    continue;
                ImuPreintegratedFromLastKF->Append(tstep, acc, angVel, acc0, angVel0);
                ImuPreintegratedFromLastFrame->Append(tstep, acc, angVel, acc0, angVel0);
            }
            if ((n == 0))
            {
                validtime = imuDatafromlastframe[0].t;
                if (backend_.lock()->GetInitializer()->initialized)
                {
                    backend_.lock()->GetInitializer()->initialized = false;
                    status = FrontendStatus::INITIALIZING;
                }
                ImuPreintegratedFromLastKF->isBad = true;
                ImuPreintegratedFromLastFrame->isBad = true;
            }
        }

        if (!ImuPreintegratedFromLastKF->isBad) //如果imu帧没坏，就赋给当前帧
            current_frame->preintegration = ImuPreintegratedFromLastKF;
        else
        {
            current_frame->preintegration = nullptr;
            current_frame->bImu = false;
            // current_frame->ImuBias.linearized_ba=Vector3d::Zero();
            // current_frame->ImuBias.linearized_bg=Vector3d::Zero();
        }

        if (!ImuPreintegratedFromLastFrame->isBad)
            current_frame->preintegrationFrame = ImuPreintegratedFromLastFrame;
        // if(!ImuPreintegratedFromLastFrame->isBad)
        //     LOG(INFO)<<current_frame->time-1.40364e+09+8.60223e+07<<" :"<<ImuPreintegratedFromLastFrame->delta_q.toRotationMatrix().eulerAngles(0,1,2).transpose();
    }
}
//IMUEND

bool Frontend::Track()
{
    //IMU
    if (Imu::Num())
    {
        PreintegrateIMU();
        //current_frame->preintegration->PreintegrateIMU(imuDatafromlastframe,last_frame->time, current_frame->time);
        if (backend_.lock()->GetInitializer()->initialized)
        {
            PredictStateIMU();
        }
    }
    if (!Imu::Num() || !backend_.lock()->GetInitializer()->initialized || (backend_.lock()->GetInitializer()->initialized && current_frame->preintegrationFrame == nullptr))
    {
        current_frame->pose = relative_i_j * last_frame_pose_cache_;
    }

    if (Imu::Num())
    {
        if (last_key_frame)
        {
            current_frame->last_keyframe = last_key_frame;
            current_frame->SetNewBias(last_key_frame->GetImuBias());
        }
    }
    //IMUEND

    int num_inliers = TrackLastFrame(last_frame);
    bool success = num_inliers > num_features_tracking_bad_ &&
                   (current_frame->pose.translation() - last_frame_pose_cache_.translation()).norm() < 5;

    if (!success)
    {
        num_inliers = Relocate(last_frame);
        success = num_inliers > num_features_tracking_bad_ &&
                  (current_frame->pose.translation() - last_frame_pose_cache_.translation()).norm() < 5;
    }

    if (status == FrontendStatus::INITIALIZING)
    {
        if (!success)
        {
            status = FrontendStatus::BUILDING;
        }
    }
    else
    {
        if (success)
        {
            // tracking good
            status = FrontendStatus::TRACKING_GOOD;
        }
        else
        {
            // tracking bad, but give a chance
            status = FrontendStatus::TRACKING_TRY;
            current_frame->features_left.clear();
            InitMap();
            current_frame->pose = last_frame_pose_cache_ * relative_i_j;
            LOG(INFO) << "Lost, try again!";
        }
    }

    if ((status == FrontendStatus::TRACKING_GOOD && num_inliers < num_features_needed_for_keyframe_) ||
        (status == FrontendStatus::INITIALIZING && current_frame->time - last_key_frame->time > 0.25))
    {
        CreateKeyframe();
    }

    // smooth trajectory
    relative_i_j = se3_slerp(relative_i_j, last_frame_pose_cache_.inverse() * current_frame->pose, 0.5);
    return true;
}

int Frontend::TrackLastFrame(Frame::Ptr base_frame)
{
    std::vector<cv::Point2f> kps_last, kps_current;
    std::vector<visual::Landmark::Ptr> landmarks;
    std::vector<uchar> status;
    // use LK flow to estimate points in the last image
    for (auto &pair_feature : base_frame->features_left)
    {
        // use project point
        auto feature = pair_feature.second;
        auto landmark = feature->landmark.lock();
        auto px = Camera::Get()->World2Pixel(position_cache_[landmark->id], current_frame->pose);
        kps_last.push_back(feature->keypoint);
        kps_current.push_back(cv::Point2f(px[0], px[1]));
        landmarks.push_back(landmark);
    }
    optical_flow(base_frame->image_left, current_frame->image_left, kps_last, kps_current, status);

    // Solve PnP
    std::vector<cv::Point3f> points_3d;
    std::vector<cv::Point2f> points_2d;
    std::vector<int> map;
    for (size_t i = 0; i < status.size(); ++i)
    {
        if (status[i])
        {
            map.push_back(i);
            points_2d.push_back(kps_current[i]);
            Vector3d p = position_cache_[landmarks[i]->id];
            points_3d.push_back(cv::Point3f(p.x(), p.y(), p.z()));
        }
    }

    int num_good_pts = 0;
    cv::Mat rvec, tvec, inliers, cv_R;
    if ((int)points_2d.size() > num_features_tracking_bad_ &&
        cv::solvePnPRansac(points_3d, points_2d, Camera::Get()->K, Camera::Get()->D, rvec, tvec, false, 100, 8.0F, 0.98, inliers, cv::SOLVEPNP_EPNP))
    {
        cv::Rodrigues(rvec, cv_R);
        Matrix3d R;
        cv::cv2eigen(cv_R, R);
        if (!Imu::Num() || !backend_.lock()->GetInitializer()->initialized || (backend_.lock()->GetInitializer()->initialized && current_frame->preintegrationFrame == nullptr)) //IMU
            current_frame->pose = (Camera::Get()->extrinsic * SE3d(SO3d(R), Vector3d(tvec.at<double>(0, 0), tvec.at<double>(1, 0), tvec.at<double>(2, 0)))).inverse();

        cv::Mat img_track = current_frame->image_left;
        cv::cvtColor(img_track, img_track, cv::COLOR_GRAY2RGB);
        for (int r = 0; r < inliers.rows; r++)
        {
            int i = map[inliers.at<int>(r)];
            cv::arrowedLine(img_track, kps_current[i], kps_last[i], cv::Scalar(0, 255, 0), 1, 8, 0, 0.2);
            cv::circle(img_track, kps_current[i], 2, cv::Scalar(0, 255, 0), cv::FILLED);
            auto feature = visual::Feature::Create(current_frame, kps_current[i], landmarks[i]);
            current_frame->AddFeature(feature);
            num_good_pts++;
        }
        cv::imshow("tracking", img_track);
        cv::waitKey(1);
    }

    LOG(INFO) << "Find " << num_good_pts << " in the last image.";
    return num_good_pts;
}

int Frontend::Relocate(Frame::Ptr base_frame)
{
    std::vector<cv::Point2f> kps_left, kps_right, kps_current;
    std::vector<Vector3d> pbs;
    int num_good_pts = matcher_.Relocate(base_frame, current_frame, kps_left, kps_right, kps_current, pbs);
    if (num_good_pts > num_features_tracking_bad_)
    {
        for (int i = 0; i < kps_left.size(); i++)
        {
            auto new_landmark = visual::Landmark::Create(pbs[i]);
            auto new_left_feature = visual::Feature::Create(base_frame, kps_left[i], new_landmark);
            auto new_right_feature = visual::Feature::Create(base_frame, kps_right[i], new_landmark);
            new_right_feature->is_on_left_image = false;
            new_landmark->AddObservation(new_left_feature);
            new_landmark->AddObservation(new_right_feature);
            base_frame->AddFeature(new_left_feature);
            base_frame->AddFeature(new_right_feature);
            Map::Instance().InsertLandmark(new_landmark);
            position_cache_[new_landmark->id] = new_landmark->ToWorld();

            auto feature = visual::Feature::Create(current_frame, kps_current[i], new_landmark);
            current_frame->AddFeature(feature);
            new_landmark->AddObservation(feature);
        }
    }
    if (base_frame != last_key_frame)
    {
        // first, add new observations of old points
        for (auto &pair_feature : base_frame->features_left)
        {
            auto feature = pair_feature.second;
            auto landmark = feature->landmark.lock();
            landmark->AddObservation(feature);
        }

        // insert!
        current_frame->id++;
        Map::Instance().InsertKeyFrame(base_frame);
        last_key_frame = base_frame;
        //IMU
        current_frame->preintegration = current_frame->preintegrationFrame;
        ImuPreintegratedFromLastKF = current_frame->preintegrationFrame;
        //IMUEND
        LOG(INFO) << "Make last frame a keyframe " << base_frame->id;

        // update backend because we have a new keyframe
        backend_.lock()->UpdateMap();
    }
    return num_good_pts;
}

bool Frontend::InitMap()
{
    int num_new_features = DetectNewFeatures();
    if (num_new_features < num_features_init_)
        return false;

    if (Imu::Num())
    {
        status = FrontendStatus::INITIALIZING;
        backend_.lock()->GetInitializer()->initialized = false; //IMU
        ImuPreintegratedFromLastKF = nullptr;                   //IMU
        validtime = current_frame->time;                        //IMU
    }
    else
    {
        status = FrontendStatus::TRACKING_GOOD;
    }
    //IMUEND
    // the first frame is a keyframe
    Map::Instance().InsertKeyFrame(current_frame);
    last_key_frame = current_frame;

    LOG(INFO) << "Initial map created with " << num_new_features << " map points";

    // update backend because we have a new keyframe
    backend_.lock()->UpdateMap();
    return true;
}

int Frontend::DetectNewFeatures()
{
    int num_tries = 0;
    int num_triangulated_pts = 0;
    int num_good_pts = 0;
    while (num_tries++ < 2 && current_frame->features_left.size() < 0.8 * num_features_)
    {
        cv::Mat mask(current_frame->image_left.size(), CV_8UC1, 255);
        for (auto &pair_feature : current_frame->features_left)
        {
            auto feature = pair_feature.second;
            cv::circle(mask, feature->keypoint, 20, 0, cv::FILLED);
        }

        std::vector<cv::Point2f> kps_left, kps_right; // must be point2f
        cv::goodFeaturesToTrack(current_frame->image_left, kps_left, num_features_ - current_frame->features_left.size(), 0.01, 20, mask);

        // use LK flow to estimate points in the right image
        kps_right = kps_left;
        std::vector<uchar> status;
        optical_flow(current_frame->image_left, current_frame->image_right, kps_left, kps_right, status);

        // triangulate new points
        for (size_t i = 0; i < kps_left.size(); ++i)
        {
            if (status[i])
            {
                num_good_pts++;
                // triangulation
                Vector2d kp_left = cv2eigen(kps_left[i]);
                Vector2d kp_right = cv2eigen(kps_right[i]);
                Vector3d pb = Vector3d::Zero();
                triangulate(Camera::Get()->extrinsic.inverse(), Camera::Get(1)->extrinsic.inverse(),
                            Camera::Get()->Pixel2Sensor(kp_left), Camera::Get(1)->Pixel2Sensor(kp_right), pb);
                if ((Camera::Get()->Robot2Pixel(pb) - kp_left).norm() < 0.5 && (Camera::Get(1)->Robot2Pixel(pb) - kp_right).norm() < 0.5)
                {
                    auto new_landmark = visual::Landmark::Create(pb);
                    auto new_left_feature = visual::Feature::Create(current_frame, kps_left[i], new_landmark);
                    auto new_right_feature = visual::Feature::Create(current_frame, kps_right[i], new_landmark);
                    new_right_feature->is_on_left_image = false;
                    new_landmark->AddObservation(new_left_feature);
                    new_landmark->AddObservation(new_right_feature);
                    current_frame->AddFeature(new_left_feature);
                    current_frame->AddFeature(new_right_feature);
                    Map::Instance().InsertLandmark(new_landmark);
                    position_cache_[new_landmark->id] = new_landmark->ToWorld();
                    num_triangulated_pts++;
                }
            }
        }
    }

    LOG(INFO) << "Find " << num_good_pts << " in the right image.";
    LOG(INFO) << "new landmarks: " << num_triangulated_pts;
    return num_triangulated_pts;
}

void Frontend::CreateKeyframe()
{
    //IMU
    if (Imu::Num())
    {
        ImuPreintegratedFromLastKF = nullptr;
    }
    //IMUEND
    // first, add new observations of old points
    for (auto &pair_feature : current_frame->features_left)
    {
        auto feature = pair_feature.second;
        auto landmark = feature->landmark.lock();
        landmark->AddObservation(feature);
    }

    // detect new features, track in right image and triangulate map points
    DetectNewFeatures();

    // insert!
    Map::Instance().InsertKeyFrame(current_frame);
    last_key_frame = current_frame;
    LOG(INFO) << "Add a keyframe " << current_frame->id;

    // update backend because we have a new keyframe
    backend_.lock()->UpdateMap();
}

// TODO
bool Frontend::Reset()
{
    backend_.lock()->Pause();
    Map::Instance().Reset();
    backend_.lock()->Continue();
    status = FrontendStatus::BUILDING;
    LOG(INFO) << "Reset Succeed";
    return true;
}

void Frontend::UpdateCache()
{
    position_cache_.clear();
    for (auto &pair_feature : last_frame->features_left)
    {
        auto feature = pair_feature.second;
        auto camera_point = feature->landmark.lock();
        position_cache_[camera_point->id] = camera_point->ToWorld();
    }
    last_frame_pose_cache_ = last_frame->pose;
}
//IMU
void Frontend::UpdateFrameIMU(const Bias &bias_)
{
    if (last_key_frame->preintegration != nullptr)
        last_key_frame->bImu = true;
    last_frame->SetNewBias(bias_);
    current_frame->SetNewBias(bias_);
    Vector3d Gz;
    Gz << 0, 0, -Imu::Get()->G;
    Gz = Imu::Get()->Rwg * Gz;
    Vector3d twb1;
    Matrix3d Rwb1;
    Vector3d Vwb1;
    double t12; // 时间间隔
    Vector3d twb2;
    Matrix3d Rwb2;
    Vector3d Vwb2;
    if (last_frame->last_keyframe && last_frame->preintegration)
    {
        if (fabs(last_frame->time - last_frame->last_keyframe->time) > 0.001)
        {
            twb1 = last_frame->last_keyframe->GetImuPosition();
            Rwb1 = last_frame->last_keyframe->GetImuRotation();
            Vwb1 = last_frame->last_keyframe->GetVelocity();
            t12 = last_frame->preintegration->sum_dt;
            Rwb2 = Rwb1 * last_frame->preintegration->GetUpdatedDeltaRotation();
            twb2 = twb1 + Vwb1 * t12 + 0.5f * t12 * t12 * Gz + Rwb1 * last_frame->preintegration->GetUpdatedDeltaPosition();
            Vwb2 = Vwb1 + Gz * t12 + Rwb1 * last_frame->preintegration->GetUpdatedDeltaVelocity();
            last_frame->SetPose(Rwb2, twb2);
            last_frame->SetVelocity(Vwb2);
        }
    }
    if (fabs(current_frame->time - current_frame->last_keyframe->time) > 0.001 && current_frame->preintegration && current_frame->last_keyframe)
    {
        twb1 = current_frame->last_keyframe->GetImuPosition();
        Rwb1 = current_frame->last_keyframe->GetImuRotation();
        Vwb1 = current_frame->last_keyframe->GetVelocity();
        t12 = current_frame->preintegration->sum_dt;
        Rwb2 = Rwb1 * current_frame->preintegration->GetUpdatedDeltaRotation();
        twb2 = twb1 + Vwb1 * t12 + 0.5f * t12 * t12 * Gz + Rwb1 * current_frame->preintegration->GetUpdatedDeltaPosition();
        Vwb2 = Vwb1 + Gz * t12 + Rwb1 * current_frame->preintegration->GetUpdatedDeltaVelocity();
        current_frame->SetPose(Rwb2, twb2);
        current_frame->SetVelocity(Vwb2);
    }
}

void Frontend::PredictStateIMU()
{
    if (Map::Instance().mapUpdated && last_key_frame)
    {
        Vector3d Gz;
        Gz << 0, 0, -Imu::Get()->G;
        Gz = Imu::Get()->Rwg * Gz;
        double t12 = current_frame->preintegration->sum_dt;
        Vector3d twb1 = last_key_frame->GetImuPosition();
        Matrix3d Rwb1 = last_key_frame->GetImuRotation();
        Vector3d Vwb1 = last_key_frame->Vw;

        Matrix3d Rwb2 = NormalizeRotation(Rwb1 * current_frame->preintegration->GetDeltaRotation(last_key_frame->GetImuBias()).toRotationMatrix());
        Vector3d twb2 = twb1 + Vwb1 * t12 + 0.5f * t12 * t12 * Gz + Rwb1 * current_frame->preintegration->GetDeltaPosition(last_key_frame->GetImuBias());
        Vector3d Vwb2 = Vwb1 + t12 * Gz + Rwb1 * current_frame->preintegration->GetDeltaVelocity(last_key_frame->GetImuBias());
        current_frame->SetVelocity(Vwb2);
        current_frame->SetPose(Rwb2, twb2);
        current_frame->SetNewBias(last_key_frame->GetImuBias());
        Map::Instance().mapUpdated = false; //IMU
    }
    else if (!Map::Instance().mapUpdated)
    {

        Vector3d Gz;
        Gz << 0, 0, -Imu::Get()->G;
        Gz = Imu::Get()->Rwg * Gz;
        double t12 = current_frame->preintegrationFrame->sum_dt;
        Vector3d twb1 = last_frame->GetImuPosition();
        Matrix3d Rwb1 = last_frame->GetImuRotation();
        Vector3d Vwb1 = last_frame->Vw;
        Matrix3d Rwb2 = NormalizeRotation(Rwb1 * current_frame->preintegrationFrame->GetDeltaRotation(last_frame->GetImuBias()).toRotationMatrix());
        Vector3d twb2 = twb1 + Vwb1 * t12 + 0.5f * t12 * t12 * Gz + Rwb1 * current_frame->preintegrationFrame->GetDeltaPosition(last_frame->GetImuBias());
        Vector3d Vwb2 = Vwb1 + t12 * Gz + Rwb1 * current_frame->preintegrationFrame->GetDeltaVelocity(last_frame->GetImuBias());
        LOG(INFO) << "PredictStateIMU  " << current_frame->time - 1.40364e+09 + 8.60223e+07 << "  T12  " << t12 << "  Vwb1  " << Vwb1.transpose() << " Vwb2  " << Vwb2.transpose();
        // LOG(INFO)<<"   RdV    "<<((Rwb1)*current_frame->preintegrationFrame->GetDeltaVelocity(last_frame->GetImuBias())).transpose()<<"   dV      "<<(current_frame->preintegrationFrame->GetDeltaVelocity(last_frame->GetImuBias())).transpose();
        // LOG(INFO)<<"  dP  "<<current_frame->preintegrationFrame->GetDeltaPosition(last_frame->GetImuBias()).transpose();
        Eigen::Vector3d dr = current_frame->preintegrationFrame->GetDeltaRotation(last_frame->GetImuBias()).toRotationMatrix().eulerAngles(0, 1, 2);
        Eigen::Vector3d dr_ = current_frame->preintegrationFrame->delta_q.toRotationMatrix().eulerAngles(0, 1, 2);

        LOG(INFO) << "  dR  " << dr.transpose() << "  dR_" << dr_.transpose();                                                                     //current_frame->preintegrationFrame->GetDeltaRotation(last_frame->GetImuBias()).w()<<" "<<current_frame->preintegrationFrame->GetDeltaRotation(last_frame->GetImuBias()).x()<<" "<<current_frame->preintegrationFrame->GetDeltaRotation(last_frame->GetImuBias()).y()<<" "<<current_frame->preintegrationFrame->GetDeltaRotation(last_frame->GetImuBias()).z();
        LOG(INFO) << "  a" << last_frame->GetImuBias().linearized_ba.transpose() << "   g " << last_frame->GetImuBias().linearized_bg.transpose(); //current_frame->preintegrationFrame->delta_q.w()<<" "<<current_frame->preintegrationFrame->delta_q.x()<<" "<<current_frame->preintegrationFrame->delta_q.y()<<" "<<current_frame->preintegrationFrame->delta_q.z();
        LOG(INFO) << "Rwb1" << Rwb1.eulerAngles(0, 1, 2).transpose();
        LOG(INFO) << "Rwb2" << Rwb2.eulerAngles(0, 1, 2).transpose();

        current_frame->SetVelocity(Vwb2);
        current_frame->SetPose(Rwb2, twb2);
        current_frame->SetNewBias(last_frame->GetImuBias());
    }
}

} // namespace lvio_fusion