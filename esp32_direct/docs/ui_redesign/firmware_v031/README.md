# 0.3.1 同源预览

PNG 来自 tests/ui_render_test.c 实际 C 绘图代码，输入为合成测试数据。运行 tests/export_ui_previews.cjs docs/ui_redesign/firmware_v031 可重新导出。真实 LCD 的伽马、色彩、观察角度与响应速度不由主机预览模拟。

方向延续已确认四方向布局；本轮按用户要求增加饱和分类色和白色文字，字体采用原生像素二值覆盖及半粗笔画。初始化恢复用户原 ARM 驱动的模拟电源/伽马参数。
