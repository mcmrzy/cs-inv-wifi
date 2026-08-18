# CS-L10-6K2 逆变器认领模拟平台（内测演示）

> 全栈（Web 前端 + Node.js 后端），最终编译为 Docker 容器。
> 核心功能：中国行政区划地图 → 地级市圆点（蓝=未认领 / 绿=已认领）→ 认领即获得该逆变器的 SN / PIN / 二维码 → 进入"别墅电站场景页"（屋顶光伏 + 逆变器 + 电池 + 负载），发电数据按城市实时天气/太阳模拟，全部经 MQTT 上传，并支持 MQTT 下发控制。
>
> 设备额定：**CS-L10-6K2 = 6.2 kW**。

---

## 1. 部署形态

```
┌────────────────────────── Docker 容器 (simulator_platform) ──────────────────────────┐
│                                                                                      │
│   Web (Express 静态)   ──────► 浏览器：中国地图 + 认领 + 别墅场景页                      │
│        │                                                                             │
│   REST API (Express)  /api/claims  /api/devices/:sn/telemetry  /api/cities            │
│        │                                                                             │
│   Simulation Engine  每被认领设备独立实例，按分钟步进，天气/太阳驱动                     │
│        │                      │                                                      │
│        └───► MQTT client (每设备 clientID=SN，唯一)                                   │
│                   topic: cs_inv/{sn}/heartbeat|info|config|cmd|cmd/response|status    │
│                   broker: mqtt://mqtt.jiuxiaoyw.online:8883  (mqtts)                   │
│                   envelope: {"t":<unix秒>,"v":2,"data":{...}}                          │
│                                                                                      │
│   Store  认领关系 + SN/PIN + 设备配置 + 遥测快照  (JSON 文件，可换 SQLite)              │
└──────────────────────────────────────────────────────────────────────────────────────┘
```

- 单个容器内：Express 同时提供静态前端 + REST API；MQTT 客户端按"被认领设备"各建一个（每台一个 clientID）。
- 认领关系、SN/PIN、设备配置、最近遥测快照由后端持久化（默认 JSON 文件，可平滑换 SQLite/Postgres）。
- 未认领城市只显示状态色；**只有被认领的设备才启动模拟引擎 + MQTT 上云**（按需，省 CPU/MQTT 连接）。

---

## 2. 目录结构

```
simulator_platform/
├─ README.md                  # 本文档
├─ docker-compose.yml
├─ Dockerfile
├─ .env.example               # MQTT 凭据 / 天气 key 占位
├─ tools/
│  └─ fetch_cities.py         # 从阿里 DataV GeoAtlas 拉取全国地级市表 → geo-cities.json
├─ server/                    # 后端 (Node.js)
│  ├─ package.json
│  └─ src/
│     ├─ index.js             # HTTP 入口 (静态 + REST)
│     ├─ mqtt.js              # 每设备 MQTT 客户端；发布 heartbeats、订阅 cmd、回报
│     ├─ simulation.js        # 模拟引擎调度（每分钟步进被认领设备）
│     ├─ store.js             # 认领/设备/快照持久化
│     ├─ device-factory.js    # SN 生成 + PIN 计算（对齐固件 HMAC） + 二维码数据
│     ├─ geo.js               # 从 geo-cities.json 加载全国 337 城市 + 气候基线
│     ├─ geo-cities.json      # 337 个地级市/直辖市坐标（由 tools/fetch_cities.py 生成）
│     ├─ weather-provider.js  # 真实天气（Open-Meteo，免费无 key；失败回退内置气候）
│     ├─ routes/
│     │  ├─ claims.js         # POST /api/claims 认领
│     │  ├─ cities.js         # GET  /api/cities 圆点状态
│     │  └─ devices.js        # GET  /api/devices/:sn/telemetry
│     └─ algorithms/          # 模拟算法（见 §5）
│        ├─ weather.js        # 城市天气/太阳（晴雨/云量/温度/湿度 → 辐照）
│        ├─ pv.js             # 光伏发电功率
│        ├─ battery.js        # 电池 SOC / 充放电
│        ├─ load.js           # 家庭用电曲线
│        └─ grid.js           # 并网/离网能量流与系统状态
└─ web/                      # 前端 (纯 HTML/CSS/JS，无构建)
   ├─ index.html             # 地图页
   ├─ scenario.html          # 别墅电站场景页
   ├─ css/styles.css
   ├─ vendor/qrcode-generator.js   # 标准库二维码（npm 拷贝，MIT）
   ├─ js/map.js              # 行政区划地图（真实省界 GeoJSON）+ 城市圆点
   ├─ js/app.js              # 认领交互 / API 调用
   ├─ js/scenario.js         # 场景页实时数据渲染
   ├─ js/qrcode.js           # QRCode.draw 适配层（底层 qrcode-generator）
   └─ geo/china-provinces.geojson  # 中国省界（阿里 DataV，已离线，35 省）
```

---

## 3. 数据模型与 MQTT 对齐

### 3.1 设备（Device）
| 字段 | 说明 |
|------|------|
| `sn` | 设备序列号，格式 `L10` + 12 位十六进制（与固件 `SN_DEV_PREFIX="L10"` 一致） |
| `pin` | 6 位 PIN，`HMAC_SHA256(PRODUCT_SECRET="CS_INV_L10_2026_SECRET", sn)字节[0..2] % 1000000`（与固件 `ble_ct_pin.c` 完全一致，任何设备端都能算出同 PIN） |
| `city` | 关联地级市（认领时绑定） |
| `claimed` | 是否被认领 |
| `claimed_by` | 认领者标识（免登录，用浏览器生成 id 或随机匿名） |
| `rated_w` | 6200（6.2 kW） |
| `qrcode` | 认领信息二维码（SN/城市/演示链接） |

> **clientID 唯一性**：MQTT 每台设备一个连接，`client_id = sn`。不同城市不同 SN → 天然唯一，互不抢连接（对齐固件 `mqtt_get_client_id()` 返回 SN 的做法）。

### 3.2 认领流程（免登录）
```
浏览器点圆点 → GET /api/cities/:code（是否已认领、若已认领返回 SN 概览）
   ├─ 未认领 → 弹出认领卡 → POST /api/claims {city_code, browser_id}
   │          → 后端生成 SN+PIN+qrcode，标记 claimed，启动该设备模拟+Mqtt
   │          → 前端展示 SN/PIN/二维码 + 进入「别墅场景页」
   └─ 已认领 → 直接展示 SN/PIN/二维码 + 进入场景页
```

### 3.3 MQTT 协议（对齐固件，见 `esp32c3_l10_idf`）
- Broker：`mqtt.jiuxiaoyw.online:8883`（**mqtts/TLS**，跳过证书校验与固件一致）
- 用户名/密码：对齐固件 `CONFIG_CS_INV_MQTT_USERNAME/PASSWORD`（`CSKJ-INV-DEVICE-6K2`），放入 `.env`
- topic 前缀：`cs_inv/{sn}/...`
- 信封：`{"t":<unix秒>,"v":2,"data":<上报体>}`
- 上报：`heartbeat`（运行参数，对齐 `cs_protocol_defs.h` 的 57 值结构）、`info`、`config`、`status`/LWT（`{online:false}` QoS1 retain）、`cmd/response`
- 下行：订阅 `cs_inv/{sn}/cmd`，用 `cmd_handler` 的命令名（`set_charge_limit` / `output_priority` 等），回报走 `cmd/response`

> **兼容性**：模拟器复用固件同一套 topic/信封/命令表/凭据。内测时它在后台就是一台真实上云的 L10 设备，前端脚本/后端统计无需区分真机与模拟。

---

## 4. REST API（后端）

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/cities` | 全部地级市 → `{code,name,lat,lng,claimed,sn}` 供地图画点 |
| GET | `/api/cities/:code` | 单市状态 + 已认领时 SN 概览 |
| POST | `/api/claims` | `{city_code, browser_id}` → 免登录认领，返回 `{sn,pin,qrcode}` |
| GET | `/api/devices/:sn` | 设备信息（SN/PIN/rated/qrcode） |
| GET | `/api/devices/:sn/telemetry` | 最近快照（场景页轮询） |
| POST | `/api/devices/:sn/control` | （可选）本地控制转发到 MQTT 下发 |
| GET | `/api/meta/cities-geo` | 城市 GeoJSON 归档（如不随静态打包） |

---

## 5. 模拟算法的设计（backend/algorithms）

四个算法模块 + 一个调度器，全部输入明确、可单测，按"城市 + 当前时刻 + 天气"推导功率。

### 5.1 weather.js — 天气/太阳（驱动源）
- **双模式**（`weather-provider.js`）：
  - **真实天气**：默认调用 `open-meteo`（免费、无需 key）按城市经纬度拉取实时 `cloud_cover / temperature_2m / relative_humidity_2m`，缓存 1 小时（不扣 API 额度、不每分钟打接口）；有数据时用真实云量/温度/湿度，辐照仍由太阳几何推算。
  - **内置回退**：无网络 / `WEATHER_DISABLE=1` / 未拉到数据时，用城市气候基线的马尔可夫云量模型。
- 输入：城市经纬度、本地时刻、日面赤纬/时角投影（太阳高度角）、云量。
- 输出：`{cloud_cover, temp_c, humidity, irradiance_wm2, fromReal}`。
- 算法：由太阳高度角算出理论晴空辐照（AM1.5 简化 + 大气光学质量），用云量系数折减。

### 5.2 pv.js — 光伏发电
- 输入：`irradiance`, 组件标称功率（≈ 屋顶阵列 1.3×6.2kW）、组件温度（`NOCT` 折算）、倾角/朝向。
- 输出：`pv_power_w`（并随温度损失、逆变器限流 6.2kW clamp）。
- 方程：`pv_power = eff_irradiance × (STC功率) × (1 - 0.004×(Tcell-25)) × 逆变器效率曲线[≤6200W]`。

### 5.3 load.js — 家用负载
- 输入：本地时刻、星期、城市/季节、（可选随机波动）。
- 输出：`load_power_w` 曲线（早高峰、晚高峰，峰谷 0.2–8kW）。
- 方程：分段/和函数叠加 + ±10% 噪声。

### 5.4 battery.js — 电池储能
- 输入：`pv_power`, `load_power`, 电池额定（如 48V/200Ah=9.6kWh）、SOC 上下限、充放电效率、当前 SOC。
- 输出：`battery {soc, charge_power, discharge_power, voltage}`，时间积分更新 SOC。
- 策略：光伏富余先充电池，不足时电池放电兜底；过充/过放保护。

### 5.5 grid.js — 并网/离网能量流与系统状态
- 综合 5.2–5.4，产生产出：`sys_status`（运行模式）、`SysStatus` 位域、`output`（AC 输出）、`energy` 日/累计、`warning/fault`、`grid` 买/卖电。
- 输出即 `heartbeat` 上报体（对齐 `telemetry.c` 的 57 值数组结构）。

### 调度器 simulation.js
- 每设备一个实例，`setInterval` 每分钟 `step()` 一次。
- step = 读当前城市时刻 + weather → pv → load → battery → grid → 生成快照：
  - 写入 `store`（本地留存）；
  - 经 MQTT 发布 `heartbeat`（QoS1，180s 节拍 + 突变即时）；
  - 触发场景页 `GET /api/devices/:sn/telemetry` 的实时推送（SSE 或轮询）。

---

## 6. 前端

### 6.1 地图页 `index.html`
- 纯 SVG 绘制离线省界（`china-provinces.geojson`）+ 地级市圆点。
- 圆点状态：**蓝=未认领，绿=已认领**（数据源 `GET /api/cities`）。
- 点击圆点 → 认领弹层（SN / PIN / 二维码预览）→「进入电站场景」按钮。
- 认领状态变化后前端本地缓存 + 后端返回统一，刷新不丢（后端留存）。

### 6.2 别墅场景页 `scenario.html`
- 静态示意图：别墅屋顶光伏板（向阳）、逆变器、电池、负载（家电/灯/空调）。
- 实时数据覆盖：光伏发电功率、逆变器输出、电池 SOC/充放、负载功率、日发电量/消费、当前模式。
- 通过 `GET /api/devices/:sn/telemetry` 轮询（或 SSE）刷新，数值跳动+箭头动画表现能量流向。

---

## 7. 部署 / 运行

```bash
# 本地开发
cd simulator_platform/server && npm install && npm start

# Docker
cp .env.example .env    # 填入真实的 MQTT 凭据/天气 key
docker compose up --build
```
- 端口：`8080`（Web + API）。
- 环境变量：见 `.env.example`（`MQTT_HOST/PORT/USER/PASS`、`WEATHER_API_KEY`(可选)、`PORT`、`DATA_DIR`）。
- 数据持久化：`server/data/`（JSON 文件），挂载 volume 保留。

---

## 8. 内测演示流程

1. 部署容器，打开前端 → 全国地图，全部蓝色。
2. 内测用户点击自己所在城市地级市 → 认领 → 得到该设备的 SN/PIN/二维码，进入别墅场景页。
3. 场景页按该城市实时天气/太阳模拟发电，数据实时跳动，并通过 MQTT 以 `cs_inv/{sn}/heartbeat` 上传到 `mqtt.jiuxiaoyw.online`。
4. 认领过的城市变绿，再次点击可直接看到该设备并进入场景。
5. 多次刷新/重启容器，认领关系与设备数据由后端留存、不丢。

---

## 9. 已落地增强与后续可扩展

- ✅ 真实中国地图：`web/geo/china-provinces.geojson`（阿里 DataV，35 省，离线）+ `server/src/geo-cities.json`（`tools/fetch_cities.py` 拉取，337 个地级市/直辖市）。
- ✅ 标准库二维码：`web/vendor/qrcode-generator.js`（npm 拷贝 MIT），`js/qrcode.js` 做适配，替换了手写编码器，可被手机扫描。
- ✅ 真实天气：`weather-provider.js`（Open-Meteo 免费无 key，缓存 1h），失败自动回退内置气候（`WEATHER_DISABLE=1` 可关）。

后续可扩展：
- 换用 SQLite/Postgres 持久化 + 认领审计。
- 认领引入 PIN 校验 / 账号体系。
- 接入后台看板：按城市统计认领设备数、发电量、在线状态（消费模拟器上报的 MQTT 数据）。
- MQTT 下行真实控制（`cmd` 下发到虚拟设备→场景页联动开/关机、调参）。
- weather-provider 增加和风 / OpenWeather 分支（填 `WEATHER_API_KEY`）。
