"""Offline CANoe DLL ABI + CAN frame pump integration, synthetic ECU/key only."""
from pathlib import Path
import ctypes as C
import sys, time, shutil, tempfile
import win32crypt

root=Path(tempfile.mkdtemp(prefix='perodua_capl_test_'))
(root/'Exec64').mkdir();(root/'profiles').mkdir()
shutil.copy2(sys.argv[1],root/'Exec64/perodua_capl.dll')
shutil.copy2(Path(__file__).resolve().parents[1]/'profiles/perodua_p02c.ini',root/'profiles/perodua_p02c.ini')
(root/'image.bin').write_bytes(b'123456789')
(root/'test.key').write_bytes(b'CHKEY1'+win32crypt.CryptProtectData(bytes([0x33])*16,'test',None,None,None,1))
d=C.CDLL(str(root/'Exec64/perodua_capl.dll'))
d.p02Text.argtypes=[C.c_ulong,C.c_char_p]
d.p02Number.argtypes=[C.c_ulong,C.c_ulong]
d.p02Rx.argtypes=[C.c_ulong,C.c_ulong,C.POINTER(C.c_ubyte)]
d.p02Tx.argtypes=[C.c_ulong,C.POINTER(C.c_ubyte)]
d.p02Log.argtypes=[C.c_ulong,C.c_char_p]
buf=(C.c_ubyte*8)(); log=C.create_string_buffer(2048)
def text(i,s): assert d.p02Text(i,str(s).encode('mbcs'))==1
for i in range(3):text(i,root/'image.bin')
text(3,root/'test.key');text(4,'SHOP000001TESTER00000000001')
for i,v in enumerate([0,1,0x2000,0x10000,0x20000]):assert d.p02Number(i,v)==1
def push(id,payload):
 payload=payload+bytes([255])*(8-len(payload))
 d.p02Rx(id,8,(C.c_ubyte*8).from_buffer_copy(payload))
def logs():
 lines=[]
 while d.p02Log(2048,log):lines.append(log.value.decode('utf-8','replace'))
 return lines
try:
 for mode in range(3):
  d.p02Number(0,mode); assert d.p02Begin()==1
  incoming=bytearray();length=0; pending=b'';requests=[];functionals=[];data_image=bytearray()
  deadline=time.monotonic()+15
  while d.p02Status()==1 and time.monotonic()<deadline:
   id=d.p02Tx(8,buf)
   if not id:time.sleep(.0005);continue
   f=bytes(buf);d.p02Ack()
   if id==0x7df:functionals.append(f[1:1+f[0]]);continue
   assert id in (0x714,0x701)
   req=None;typ=f[0]>>4
   if typ==0:req=f[1:1+f[0]]
   elif typ==1:
    length=((f[0]&15)<<8)|f[1];incoming=bytearray(f[2:]);push(id+0x80,b'\x30\x00\x00')
   elif typ==2:
    incoming.extend(f[1:])
    if len(incoming)>=length:req=bytes(incoming[:length])
   elif typ==3:
    assert pending
    for seq,offset in enumerate(range(6,len(pending),7),1):push(id+0x80,bytes([0x20|(seq&15)])+pending[offset:offset+7])
    pending=b''
   if req is None:continue
   requests.append(req)
   if req==bytes.fromhex('22F191'):reply=bytes.fromhex('62F191435044')
   elif req==bytes.fromhex('31010203'):
    assert id==0x701;reply=bytes.fromhex('71010203')
   elif req==bytes.fromhex('1002'):reply=bytes.fromhex('5002003201F4')
   elif req==bytes.fromhex('2707'):reply=bytes.fromhex('6707')+bytes(16) # already unlocked
   elif req[:3]==bytes.fromhex('2EF107'):assert len(req)==33;reply=bytes.fromhex('6EF107')
   elif req[0]==0x34:
    assert req[1:3]==b'\x00\x44';data_image.clear();reply=bytes.fromhex('74200008')
   elif req[0]==0x36:data_image.extend(req[2:]);reply=bytes([0x76,req[1]])
   elif req[0]==0x37:assert data_image==b'123456789';reply=b'\x77'
   elif req[0]==0x31:
    if req[2:4]==bytes.fromhex('0202'):assert req[4:]==bytes.fromhex('CBF43926')
    push(id+0x80,bytes.fromhex('037F3178'))
    reply=bytes([0x71,1,req[2],req[3],0])
   elif req==bytes.fromhex('1101'):reply=bytes.fromhex('5101')
   else:raise AssertionError(req.hex())
   if len(reply)<=7:push(id+0x80,bytes([len(reply)])+reply)
   else:pending=reply;push(id+0x80,bytes([0x10|(len(reply)>>8),len(reply)&255])+reply[:6])
  result=d.p02Status();lines=logs();d.p02Stop()
  assert result==2,(result,lines[-8:])
  assert d.p02Progress()==100
  assert len([r for r in requests if r[:4]==bytes.fromhex('3101FF00')])==(2 if mode==2 else 1)
  assert functionals[-4:]==[bytes.fromhex(x) for x in ['1083','288003','8581','1081']]
  assert all(r[:3] not in [bytes.fromhex('22F107'),bytes.fromhex('22F186')] for r in requests)
  print('CAPL DLL CAN/ISO-TP mode',mode,'PASS',flush=True)
 d.p02Number(1,0);d.p02Begin()
 deadline=time.monotonic()+2
 while d.p02Status()==1 and time.monotonic()<deadline:time.sleep(.001)
 assert d.p02Status()==-1 and d.p02Tx(8,buf)==0
 d.p02Stop();d.p02Number(1,1);d.p02Begin();time.sleep(.01);d.p02Stop()
 assert d.p02Status()==-2 and d.p02Tx(8,buf)==0
 print('CAPL DLL missing parameters / stop / queue cleanup PASS')
finally:
 d.p02Stop()
 # The DLL remains loaded until this process exits; leave only this temp fixture.
 print('Synthetic fixture:',root)
