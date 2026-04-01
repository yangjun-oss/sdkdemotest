# 标准 GCC 带宽探测流程深度解析

> **GCC**（Google Congestion Control）是 WebRTC 所采用的拥塞控制算法，最初由 Google 在 2012 年提出并在 RFC 8698 / Google RMCAT 草案中持续演进。本文以 WebRTC 的 libwebrtc（Chromium M100+）源码为参考，系统梳理 GCC 带宽探测的完整流程，并对每一步的数学原理进行深入剖析。

---

## 目录

1. [总体架构与数据流](#1-总体架构与数据流)
2. [探测触发机制（Probing）](#2-探测触发机制probing)
3. [数据包到达间隔模型](#3-数据包到达间隔模型)
4. [基于延迟的估计器（Delay-Based Estimator）](#4-基于延迟的估计器delay-based-estimator)
   - 4.1 [Trendline 滤波器](#41-trendline-滤波器)
   - 4.2 [过载检测器（Overuse Detector）](#42-过载检测器overuse-detector)
5. [基于丢包的估计器（Loss-Based Estimator）](#5-基于丢包的估计器loss-based-estimator)
6. [速率控制状态机](#6-速率控制状态机)
7. [AIMD 速率调整](#7-aimd-速率调整)
8. [完整流程时序图](#8-完整流程时序图)
9. [关键参数汇总](#9-关键参数汇总)
10. [各阶段数据示例](#10-各阶段数据示例)
11. [数学原理剖析](#11-数学原理剖析)

---

## 1. 总体架构与数据流

GCC 的带宽估计由**两条并行管道**组成，最终取二者的**最小值**作为发送码率上限：

```
发送端                               接收端
  │                                    │
  │──── RTP 数据包 ─────────────────►  │
  │                                    │ Transport-CC / REMB
  │ ◄──────── RTCP Feedback ───────────│
  │                                    │
  ▼
┌──────────────────────────────────────────────┐
│               GCC 控制器                      │
│                                              │
│  ┌─────────────────┐   ┌──────────────────┐  │
│  │ 基于延迟的估计器 │   │ 基于丢包的估计器  │  │
│  │ (Delay-Based)   │   │  (Loss-Based)    │  │
│  └────────┬────────┘   └────────┬─────────┘  │
│           │                     │            │
│           ▼                     ▼            │
│       delay_bwe             loss_bwe          │
│           │                     │            │
│           └──────── min ────────┘            │
│                      │                       │
│                    target_bwe                │
└──────────────────────┬───────────────────────┘
                       │
                  发送码率控制器
                  (PacingController)
```

### 核心模块说明

| 模块 | 功能 | 源码文件（libwebrtc） |
|------|------|----------------------|
| `BitrateProber` | 生成探测包群，主动探测带宽 | `modules/pacing/bitrate_prober.cc` |
| `TrendlineEstimator` | 基于到达时间斜率估计网络延迟趋势 | `modules/remote_bitrate_estimator/inter_arrival.cc` |
| `OveruseDetector` | 判断当前是否过载/欠载/正常 | `modules/remote_bitrate_estimator/overuse_detector.cc` |
| `AimdRateControl` | AIMD 速率调整 | `modules/remote_bitrate_estimator/aimd_rate_control.cc` |
| `LossBasedBandwidthEstimation` | 基于丢包率估计带宽 | `modules/congestion_controller/goog_cc/loss_based_bwe_v2.cc` |
| `GoogCcNetworkController` | 汇总上层控制逻辑 | `modules/congestion_controller/goog_cc/goog_cc_network_controller.cc` |

---

## 2. 探测触发机制（Probing）

### 2.1 探测时机

GCC 在以下情形触发主动带宽探测：

| 触发条件 | 探测类型 | 说明 |
|----------|----------|------|
| 会话启动 | 初始探测 | 以 2× 和 3× 初始码率各发一个包群 |
| 估计值大幅增长 | 上行探测 | 新估计值 > 1.5× 旧估计值时触发 |
| 网络恢复（ALR 结束） | 恢复探测 | 应用层码率受限（ALR）结束后触发 |
| 周期性 | 定时探测 | 默认每 5s 一次，以维持带宽感知 |

### 2.2 探测包群设计

探测的核心思想是：**在短时间内以目标码率突发发送一批包，通过接收端反馈的接收时间推算实际可用带宽**。

```
发送时序（目标码率 = 2 Mbps，包大小 = 1200 B）：

时间轴(ms):  0   4.8  9.6  14.4 19.2 24.0 28.8 ...
             │    │    │    │    │    │    │
发送包:      P1   P2   P3   P4   P5   P6   P7 ...
             │◄─────────────── Δt ≈ 4.8ms ──────────►│

发包间隔 Δt = 包大小 / 目标码率
           = (1200 × 8 bits) / (2 × 10⁶ bps)
           = 4.8 ms  ← 实际按此间隔发送
```

> **注**：实际 libwebrtc 中探测包群大小（`kMinProbePacketsSent = 5`），持续时间不超过 15ms，以减少对正常流量的影响。

### 2.3 探测码率计算

```
探测估计带宽 = (总发送字节数 × 8) / 接收端实际接收时间窗口

            send_size_bits
B_probe = ─────────────────
          receive_interval
```

**示例**：

| 参数 | 值 |
|------|----|
| 探测包群总大小 | 15 × 1200 B = 18000 B |
| 发送持续时间 | 72 ms |
| 接收端接收时间窗口 | 85 ms（有排队延迟） |
| 探测估计带宽 | 18000 × 8 / 0.085 ≈ **1.694 Mbps** |

---

## 3. 数据包到达间隔模型

### 3.1 基本定义

设发送端发送两个相邻数据包：

- 发送时刻：$t_1$，$t_2$，发送间隔 $s_i = t_2 - t_1$  
- 接收时刻：$r_1$，$r_2$，接收间隔 $d_i = r_2 - r_1$  

**单向传输延迟变化量**（Inter-arrival time difference，即网络延迟梯度）：

$$\delta_i = d_i - s_i = (r_2 - r_1) - (t_2 - t_1)$$

当网络发生拥塞时，队列积累导致 $\delta_i > 0$（延迟增大）；网络空闲时 $\delta_i < 0$（延迟减小）。

### 3.2 系统模型

GCC 将网络延迟建模为：

$$\delta_i = m_i + v_i$$

其中：
- $m_i$：网络**队列延迟**（queue delay），是我们想要估计的状态量
- $v_i$：**测量噪声**，假设为零均值高斯噪声 $v_i \sim \mathcal{N}(0, \sigma_v^2)$

队列延迟的状态转移模型（随机游走）：

$$m_i = m_{i-1} + u_{i-1}, \quad u_i \sim \mathcal{N}(0, \sigma_u^2)$$

---

## 4. 基于延迟的估计器（Delay-Based Estimator）

### 4.1 Trendline 滤波器

Trendline 滤波器取代了早期的 Kalman 滤波器（自 M56 起），用线性回归对一段时间窗口内的延迟梯度进行趋势拟合。

#### 4.1.1 累积延迟梯度

将到达时间差累积为时间序列：

$$S_i = \sum_{k=1}^{i} \delta_k$$

即累积的延迟变化量（smoothed_delay），libwebrtc 使用指数平滑：

$$\hat{S}_i = (1 - \alpha) \cdot \hat{S}_{i-1} + \alpha \cdot \delta_i, \quad \alpha = 0.9$$

#### 4.1.2 线性回归（最小二乘法）

在长度为 $N$（默认 $N = 20$）的滑动窗口内，以接收时刻 $t_i$ 为自变量，$\hat{S}_i$ 为因变量，做最小二乘线性拟合：

$$\hat{S}_i \approx \beta \cdot t_i + c$$

斜率 $\beta$（trendline slope）的计算：

$$\beta = \frac{\sum_{i=1}^{N}(t_i - \bar{t})(\hat{S}_i - \bar{S})}{\sum_{i=1}^{N}(t_i - \bar{t})^2}$$

- $\bar{t} = \frac{1}{N}\sum t_i$，$\bar{S} = \frac{1}{N}\sum \hat{S}_i$

**物理含义**：
- $\beta > 0$：队列延迟持续增大 → 网络趋向拥塞
- $\beta \approx 0$：队列延迟平稳 → 网络正常
- $\beta < 0$：队列延迟减小 → 网络有空余带宽

#### 4.1.3 归一化斜率

为了与阈值比较，将斜率归一化：

$$k = \frac{\beta}{\hat{S}_{\max}}$$

其中 $\hat{S}_{\max}$ 是当前窗口内的最大累积延迟值。

---

```
Trendline 斜率 示意图：

累积延迟
  (ms)
 10 |         ·  ·
    |      ·       ·
  8 |   ·              ·    ← 斜率 β > 0（过载）
    | ·                   ·
  4 |·
    |
  0 +─────────────────────── 时间(ms)
    0    50   100   150  200

  vs.

  4 | · · · ·
    |         · · · ·
  2 |                 · · · ·  ← 斜率 β ≈ 0（正常）
    |
  0 +─────────────────────── 时间(ms)
```

---

### 4.2 过载检测器（Overuse Detector）

过载检测器根据 trendline 斜率 $k$ 与动态阈值 $\gamma$ 比较，输出三种信号：

| 信号 | 英文 | 判定条件 | 含义 |
|------|------|----------|------|
| 过载 | Overuse | $k > \gamma$ 且持续超过 $T_{ou}$（默认 10ms） | 网络已拥塞，需降速 |
| 正常 | Normal | $-\gamma \leq k \leq \gamma$ | 带宽充足，可保持或小幅增速 |
| 欠载 | Underuse | $k < -\gamma$ | 带宽有剩余，可增速 |

#### 4.2.1 动态阈值自适应（Adaptive Threshold）

固定阈值易受突发和背景流量干扰，GCC 引入自适应阈值：

$$\frac{d\gamma}{dt} = k_{\gamma} \cdot (|k| - \gamma)$$

离散化为：

$$\gamma_{i+1} = \gamma_i + \Delta t \cdot k_{\gamma} \cdot (|k_i| - \gamma_i)$$

其中：
- $k_{\gamma}$：自适应速率（默认 0.01）
- $\gamma$ 的取值范围：$[\gamma_{\min}, \gamma_{\max}] = [6\text{ ms}, 600\text{ ms}]$

**直觉**：当网络延迟梯度大幅超过阈值时，阈值上升（避免虚警）；当梯度持续低于阈值时，阈值下降（提高灵敏度）。

---

## 5. 基于丢包的估计器（Loss-Based Estimator）

### 5.1 丢包率计算

通过 RTCP RR（接收报告）或 Transport-CC 反馈报文统计：

$$p_{loss} = \frac{\text{期望接收包数} - \text{实际接收包数}}{\text{期望接收包数}}$$

### 5.2 码率调整策略

| 丢包率 $p_{loss}$ | 操作 | 数学表达 |
|-------------------|------|----------|
| $p_{loss} < 2\%$ | 增加码率（10% / 上报周期） | $B_{loss} = 1.08 \times B_{loss,\text{prev}}$ |
| $2\% \leq p_{loss} \leq 10\%$ | 保持码率 | $B_{loss} = B_{loss,\text{prev}}$ |
| $p_{loss} > 10\%$ | 降低码率 | $B_{loss} = (1 - 0.5 \times p_{loss}) \times B_{loss,\text{prev}}$ |

**示例计算**（$p_{loss} = 15\%$）：

```
B_loss = (1 - 0.5 × 0.15) × B_prev
       = (1 - 0.075) × B_prev
       = 0.925 × B_prev
```

若当前码率为 2 Mbps，则降为 **1.85 Mbps**。

### 5.3 Loss-Based v2（LBBEv2）

libwebrtc M100+ 引入了基于贝叶斯推断的 LBBEv2，通过后验概率建模区分随机丢包（无线信道）和拥塞丢包：

$$P(\text{congestion} | p_{loss}) = \frac{P(p_{loss} | \text{congestion}) \cdot P(\text{congestion})}{P(p_{loss})}$$

此模型能更准确地区分随机丢包与拥塞丢包，避免在无线网络中无谓降速。

---

## 6. 速率控制状态机

GCC 的延迟控制器维护一个三状态机：

```
           ┌─────────────────────────────┐
           │                             │
    Normal ▼     Overuse                 │ Underuse
  ┌──────────────┐  ──►  ┌───────────┐  │
  │              │       │           │  │
  │    Increase  │       │  Decrease │──┘
  │   (增速状态)  │  ◄──  │  (降速状态)│
  │              │ Normal│           │
  └──────┬───────┘       └─────┬─────┘
         │                     │
         │  Overuse             │
         └─────────────────────┘
         
┌─────────────────────────────────────────────────────┐
│ 状态说明：                                            │
│  Increase（增速）：Overuse 信号未触发，持续缓慢增速    │
│  Decrease（降速）：收到 Overuse 信号，快速降速         │
│  Hold（保持）:  介于增速和降速之间的短暂稳定期         │
└─────────────────────────────────────────────────────┘
```

### 状态转移规则

| 当前状态 | 过载检测器输出 | 下一状态 |
|----------|---------------|----------|
| Increase | Normal | Increase |
| Increase | Overuse | Decrease |
| Increase | Underuse | Increase |
| Decrease | Normal | Hold |
| Decrease | Overuse | Decrease |
| Hold | Normal | Increase |
| Hold | Overuse | Decrease |
| Hold | Underuse | Increase |

---

## 7. AIMD 速率调整

### 7.1 加性增（Additive Increase，AI）

在 **Increase** 状态下，每次 RTCP 反馈后码率增加：

$$B_{new} = B_{current} + \alpha_{AI}$$

其中自适应增量：

$$\alpha_{AI} = \min\left(\eta \cdot B_{current}, \frac{\text{RESPONSE\_TIME} \cdot B_{current}}{B_{current} + \Delta B}\right)$$

简化后的近似：

$$B_{new} \approx B_{current} \times 1.08^{\frac{\Delta t}{1\text{s}}}$$

即每秒增加约 **8%**。

### 7.2 乘性减（Multiplicative Decrease，MD）

在 **Decrease** 状态下（收到 Overuse 信号）：

$$B_{new} = \eta_{MD} \times B_{current}, \quad \eta_{MD} = 0.85$$

即每次降为当前码率的 **85%**。

### 7.3 AIMD 收敛性证明（简要）

设目标带宽为 $C$，当前估计为 $B$：

**增速阶段**：$B_{n+1} = B_n + \alpha$（线性增长逼近 $C$）

**过载触发**：当 $B_n > C$ 时触发 Overuse，$B_{n+1} = 0.85 \times B_n$

**收敛性**：AIMD 控制律在多流竞争时具有**公平性**（Fairness）和**效率**（Efficiency），可以证明所有流最终收敛到公平分享带宽，证明基于势函数（Lyapunov 函数）：

$$V(B) = \sum_i \frac{(B_i - C/N)^2}{2}$$

---

## 8. 完整流程时序图

```
发送端                    网络                   接收端
  │                        │                       │
  │──── 初始探测包群(2x) ──►│──────────────────────►│
  │──── 初始探测包群(3x) ──►│──────────────────────►│
  │                        │                       │
  │        ◄──── Transport-CC RTCP Feedback ────────│
  │                        │                       │
  │  ┌──────────────────────────────────────────┐   │
  │  │ 1. 解析 Transport-CC，重建发送/接收时序    │   │
  │  │ 2. 计算 δ_i（到达间隔差）                  │   │
  │  │ 3. 更新 Trendline 滤波器，计算斜率 β       │   │
  │  │ 4. 过载检测器：β vs γ → {Overuse/Normal}  │   │
  │  │ 5. 更新状态机 → {Increase/Hold/Decrease}  │   │
  │  │ 6. AIMD 调整 delay_bwe                    │   │
  │  │ 7. 解析 RR/RTCP，计算丢包率 p_loss        │   │
  │  │ 8. 调整 loss_bwe                          │   │
  │  │ 9. target_bwe = min(delay_bwe, loss_bwe)  │   │
  │  │ 10. 更新 PacingController 发送速率         │   │
  │  └──────────────────────────────────────────┘   │
  │                        │                       │
  │──── 正常 RTP 数据 ─────►│──────────────────────►│
  │                        │                       │
  │        ◄──── RTCP Feedback (周期 ~100ms) ────────│
  │  [重复步骤 1-10...]     │                       │
  │                        │                       │
  │  [条件触发：上行探测]    │                       │
  │──── 探测包群(1.5x BWE)─►│──────────────────────►│
  │                        │                       │
  │        ◄──── Transport-CC Feedback ─────────────│
  │  [更新带宽估计...]       │                       │
```

---

## 9. 关键参数汇总

| 参数名 | 默认值 | 含义 |
|--------|--------|------|
| `kTrendlineWindowSize` | 20 个包 | Trendline 线性回归窗口大小 |
| `kTrendlineSmoothingCoeff` | 0.9 | 延迟梯度指数平滑系数 α |
| `kOverUseTimeThreshold` | 10 ms | 过载持续时间阈值 |
| `kInitialThreshold` | 12.5 ms | 过载检测初始阈值 γ₀ |
| `kThresholdGain` | 0.01 | 自适应阈值调整速率 k_γ |
| `kThresholdMinValue` | 6 ms | 阈值下界 γ_min |
| `kThresholdMaxValue` | 600 ms | 阈值上界 γ_max |
| `kBeta` | 0.85 | 乘性减系数 η_MD |
| `kDefaultRttMs` | 200 ms | 默认 RTT（无测量时） |
| `kProbeMinProbes` | 5 | 最小探测包数 |
| `kMinProbePacketsSent` | 5 | 最小探测包群大小 |
| `kMaxProbeDelay` | 3 s | 最大探测延迟 |
| Loss 无动作区间 | [2%, 10%] | 丢包率不触发调整的区间 |
| AI 增量 | ~8%/s | 加性增速率 |

---

## 10. 各阶段数据示例

### 10.1 典型会话带宽估计演变

```
码率 (Mbps)
  5 |                            ·····················
    |                       ····
    |                  ·····
  3 |             ·····             ▼ Overuse 触发
    |        ·····          ·  ···
    |   ·····              ·· ·     ···············
  1 |···                              ←稳定在约3Mbps
    |
  0 +───────────────────────────────────────────── 时间(s)
    0    5    10   15   20   25   30   35   40   45
    
    ← 初始探测→←──── 加性增速 ────→← 降速 →← 稳态 ──→
```

### 10.2 过载检测阈值自适应过程

```
阈值 γ (ms)
 25 |         ▲
    |        / \      ← 短暂突发使阈值升高
 20 |       /   \
    |      /     \___
 15 |     /           ───────────────   ← 稳定后回落
    |    /
 12.5──── 初始阈值
    |
  6 |                                  ← 最小阈值
    +───────────────────────────────── 时间(s)
    0    5    10   15   20   25   30
```

### 10.3 Trendline 斜率与状态机关系

| 时刻 (s) | β (ms/ms) | γ (ms) | 过载信号 | 状态机 | 目标码率 (Mbps) |
|---------|-----------|--------|----------|--------|----------------|
| 0 | 0.00 | 12.5 | Normal | Increase | 0.5 |
| 5 | 0.02 | 12.5 | Normal | Increase | 1.2 |
| 10 | 0.05 | 13.1 | Normal | Increase | 2.1 |
| 15 | 0.18 | 14.2 | **Overuse** | Decrease | 1.79 |
| 17 | 0.08 | 13.8 | Normal | Hold | 1.79 |
| 20 | -0.01 | 13.5 | Normal | Increase | 2.0 |
| 30 | 0.03 | 12.8 | Normal | Increase | 2.8 |
| 40 | 0.01 | 12.6 | Normal | Increase | 3.1 |

---

## 11. 数学原理剖析

### 11.1 早期 Kalman 滤波器（历史参考）

在 RFC 草案和 M56 之前，GCC 使用 Kalman 滤波器估计队列延迟梯度 $m_i$。

#### 状态空间模型

$$\text{状态转移：} m_i = m_{i-1} + u_i, \quad u_i \sim \mathcal{N}(0, q)$$
$$\text{观测方程：} \delta_i = m_i + v_i, \quad v_i \sim \mathcal{N}(0, r_i)$$

#### Kalman 滤波递推

**预测步骤（Prediction）**：

$$\hat{m}_{i|i-1} = \hat{m}_{i-1|i-1}$$
$$P_{i|i-1} = P_{i-1|i-1} + q$$

**更新步骤（Update）**：

$$K_i = \frac{P_{i|i-1}}{P_{i|i-1} + r_i}$$（卡尔曼增益）

$$\hat{m}_{i|i} = \hat{m}_{i|i-1} + K_i(\delta_i - \hat{m}_{i|i-1})$$（状态更新）

$$P_{i|i} = (1 - K_i) \cdot P_{i|i-1}$$（协方差更新）

其中观测噪声方差 $r_i$ 自适应：

$$r_i = \alpha \cdot r_{i-1} + (1-\alpha) \cdot (\delta_i - \hat{m}_{i|i-1})^2$$

#### 物理直觉

- $K_i \to 1$：完全信任新观测，快速响应（噪声小）
- $K_i \to 0$：完全信任预测，抗噪能力强（测量噪声大）

### 11.2 Trendline 滤波器的线性回归推导

对长度 $N$ 的窗口数据 $\{(t_i, \hat{S}_i)\}_{i=1}^{N}$，最小化残差平方和：

$$\min_{\beta, c} \sum_{i=1}^{N} (\hat{S}_i - \beta t_i - c)^2$$

对 $\beta$ 和 $c$ 分别求导并令其为零：

$$\frac{\partial}{\partial \beta}: \sum_{i=1}^{N} t_i(\hat{S}_i - \beta t_i - c) = 0$$
$$\frac{\partial}{\partial c}: \sum_{i=1}^{N} (\hat{S}_i - \beta t_i - c) = 0$$

解方程组得：

$$\beta = \frac{N\sum t_i \hat{S}_i - \sum t_i \sum \hat{S}_i}{N\sum t_i^2 - (\sum t_i)^2}$$

等价于：

$$\beta = \frac{\sum(t_i - \bar{t})(\hat{S}_i - \bar{S})}{\sum(t_i - \bar{t})^2} = \frac{S_{t\hat{S}}}{S_{tt}}$$

**在线/增量更新**（libwebrtc 实现）：

```
每收到新数据点 (t_new, S_new):
  accumulated_delay += S_new - smoothed_delay
  smoothed_delay = (1-alpha) * smoothed_delay + alpha * S_new
  
  // Welford 单步更新
  num_of_deltas_++
  E_x += (t_new - E_x) / num_of_deltas_
  E_y += (S_new - E_y) / num_of_deltas_
  
  // 协方差和方差增量更新
  cov_xy += (t_new - E_x) * (S_new - E_y)
  var_x  += (t_new - E_x)^2
  
  slope = cov_xy / max(var_x, 1e-7)
```

### 11.3 自适应阈值的动态系统分析

自适应阈值 ODE（常微分方程）：

$$\dot{\gamma} = k_\gamma \cdot (|k| - \gamma)$$

这是一阶线性 ODE，稳态解为 $\gamma^* = |k|$，即阈值最终追踪延迟梯度幅值。

瞬态响应（以 $|k|$ 为阶跃输入）：

$$\gamma(t) = |k| + (\gamma_0 - |k|) e^{-k_\gamma t}$$

**时间常数** $\tau = 1/k_\gamma = 100$ s（$k_\gamma = 0.01$），意味着阈值调整非常缓慢，具有良好的抗突发噪声能力。

### 11.4 AIMD 的公平性证明

设 $N$ 个流竞争带宽 $C$，第 $k$ 个流的码率为 $B_k$。

**效率条件**：$\sum B_k = C$（充分利用带宽）

**公平条件**（Jain Fairness Index）：

$$J = \frac{(\sum B_k)^2}{N \sum B_k^2}$$

对 AIMD，可以证明：
1. **加性增**保持向量 $(B_1, B_2, \ldots, B_N)$ 平行于公平线方向移动
2. **乘性减**将向量拉向原点，恰好沿公平线收缩

```
B_2
 ↑              ← 公平线 B_1 = B_2
 │      /
 │     /  乘性减后仍在公平线附近
 │    / ↗
 │   / AI 增速（平行于公平线）
 │  /
 │ /
 └──────────────→ B_1
```

这两个操作的组合使所有流收敛到公平点 $B_k = C/N$，具体数学证明依赖于如下势函数收缩性：

$$\|B^{(n+1)} - B^*\| < \|B^{(n)} - B^*\|$$

---

## 参考文献

1. Holmer S, Lundin H, Carlucci G, et al. *A Google Congestion Control Algorithm for Real-Time Communication*. IETF RFC Draft, 2015.
2. Carlucci G, De Cicco L, Holmer S, et al. *Analysis and Design of the Google Congestion Control for Web Real-Time Communication (WebRTC)*. ACM MMSys, 2016.
3. Zhu X, Pan R, Watson M, et al. *Congestion Control for RMCAT Application Profile (CCRMCAT)*. RFC 8836, 2021.
4. De Cicco L, Mascolo S. *An Experimental Investigation of the Google Congestion Control for Real-Time Flows*. Packet Video Workshop, 2013.
5. libwebrtc 源码：`modules/congestion_controller/goog_cc/` (Chromium repository)
6. Welford B P. *Note on a method for calculating corrected sums of squares and products*. Technometrics, 1962.
