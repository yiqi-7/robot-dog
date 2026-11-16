#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CHAMP 步态控制器启动文件（配合已在运行的 gz 仿真使用）

用法：
  ros2 launch quadruped_champ_config champ_controller.launch.py

前置条件：
  1. 主仿真 quadruped_gz.launch.py 已在运行（gz + 桥接 + robot_state_publisher）
  2. 自研 gait_controller 已停止（否则两套步态同时写 cmd_pos 会打架）

数据链路：
  /cmd_vel
    -> quadruped_controller_node (CHAMP, 订阅 cmd_vel/smooth 已重映射)
    -> /champ/joint_cmd          (JointState, 12 关节目标角度)
    -> champ_joint_relay         (本仓库 quadruped_sim 里的 C++ 中继)
    -> /quadruped/<关节>/cmd_pos (Float64)
    -> gz 关节位置控制器
"""
import os

import xacro
from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch_ros.actions import Node


def generate_launch_description():
    champ_share = get_package_share_directory("quadruped_champ_config")
    qsim_share = get_package_share_directory("quadruped_sim")

    # 展开_xacro 得到完整 URDF 字符串：CHAMP 通过 urdf 参数读模型做运动学，
    # 关节轴/原点直接从 URDF 里来，不用再手工写腿长腿距
    urdf_path = os.path.join(qsim_share, "urdf", "quadruped.urdf.xacro")
    robot_desc = xacro.process_file(urdf_path).toxml()

    controller = Node(
        package="champ_base",
        executable="quadruped_controller_node",
        output="screen",
        parameters=[
            os.path.join(champ_share, "config", "champ.yaml"),
            {"urdf": robot_desc, "use_sim_time": True},
        ],
        remappings=[
            # champ 内部约定入口是 cmd_vel/smooth，统一映射到全栈共用的 /cmd_vel
            ("cmd_vel/smooth", "cmd_vel"),
            # champ 输出的 joint_states 与 gz 桥接发布的真实 /joint_states 撞名，
            # 把指令流重映射到 /champ/joint_cmd，实测数据流继续走 /joint_states
            ("joint_states", "champ/joint_cmd"),
        ],
    )

    relay = Node(
        package="quadruped_sim",
        executable="champ_joint_relay",
        output="screen",
        parameters=[{"use_sim_time": True}],
    )

    return LaunchDescription([controller, relay])
