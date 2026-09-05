"""Create the standalone P02C CANoe shell; no CANoe measurement is started."""
from pathlib import Path
import shutil
import xml.etree.ElementTree as ET
import re
import sys

repo=Path(__file__).resolve().parents[1]
root=Path(r'D:\project\136_PeroduaP02C_ARS1.33_30200\CANoe_Flash_P02C')
sample=Path(r'C:\Users\Public\Documents\Vector\CANoe\Sample Configurations 12.0.75\Programming\CAPLdll\EXAMPLE')
panel_ref=Path(r'D:\project\奇瑞\03_CANoe刷写工程\T22_T1EJ共用\Panel\FlashChery.xvp')
for d in ['CAPL','Panel','Database','Exec64','Exec32','profiles','Test','Reports','Source']:
 (root/d).mkdir(parents=True,exist_ok=True)
cfg=(sample/'CANoeCAPLdll.cfg').read_text(encoding='cp1252')
cfg=cfg.replace('dbc\\CAPLdll.dbc','Database\\P02C_Panel.dbc').replace('node\\capldll.can','CAPL\\P02C_Flash.can')
cfg=cfg.replace('Panels\\HelpCANoe.xvp','Panel\\P02C_Flash.xvp')
cfg=cfg.replace('Comment_CANoeCAPLdll.txt','README.txt')
cfg=cfg.replace('CAPLdll','P02C_Panel').replace('capldll','P02C_Flash')
cfg=re.sub(r'D:\\Demos[^\n]*\.cbf',str(root/'CAPL/P02C_Flash.cbf').replace('\\','/'),cfg)
cfg=cfg.replace('node\\P02C_Flash2.cbf','CAPL\\P02C_Flash.cbf').replace('node\\P02C_Flash.cbf','CAPL\\P02C_Flash.cbf')
python_example=sample.parents[1]/'Python/CANoeConfig'
test_cfg=(python_example/'PythonBasic.cfg').read_text(encoding='cp1252')
def object_block(s,name):
 a=s.index(name+' Begin_Of_Object');b=s.index('End_Of_Object '+name,a)+len('End_Of_Object '+name)
 return s[a:b]
test_box=object_block(test_cfg,'VTestSetupBox 2').replace('TestEnvironments\\Test Environment.tse','Test\\P02C_Flash.tse')
cfg=cfg.replace(object_block(cfg,'VTestSetupBox 2'),test_box)
if '--assets-only' not in sys.argv:
 native=repo/'canoe/perodua_p02c/Perodua_P02C_Flash.cfg'
 if native.exists(): shutil.copy2(native,root/native.name)
 else: (root/'Perodua_P02C_Flash.cfg').write_text(cfg,encoding='cp1252')
tse=(python_example/'TestEnvironments/Test Environment.tse').read_text(encoding='cp1252')
node=object_block(tse,'VTSProgrammedNode 2')
node=node.replace('..\\TestModules\\Test_Light.can','P02C_Test.can').replace('..\\TestModules\\Test_Light.cbf','P02C_Test.cbf')
node=re.sub(r'D:\\Demos[^\n]*\.cbf',str(root/'Test/P02C_Test.cbf').replace('\\','/'),node)
node=node.replace('HazardLight','P02C Flash').replace('..\\TestModules_Report\\Test_Light_report.vtestreport','..\\Reports\\P02C_Flash.vtestreport')
folder='''VTSFolder 2 Begin_Of_Object
1
VTSItem 3 Begin_Of_Object
1
3 1256878368 0
1
END_OF_ITEM_DATA
End_Of_Object VTSItem 3
4
P02C Flash
1
401543552
<VFileName V7 QL> 1 "..\\Reports\\P02C_Flash.xml"
END_OF_FILE_NAME
<VFileName V7 QL> 1 ""
END_OF_FILE_NAME
2
1
END_OF_FOLDER_DATA
End_Of_Object VTSFolder 2
1
1256878368
2
1
401543552
0
1
1256878368
1
401543552
End_Of_Object VTSPersistentRoot 1
'''
header=tse[:tse.index('VTSProgrammedNode 2')].replace('TS_PERSISTENT_ROOT_ID_STRING_0x792341fe\n1\n9','TS_PERSISTENT_ROOT_ID_STRING_0x792341fe\n1\n2')
if '--assets-only' not in sys.argv:
 native=repo/'canoe/perodua_p02c/P02C_Flash.tse'
 if native.exists(): shutil.copy2(native,root/'Test'/native.name)
 else: (root/'Test/P02C_Flash.tse').write_text(header+node+'\n'+folder,encoding='cp1252')
shutil.copy2(repo/'profiles/perodua_p02c.ini',root/'profiles/perodua_p02c.ini')

numeric={'Mode':(0,2,0),'CRC':(0,2,0),'Channel':(1,32,1),'DriverStart':(0,4294967295,0),
 'AppStart':(0,4294967295,0),'CalStart':(0,4294967295,0),'Start':(0,1,0),
 'Stop':(0,1,0),'Status':(-2,2,0),'Progress':(0,100,0),
 'TestOwner':(0,1,0),'TestHeartbeat':(0,1,0)}
strings=['DriverFile','AppFile','CalFile','KeyFile','TesterIdentity']
dbc='VERSION "P02C flash panel only - no vehicle messages"\n\nNS_ :\n\tVAL_\n\tEV_DATA_\n\nBS_:\n\nBU_: Vector__XXX\n\n'
for i,(name,(lo,hi,initial)) in enumerate(numeric.items(),1):
 dbc+=f'EV_ {name}: {1 if name.endswith("Start") and name!="Start" else 0} [{lo}|{hi}] "" {initial} {i} DUMMY_NODE_VECTOR0 Vector__XXX;\n'
for i,name in enumerate(strings,len(numeric)+1):
 dbc+=f'EV_ {name}: 0 [0|0] "" 0 {i} DUMMY_NODE_VECTOR8000 Vector__XXX;\n'
dbc+='VAL_ Mode 0 "APP" 1 "CAL" 2 "APP+CAL";\nVAL_ CRC 0 "Select ECU CRC" 1 "Reflected" 2 "Non-reflected";\n'
dbc+='VAL_ Status 0 "Idle" 1 "Flashing" 2 "PASS" -1 "FAIL" -2 "Stopped";\n'
(root/'Database/P02C_Panel.dbc').write_text(dbc,encoding='ascii')

reference=ET.parse(panel_ref if panel_ref.exists() else repo/'canoe/perodua_p02c/P02C_Flash.xvp')
panel=reference.getroot()
parent=panel.find('Object')
for obj in list(parent): parent.remove(obj)
parent.set('ControlName','Perodua P02C Flash')
version='Vector.CANalyzer.Panels.CommonControls, Version=12.0.75.0, Culture=neutral, PublicKeyToken=null'
def prop(obj,name,value): ET.SubElement(obj,'Property',Name=name).text=str(value)
def control(cls,name,x,y,w,h,text=None,variable=None,string=False,extras=None):
 o=ET.SubElement(parent,'Object',Type=f'Vector.CANalyzer.Panels.Design.{cls}, {version}',Name=name,Children='Controls',ControlName=name)
 for n,v in [('Name',name),('Location',f'{x}, {y}'),('Size',f'{w}, {h}')]: prop(o,n,v)
 if text is not None: prop(o,'Text',text)
 if variable: prop(o,'SymbolConfiguration',f'5;1;P02C_Panel;;;{variable};{4 if string else 1};;;-1;;;')
 if cls in ['TextBoxControl','ComboBoxControl']:prop(o,'DisplayLabel','Hide')
 for n,v in (extras or {}).items(): prop(o,n,v)
control('StaticTextControl','Title',20,15,750,30,'Perodua P02C / CPD ARS1.33 - Flash')
control('StaticTextControl','Bus',20,50,750,24,'CAN 500 kbit/s   CPD 714/794   Gateway 701/781   Functional 7DF')
for i,(var,label) in enumerate(zip(strings,['Driver','APP','CAL','OEM Key (blank = local key)','Shop code + tester serial'])):
 y=88+i*42
 control('StaticTextControl','Label'+var,20,y+4,200,24,label)
 control('TextBoxControl' if var=='TesterIdentity' else 'PathControl',var,225,y,545,28,variable=var,string=True)
control('StaticTextControl','ModeLabel',20,310,80,24,'Mode')
control('ComboBoxControl','Mode',105,306,180,28,variable='Mode',extras={'UsedValueTable':'PhysicalValue','ShowValues':'Decimal'})
control('StaticTextControl','CRCLabel',320,310,100,24,'ECU CRC32')
control('ComboBoxControl','CRC',425,306,230,28,variable='CRC',extras={'UsedValueTable':'PhysicalValue','ShowValues':'Decimal'})
control('StaticTextControl','ChannelLabel',665,310,55,24,'CAN')
control('TextBoxControl','Channel',722,306,48,28,variable='Channel',extras={'ValueDecimalPlaces':'0'})
control('StaticTextControl','BinInfo',20,349,750,24,'BIN start addresses (decimal); S-record uses file segment addresses.')
for i,(var,label) in enumerate([('DriverStart','Driver'),('AppStart','APP'),('CalStart','CAL')]):
 x=20+i*250
 control('StaticTextControl',var+'Label',x,386,60,24,label)
 control('TextBoxControl',var,x+65,382,170,28,variable=var,extras={'ValueDisplay':'Decimal','ValueDecimalPlaces':'0'})
control('ButtonControl','Start',20,430,160,36,'Start flash',variable='Start',extras={'PressValueVT':'1;Lower;1','ReleaseValueVT':'1;Lower;0'})
control('ButtonControl','Stop',195,430,120,36,'Stop',variable='Stop',extras={'PressValueVT':'1;Lower;1','ReleaseValueVT':'1;Lower;0'})
control('TextBoxControl','Status',335,433,160,28,variable='Status',extras={'DisplayOnly':'True','ValueDisplay':'Symbolic','UsedValueTable':'PhysicalValue'})
control('ProgressBarControl','Progress',515,430,255,36,variable='Progress')
control('StaticTextControl','Help',20,485,750,50,'Steps and errors: CANoe Write / Trace. Test Module reports: Reports folder.')
prop(parent,'Name','Panel');prop(parent,'Size','800, 545');prop(parent,'BackColor','White')
ET.indent(panel)
ET.ElementTree(panel).write(root/'Panel/P02C_Flash.xvp',encoding='utf-8',xml_declaration=True)

for name in ['P02C_Flash.can','P02C_Test.can']:
 shutil.copy2(repo/'canoe/perodua_p02c'/name,root/('CAPL' if name=='P02C_Flash.can' else 'Test')/name)
print(root)
