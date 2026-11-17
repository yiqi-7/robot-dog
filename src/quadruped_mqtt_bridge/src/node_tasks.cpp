// ===========================================================================
// node_tasks.cpp —— 任务执行：assign 派单 / Nav2 导航 / 状态回写 / 初始位姿
// 从原 mqtt_bridge.cpp 拆出，行为不变；类声明见 mqtt_bridge_node.hpp
// ===========================================================================

#include "quadruped_mqtt_bridge/mqtt_bridge_node.hpp"


  // =========================================================================
  // 开始前往目标点
  // =========================================================================
  void MqttBridge::startNavigation(const std::string &target_id, const std::string &title, int task_id, const json &task_obj) 
  {
    if (target_id.empty()) {
      RCLCPP_WARN(get_logger(), "任务未指定目标点，忽略");
      return;
    }
    // 在点位表里查找（先按 id，再按名称模糊匹配）
    const SitePoint *found = nullptr;
    for (const auto &p : kPoints) {
      if (target_id == p.id) { found = &p; break; }
    }
    if (!found) {
      for (const auto &p : kPoints) {
        if (target_id.find(p.name) != std::string::npos ||
            std::string(p.name).find(target_id) != std::string::npos) {
          found = &p; break;
        }
      }
    }
    if (!found) {
      RCLCPP_WARN(get_logger(), "未知目标点 '%s'，可用点位见启动日志", target_id.c_str());
      return;
    }

    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      nav_target_x_ = found->x;      // 仿真世界坐标
      nav_target_y_ = found->y;
      nav_target_name_ = found->name;
      navigating_ = true;
      status_ = "working";
      task_ = title.empty() ? std::string("前往 ") + found->name : title;
      nav_start_time_ = now();
      active_task_id_ = task_id;
      if (task_obj.is_object()) cached_task_ = task_obj;   // 回写底稿
    }
    // 派单接管控制权：先让巡逻停手，否则两个节点同时发导航目标互相打断
    pausePatrol(true);
    RCLCPP_INFO(get_logger(), "开始前往 [%s] %s  目标=(%.2f, %.2f) 任务#%d",
      found->id, found->name, found->x, found->y, task_id);

    sendNavGoal(found->x, found->y, found->name);
  }


  // =========================================================================
  // 下发 Nav2 导航目标（map 坐标系）
  // =========================================================================
  void MqttBridge::sendNavGoal(double x, double y, const std::string & name) 
  {
    if (!nav_client_->wait_for_action_server(2s)) {
      RCLCPP_WARN(get_logger(), "Nav2 action server 未就绪，目标暂未下发（稍后重试）");
      // 保留 navigating_=true，由 onNavTimer 的超时逻辑兜底重试/放弃
      return;
    }

    NavigateToPose::Goal goal;
    goal.pose.header.frame_id = "map";
    goal.pose.header.stamp = now();
    goal.pose.pose.position.x = x;
    goal.pose.pose.position.y = y;
    goal.pose.pose.position.z = 0.0;
    goal.pose.pose.orientation.w = 1.0;   // 不约束最终朝向，到位即可

    auto options = rclcpp_action::Client<NavigateToPose>::SendGoalOptions();
    options.goal_response_callback =
      [this, name](const NavGoalHandle::SharedPtr & handle) {
        if (!handle) {
          RCLCPP_WARN(get_logger(), "Nav2 拒绝了目标 %s（可能不在代价地图可通行区）", name.c_str());
          finishTask("（目标被拒绝）", Outcome::Failed);
          return;
        }
        std::lock_guard<std::mutex> lk(data_mutex_);
        nav_goal_ = handle;
        RCLCPP_INFO(get_logger(), "Nav2 已接受目标 %s", name.c_str());
      };
    options.result_callback =
      [this, name](const NavGoalHandle::WrappedResult & result) {
        if (result.code == rclcpp_action::ResultCode::SUCCEEDED) {
          RCLCPP_INFO(get_logger(), "任务完成：已到达 %s", name.c_str());
          finishTask(name, Outcome::Success);
        } else if (result.code == rclcpp_action::ResultCode::CANCELED) {
          RCLCPP_INFO(get_logger(), "导航已取消（%s）", name.c_str());
          finishTask("已取消", Outcome::Canceled);
        } else {
          RCLCPP_WARN(get_logger(), "导航失败（%s），Nav2 将自行尝试恢复行为", name.c_str());
          char buf[192];
          std::snprintf(buf, sizeof(buf),
            "导航失败（%s）：Nav2 未能到达目标点，本次任务放弃", name.c_str());
          publishEvent("warning", buf);
          finishTask("（导航失败）", Outcome::Failed);
        }
      };
    nav_client_->async_send_goal(goal, options);
  }


  // 任务结局：决定回写 fleet/tasks 时把 status 改成什么
  enum class Outcome { Success, Canceled, Failed };

  // 结束当前任务：清状态、停车、恢复巡逻、回写任务状态
  void MqttBridge::finishTask(const std::string & detail, Outcome outcome) 
  {
    int tid = 0;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      navigating_ = false;
      status_ = "idle";
      task_.clear();
      nav_goal_.reset();
      tid = active_task_id_;
      active_task_id_ = 0;
    }
    pub_cmd_vel_->publish(geometry_msgs::msg::Twist());   // 确保停车
    pausePatrol(false);

    // 3.5 任务回写：成功 → completed；被取消/急停 → revoked；
    // 失败则保持 published，让调度方可以重新派单（规范里只有这四种状态）。
    if (outcome == Outcome::Success) {
      writeBackTask(tid, "completed");
    } else if (outcome == Outcome::Canceled) {
      writeBackTask(tid, "revoked");
    }
    (void)detail;
  }


  // 取消当前导航目标（看板"取消"按钮 / 急停都走这里）
  void MqttBridge::cancelNavigation(const char * reason) 
  {
    std::shared_ptr<NavGoalHandle> handle;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      handle = nav_goal_;
    }
    if (handle) {
      nav_client_->async_cancel_goal(handle);
      RCLCPP_WARN(get_logger(), "取消导航目标（%s）", reason);
      char buf[160];
      std::snprintf(buf, sizeof(buf), "任务被取消：%s", reason);
      publishEvent("info", buf);
    } else {
      RCLCPP_INFO(get_logger(), "当前无导航目标可取消（%s）", reason);
    }
    finishTask("已取消", Outcome::Canceled);
  }


  // =========================================================================
  // 3.5 任务状态回写 {P}/fleet/tasks
  //
  // 看板是任务列表的主要写入方，每次增删改都 retained 发布【全量数组】；
  // 机器人执行结束后要在同一主题上把该任务 status 改成 completed / revoked
  // 并重新全量发布（retained）。否则看板的任务列表永远停在"已发布"，
  // 用户根本看不出任务到底做完了没有。
  // =========================================================================
  void MqttBridge::writeBackTask(int task_id, const char *new_status) 
  {
    if (task_id <= 0) return;      // 无 id（如手动测试报文）无可回写

    std::string payload;
    bool ready = false;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      const long long ts = wallClockMs();

      // 记下终态：看板之后若重发旧列表，还要把这个终态盖回去
      task_terminal_[task_id] = std::make_pair(std::string(new_status), ts);

      if (!fleet_tasks_.is_array()) {
        // 还没收到看板的快照：用 assign 里缓存的任务对象自己拼一个数组
        if (cached_task_.is_object()) {
          json t = cached_task_;
          t["id"] = task_id;
          t["status"] = new_status;
          t["updatedAt"] = ts;
          fleet_tasks_ = json::array({t});
          ready = true;
        }
      } else {
        bool hit = false;
        for (auto &t : fleet_tasks_) {
          if (t.value("id", 0) == task_id) {
            t["status"] = new_status;
            t["updatedAt"] = ts;
            hit = true;
            break;
          }
        }
        // 快照里没有这一条（看板可能还没同步到）：补一条进去
        if (!hit && cached_task_.is_object()) {
          json t = cached_task_;
          t["id"] = task_id;
          t["status"] = new_status;
          t["updatedAt"] = ts;
          fleet_tasks_.push_back(t);
        }
        ready = true;
      }
      if (ready) payload = fleet_tasks_.dump();
    }

    if (!ready) {
      RCLCPP_WARN(get_logger(), "任务#%d 状态回写跳过：既无列表快照也无任务对象", task_id);
      return;
    }

    int rc = mosquitto_publish(mosq_, nullptr, fleet_tasks_topic_.c_str(),
      static_cast<int>(payload.size()), payload.c_str(), 1, /*retain=*/true);
    if (rc == MOSQ_ERR_SUCCESS) {
      RCLCPP_INFO(get_logger(), "任务#%d 已回写 %s（fleet/tasks 全量 %zu 字节）",
        task_id, new_status, payload.size());
    } else {
      RCLCPP_WARN(get_logger(), "任务#%d 回写失败: %s",
        task_id, mosquitto_strerror(rc));
    }
  }


  // 收到看板的任务列表全量快照：存下来，并把本机已产生的终态盖回去
  void MqttBridge::onFleetTasks(const std::string &text) 
  {
    json arr;
    try {
      arr = json::parse(text);
    } catch (const std::exception &e) {
      RCLCPP_WARN(get_logger(), "任务列表解析失败: %s", e.what());
      return;
    }
    if (!arr.is_array()) return;

    bool patched = false;
    std::string payload;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      for (auto &t : arr) {
        auto it = task_terminal_.find(t.value("id", 0));
        if (it != task_terminal_.end() && t.value("status", "") != it->second.first) {
          t["status"] = it->second.first;
          t["updatedAt"] = it->second.second;
          patched = true;
        }
      }
      fleet_tasks_ = arr;
      if (patched) payload = fleet_tasks_.dump();
    }
    RCLCPP_INFO(get_logger(), "已同步看板任务列表：%zu 条%s",
      arr.size(), patched ? "（已补回本机终态）" : "");

    // 只有真的补过终态才回发一次，避免和看板来回刷。
    // 看板收到后只 setTasks 不会回发，所以不存在无限循环。
    if (patched && mqtt_connected_) {
      mosquitto_publish(mosq_, nullptr, fleet_tasks_topic_.c_str(),
        static_cast<int>(payload.size()), payload.c_str(), 1, true);
    }
  }



  // =========================================================================
  // 巡视定时器（20 Hz）：初始位姿对齐 + 任务超时兜底
  //
  // 注意：这里【不再】直接发 cmd_vel 驱动轮子。行驶由 Nav2 的
  // controller_server 负责（它会看局部代价地图绕障），本函数只做状态维护。
  // =========================================================================
  void MqttBridge::onNavTimer() 
  {
    // ① 启动后补发一次 AMCL 初始位姿（见构造函数注释）
    if (!initial_pose_sent_ && odom_ok_) {
      if ((now() - start_time_).seconds() > 8.0) {
        publishInitialPose();
        initial_pose_sent_ = true;
      }
      return;
    }

    // ② 任务超时兜底：Nav2 长时间无结果时主动取消，避免"永远 working"
    bool timeout = false;
    std::string name;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      if (!navigating_) return;
      if ((now() - nav_start_time_).seconds() > nav_timeout_s_) {
        timeout = true;
        name = nav_target_name_;
      }
    }
    if (timeout) {
      RCLCPP_WARN(get_logger(), "任务超时（%.0f s）自动取消：%s",
        nav_timeout_s_, name.c_str());
      char buf[192];
      std::snprintf(buf, sizeof(buf), "导航超时（%.0f s）：未能到达 %s，任务已撤销",
        nav_timeout_s_, name.c_str());
      publishEvent("warning", buf);
      cancelNavigation("超时");
    }
  }


  // 用当前里程计位姿对齐 AMCL，消除初始定位偏差
  void MqttBridge::publishInitialPose() 
  {
    geometry_msgs::msg::PoseWithCovarianceStamped msg;
    msg.header.frame_id = "map";
    msg.header.stamp = now();
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      msg.pose.pose.position.x = pos_x_;
      msg.pose.pose.position.y = pos_y_;
      msg.pose.pose.position.z = 0.0;
      // 朝向：由 yaw 还原四元数（只用 z/w 分量）
      msg.pose.pose.orientation.z = std::sin(heading_ * 0.5);
      msg.pose.pose.orientation.w = std::cos(heading_ * 0.5);
    }
    // 协方差：位置 0.25 m²、朝向 0.07 rad²，给粒子滤波合理的初始散布
    msg.pose.covariance = {0.25, 0, 0, 0, 0, 0,
                           0, 0.25, 0, 0, 0, 0,
                           0, 0, 0.06, 0, 0, 0,
                           0, 0, 0, 0.02, 0, 0,
                           0, 0, 0, 0, 0.02, 0,
                           0, 0, 0, 0, 0, 0.07};
    pub_initial_pose_->publish(msg);
    RCLCPP_INFO(get_logger(), "已按里程计对齐 AMCL 初始位姿 (%.2f, %.2f)",
      msg.pose.pose.position.x, msg.pose.pose.position.y);
  }


  // 巡逻开关：派单期间暂停巡逻，避免两个节点抢同一个导航通道
  void MqttBridge::pausePatrol(bool paused) 
  {
    std_msgs::msg::Bool m;
    m.data = !paused;
    pub_patrol_enable_->publish(m);
  }
