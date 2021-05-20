#include "lvio_fusion/loop/pose_graph.h"
#include "lvio_fusion/ceres/pose_error.hpp"
#include "lvio_fusion/utility.h"

namespace lvio_fusion
{

Section &PoseGraph::AddSubMap(double old_time, double start_time, double end_time)
{
    Section new_submap;
    new_submap.C = end_time;
    new_submap.B = start_time;
    new_submap.A = old_time;
    submaps_[end_time] = new_submap;
    return submaps_[end_time];
}

/**
 * build active submaps and inner submaps
 * @param old_time      time of the first frame
 * @param start_time    time of the first loop frame
 * @return old frame of inner submaps; key is the first frame's time; value is the pose of the first frame
 */
Atlas PoseGraph::FilterOldSubmaps(double start, double end)
{
    auto start_iter = submaps_.lower_bound(start);
    auto end_iter = submaps_.upper_bound(end);
    Atlas active_sections = GetSections(start, end);
    if (start_iter != submaps_.end())
    {
        for (auto iter = start_iter; iter != end_iter; iter++)
        {
            if (iter->second.A <= start)
            {
                // remove outer submap
                auto outer_end = active_sections.upper_bound(iter->first);
                active_sections.erase(active_sections.begin(), outer_end);
                start = outer_end->first;
            }
            else
            {
                // remove inner submap
                auto inner_begin = active_sections.upper_bound(iter->second.A);
                auto inner_end = active_sections.upper_bound(iter->first);
                active_sections.erase(inner_begin, inner_end);
            }
        }
    }
    return active_sections;
}

void PoseGraph::UpdateSections(double time)
{
    static bool initialized = false;
    static Vector3d last_ori(1, 0, 0), B_ori(1, 0, 0), current_ori(0, 0, 0);
    static double accumulate_degree = 0;
    static double finished = 0;
    static std::queue<std::pair<double, Vector3d>> ori_buf;
    static const int buf_size = 3;

    if (Map::Instance().end && !turning)
    {
        current_section.C = time;
        sections_[current_section.A] = current_section;
        current_section.A = time;
        current_section.B = time;
        return;
    }

    if (time < finished)
        return;
    Frames active_kfs = Map::Instance().GetKeyFrames(finished, time);
    finished = time + epsilon;
    for (auto &pair : active_kfs)
    {
        if (!initialized)
        {
            current_section.A = pair.first;
            current_section.B = pair.first;
            initialized = true;
        }
        // average orientation of keyframes in buffer
        Vector3d ori = pair.second->pose.so3() * Vector3d::UnitX();
        ori_buf.push(std::make_pair(pair.first, ori));
        current_ori += ori;
        if (ori_buf.size() > buf_size)
        {
            current_ori -= ori_buf.front().second;
            last_ori = ori_buf.front().second;
            ori_buf.pop();
        }
        if (ori_buf.size() == buf_size)
        {
            double degree = vectors_degree_angle(last_ori, current_ori);
            // turning requires
            if (!turning && (degree >= 5 || vectors_degree_angle(B_ori, current_ori) > 10))
            {
                // if we have enough keyframes and total degree, create new section
                if (current_section.A == current_section.B ||
                    frames_distance(current_section.A, pair.first) > 20)
                {
                    current_section.C = pair.first;
                    sections_[current_section.A] = current_section;
                    current_section.A = pair.first;
                }
                turning = true;
            }
            // go straight requires
            else if (turning && degree < 3)
            {
                current_section.B = pair.first;
                B_ori = current_ori;
                turning = false;
            }
        }
    }
}

// [start, end]
// end = 0 -> all sections from start
Atlas PoseGraph::GetSections(double start, double end)
{
    UpdateSections(end);

    auto start_iter = sections_.lower_bound(start);
    auto end_iter = end == 0 ? sections_.end() : sections_.upper_bound(end);
    return Atlas(start_iter, end_iter);
}

Section PoseGraph::GetSection(double time)
{
    assert(time >= Map::Instance().keyframes.begin()->first);
    return (--sections_.upper_bound(time))->second;
}

bool PoseGraph::AddSection(double time)
{
    if ((!sections_.empty() && time > (--sections_.end())->second.C &&
         !turning && frames_distance(current_section.B, time) > 40))
    {
        current_section.C = time;
        sections_[current_section.A] = current_section;
        current_section.A = time;
        current_section.B = time;
        return true;
    }
    return false;
}

void PoseGraph::BuildProblem(Atlas &sections, Section &submap, adapt::Problem &problem)
{
    if (sections.empty())
        return;

    ceres::LocalParameterization *local_parameterization = new ceres::ProductParameterization(
        new ceres::EigenQuaternionParameterization(),
        new ceres::IdentityParameterization(3));

    Frame::Ptr old_frame = Map::Instance().GetKeyFrame(submap.A);
    Frame::Ptr start_frame = Map::Instance().GetKeyFrame(submap.B);
    double *para_old = old_frame->pose.data(), *para_start = start_frame->pose.data();
    problem.AddParameterBlock(para_old, SE3d::num_parameters, local_parameterization);
    problem.SetParameterBlockConstant(para_old);
    problem.AddParameterBlock(para_start, SE3d::num_parameters, local_parameterization);
    problem.SetParameterBlockConstant(para_start);

    Frame::Ptr last_frame = old_frame;
    double aa;
    for (auto &pair : sections)
    {
        auto frame_A = Map::Instance().GetKeyFrame(pair.second.A);
        double *para = frame_A->pose.data();
        problem.AddParameterBlock(para, SE3d::num_parameters, local_parameterization);
        double *para_last_kf = last_frame->pose.data();
        ceres::CostFunction *cost_function1 = PoseGraphError::Create(last_frame->pose, frame_A->pose);
        problem.AddResidualBlock(ProblemType::Other, cost_function1, NULL, para_last_kf, para);
        ceres::CostFunction *cost_function2 = RError::Create(frame_A->pose);
        problem.AddResidualBlock(ProblemType::Other, cost_function2, NULL, para);
        pair.second.pose = frame_A->pose;
        last_frame = frame_A;
    }
    ceres::CostFunction *cost_function = PoseGraphError::Create(last_frame->pose, start_frame->pose);
    problem.AddResidualBlock(ProblemType::Other, cost_function, NULL, last_frame->pose.data(), para_start);
}

void PoseGraph::Optimize(Atlas &sections, Section &submap, adapt::Problem &problem)
{
    if (sections.empty())
        return;

    ceres::Solver::Options options;
    options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
    ceres::Solver::Summary summary;
    ceres::Solve(options, &problem, &summary);

    Section last_section;
    double last_time = 0;
    for (auto &pair : sections)
    {
        if (last_time)
        {
            SE3d transfrom = Map::Instance().GetKeyFrame(last_time)->pose * last_section.pose.inverse();
            Frames forward_kfs = Map::Instance().GetKeyFrames(last_time + epsilon, pair.first - epsilon);
            ForwardUpdate(transfrom, forward_kfs);
        }
        last_time = pair.first;
        last_section = pair.second;
    }
    SE3d transfrom = Map::Instance().GetKeyFrame(last_time)->pose * last_section.pose.inverse();
    Frames forward_kfs = Map::Instance().GetKeyFrames(last_time + epsilon, submap.B - epsilon);
    ForwardUpdate(transfrom, forward_kfs);
}

// new pose = transform * old pose;
void PoseGraph::ForwardUpdate(SE3d transform, double start_time, bool need_lock)
{
    std::unique_lock<std::mutex> lock(frontend_->mutex, std::defer_lock);
    if (need_lock)
    {
        lock.lock();
    }
    Frames forward_kfs = Map::Instance().GetKeyFrames(start_time);
    Frame::Ptr last_frame = frontend_->last_frame;
    if (forward_kfs.find(last_frame->time) == forward_kfs.end())
    {
        forward_kfs[last_frame->time] = last_frame;
    }
    ForwardUpdate(transform, forward_kfs);
    frontend_->UpdateCache();
}

// new pose = transform * old pose;
void PoseGraph::ForwardUpdate(SE3d transform, const Frames &forward_kfs)
{
    for (auto &pair : forward_kfs)
    {
        pair.second->pose = transform * pair.second->pose;
        pair.second->Vw = transform.rotationMatrix() * pair.second->Vw;
    }
}

} // namespace lvio_fusion