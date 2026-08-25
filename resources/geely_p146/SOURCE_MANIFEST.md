# Geely P146 GEEA2.0 source manifest

## Requirement source

- Public-share archive: `\\jdatasrv\测试部公共盘\01_软件测试组\02_项目管理\客户项目测试工程\吉利\P146\02-刷写工程\GEEA2.0_SWDLonCAN_V7.6.zip`
- Archive SHA-256: `C59AF6CE269E767E17D0B64F034D65D613D0EC779073E8200007EC7C3512384D`
- Audited on: 2026-08-25

## Evidence anchors

- `GEEA2.0_SWDLonCAN_TestProject.cfg`: `20E8D016D8922CE6EFC1FFDB1E13A544CCB8DE4184EEDA721CD7B0F5F6480892`
- `04_TestModule/SWDL_CAN2.cbf`: `47237D7B535CED3D411EBB8DBA2453D6FFD50DFA8184F6C3A52C3E92AB12DD1D`
- `04_TestModule/SWDL_CAN2.dbg`: `DCA963D132111B4AA86E2C643F755A049C5847321C74C180C353789738359303`
- `04_TestModule/SWDL.xml`: `99C4BF70C460BAB8AF8DBD9013D38C970C2300C1F1DE2CA11B10D29DB3FE338C`
- `07_TestPanel/FBLpanel.xvp`: `19B68101C9CE6177E2B103BF886636C2F034F992D75964984136F9E60F74DDA3`

## Implemented contract

The dedicated P146 Profile selects the shared, project-neutral GEEA2.0 normal
Download engine. The engine reproduces the source sequence: `10 03`,
programming-precondition `31 01 02 06`, `10 02`, configurable `27`, optional
SBL `34/36/37 + 31 01 02 12 + 31 01 03 01`, per-image
`31 01 FF 00 + 34/36/37 + 31 01 02 12`, final `31 01 02 05`, `19 02 08`, and
`11 01`. Addresses, lengths, data-format identifier, erase ranges, call address,
ECU address and development/production signatures are read from selected VBF
files rather than copied from another Geely project.
The Profile's `vbf_signature_policy` selects `auto`, `development` or
`production`; `auto` keeps backward compatibility by preferring development
and falling back to production.

## Evidence boundary

The archive is a generic GEEA2.0 SWDL framework. It does not freeze the P146
target ECU's physical diagnostic IDs, SeedKey DLL/variant, firmware set,
bench-power procedure, or final version-acceptance values. Those inputs remain
editable and must be confirmed against the selected P146 ECU. Offline tests do
not constitute a CAN/ECU flash PASS.
