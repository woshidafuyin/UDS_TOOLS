# Geely P417 flashing resources

P417 intentionally uses the same flashing workflow as P416 while keeping an
independent resource directory. The SBL and APP resources are the existing
successful-BLF reconstructions. The original imported `ESS/ess_out.VBF` is
used directly and is never rewritten by the tool or packaging script.

The ESS file is 1164 bytes and has SHA-256
`F85E2B378151A8E6630FEA3476311193F0E18108AA79ABF9CC2B25BDC2BA6C20`.
Its transferred blocks, DFI and signature match the successful P416 BLF ESS
transfer. The P417 profile points to its own copy under
`resources/geely_p417/ESS/ess_out.VBF`; no reconstructed ESS duplicate is
produced or required.

This is an offline resource and workflow parity statement, not a P417 ECU
bench acceptance result.
