#!/usr/bin/env python3
"""获取 MQTT Broker 的 TLS 证书"""
import ssl
import socket
import sys

def get_server_cert(host, port):
    """获取服务器证书链"""
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    
    with socket.create_connection((host, port)) as sock:
        with context.wrap_socket(sock, server_hostname=host) as ssock:
            cert = ssock.getpeercert(binary_form=True)
            return cert

def cert_to_pem(cert_der):
    """将 DER 格式证书转换为 PEM 格式"""
    import base64
    pem = base64.b64encode(cert_der).decode('ascii')
    lines = ['-----BEGIN CERTIFICATE-----']
    for i in range(0, len(pem), 64):
        lines.append(pem[i:i+64])
    lines.append('-----END CERTIFICATE-----')
    return '\n'.join(lines)

if __name__ == '__main__':
    host = 'jiuxiaoyw.online'
    port = 8883
    
    print(f"正在连接 {host}:{port} 获取证书...")
    try:
        cert_der = get_server_cert(host, port)
        cert_pem = cert_to_pem(cert_der)
        print("\n=== 服务器证书 (PEM 格式) ===")
        print(cert_pem)
        
        # 保存到文件
        output_file = f'{host}_ca.crt'
        with open(output_file, 'w') as f:
            f.write(cert_pem)
        print(f"\n证书已保存到: {output_file}")
        
    except Exception as e:
        print(f"错误: {e}")
        sys.exit(1)
