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
  ros2 launch quadruped_sim quadruped_gz.launch.py gz_minimized:=false  # 窗口不最小化
  ros2 launch quadruped_sim quadruped_gz.launch.py gait:=champ   # 用 CHAMP 步态控制
  ros2 launch quadruped_sim quadruped_gz.launch.py teleop:=false  # 不启动键盘遥控
  ros2 launch quadruped_sim quadruped_gz.launch.py rviz:=false nav:=false  # 只跑仿真
"""

import os

from ament_index_python.packages import get_package_prefix, get_package_share_directory
from launch import LaunchDescription
from launch.actions import (
    DeclareLaunchArgument,
    ExecuteProcess,
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
    # gait 取值：trot（本项目自研 trot 步态）/ champ（CHAMP 通用步态控制器）/ none
    # 兼容旧写法：true -> trot，false -> none
    gait_mode = LaunchConfiguration("gait").perform(context).strip().lower()
    if gait_mode in ("true", "1", ""):
        gait_mode = "trot"
    elif gait_mode in ("false", "0"):
        gait_mode = "none"
    if gait_mode not in ("trot", "champ", "none"):
        raise RuntimeError(
            f"gait 参数只能是 trot / champ / none，收到：{gait_mode}"
        )
    gait_on = "true" if gait_mode == "trot" else "false"
    champ_on = "true" if gait_mode == "champ" else "false"
    gz_minimized = LaunchConfiguration("gz_minimized").perform(context)

    # 把 Gazebo 窗口收进任务栏：启动后调用本包的小工具发送最小化请求，
    # 用户点任务栏图标才展开，不点就一直不打扰桌面。
    gz_minimize_action = None
    if gui == "true" and gz_minimized == "true":
        gz_minimize_action = ExecuteProcess(
            cmd=[
                os.path.join(
                    get_package_prefix("quadruped_sim"),
                    "lib", "quadruped_sim", "gz_window_minimize",
                ),
                "Gazebo Sim",
                "60",
            ],
            output="screen",
            additional_env={"DISPLAY": os.environ.get("DISPLAY", ":0")},
        )

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
            "-x", "0", "-y", "-5.6", "-z", "0.30",
            # 出生朝向改为正北(+y)：竖走廊向南到北有 5.5 m 纵深；
            # 若朝东则正前方 1.3 m 就是仓库西墙，一起步就撞墙
            "-Y", "1.5708",
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

    # CHAMP 步态控制器（vendored 上游 champ_base + 本项目适配配置）
    # 它把 /cmd_vel 转成 12 路关节角 -> /champ/joint_cmd
    # -> champ_joint_relay -> /quadruped/<leg>_<joint>/cmd_pos
    champ_launch = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(
            os.path.join(
                get_package_share_directory("quadruped_champ_config"),
                "launch", "champ_controller.launch.py",
            )
        ),
        condition=IfCondition(champ_on),
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
                # bond 心跳超时：多个 Nav2 节点与 gz/桥接同时启动时，DDS 发现
                # 需要几秒，4 s 会出现 "unable to be reached by bond" 导致
                # 整个 bringup 中止，放宽到 20 s
                "bond_timeout": 20.0,
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

    actions = [
        robot_state_publisher,
        gz_sim,
        TimerAction(period=3.0, actions=[spawn]),
        TimerAction(period=5.0, actions=[bridge, gait, odom, camera_info]),
        # CHAMP 依赖 gz 桥接（/joint_states 与 cmd_pos 都要通），放到 8 秒起
        TimerAction(period=8.0, actions=[champ_launch]),
        # SLAM 先起：它要初始化 Ceres 求解器并发布 map->odom，
        # planner_server 的 global_costmap 激活时必须要这条变换，否则
        # 会一直等到 100 s 超时后放弃，整个 Nav2 bringup 中止
        TimerAction(period=6.0, actions=[slam]),
        # Nav2 放到 20 秒：等 SLAM 把 map->odom 建起来，且 gz/桥接稳定
        TimerAction(period=20.0, actions=[navigation, nav_lifecycle_manager]),
        rviz_node,
        teleop_node,
    ]
    if gz_minimize_action is not None:
        # 等 5 秒让 Gazebo 窗口真正创建出来，再请求最小化
        actions.append(TimerAction(period=5.0, actions=[gz_minimize_action]))
    return actions


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
                "gait",
                default_value="trot",
                description="步态控制器：trot(自研) / champ(通用) / none(不启动)",
            ),
            DeclareLaunchArgument(
                "gz_minimized",
                default_value="true",
                description="Gazebo 窗口启动后直接最小化到任务栏（不弹到前台）",
            ),
            OpaqueFunction(function=launch_setup),
        ]
    )
