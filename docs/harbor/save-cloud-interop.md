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

## Expanded size and verification hashes

The next native attempt accepted the account binding and initial metadata, started download,
and fetched the first data chunk. FS returned 2002-3005 (out of range). The field at initial-data
offset 0x1018 is the total **uncompressed save-image size**. Harbor had stored the sum of the
compressed component sizes. Native FS uses the expanded size for its division calculator;
the owned firmware's importer at 0xc4e90 reads it before constructing chunk ranges. The native
Dinkum archive independently contains 63,225,856 in that field and only 276,488 compressed
chunk bytes. Harbor now records its actual concatenated source-image length there.

ChunkHashes are not SHA-256 of each expanded plaintext chunk. The owned firmware's 0xca850
maps the chunk's byte range into its verification storage and 0xc2370 hashes that range. The
thumbnail, control, allocation table, and padding contribute their ordinary bytes. For raw
save data, each 0x4000-byte block contributes its 32-byte IVFC leaf hash. Allocated blocks use
SHA-256(salt || block), with bit 255 set; free blocks contribute zero hashes. The salt is
HMAC-SHA256 with the public L3 domain string as its key and the save's integrity seed as its
message. Two additional 4-byte values at 0x1060 + 8*n are the truncated SHA-256 hashes of the
first and second halves of the same verification range.

All nine full hashes and both halves of every chunk in the owner's native Dinkum archive
matched this implementation. The updated ArchiveReader also authenticated, inflated and
verified that complete native archive. A fixed public fixture additionally checks the distinct
handling of an allocated zero block and free blocks, native writer/reader operation, and
rejection of incorrect short hashes, including a chunk that contains only prefix storages.
Older Harbor archives with plaintext chunk hashes remain readable and usable as delta bases;
new metadata records native hashes even when reusing their encrypted chunks.

Corrected Splatoon revision367 preserves the original save chunks and includes the native
size, account binding, full hashes and short hashes. Its freshly downloaded metadata, GCM
authentication, KSP MAC and component digests all verified offline. The owner subsequently confirmed that revision367 downloaded completely on the real Switch.
