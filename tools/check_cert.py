#!/usr/bin/env python3
"""检查 jiuxiaoyw.online:8883 TLS 证书详情与系统信任验证"""
import ssl
import socket
import sys

HOST = 'jiuxiaoyw.online'
PORT = 8883


def show_cert(der, label):
    try:
        info = ssl._ssl._test_decode_cert(der)
        print(f"--- {label} ---")
        print("subject:", info.get('subject'))
        print("issuer :", info.get('issuer'))
        print("notBefore:", info.get('notBefore'))
        print("notAfter :", info.get('notAfter'))
        san = info.get('subjectAltName')
        print("SAN     :", san if san else "(none)")
        print()
    except Exception as e:
        print(f"{label} decode failed: {e}")


def get_chain_certs():
    """抓取服务器下发的完整证书链 (含中间证书)"""
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    with socket.create_connection((HOST, PORT), timeout=10) as sock:
        with ctx.wrap_socket(sock, server_hostname=HOST) as ssock:
            chain = ssock.get_unverified_chain()  # 完整链
            return chain


def verify_with_system(leaf):
    """用系统 CA 库验证叶子证书"""
    try:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_REQUIRED
        with socket.create_connection((HOST, PORT), timeout=10) as sock:
            with ctx.wrap_socket(sock, server_hostname=HOST) as ssock:
                print("系统CA验证: OK, cipher=", ssock.cipher())
    except Exception as e:
        print("系统CA验证失败:", e)


if __name__ == '__main__':
    print(f"连接 {HOST}:{PORT} ...")
    try:
        chain = get_chain_certs()
        print(f"服务器下发证书链数量: {len(chain)}\n")
        for i, der in enumerate(chain):
            label = f"证书[{i}]"
            show_cert(der, label)
        if chain:
            verify_with_system(chain[0])
    except Exception as e:
        print("错误:", e)
        sys.exit(1)
