"""Factory PIN derivation. Product secret is read at runtime, never bundled."""
import hashlib
import hmac
import os
import re

def compute_pin(sn, secret=None):
    if not re.fullmatch(r'[A-Z0-9]{16}', sn):
        raise ValueError('扫码 SN 必须为16位大写字母或数字')
    if secret is None:
        secret = os.environ.get('PRODUCT_SECRET', '')
    if not secret:
        raise ValueError('工位未配置 PRODUCT_SECRET，未写入 PIN')
    digest = hmac.new(secret.encode('utf-8'), sn.encode('ascii'), hashlib.sha256).digest()
    return f'{int.from_bytes(digest[:3], "big") % 1000000:06d}'

def payload(sn, pin):
    if not re.fullmatch(r'[A-Z0-9]{16}', sn) or not re.fullmatch(r'[0-9]{6}', pin):
        raise ValueError('SN/PIN 格式不正确')
    return (sn + pin).encode('ascii') + b'\0'
