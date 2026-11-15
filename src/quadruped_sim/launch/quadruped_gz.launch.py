#!/usr/bin/env python3
"""把四足机器人加载进厂区 Gazebo 世界。

同时启动:
  - Gazebo 仿真 + 机器人 + trot 步态控制
  - 里程计桥接 (/odom + tf: odom -> base_link)
  - Nav2 导航栈（slam_toolbox 在线建图 + 路径规划）
  - RViz2 可视化
  - 键盘遥控（可选）

用法:
  ros2 launch quadruped_sim quadruped_gz.launch.py
  ros2 launch quadruped_sim quadruped_gz.launch.py gui:=false     # 无 Gazebo 界面
  ros2 launch quadruped_sim quadruped_gz.launch.py teleop:=false  # 不启动键盘遥控
  ros2 launch quadruped_sim quadruped_gz.launch.py rviz:=false nav:=false  # 只跑仿真
"""

import os

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import (
    DeclareLaunchArgument,
    IncludeLaunchDescription,
    OpaqueFunction,
    TimerAction,
)
from launch.conditions import IfCondition
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import Command, LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue

# trot 关节指令话题
LEGS = ("fl", "fr", "rl", "rr")
JOINTS = ("abduction", "hip", "knee")


def launch_setup(context, *args, **kwargs):
    pkg_share = get_package_share_directory("quadruped_sim")
    xacro_file = os.path.join(pkg_share, "urdf", "quadruped.urdf.xacro")
    world_file = os.path.join(pkg_share, "worlds", "factory.sdf")
    nav2_params = os.path.join(pkg_share, "config", "nav2_params.yaml")
    rviz_config = os.path.join(pkg_share, "config", "quadruped.rviz")

    use_sim_time = LaunchConfiguration("use_sim_time").perform(context) == "true"
    gui = LaunchConfiguration("gui").perform(context)
    teleop = LaunchConfiguration("teleop").perform(context)
    rviz_on = LaunchConfiguration("rviz").perform(context)
    nav_on = LaunchConfiguration("nav").perform(context)
    gait_on = LaunchConfiguration("gait").perform(context)

    # ---------- 1. 展开 xacro ----------
    robot_description = ParameterValue(
        Command(["xacro ", xacro_file]), value_type=str
    )

    robot_state_publisher = Node(
        package="robot_state_publisher",
        executable="robot_state_publisher",
        output="screen",
        parameters=[
            {"robot_description": robot_description, "use_sim_time": use_sim_time}
        ],
    )

    # ---------- 2. 启动 Gazebo ----------
    # gui:=true  -> 完整界面（服务器 + GUI 客户端）
    # gui:=false -> 仅服务器（-s），适合无显示环境
    gz_args = f"-r -v 2 {world_file}"
    if gui != "true":
        gz_args = f"-s -r -v 2 {world_file}"

    gz_sim = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(
            os.path.join(
                get_package_share_directory("ros_gz_sim"), "launch", "gz_sim.launch.py"
            )
        ),
        launch_arguments={
            "gz_args": gz_args,
            "on_exit_shutdown": "true",
        }.items(),
    )

    # ---------- 3. 把机器人生成到世界里 ----------
    # ---------- 3. 把机器人生成到世界里 ----------
    # z 取站立脚底高度 (stand_z=0.315) 略上方 0.32：落地即站在脚上，
    # 避免落地瘫倒后在软启动期被蹬出数米
    # (x, y) 放到厂区南部空旷带 (0, -3.2)，避开货箱 (±2,0) 与储罐 (±3.5,±3.5)
    spawn = Node(
        package="ros_gz_sim",
        executable="create",
        output="screen",
        arguments=[
            "-topic", "robot_description",
            "-name", "quadruped",
            "-x", "0", "-y", "-5.5", "-z", "0.30",
        ],
        parameters=[{"use_sim_time": use_sim_time}],
    )

    # ---------- 4. 话题桥接 ----------
    bridge_args = [
        "/clock@rosgraph_msgs/msg/Clock[gz.msgs.Clock",
        "/joint_states@sensor_msgs/msg/JointState[gz.msgs.Model",
        # IMU（100 Hz）
        "/imu@sensor_msgs/msg/Imu[gz.msgs.IMU",
        # 360 度激光雷达（10 Hz）
        "/scan@sensor_msgs/msg/LaserScan[gz.msgs.LaserScan",
        # 前向深度相机（8 m）：gz depth_camera 只发深度图 + 点云
        "/depth_camera@sensor_msgs/msg/Image[gz.msgs.Image",
        "/depth_camera/points@sensor_msgs/msg/PointCloud2[gz.msgs.PointCloudPacked",
        # 里程计：gz 的 OdometryPublisher 插件 -> nav_msgs/Odometry
        "/model/quadruped/odometry@nav_msgs/msg/Odometry[gz.msgs.Odometry",
    ]
    for leg in LEGS:
        for j in JOINTS:
            bridge_args.append(
                f"/quadruped/{leg}_{j}/cmd_pos@std_msgs/msg/Float64]gz.msgs.Double"
            )

    bridge = Node(
        package="ros_gz_bridge",
        executable="parameter_bridge",
        output="screen",
        arguments=bridge_args,
        parameters=[{"use_sim_time": use_sim_time}],
        remappings=[("/model/quadruped/odometry", "/odom")],
    )

    # ---------- 5. trot 步态 + 里程计 ----------
    gait = Node(
        package="quadruped_sim",
        executable="gait_controller",
        name="trot_gait",
        output="screen",
        parameters=[{"use_sim_time": use_sim_time}],
        condition=IfCondition(gait_on),
    )

    odom = Node(
        package="quadruped_sim",
        executable="odom_pub",
        name="odom_pub",
        output="screen",
        parameters=[{"use_sim_time": use_sim_time}],
    )

    # 深度相机 CameraInfo（gz 的 depth_camera 不发布该话题，由本节点补发）
    camera_info = Node(
        package="quadruped_sim",
        executable="camera_info_pub",
        name="depth_camera_info",
        output="screen",
        parameters=[
            {
                "use_sim_time": use_sim_time,
                "width": 640,
                "height": 480,
                "horizontal_fov": 1.2,
                "frame_id": "depth_camera_link",
                "rate": 15.0,
            }
        ],
    )

    # ---------- 6. Nav2（slam_toolbox 在线建图 + 导航） ----------
    slam = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(
            os.path.join(
                get_package_share_directory("nav2_bringup"),
                "launch", "slam_launch.py",
            )
        ),
        launch_arguments={
            "params_file": nav2_params,
            "use_sim_time": "true" if use_sim_time else "false",
        }.items(),
        condition=IfCondition(nav_on),
    )

    navigation = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(
            os.path.join(
                get_package_share_directory("nav2_bringup"),
                "launch", "navigation_launch.py",
            )
        ),
        launch_arguments={
            "params_file": nav2_params,
            "use_sim_time": "true" if use_sim_time else "false",
        }.items(),
        condition=IfCondition(nav_on),
    )

    # 这个版本的 navigation_launch.py 不含 lifecycle_manager，
    # 自己启动一个把导航生命周期节点全部配置并激活
    NAV_LIFECYCLE_NODES = [
        "controller_server",
        "smoother_server",
        "planner_server",
        "route_server",
        "behavior_server",
        "velocity_smoother",
        "collision_monitor",
        "bt_navigator",
        "waypoint_follower",
    ]
    nav_lifecycle_manager = Node(
        package="nav2_lifecycle_manager",
        executable="lifecycle_manager",
        name="lifecycle_manager_navigation",
        output="screen",
        parameters=[
            {
                "use_sim_time": use_sim_time,
                "autostart": True,
                "node_names": NAV_LIFECYCLE_NODES,
                "bond_timeout": 4.0,
            }
        ],
        condition=IfCondition(nav_on),
    )

    # ---------- 7. RViz2 ----------
    rviz_node = Node(
        package="rviz2",
        executable="rviz2",
        name="rviz2",
        output="screen",
        arguments=["-d", rviz_config],
        parameters=[{"use_sim_time": use_sim_time}],
        condition=IfCondition(rviz_on),
    )

    # ---------- 8. 键盘遥控 ----------
    teleop_node = Node(
        package="teleop_twist_keyboard",
        executable="teleop_twist_keyboard",
        name="teleop_twist_keyboard",
        output="screen",
        remappings=[("cmd_vel", "cmd_vel")],
        parameters=[{"use_sim_time": use_sim_time}],
        condition=IfCondition(teleop),
    )

    return [
        robot_state_publisher,
        gz_sim,
        TimerAction(period=3.0, actions=[spawn]),
        TimerAction(period=5.0, actions=[bridge, gait, odom, camera_info]),
        TimerAction(period=8.0, actions=[slam, navigation, nav_lifecycle_manager]),
        rviz_node,
        teleop_node,
    ]


def generate_launch_description():
    return LaunchDescription(
        [
            DeclareLaunchArgument("use_sim_time", default_value="true"),
            DeclareLaunchArgument(
                "gui", default_value="true", description="是否显示 Gazebo 界面"
            ),
            DeclareLaunchArgument(
                "teleop", default_value="true", description="是否启动键盘遥控"
            ),
            DeclareLaunchArgument(
                "rviz", default_value="true", description="是否启动 RViz2"
            ),
            DeclareLaunchArgument(
                "nav", default_value="true", description="是否启动 Nav2 + SLAM"
            ),
            DeclareLaunchArgument(
                "gait", default_value="true", description="是否启动 trot 步态控制"
            ),
            OpaqueFunction(function=launch_setup),
        ]
    )
