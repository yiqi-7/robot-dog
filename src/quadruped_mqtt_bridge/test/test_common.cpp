// ===========================================================================
// test_common.cpp —— common.hpp 纯函数 + site_map.hpp 静态表的单元测试
//
// 【为什么有这个文件】本包此前 0 测试，遥测/任务/语音全靠手工验证。这里先把
// 【不需要 ROS 运行时】的部分锁住：
//   · common.hpp   wrapPi / yawFromQuat / base64Encode / wallClockMs / publishText
//   · site_map.hpp kZones / kPoints 静态表自洽性 + findSitePoint 解析规则
//
// 【不覆盖什么】MqttBridgeNode / TaskRunnerNode / VoiceLinkNode 三个节点类需要
// rclcpp executor、真实 mosquitto broker、Nav2 action server 才能驱动，属于集成
// 测试范畴，本文件故意不碰（那部分要么接 broker 打桩，要么上 launch_testing）。
//
// 运行：
//   colcon test --packages-select quadruped_mqtt_bridge
//   colcon test-result --verbose
// ===========================================================================

#include <gtest/gtest.h>

#include <quadruped_mqtt_bridge/common.hpp>
#include <quadruped_mqtt_bridge/site_map.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

// 独立实现的 Base64 解码（RFC 4648），只用于验证 base64Encode 的输出符合标准。
// 故意不复用 common.hpp 里的编码表：自己验自己抓不到错。
bool refBase64Decode(const std::string &in, std::vector<uint8_t> *out)
{
  auto value = [](char c) -> int {
      if (c >= 'A' && c <= 'Z') return c - 'A';
      if (c >= 'a' && c <= 'z') return c - 'a' + 26;
      if (c >= '0' && c <= '9') return c - '0' + 52;
      if (c == '+') return 62;
      if (c == '/') return 63;
      return -1;
    };
  if (in.size() % 4 != 0) return false;
  out->clear();
  for (size_t i = 0; i < in.size(); i += 4) {
    int q[4] = {0, 0, 0, 0};
    int pads = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = in[i + static_cast<size_t>(k)];
      if (c == '=') {
        if (i + 4 != in.size()) return false;  // '=' 只允许出现在最后一组
        ++pads;
        continue;
      }
      if (pads > 0) return false;  // '=' 之后不能再出现字符
      q[k] = value(c);
      if (q[k] < 0) return false;
    }
    const uint32_t v = (static_cast<uint32_t>(q[0]) << 18) |
                       (static_cast<uint32_t>(q[1]) << 12) |
                       (static_cast<uint32_t>(q[2]) << 6) |
                       static_cast<uint32_t>(q[3]);
    out->push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    if (pads < 2) out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    if (pads < 1) out->push_back(static_cast<uint8_t>(v & 0xFF));
  }
  return true;
}

std::string encodeString(const std::string &s)
{
  return qmb::base64Encode(reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

// 由"绕 Z 轴转 yaw 弧度"构造四元数（纯数学，不经过被测代码）
void quatFromYaw(double yaw, double *x, double *y, double *z, double *w)
{
  *x = 0.0;
  *y = 0.0;
  *z = std::sin(yaw / 2.0);
  *w = std::cos(yaw / 2.0);
}

size_t countChar(const std::string &s, char c)
{
  return static_cast<size_t>(std::count(s.begin(), s.end(), c));
}

const qmb::SiteZone *findZone(const char *id)
{
  for (const auto &z : qmb::kZones) {
    if (std::string(z.id) == id) return &z;
  }
  return nullptr;
}

}  // namespace

// ===========================================================================
// wrapPi：角度归一化到 [-pi, pi]
// ===========================================================================

TEST(CommonWrapPi, ZeroIsUnchanged)
{
  EXPECT_DOUBLE_EQ(qmb::wrapPi(0.0), 0.0);
}

// 归一化区间是【闭区间】[-pi, pi]：恰好 ±pi 不再移动。这是刻意选择——
// 端点幂等，避免姿态在 ±pi 边界上反复抖动 2pi。
TEST(CommonWrapPi, BoundaryIsInclusive)
{
  EXPECT_DOUBLE_EQ(qmb::wrapPi(kPi), kPi);
  EXPECT_DOUBLE_EQ(qmb::wrapPi(-kPi), -kPi);
  // 稍微越过边界立刻折回另一侧
  EXPECT_NEAR(qmb::wrapPi(kPi + 1e-12), -kPi, 1e-9);
  EXPECT_NEAR(qmb::wrapPi(-kPi - 1e-12), kPi, 1e-9);
}

// 多圈缠绕：绕一圈/两圈/37 圈都要回到同一角度（while 循环而不是一次取模）
TEST(CommonWrapPi, MultiTurnWinding)
{
  const double base = 0.37;
  for (int k = -5; k <= 5; ++k) {
    if (k == 0) continue;
    EXPECT_NEAR(qmb::wrapPi(base + 2.0 * kPi * static_cast<double>(k)), base, 1e-9)
      << "k=" << k;
  }
  EXPECT_NEAR(qmb::wrapPi(0.5 + 2.0 * kPi * 37.0), 0.5, 1e-9);
  EXPECT_NEAR(qmb::wrapPi(-0.5 - 2.0 * kPi * 37.0), -0.5, 1e-9);
}

// 值域 + 三角恒等 + 幂等：任意输入都落在 [-pi, pi]，且与输入指向同一方向
TEST(CommonWrapPi, RangeIdentityAndIdempotent)
{
  for (double a = -25.0; a <= 25.0; a += 0.017) {
    const double r = qmb::wrapPi(a);
    EXPECT_GE(r, -kPi - 1e-12) << "a=" << a;
    EXPECT_LE(r, kPi + 1e-12) << "a=" << a;
    EXPECT_NEAR(std::sin(r), std::sin(a), 1e-9) << "a=" << a;
    EXPECT_NEAR(std::cos(r), std::cos(a), 1e-9) << "a=" << a;
    EXPECT_DOUBLE_EQ(qmb::wrapPi(r), r) << "a=" << a;  // 已归一的值再归一不变
  }
}

// ===========================================================================
// yawFromQuat：四元数 → 偏航角
// ===========================================================================

TEST(CommonYaw, IdentityQuaternionIsZero)
{
  EXPECT_NEAR(qmb::yawFromQuat(0.0, 0.0, 0.0, 1.0), 0.0, 1e-12);
}

TEST(CommonYaw, QuarterTurnIsHalfPi)
{
  double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
  quatFromYaw(kPi / 2.0, &x, &y, &z, &w);
  EXPECT_NEAR(qmb::yawFromQuat(x, y, z, w), kPi / 2.0, 1e-9);
}

TEST(CommonYaw, NegativeQuarterTurnIsMinusHalfPi)
{
  double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
  quatFromYaw(-kPi / 2.0, &x, &y, &z, &w);
  EXPECT_NEAR(qmb::yawFromQuat(x, y, z, w), -kPi / 2.0, 1e-9);
}

TEST(CommonYaw, HalfTurnIsPi)
{
  double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
  quatFromYaw(kPi, &x, &y, &z, &w);
  EXPECT_NEAR(qmb::yawFromQuat(x, y, z, w), kPi, 1e-9);
}

// yaw 回绕：转 270° 与转 -90° 是同一姿态，输出必须落在 [-pi, pi] 内
TEST(CommonYaw, ThreeQuarterTurnWrapsNegative)
{
  double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
  quatFromYaw(3.0 * kPi / 2.0, &x, &y, &z, &w);
  const double yaw = qmb::yawFromQuat(x, y, z, w);
  EXPECT_NEAR(yaw, -kPi / 2.0, 1e-9);
  EXPECT_NEAR(yaw, qmb::wrapPi(3.0 * kPi / 2.0), 1e-9);
}

// 转 450°（= 90° + 一整圈）应和转 90° 得到同一个 yaw
TEST(CommonYaw, ExtraFullTurnIsEquivalent)
{
  double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
  quatFromYaw(kPi / 2.0 + 2.0 * kPi, &x, &y, &z, &w);
  EXPECT_NEAR(qmb::yawFromQuat(x, y, z, w), kPi / 2.0, 1e-9);
}

// 连续扫过 ±3 圈：yaw 必须始终在 [-pi, pi] 且与 wrapPi(输入) 一致
TEST(CommonYaw, AgreesWithWrapPiAcrossTurns)
{
  for (double yaw0 = -3.0 * kPi; yaw0 <= 3.0 * kPi + 1e-9; yaw0 += 0.05) {
    double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
    quatFromYaw(yaw0, &x, &y, &z, &w);
    const double got = qmb::yawFromQuat(x, y, z, w);
    EXPECT_GE(got, -kPi - 1e-9) << "yaw0=" << yaw0;
    EXPECT_LE(got, kPi + 1e-9) << "yaw0=" << yaw0;
    EXPECT_NEAR(got, qmb::wrapPi(yaw0), 1e-9) << "yaw0=" << yaw0;
    EXPECT_NEAR(std::sin(got), std::sin(yaw0), 1e-9) << "yaw0=" << yaw0;
    EXPECT_NEAR(std::cos(got), std::cos(yaw0), 1e-9) << "yaw0=" << yaw0;
  }
}

// ===========================================================================
// base64Encode：语音上行 {P}/robots/{id}/audio 的 data 字段
// ===========================================================================

TEST(CommonBase64, EmptyString)
{
  EXPECT_EQ(encodeString(""), "");
}

// RFC 4648 §10 标准测试向量：覆盖 1/2/3 字节与各种 padding 组合
TEST(CommonBase64, Rfc4648Vectors)
{
  EXPECT_EQ(encodeString("f"), "Zg==");       // 1 字节 → 2 个 '='
  EXPECT_EQ(encodeString("fo"), "Zm8=");      // 2 字节 → 1 个 '='
  EXPECT_EQ(encodeString("foo"), "Zm9v");     // 3 字节 → 无填充
  EXPECT_EQ(encodeString("foob"), "Zm9vYg==");
  EXPECT_EQ(encodeString("fooba"), "Zm9vYmE=");
  EXPECT_EQ(encodeString("foobar"), "Zm9vYmFy");
}

// 含中文的 UTF-8 字节串（语音转写文本会走这里）
TEST(CommonBase64, Utf8Chinese)
{
  const std::string ji = "机";  // 3 字节 E6 9C BA，正好一组
  ASSERT_EQ(ji.size(), 3u);
  EXPECT_EQ(encodeString(ji), "5py6");

  const std::string zh = "机器人";  // 9 字节，3 的倍数 → 无填充
  ASSERT_EQ(zh.size(), 9u);
  EXPECT_EQ(encodeString(zh), "5py65Zmo5Lq6");
  EXPECT_EQ(encodeString(zh).find('='), std::string::npos);
}

// 长度 0..64 全扫：输出长度、padding 数量、可被独立解码器还原
TEST(CommonBase64, LengthsAndRoundTrip)
{
  for (size_t len = 0; len <= 64; ++len) {
    std::vector<uint8_t> raw(len);
    for (size_t i = 0; i < len; ++i) {
      raw[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
    }
    const std::string enc = qmb::base64Encode(raw.data(), raw.size());
    EXPECT_EQ(enc.size(), (len + 2) / 3 * 4) << "len=" << len;
    EXPECT_EQ(countChar(enc, '='), (3 - len % 3) % 3) << "len=" << len;

    std::vector<uint8_t> back;
    ASSERT_TRUE(refBase64Decode(enc, &back)) << "len=" << len << " enc=" << enc;
    EXPECT_EQ(back, raw) << "len=" << len;
  }
}

// 输出只含标准字母表字符，且 '=' 只能出现在末尾
TEST(CommonBase64, AlphabetAndTailPaddingOnly)
{
  static const std::string kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (size_t len = 0; len <= 32; ++len) {
    std::string raw(len, 'x');
    const std::string enc = encodeString(raw);
    for (size_t i = 0; i < enc.size(); ++i) {
      const char c = enc[i];
      if (c == '=') {
        EXPECT_GE(i + 2, enc.size()) << "len=" << len;  // 只允许最后两格
      } else {
        EXPECT_NE(kAlphabet.find(c), std::string::npos)
          << "len=" << len << " 非法字符 " << c;
      }
    }
  }
}

// ===========================================================================
// wallClockMs：必须是 Unix epoch 毫秒（不能是 ROS 仿真时钟）
// ===========================================================================

TEST(CommonWallClock, LooksLikeEpochMilliseconds)
{
  const long long ms = qmb::wallClockMs();
  // 下界 1.7e12 ms ≈ 2023-11-14；用 use_sim_time 的 nanoseconds() 会得到 217835 这种值
  EXPECT_GT(ms, 1700000000000LL);
  EXPECT_LT(ms, 4102444800000LL);  // < 2100-01-01
  const long long sys_now = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  EXPECT_LT(std::llabs(ms - sys_now), 5000LL);
}

TEST(CommonWallClock, NonDecreasingAcrossCalls)
{
  long long prev = qmb::wallClockMs();
  for (int i = 0; i < 1000; ++i) {
    const long long now = qmb::wallClockMs();
    EXPECT_GE(now, prev) << "iteration " << i;
    prev = now;
  }
}

// ===========================================================================
// publishText：空句柄保护（节点初始化早期会走到）
// ===========================================================================

TEST(CommonPublish, NullClientReturnsInvalid)
{
  EXPECT_EQ(qmb::publishText(nullptr, "qbot/v1/event", "{}", 1, false), MOSQ_ERR_INVAL);
}

// ===========================================================================
// site_map.hpp：静态站点表自洽性
// ===========================================================================

TEST(SiteMap, ZonesAreWellFormed)
{
  ASSERT_GT(qmb::kZones.size(), 0u);
  for (const auto &z : qmb::kZones) {
    ASSERT_NE(z.id, nullptr);
    ASSERT_NE(z.name, nullptr);
    EXPECT_GT(std::string(z.name).size(), 0u) << z.id;
    EXPECT_GT(z.w, 0.0) << z.id;
    EXPECT_GT(z.h, 0.0) << z.id;
  }
  for (size_t i = 0; i < qmb::kZones.size(); ++i) {
    for (size_t j = i + 1; j < qmb::kZones.size(); ++j) {
      EXPECT_STRNE(qmb::kZones[i].id, qmb::kZones[j].id);
      EXPECT_STRNE(qmb::kZones[i].name, qmb::kZones[j].name);
    }
  }
}

// 六个区域是互不重叠的厂房区块（互相贴边：x=±1.3 / y=±0.6 走廊、x=4.6 与
// y=3.4~3.6 隔墙）。出现正面积重叠说明表被复制粘贴写错了。
TEST(SiteMap, ZonesDoNotOverlap)
{
  for (size_t i = 0; i < qmb::kZones.size(); ++i) {
    for (size_t j = i + 1; j < qmb::kZones.size(); ++j) {
      const auto &a = qmb::kZones[i];
      const auto &b = qmb::kZones[j];
      const double ox = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x);
      const double oy = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
      EXPECT_FALSE(ox > 1e-9 && oy > 1e-9)
        << a.id << " 与 " << b.id << " 区域重叠（overlap " << ox << " x " << oy << " m）";
    }
  }
}

TEST(SiteMap, PointsAreWellFormed)
{
  ASSERT_GT(qmb::kPoints.size(), 0u);
  for (const auto &p : qmb::kPoints) {
    ASSERT_NE(p.id, nullptr);
    ASSERT_NE(p.name, nullptr);
    EXPECT_GT(std::string(p.name).size(), 0u) << p.id;
    if (p.kind != nullptr) {
      const std::string kind = p.kind;
      EXPECT_TRUE(kind == "waypoint" || kind == "charger") << p.id << " kind=" << kind;
    }
    if (p.zone != nullptr) {
      EXPECT_NE(findZone(p.zone), nullptr) << p.id << " 指向不存在的区域 " << p.zone;
    }
  }
  // id / 名称都必须唯一：看板按【名称】下发 target、语音按【id】下发 target，
  // 两条路都不能出现歧义
  for (size_t i = 0; i < qmb::kPoints.size(); ++i) {
    for (size_t j = i + 1; j < qmb::kPoints.size(); ++j) {
      EXPECT_STRNE(qmb::kPoints[i].id, qmb::kPoints[j].id);
      EXPECT_STRNE(qmb::kPoints[i].name, qmb::kPoints[j].name);
    }
  }
}

// 坐标不许越界：有 zone 的点必须落在该区域矩形内；没有 zone 的点（厂区北门/
// 环路）位于区域之间的走廊，只要求落在全厂范围内。
// 说明：site_map.hpp 没有单独的世界尺寸常量（600m×380m 是看板契约里的数值），
// 所以世界范围由区域表自身的外包矩形推出。
TEST(SiteMap, PointsLieInsideDeclaredZone)
{
  ASSERT_GT(qmb::kZones.size(), 0u);
  double min_x = qmb::kZones[0].x, max_x = qmb::kZones[0].x + qmb::kZones[0].w;
  double min_y = qmb::kZones[0].y, max_y = qmb::kZones[0].y + qmb::kZones[0].h;
  for (const auto &z : qmb::kZones) {
    min_x = std::min(min_x, z.x);
    max_x = std::max(max_x, z.x + z.w);
    min_y = std::min(min_y, z.y);
    max_y = std::max(max_y, z.y + z.h);
  }
  for (const auto &p : qmb::kPoints) {
    EXPECT_GE(p.x, min_x - 1e-9) << p.id;
    EXPECT_LE(p.x, max_x + 1e-9) << p.id;
    EXPECT_GE(p.y, min_y - 1e-9) << p.id;
    EXPECT_LE(p.y, max_y + 1e-9) << p.id;
    if (p.zone == nullptr) continue;
    const qmb::SiteZone *z = findZone(p.zone);
    ASSERT_NE(z, nullptr) << p.id;
    EXPECT_GE(p.x, z->x - 1e-9) << p.id << " 越出区域 " << z->id << " 左边界";
    EXPECT_LE(p.x, z->x + z->w + 1e-9) << p.id << " 越出区域 " << z->id << " 右边界";
    EXPECT_GE(p.y, z->y - 1e-9) << p.id << " 越出区域 " << z->id << " 下边界";
    EXPECT_LE(p.y, z->y + z->h + 1e-9) << p.id << " 越出区域 " << z->id << " 上边界";
  }
}

// 充电任务（语音"回充"、看板充电类任务）必须能解析到一个 charger 点位
TEST(SiteMap, HasChargerPoint)
{
  size_t chargers = 0;
  for (const auto &p : qmb::kPoints) {
    if (p.kind != nullptr && std::string(p.kind) == "charger") ++chargers;
  }
  EXPECT_GT(chargers, 0u);
}

TEST(SiteMap, FindByExactId)
{
  const qmb::SitePoint *p = qmb::findSitePoint("warehouse-racks");
  ASSERT_NE(p, nullptr);
  EXPECT_STREQ(p->name, "仓库-货架区");
  EXPECT_DOUBLE_EQ(p->x, 5.0);
  EXPECT_DOUBLE_EQ(p->y, -3.5);
  EXPECT_STREQ(p->zone, "warehouse");
}

// 看板下发的 target 是点位【名称】（MQTT-INTERFACE.md §4.1），必须能解析
TEST(SiteMap, FindByExactName)
{
  const qmb::SitePoint *p = qmb::findSitePoint("车间B-装配台");
  ASSERT_NE(p, nullptr);
  EXPECT_STREQ(p->id, "b-assembly");

  const qmb::SitePoint *c = qmb::findSitePoint("充电区-1号桩");
  ASSERT_NE(c, nullptr);
  EXPECT_STREQ(c->id, "charger-1");
  ASSERT_NE(c->kind, nullptr);
  EXPECT_STREQ(c->kind, "charger");
}

// 语音/人给的是口语名，走模糊匹配
TEST(SiteMap, FindByPartialName)
{
  const qmb::SitePoint *p = qmb::findSitePoint("货架");
  ASSERT_NE(p, nullptr);
  EXPECT_STREQ(p->id, "warehouse-racks");
}

TEST(SiteMap, FindMissReturnsNull)
{
  EXPECT_EQ(qmb::findSitePoint(""), nullptr);
  EXPECT_EQ(qmb::findSitePoint("不存在的点位"), nullptr);
  EXPECT_EQ(qmb::findSitePoint("zigzag"), nullptr);
}
