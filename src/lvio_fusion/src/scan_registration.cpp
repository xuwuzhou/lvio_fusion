#include "lvio_fusion/lidar/scan_registration.h"
#include "lvio_fusion/ceres/lidar_error.hpp"
#include "lvio_fusion/lidar/feature.h"
#include "lvio_fusion/utility.h"
#include <pcl/filters/voxel_grid.h>
#include <pcl/kdtree/kdtree_flann.h>

namespace lvio_fusion
{

void ScanRegistration::AddScan(double time, Point3Cloud::Ptr new_scan)
{
    raw_point_clouds_.insert(std::make_pair(time, new_scan));

    Frames &all_kfs = map_->GetAllKeyFrames();
    for (auto iter = all_kfs.upper_bound(head_); iter != all_kfs.end() && iter->first < time; iter++)
    {
        PointICloud point_cloud;
        if (TimeAlign(iter->first, point_cloud))
        {
            Preprocess(point_cloud, iter->second);
            head_ = iter->first;
        }
    }
}

bool ScanRegistration::TimeAlign(double time, PointICloud &out)
{
    auto iter = raw_point_clouds_.upper_bound(time);
    Point3Cloud &pc2 = *(iter->second);
    double end_time = iter->first + cycle_time_ / 2;
    Point3Cloud &pc1 = *((--iter)->second);
    double start_time = iter->first - cycle_time_ / 2;
    Point3Cloud pc = pc1 + pc2;
    int size = pc.size();
    if (time - cycle_time_ / 2 < start_time || time + cycle_time_ / 2 > end_time)
    {
        return false;
    }
    auto start_iter = pc.begin() + size * (time - start_time - cycle_time_ / 2) / (end_time - start_time);
    auto end_iter = pc.begin() + size * (time - start_time + cycle_time_ / 2) / (end_time - start_time);
    Point3Cloud out3;
    out3.clear();
    out3.insert(out3.begin(), start_iter, end_iter);
    pcl::copyPointCloud(out3, out);
    raw_point_clouds_.erase(raw_point_clouds_.begin(), iter);
    return true;
}

void ScanRegistration::UndistortPoint(PointI &point, Frame::Ptr frame)
{
    double time_delta = (point.intensity - int(point.intensity));
    double time = frame->time - cycle_time_ * 0.5 + time_delta;
    SE3d pose = map_->ComputePose(time);
    auto p1 = lidar_->Sensor2World(Vector3d(point.x, point.y, point.z), pose);
    auto p2 = lidar_->World2Sensor(p1, frame->pose);
    point.x = p2.x();
    point.y = p2.y();
    point.z = p2.z();
}

inline void ScanRegistration::UndistortPointCloud(PointICloud &points, Frame::Ptr frame)
{
    for (auto &p : points)
    {
        UndistortPoint(p, frame);
    }
}

void ScanRegistration::Preprocess(PointICloud &points, Frame::Ptr frame)
{
    PointICloud points_sharp;
    PointICloud points_less_sharp;
    PointICloud points_flat;
    PointICloud point_less_flat;

    std::vector<int> indices;
    pcl::removeNaNFromPointCloud(points, points, indices);
    filter_points_by_distance(points, points, min_range_, max_range_);

    int raw_size = points.size();
    float start_ori = -atan2(points.points[0].y, points.points[0].x);
    float end_ori = -atan2(points.points[raw_size - 1].y, points.points[raw_size - 1].x) + 2 * M_PI;

    if (end_ori - start_ori > 3 * M_PI)
    {
        end_ori -= 2 * M_PI;
    }
    else if (end_ori - start_ori < M_PI)
    {
        end_ori += 2 * M_PI;
    }

    int size = raw_size;
    std::vector<PointICloud> scans_in_point_cloud(num_scans_);
    PointI point;
    bool half_passed = false;
    for (int i = 0; i < raw_size; i++)
    {
        point = points[i];
        float angle = atan(point.z / sqrt(point.x * point.x + point.y * point.y)) * 180 / M_PI;
        int scan_id = 0;
        if (num_scans_ == 16)
        {
            scan_id = int((angle + 15) / 2 + 0.5);
            if (scan_id > (num_scans_ - 1) || scan_id < 0)
            {
                size--;
                continue;
            }
        }
        else if (num_scans_ == 32)
        {
            scan_id = int((angle + 92.0 / 3.0) * 3.0 / 4.0);
            if (scan_id > (num_scans_ - 1) || scan_id < 0)
            {
                size--;
                continue;
            }
        }
        else if (num_scans_ == 64)
        {
            if (angle >= -8.83)
                scan_id = int((2 - angle) * 3.0 + 0.5);
            else
                scan_id = num_scans_ / 2 + int((-8.83 - angle) * 2.0 + 0.5);

            if (angle > 2 || angle < -24.33 || scan_id > 50 || scan_id < 0)
            {
                size--;
                continue;
            }
        }

        float ori = -atan2(point.y, point.x);
        if (!half_passed)
        {
            if (ori < start_ori - M_PI / 2)
            {
                ori += 2 * M_PI;
            }
            else if (ori > start_ori + M_PI * 3 / 2)
            {
                ori -= 2 * M_PI;
            }

            if (ori - start_ori > M_PI)
            {
                half_passed = true;
            }
        }
        else
        {
            ori += 2 * M_PI;
            if (ori < end_ori - M_PI * 3 / 2)
            {
                ori += 2 * M_PI;
            }
            else if (ori > end_ori + M_PI / 2)
            {
                ori -= 2 * M_PI;
            }
        }

        point.intensity = scan_id + cycle_time_ * (ori - start_ori) / (end_ori - start_ori);
        scans_in_point_cloud[scan_id].push_back(point);
    }

    points.clear();

    std::vector<int> start_index(num_scans_, 0);
    std::vector<int> end_index(num_scans_, 0);
    // ignore the first 5 and the last 5 points
    for (int i = 0; i < num_scans_; i++)
    {
        start_index[i] = points.size() + 5;
        points += scans_in_point_cloud[i];
        end_index[i] = points.size() - 6;
    }

    // calculate curvatures
    static float curvatures[150000]; // curvatures
    static int sort_index[150000];   // index
    static int is_feature[150000];   // is feature
    static int label[150000];        // Label 2: corner_sharp
                                     // Label 1: corner_less_sharp
                                     // Label -1: surf_flat
                                     // Label 0: surf_less_flat
    for (int i = 5; i < size - 5; i++)
    {
        float dx = points.points[i - 5].x + points.points[i - 4].x + points.points[i - 3].x + points.points[i - 2].x + points.points[i - 1].x - 10 * points.points[i].x + points.points[i + 1].x + points.points[i + 2].x + points.points[i + 3].x + points.points[i + 4].x + points.points[i + 5].x;
        float dy = points.points[i - 5].y + points.points[i - 4].y + points.points[i - 3].y + points.points[i - 2].y + points.points[i - 1].y - 10 * points.points[i].y + points.points[i + 1].y + points.points[i + 2].y + points.points[i + 3].y + points.points[i + 4].y + points.points[i + 5].y;
        float dz = points.points[i - 5].z + points.points[i - 4].z + points.points[i - 3].z + points.points[i - 2].z + points.points[i - 1].z - 10 * points.points[i].z + points.points[i + 1].z + points.points[i + 2].z + points.points[i + 3].z + points.points[i + 4].z + points.points[i + 5].z;

        curvatures[i] = dx * dx + dy * dy + dz * dz;
        sort_index[i] = i;
        is_feature[i] = 0;
        label[i] = 0;
    }

    // extract features
    static pcl::VoxelGrid<PointI> down_sampling;
    down_sampling.setLeafSize(0.2, 0.2, 0.2);
    for (int i = 0; i < num_scans_; i++)
    {
        if (end_index[i] - start_index[i] < 6) // too few
            continue;
        PointICloud::Ptr scan_less_flat(new PointICloud());
        // divide one scan into six segments
        for (int j = 0; j < 6; j++)
        {
            int sp = start_index[i] + (end_index[i] - start_index[i]) * j / 6;
            int ep = start_index[i] + (end_index[i] - start_index[i]) * (j + 1) / 6 - 1;

            std::sort(sort_index + sp, sort_index + ep + 1,
                      [](int i, int j) {
                          return (curvatures[i] < curvatures[j]);
                      });

            // extract sharp features
            int num_largest_curvature = 0;
            for (int k = ep; k >= sp; k--)
            {
                int si = sort_index[k];

                if (is_feature[si] == 0 && curvatures[si] > 0.1)
                {
                    num_largest_curvature++;
                    if (num_largest_curvature <= 2) // mark sharp
                    {
                        label[si] = 2;
                        points_sharp.push_back(points.points[si]);
                        points_less_sharp.push_back(points.points[si]);
                    }
                    else if (num_largest_curvature <= 20) // mark less sharp
                    {
                        label[si] = 1;
                        points_less_sharp.push_back(points.points[si]);
                    }
                    else
                    {
                        break;
                    }

                    is_feature[si] = 1;

                    // avoid too dense
                    for (int l = 1; l <= 5; l++)
                    {
                        float dx = points.points[si + l].x - points.points[si + l - 1].x;
                        float dy = points.points[si + l].y - points.points[si + l - 1].y;
                        float dz = points.points[si + l].z - points.points[si + l - 1].z;
                        if (dx * dx + dy * dy + dz * dz > 0.05)
                        {
                            break;
                        }
                        is_feature[si + l] = 1;
                    }
                    for (int l = -1; l >= -5; l--)
                    {
                        float dx = points.points[si + l].x - points.points[si + l + 1].x;
                        float dy = points.points[si + l].y - points.points[si + l + 1].y;
                        float dz = points.points[si + l].z - points.points[si + l + 1].z;
                        if (dx * dx + dy * dy + dz * dz > 0.05)
                        {
                            break;
                        }
                        is_feature[si + l] = 1;
                    }
                }
            }

            // extract flat features
            int num_smallest_corvature = 0;
            for (int k = sp; k <= ep; k++)
            {
                int si = sort_index[k];

                if (is_feature[si] == 0 &&
                    curvatures[si] < 0.1)
                {

                    label[si] = -1;
                    points_flat.push_back(points.points[si]);

                    num_smallest_corvature++;
                    if (num_smallest_corvature >= 4)
                    {
                        break;
                    }

                    is_feature[si] = 1;
                    for (int l = 1; l <= 5; l++)
                    {
                        float dx = points.points[si + l].x - points.points[si + l - 1].x;
                        float dy = points.points[si + l].y - points.points[si + l - 1].y;
                        float dz = points.points[si + l].z - points.points[si + l - 1].z;
                        if (dx * dx + dy * dy + dz * dz > 0.05)
                        {
                            break;
                        }

                        is_feature[si + l] = 1;
                    }
                    for (int l = -1; l >= -5; l--)
                    {
                        float dx = points.points[si + l].x - points.points[si + l + 1].x;
                        float dy = points.points[si + l].y - points.points[si + l + 1].y;
                        float dz = points.points[si + l].z - points.points[si + l + 1].z;
                        if (dx * dx + dy * dy + dz * dz > 0.05)
                        {
                            break;
                        }

                        is_feature[si + l] = 1;
                    }
                }
            }

            // mark other point as less flat
            for (int k = sp; k <= ep; k++)
            {
                if (label[k] <= 0)
                {
                    scan_less_flat->push_back(points.points[k]);
                }
            }
        }
        PointICloud scan_less_flat_ds;
        down_sampling.setInputCloud(scan_less_flat);
        down_sampling.filter(scan_less_flat_ds);
        point_less_flat += scan_less_flat_ds;
    }
    lidar::Feature::Ptr feature = lidar::Feature::Create(points_sharp, points_less_sharp, points_flat, point_less_flat);
    frame->feature_lidar = feature;
}

void ScanRegistration::Associate(Frame::Ptr current_frame, Frame::Ptr last_frame, ceres::Problem &problem, ceres::LossFunction *loss_function)
{
    PointICloud &points_sharp = current_frame->feature_lidar->points_sharp;
    PointICloud &points_less_sharp = current_frame->feature_lidar->points_less_sharp;
    PointICloud &points_flat = current_frame->feature_lidar->points_flat;
    PointICloud &points_less_flat = current_frame->feature_lidar->points_less_flat;
    PointICloud &points_less_sharp_last = last_frame->feature_lidar->points_less_sharp;
    PointICloud &points_less_flat_last = last_frame->feature_lidar->points_less_flat;

    static pcl::KdTreeFLANN<PointI> kdtree_sharp_last;
    static pcl::KdTreeFLANN<PointI> kdtree_flat_last;
    kdtree_sharp_last.setInputCloud(boost::make_shared<PointICloud>(points_less_sharp_last));
    kdtree_flat_last.setInputCloud(boost::make_shared<PointICloud>(points_less_flat_last));

    double *para_kf = current_frame->pose.data();
    double *para_last_kf = last_frame->pose.data();

    PointI point;
    std::vector<int> points_index;
    std::vector<float> points_distance;

    static const double distance_threshold = 0.5;
    static const double nearby_scan = 2.5;
    int num_points_sharp = points_sharp.points.size();
    int num_points_flat = points_flat.points.size();
    float *tf = lidar_->TransformMatrix(current_frame->pose, last_frame->pose).cast<float>().data();
    // find correspondence for corner features
    for (int i = 0; i < num_points_sharp; ++i)
    {
        //NOTE: Sophus is too slow
        // lidar_->Transform(points_sharp.points[i], current_frame->pose, last_frame->pose, point);  //  too slow
        ceres::SE3TransformPoint(tf, points_sharp.points[i].data, point.data);
        point.intensity = points_sharp.points[i].intensity;
        kdtree_sharp_last.nearestKSearch(point, 1, points_index, points_distance);

        int closest_index = -1, closest_index2 = -1;
        if (points_distance[0] < distance_threshold)
        {
            closest_index = points_index[0];
            int scan_id = int(points_less_sharp_last.points[closest_index].intensity);
            double distance_threshold = distance_threshold;
            // point b in the direction of increasing scan line
            for (int j = closest_index + 1; j < (int)points_less_sharp_last.points.size(); ++j)
            {
                // if in the same scan line, continue
                if (int(points_less_sharp_last.points[j].intensity) <= scan_id)
                    continue;

                // if not in nearby scans, end the loop
                if (int(points_less_sharp_last.points[j].intensity) > (scan_id + nearby_scan))
                    break;

                double point_distance = (points_less_sharp_last.points[j].x - point.x) *
                                            (points_less_sharp_last.points[j].x - point.x) +
                                        (points_less_sharp_last.points[j].y - point.y) *
                                            (points_less_sharp_last.points[j].y - point.y) +
                                        (points_less_sharp_last.points[j].z - point.z) *
                                            (points_less_sharp_last.points[j].z - point.z);

                if (point_distance < distance_threshold)
                {
                    // find nearer point
                    distance_threshold = point_distance;
                    closest_index2 = j;
                }
            }

            // point b in the direction of decreasing scan line
            for (int j = closest_index - 1; j >= 0; --j)
            {
                // if in the same scan line, continue
                if (int(points_less_sharp_last.points[j].intensity) >= scan_id)
                    continue;

                // if not in nearby scans, end the loop
                if (int(points_less_sharp_last.points[j].intensity) < (scan_id - nearby_scan))
                    break;

                double point_distance = (points_less_sharp_last.points[j].x - point.x) *
                                            (points_less_sharp_last.points[j].x - point.x) +
                                        (points_less_sharp_last.points[j].y - point.y) *
                                            (points_less_sharp_last.points[j].y - point.y) +
                                        (points_less_sharp_last.points[j].z - point.z) *
                                            (points_less_sharp_last.points[j].z - point.z);

                if (point_distance < distance_threshold)
                {
                    // find nearer point
                    distance_threshold = point_distance;
                    closest_index2 = j;
                }
            }
        }
        if (closest_index2 >= 0) // both A and B is valid
        {
            Vector3d curr_point(points_sharp.points[i].x,
                                points_sharp.points[i].y,
                                points_sharp.points[i].z);
            Vector3d last_point_a(points_less_sharp_last.points[closest_index].x,
                                  points_less_sharp_last.points[closest_index].y,
                                  points_less_sharp_last.points[closest_index].z);
            Vector3d last_point_b(points_less_sharp_last.points[closest_index2].x,
                                  points_less_sharp_last.points[closest_index2].y,
                                  points_less_sharp_last.points[closest_index2].z);
            ceres::CostFunction *cost_function = LidarEdgeError::Create(curr_point, last_point_a, last_point_b, lidar_);
            problem.AddResidualBlock(cost_function, loss_function, para_last_kf, para_kf);
        }
    }
    // find correspondence for plane features
    for (int i = 0; i < num_points_flat; ++i)
    {
        //NOTE: Sophus is too slow
        // lidar_->Transform(points_flat.points[i], current_frame->pose, last_frame->pose, point);
        ceres::SE3TransformPoint(tf, points_flat.points[i].data, point.data);
        point.intensity = points_flat.points[i].intensity;
        kdtree_flat_last.nearestKSearch(point, 1, points_index, points_distance);

        int closest_index = -1, closest_index2 = -1, closest_index3 = -1;
        if (points_distance[0] < distance_threshold && points_index[0] < points_less_flat_last.size())
        {
            closest_index = points_index[0];

            // get closest point's scan ID
            int scan_id = int(points_less_flat_last.points[closest_index].intensity);
            double distance_threshold2 = distance_threshold, distance_threshold3 = distance_threshold;

            // search in the direction of increasing scan line
            for (int j = closest_index + 1; j < (int)points_less_flat_last.points.size(); ++j)
            {
                // if not in nearby scans, end the loop
                if (int(points_less_flat_last.points[j].intensity) > (scan_id + nearby_scan))
                    break;

                double point_distance = (points_less_flat_last.points[j].x - point.x) *
                                            (points_less_flat_last.points[j].x - point.x) +
                                        (points_less_flat_last.points[j].y - point.y) *
                                            (points_less_flat_last.points[j].y - point.y) +
                                        (points_less_flat_last.points[j].z - point.z) *
                                            (points_less_flat_last.points[j].z - point.z);

                // if in the same or lower scan line
                if (int(points_less_flat_last.points[j].intensity) <= scan_id && point_distance < distance_threshold2)
                {
                    distance_threshold2 = point_distance;
                    closest_index2 = j;
                }
                // if in the higher scan line
                else if (int(points_less_flat_last.points[j].intensity) > scan_id && point_distance < distance_threshold3)
                {
                    distance_threshold3 = point_distance;
                    closest_index3 = j;
                }
            }

            // search in the direction of decreasing scan line
            for (int j = closest_index - 1; j >= 0; --j)
            {
                // if not in nearby scans, end the loop
                if (int(points_less_flat_last.points[j].intensity) < (scan_id - nearby_scan))
                    break;

                double point_distance = (points_less_flat_last.points[j].x - point.x) *
                                            (points_less_flat_last.points[j].x - point.x) +
                                        (points_less_flat_last.points[j].y - point.y) *
                                            (points_less_flat_last.points[j].y - point.y) +
                                        (points_less_flat_last.points[j].z - point.z) *
                                            (points_less_flat_last.points[j].z - point.z);

                // if in the same or higher scan line
                if (int(points_less_flat_last.points[j].intensity) >= scan_id && point_distance < distance_threshold2)
                {
                    distance_threshold2 = point_distance;
                    closest_index2 = j;
                }
                else if (int(points_less_flat_last.points[j].intensity) < scan_id && point_distance < distance_threshold3)
                {
                    // find nearer point
                    distance_threshold3 = point_distance;
                    closest_index3 = j;
                }
            }

            if (closest_index2 >= 0 && closest_index3 >= 0)
            {

                Vector3d curr_point(points_flat.points[i].x,
                                    points_flat.points[i].y,
                                    points_flat.points[i].z);
                Vector3d last_point_a(points_less_flat_last.points[closest_index].x,
                                      points_less_flat_last.points[closest_index].y,
                                      points_less_flat_last.points[closest_index].z);
                Vector3d last_point_b(points_less_flat_last.points[closest_index2].x,
                                      points_less_flat_last.points[closest_index2].y,
                                      points_less_flat_last.points[closest_index2].z);
                Vector3d last_point_c(points_less_flat_last.points[closest_index3].x,
                                      points_less_flat_last.points[closest_index3].y,
                                      points_less_flat_last.points[closest_index3].z);
                ceres::CostFunction *cost_function = LidarPlaneError::Create(curr_point, last_point_a, last_point_b, last_point_c, lidar_);
                problem.AddResidualBlock(cost_function, loss_function, para_last_kf, para_kf);
            }
        }
    }
}
} // namespace lvio_fusion