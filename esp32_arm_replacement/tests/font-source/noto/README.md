# Noto Sans CJK SC Medium 字体来源

官方来源：https://github.com/notofonts/noto-cjk
源文件：https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Medium.otf
许可：https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/LICENSE
下载日期：2026-10-08。许可证全文保存在LICENSE.txt。
源OTF SHA256：ca094f6b0001fb048ca39ddd797a0cdb0179e1e55c6561e111c49c3e6a61d7b7。

固件仅使用界面所需字形子集。生成工具 tests/generate_gfx_noto.py：Pillow12.3.0，fontTools4.60.1；在工程根目录运行。临时字体检查依赖可通过PYTHONPATH=tmp/font_python加载。字体生成后需运行gfx_test与ui_render_test确认布局和条带一致性。
旧 generate_gfx_native.cjs 用于历史点阵版本，会覆盖同名资产；当前版本请使用 generate_gfx_noto.py。

当前v0.4.3：正文全部16px/20px统一字框，无按字符语言选择字号；主数值24/28px字库含数字标点及单位，其他文字整行回退16px。字模位图69000字节。
