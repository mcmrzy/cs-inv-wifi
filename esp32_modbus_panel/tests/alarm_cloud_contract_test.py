"""Compile local server's real V1 validators against firmware-generated frames.
No server checkout is changed, no network connection is made.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys

root=Path(__file__).resolve().parents[1]
server=Path(sys.argv[1]) if len(sys.argv)>1 else Path('D:/CS_APP_PROJECT/cs_inv_monitor-develop/cs_inv_monitor-develop/business-api/internal/handler/protocol_ingest_v1.go')
text=server.read_text(encoding='utf-8')
def extract(prefix):
    start=text.index(prefix);opening=text.index('{',start);depth=1;end=opening+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
parts=[extract(p) for p in ['type protocolV1Envelope struct','type alarmV1Data struct','func decodeStrictReader(','func decodeStrictJSON(','func validateAlarmV1(']]
assert 'if envelope.V != 1' in text and 'if envelope.T <= 0' in text
folder=root/'tmp/alarm_cloud_contract';folder.mkdir(exist_ok=True)
(folder/'go.mod').write_text('module local.alarm.contract\n\ngo 1.22\n',encoding='utf-8')
(folder/'contract.go').write_text('package contract\nimport("bytes";"encoding/json";"errors";"io")\n'+'\n\n'.join(parts),encoding='utf-8')
shutil.copyfile(root/'tmp/alarm_contract.jsonl',folder/'firmware.jsonl')
(folder/'contract_test.go').write_text(r'''
package contract
import("bufio";"encoding/json";"os";"strings";"testing")
func accept(b []byte) bool {var e protocolV1Envelope;if decodeStrictJSON(b,&e)!=nil || e.V!=1 || e.T<=0{return false};var d alarmV1Data;return decodeStrictJSON(e.Data,&d)==nil && validateAlarmV1(d)==nil}
func TestFirmwareFrames(t *testing.T){
 f,err:=os.Open("firmware.jsonl");if err!=nil{t.Fatal(err)};defer f.Close();s:=bufio.NewScanner(f);count:=0
 for s.Scan(){b:=s.Bytes();if !accept(b){t.Fatalf("rejected firmware: %s",b)};var e protocolV1Envelope;json.Unmarshal(b,&e);var d alarmV1Data;json.Unmarshal(e.Data,&d)
  index:=count/2;code:=((0x46+index/16)<<8)|(index%16);source:=0;level:=1
  if index<32{level=2};if index>=64 && index<67{code=0x4a00+index-64;source=2;if index==64{level=2}}
  if index>=67{bit:=index-67;code=((0x56+bit/16)<<8)|(bit%16);if bit<32{level=2}}
  if d.Code!=code || d.Source!=source || d.Level!=level || d.State!=count%2{t.Fatalf("wrong code/bit/state: %s",b)}
  if accept([]byte(strings.Replace(string(b),`"v":1`,`"v":2`,1))){t.Fatal("accepted v2")}
  if accept([]byte(strings.Replace(string(b),`"state":`, `"extra":1,"state":`,1))){t.Fatal("accepted extra field")};count++
 };if s.Err()!=nil{t.Fatal(s.Err())};if count!=262{t.Fatalf("expected all 131 codes active/recovered, got %d",count)}
 for _,b:=range []string{`{"t":0,"v":1,"data":{}}`,`{"t":1,"v":1,"data":{"source":4,"code":1,"level":2,"state":1}}`,`{"t":1,"v":1,"data":{"source":0,"code":-1,"level":2,"state":1}}`,`{"t":1,"v":1,"data":{"source":0,"code":1,"level":3,"state":1}}`,`{"t":1,"v":1,"data":{"source":0,"code":1,"level":2,"state":2}}`}{if accept([]byte(b)){t.Fatalf("accepted invalid %s",b)}}
}
''',encoding='utf-8')
env=dict(os.environ,GOWORK='off',GOPROXY='off',GOTOOLCHAIN='local',GOCACHE=str(root/'tmp/go_cache'),GOTMPDIR=str(root/'tmp/go_tmp'))
Path(env['GOTMPDIR']).mkdir(exist_ok=True)
result=subprocess.run(['C:/Program Files/Go/bin/go.exe','test','-v','-count=1','.'],cwd=folder,env=env,text=True,capture_output=True)
print(result.stdout,end='');print(result.stderr,end='');result.check_returncode()
report={'server_source':str(server),'sha256':hashlib.sha256(server.read_bytes()).hexdigest(),'firmware_frames':262,'scope':'local strict V1 validators only; no broker, database, deployment or App acceptance'}
(root/'tmp/alarm_cloud_contract.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print('PASS alarm cloud contract: 262 firmware frames accepted by the V1 server validators; includes independent 3KU active/recovery codes')
