`Microsoft.PowerShell.Archive.cat` is an unchanged public catalog from the
Microsoft PowerShell 7.4.13 Linux x64 release, module
`Modules/Microsoft.PowerShell.Archive`. PowerShell is distributed under the MIT
license: https://github.com/PowerShell/PowerShell/blob/v7.4.13/LICENSE.txt
Release: https://github.com/PowerShell/PowerShell/releases/tag/v7.4.13

SHA-256: `80ff18a0f1da3c8de428effe7e8f70d3233be546a45d7195a19379aefde81c47`
Signer thumbprint: `711AF71DC4C4952C8ED65BB4BA06826ED3922A32`

This existing signed CTL exercises the catalog reader with an unrelated signer.
It is not a driver-package fixture or evidence of native Windows Driver Store
membership. The tests model DISM/CIM inventory at the OS boundary and use this
catalog for the collector's conservative scan of all catalog files. The existing
Sunshine/SudoVDA catalogs exercise actual pinned legacy driver signatures. No
keys or new certificates are generated and no trust stores are changed.
