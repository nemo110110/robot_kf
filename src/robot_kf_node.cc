#include <Eigen/Geometry>
#include <ros/ros.h>
#include <tf/transform_datatypes.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>
#include <tf/transform_broadcaster.h>
#include <tf_conversions/tf_eigen.h>
#include <geometry_msgs/QuaternionStamped.h>
#include <robot_kf/WheelOdometry.h>
#include <nav_msgs/Odometry.h>
#include <sensor_msgs/Imu.h>
#include <robot_kf/robot_kf.h>

static double const big = 99999.0;

static boost::shared_ptr<tf::TransformListener> sub_tf;
static boost::shared_ptr<tf::TransformBroadcaster> pub_tf;
static ros::Subscriber sub_compass, sub_encoders, sub_gps;
static ros::Publisher pub_fused;

static robot_kf::KalmanFilter kf;
static geometry_msgs::Twist velocity;

static bool watch_compass, watch_encoders, watch_gps;
static std::string global_frame_id, odom_frame_id, base_frame_id;

static void publish(ros::Time stamp)
{
    Eigen::Vector3d const state = kf.getState();

    // Wrap the fused state estimate in a ROS message.
    geometry_msgs::PoseStamped fused_base;
    fused_base.header.stamp    = stamp;
    fused_base.header.frame_id = odom_frame_id;
    fused_base.pose.position.x = state[0];
    fused_base.pose.position.y = state[1];
    fused_base.pose.position.z = 0.0;
    fused_base.pose.orientation = tf::createQuaternionMsgFromYaw(state[2]);

    /*
     * We actually want to publish the /map to /base_footprint transform, but
     * this would cause /base_footprint to have two parents. Instead, we need
     * to publish the /map to /odom transform. See this chart:
     *
     * /map --[T1]--> /odom --[T2]--> /base_footprint
     * /map ----------[T3]----------> /base_footprint
     *
     * The output of the Kalman filter is T3, but we want T1. We find T1 by
     * computing T1 = T3 * inv(T2), where T2 is provided by the odometry
     * source.
     */
    tf::Vector3 const pos(state[0], state[1], 0);
    tf::Quaternion const ori = tf::createQuaternionFromYaw(state[2]);
    tf::Transform t3(ori, pos);

    tf::StampedTransform t2;
    try {
        sub_tf->lookupTransform(odom_frame_id, base_frame_id, stamp, t2);
    } catch (tf::TransformException const &e) {
        ROS_WARN("%s", e.what());
        return;
    }

    ros::Time out_stamp = ros::Time::now();

    tf::Transform const t1 = t3 * t2.inverse();
    tf::StampedTransform transform(t1, out_stamp, global_frame_id, odom_frame_id);
    pub_tf->sendTransform(transform);
    
    // Publish the odometry message.
    nav_msgs::Odometry msg;
    msg.header.stamp = out_stamp;
    msg.header.frame_id = global_frame_id;
    msg.child_frame_id = base_frame_id;
    msg.pose.pose =  fused_base.pose;
    msg.pose.covariance[0] = -1;
    msg.twist.twist = velocity;
    msg.twist.covariance[0] = -1;
    pub_fused.publish(msg);
}

static void updateCompass(sensor_msgs::Imu const &msg)
{
    try {
        ros::Time const stamp = msg.header.stamp;
        std::string const frame_id = msg.header.frame_id;

        // Transform the orientation into the base coordinate frame.
        geometry_msgs::QuaternionStamped stamped_in, stamped_out;
        stamped_in.header.frame_id = frame_id;
        stamped_in.header.stamp    = stamp;
        stamped_in.quaternion = msg.orientation;
        sub_tf->transformQuaternion(base_frame_id, stamped_in, stamped_out);

        // Rotate the covariance matrix according to the transformation.
        tf::StampedTransform transform;
        Eigen::Affine3d eigen_transform;
        sub_tf->lookupTransform(base_frame_id, frame_id, stamp, transform);
        tf::TransformTFToEigen(transform, eigen_transform);

        Eigen::Matrix3d const rotation = eigen_transform.rotation();
        Eigen::Map<Eigen::Matrix3d const> cov_raw(&msg.orientation_covariance.front());
        Eigen::Matrix3d const cov = rotation.transpose() * cov_raw * rotation;

        kf.update_compass(tf::getYaw(stamped_out.quaternion), cov(2, 2));
        if (watch_compass) publish(stamp);
    } catch (tf::TransformException const &e) {
        ROS_WARN("%s", e.what());
    }
}

static void updateEncoders(robot_kf::WheelOdometry const &msg)
{
    if (msg.header.frame_id != base_frame_id) {
        ROS_ERROR_THROTTLE(10, "WheelOdometry message must have frame_id '%s'", base_frame_id.c_str());
        return;
    } else if (msg.separation <= 0) {
        ROS_ERROR_THROTTLE(10, "Wheel separation in WheelOdometry message must be positive.");
        return;
    }

    Eigen::Vector2d const z = (Eigen::Vector2d() <<
        msg.left.movement, msg.right.movement).finished();
    Eigen::Matrix2d const cov_z = (Eigen::Matrix2d() <<
        msg.left.variance, 0.0, 0.0, msg.right.variance).finished();

    // TODO: Compute velocity using the wheel encoders.

    kf.update_encoders(z, cov_z, msg.separation);
    if (watch_encoders) publish(msg.header.stamp);
}

static void updateGps(nav_msgs::Odometry const &msg)
{
    try {
        ros::Time const stamp = msg.header.stamp;
        std::string const frame_id = msg.child_frame_id;

        // Transform the position into the base coordinate frame.
        geometry_msgs::PointStamped stamped_in, stamped_out;
        stamped_in.header.stamp = stamp;
        stamped_in.header.frame_id = frame_id;
        stamped_in.point = msg.pose.pose.position;
        sub_tf->transformPoint(base_frame_id, stamped_in, stamped_out);

        Eigen::Vector2d const z = (Eigen::Vector2d() <<
            stamped_out.point.x, stamped_out.point.y).finished();
        Eigen::Map<Eigen::Matrix<double, 6, 6> const> cov_raw(
            &msg.pose.covariance.front()
        );
        Eigen::Matrix3d const cov3_raw = cov_raw.topLeftCorner<3, 3>();

        // Rotate the covariance matrix according to the transformation.
        tf::StampedTransform transform;
        Eigen::Affine3d eigen_transform;
        sub_tf->lookupTransform(base_frame_id, frame_id, stamp, transform);
        tf::TransformTFToEigen(transform, eigen_transform);

        Eigen::Matrix3d const rotation = eigen_transform.rotation();
        Eigen::Matrix3d const cov3 = rotation.transpose() * cov3_raw * rotation;
        Eigen::Matrix2d const cov  = cov3.topLeftCorner<2, 2>();

        kf.update_gps(z, cov);

        if (watch_gps) publish(msg.header.stamp);
    } catch (tf::TransformException const &e) {
        ROS_WARN("%s", e.what());
    }
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "robot_kf_node");

    ros::NodeHandle nh, nh_node("~");
    nh_node.param<bool>("watch_compass",  watch_compass,  true);
    nh_node.param<bool>("watch_encoders", watch_encoders, true);
    nh_node.param<bool>("watch_gps",      watch_gps,      true);
    nh_node.param<std::string>("global_frame_id", global_frame_id, "/map");
    nh_node.param<std::string>("odom_frame_id",   odom_frame_id,   "/odom");
    nh_node.param<std::string>("base_frame_id",   base_frame_id,   "/base_footprint");

    sub_tf = boost::make_shared<tf::TransformListener>();
    pub_tf = boost::make_shared<tf::TransformBroadcaster>();
    sub_compass  = nh.subscribe("compass", 1, &updateCompass);
    sub_encoders = nh.subscribe("wheel_odom", 1, &updateEncoders);
    sub_gps      = nh.subscribe("gps", 1, &updateGps);
    pub_fused    = nh.advertise<nav_msgs::Odometry>("odom_fused", 100);

    ros::spin();
    return 0;
}
