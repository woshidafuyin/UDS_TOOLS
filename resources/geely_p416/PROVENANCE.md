# Geely P416 flashing resources

The SBL and APP VBF files are reproducibly reconstructed from the exact
download payloads and 256-byte signatures in the successful P416 BLF traces.
The ESS resource is different: the original imported `ESS/ess_out.VBF` is used
directly and is never rewritten by the packaging or reconstruction script.

The ESS file is 1164 bytes and has SHA-256
`F85E2B378151A8E6630FEA3476311193F0E18108AA79ABF9CC2B25BDC2BA6C20`.
Its two transferred blocks, DFI and 256-byte signature match the successful
BLF ESS transfer. The active P416 profile therefore points directly to this
file. No `P416_ESS_reconstructed.vbf` is produced or required.

The flashing workflow itself is unchanged: the same P416 UDS sequence,
RequestDownload/TransferData/RequestTransferExit operations and signature
verification remain in use. The C++ parser checks VBF CRC32 and every block
CRC16 before opening CAN.

Real-ECU acceptance is still required; offline identity and parser tests do
not constitute a bench PASS.
