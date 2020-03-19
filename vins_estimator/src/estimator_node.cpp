#include <stdio.h>
#include <queue>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <ros/ros.h>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/opencv.hpp>

#include "estimator.h"
#include "parameters.h"
#include "utility/visualization.h"


Estimator estimator;

std::condition_variable con;
double current_time = -1;
queue<sensor_msgs::ImuConstPtr> imu_buf;
queue<sensor_msgs::PointCloudConstPtr> feature_buf;
queue<sensor_msgs::PointCloudConstPtr> relo_buf; // 订阅pose graph node发布的回环帧数据，存到relo_buf队列中，供重定位使用
int sum_of_wait = 0;

std::mutex m_buf; // 用于处理多个线程使用imu_buf和feature_buf的冲突
std::mutex m_state; // 用于处理多个线程使用当前里程计信息（即tmp_P、tmp_Q、tmp_V）的冲突
std::mutex i_buf;
std::mutex m_estimator; // 用于处理多个线程使用VINS系统对象（即Estimator类的实例estimator）的冲突

double latest_time; // 最近一次里程计信息对应的IMU时间戳
// 当前里程计信息
Eigen::Vector3d tmp_P; //坐标
Eigen::Quaterniond tmp_Q; //旋转
Eigen::Vector3d tmp_V; //速度

// 当前里程计信息对应的IMU bias
Eigen::Vector3d tmp_Ba;
Eigen::Vector3d tmp_Bg;

// 当前里程计信息对应的IMU测量值
Eigen::Vector3d acc_0;
Eigen::Vector3d gyr_0;

bool init_feature = 0;
bool init_imu = 1; // true：第一次接收IMU数据
double last_imu_t = 0; // 最近一帧IMU数据的时间戳


// 从IMU测量值imu_msg和上一个PVQ递推得到当前PVQ
void predict(const sensor_msgs::ImuConstPtr &imu_msg)
{
    double t = imu_msg->header.stamp.toSec();
    if (init_imu) // 第一次接收IMU数据, init_imu初始化的值为1
    {
        latest_time = t;
        init_imu = 0;
        return;
    }
    double dt = t - latest_time;
    latest_time = t;

    double dx = imu_msg->linear_acceleration.x;
    double dy = imu_msg->linear_acceleration.y;
    double dz = imu_msg->linear_acceleration.z;
    Eigen::Vector3d linear_acceleration{dx, dy, dz};

    double rx = imu_msg->angular_velocity.x;
    double ry = imu_msg->angular_velocity.y;
    double rz = imu_msg->angular_velocity.z;
    Eigen::Vector3d angular_velocity{rx, ry, rz};

    Eigen::Vector3d un_acc_0 = tmp_Q * (acc_0 - tmp_Ba) - estimator.g;

    Eigen::Vector3d un_gyr = 0.5 * (gyr_0 + angular_velocity) - tmp_Bg;
    tmp_Q = tmp_Q * Utility::deltaQ(un_gyr * dt);

    Eigen::Vector3d un_acc_1 = tmp_Q * (linear_acceleration - tmp_Ba) - estimator.g;

    Eigen::Vector3d un_acc = 0.5 * (un_acc_0 + un_acc_1);

    tmp_P = tmp_P + dt * tmp_V + 0.5 * dt * dt * un_acc;
    tmp_V = tmp_V + dt * un_acc;

    acc_0 = linear_acceleration;
    gyr_0 = angular_velocity;
}

// 当处理完measurements中的所有数据后，如果VINS系统正常完成滑动窗口优化，那么需要用优化后的结果更新里程计数据
void update()
{
    TicToc t_predict;
    latest_time = current_time;

    // 首先获取滑动窗口中最新帧的P、V、Q
    tmp_P = estimator.Ps[WINDOW_SIZE];
    tmp_Q = estimator.Rs[WINDOW_SIZE];
    tmp_V = estimator.Vs[WINDOW_SIZE];
    tmp_Ba = estimator.Bas[WINDOW_SIZE];
    tmp_Bg = estimator.Bgs[WINDOW_SIZE];
    acc_0 = estimator.acc_0;
    gyr_0 = estimator.gyr_0;

    // 滑动窗口中最新帧并不是当前帧，中间隔着缓存队列的数据，所以还需要使用缓存队列中的IMU数据进行积分得到当前帧的里程计信息
    queue<sensor_msgs::ImuConstPtr> tmp_imu_buf = imu_buf;
    for (sensor_msgs::ImuConstPtr tmp_imu_msg; !tmp_imu_buf.empty(); tmp_imu_buf.pop())
        predict(tmp_imu_buf.front());

}

// 从缓存中读取，并按时间戳匹配imu数据和特征点数据，返回所有匹配好的pair
// 要找的imu数据，包括该帧特征点与上一帧特征点之间的IMU数据，以及该帧后面的第一帧IMU数据。如下图：
// *     *   (  *       *      *       *   )   *      IMU数据，括号内为与当前帧特征点匹配的
//   |                               |                图像特征点数据，前一帧和当前帧
std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> getMeasurements() 
{
  std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> measurements;

  // 直到把imu_buf或者feature_buf中的数据全部取出，才会退出while循环
  while (true) 
  {
    // 检查缓存中是否还有可以尝试配对的imu和特征点数据可以
    if (imu_buf.empty() || feature_buf.empty())
      return measurements;

        // imu_buf 的时间戳全都小于等于 feature_buf 的时间戳（时间偏移补偿后）时，需要等待更多IMU数据来覆盖特征点的时间
    if (!(imu_buf.back()->header.stamp.toSec() > feature_buf.front()->header.stamp.toSec() + estimator.td)) 
    {
      //ROS_WARN("wait for imu, only should happen at the beginning");
      sum_of_wait++;
      return measurements;
    }

    // imu_buf 时间戳全都大于等于 第一个feature_buf 的时间戳（时间偏移补偿后）时，该 feature 无法被覆盖，剔除
    if (!(imu_buf.front()->header.stamp.toSec() < feature_buf.front()->header.stamp.toSec() + estimator.td))
    {
        ROS_WARN("throw img, only should happen at the beginning");
        feature_buf.pop(); // 剔除
        continue;
    }
    // 到这里时，说明头部的特征点数据，其时间戳在imu的时间范围内
    sensor_msgs::PointCloudConstPtr img_msg = feature_buf.front(); // 读取feature_buf队首的数据
    feature_buf.pop(); // 剔除feature_buf队首的数据
    
    // 获取所有匹配的imu数据
    std::vector<sensor_msgs::ImuConstPtr> IMUs;        
    while (imu_buf.front()->header.stamp.toSec() < img_msg->header.stamp.toSec() + estimator.td)
    {
      IMUs.emplace_back(imu_buf.front());
      imu_buf.pop();
    }
    IMUs.emplace_back(imu_buf.front()); // 时间戳晚于当前帧图像的第一帧IMU数据

    if (IMUs.empty())
      ROS_WARN("no imu between two image");
        
    // 添加到输出项
    measurements.emplace_back(IMUs, img_msg);
  }
  return measurements;
}

//imu回调函数，将imu_msg存入imu_buf，递推IMU的PQV并发布"imu_propagate”
void imu_callback(const sensor_msgs::ImuConstPtr &imu_msg)
{
    //用时间戳来判断IMU message是否乱序
    if (imu_msg->header.stamp.toSec() <= last_imu_t)
    {
        ROS_WARN("imu message in disorder!");
        return;
    }
    last_imu_t = imu_msg->header.stamp.toSec(); 

    // 将IMU数据存入IMU数据缓存队列imu_buf
    m_buf.lock();
    imu_buf.push(imu_msg);
    m_buf.unlock();

    con.notify_one(); // 唤醒process线程, getMeasurements()读取缓存imu_buf和feature_buf中的观测数据

    // 通过IMU测量值积分更新并发布里程计信息
    last_imu_t = imu_msg->header.stamp.toSec();

    {
        std::lock_guard<std::mutex> lg(m_state);
        //预测函数，这里推算的是tmp_P,tmp_Q,tmp_V
        predict(imu_msg);
        std_msgs::Header header = imu_msg->header;
        header.frame_id = "world";

        // VINS初始化已完成，正处于滑动窗口非线性优化状态，如果VINS还在初始化，则不发布里程计信息
        if (estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR) 
            // 发布predict()处预测的里程计信息，
            // 发布频率很高（与IMU数据同频），每次获取IMU数据都会及时进行更新，
            pubLatestOdometry(tmp_P, tmp_Q, tmp_V, header); 
            // 还有一个pubOdometry()函数，似乎也是发布里程计信息，但是它是在estimator每次处理完一帧图像特征点数据后才发布的，有延迟，而且频率也不高（至多与图像同频）
    }
}

//feature回调函数，将feature_msg放入feature_buf
void feature_callback(const sensor_msgs::PointCloudConstPtr &feature_msg)
{
    //如果是第一个检测到的特征则直接忽略掉，这里直接return了二没有将该feature加入到feature_buf中
    if (!init_feature)
    {
        //skip the first detected feature, which doesn't contain optical flow speed
        init_feature = 1;
        return;
    }

    // 将图像特征点数据存入图像特征点数据缓存队列feature_buf
    m_buf.lock();
    feature_buf.push(feature_msg);
    m_buf.unlock();

    //唤醒process线程，调用getMeasurements(), 读取缓存imu_buf和feature_buf中的观测数据
    con.notify_one();
}

//restart回调函数，收到restart消息时清空feature_buf和imu_buf，估计器重置，时间重置
void restart_callback(const std_msgs::BoolConstPtr &restart_msg)
{
    if (restart_msg->data == true)
    {
        ROS_WARN("restart the estimator!");
        m_buf.lock();
        while(!feature_buf.empty())
            feature_buf.pop();
        while(!imu_buf.empty())
            imu_buf.pop();
        m_buf.unlock();

        m_estimator.lock();
        estimator.clearState();
        estimator.setParameter();
        m_estimator.unlock();
        current_time = -1;
        last_imu_t = 0;
    }
    return;
}

//relocalization回调函数，将接收到的匹配的地图点points_msg放入relo_buf,为后边的重定位提供数据支持
void relocalization_callback(const sensor_msgs::PointCloudConstPtr &points_msg)
{
    //printf("relocalization callback! \n");
    m_buf.lock();
    relo_buf.push(points_msg);
    m_buf.unlock();
}

// thread: visual-inertial odometry
// process()是measurement_process线程的线程函数，在process()中处理VIO后端，包括IMU预积分、松耦合初始化和local BA
void process()
{
  while (true)
  {
    std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> measurements;

    // 创建unique_lock对象lk，以独占所有权的方式管理mutex对象m_buf的上锁和解锁操作。
    // 所谓独占所有权，就是没有其他的unique_lock对象同时拥有m_buf的所有权，
    // lk会尝试调用m_buf.lock()，对m_buf进行上锁，如果此时另外的unique_lock对象已经管理了该Mutex对象m_buf,
    // 则当前线程将会被阻塞；如果此时m_buf本身就处于上锁状态，当前线程也会被阻塞（我猜的）。
    // 在lk的声明周期内，它所管理的m_buf会一直保持上锁状态。
    std::unique_lock<std::mutex> lk(m_buf);

    // 解释一下下面用到的函数：
    // std::condition_variable::wait(std::unique_lock<std::mutex>& lock, Predicate pred) {
    //   while (!pred()) { // 当 pred 为 false 时
    //     wait(lock);   // 调用wait(lock)阻塞当前线程，释放被lock管理的m_buf
    //   }
    // }
    // 当同一条件变量在其它线程中调用了notify_*函数时，当前线程被唤醒。
    // 直到pred为ture的时候，退出while循环。

    // [&]{return (measurements = getMeasurements()).size() != 0;} 则是lamda表达式（匿名函数）

    // 调用lamda表达式，尝试从缓存队列中，读取IMU数据和图像特征点数据，保存到measurements，
    // |-> 如果没读到，则匿名函数返回false
    // |   |-> 调用wait(lk)阻塞当前线程，释放m_buf
    // |        |-> 释放后，图像和IMU回调函数得以访问缓存队列，并调用con.notify_one()唤醒当前线程
    // |-> measurements不为空时，匿名函数返回true，则退出while循环
    con.wait(lk, [&]
              {
        return (measurements = getMeasurements()).size() != 0;
              });
    lk.unlock(); // 从缓存队列中读取数据完成，解锁

    m_estimator.lock();

    for (auto &measurement : measurements) // 遍历匹配好的特征点数据和imu数据
    {
      auto img_msg = measurement.second; // 当前帧的图像特征点数据
      double dx = 0, dy = 0, dz = 0, rx = 0, ry = 0, rz = 0;

      //遍历该组中的各帧imu数据，进行预积分
      for (auto &imu_msg : measurement.first)
      {
        double t = imu_msg->header.stamp.toSec(); // 最新IMU数据的时间戳
        double img_t = img_msg->header.stamp.toSec() + estimator.td; // 图像特征点数据的时间戳，补偿了通过优化得到的一个时间偏移
        if (t <= img_t) // IMU时间戳小于等于特征点的时间戳
        { 
          if (current_time < 0) // 第一次接收IMU数据时会出现这种情况
              current_time = t;
          double dt = t - current_time;
          ROS_ASSERT(dt >= 0);
          current_time = t; // 更新最近一次接收的IMU数据的时间戳

          // IMU测量值：3轴加速度，3轴角速度
          dx = imu_msg->linear_acceleration.x;
          dy = imu_msg->linear_acceleration.y;
          dz = imu_msg->linear_acceleration.z;
          rx = imu_msg->angular_velocity.x;
          ry = imu_msg->angular_velocity.y;
          rz = imu_msg->angular_velocity.z;
          estimator.processIMU(dt, Vector3d(dx, dy, dz), Vector3d(rx, ry, rz));
          //printf("imu: dt:%f a: %f %f %f w: %f %f %f\n",dt, dx, dy, dz, rx, ry, rz);
        }
        else // 时间戳晚于图像特征点数据（时间偏移补偿后）的第一帧IMU数据（也是一组measurement中的唯一一帧），对IMU数据进行插值，得到图像帧时间戳对应的IMU数据
        {
          // 时间戳的对应关系如下图所示：
          //                                            current_time         t
          // *               *               *               *               *     （IMU数据）
          //                                                          |            （图像特征点数据）
          //                                                        img_t
          double dt_1 = img_t - current_time;
          double dt_2 = t - img_t;
          current_time = img_t;
          ROS_ASSERT(dt_1 >= 0);
          ROS_ASSERT(dt_2 >= 0);
          ROS_ASSERT(dt_1 + dt_2 > 0);
          double w1 = dt_2 / (dt_1 + dt_2);
          double w2 = dt_1 / (dt_1 + dt_2);
          dx = w1 * dx + w2 * imu_msg->linear_acceleration.x;
          dy = w1 * dy + w2 * imu_msg->linear_acceleration.y;
          dz = w1 * dz + w2 * imu_msg->linear_acceleration.z;
          rx = w1 * rx + w2 * imu_msg->angular_velocity.x;
          ry = w1 * ry + w2 * imu_msg->angular_velocity.y;
          rz = w1 * rz + w2 * imu_msg->angular_velocity.z;
          estimator.processIMU(dt_1, Vector3d(dx, dy, dz), Vector3d(rx, ry, rz));
          //printf("dimu: dt:%f a: %f %f %f w: %f %f %f\n",dt_1, dx, dy, dz, rx, ry, rz);
        }
      }

      // 设置重定位用的回环帧
      // set relocalization frame
      sensor_msgs::PointCloudConstPtr relo_msg = NULL;
      while (!relo_buf.empty())
      {
        relo_msg = relo_buf.front();
        relo_buf.pop();
      }
      if (relo_msg != NULL)
      {
        vector<Vector3d> match_points;
        double frame_stamp = relo_msg->header.stamp.toSec(); // 回环帧的时间戳
        for (unsigned int i = 0; i < relo_msg->points.size(); i++)
        {
            Vector3d u_v_id;
            u_v_id.x() = relo_msg->points[i].x;
            u_v_id.y() = relo_msg->points[i].y;
            u_v_id.z() = relo_msg->points[i].z;
            match_points.push_back(u_v_id);
        }
        Vector3d relo_t(relo_msg->channels[0].values[0], relo_msg->channels[0].values[1], relo_msg->channels[0].values[2]);
        Quaterniond relo_q(relo_msg->channels[0].values[3], relo_msg->channels[0].values[4], relo_msg->channels[0].values[5], relo_msg->channels[0].values[6]);
        Matrix3d relo_r = relo_q.toRotationMatrix();
        int frame_index;
        frame_index = relo_msg->channels[0].values[7];
        estimator.setReloFrame(frame_stamp, frame_index, match_points, relo_t, relo_r); // 设置回环帧
      }


      ROS_DEBUG("processing vision data with stamp %f \n", img_msg->header.stamp.toSec());
      TicToc t_s;
      // 将图像特征点数据存到一个map容器中，key是特征点id
      map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>> image; // 为什么键值是一个vector啊，一个id的特征点对应一个vector，难道是因为可能有多个相机
      for (unsigned int i = 0; i < img_msg->points.size(); i++)
      {
        int v = img_msg->channels[0].values[i] + 0.5; // ？？？这是什么操作
        int feature_id = v / NUM_OF_CAM;
        int camera_id = v % NUM_OF_CAM;
        double x = img_msg->points[i].x;
        double y = img_msg->points[i].y;
        double z = img_msg->points[i].z;
        double p_u = img_msg->channels[1].values[i];
        double p_v = img_msg->channels[2].values[i];
        double velocity_x = img_msg->channels[3].values[i];
        double velocity_y = img_msg->channels[4].values[i];
        ROS_ASSERT(z == 1);
        Eigen::Matrix<double, 7, 1> xyz_uv_velocity;
        xyz_uv_velocity << x, y, z, p_u, p_v, velocity_x, velocity_y;
        image[feature_id].emplace_back(camera_id,  xyz_uv_velocity);
      }
      estimator.processImage(image, img_msg->header);

      double whole_t = t_s.toc();
      printStatistics(estimator, whole_t);
      std_msgs::Header header = img_msg->header;
      header.frame_id = "world";

      // 每处理完一帧图像特征点数据，都要发布这些话题
      pubOdometry(estimator, header);
      pubKeyPoses(estimator, header);
      pubCameraPose(estimator, header);
      pubPointCloud(estimator, header);
      pubTF(estimator, header);
      pubKeyframe(estimator);
      if (relo_msg != NULL)
        pubRelocalization(estimator);
      //ROS_ERROR("end: %f, at %f", img_msg->header.stamp.toSec(), ros::Time::now().toSec());
    }
    m_estimator.unlock();

    m_buf.lock();
    m_state.lock();
    if (estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR)
      // VINS系统完成滑动窗口优化后，用优化后的结果，更新里程计数据
      update();
    m_state.unlock();
    m_buf.unlock();
  }
}

int main(int argc, char **argv)
{
    //1.相关初始化
    ros::init(argc, argv, "vins_estimator");
    ros::NodeHandle n("~");
    ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Info);
    //2.参数读取
    readParameters(n); 
    //3.设置状态估计器的参数
    estimator.setParameter();
#ifdef EIGEN_DONT_PARALLELIZE
    ROS_DEBUG("EIGEN_DONT_PARALLELIZE");
#endif
    ROS_WARN("waiting for image and imu...");

    //4.注册发布器 //RViz相关话题
    registerPub(n); // 注册visualization.cpp中创建的发布器

    //5.订阅topic(imu, image, ...)
    ros::Subscriber sub_imu = n.subscribe(IMU_TOPIC, 2000, imu_callback, ros::TransportHints().tcpNoDelay()); // IMU数据
    ros::Subscriber sub_image = n.subscribe("/feature_tracker/feature", 2000, feature_callback); // 图像特征点数据
    ros::Subscriber sub_restart = n.subscribe("/feature_tracker/restart", 2000, restart_callback); // ？？？接收一个bool值，判断是否重启estimator
    ros::Subscriber sub_relo_points = n.subscribe("/pose_graph/match_points", 2000, relocalization_callback); // ？？？根据回环检测信息进行重定位

    //6.创建process线程，这个是主线程
    std::thread measurement_process{process}; 
    // estimator_node中的线程由旧版本中的3个改为现在的1个, 回环检测和全局位姿图优化在新增的一个ROS节点中运行
    // measurement_process线程的线程函数是process()，在process()中处理VIO后端，包括IMU预积分、松耦合初始化和local BA
    ros::spin();

    return 0;
}
