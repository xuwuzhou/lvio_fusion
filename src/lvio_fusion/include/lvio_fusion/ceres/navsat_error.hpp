#ifndef lvio_fusion_NAVSAT_ERROR_H
#define lvio_fusion_NAVSAT_ERROR_H

#include "lvio_fusion/ceres/base.hpp"

namespace lvio_fusion
{

class NavsatInitRError
{
public:
    NavsatInitRError(Vector3d p0, Vector3d p1)
        : x0_(p0.x()), y0_(p0.y()), z0_(p0.z()),
          x1_(p1.x()), y1_(p1.y()), z1_(p1.z())
    {
    }

    template <typename T>
    bool operator()(const T *r, T *residuals) const
    {
        T p1[3] = {T(x1_), T(y1_), T(z1_)};
        T tf_p1[3];
        T tf[7] = {r[0], r[1], r[2], r[3], T(0), T(0), T(0)};
        ceres::SE3TransformPoint(tf, p1, tf_p1);
        residuals[0] = T(x0_) - tf_p1[0];
        residuals[1] = T(y0_) - tf_p1[1];
        residuals[2] = T(z0_) - tf_p1[2];
        return true;
    }

    static ceres::CostFunction *Create(Vector3d p0, Vector3d p1)
    {
        return (new ceres::AutoDiffCostFunction<NavsatInitRError, 3, 4>(new NavsatInitRError(p0, p1)));
    }

private:
    double x0_, y0_, z0_;
    double x1_, y1_, z1_;
};

class NavsatInitRXError
{
public:
    NavsatInitRXError(Vector3d p0, Vector3d p1)
        : x0_(p0.x()), y0_(p0.y()), z0_(p0.z()),
          x1_(p1.x()), y1_(p1.y()), z1_(p1.z())
    {
    }

    template <typename T>
    bool operator()(const T *rx, T *residuals) const
    {
        T p1[3] = {T(x1_), T(y1_), T(z1_)};
        T tf_p1[3];
        T tf[7] = {rx[0], rx[1], rx[2], rx[3], rx[4], T(0), T(0)};
        ceres::SE3TransformPoint(tf, p1, tf_p1);
        residuals[0] = T(x0_) - tf_p1[0];
        residuals[1] = T(y0_) - tf_p1[1];
        residuals[2] = T(z0_) - tf_p1[2];
        return true;
    }

    static ceres::CostFunction *Create(Vector3d p0, Vector3d p1)
    {
        return (new ceres::AutoDiffCostFunction<NavsatInitRXError, 3, 5>(new NavsatInitRXError(p0, p1)));
    }

private:
    double x0_, y0_, z0_;
    double x1_, y1_, z1_;
};

class NavsatRError
{
public:
    NavsatRError(SE3d origin, SE3d pose, Vector3d A, Vector3d B, Vector3d C) : pose_(pose), origin_(origin)
    {
        abc_norm_ = (A - B).cross(A - C);
        abc_norm_.normalize();
        pi_o_ = pose_.inverse() * origin_;
    }

    template <typename T>
    bool operator()(const T *roll, const T *pitch, const T *yaw, T *residual) const
    {
        T pose[7], new_pose[7], relative_pose[7], pi_o[7], pr[7];
        T rpyxyz[6] = {yaw[0], pitch[0], roll[0], T(0), T(0), T(0)};
        ceres::RpyxyzToSE3(rpyxyz, relative_pose);
        ceres::Cast(pose_.data(), SE3d::num_parameters, pose);
        ceres::Cast(pi_o_.data(), SE3d::num_parameters, pi_o);
        ceres::SE3Product(pose, relative_pose, pr);
        ceres::SE3Product(pr, pi_o, new_pose);
        T y[3] = {T(0), T(5), T(0)};
        T tf_p1[3];
        ceres::EigenQuaternionRotatePoint(new_pose, y, tf_p1);
        T ljm[3] = {T(abc_norm_.x()), T(abc_norm_.y()), T(abc_norm_.z())};
        residual[0] = ceres::DotProduct(tf_p1, ljm);
        return true;
    }

    static ceres::CostFunction *Create(SE3d origin, SE3d pose, Vector3d A, Vector3d B, Vector3d C)
    {
        return (new ceres::AutoDiffCostFunction<NavsatRError, 1, 1, 1, 1>(new NavsatRError(origin, pose, A, B, C)));
    }

private:
    SE3d pose_, origin_, pi_o_;
    Vector3d abc_norm_;
};

class NavsatRXError
{
public:
    NavsatRXError(Vector3d p0, Vector3d p1, SE3d pose)
        : x0_(p0.x()), y0_(p0.y()), z0_(p0.z()),
          x1_(p1.x()), y1_(p1.y()), z1_(p1.z()),
          pose_(pose)
    {
    }

    template <typename T>
    bool operator()(const T *roll, const T *pitch, const T *yaw, const T *x, T *residuals) const
    {
        T pose[7], tf[7], relative_pose[7];
        T rpyxyz[6] = {yaw[0], pitch[0], roll[0], x[0], T(0), T(0)};
        ceres::RpyxyzToSE3(rpyxyz, relative_pose);
        ceres::Cast(pose_.data(), SE3d::num_parameters, pose);
        ceres::SE3Product(pose, relative_pose, tf);
        T p1[3] = {T(x1_), T(y1_), T(z1_)};
        T tf_p1[3];
        ceres::SE3TransformPoint(tf, p1, tf_p1);
        residuals[0] = T(x0_) - tf_p1[0];
        residuals[1] = T(y0_) - tf_p1[1];
        residuals[2] = T(z0_) - tf_p1[2];
        return true;
    }

    static ceres::CostFunction *Create(Vector3d p0, Vector3d p1, SE3d pose)
    {
        return (new ceres::AutoDiffCostFunction<NavsatRXError, 3, 1, 1, 1, 1>(new NavsatRXError(p0, p1, pose)));
    }

private:
    double x0_, y0_, z0_;
    double x1_, y1_, z1_;
    SE3d pose_;
};

class NavsatXError
{
public:
    NavsatXError(Vector3d p0, Vector3d p1, SE3d pose)
        : x0_(p0.x()), y0_(p0.y()), z0_(p0.z()),
          x1_(p1.x()), y1_(p1.y()), z1_(p1.z()),
          pose_(pose)
    {
    }

    template <typename T>
    bool operator()(const T *x, T *residuals) const
    {
        T pose[7], tf[7], relative_pose[7];
        T rpyxyz[6] = {T(0), T(0), T(0), x[0], T(0), T(0)};
        ceres::RpyxyzToSE3(rpyxyz, relative_pose);
        ceres::Cast(pose_.data(), SE3d::num_parameters, pose);
        ceres::SE3Product(pose, relative_pose, tf);
        T p1[3] = {T(x1_), T(y1_), T(z1_)};
        T tf_p1[3];
        ceres::SE3TransformPoint(tf, p1, tf_p1);
        residuals[0] = T(x0_) - tf_p1[0];
        residuals[1] = T(y0_) - tf_p1[1];
        residuals[2] = T(z0_) - tf_p1[2];
        return true;
    }

    static ceres::CostFunction *Create(Vector3d p0, Vector3d p1, SE3d pose)
    {
        return (new ceres::AutoDiffCostFunction<NavsatXError, 3, 1>(new NavsatXError(p0, p1, pose)));
    }

private:
    double x0_, y0_, z0_;
    double x1_, y1_, z1_;
    SE3d pose_;
};

} // namespace lvio_fusion

#endif // lvio_fusion_NAVSAT_ERROR_H
