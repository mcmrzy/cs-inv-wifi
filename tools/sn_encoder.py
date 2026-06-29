#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
SN 编码模块 (Python) — 辰烁产品序列号编码规则
遵循文档: CS-SN-STD-001 V1.1

编码格式 (16 位): MM-CC-PPPP-YW-SSSSS-C
校验位算法: CRC-16 MODBUS (多项式 0x8005) mod 36
"""

# ==================== 年份编码表 ====================
# 36 进制, 跳过 O 和 I
YEAR_CODES = "123456789ABCDEFGHJKLMNPQRSTUVWXYZ"  # 33 个值
YEAR_BASE  = 2024

# 月份编码: 1-9→'1'-'9', 10→'A', 11→'B', 12→'C'
MONTH_CODES = {
    1:'1', 2:'2', 3:'3', 4:'4', 5:'5', 6:'6',
    7:'7', 8:'8', 9:'9', 10:'A', 11:'B', 12:'C'
}
MONTH_DECODE = {v:k for k, v in MONTH_CODES.items()}

# ==================== 客户等级 ====================
VALID_GRADES = ['A', 'B', 'C', 'D', 'E', 'P', 'X']
GRADE_NAMES = {
    'A': '海外战略大客户',
    'B': '区域核心分销商',
    'C': '安装商/经销商',
    'D': '授权安装商/EPC',
    'E': '区域二级代理',
    'P': '项目代码',
    'X': '零售/无合同',
}

# ==================== 常用国家代码 (ISO 3166-1 alpha-2) ====================
COMMON_COUNTRIES = [
    ("CN", "中国"), ("DE", "德国"), ("NL", "荷兰"), ("AU", "澳大利亚"),
    ("BR", "巴西"), ("JP", "日本"), ("US", "美国"), ("ZA", "南非"),
    ("GB", "英国"), ("FR", "法国"), ("IT", "意大利"), ("ES", "西班牙"),
    ("PT", "葡萄牙"), ("BE", "比利时"), ("AT", "奥地利"), ("CH", "瑞士"),
    ("SE", "瑞典"), ("NO", "挪威"), ("DK", "丹麦"), ("FI", "芬兰"),
    ("PL", "波兰"), ("TR", "土耳其"), ("IN", "印度"), ("TH", "泰国"),
    ("VN", "越南"), ("MY", "马来西亚"), ("SG", "新加坡"), ("PH", "菲律宾"),
    ("KR", "韩国"), ("MX", "墨西哥"), ("CL", "智利"), ("AR", "阿根廷"),
    ("AE", "阿联酋"), ("SA", "沙特"), ("EG", "埃及"), ("NG", "尼日利亚"),
    ("CA", "加拿大"), ("NZ", "新西兰"), ("RU", "俄罗斯"),
    ("99", "未指定"), ("ZZ", "其他"),
]

# ==================== 厂商编码 ====================
MANUFACTURER_TYPES = {
    'H': '总部自研产线',
    'O': '代工厂',
    'S': '子公司产线',
}


# ==================== CRC-16 MODBUS ====================
def _crc16_modbus(data: bytes) -> int:
    """
    CRC-16 MODBUS
    参数: Poly=0x8005, Init=0xFFFF, RefIn=true, RefOut=true
    """
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001  # 0x8005 反转后为 0xA001
            else:
                crc >>= 1
    return crc


def _value_char(v: int) -> str:
    """数值 → 字符: 0-9→'0'-'9', 10-35→'A'-'Z'"""
    if v < 10:
        return chr(ord('0') + v)
    return chr(ord('A') + (v - 10))


def compute_check_digit(prefix_15: str) -> str:
    """
    计算前 15 位的 CRC-16 MODBUS 校验码
    步骤:
      1. 对前15位字符串的ASCII字节做 CRC-16 MODBUS 运算
      2. 取低8位 (crc & 0xFF)
      3. 对36取模 (lo % 36)
      4. 映射为36进制字符 (0-9→'0'-'9', 10-35→'A'-'Z')
    """
    if len(prefix_15) != 15:
        return '?'
    crc = _crc16_modbus(prefix_15.encode('ascii'))
    lo = crc & 0xFF  # 取低8位
    return _value_char(lo % 36)


def validate_format(sn: str) -> bool:
    """验证 SN 格式 (不验证校验位)"""
    if not sn or len(sn) != 16:
        return False

    sn = sn.upper()
    # 全部字母数字
    if not all(c.isalnum() for c in sn):
        return False
    # 厂商码第 1 位必须字母
    if not sn[0].isalpha():
        return False
    # 客户等级
    if sn[4] not in VALID_GRADES and not sn[4].isdigit():
        return False
    # 客户流水 3 位数字
    if not sn[5:8].isdigit():
        return False
    # 年份码
    if sn[8] not in YEAR_CODES:
        return False
    # 月份码
    if sn[9] not in MONTH_DECODE:
        return False
    # 序列号 5 位数字
    if not sn[10:15].isdigit():
        return False
    return True


def validate(sn: str) -> bool:
    """完整验证 SN (格式 + 校验位)"""
    if not validate_format(sn):
        return False
    sn = sn.upper()
    expected = compute_check_digit(sn[:15])
    return expected == sn[15]


def parse(sn: str) -> dict:
    """解析 SN 为字段字典"""
    if not validate_format(sn):
        return {}
    sn = sn.upper()
    year = YEAR_BASE + YEAR_CODES.index(sn[8])
    month = MONTH_DECODE.get(sn[9], 0)
    return {
        'manufacturer':  sn[0:2],
        'country':       sn[2:4],
        'customer':      sn[4:8],
        'grade':         sn[4],
        'grade_name':    GRADE_NAMES.get(sn[4], ''),
        'cust_num':      int(sn[5:8]),
        'date_code':     sn[8:10],
        'year':          year,
        'month':         month,
        'sequence':      sn[10:15],
        'seq_num':       int(sn[10:15]),
        'check_digit':   sn[15],
    }


def generate(manufacturer: str, country: str, customer: str,
             date_code: str, sequence: str) -> str:
    """从字段生成完整 SN (含校验位)"""
    prefix = (manufacturer + country + customer + date_code + sequence).upper()
    if len(prefix) != 15:
        return ''
    check = compute_check_digit(prefix)
    return prefix + check


def year_encode(year: int) -> str:
    """公历年份 → 年份码"""
    offset = max(0, year - YEAR_BASE)
    offset = offset % len(YEAR_CODES)
    return YEAR_CODES[offset]


def year_decode(code: str) -> int:
    """年份码 → 公历年份"""
    code = code.upper()
    if code in YEAR_CODES:
        return YEAR_BASE + YEAR_CODES.index(code)
    return -1


def month_encode(month: int) -> str:
    """月份 → 月份码"""
    return MONTH_CODES.get(month, '?')


def printable(sn: str) -> str:
    """返回原始 SN 字符串"""
    return sn


# ==================== 自测 ====================
if __name__ == '__main__':
    # 生成一个正确的测试 SN (CRC-16 MODBUS)
    test_prefix = "H1DEA0012A00088"
    check = compute_check_digit(test_prefix)
    test_sn = test_prefix + check
    print(f"测试 SN: {test_sn}")
    print(f"  可读:   {printable(test_sn)}")
    print(f"  格式:   {'✓' if validate_format(test_sn) else '✗'}")
    print(f"  校验:   {'✓' if validate(test_sn) else '✗'}")
    crc_val = _crc16_modbus(test_prefix.encode('ascii'))
    print(f"  CRC-16: 0x{crc_val:04X}")
    print(f"  低8位:  0x{crc_val & 0xFF:02X} ({crc_val & 0xFF})")
    print(f"  mod 36: {(crc_val & 0xFF) % 36}")
    print(f"  校验位: {check}")

    fields = parse(test_sn)
    if fields:
        print(f"\n解析结果:")
        print(f"  厂商:   {fields['manufacturer']}")
        print(f"  国家:   {fields['country']}")
        print(f"  客户:   {fields['customer']} ({fields['grade_name']})")
        print(f"  日期:   {fields['year']}年{fields['month']}月")
        print(f"  序列:   {fields['sequence']}")

    # 生成测试
    gen = generate("H1", "DE", "A001", "2A", "00088")
    print(f"\n生成 SN: {gen}")
    print(f"  一致性: {'✓' if gen == test_sn else '✗'}")

    # 年份编码测试
    print(f"\n年份编码:")
    for y in [2024, 2025, 2026, 2032, 2033, 2034, 2049]:
        print(f"  {y} → '{year_encode(y)}' → {year_decode(year_encode(y))}")

    # 月份编码测试
    print(f"\n月份编码:")
    for m in range(1, 13):
        print(f"  {m:2d} → '{month_encode(m)}'")
