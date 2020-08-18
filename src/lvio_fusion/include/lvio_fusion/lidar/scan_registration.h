#ifndef lvio_fusion_REGISTRATION_H
#define lvio_fusion_REGISTRATION_H

#include "lvio_fusion/common.h"
#include "lvio_fusion/lidar/lidar.hpp"
#include "lvio_fusion/map.h"
#include <ceres/ceres.h>

namespace lvio_fusion
{

class Frontend;

class ScanRegistration
{
public:
    typedef std::shared_ptr<ScanRegistration> Ptr;

    ScanRegistration(int num_scans, double cycle_time, double minimum_range, double deskew) : num_scans_(num_scans), cycle_time_(cycle_time), minimum_range_(minimum_range), deskew_(deskew) {}

    void SetLidar(Lidar::Ptr lidar)
    {
        lidar_ = lidar;
    }

    void SetMap(Map::Ptr map)
    {
        map_ = map;
    }

    void AddScan(double time, Point3Cloud::Ptr new_scan);

    void Associate(Frame::Ptr current_frame, Frame::Ptr last_frame, ceres::Problem &problem, ceres::LossFunction *loss_function);

private:
    void UndistortPoint(PointI &p, Frame::Ptr frame);
    void UndistortPointCloud(PointICloud &pc, Frame::Ptr frame);

    void Deskew(Frame::Ptr frame);

    bool TimeAlign(double time, PointICloud &out);

    void Preprocess(PointICloud &pc, Frame::Ptr frame);

    void Transform(const PointI &in, Frame::Ptr from, Frame::Ptr to, PointI &out);

    Map::Ptr map_;
    std::map<double, Point3Cloud::Ptr> raw_point_clouds_;
    double head_ = 0; // header of the frames' time which already has a point cloud
    Lidar::Ptr lidar_;

    // params
    const int num_scans_;
    const double cycle_time_;
    const double minimum_range_;
    const bool deskew_;
};

} // namespace lvio_fusion

#endif // lvio_fusion_REGISTRATION_H
