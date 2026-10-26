# 远程设置服务端、App 与后台发布记录

2026-10-20。按服务端优先、App/后台其次的顺序发布远程控制阶段语义与设备能力门禁。

## 服务端

- PR [#12](https://github.com/mcmrzy/cs_inv_monitor-develop/pull/12)，合并提交 `a5bb0a731e963614fbd023c4f54341c79359b0f7`。
- 业务 API、设备服务运行该提交；`acknowledged` / `executing` 保持进度，`submitted` 独立显示“已保存并提交，设备生效未确认”，仅 `applied` / `completed` 收口成功。
- 命令期限完整透传，回执按 MQTT 路由 SN 绑定；重复、延迟 ACK 与运输状态不能覆盖设备结果。direct 能力证据限制未确认的 AC 开关。
- Excelize 升级到包含四项漏洞修复的提交；`govulncheck` 报告代码可达漏洞为 0。
- CD 构建、数据库备份、九个容器健康、API Gateway `/health` 与设备服务 MQTT/Redis 就绪检查全部通过。

## App 与后台

- PR [#13](https://github.com/mcmrzy/cs_inv_monitor-develop/pull/13)，合并提交 `ebf39c4d55e70a897d5b60ba76358e4a489538a6`。
- 后台生产镜像为 `ghcr.io/mcmrzy/inv-admin-frontend:ebf39c4d55e70a897d5b60ba76358e4a489538a6`，部署后健康检查通过。
- App 版本 1.1.13 (41)，包名 `com.csergy.app1`，大小 44,989,685 字节。
- SHA-256：`84002a65e49d6db9c2ad4e6a6b02190a535c7c72a2234bbebcb79f6cd3d48b9f`。
- v2 签名有效，证书 SHA-256 与此前版本一致。
- 通过正式后台发布记录 36；公开接口 `ota/app/latest?platform=android` 返回 1.1.13 / 41，灰度 100%，下载文件重新下载后哈希与本地交付一致。

App 交付目录：`release/CSERGY_App_v1.1.13_41_20261020/`。

## 验证边界

本次确认源码、CI、生产部署、后台记录和公开下载。固件 0.6.40 已构建但未刷机；真实 MQTT 命令下发、ESP 保存/串口提交、DSP 实际采用、手机安装新 App 与实机界面仍需现场验收。
