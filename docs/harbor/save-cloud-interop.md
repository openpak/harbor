# Save Data Cloud account binding

2026-10-11. Native Switch testing identified a Harbor archive metadata error.

Initial data's 32-byte AAD at plaintext offset 0x1020 holds the network service account ID,
archive ID, and two zero u64 values. The first value is the account sent in X-NSA-ID. Harbor
previously wrote the application ID there, allowing Harbor-to-Harbor transfers but causing
native OLSC to return 2179-3202 before requesting save chunks. Delta uploads also read and
check their base archive's AAD, so an incompatible Harbor backup can block a native upload.

Protocol evidence comes from the owner's firmware 23.0.1 OLSC: account loading at 0x1a90c0
writes the network account ID into the task's authentication record; download validation
at 0x20d0c0 compares the import AAD against that record and the archive ID. A Dinkum full
backup uploaded by the owner's real Switch independently confirmed the account binding.
The first field differs from the application ID, and the second equals the assigned archive
ID. Its metadata GCM authentication and initial-data CMAC both verified offline. No firmware
bytes, account identifiers or cryptographic keys are stored in this document or its tests.
Earlier research notes that identified the first AAD value as an application ID were wrong.

The transfer adapter now obtains the network account ID from its authenticated user session
and writes it on full and delta uploads. Reads accept both that native binding and the old
Harbor application-ID binding, with the same archive-ID check, so existing Harbor backups
remain usable as downloads and delta bases. A new delta emits the native account binding.
An unrelated account binding is rejected.

The core-free transfer self-check covers the emitted metadata fields, rejection of a different
account, legacy Harbor downloads, and delta upload/download against a legacy base. The
production session adapter is also compiled. Native full upload of Dinkum completed after
quarantining the incompatible Harbor revisions, whose blobs were retained. Dinkum subsequently
showed in sync, so a native download of that backup was not independently tested. The corrected
Splatoon metadata revision requires a separate native download acceptance check; metadata
acceptance alone does not prove save-image import compatibility.
