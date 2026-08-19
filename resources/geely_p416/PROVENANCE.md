# Geely P416 flashing resources

The three packaged VBF files are reproducibly reconstructed from the exact
download payloads and 256-byte signatures present in the successful
`JL_P416APPtoAPP_Flashsoft_Log.blf` and
`JL_P416PLStoAPP_Flashsoft_Log.blf` traces.

They are **not supplier-original VBF releases**. The reconstruction script
requires every complete APP and PLS cycle to contain identical blocks and
signatures before it writes output. `manifest.json` records the source BLF and
output SHA-256 identities. The C++ parser also checks VBF file CRC32 and every
block CRC16 before opening CAN.

Bench acceptance is still required before this reconstructed workflow is used
for production flashing.

## Default supplier-original V1.2.00 inputs

The active `profiles/geely_p416.ini` defaults use the supplier-original P416
IRFM pair and ESS file from
`ARS1.31LC3A_G416FA(FR)_B1.0.00_APP_V1.2.00_CHF0378N(2026.05.20).zip` on the
P416 project share:

- `SBL/80048576AA.vbf` SHA-256
  `F67E4D8E95459C43AA0E398B6E3B20999A1275F0F8E59EA98B79D4FF24A464C4`;
- `APP/80078428AA.vbf` SHA-256
  `8A33B5BB4A95D279224BBBB766B9B49245A240E8E428D8EAF853D8229F727F4D`;
- `ESS/ess_out.VBF` SHA-256
  `F85E2B378151A8E6630FEA3476311193F0E18108AA79ABF9CC2B25BDC2BA6C20`.

The reconstructed resources remain packaged for trace-parity analysis. These
input files have only passed offline structural and hash checks; real-ECU
APP and FT acceptance remains required before production use.
