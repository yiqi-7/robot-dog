# GitHub About 配置

## Description（仓库描述，350 字符上限）

**推荐版本（English，GitHub 主展示位）：**

```
Quadruped inspection robot on ROS 2 + Gazebo: CHAMP gait, Nav2, MQTT ops dashboard, WebRTC video, voice pipeline (Opus → ASR → LLM). Runs local all-in-one or split with a cloud relay.
```

字符数：183 / 350 ✅

**中文版（若偏好中文描述）：**

```
ROS 2 + Gazebo 四足巡检机器人：CHAMP 步态、Nav2 导航、MQTT 运维看板、WebRTC 视频、语音链路（Opus→ASR→大模型决策）。支持本地全栈与云端中转两种部署。
```

字符数：97 / 350 ✅

---

## Topics（仓库标签，最多 20 个，小写、连字符）

```
ros2
gazebo
quadruped-robot
legged-robot
robotics
mqtt
webrtc
mediamtx
slam
nav2
computer-vision
h264
opus
speech-recognition
asr
llm
python
cpp
typescript
react
```

> 逐个粘贴到 GitHub 仓库页右上角 **About → ⚙️ → Topics**。
> 注意 GitHub 的 topic 会自动小写并只允许 `a-z0-9-`。

---

## 如何设置

### 方式 A：网页（最简单）

1. 打开 https://github.com/yiqi-7/robot-dog
2. 右侧 **About** 区域点齿轮图标 ⚙️
3. **Description** 粘贴上面的描述
4. **Topics** 逐个粘贴标签
5. 可选勾选 **Releases / Packages / Environments** 中需要的
6. 点 **Save changes**

### 方式 B：GitHub CLI（本机未安装 gh）

```bash
# 1. 安装（Windows）
winget install --id GitHub.cli
# 2. 登录
gh auth login
# 3. 一次性设置描述与标签
gh repo edit yiqi-7/robot-dog \
  --description "Quadruped inspection robot on ROS 2 + Gazebo: CHAMP gait, Nav2, MQTT ops dashboard, WebRTC video, voice pipeline (Opus → ASR → LLM). Runs local all-in-one or split with a cloud relay." \
  --add-topic ros2,gazebo,quadruped-robot,legged-robot,robotics,mqtt,webrtc,mediamtx,slam,nav2,computer-vision,h264,opus,speech-recognition,asr,llm,python,cpp,typescript,react
# 4. 验证
gh repo view yiqi-7/robot-dog --json description,repositoryTopics
```

### 方式 C：GitHub REST API（用个人访问令牌）

```bash
# 需要 token 具有 repo 权限
curl -X PATCH https://api.github.com/repos/yiqi-7/robot-dog \
  -H "Authorization: token <YOUR_TOKEN>" \
  -H "Accept: application/vnd.github+json" \
  -d '{"description":"Quadruped inspection robot on ROS 2 + Gazebo: CHAMP gait, Nav2, MQTT ops dashboard, WebRTC video, voice pipeline (Opus → ASR → LLM). Runs local all-in-one or split with a cloud relay."}'

# topics 需单独接口，且要整体覆盖
curl -X PUT https://api.github.com/repos/yiqi-7/robot-dog/topics \
  -H "Authorization: token <YOUR_TOKEN>" \
  -H "Accept: application/vnd.github+json" \
  -d '{"names":["ros2","gazebo","quadruped-robot","legged-robot","robotics","mqtt","webrtc","mediamtx","slam","nav2","computer-vision","h264","opus","speech-recognition","asr","llm","python","cpp","typescript","react"]}'
```
