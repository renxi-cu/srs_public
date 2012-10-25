/******************************************************************************
 * \file
 *
 * $Id:$
 *
 * Copyright (C) Brno University of Technology
 *
 * This file is part of software developed by dcgm-robotics@FIT group.
 *
 * Author: Vit Stancl (stancl@fit.vutbr.cz)
 * Supervised by: Michal Spanel (spanel@fit.vutbr.cz)
 * Date: 25/1/2012
 *
 * This file is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <srs_env_model/but_server/registration/pcl_registration_module.h>
#include <srs_env_model/topics_list.h>
#include <pcl_ros/transforms.h>

/**
 * Constructor
 */
srs_env_model::COcToPcl::COcToPcl()
: m_bTransformCamera( false )
, m_bSpinThread( true )
, m_bPublishCloud(true)
{

}

/**
 * Initialize module - called in server constructor
 */
void srs_env_model::COcToPcl::init(ros::NodeHandle & node_handle)
{
	ROS_DEBUG("Initializing CCompressedPointCloudPlugin");

	if ( m_bSpinThread )
	{
		// if we're spinning our own thread, we'll also need our own callback queue
		node_handle.setCallbackQueue( &callback_queue_ );

		need_to_terminate_ = false;
		spin_thread_.reset( new boost::thread(boost::bind(&COcToPcl::spinThread, this)) );
		node_handle_ = node_handle;
	}

	// Read parameters
	{
		// Where to get camera position information
		node_handle.param("camera_info_topic_name", m_cameraInfoTopic, CPC_CAMERA_INFO_PUBLISHER_NAME );
	}

	 // Create publisher - simple point cloud
	m_pubConstrainedPC = node_handle.advertise<sensor_msgs::PointCloud2> (REGISTRATION_CONSTRAINED_CLOUD_PUBLISHER_NAME, 100, false);

	// Subscribe to position topic
	// Create subscriber
	m_camPosSubscriber = node_handle.subscribe<sensor_msgs::CameraInfo>( m_cameraInfoTopic, 10, &srs_env_model::COcToPcl::onCameraChangedCB, this );

	if (!m_camPosSubscriber)
	{
		ROS_ERROR("Not subscribed...");
		ROS_ERROR( "Cannot subscribe to the camera position publisher..." );
	}

	// stereo cam params for sensor cone:
	node_handle.param<int> ("compressed_pc_camera_stereo_offset_left", m_camera_stereo_offset_left, 0); // 128
	node_handle.param<int> ("compressed_pc_camera_stereo_offset_right", m_camera_stereo_offset_right, 0);
}

/**
 * Get output pointcloud
 */
bool srs_env_model::COcToPcl::computeCloud( const SMapWithParameters & par )
{
	ROS_DEBUG( "CCompressedPointCloudPlugin: onFrameStart" );

	// Copy buffered camera normal and d parameter
	boost::recursive_mutex::scoped_lock lock( m_camPosMutex );

	// Clear data
	m_cloud.clear();
	m_ocFrameId = par.frameId;
	m_DataTimeStamp = m_time_stamp = par.currentTime;

	bool bTransformOutput = m_ocFrameId != m_pcFrameId;

	// Output transform matrix
	Eigen::Matrix4f pcOutTM;

	// If different frame id
	if( bTransformOutput )
	{
		tf::StampedTransform ocToPcTf;

		// Get transform
		try {
			// Transformation - to, from, time, waiting time
			m_tfListener.waitForTransform(m_pcFrameId, m_ocFrameId,
					par.currentTime, ros::Duration(5));

			m_tfListener.lookupTransform(m_pcFrameId, m_ocFrameId,
					par.currentTime, ocToPcTf);

		} catch (tf::TransformException& ex) {
			ROS_ERROR_STREAM("Transform error: " << ex.what() << ", quitting callback");
			ROS_ERROR_STREAM( "Transform error.");
			return false;
		}


		// Get transformation matrix
		pcl_ros::transformAsMatrix(ocToPcTf, pcOutTM);	// Sensor TF to defined base TF

	}

	if( m_cameraFrameId.size() == 0 )
	{
		ROS_ERROR_STREAM("Wrong camera frame id...");
		m_bTransformCamera = false;
		return false;
	}

	m_bTransformCamera = m_cameraFrameId != m_pcFrameId;

	m_to_sensorTf = tf::StampedTransform::getIdentity();

	// If different frame id
	if( m_bTransformCamera )
	{

		// Some transforms
		tf::StampedTransform camToOcTf, ocToCamTf;

		// Get transforms
		try {
			// Transformation - to, from, time, waiting time
			m_tfListener.waitForTransform(m_cameraFrameId, m_ocFrameId,
					par.currentTime, ros::Duration(5));

			m_tfListener.lookupTransform( m_cameraFrameId, m_ocFrameId,
					par.currentTime, ocToCamTf );

		} catch (tf::TransformException& ex) {
			ROS_ERROR_STREAM( ": Transform error - " << ex.what() << ", quitting callback");
			ROS_ERROR_STREAM( "Camera FID: " << m_cameraFrameId << ", Octomap FID: " << m_ocFrameId );
			return false;
		}


		m_to_sensorTf = ocToCamTf;

//        PERROR( "Camera position: " << m_camToOcTrans );
	}


	// Store camera information
	m_camera_size = m_camera_size_buffer;
	m_camera_model.fromCameraInfo( m_camera_info_buffer);
	m_octomap_updates_msg->camera_info = m_camera_info_buffer;
	m_octomap_updates_msg->pointcloud2.header.stamp = par.currentTime;

	// Initialize leaf iterators
	tButServerOcTree & tree( par.map->octree );
	srs_env_model::tButServerOcTree::leaf_iterator it, itEnd( tree.end_leafs() );

	// Crawl through nodes
	for ( it = tree.begin_leafs(par.treeDepth); it != itEnd; ++it)
	{
		// Node is occupied?
		if (tree.isNodeOccupied(*it))
		{
			handleOccupiedNode(it, par);
		}// Node is occupied?

	} // Iterate through octree

	if( bTransformOutput )
	{
		// transform point cloud from octomap frame to the preset frame
		pcl::transformPointCloud< tPclPoint >(m_cloud, m_cloud, pcOutTM);
	}
}

/**
 * hook that is called when traversing occupied nodes of the updated Octree (does nothing here)
 */
void srs_env_model::COcToPcl::handleOccupiedNode(srs_env_model::tButServerOcTree::iterator& it, const SMapWithParameters & mp)
{
//	PERROR("OnHandleOccupied");

	if( ! m_bCamModelInitialized )
		return;

	// Test input point
	tf::Point pos(it.getX(), it.getY(), it.getZ());
	if( m_bTransformCamera )
		 pos = m_to_sensorTf(pos);

	cv::Point2d uv = m_camera_model.project3dToPixel(cv::Point3d( pos.x(), pos.y(), pos.z()));

	// ignore point if not in sensor cone
	if (!inSensorCone(uv))
		return;

	// Ok, add it...
	//	std::cerr << "PCP: handle occupied" << std::endl;
	tPclPoint point;

	// Set position
	point.x = it.getX();
	point.y = it.getY();
	point.z = it.getZ();

	// Set color
	point.r = it->r();
	point.g = it->g();
	point.b = it->b();

//	std::cerr << "Occupied node r " << (int)point.r << ", g " << (int)point.g << ", b " << (int)point.b << std::endl;

	m_cloud.push_back( point );
}

/**
 * On camera position changed callback
 */
void srs_env_model::COcToPcl::onCameraChangedCB(const sensor_msgs::CameraInfo::ConstPtr &cam_info)
{
	boost::recursive_mutex::scoped_lock lock( m_camPosMutex );

	//PERROR("OnCameraChange.");

	//	ROS_DEBUG( "CCompressedPointCloudPlugin: onCameraChangedCB" );

	// Set camera position frame id
	m_cameraFrameId = cam_info->header.frame_id;


	ROS_DEBUG("COcToPcl: Set camera info: %d x %d\n", cam_info->height, cam_info->width);
	m_camera_model_buffer.fromCameraInfo(*cam_info);
	m_camera_size_buffer = m_camera_model_buffer.fullResolution();

	// Set flag
	m_bCamModelInitialized = true;

	m_camera_info_buffer = *cam_info;
}


/**
 * Test if point is in camera cone
 */
bool srs_env_model::COcToPcl::inSensorCone(const cv::Point2d& uv) const
{
	//PERROR( uv.x << " > " << m_camera_stereo_offset_left + 1 << " && " << uv.x << " < " << m_camera_size.width + m_camera_stereo_offset_right - 2 );
	//PERROR( uv.y <<	" > " << 1 << " && " << uv.y << " < " << m_camera_size.height - 2 );
	// Check if projected 2D coordinate in pixel range.
		// This check is a little more restrictive than it should be by using
		// 1 pixel less to account for rounding / discretization errors.
		// Otherwise points on the corner are accounted to be in the sensor cone.
		return ((uv.x > m_camera_stereo_offset_left + 1) &&
				(uv.x < m_camera_size.width + m_camera_stereo_offset_right - 2) &&
				(uv.y > 1) &&
				(uv.y < m_camera_size.height - 2));
}

/**
 * Main loop when spinning our own thread - process callbacks in our callback queue - process pending goals
 */
void srs_env_model::COcToPcl::spinThread()
{
	while (node_handle_.ok())
		{
			if (need_to_terminate_)
			{
				break;
			}
			callback_queue_.callAvailable(ros::WallDuration(0.033f));
		}
}
