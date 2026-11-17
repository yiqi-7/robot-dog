#pragma once
// ===========================================================================
// common.hpp —— 三个 MQTT 节点共用的小工具（无状态纯函数 + 一条发布封装）
//
// 【为什么有这个文件】
// 原来单进程 mqtt_bridge 把遥测/任务/语音塞在一个类里。现在按单一职责拆成
//   mqtt_bridge  遥测上行 + 站点地图 + 告警
//   task_runner  指令分发 + Nav2 导航 + 任务回写
//   voice_link   语音上行（§3.1）+ TTS 播报（§3.2）
// 三个节点各自独立连 broker（各自的客户端 ID），互不依赖。
// 凡是与"节点职责"无关的小工具都抽到这里，避免三份拷贝——三份拷贝必然漂移。
//
// 【边界】这里只放【无状态】帮助函数：
//   · 站点表（kZones/kPoints）在 site_map.hpp（mqtt_bridge 发布地图、
//     task_runner 解析目标点，两边都要，但绝不能塞进 common 变成"公共垃圾桶"）
//   · 坐标缩放（仿真世界 ⇄ 看板地图）只在 mqtt_bridge 节点里，看板契约归它
// ===========================================================================

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

#include <mosquitto.h>
#include <nlohmann/json.hpp>

// 全部 MQTT 报文统一用它，避免三个节点各写一种 JSON 类型
using json = nlohmann::json;

namespace qmb {

// 角度归一化到 [-pi, pi]，避免偏航累计超过一圈后跳变
inline double wrapPi(double a)
{
  while (a > M_PI) a -= 2.0 * M_PI;
  while (a < -M_PI) a += 2.0 * M_PI;
  return a;
}

// 四元数 → 偏航角
inline double yawFromQuat(double x, double y, double z, double w)
{
  return std::atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z));
}

// Base64 编码（VOICE-INTERFACE.md §3.1 的 data 字段）。
// 只有这一处用途，手写省一个依赖；每 3 字节 → 4 字符，末尾补 '='。
inline std::string base64Encode(const uint8_t *data, size_t len)
{
  static const char kTab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t b0 = data[i];
    const uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0;
    const uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0;
    const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(kTab[(v >> 18) & 0x3F]);
    out.push_back(kTab[(v >> 12) & 0x3F]);
    out.push_back((i + 1 < len) ? kTab[(v >> 6) & 0x3F] : '=');
    out.push_back((i + 2 < len) ? kTab[v & 0x3F] : '=');
  }
  return out;
}

// 当前墙上时钟的毫秒时间戳（Unix epoch ms）。
// 【不能】用 rclcpp 的 now()：节点受 use_sim_time 控制，Gazebo 仿真时钟的
// nanoseconds() 不是 Unix epoch，直接当毫秒用会得到 217835 这种荒唐值，
// 看板按 updatedAt/ts 排序、显示"多久之前"就全乱了。
// 所有 MQTT 报文里的时间戳统一用系统时钟，与看板 Date.now() 语义一致。
inline long long wallClockMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch()).count();
}

// 把一段文本发布到 topic；返回 mosquitto 返回码，由调用方决定怎么记日志。
// 空指针保护：节点初始化早期（mosquitto_new 失败）也会走到发布路径。
inline int publishText(struct mosquitto *mosq, const std::string &topic,
                       const std::string &payload, int qos, bool retain)
{
  if (!mosq) return MOSQ_ERR_INVAL;
  return mosquitto_publish(mosq, nullptr, topic.c_str(),
    static_cast<int>(payload.size()), payload.c_str(), qos, retain);
}

}  // namespace qmb
