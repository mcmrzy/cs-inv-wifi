import 'dart:math';

/// SN 编码生成器 — 辰烁产品序列号编码规则
/// 遵循文档: CS-SN-STD-001 V1.1
/// 编码格式 (16 位): MM-CC-PPPP-YW-SSSSS-C
class SnGenerator {
  // 年份编码表 (36进制, 跳过 O 和 I)
  static const String _yearCodes = "123456789ABCDEFGHJKLMNPQRSTUVWXYZ";
  static const int _yearBase = 2024;

  // 月份编码映射
  static const Map<int, String> _monthCodes = {
    1: '1', 2: '2', 3: '3', 4: '4', 5: '5', 6: '6',
    7: '7', 8: '8', 9: '9', 10: 'A', 11: 'B', 12: 'C'
  };

  // 客户等级
  static const List<String> validGrades = ['A', 'B', 'C', 'D', 'E', 'P', 'X'];

  // 常用国家代码 (ISO 3166-1 alpha-2)
  static const List<String> commonCountries = [
    'CN', 'DE', 'NL', 'AU', 'BR', 'JP', 'US', 'ZA',
    'GB', 'FR', 'IT', 'ES', 'PT', 'BE', 'AT', 'CH',
    'SE', 'NO', 'DK', 'FI', 'PL', 'TR', 'IN', 'TH',
    'VN', 'MY', 'SG', 'PH', 'KR', 'MX', 'CL', 'AR',
    'AE', 'SA', 'EG', 'NG', 'CA', 'NZ', 'RU',
    '99', 'ZZ',
  ];

  // 厂商编码类型
  static const Map<String, String> manufacturerTypes = {
    'H': '总部自研产线',
    'O': '代工厂',
    'S': '子公司产线',
  };

  /// CRC-16 MODBUS 算法
  static int _crc16Modbus(List<int> data) {
    int crc = 0xFFFF;
    for (final b in data) {
      crc ^= b;
      for (int i = 0; i < 8; i++) {
        if (crc & 0x0001 != 0) {
          crc = (crc >> 1) ^ 0xA001;
        } else {
          crc >>= 1;
        }
      }
    }
    return crc;
  }

  /// 数值转字符 (0-9 → '0'-'9', 10-35 → 'A'-'Z')
  static String _valueChar(int v) {
    if (v < 10) return String.fromCharCode('0'.codeUnitAt(0) + v);
    return String.fromCharCode('A'.codeUnitAt(0) + (v - 10));
  }

  /// 计算前15位的CRC-16 MODBUS校验码
  static String computeCheckDigit(String prefix15) {
    if (prefix15.length != 15) return '?';
    final bytes = prefix15.codeUnits;
    final crc = _crc16Modbus(bytes);
    final lo = crc & 0xFF;
    return _valueChar(lo % 36);
  }

  /// 验证SN格式 (不验证校验位)
  static bool validateFormat(String sn) {
    if (sn.isEmpty || sn.length != 16) return false;
    sn = sn.toUpperCase();
    
    // 全部字母数字
    if (!RegExp(r'^[A-Z0-9]+$').hasMatch(sn)) return false;
    
    // 厂商码第1位必须字母
    if (!RegExp(r'^[A-Z]').hasMatch(sn)) return false;
    
    // 客户等级 (第5位)
    final grade = sn[4];
    if (!validGrades.contains(grade) && !RegExp(r'^[0-9]$').hasMatch(grade)) return false;
    
    // 客户流水 3位数字
    if (!RegExp(r'^[0-9]{3}$').hasMatch(sn.substring(5, 8))) return false;
    
    // 年份码
    if (!_yearCodes.contains(sn[8])) return false;
    
    // 月份码
    if (!_monthCodes.values.contains(sn[9])) return false;
    
    // 序列号 5位数字
    if (!RegExp(r'^[0-9]{5}$').hasMatch(sn.substring(10, 15))) return false;
    
    return true;
  }

  /// 完整验证SN (格式 + 校验位)
  static bool validate(String sn) {
    if (!validateFormat(sn)) return false;
    sn = sn.toUpperCase();
    final expected = computeCheckDigit(sn.substring(0, 15));
    return expected == sn[15];
  }

  /// 解析SN为字段字典
  static Map<String, dynamic>? parse(String sn) {
    if (!validateFormat(sn)) return null;
    sn = sn.toUpperCase();
    
    final yearIndex = _yearCodes.indexOf(sn[8]);
    final year = _yearBase + yearIndex;
    final month = _monthCodes.entries
        .firstWhere((e) => e.value == sn[9], orElse: () => MapEntry(0, ''))
        .key;
    
    return {
      'manufacturer': sn.substring(0, 2),
      'country': sn.substring(2, 4),
      'customer': sn.substring(4, 8),
      'grade': sn[4],
      'grade_name': _gradeName(sn[4]),
      'cust_num': int.tryParse(sn.substring(5, 8)) ?? 0,
      'date_code': sn.substring(8, 10),
      'year': year,
      'month': month,
      'sequence': sn.substring(10, 15),
      'seq_num': int.tryParse(sn.substring(10, 15)) ?? 0,
      'check_digit': sn[15],
    };
  }

  /// 生成完整SN (含校验位)
  static String generate({
    String manufacturer = 'H1',
    String country = 'DE',
    String customer = 'A001',
    String dateCode = '2A',
    String sequence = '00088',
  }) {
    manufacturer = manufacturer.toUpperCase();
    country = country.toUpperCase();
    customer = customer.toUpperCase();
    dateCode = dateCode.toUpperCase();
    sequence = sequence.toUpperCase();
    
    final prefix = manufacturer + country + customer + dateCode + sequence;
    if (prefix.length != 15) return '';
    
    final check = computeCheckDigit(prefix);
    return prefix + check;
  }

  /// 年份编码
  static String yearEncode(int year) {
    final offset = max(0, year - _yearBase) % _yearCodes.length;
    return _yearCodes[offset];
  }

  /// 年份解码
  static int yearDecode(String code) {
    code = code.toUpperCase();
    final index = _yearCodes.indexOf(code);
    if (index >= 0) return _yearBase + index;
    return -1;
  }

  /// 月份编码
  static String monthEncode(int month) {
    return _monthCodes[month] ?? '?';
  }

  /// 批量生成SN列表
  static List<String> generateBatch({
    required int count,
    String manufacturer = 'H1',
    String country = 'DE',
    String customer = 'A001',
    String? dateCode,
    int startSequence = 1,
  }) {
    final now = DateTime.now();
    dateCode ??= '${yearEncode(now.year)}${monthEncode(now.month)}';
    
    final List<String> sns = [];
    for (int i = 0; i < count; i++) {
      final seq = (startSequence + i).toString().padLeft(5, '0');
      final sn = generate(
        manufacturer: manufacturer,
        country: country,
        customer: customer,
        dateCode: dateCode,
        sequence: seq,
      );
      if (sn.isNotEmpty) {
        sns.add(sn);
      }
    }
    return sns;
  }

  /// 生成随机SN (用于测试)
  static String generateRandom() {
    final random = Random();
    final manufacturer = 'H${random.nextInt(9) + 1}';
    final country = commonCountries[random.nextInt(commonCountries.length)];
    final grade = validGrades[random.nextInt(validGrades.length)];
    final custNum = random.nextInt(1000).toString().padLeft(3, '0');
    final customer = '$grade$custNum';
    final now = DateTime.now();
    final dateCode = '${yearEncode(now.year)}${monthEncode(now.month)}';
    final sequence = (random.nextInt(100000)).toString().padLeft(5, '0');
    
    return generate(
      manufacturer: manufacturer,
      country: country,
      customer: customer,
      dateCode: dateCode,
      sequence: sequence,
    );
  }

  /// 客户等级名称
  static String _gradeName(String grade) {
    const names = {
      'A': '海外战略大客户',
      'B': '区域核心分销商',
      'C': '安装商/经销商',
      'D': '授权安装商/EPC',
      'E': '区域二级代理',
      'P': '项目代码',
      'X': '零售/无合同',
    };
    return names[grade] ?? '';
  }

  /// 获取SN的简短标识 (末6位)
  static String shortId(String sn) {
    if (sn.length >= 6) {
      return sn.substring(sn.length - 6);
    }
    return sn;
  }
}