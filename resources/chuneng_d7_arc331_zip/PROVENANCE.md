# ChuNeng ARC331 resource provenance

The active ARC331 Profile uses one atomic Driver/APP CBF pair. The Workflow
parses and validates both containers before creating a CAN provider. A job may
alternatively use a complete S-record set (Driver S19/SREC + Driver
`*_Ver.asc` + Driver `*_ABT.asc`, and the corresponding three APP files), but
it may not mix CBF and S-record roles or sidecars from different packages.

Both input modes feed the same ChuNeng state machine and the same fixed
download windows. Neither mode enters the LP `6000/6001` plus 1322-byte
certificate flow, and neither mode sends `0x771`.

## Active Driver CBF

- Source: `D:\project\cbf\Driver\driver 712345678AB.cbf`
- Packaged path: `CBF/Driver/driver_712345678AB.cbf`
- CBF SHA-256:
  `A3D4B9A5323FDA405400712FA5E46D8CD0CB1D9AD218AEBCC35008716FB933C3`
- Header role: `sw_type=SBL`.
- Main source window: `0x10280000/0x00004000`.
- ECU download window: `0x00000000/0x00004000`.
- Main SHA-256:
  `102D8AAC81DC2AA5C77A89682CD80F81C353A6F520D2CE8901C6B75B2D0C9142`
- ABT: address `0x000C0000`, length `0x2C`.
- The Workflow extracts the 256-byte `dev_signature` from this same CBF for
  Driver `31 01 02 02` verification.

The main payload contains the identifier
`FAKE_CN2944_FLASH_DRIVER_RAW_0x4000`. For this project the user has confirmed
that this is a flashable Driver payload, so the runtime and packaging checks do
not reject a CBF based on a `FAKE_` filename or byte prefix. Container
integrity, role, address, length, ABT and signature checks still apply.

## Active APP CBF

- Source: `D:\project\cbf\APP\7052A5023002AB (2).cbf`
- Packaged path: `CBF/APP/7052A5023002AB.cbf`
- CBF SHA-256:
  `3EEF5C26084570BD9B1E7C5430025A2A5ED307AEE955793BCC35CC05C8278205`
- Header identity: `sw_id=7052A5023002`, `sw_version=AB`, `sw_type=APP`.
- Main/download window: `0x000C0000/0x00180000`.
- Main SHA-256:
  `24A661A59A55652C15983EA73DCDB98A9F3733CB7378953AE834D164ACAEBD7A`
- ABT: address `0x000C0000`, length `0x2C`.
- The Workflow extracts the 256-byte `dev_signature` from this same CBF for
  APP `31 01 02 02` verification.

The independently generated APP S19 reference has SHA-256
`1492CAFECEE8715F23DCF0E4E5C1B549C4414DFCB27BB02DE7A123EC06AE1AAE`.
It is packaged under `S19/` with the CBF-derived Driver S19 and both roles'
ABT/signature sidecars. In S-record mode the Workflow derives `Driver_ABT.asc`
from selected `Driver_Ver.asc` (and likewise for APP), validates the ABT
address, length and SHA-256 against the selected S19 before CAN is opened, and
then downloads the ABT before each `0202` verification. The default Profile
continues to select the simpler two-file CBF pair.

## CBF parser contract

For each CBF the common parser checks version and required Header fields,
`sw_type`, raw data format, main and ABT block boundaries, both block CRC16
values, aggregate CRC32, ABT Header values, ABT SHA-256, the ABT-to-main
address/length/SHA-256 mapping, and an exactly 256-byte `dev_signature`.
Driver and APP project roles and fixed windows are then checked by the
Workflow. Passing these offline checks does not by itself prove ECU signature
acceptance or a completed bench flash.

## Security set

- CANoe reference package:
  `D:\project\楚能_D7_ARC3.31_30186_刷写规范_20260813\LP_ARC331.zip`
- Archive SHA-256:
  `76154EC215390AD085002D4F027954FDB713726FAAD9107A2E8BEA177C871037`
- SeedKey DLL: `dll/ChuNeng_D7_SeednKey_V1.0.dll`
- SHA-256:
  `DF4FEBCC26FAE799F9010B101EEDAB09A02CE1BEB866F2292F658B4AA76214FA`
- Security CDD: `Reference/LeapMotor_UDS27_SeedKey_HexDumpVar.cdd`
- SHA-256:
  `9643DE7ABB0C339B236BDE03D12A7A3A57482A77173B13EFA76E5DB4A61674F4`
- Runtime contract: `27 11`, 16-byte Seed, level `0x11`, 16-byte Key,
  `27 12`. The runtime tool calls `GenerateKeyEx` through the x86 broker.
