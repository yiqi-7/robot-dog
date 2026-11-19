#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CHAMP 步态控制器启动文件

本包【不引用 quadruped_sim】：机器人模型路径由调用方通过参数 urdf_path 传入。
依赖方向必须是「具体 → 通用」，配置包反向依赖仿真包会导致真机部署时
必须装一个用不上的仿真包（这是本次依赖反转要解决的问题）。

单独启动（需主仿真已在运行；主 launch 默认 gait:=champ 时无须单独起）：
  ros2 launch quadruped_champ_config champ_controller.launch.py \
      urdf_path:=$(ros2 pkg prefix quadruped_sim)/share/quadruped_sim/urdf/quadruped.urdf.xacro

推荐由主 launch 带路径拉起（gait 默认就是 champ）：
  ros2 launch quadruped_sim quadruped_gz.launch.py

数据链路（单一职责：本包只管「CHAMP 指令落到 gz 关节插件」这一件事）：
  /cmd_vel
    -> quadruped_controller_node (champ_base，订阅 cmd_vel/smooth，已重映射)
    -> /champ/joint_cmd          (JointState，12 关节目标角度)
    -> champ_joint_relay         (本包的 C++ 中继：JointState -> 12 路 Float64)
    -> /quadruped/<关节>/cmd_pos
    -> gz 关节位置控制器
"""
import os

import xacro
from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def _start_champ(context):
    """urdf_path 在这里才求值：没传就明确报错，而不是悄悄去读仿真包。"""
    urdf_path = LaunchConfiguration("urdf_path").perform(context).strip()
    if not urdf_path:
        raise RuntimeError(
            "缺少 urdf_path 参数。CHAMP 需要用 URDF/xacro 做运动学求解，"
            "本包不再自行定位仿真包里的模型文件（避免反向依赖），"
            "请由调用方传入，例如：\n"
            "  ros2 launch quadruped_sim quadruped_gz.launch.py\n"
            "或手动：\n"
            "  ros2 launch quadruped_champ_config champ_controller.launch.py "
            "urdf_path:=<绝对路径>/quadruped.urdf.xacro"
        )

    champ_share = get_package_share_directory("quadruped_champ_config")

    # 展开 xacro 得到完整 URDF 字符串：关节轴/原点直接从模型里来，
    # 不用再手工维护腿长、腿距（那套参数最容易和模型不同步）。
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
        package="quadruped_champ_config",
        executable="champ_joint_relay",
        output="screen",
        parameters=[{"use_sim_time": True}],
    )

    return [controller, relay]


def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument(
            "urdf_path",
            default_value="",
            description="机器人模型（.urdf.xacro 或 .urdf）的绝对路径，必填。"
                        "由调用方提供，使本配置包不依赖任何具体机器人包。",
        ),
        OpaqueFunction(function=_start_champ),
    ])
