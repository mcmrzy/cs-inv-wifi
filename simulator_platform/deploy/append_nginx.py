#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""追加 sim.jiuxiaoyw.online server block 到 nginx 配置"""
import sys

CONF = "nginx-proxy.conf.bak"
BLOCK = '''

# ---------- sim.jiuxiaoyw.online -> 逆变器模拟平台（CS-L10-6K2） ----------
server {
    listen 80;
    server_name sim.jiuxiaoyw.online;

    location /.well-known/acme-challenge/ {
        root /var/www/certbot;
    }

    location / {
        return 301 https://$host$request_uri;
    }
}

server {
    listen 443 ssl;
    http2 on;
    server_name sim.jiuxiaoyw.online;

    ssl_certificate /etc/letsencrypt/live/sim.jiuxiaoyw.online/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/sim.jiuxiaoyw.online/privkey.pem;
    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_session_cache shared:SSL:10m;
    ssl_session_timeout 1d;
    ssl_session_tickets off;

    add_header Strict-Transport-Security "max-age=31536000; includeSubDomains" always;
    add_header X-Content-Type-Options "nosniff" always;

    client_max_body_size 20m;

    location / {
        proxy_pass http://cs-inv-sim:8088;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto https;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection $connection_upgrade;
        proxy_read_timeout 120s;
    }
}
'''

def main():
    with open(CONF, "a", encoding="utf-8") as f:
        f.write(BLOCK)
    print(f"已追加到 {CONF}")

if __name__ == "__main__":
    main()