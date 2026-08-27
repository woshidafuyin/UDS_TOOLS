# B01 historical evidence retained under the unified LP-ARF 631 entry

- CANoe source: `D:\project\CANoe刷写验收工程\09_零跑LP_ARF2.31_B01_CANoe历史Passed\工程本体`.
- Passing evidence: `Reference/Flash_report0011.xml`; reports 0001 through 0010 failed.
- Historical passing entry: APP to APP, `0x751 -> 0x759`, functional `0x7DF`.
- APP window: `0x000C0000 / 0x00180000`; no Driver phase was present in the passing report.
- The packaged 1322-byte certificate was reconstructed exactly from the ISO-TP payload of `31 01 60 00` in report 0011. Its first 32 bytes are the accepted APP SHA-256.
- No matching B01 APP source file was present in the frozen CANoe project. The independent B01 profile was retired when the two identical APP protocol bodies were merged into `profiles/lp_arf.ini`. To flash a B01 package, select its project-appropriate APP and `Verification/B01_ARF2.31_PASS0011_certificate.asc`; matching the CANoe baseline, the workflow sends the 1322-byte certificate as-is and treats ECU routine `31 01 60 00` as the authoritative binding verdict before erase/download.
- The SeedKey DLL is the frozen Leapmotor DLL already used by LP-ARC/ARF6.31. It was independently checked against the B01 report vector `7C05ADF8 -> EFBE04B5` at level `0x11`, variant `lingpao`.
- The unified 631 entry retains both APP-to-APP and PLS-to-APP. The B01 report proves only APP-to-APP; it does not independently prove the PLS path on a B01 ECU. Historical CANoe PASS is provenance evidence, not C++ real-ECU acceptance.
