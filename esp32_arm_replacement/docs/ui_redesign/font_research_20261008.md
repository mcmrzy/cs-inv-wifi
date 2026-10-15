# 小屏幕中文字体选型调研（2026-10-08）

## 0. 结论摘要

- **主推组合：Ark Pixel 16px proportional Latin（OFL-1.1）+ GNU Unifont 16×16 CJK，16px 单元格** —— 也就是当前 0.4.1 已经采用的组合。调研没有找到"同样可自由分发、又明显更好"的 16px 简体中文点阵字体。
- **关键实测发现**：本 UI 用到的 313 个汉字里，**文泉驿点阵宋体 16×16 与 GNU Unifont 16×16 有 305 个字形逐字节完全相同**（只有 8 个不同）。Unifont 的 CJK 字形大量来自文泉驿，所以"换成文泉驿点阵宋"在 16px 下几乎没有视觉变化，却要把许可从 OFL 换成 GPL。
- **数字**：保留现有手工 12×24 段式大数字（可微调风格），不建议换成七段数码管风格字体。理由见第 5 节。
- **备选**：Fusion Pixel 12px proportional（OFL-1.1）全线 12px，更紧凑，但在 2.8" 屏上汉字偏小。
- **不能随固件分发**：Zpix 最像素（付费商业授权）、丁卯点阵体（付费商业授权）、凤凰点阵体 VonwaonBitmap（上游字模版权存疑）、Windows 中易宋体 12×12/16×16 点阵（专有）。
- **未做真机验证**：本笔记全部结论来自公开资料 + 主机端渲染，未在真实屏幕上确认。

## 1. 研究问题

屏幕：2.8" 240×320 TN 模块（ST7789P3，RGB565，4 线 SPI），用作逆变器/BMS 界面；文字以 1-bit（无抗锯齿）自绘位图字库渲染，可用字号为 12/14/16 px 单元格 + 手工 12×24 仪表数字；界面语言为英文 + 简体中文，多为短标签；观察角度略偏。当前固件使用 Ark Pixel 16px proportional Latin + GNU Unifont 16×16 CJK，用户认为"字体不太对"，要求先做网络调研再在本 UI 里渲染验证。

## 2. 搜索来源（豆包搜索，2026-10-08）

检索词覆盖：嵌入式小屏幕中文点阵字体推荐 12px/16px、320x240 液晶中文点阵字库选型、逆变器/储能/充电桩屏幕中文点阵字体、中文像素字体（Zpix/小赖/Cubic11/丁卯）、best bitmap pixel font for small embedded LCD Chinese CJK 12px、文泉驿点阵正黑/点阵宋 12/13/15/16px、Sazanami/Misaki/Galmuri、embedded instrument digit font / DSEG。

| # | 来源 | 关键信息 |
|---|---|---|
| 1 | https://github.com/TakWolf/ark-pixel-font | 方舟像素字体，开源泛中日韩像素字体，黑体无衬线，10/12/16px；**16px 尺寸已废弃**（最后一个含 16px 的版本为 2026.09.01），官方建议 16px 改用 Unifont；字体 OFL-1.1 |
| 2 | https://github.com/TakWolf/fusion-pixel-font | 缝合像素字体，"缝合"多种开源字体补足 10/12px；字体 OFL-1.1（仓库工具 MIT） |
| 3 | http://wenq.org/wqy2/index.cgi?BitmapSong | 文泉驿点阵宋体：9pt–12pt（12×12…16×16），覆盖 GB2312/Big5/GBK/GB18030；**授权为 GPL v2（带字体嵌入例外）** |
| 4 | https://www.maoken.com/freefonts/20947.html | 复述文泉驿点阵宋体：>27,842 汉字，五档屏幕字号，GPL（个人/企业可免费商用） |
| 5 | https://github.com/larryli/u8g2_wqy | 把文泉驿点阵宋体做成 u8g2 字体，仓库内含 12×12/13×13/15×15/16×16 的 **BDF** 源文件；仓库代码 MIT，字体数据继承文泉驿 GPL |
| 6 | https://unifoundry.com/unifont/ 与 http://savannah.gnu.org/projects/unifont/ | GNU Unifont：BMP 全覆盖，8/16 像素宽、16 像素高，OFL-1.1 或 GPL-2+（字体嵌入例外） |
| 7 | https://m.baike.com/wiki/%E6%96%87%E6%B3%89%E9%A9%BF/2372378 | 文泉驿"提供了包含最多字符的开源字体 GNU Unifont 中大部分中日韩相关符号" —— 解释了我们测到的字形大量重合 |
| 8 | https://github.com/SolidZORO/zpix-pixel-font + 其 README | Zpix 最像素：简体/繁体/日文 12px 像素字体，口碑好；但 README 明确"授权与价格"：**付费取得授权**，且**禁止修改、反编译、转换、拆分与非法传播** → 不能随固件分发 |
| 9 | https://www.indienova.com/indie-game-news/dinkie-bitmap-font-by-3type/ 、https://www.gcores.com/articles/124251 | 丁卯点阵体 Dinkie Bitmap：7px/9px 中文像素字体，探索小尺寸极限；**商业授权**（个人 ¥299 / 商业授权另计） |
| 10 | https://github.com/ACh-K/Cubic-11 + https://www.100font.com/thread-228.htm | 俐方体 11 号 Cubic 11：基于 M+ gothic 12r，11px，OFL-1.1；简体覆盖为 GB2312 一级 + 常用国标字表一级，**更偏繁体**；仓库只提供 TTF |
| 11 | https://github.com/Warren2060/ChillBitmap + https://www.100font.com/thread-240.htm | 寒蝉点阵体：7px（基于美咲，13,600 字符）与 **16px（基于 Unifont 修改的字型）**，OFL-1.1 + GPL-2 字体嵌入例外 |
| 12 | https://timothyqiu.itch.io/vonwaon-bitmap + https://www.maoken.com/zhengyi/11117.html | 凤凰点阵体 VonwaonBitmap：自称 CC0、6763 汉字；但猫啃网指出**上游 aguegu/BitmapFont 为 GPL-3.0 且字模版权被质疑**，"不建议商用" |
| 13 | https://gitee.com/xu-fencer/kose-font | 小赖字体 Xiaolai：源自濑户/内海字体，覆盖 GB18030 全部 27,484 汉字、OFL；但它是**手写风格轮廓字体**，不是点阵字体 |
| 14 | https://www.keshikan.net/fonts-e.html | DSEG：7 段/14 段数码管风格字体，OFL-1.1，可作仪表数字参考 |
| 15 | https://www.universal-solder.ca/comparison-7-segment-matrix-character/ 、https://www.lightbo-led.com/blog/why-segment-vs-dot-matrix-displays-perform-differently/ | 七段适合"大而清晰的数字"，点阵适合较长/复杂的文字；两者不可互替 |
| 16 | http://www.perfectlawnsct.com/upload/datasheet/GT23L16U2Y.pdf | 集通数码 GENITOP GT23L16U2Y 标准汉字字库芯片：**12×12 与 16×16 点阵**，Unicode V1.0 兼容 GBK，SPI，DFN8/QFN16 —— 这类仪表/电表/逆变器产品的常见做法 |

补充（同为搜索命中，未单独成表）：CSDN 12864 取模教程（16×16 宋体字模数组的通用做法）、u8g2 官方 `chinese1/2/3` 与 `gb2312` 字符集约定、乐鑫 `xiaozhi-fonts` 组件（嵌入式中文点阵字库的另一种落地方式）。

## 3. 候选对比表

| 字体 | 像素尺寸 | 覆盖 | 风格 | 许可 | 能否随固件分发 | 链接 |
|---|---|---|---|---|---|---|
| **Ark Pixel（方舟像素）** | 10/12/16px（16px 已废弃） | 12px zh_hans 实测 24,870 字形；16px 仅 3,253 字形（缺常用字） | 黑体无衬线像素 | 字体 OFL-1.1，工具 MIT | ✅ 可以 | https://github.com/TakWolf/ark-pixel-font |
| **Fusion Pixel（缝合像素）** | 8/10/12px | 12px zh_hans 实测 37,000 字形 | 黑体像素（缝合多家） | 字体 OFL-1.1，工具 MIT | ✅ 可以 | https://github.com/TakWolf/fusion-pixel-font |
| **GNU Unifont** | 16px（8/16 宽） | BMP 全覆盖 | 等宽点阵（无衬线） | OFL-1.1 或 GPL-2+ 字体嵌入例外 | ✅ 可以（现用） | https://unifoundry.com/unifont/ |
| **文泉驿点阵宋体 WQY Bitmap Song** | 12/13/14/15/16px | 20,932 汉字 + 6,582 扩展 A（GB18030 全覆盖） | 点阵**宋体**（有衬线、横细竖粗） | **GPL-2.0 + 字体嵌入例外** | ⚠️ 可以，但带 copyleft，需法务确认与闭源固件混合 | http://wenq.org/wqy2/index.cgi?BitmapSong |
| **寒蝉点阵体 ChillBitmap** | 7px、16px | 7px 13,600 字符；16px 基于 Unifont | 像素；16px 的 CJK 与 Unifont 同源 | OFL-1.1 + GPL-2 字体嵌入例外 | ⚠️ 可以；16px 无实际增益 | https://github.com/Warren2060/ChillBitmap |
| **俐方体 11 号 Cubic 11** | 11px | 常用国标 4808 + Big5 5401 + GB2312 一级 | 黑体像素（偏繁体风格） | OFL-1.1 | ✅ 可以；但只有 TTF、11px 偏小、简体覆盖仅一级 | https://github.com/ACh-K/Cubic-11 |
| **Zpix 最像素** | 12px | 简/繁/日 | 像素黑体 | **商业付费授权** | ❌ 不可（禁止修改/再分发） | https://github.com/SolidZORO/zpix-pixel-font |
| **丁卯点阵体 Dinkie Bitmap** | 7/9px | 7px≈7000、9px≈11000 汉字 | 像素（矢量轮廓实现） | **商业付费授权** | ❌ 不可 | https://www.indienova.com/indie-game-news/dinkie-bitmap-font-by-3type/ |
| **凤凰点阵体 VonwaonBitmap** | 12/16px | 6763 汉字（Lite 3755） | 点阵宋 | 自称 CC0，**上游 GPL-3.0 + 字模版权存疑** | ❌ 不建议（法律风险） | https://timothyqiu.itch.io/vonwaon-bitmap |
| **小赖字体 Xiaolai** | 轮廓（非点阵） | GB18030 全 27,484 汉字 | 手写风 | OFL | ✅ 可以，但不适用：不是点阵字体 | https://gitee.com/xu-fencer/kose-font |
| **文泉驿点阵正黑** | —— | —— | —— | —— | ❌ 不存在点阵版本（"正黑"只有矢量轮廓，点阵版只有"点阵宋"） | http://wenq.org/wqy2/index.cgi |
| **Windows 中易宋体 12×12/16×16 点阵** | 12/16px | GB2312/GBK | 宋体点阵 | 专有（中易中标） | ❌ 不可 | 见 CSDN 取模教程等二手来源，未见公开授权 |

## 4. 同类产品的实际做法（调研所得）

搜索证据显示这一类产品（电表/逆变器/充电桩/小仪表）主流做法不是"找一款好看的矢量字体"，而是：

1. **字库芯片**：如集通 GENITOP GT23L16U2Y，直接把 **12×12 与 16×16** 点阵汉字放在 SPI 字库里，Unicode V1.0 兼容 GBK。即"12px 给密集/次要信息、16px 给正文"的两档点阵是行业惯例。
2. **嵌入式字库转换**：u8g2_wqy 这类项目把文泉驿点阵宋转成 12/13/14/15/16px 的 MCU 字库，说明文泉驿点阵宋在这一领域被长期使用——而它的字形与我们现在用的 Unifont CJK 高度重合（第 0 节实测）。
3. 16×16 宋体字模数组（CSDN 等大量取模教程）说明"16px 汉字"是这类屏幕的事实标准尺寸下限。

结论：在 2.8"/320×240 且倾斜观看的条件下，**16×16 汉字**是合适的下限；12×12 汉字偏小，只适合次要或密集信息。

## 5. 数字显示结论

- 检索到 DSEG（OFL-1.1）等七段/十四段字体，以及多篇"七段 vs 点阵"的工程对比文章，结论一致：**七段数码管风格适合"大而清晰的纯数字"**，点阵更适合混合文字；两者不可互换。
- 我们的读数（`4200 W`、`231.0V`、`52.4 V`、`78 %`）与单位、百分号混排，且同屏还有汉字段落；若改成七段风格字体，`V`/`%`/单位字母要么缺失要么风格冲突，反而降低可读性。
- 现有手工 12×24 段式数字比正文字高约 50%，形成清晰的信息层级，符合"数字要显著大于标签"的仪表惯例。
- **建议：保留 12×24 手工数字**。可选优化是把笔画端点/圆角改成与 Ark Pixel 更接近的像素风格（纯视觉微调，不涉及尺寸或许可）。

## 6. 渲染证据（host-only，未改 main/）

用 `tmp/font_preview/gen_variant_assets.cjs`（按 `--latin/--cjk` 组合生成 1-bit 字形表）与 `tmp/font_preview/render_ui_variant.c`（真实调用 `direct_ui_build` + `direct_ui_scene_draw`）渲染；5 行样例表用 `tmp/font_preview/render_unifont_baseline.c` 编译到同一套字形表。输出在 `docs/ui_redesign/font_candidates/`：

| 变体 | Latin / CJK | 页面 PNG | 样例表 PNG | 缺字 | 截断 |
|---|---|---|---|---|---|
| `current` | Ark16 Latin + Unifont 16 CJK（现固件） | ui_current_home / _parameters / _alarms | ui_samples_current | 无 | 无 |
| `fusion12` | Fusion 12px（Latin+CJK 同一字体） | ui_fusion12_home / _parameters / _alarms | ui_samples_fusion12 | 无 | 无 |
| `wqy16` | Ark16 Latin + 文泉驿点阵宋 16×16 | ui_wqy16_home / _parameters / _alarms | ui_samples_wqy16 | 无 | 无 |
| `wqy12` | Ark16 Latin + 文泉驿点阵宋 12×12 | ui_wqy12_home / _parameters / _alarms | ui_samples_wqy12 | 无 | 无 |

（另有上一轮遗留的 `mixed` 变体 = Ark Latin 按原始 BBX 摆放 + Unifont CJK，仅用于对比 Latin 垂直定位，不代表新方案。）

- 全部变体在 408 个 UI 相关码位上**零缺字**；在 home / running-parameters / alarms 三页上**零截断**（脚本按行宽 292px 检查，行宽超出即报 CLIP）。
- `ui_samples_wqy16.png` 与 `ui_samples_current.png` 的 SHA256 完全相同，直观印证了"16px 文泉驿点阵宋 ≈ 16px Unifont"。
- 观感：`wqy12`/`fusion12` 的汉字明显偏小、行间留白大，在 320×240 上更"淡"；`current`/`wqy16` 的 16px 汉字在倾斜视角下更稳。

## 7. 推荐

**主推（保持现状）：Ark Pixel 16px proportional Latin（OFL-1.1） + GNU Unifont 16×16 CJK，16px 单元格；数字保留手工 12×24。**

理由：
1. 16px 汉字在这一屏（2.8"/240×320、倾斜观看）是合适的下限；12px 汉字偏小。
2. 在"可自由分发"的前提下，**没有比 Unifont 更好的 16×16 简体点阵汉字**：文泉驿点阵宋与之 305/313 字形完全相同（且换过去要背 GPL）；Ark 16px CJK 已废弃且只有 3,253 字形；Zpix/丁卯好看但是付费；凤凰点阵体/中易宋存在版权风险；Cubic 11/寒蝉 7px 尺寸过小；小赖不是点阵。
3. Latin 侧 Ark Pixel 16px proportional 已经解决了原来固定 8px 等宽 Unifont ASCII 的粗糙问题（比例字宽、字形更规整），且为 OFL。

**备选：Fusion Pixel 12px proportional（OFL-1.1）全线 12px。** 优点是字形更细腻、覆盖率最高（12px zh_hans 实测 37,000 字形）、许可干净；缺点是整体字号小一档，2.8" 屏 + 倾斜视角下中文可读性略逊。若后续要提高每屏信息密度（更多行），这是首选替代。

**不建议**：Zpix（付费）、丁卯点阵体（付费）、凤凰点阵体（版权存疑）、Windows 中易宋点阵（专有）。

## 8. 局限与未验证

- **没有做真机验证**：以上全部是公开资料 + 主机端 1-bit 渲染的结果，未在真实 ST7789 面板上确认对比度、TN 视角下的可读性、刷新与观感。
- 12px 汉字的"是否够读"只能在真机上最终判定；本次只给出渲染对比。
- 部分二手页面（字体下载站）对覆盖率的表述与仓库实际文件可能不一致；本笔记里凡我未直接核对的数字都注明了出处（如"猫啃网称"），未直接核对的项已标注。
- 文中"Unifont CJK 大量来自文泉驿"的证据是：文泉驿百科的表述 + 本次 313 个 UI 汉字中 305 个字形逐字节相同的实测；这是强相关证据，但不是官方 provenance 声明。
- 未在本次任务中修改任何 `main/` 代码、未重新编译固件、未改动 release 目录。
