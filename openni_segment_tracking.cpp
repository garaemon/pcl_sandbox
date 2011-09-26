#include <pcl/tracking/tracking.h>
#include <pcl/tracking/particle_filter.h>
#include <pcl/tracking/particle_filter_omp.h>

#include <pcl/tracking/coherence.h>
#include <pcl/tracking/distance_coherence.h>
#include <pcl/tracking/hsv_color_coherence.h>
#include <pcl/tracking/normal_coherence.h>

#include <pcl/tracking/nearest_pair_point_cloud_coherence.h>

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/io/openni_grabber.h>
#include <pcl/console/parse.h>
#include <pcl/common/time.h>

#include <pcl/visualization/cloud_viewer.h>
#include <pcl/visualization/pcl_visualizer.h>

#include <pcl/io/pcd_io.h>

#include <pcl/filters/passthrough.h>
#include <pcl/filters/project_inliers.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/filters/approximate_voxel_grid.h>
#include <pcl/filters/extract_indices.h>

#include <pcl/features/normal_3d.h>
#include <pcl/features/normal_3d_omp.h>
#include <pcl/features/integral_image_normal.h>

#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>

#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/segmentation/extract_polygonal_prism_data.h>
#include <pcl/segmentation/extract_clusters.h>


#include <pcl/surface/convex_hull.h>

#include <pcl/search/auto.h>
#include <pcl/search/octree.h>

#include <pcl/common/transforms.h>

#define FPS_CALC_BEGIN                          \
    static double duration = 0;                 \
    double start_time = pcl::getTime ();        \

#define FPS_CALC_END(_WHAT_)                    \
  {                                             \
    double end_time = pcl::getTime ();          \
    static unsigned count = 0;                  \
    if (++count == 10)                          \
    {                                           \
      std::cout << "Average framerate("<< _WHAT_ << "): " << double(count)/double(duration) << " Hz" <<  std::endl; \
      count = 0;                                                        \
      duration = 0.0;                                                   \
    }                                           \
    else                                        \
    {                                           \
      duration += end_time - start_time;        \
    }                                           \
  }

using namespace pcl::tracking;

template <typename PointType>
class OpenNISegmentTracking
{
public:
  typedef pcl::PointXYZRGBNormal RefPointType;
  typedef ParticleXYZRPY ParticleT;
  
  typedef pcl::PointCloud<PointType> Cloud;
  typedef pcl::PointCloud<RefPointType> RefCloud;
  typedef typename RefCloud::Ptr RefCloudPtr;
  typedef typename RefCloud::ConstPtr RefCloudConstPtr;
  typedef typename Cloud::Ptr CloudPtr;
  typedef typename Cloud::ConstPtr CloudConstPtr;
  typedef ParticleFilterOMPTracker<RefPointType, ParticleT> ParticleFilter;
  //typedef ParticleFilterTracker<RefPointType, ParticleT> ParticleFilter;
  typedef typename ParticleFilter::CoherencePtr CoherencePtr;
  
  OpenNISegmentTracking (const std::string& device_id)
  : viewer_ ("PCL OpenNI Tracking Viewer")
  , device_id_ (device_id)
  , sensor_view (0)
  , reference_view (0)
  , new_cloud_ (false)
  , ne_ (4)                   // 8 threads
  {
    pass_.setFilterFieldName ("z");
    pass_.setFilterLimits (0.0, 2.0);
    pass_.setKeepOrganized (true);
    firstp_ = true;
    // grid_.setFilterFieldName ("z");
    // grid_.setFilterLimits (0.0, 2.0);
    grid_.setLeafSize (0.01, 0.01, 0.01);
    //grid_.setDownsampleAllData (true);
    
    seg_.setOptimizeCoefficients (true);
    seg_.setModelType (pcl::SACMODEL_PLANE);
    seg_.setMethodType (pcl::SAC_RANSAC);
    seg_.setMaxIterations (1000);
    seg_.setDistanceThreshold (0.03);
    
    pcl::KdTreeFLANN<pcl::PointXYZRGB>::Ptr tree (new pcl::KdTreeFLANN<pcl::PointXYZRGB> ());
    ne_.setSearchMethod (tree);
    ne_.setRadiusSearch (0.03);
    
    //ne_.setNormalEstimationMethod (pcl::IntegralImageNormalEstimation<pcl::PointXYZRGB, pcl::Normal>::COVARIANCE_MATRIX);
    //ne_.setRectSize (50, 50);
    
    std::vector<double> default_step_covariance = std::vector<double> (6, 0.01 * 0.01);
    std::vector<double> initial_noise_covariance = std::vector<double> (6, 0.0);
    std::vector<double> default_initial_mean = std::vector<double> (6, 0.0);
    
    tracker_ = boost::shared_ptr<ParticleFilter>
      (new ParticleFilter (4)); // 9.52 - 800 particles/4threads
    
    tracker_->setStepNoiseCovariance (default_step_covariance);
    tracker_->setInitialNoiseCovariance (initial_noise_covariance);
    tracker_->setInitialNoiseMean (default_initial_mean);
    tracker_->setIterationNum (1);
    //tracker_->setParticleNum (200);
    tracker_->setParticleNum (400); 
    // setup coherences
    NearestPairPointCloudCoherence<RefPointType>::Ptr coherence = NearestPairPointCloudCoherence<RefPointType>::Ptr
      (new NearestPairPointCloudCoherence<RefPointType> ());
    boost::shared_ptr<DistanceCoherence<RefPointType> > distance_coherence
      = boost::shared_ptr<DistanceCoherence<RefPointType> > (new DistanceCoherence<RefPointType> ());
    distance_coherence->setWeight (05.0);
    coherence->addPointCoherence (distance_coherence);
    
    boost::shared_ptr<HSVColorCoherence<RefPointType> > color_coherence
      = boost::shared_ptr<HSVColorCoherence<RefPointType> > (new HSVColorCoherence<RefPointType> ());
    color_coherence->setWeight (0.1);
    coherence->addPointCoherence (color_coherence);
    
    boost::shared_ptr<NormalCoherence<RefPointType> > normal_coherence
      = boost::shared_ptr<NormalCoherence<RefPointType> > (new NormalCoherence<RefPointType> ());
    normal_coherence->setWeight (0.1);
    coherence->addPointCoherence (normal_coherence);
    
    pcl::search::AutotunedSearch<RefPointType>::SearchPtr oct
      (new pcl::search::AutotunedSearch<RefPointType> (pcl::search::KDTREE));

    //pcl::search::Octree<RefPointType>::Ptr oct (new pcl::search::Octree<RefPointType> (0.01));
    coherence->setSearchMethod (oct);
    
    tracker_->setCloudCoherence (coherence);
    extract_positive_.setNegative (false);
  }

  void
  drawPlaneCoordinate (pcl::visualization::PCLVisualizer& viz)
  {
      pcl::PointXYZ O, X, Y, Z;
      O.x = plane_trans_ (0, 3);
      O.y = plane_trans_ (1, 3);
      O.z = plane_trans_ (2, 3);
      X.x = O.x + plane_trans_ (0, 0) * 0.1;
      X.y = O.y + plane_trans_ (1, 0) * 0.1;
      X.z = O.z + plane_trans_ (2, 0) * 0.1;
      Y.x = O.x + plane_trans_ (0, 1) * 0.1;
      Y.y = O.y + plane_trans_ (1, 1) * 0.1;
      Y.z = O.z + plane_trans_ (2, 1) * 0.1;
      Z.x = O.x + plane_trans_ (0, 2) * 0.15;
      Z.y = O.y + plane_trans_ (1, 2) * 0.15;
      Z.z = O.z + plane_trans_ (2, 2) * 0.15;
      drawLine (viz, O, X, "x");
      drawLine (viz, O, Y, "y");
      drawLine (viz, O, Z, "z");
  }

  void
  drawSearchArea (pcl::visualization::PCLVisualizer& viz)
  {
    const ParticleXYZRPY zero_particle;
    Eigen::Affine3f trans = tracker_->getTrans ();
    Eigen::Affine3f search_origin = trans * tracker_->toEigenMatrix (zero_particle);
    Eigen::Quaternionf q = Eigen::Quaternionf (search_origin.rotation ());
    
    pcl::ModelCoefficients coefficients;
    coefficients.values.push_back (search_origin.translation ()[0]);
    coefficients.values.push_back (search_origin.translation ()[1]);
    coefficients.values.push_back (search_origin.translation ()[2]);
    coefficients.values.push_back (q.x ());
    coefficients.values.push_back (q.y ());
    coefficients.values.push_back (q.z ());
    coefficients.values.push_back (q.w ());
    
    coefficients.values.push_back (1.0);
    coefficients.values.push_back (1.0);
    coefficients.values.push_back (1.0);
    
    viz.removeShape ("searcharea");
    viz.addCube (coefficients, "searcharea");
  }

  void drawLine (pcl::visualization::PCLVisualizer& viz, const pcl::PointXYZ& from, const pcl::PointXYZ& to, const std::string& name)
  {
    viz.removeShape (name);
    viz.addLine<pcl::PointXYZ> (from, to, name);
  }
  
  bool
  drawParticles (pcl::visualization::PCLVisualizer& viz)
  {
    ParticleFilter::PointCloudStatePtr particles = tracker_->getParticles ();
    if (particles)
    {
      pcl::PointCloud<pcl::PointXYZ>::Ptr particle_cloud (new pcl::PointCloud<pcl::PointXYZ> ());
      for (size_t i = 0; i < particles->points.size (); i++)
      {
        pcl::PointXYZ point;
        
        ParticleXYZRPY particle = particles->points[i];
        point.x = particles->points[i].x;
        point.y = particles->points[i].y;
        point.z = particles->points[i].z;
        particle_cloud->points.push_back (point);
      }
      
      {
        pcl::visualization::PointCloudColorHandlerCustom<pcl::PointXYZ> blue_color (particle_cloud, 0, 0, 255);
        if (!viz.updatePointCloud (particle_cloud, blue_color, "particle cloud"))
          viz.addPointCloud (particle_cloud, blue_color, "particle cloud");
      }
      return true;
    }
    else
    {
      PCL_WARN ("no particles\n");
      return false;
    }
  }
  
  void
  drawResult (pcl::visualization::PCLVisualizer& viz)
  {
    ParticleXYZRPY result = tracker_->getResult ();
    std::cout << "result: " << result.weight << std::endl; //debug
    Eigen::Affine3f transformation = tracker_->toEigenMatrix (result);
    pcl::PointCloud<pcl::PointXYZRGBNormal>::Ptr result_cloud (new pcl::PointCloud<pcl::PointXYZRGBNormal> ());
    
    pcl::transformPointCloud<pcl::PointXYZRGBNormal> (*(tracker_->getReferenceCloud ()), *result_cloud,
                                                      transformation);
    
    {
      pcl::visualization::PointCloudColorHandlerCustom<pcl::PointXYZRGBNormal> red_color (result_cloud, 255, 0, 0);
      if (!viz.updatePointCloud (result_cloud, red_color, "resultcloud"))
        viz.addPointCloud (result_cloud, red_color, "resultcloud");
    }
  }

  void
  viz_cb (pcl::visualization::PCLVisualizer& viz)
  {
    boost::mutex::scoped_lock lock (mtx_);
    viz.setBackgroundColor (0.8, 0.8, 0.8);
    if (!cloud_pass_downsampled_)
    {
      boost::this_thread::sleep (boost::posix_time::seconds (1));
      return;
    }

    if (!viz.updatePointCloud (cloud_pass_downsampled_, "cloudpass"))
    {
      viz.addPointCloud (cloud_pass_downsampled_, "cloudpass");
      viz.resetCameraViewpoint ("cloudpass");
    }
      
    if (new_cloud_)
    {
      bool ret = drawParticles (viz);
      if (ret)
        drawResult (viz);
    }
    new_cloud_ = false;
  }

  void filterPassThrough (const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr &cloud, Cloud &result)
  {
    FPS_CALC_BEGIN;
    pass_.setInputCloud (cloud);
    pass_.filter (result);
    FPS_CALC_END("filterPassThrough");
  }

  void euclideanSegment (const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr &cloud,
                         std::vector<pcl::PointIndices> &cluster_indices)
  {
    pcl::EuclideanClusterExtraction<pcl::PointXYZRGB> ec;
    pcl::KdTree<pcl::PointXYZRGB>::Ptr tree (new pcl::KdTreeFLANN<pcl::PointXYZRGB>);
    
    ec.setClusterTolerance (0.05); // 2cm
    ec.setMinClusterSize (100);
    ec.setMaxClusterSize (25000);
    ec.setSearchMethod (tree);
    ec.setInputCloud (cloud);
    ec.extract (cluster_indices);

  }
  
  void gridSample (const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr &cloud, Cloud &result)
  {
    FPS_CALC_BEGIN;
    grid_.setInputCloud (cloud);
    grid_.filter (result);
    FPS_CALC_END("gridSample");
  }
  
  
  void planeSegmentation (const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr &cloud,
                          pcl::ModelCoefficients &coefficients,
                          pcl::PointIndices &inliers)
  {
    FPS_CALC_BEGIN;
    seg_.setInputCloud (cloud);
    seg_.segment (inliers, coefficients);
    FPS_CALC_END("planeSegmentation");
  }

  void planeProjection (const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr &cloud,
                        Cloud &result,
                        const pcl::ModelCoefficients::ConstPtr &coefficients)
  {
    FPS_CALC_BEGIN;
    pcl::ProjectInliers<pcl::PointXYZRGB> proj;
    proj.setModelType (pcl::SACMODEL_PLANE);
    proj.setInputCloud (cloud);
    proj.setModelCoefficients (coefficients);
    proj.filter (result);
    FPS_CALC_END("planeProjection");
  }

  void convexHull (const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr &cloud,
                   Cloud &result,
                   std::vector<pcl::Vertices> &hull_vertices)
  {
    FPS_CALC_BEGIN;
    pcl::ConvexHull<pcl::PointXYZRGB> chull;
    chull.setInputCloud (cloud);
    chull.reconstruct (*cloud_hull_, hull_vertices);
    FPS_CALC_END("convexHull");
  }

  void normalEstimation (const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr &cloud,
                         pcl::PointCloud<pcl::Normal> &result)
  {
    FPS_CALC_BEGIN;
    ne_.setInputCloud (cloud);
    ne_.compute (result);
    FPS_CALC_END("normalEstimation");
  }
  
  void tracking (const pcl::PointCloud<pcl::PointXYZRGBNormal>::ConstPtr &cloud)
  {
    FPS_CALC_BEGIN;
    tracker_->setInputCloud (cloud);
    tracker_->compute ();
    FPS_CALC_END("tracking");
  }

  void addNormalToCloud (const CloudConstPtr &cloud,
                         const pcl::PointCloud<pcl::Normal>::ConstPtr &normals,
                         RefCloud &result)
    {
      result.width = cloud->width;
      result.height = cloud->height;
      result.is_dense = cloud->is_dense;
      for (size_t i = 0; i < cloud->points.size (); i++)
      {
        pcl::PointXYZRGBNormal point;
        point.x = cloud->points[i].x;
        point.y = cloud->points[i].y;
        point.z = cloud->points[i].z;
        point.rgb = cloud->points[i].rgb;
        point.normal[0] = normals->points[i].normal[0];
        point.normal[1] = normals->points[i].normal[1];
        point.normal[2] = normals->points[i].normal[2];
        result.points.push_back (point);
      }
    }
  
  void
  cloud_cb (const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr &cloud)
  {
    boost::mutex::scoped_lock lock (mtx_);
    FPS_CALC_BEGIN;
    cloud_pass_.reset (new Cloud);
    pcl::ModelCoefficients::Ptr coefficients (new pcl::ModelCoefficients ());
    pcl::PointIndices::Ptr inliers (new pcl::PointIndices ());
    filterPassThrough (cloud, *cloud_pass_);
    cloud_pass_downsampled_.reset (new Cloud);
    gridSample (cloud_pass_, *cloud_pass_downsampled_);
    
     if (firstp_)
    {
      planeSegmentation (cloud_pass_downsampled_, *coefficients, *inliers);
      if (inliers->indices.size () > 3)
      {
        CloudPtr cloud_projected (new Cloud ());
        planeProjection (cloud_pass_downsampled_, *cloud_projected, coefficients);
        
        cloud_hull_.reset (new Cloud);
        convexHull (cloud_projected, *cloud_hull_, hull_vertices_);
          
        plane_trans_ = estimatePlaneCoordinate(cloud_hull_);
        
        // setup offset to tracker_
        Eigen::Affine3f affine_plane = Eigen::Affine3f (plane_trans_);
        //std::cout << "trans: " << plane_trans_ << std::endl; //debug
        Eigen::Affine3f offset = Eigen::Affine3f::Identity ();
        offset = Eigen::Translation3f (0.0, 1.0, 0.0);
        tracker_->setTrans (Eigen::Affine3f::Identity ());
        //tracker_->setTrans (affine_plane * offset);
        pcl::PointIndices::Ptr inliers_polygon (new pcl::PointIndices ());
        pcl::ExtractPolygonalPrismData<pcl::PointXYZRGB> polygon_extract;
        nonplane_cloud_.reset (new Cloud);
        
        polygon_extract.setHeightLimits (0.01, 10.0);
        polygon_extract.setInputPlanarHull (cloud_hull_);
        polygon_extract.setInputCloud (cloud_pass_downsampled_);
        polygon_extract.segment (*inliers_polygon);
        
        extract_positive_.setInputCloud (cloud_pass_downsampled_);
        extract_positive_.setIndices (inliers_polygon);
      
        extract_positive_.filter (*nonplane_cloud_);
        
        std::vector<pcl::PointIndices> cluster_indices;
        euclideanSegment (nonplane_cloud_, cluster_indices);
        std::cout << "clusters: " << cluster_indices.size () << std::endl;
        
        // select segment randomly
        int segment_index = rand () % cluster_indices.size ();
        std::cout << "segmented_cloud: " << segment_index << std::endl;
        pcl::PointIndices segmented_indices = cluster_indices[segment_index];
        segmented_cloud_.reset (new Cloud);
        for (size_t i = 0; i < segmented_indices.indices.size (); i++)
        {
          pcl::PointXYZRGB point = nonplane_cloud_->points[segmented_indices.indices[i]];
          segmented_cloud_->points.push_back (point);
        }
        segmented_cloud_->width = segmented_cloud_->points.size ();
        segmented_cloud_->height = 1;
        segmented_cloud_->is_dense = true;

        pcl::PointCloud<pcl::Normal>::Ptr normals (new pcl::PointCloud<pcl::Normal>);
        normalEstimation (segmented_cloud_, *normals);
        RefCloudPtr ref_cloud (new RefCloud);
        addNormalToCloud (segmented_cloud_, normals, *ref_cloud);
        // initialie tracker
        tracker_->setReferenceCloud (ref_cloud);
        tracker_->setMinIndices (ref_cloud->points.size () / 2);
        firstp_ = false;
      }}
     else
     {
        normals_.reset (new pcl::PointCloud<pcl::Normal>);
        normalEstimation (cloud_pass_downsampled_, *normals_);
        pcl::PointCloud<pcl::PointXYZRGBNormal>::Ptr tracking_cloud (new pcl::PointCloud<pcl::PointXYZRGBNormal> ());
        tracking_cloud->width = cloud_pass_downsampled_->width;
        tracking_cloud->height = cloud_pass_downsampled_->height;
        tracking_cloud->is_dense = cloud_pass_downsampled_->is_dense;
        for (size_t i = 0; i < cloud_pass_downsampled_->points.size (); i++)
      {
        pcl::PointXYZRGBNormal point;
        point.x = cloud_pass_downsampled_->points[i].x;
        point.y = cloud_pass_downsampled_->points[i].y;
        point.z = cloud_pass_downsampled_->points[i].z;
        point.rgb = cloud_pass_downsampled_->points[i].rgb;
        point.normal[0] = normals_->points[i].normal[0];
        point.normal[1] = normals_->points[i].normal[1];
        point.normal[2] = normals_->points[i].normal[2];
        tracking_cloud->points.push_back (point);
      }
      pcl::PointCloud<pcl::PointXYZRGBNormal>::ConstPtr tracking_const_ptr = tracking_cloud;
      tracking (tracking_cloud);
     }
     new_cloud_ = true;
     FPS_CALC_END("computation");
  }
      
  Eigen::Matrix4f 
  estimatePlaneCoordinate (CloudPtr cloud_hull)
  {
    if (cloud_hull->points.size() >= 3)
    {
      Eigen::Vector3f BA (cloud_hull->points[0].x - cloud_hull->points[1].x,
                          cloud_hull->points[0].y - cloud_hull->points[1].y,
                          cloud_hull->points[0].z - cloud_hull->points[1].z);
      Eigen::Vector3f BC (cloud_hull->points[2].x - cloud_hull->points[1].x,
                          cloud_hull->points[2].y - cloud_hull->points[1].y,
                          cloud_hull->points[2].z - cloud_hull->points[1].z);
      Eigen::Vector3f z = BC.cross (BA);
      z.normalize ();
      // check the direction of z
      
      Eigen::Vector3f B (cloud_hull->points[1].x, cloud_hull->points[1].y, cloud_hull->points[1].z);
      if (B.dot (z) > 0)
        z = - z;
      
      // calc x, y
      Eigen::Vector3f xx = BA;
      // check the direction of xx
      if (xx.dot (Eigen::Vector3f (1, 0, 0)) < 0)
        xx = -xx;
      xx.normalize ();
      Eigen::Vector3f yy = z.cross (xx);
      yy.normalize ();
      Eigen::Vector3f ux (1.0, 0.0, 0.0);
      double tmp = ux.dot (yy) / ux.dot (xx);
      double beta2 = 1 / (1 + tmp * tmp);
      double beta = sqrt (beta2);
      double alpha = - beta * tmp;
      Eigen::Vector3f y = alpha * xx + beta * yy;
      Eigen::Vector3f x = y.cross (z);
      x.normalize ();
      y.normalize ();
      
      Eigen::Matrix4f ret = Eigen::Matrix4f::Identity ();
      
      // fill rotation
      for (int i = 0; i < 3; i++)
      {
        ret(i, 0) = x[i];
        ret(i, 1) = y[i];
        ret(i, 2) = z[i];
      }
      
      Eigen::Vector3f OB (cloud_hull->points[1].x, cloud_hull->points[1].y, cloud_hull->points[1].z);
      double yscale = - OB.dot (y);
      double xscale = - OB.dot (x);
      Eigen::Vector3f position = OB + yscale * y + xscale * x;
        
      for (int i = 0; i < 3; i++)
        ret (i, 3) = position[i];
        
      return ret;
    }
    return Eigen::Matrix4f::Identity ();
  }
  
  void
  run ()
  {
    // send PCD to tracker_
    // tracker_->setReferenceCloud (ref_cloud_downsampled);
    // tracker_->setMinIndices (ref_cloud_downsampled->points.size () / 2);
    
    pcl::Grabber* interface = new pcl::OpenNIGrabber (device_id_);
    boost::function<void (const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr&)> f =
      boost::bind (&OpenNISegmentTracking::cloud_cb, this, _1);
    interface->registerCallback (f);
    
    viewer_.runOnVisualizationThread (boost::bind(&OpenNISegmentTracking::viz_cb, this, _1), "viz_cb");
    
    interface->start ();
      
    while (!viewer_.wasStopped ())
      boost::this_thread::sleep(boost::posix_time::seconds(1));
    interface->stop ();
  }
  
  pcl::PassThrough<PointType> pass_;
  pcl::VoxelGrid<PointType> grid_;
  //pcl::ApproximateVoxelGrid<PointType> grid_;
  pcl::SACSegmentation<PointType> seg_;
  pcl::ExtractIndices<PointType> extract_positive_;
  
  pcl::visualization::CloudViewer viewer_;
  pcl::PointCloud<pcl::Normal>::Ptr normals_;
  CloudPtr cloud_pass_;
  CloudPtr cloud_pass_downsampled_;
  CloudPtr plane_cloud_;
  CloudPtr nonplane_cloud_;
  CloudPtr cloud_hull_;
  CloudPtr segmented_cloud_;
  
  std::vector<pcl::Vertices> hull_vertices_;
  Eigen::Matrix4f plane_trans_;
  
  std::string device_id_;
  boost::mutex mtx_;
  int sensor_view, reference_view;
  bool new_cloud_;
  pcl::NormalEstimationOMP<PointType, pcl::Normal> ne_;
  //pcl::IntegralImageNormalEstimation<PointType, pcl::Normal> ne_;
  boost::shared_ptr<ParticleFilter> tracker_;
  bool firstp_;
};

void
usage (char** argv)
{
  std::cout << "usage: " << argv[0] << " <device_id> <pcd_file> <options>\n\n";
}

int
main (int argc, char** argv)
{
  
  std::string device_id = std::string (argv[1]);

  // open kinect
  pcl::OpenNIGrabber grabber ("");
  if (grabber.providesCallback<pcl::OpenNIGrabber::sig_cb_openni_point_cloud_rgb> ())
  {
    PCL_INFO ("PointXYZRGB mode enabled.\n");
    OpenNISegmentTracking<pcl::PointXYZRGB> v (device_id);
    v.run ();
  }
  else
  {
    PCL_INFO ("PointXYZ mode enabled.\n");
    OpenNISegmentTracking<pcl::PointXYZRGB> v (device_id);
    v.run ();
  }
  return (0);

}

