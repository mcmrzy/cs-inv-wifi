# ESP32 直连 F280039C 的 OTA（无需 ARM、无需 PC、无需 JTAG）

```
      ESP32                          F280039C
   ┌──────────┐   115200 8N1    ┌───────────────────────┐
   │ UART0 TX ├─────────────────►│ GPIO23 (SCI-B RX)     │
   │ UART0 RX │◄─────────────────┤ GPIO22 (SCI-B TX)     │
   └──────────┘                 └───────────────────────┘
```

本目录给出 **ESP32 侧可直接编译的 C 代码**，让 ESP32 自己把新的 App 镜像
通过 IAP 帧协议灌进 DSP，全程不依赖 PC / JTAG / ARM。

| 文件 | 说明 |
|---|---|
| `dsp_ota.h` / `dsp_ota.c` | ESP-IDF 模块，实现完整升级流程（握手→擦除→逐包→EOT）|

---

## 一、三步接入

**① 把两个文件加进你的 ESP-IDF 工程**，并在 `main/CMakeLists.txt` 的 `SRCS` 里加上 `dsp_ota.c`。

**② 拿到"带头镜像"**（PC 侧做一次，产物烧进 ESP32 的 flash 或固件里都行）：

```powershell
# 先用 DSP 工程构建 App（得到 app.out），再打包注入镜像头
py host\pack_f28003x.py app.out app.bin --version 0x00010000
```

> ⚠️ **必须是打包后的 `app.bin`** ✗ —— 直接给 `.out` 或未打包的 `.bin` 会被
> `dsp_ota_flash()` 在握手前就拒掉（它检查入口 `LB` 操作码 + 头部 magic ✓）。

**③ 独占串口后调用**：

```c
#include "dsp_ota.h"

void do_dsp_update(const uint8_t *bin, size_t len)
{
    /* ★ 必须独占！你的 direct_link 任务此刻不能碰这个 UART */
    direct_link_suspend();            /* 或取你的 UART 互斥锁 */
    dsp_ota_set_progress(my_progress); /* 可选：进度条 */
    dsp_ota_result_t r = dsp_ota_flash(UART_NUM_0, bin, len, my_log);
    direct_link_resume();

    if (r == DSP_OTA_OK) {
        ESP_LOGI("ota", "DSP 升级完成，正在运行新 App");
    } else {
        ESP_LOGE("ota", "DSP 升级失败，错误码 %d", (int)r);
    }
}
```

---

## 二、关键：怎么让 DSP 进入 bootloader

DSP 平时跑 App，**只有 bootloader 在监听时才认 IAP 帧**。有两条路：

### 路径 A ★推荐★（可靠，App 侧加 5 行）

让 App 主动"交接"给 bootloader：往一块共享 RAM 字里写魔数，然后长跳转到
`0x080000`。你的 `direct_link` 收到某个自定义命令（比如 `03/04/25` 协议里加
一条"进升级"）时执行：

```c
#define OTA_REQUEST_ADDR  0x0000BFFEul
#define OTA_REQUEST_MAGIC 0x4F544152ul     /* 'OTAR' */

*(volatile uint32_t *)OTA_REQUEST_ADDR = OTA_REQUEST_MAGIC;
((void (*)(void))0x00080000ul)();          /* 跳到 bootloader，不要返回 */
```

> ⚠️ App 的链接脚本**不能使用 `0x0000BFFE`** 这个字（bootloader 把它留作
> NOINIT 共享标志）。App 启动时清 `.bss` 无所谓 —— 魔数是在跳转前一刻才写的 ✓。

之后 ESP32 端：`dsp_ota_flash()` 里的握手会立刻拿到 `"HR"` ✓。

### 路径 B（不改 App，靠上电窗口）

bootloader 在**冷启动后有 3 秒监听窗口**。所以 ESP32 需要在**上电后立刻**
发 `CMD_UPGRADE`：

```c
void app_main(void)
{
    /* 先给一小段时间让 DSP 的 bootloader 起来，然后马上抢窗口 */
    vTaskDelay(pdMS_TO_TICKS(50));
    if (!dsp_ota_probe(UART_NUM_0, 500, 6, NULL)) {
        /* 没抢到：DSP 已经跳进 App 了，转正常业务 */
        direct_link_start();
        return;
    }
    /* 抢到了，直接跑完整升级 */
    dsp_ota_flash(UART_NUM_0, g_new_image, g_new_image_len, NULL);
}
```

- 优点：**App 一个字不用改** ✓
- 代价：**只有上电后 3 秒的机会** ✗；错过了就得等下次上电 ✗
- 如果觉得太紧 ✓：把 bootloader 的 `OTA_WINDOW_MS`（`boot/boot.h`）从 `3000`
  调大 ✓，重编译后用 `tools\dss_ramwriter.js` **只烧扇区 0/1** 更新即可 ✓
  （App 不受影响 ✓）

---

## 三、时序与重试（与 PC 端驱动一致）

| 阶段 | 主机发 | DSP 回 | 超时 | 重试 |
|---|---|---|---|---|
| 握手 | `01 05 42 00 00 00 00 73 9A` (9B) | `48 52` "HR" | 2 s | 5 |
| 擦除 | `01 10 04 38 00 02 04 00 00 00 00 C3 DD` (13B) | 13 字节应答 | 3 s | 5 |
| 擦除完成 | — | `43 52` "CR" | **12 s** | 5 |
| 数据包 | `02 AddrH AddrL <1024B> CrcH CrcL` | `43 52` "CR" | 3 s | 10 |
| 结束 | `02 00 FF 45 4F 54` (6B) | 无应答 | 1 s 落盘 | — |

- 包地址：`Addr = (0x01FE + 0x00FF × 包号) & 0xFFFF`，包号从 0 开始
- CRC：**CRC16-Modbus**（多项式 `0xA001`，初值 `0xFFFF`），覆盖 `02 + AddrH + AddrL + 数据`
- 每包数据 **1024 字节**（= 512 字）
- F280039C 的 App 区（扇区 2~47 = 367 KB）最多 **367 包**

---

## 四、必须知道的几个坑

1. **串口独占** ✗ —— 升级期间 DSP 不说 `03/04/25` 应用协议 ✓，你的 `direct_link`
   会收到一堆"看不懂"的 `"CR"` 字节 ✓。**先停掉它** ✓，升级完再恢复 ✓。
2. **波特率固定 115200 8N1** ✓（bootloader 里写死 ✓，`BRR=0x1F` = 30MHz/(115200×8)−1 ✓）。
3. **bootloader 所在的扇区 0/1 永不被擦** ✓ —— 升级中断电也不会变砖 ✓，
   最多是 App 区不完整 ✓ → bootloader 校验不过 → **停在监听** ✓，重发即可 ✓。
4. **无头 App 也能启动** ✓ —— 如果你的 App 是 JTAG 直接烧的（没有镜像头 ✓），
   bootloader 走"无头兼容"通道放行 ✓。但**经 OTA 写入的镜像一定带头** ✓，
   走严格校验（magic + 长度 + CRC32）✓，更能防"写一半"✓。
5. **镜像头只有前 6 个字参与判定**（`0x082002~0x082007`）✓ —— 第 7、8 个字
   故意不检查 ✓，因为 App 的代码可能就从 `0x082008` 开始 ✓。

---

## 五、自测顺序

```powershell
# PC 侧先确认 bootloader 在线（可选，用 JTAG 探针的串口也行）
py host\ota_f28003x.py COMx --listen 10      # 看有没有 App 的心跳/横幅
```

1. **先只验证握手** ✓：ESP32 里调 `dsp_ota_probe()`，看能否拿到 `"HR"`
   - 拿到 → 链路 ✓、DSP 在 bootloader 里 ✓
   - 拿不到 → 检查 GPIO22/23 交叉接线 ✓、115200 ✓、DSP 是否已跳进 App ✓
2. **再跑全流程** ✓：`dsp_ota_flash()`
3. **最后确认** ✓：升级后串口应打出新 App 的横幅（比如 `APP_V2`）✓，
   或看你的 `direct_link` 是否恢复正常通信 ✓
