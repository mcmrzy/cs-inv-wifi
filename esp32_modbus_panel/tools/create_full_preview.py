"""Create a contact sheet from actual host-rendered firmware UI fixtures."""
from pathlib import Path
import re
from PIL import Image, ImageDraw, ImageFont
root = Path(__file__).resolve().parents[1]
fixtures = [
 ('能源仪表盘 · 实时功率与流向','lvgl_home.ppm'),
 ('能源仪表盘 · 浅色主题','lvgl_home_light.ppm'),
 ('能源仪表盘 · 通信中断时的上次数据','lvgl_home_held.ppm'),
 ('主菜单 / 图标宫格与四向选择','lvgl_menu.ppm'),
 ('逆变器设置 / 参数与当前值','lvgl_settings.ppm'),
 ('电压编辑 / 当前与待设值','lvgl_voltage_editor.ppm'),
 ('读回值不一致 / 对照目标与实际值','lvgl_mismatch.ppm'),
 ('频率编辑 / 方向键调整','lvgl_editor.ppm'),
 ('电池状态 / 实时数据','lvgl_bms.ppm'),
 ('电池状态 / 单体电压','lvgl_bms_cells.ppm'),
 ('手机连接 / 设备二维码','lvgl_phone.ppm'),
 ('电池断联 / 上次数据','lvgl_bms_offline.ppm'),
 ('开关机等待 / 不重复下发','lvgl_waiting.ppm'),
 ('本次设置 / 已读回确认','lvgl_result.ppm'),
 ('主动03查询 / 实际值与返回状态','lvgl_setting_reads.ppm'),
 ('读写地址返回值 / 用于编辑','lvgl_returned_value.ppm'),
 ('中文逗号 / 字体基线回归','lvgl_font_alignment.ppm'),
 ('故障告警 · 当前告警与详细说明','lvgl_alarms.ppm'),
 ('首页 · 告警入口突出显示','lvgl_home_alarm.ppm'),
 ('参数 · 编辑前显示限制','lvgl_setting_info.ppm'),
 ('显示设置 · 保存失败可直接重试','lvgl_appearance_retry.ppm'),
 ('逆变器设置 / 参数分类','lvgl_settings_groups.ppm'),
 ('运行参数 / 图示目录','lvgl_detail_groups.ppm'),
 ('运行参数 / 电网仪表','lvgl_detail_dashboard.ppm'),
 ('开关机 / 英文菜单','lvgl_power_en.ppm'),
 ('电压编辑 / 英文对照','lvgl_voltage_editor_en.ppm'),
 ('逆变器设置 / 系统与并机','lvgl_settings_groups_more.ppm'),
 ('逆变器设置 / 英文分类','lvgl_settings_groups_more_en.ppm'),
 ('输出电压 / 当前与目标并排','lvgl_output_editor.ppm'),
 ('0A 小功率 / 保留实测 3W','lvgl_home_zero_amp.ppm'),
 ('参数编辑 / 浅色对齐','lvgl_voltage_editor_light.ppm'),
 ('设置分类 / 浅色与英文','lvgl_settings_groups_more_light.ppm'),
 ('长沙 / 七天天气','lvgl_weather_7days.ppm'),
]
font_path=root.parent/'esp32_direct/tests/font-source/noto/NotoSansCJKsc-Medium.otf'
font=ImageFont.truetype(str(font_path),16)
large=ImageFont.truetype(str(font_path),22)
canvas=Image.new('RGB',(1020,62+((len(fixtures)+2)//3)*280),'#e9edf2');draw=ImageDraw.Draw(canvas)
version=re.search(r'PROJECT_VER "([^"]+)-modbus-full"',(root/'CMakeLists.txt').read_text()).group(1)
draw.text((20,12),f'CS-MB-PANEL {version} 试产验证 · 固件渲染与示例数据',font=large,fill='#182b45')
for i,(title,filename) in enumerate(fixtures):
 im=Image.open(root/'tmp'/filename).convert('RGB');assert im.size==(320,240)
 x=10+(i%3)*340;y=62+(i//3)*280
 canvas.paste(im,(x,y));draw.text((x,y+247),title,font=font,fill='#182b45')
canvas.save(root/'tmp/full_screen_preview.png')
review=Image.new('RGB',(1020,902),'#e9edf2');review_draw=ImageDraw.Draw(review)
review_draw.text((20,12),f'CS-MB-PANEL {version} 界面设计 · 固件渲染与示例数据',font=large,fill='#182b45')
for i,index in enumerate([0,21,23,4,5,25,26,30,32]):
 title,filename=fixtures[index];x=10+i%3*340;y=62+i//3*280
 review.paste(Image.open(root/'tmp'/filename).convert('RGB'),(x,y))
 review_draw.text((x,y+247),title,font=font,fill='#182b45')
review.save(root/'tmp/ui_readback_review.png')
print(f'PASS preview: {len(fixtures)} fixture views -> tmp/full_screen_preview.png')
