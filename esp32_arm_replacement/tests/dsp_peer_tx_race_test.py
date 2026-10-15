"""Replay the supplied DSP's exact TX routines and parameter-ready branch.
Uses 16-bit C28x byte storage and an explicit element-copy shim on the host.
"""
from pathlib import Path
import re
import subprocess

root=Path(__file__).resolve().parents[1]
source_path=Path(r'C:\Users\29538\Documents\xwechat_files\wxid_6n4stl4q5dzq22_9518\msg\file\2026-10\ArmComm.c')
if (root/'tmp/ArmComm.c').exists():source_path=root/'tmp/ArmComm.c'
source=source_path.read_text(encoding='utf-8-sig')
def extract(start):
    match=re.search(r'^'+re.escape(start),source,re.M) if start.startswith('void ') else None
    begin=match.start() if match else source.index(start)
    brace=source.index('{',begin);end=brace+1;depth=1
    while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[begin:end]
ready=extract('if(RecvParam00 && RecvParam01 && RecvParam02)')
begin=source.index('else{',source.index(ready)+len(ready));end=source.index('if(QueryParamDelay>0)',begin)
ready+=source[begin:end]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef uint16_t Uint16;
typedef uint16_t uint8_T;
static uint8_T ScibTxBuf[100];
static Uint16 ScibTxCnt,ScibTxlength;
static uint16_t RecvParam00,RecvParam01,RecvParam02,QueryParamDelay,GetArmParamReady,ArmCommLostCnt,ArmComErr;
static struct { uint16_t SCITXBUF; struct{struct{unsigned TXFFIENA,TXFFINTCLR;}bit;} SCIFFTX; } ScibRegs;
static struct {struct{unsigned all;}PIEACK;} PieCtrlRegs;
static void c28x_copy(void *dest,const void *src,size_t n){uint16_t *d=dest;const uint16_t *s=src;for(size_t i=0;i<n;i++)d[i]=s[i];}
#define memcpy c28x_copy
static uint16_t calculate_CRCmodbus16(const uint16_t *p,size_t n){uint16_t c=65535;for(size_t i=0;i<n;i++){c^=p[i];for(unsigned j=0;j<8;j++)c=c&1?(c>>1)^0xa001:c>>1;}return c;}
'''+extract('void scibTxIsr(void)')+'\n'+extract('void ScibSendData(')+'\n'+r'''
static void ready_branch(void){uint16_t TxBuf[100]={0};
'''+ready+r'''
}
int main(void){
    for(unsigned len=18;len<=65;len+=47){
        uint16_t reply[100]={1,0,0};reply[1]=len;reply[2]=len==18?3:4;
        RecvParam00=RecvParam01=RecvParam02=QueryParamDelay=0;ScibTxCnt=0;
        ScibSendData(reply,len+4);
        uint16_t wire[5];for(unsigned i=0;i<2;i++){scibTxIsr();wire[i]=ScibRegs.SCITXBUF;}
        ready_branch(); /* actual second send overwrites shared buffer */
        for(unsigned i=2;i<5;i++){scibTxIsr();wire[i]=ScibRegs.SCITXBUF;}
        assert(wire[0]==1 && wire[1]==len && wire[2]==0 && wire[3]==0x21 && wire[4]==0x90);
        assert(calculate_CRCmodbus16(wire,3)!=0x9021);
    }
    RecvParam00=RecvParam01=RecvParam02=1;QueryParamDelay=0;ScibTxCnt=0;
    uint16_t reply[]={1,18,3,0,0};ScibSendData(reply,5);ready_branch();
    assert(GetArmParamReady==1 && ScibTxBuf[1]==18 && ScibTxBuf[2]==3);
    puts("PASS supplied DSP exact TX routines: two-byte interleaving reproduces 01 12/41 00 21 90; all three params ready prevents overwrite branch (host scheduling model, not physical proof)");
}
'''
test=root/'tmp/dsp_peer_tx_race_test.c';test.write_text(code,encoding='utf-8')
gcc=r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe'
exe=root/'tmp/dsp_peer_tx_race_test.exe'
subprocess.run([gcc,'-std=c11','-Wall','-Wextra','-Werror',str(test),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
