#include "lvio_fusion/navigation/gridmap.h"

#include <pcl/filters/extract_indices.h>
#include <pcl/filters/radius_outlier_removal.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/sac_segmentation.h>

namespace lvio_fusion
{

void Gridmap::AddScan( PointICloud scan_msg)
{
    ToCartesianCoordinates(scan_msg,current_frame);
}

void Gridmap::AddFrame(Frame::Ptr& frame)
{
    current_frame=frame;
}

void Gridmap::ToCartesianCoordinates(PointICloud scan_msg,Frame::Ptr& frame)
{
    std::vector<Vector2d> scan_points;
    for(int i = 0; i < scan_msg.size(); ++i) {
        Vector3d point( scan_msg[i].x,scan_msg[i].y,scan_msg[i].z);
        Vector3d trans_point=Rwg.inverse()*point;
        scan_points.emplace_back(Vector2d(trans_point[0],trans_point[1]));
    }
    LaserScan::Ptr laser_scan=LaserScan::Ptr(new LaserScan(frame,scan_points));
    laser_scans_2d.emplace_back(laser_scan);
    LOG(INFO)<<"laser_scans_2d "<<laser_scans_2d.size();
}

cv::Mat Gridmap::GetGridmap()    
{
    LOG(INFO)<<"GetGridmap";
    ClearMap();
    for(const LaserScan::Ptr&  scan : laser_scans_2d) {
        Vector3d trans_pose = Rwg.inverse()*scan->GetFrame()->pose.translation();
        Vector2d start =Vector2d(trans_pose[0],trans_pose[1]);
        for(Vector2d end : scan->GetPoints()) {
            std::vector<Eigen::Vector2i> points;
            Bresenhamline(start[0],  start[1], end[0], end[1], points);

            int n = points.size();
            if(n == 0) {
                continue;
            }
    
            for(int j = 0; j < n - 1; ++j) {
                Vector2i index = GetIndex(points[j][0], points[j][1]);
                visual_counter.at<int>(index[0],index[1])++;
            }
            Vector2i index = GetIndex(points[n - 1][0], points[n - 1][1]);
            occupied_counter.at<int>(index[0],index[1])++;
        }
    }
	for (int row = 0; row < height; ++row)
	{
		for (int col = 0; col < width; ++col)
        {
            int visits = visual_counter.at<int>(row, col);
			int occupieds = occupied_counter.at<int>(row, col);
            if((occupieds+ visits)<=0)//unknow
            {
                grid_map.at<float>(row, col) =-1.0;
            }
            else
            {
                grid_map.at<float>(row, col) = 1.0 - ((1.0 * occupieds )/(occupieds+ visits));
                grid_map_int.at<char>(row, col) = (1 - grid_map.at<float>(row, col)) * 100;
            }
        }
    }
    LOG(INFO)<<"GetGridmap end";
    return grid_map_int;
}

Vector2i  Gridmap::GetIndex(int x, int y)
{
    Vector2i index(x+height/2,y+width/2);
    return index;
}

void ExpendMap(){

}

void Gridmap::Bresenhamline (double x1,double y1,double x2,double y2, std::vector<Eigen::Vector2i>& points)
{
    double  dx, dy,  p, temp;
    int x, y, s1, s2, interchange=0, i;
    x=(x1+0.5)/1;
    y=(y1+0.5)/1;
    dx=abs(x2-x1);
    dy=abs(y2-y1);

    s1=x2>x1?1:-1;
    s2=y2>y1?1:-1;

    if(dy>dx)
    {
        temp=dx;
        dx=dy;
        dy=temp;
        interchange=1;
    }

    p=2*dy-dx;
    for(i=1;i<=dx;i++)
    {
        points.push_back(Vector2i(x,y));
        if(p>=0)
        {
            if(interchange==0)
                y=y+s2;
            else
                x=x+s1;
            p=p-2*dx;
        }   
        if(interchange==0)
            x=x+s1; 
        else
            y=y+s2;
        p=p+2*dy;
    }
}

void Gridmap::SetGravityDirection(Matrix3d newRwg)
{
    ChangeMapRwg(newRwg);
    Rwg=newRwg;
}

void Gridmap::ChangeMapRwg(Matrix3d newRwg)
{
    Matrix3d transform=newRwg*Rwg.inverse();
    //grid_map=transform*grid_map;
}

} // namespace lvio_fusion