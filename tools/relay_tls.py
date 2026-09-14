"""TCP 中继: 读一条 TLS 连接两个方向的记录长度, 用于判定 mbedtls IN_CONTENT_LEN 需求。
用法: python relay.py <listen_port> <real_ip> <real_port> <log_path>
"""
import socket
import struct
import sys
import threading
import time

listen_port = int(sys.argv[1])
real_ip = sys.argv[2]
real_port = int(sys.argv[3])
log_path = sys.argv[4]

out = open(log_path, "w", encoding="utf-8")
lock = threading.Lock()
stats = {"c2s": [], "s2c": []}


def log(msg):
    with lock:
        out.write(msg + "\n")
        out.flush()


def pump(src, dst, direction):
    """按 TLS 记录边界读取并记录长度 (记录头 5 字节, 明文握手/CCS/alert 可见;
    加密记录长度即记录头长度字段)。"""
    buf = b""
    while True:
        try:
            data = src.recv(65536)
        except OSError:
            break
        if not data:
            break
        try:
            dst.sendall(data)
        except OSError:
            break
        buf += data
        while len(buf) >= 5:
            ctype = buf[0]
            length = struct.unpack(">H", buf[3:5])[0]
            total = 5 + length
            if len(buf) < total:
                break
            stats[direction].append(length)
            names = {20: "CCS", 21: "ALERT", 22: "HANDSHAKE", 23: "APP_DATA"}
            if len(stats[direction]) <= 12 or length > 8192:
                log(f"{direction} type={names.get(ctype, ctype)} len={length}")
            buf = buf[total:]
    # 收尾: 合并双向统计
    try:
        dst.shutdown(socket.SHUT_WR)
    except OSError:
        pass


def handle(client):
    try:
        server = socket.create_connection((real_ip, real_port), timeout=20)
    except OSError as exc:
        log(f"upstream connect failed: {exc}")
        client.close()
        return
    t1 = threading.Thread(target=pump, args=(client, server, "c2s"), daemon=True)
    t2 = threading.Thread(target=pump, args=(server, client, "s2c"), daemon=True)
    t1.start()
    t2.start()
    t1.join()
    t2.join()
    client.close()
    server.close()


srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", listen_port))
srv.listen(5)
log(f"relay 127.0.0.1:{listen_port} -> {real_ip}:{real_port}")
srv.settimeout(90)
deadline = time.time() + 90
while time.time() < deadline:
    try:
        conn, _ = srv.accept()
    except socket.timeout:
        break
    handle(conn)
    break

for direction in ("c2s", "s2c"):
    lens = stats[direction]
    if lens:
        log(f"== {direction}: records={len(lens)} max={max(lens)}")
        over = sorted({x for x in lens if x > 4096})
        if over:
            log(f"== {direction}: lengths >4096 (uniq, asc): {over[:20]}")
out.close()
