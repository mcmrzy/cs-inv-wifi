"""Import exact captured original ARM frames, without embedded defaults or passwords."""
import argparse
import getpass
import json
import re
import time
from pathlib import Path

def crc16(data):
    crc=0xffff
    for value in data:
        crc^=value
        for _ in range(8):crc=(crc>>1)^0xa001 if crc&1 else crc>>1
    return crc

def load_profile(path):
    profile=json.loads(Path(path).read_text(encoding='utf-8-sig'))
    sn=profile.get('sn','')
    if not re.fullmatch(r'[A-Za-z0-9]{16}',sn):raise ValueError('SN must be exactly 16 letters/digits')
    frames=[]
    for cmd,length in enumerate((25,23,16)):
        raw=profile.get(f'cmd{cmd:02}', '')
        if not isinstance(raw,str):raise ValueError('Frames must be hex strings')
        frame=bytes.fromhex(raw)
        if len(frame)!=length or frame[:3]!=bytes((1,length-4,cmd)) or crc16(frame[:-2])!=int.from_bytes(frame[-2:],'little'):
            raise ValueError(f'CMD{cmd:02} length/header/CRC mismatch')
        frames.append(frame)
    return sn,frames

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('profile',help='JSON with sn, cmd00, cmd01, cmd02 (exact original ARM TX frames)')
    parser.add_argument('--port',help='USB serial port, e.g. COM16')
    args=parser.parse_args();sn,frames=load_profile(args.profile)
    print(f'Validated captured frame lengths/CRC for {sn}: 25/23/16 bytes. Firmware also checks parameter ranges.')
    if not args.port:return
    import serial
    password=getpass.getpass('Existing device administrator password: ')
    if not password or any(c.isspace() for c in password) or not password.isascii():raise ValueError('Password cannot contain whitespace')
    command=f'set_dsp {password} {sn} '+b''.join(frames).hex()+'\n'
    if len(command)>256:raise ValueError('Administrator command exceeds device line limit')
    with serial.Serial(args.port,115200,timeout=0.5) as port:
        port.write(b'dsp_params\n');deadline=time.monotonic()+5;matched=False
        while time.monotonic()<deadline:
            line=port.readline().decode('utf-8',errors='replace')
            if line.startswith('[DSP_PARAMS] ready='):
                if f'sn={sn}' not in line:raise ValueError('Connected device SN differs from profile')
                matched=True;break
        if not matched:raise RuntimeError('No device identity response; check USB port and v0.6.12 firmware')
        port.write(command.encode('ascii'));command='';password=''
        deadline=time.monotonic()+10;ok=False
        while time.monotonic()<deadline:
            line=port.readline().decode('utf-8',errors='replace')
            if line.startswith('[SET_DSP]') or line.startswith('[SET_SN] ERROR'):
                print(line.strip());ok=line.startswith('[SET_DSP] OK ');break
        if not ok:raise RuntimeError('Device did not confirm durable import; no success assumed')
        port.write(b'dsp_params\n');deadline=time.monotonic()+5;readback={};durable=False
        while time.monotonic()<deadline and len(readback)<3:
            line=port.readline().decode('utf-8',errors='replace')
            if line.startswith('[DSP_PARAMS] ready='):durable='ready=1 durable=1 ' in line and f'sn={sn}' in line
            match=re.fullmatch(r'\[DSP_PARAMS\] cmd=(\d{2}) frame=([0-9A-F]+)\s*',line)
            if match:readback[int(match[1])]=bytes.fromhex(match[2])
        if not durable or any(readback.get(i)!=frame for i,frame in enumerate(frames)):
            raise RuntimeError('Imported record changed or failed exact readback; inspect DSP state before proceeding')
        print('PASS durable device readback of all three original frames; UART replies still require device acceptance.')

if __name__=='__main__':
    try:main()
    except (ValueError,RuntimeError) as error:raise SystemExit(str(error)) from None
