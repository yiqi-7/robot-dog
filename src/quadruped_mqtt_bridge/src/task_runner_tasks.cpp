// ===========================================================================
// task_runner_tasks.cpp —— 任务执行：assign 派单 / Nav2 导航 / 状态回写 /
// 超时兜底 / 初始位姿 / 巡逻暂停
//
// 整段从原 node_tasks.cpp 搬过来，逻辑与日志文案保持原样（回归脚本按这些
// 文案断言："开始前往 [...]"、"任务完成：已到达"）；
// 新增的只有 publishTaskState()——把任务状态跨节点告诉 mqtt_bridge。
// ===========================================================================

#include "quadruped_mqtt_bridge/task_runner_node.hpp"


namespace {

  // -------------------------------------------------------------------------
  // 任务版本号 rev 的语义（跨端合并协议，必须与看板侧 JS 的实现一致）
  //
  // rev 是【单条任务对象】的单调递增整数版本号，字段缺省视为 0：
  //   · 谁改了这条任务，就把这条的 rev 改成"两端已知的较大 rev + 1"；
  //   · 两端同一条任务不一致时【以 rev 大者为准】，rev 相等即同一版本
  //     （幂等：不重复发布，不会两端来回刷）；
  //   · 看板新建任务时 rev 从 1 开始；老看板不带 rev 时按 0 处理，
  //     本机回写会写 1，仍然大于 0，所以老看板照样能被回写覆盖。
  // fleet/tasks 是 retained 的【全量数组】，MQTT 没有"只发一条"的能力，
  // 所以两端都必须按 id 合并、按 rev 判新旧：收到别人的数组时只按 id
  // upsert 自己关心的条目，绝不能整表覆盖——整表覆盖会删掉对方在这之间
  // 新增的任务（本次修复的正是这个丢任务问题）。
  // -------------------------------------------------------------------------
  long long taskRevOf(const json &t)
  {
    if (!t.is_object()) return 0;
    auto it = t.find("rev");
    if (it == t.end()) return 0;                        // 老看板不带 rev → 0
    if (it->is_number_integer()) return it->get<long long>();
    if (it->is_number_float()) return static_cast<long long>(it->get<double>());
    if (it->is_string()) {                              // 容错："7" 这种字符串
      try {
        return std::stoll(it->get<std::string>());
      } catch (const std::exception &) {
        return 0;
      }
    }
    return 0;
  }

  // 安全读取任务 id。绝不能用 json::value<int>("id", 0)：线上报文里 id 可能被
  // 写成字符串（"7"），value<int>() 会抛 type_error，而本函数的上游
  // （onFleetTasks）跑在 libmosquitto 网络线程里，异常逃出去会直接崩节点。
  int taskIdOf(const json &t)
  {
    if (!t.is_object()) return 0;
    auto it = t.find("id");
    if (it == t.end()) return 0;
    if (it->is_number_integer()) return static_cast<int>(it->get<long long>());
    if (it->is_number_float()) return static_cast<int>(it->get<double>());
    if (it->is_string()) {
      try {
        return std::stoi(it->get<std::string>());
      } catch (const std::exception &) {
        return 0;
      }
    }
    return 0;
  }

  // 安全读取 status（非字符串一律当空串，不让异常逃出 MQTT 回调）
  std::string taskStatusOf(const json &t)
  {
    if (!t.is_object()) return std::string();
    auto it = t.find("status");
    if (it == t.end() || !it->is_string()) return std::string();
    return it->get<std::string>();
  }

  // 在任务数组里按 id 找条目；找不到返回 arr.end()
  json::iterator findTaskById(json &arr, int id)
  {
    for (auto it = arr.begin(); it != arr.end(); ++it) {
      if (it->is_object() && taskIdOf(*it) == id) return it;
    }
    return arr.end();
  }

  // 给每条任务补上 rev 缺省值（0），让看板侧可以无条件按 rev 比较新旧
  void normalizeRev(json &arr)
  {
    if (!arr.is_array()) return;
    for (auto &t : arr) {
      if (t.is_object() && !t.contains("rev")) t["rev"] = 0;
    }
  }

}  // namespace

  // =========================================================================
  // 开始前往目标点
  // =========================================================================
  void TaskRunnerNode::startNavigation(const std::string &target_id, const std::string &title,
                                       int task_id, const json &task_obj)
  {
    if (target_id.empty()) {
      RCLCPP_WARN(get_logger(), "任务未指定目标点，忽略");
      publishEvent("warn", "任务缺少目标点，已拒绝执行");
      writeBackTask(task_id, "revoked", "任务缺少目标点");
      return;
    }
    // 点位表在 site_map.hpp（与 mqtt_bridge 发布的看板地图同源，不会漂移）：
    // 先按 id 精确匹配，再按名称模糊匹配
    const qmb::SitePoint *found = qmb::findSitePoint(target_id);
    if (!found) {
      RCLCPP_WARN(get_logger(), "未知目标点 '%s'，可用点位见启动日志", target_id.c_str());
      // 安全要求：非法目标必须"拒绝 + 留痕"，不能静默忽略（看板否则无从得知）
      char buf[192];
      std::snprintf(buf, sizeof(buf), "任务#%d 目标 '%s' 不在点位表，已拒绝执行",
        task_id, target_id.c_str());
      publishEvent("warn", buf);
      writeBackTask(task_id, "revoked", "目标点不在点位表");
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
    publishTaskState();
    // 派单接管控制权：先让巡逻停手，否则两个节点同时发导航目标互相打断
    pausePatrol(true);
    RCLCPP_INFO(get_logger(), "开始前往 [%s] %s  目标=(%.2f, %.2f) 任务#%d",
      found->id, found->name, found->x, found->y, task_id);

    // 派单时 Nav2 很可能还没起来（启动时序：bridge ≈5 s、Nav2 ≈20 s）：
    // 先把目标放进待下发队列，由 onNavTimer 每 2 s 重试，最多 kNavRetryMax 次。
    // 这里【不阻塞】等 action server——本函数跑在 MQTT 网络线程上。
    enqueueNavGoal(found->x, found->y, found->name, task_id);
    pumpPendingGoal();
  }


  // =========================================================================
  // 下发 Nav2 导航目标（map 坐标系）
  //
  // 只在 action server 就绪时才由 pumpPendingGoal() 调用，所以这里【不阻塞】：
  // 旧实现在这里 wait_for_action_server(2s)，跑在 MQTT 网络线程上会把下行堵死。
  // =========================================================================
  void TaskRunnerNode::sendNavGoal(double x, double y, const std::string & name)
  {
    int task_id = 0;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      task_id = active_task_id_;
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
      [this, name, task_id](const NavGoalHandle::SharedPtr & handle) {
        if (!handle) {
          // 目标被拒绝：Nav2 刚起来时行为树/规划器可能还没就绪，直接判失败太早。
          // 清掉 inflight，交给 onNavTimer 按间隔重试；重试次数用完才判失败。
          int k = 0;
          {
            std::lock_guard<std::mutex> lk(data_mutex_);
            if (task_id != 0 && active_task_id_ != 0 && active_task_id_ != task_id) {
              return;                        // 旧目标的迟到回调，别影响新任务
            }
            if (!pending_goals_.empty()) {
              PendingGoal &f = pending_goals_.front();
              f.inflight = false;
              f.next_try = std::chrono::steady_clock::now() +
                std::chrono::seconds(kNavRetryIntervalS);
              k = f.attempts;
            }
          }
          RCLCPP_WARN(get_logger(), "Nav2 拒绝了目标 %s（第 %d/%d 次），稍后重试",
            name.c_str(), k, kNavRetryMax);
          return;
        }
        std::lock_guard<std::mutex> lk(data_mutex_);
        nav_goal_ = handle;
        pending_goals_.clear();          // 已被接受：撤销重试
        RCLCPP_INFO(get_logger(), "Nav2 已接受目标 %s", name.c_str());
      };
    options.result_callback =
      [this, name, task_id](const NavGoalHandle::WrappedResult & result) {
        {
          std::lock_guard<std::mutex> lk(data_mutex_);
          // 迟到的旧目标结果：不能拿来结束当前这个任务
          if (task_id != 0 && active_task_id_ != 0 && active_task_id_ != task_id) {
            RCLCPP_WARN(get_logger(), "忽略旧目标 %s 的迟到结果", name.c_str());
            return;
          }
        }
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
          finishTask("Nav2 未能到达目标点", Outcome::Failed);
        }
      };
    nav_client_->async_send_goal(goal, options);
  }


  // 结束当前任务：清状态、停车、恢复巡逻、回写任务状态
  void TaskRunnerNode::finishTask(const std::string & detail, Outcome outcome)
  {
    int tid = 0;
    std::string name;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      navigating_ = false;
      status_ = "idle";
      task_.clear();
      nav_goal_.reset();
      pending_goals_.clear();        // 任务结束：绝不再补发旧目标
      tid = active_task_id_;
      active_task_id_ = 0;
      name = nav_target_name_;
    }
    pub_cmd_vel_->publish(geometry_msgs::msg::Twist());   // 确保停车
    pausePatrol(false);
    publishTaskState();

    // 3.5 任务回写：成功 → completed；取消/急停 → revoked；
    // 失败 → 同样回写 revoked（契约里只有 draft/published/completed/revoked
    // 四种状态，失败归入 revoked），并把原因写进 statusNote。
    // 【为什么失败也要回写】旧实现失败时不回写任何状态，看板上任务永远停在
    // "已发布"，用户无从知道机器已经放弃了——这与本文件开头"非法目标必须
    // 拒绝 + 留痕，不能静默忽略"的原则自相矛盾，属于静默失败。
    if (outcome == Outcome::Success) {
      writeBackTask(tid, "completed");
      // 任务完成事件（event 主题允许 task_runner 发布）：看板告警中心/日志留痕
      char buf[192];
      std::snprintf(buf, sizeof(buf), "任务#%d 完成：已到达 %s",
        tid, detail.empty() ? name.c_str() : detail.c_str());
      publishEvent("info", buf);
    } else if (outcome == Outcome::Canceled) {
      writeBackTask(tid, "revoked", "已取消");
    } else {
      const std::string why = detail.empty() ? std::string("执行失败") : detail;
      writeBackTask(tid, "revoked", why);
      RCLCPP_WARN(get_logger(), "任务#%d 失败，已回写 revoked（原因：%s）", tid, why.c_str());
    }
  }


  // 取消当前导航目标（看板"取消"按钮 / 急停都走这里）
  void TaskRunnerNode::cancelNavigation(const char * reason)
  {
    std::shared_ptr<NavGoalHandle> handle;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      handle = nav_goal_;
    }
    // 待下发队列一并清掉：否则还在排队的旧目标会在取消之后被重试补发出去
    clearPendingGoals();
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
  // 【按 id 合并，不再整表覆盖】看板是任务列表的主要写入方，机器人执行结束后
  // 必须把该任务的 status 改成 completed / revoked 并 retained 重新发布，否则
  // 看板上永远停在"已发布"。但 MQTT 只能发【全量数组】，所以规则是：
  //   ① 基准 = 最新一次收到的远端表（fleet_tasks_），不是本地旧缓存；
  //   ② 只 upsert 自己关心的那一个 id（status/updatedAt/rev/statusNote），
  //      其余条目——包括收到快照之后看板新增的任务——原样带出去；
  //   ③ 每条写出的条目都带单调递增的 rev（语义见文件开头 taskRevOf 注释），
  //      两端冲突时以 rev 大者为准，rev 相等即同一版本。
  // =========================================================================
  void TaskRunnerNode::writeBackTask(int task_id, const char *new_status,
                                     const std::string &reason)
  {
    if (task_id <= 0) return;      // 无 id（如手动测试报文）无可回写

    std::string payload;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      const long long ts = qmb::wallClockMs();
      const std::string st(new_status);

      // 已知 rev：本机上次为这个 id 写过的 rev，与基准表里该条目的 rev，取大者
      long long known_rev = 0;
      auto it_term = task_terminal_.find(task_id);
      if (it_term != task_terminal_.end()) known_rev = it_term->second.rev;

      json base = fleet_tasks_.is_array() ? fleet_tasks_ : json::array();
      auto slot = findTaskById(base, task_id);
      if (slot != base.end()) {
        known_rev = std::max(known_rev, taskRevOf(*slot));
      } else if (!cached_task_.is_object()) {
        // 既没收到看板快照、手上也没有任务对象：先把终态记下来，
        // 等 fleet/tasks 到了由 onFleetTasks() 按 id 补回（不凭空造一条出来）
        task_terminal_[task_id] = TerminalStatus{st, ts, known_rev + 1};
        RCLCPP_WARN(get_logger(), "任务#%d 状态回写跳过：既无列表快照也无任务对象", task_id);
        return;
      }

      const long long rev = known_rev + 1;
      json t = (slot != base.end()) ? *slot : cached_task_;
      t["id"] = task_id;
      t["status"] = st;
      t["updatedAt"] = ts;
      t["rev"] = rev;
      if (!reason.empty()) t["statusNote"] = reason;    // 终态原因（可选字段）
      if (slot != base.end()) *slot = t; else base.push_back(t);

      normalizeRev(base);       // 其余条目只补 rev 缺省值，内容一律不动

      task_terminal_[task_id] = TerminalStatus{st, ts, rev};
      fleet_tasks_ = base;
      payload = base.dump();
    }

    int rc = qmb::publishText(mosq_, fleet_tasks_topic_, payload, 1, /*retain=*/true);
    if (rc == MOSQ_ERR_SUCCESS) {
      RCLCPP_INFO(get_logger(), "任务#%d 已回写 %s（fleet/tasks %zu 字节，按 id 合并）",
        task_id, new_status, payload.size());
    } else {
      RCLCPP_WARN(get_logger(), "任务#%d 回写失败: %s",
        task_id, mosquitto_strerror(rc));
    }
  }


  // 收到看板的任务列表快照：以它为新基准，只把"本机已经做出、且远端还没采纳"
  // 的终态按 id 补回去；远端新增/修改过的任务一律原样保留（绝不整表覆盖）。
  void TaskRunnerNode::onFleetTasks(const std::string &text)
  {
    json arr;
    try {
      arr = json::parse(text);
    } catch (const std::exception &e) {
      RCLCPP_WARN(get_logger(), "任务列表解析失败: %s", e.what());
      return;
    }
    if (!arr.is_array()) return;

    size_t patched = 0;
    bool connected = false;
    std::string payload;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      for (auto &t : arr) {
        if (!t.is_object()) continue;
        const int id = taskIdOf(t);
        auto it = task_terminal_.find(id);
        if (it == task_terminal_.end()) continue;      // 与本机无关：原样保留
        const long long remote_rev = taskRevOf(t);
        if (remote_rev > it->second.rev) {
          // 远端版本更高：看板在这之后又改过这条 → 采纳远端，本机不再补回
          RCLCPP_INFO(get_logger(), "任务#%d 远端 rev=%lld 高于本机 rev=%lld，采纳远端",
            id, remote_rev, it->second.rev);
          continue;
        }
        if (taskStatusOf(t) == it->second.status && remote_rev == it->second.rev) {
          continue;                                    // 同一版本：幂等跳过
        }
        // 只改这一条（upsert），数组里其它任务一个字都不动
        t["status"] = it->second.status;
        t["updatedAt"] = it->second.ts;
        t["rev"] = it->second.rev;
        ++patched;
      }
      // 远端列表里本机不认识的条目（看板刚新增的）全部原样收下，一条都不删
      fleet_tasks_ = arr;
      connected = mqtt_connected_;
      if (patched > 0) {
        normalizeRev(fleet_tasks_);
        payload = fleet_tasks_.dump();
      }
    }
    RCLCPP_INFO(get_logger(), "已同步看板任务列表：%zu 条，按 id 补回终态 %zu 条",
      arr.size(), patched);

    // 只有真的补过才回发，避免和看板来回刷（看板收到后按 id+rev 合并不回发，
    // 不存在无限循环）。回发的表里带着看板刚新增的条目，不会把它们删掉。
    if (patched > 0 && connected) {
      qmb::publishText(mosq_, fleet_tasks_topic_, payload, 1, /*retain=*/true);
    }
  }


  // =========================================================================
  // 任务维护定时器（20 Hz）：初始位姿对齐 + Nav2 目标重试 + 任务超时兜底
  //
  // 注意：这里【不再】直接发 cmd_vel 驱动轮子。行驶由 Nav2 的
  // controller_server 负责（它会看局部代价地图绕障），本函数只做状态维护。
  // =========================================================================
  void TaskRunnerNode::onNavTimer()
  {
    // ① 启动后补发一次 AMCL 初始位姿（见构造函数注释）
    bool odom_ready = false;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      odom_ready = odom_ok_;
    }
    if (!initial_pose_sent_ && odom_ready && (now() - start_time_).seconds() > 8.0) {
      publishInitialPose();
      initial_pose_sent_ = true;
    }

    // ② 待下发的 Nav2 目标：server 未就绪 / 上次 send 失败 → 按 2 s 间隔重试
    pumpPendingGoal();

    // ③ 任务超时兜底：Nav2 长时间无结果时主动取消，避免"永远 working"
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


  // -------------------------------------------------------------------------
  // 待下发目标队列：入队 / 清空 / 重试泵
  // -------------------------------------------------------------------------
  void TaskRunnerNode::enqueueNavGoal(double x, double y, const std::string & name, int task_id)
  {
    std::lock_guard<std::mutex> lk(data_mutex_);
    // 同一时刻只允许一个待下发目标：新任务直接顶替（旧目标绝不能被补发）
    pending_goals_.clear();
    PendingGoal g;
    g.x = x;
    g.y = y;
    g.name = name;
    g.task_id = task_id;
    g.seq = ++pending_seq_;
    g.attempts = 0;
    g.inflight = false;
    g.next_try = std::chrono::steady_clock::now();   // 立刻尝试第一次
    pending_goals_.push_back(g);
  }

  void TaskRunnerNode::clearPendingGoals()
  {
    std::lock_guard<std::mutex> lk(data_mutex_);
    pending_goals_.clear();
  }

  // 重试泵（由 20 Hz 的 onNavTimer 驱动，每次最多做一轮尝试）：
  //   · action server 还没就绪  → 记一次尝试，2 s 后再来（日志"第 k 次"）
  //   · 上次发出后 5 s 没等到 goal_response / 被 Nav2 拒绝 → 同样按间隔重试
  //   · 尝试满 kNavRetryMax 次（≈30 s）仍发不出去 → 判失败并回写 revoked
  void TaskRunnerNode::pumpPendingGoal()
  {
    PendingGoal g;
    int attempt = 0;
    bool give_up = false;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      if (pending_goals_.empty()) return;
      PendingGoal &f = pending_goals_.front();
      const auto tp = std::chrono::steady_clock::now();
      if (f.inflight) {
        if (tp - f.sent_at < std::chrono::seconds(kNavInflightTimeoutS)) return;
        f.inflight = false;          // 等回应超时：当这次下发失败，准备重发
        f.next_try = tp;
        RCLCPP_WARN(get_logger(), "目标 %s 等待 Nav2 回应超时，准备重发（第 %d/%d 次）",
          f.name.c_str(), f.attempts, kNavRetryMax);
      }
      if (tp < f.next_try) return;
      if (f.attempts >= kNavRetryMax) {
        give_up = true;              // 次数用完：到锁外判失败
        g = f;
      } else {
        f.attempts += 1;
        f.inflight = true;
        f.sent_at = tp;
        attempt = f.attempts;
        g = f;
      }
    }

    if (give_up) {
      char buf[224];
      std::snprintf(buf, sizeof(buf),
        "Nav2 未就绪：目标 %s 重试 %d 次（约 %d s）仍未下发，本次任务放弃",
        g.name.c_str(), kNavRetryMax, kNavRetryMax * kNavRetryIntervalS);
      RCLCPP_WARN(get_logger(), "%s", buf);
      publishEvent("warning", buf);
      finishTask("Nav2 未就绪（重试用尽）", Outcome::Failed);   // 失败也回写
      return;
    }

    if (!nav_client_->wait_for_action_server(0s)) {
      // Nav2 还没起来（比 bridge 晚约 15 s）：本轮不算数，排到 2 s 后再试
      bool alive = false;
      {
        std::lock_guard<std::mutex> lk(data_mutex_);
        if (!pending_goals_.empty() && pending_goals_.front().seq == g.seq) {
          PendingGoal &f = pending_goals_.front();
          f.inflight = false;
          f.next_try = std::chrono::steady_clock::now() +
            std::chrono::seconds(kNavRetryIntervalS);
          alive = true;
        }
      }
      if (alive) {
        RCLCPP_INFO(get_logger(), "正在等待 Nav2 就绪（第 %d/%d 次，目标 %s，%d s 后重试）",
          attempt, kNavRetryMax, g.name.c_str(), kNavRetryIntervalS);
      }
      return;
    }

    RCLCPP_INFO(get_logger(), "Nav2 已就绪，下发目标 %s（第 %d/%d 次尝试，任务#%d）",
      g.name.c_str(), attempt, kNavRetryMax, g.task_id);
    sendNavGoal(g.x, g.y, g.name);
  }


  // 用当前里程计位姿对齐 AMCL，消除初始定位偏差
  void TaskRunnerNode::publishInitialPose()
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
  void TaskRunnerNode::pausePatrol(bool paused)
  {
    std_msgs::msg::Bool m;
    m.data = !paused;
    pub_patrol_enable_->publish(m);
  }
