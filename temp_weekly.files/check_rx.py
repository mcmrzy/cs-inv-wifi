import serial, time
ser = serial.Serial('COM7', 115200, timeout=0.5)
end = time.time() + 8
buf = b''
while time.time() < end:
    d = ser.read(4096)
    if d:
        buf += d
ser.close()
text = buf.decode('utf-8', 'replace')
for line in text.splitlines():
    if ('init done' in line) or ('RX' in line) or ('decrypt' in line) or ('parse' in line):
        print(line)
