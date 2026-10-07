# Network-entry profile sync

Boundary: the D-Code sync task (outer network task 3) and the own-profile helpers it drives.
RE and design: `docs/Research/ProfileResetPromptInvestigation.md`. Rewrite:
`src/Network/ProfileSync.cpp`. Addresses are VAs at image base 0x00400000.

Functions the rewrite calls instead of replacing are L2 by design: they are shared with the
rest of the network code and behave correctly.

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x004B0970 | ProfileSyncTask | 1975 | L3 | Whole task, every state; fastcall(task). Single caller 0x4A70BF. |
| 0x004A96D0 | UploadOwnProfile | 162 | L2 | Guarded at entry by the rewrite (upload guard); returns 0 when latched. |
| 0x0049A940 | OwnProfileFetchPoll | 32 | L2 | Tick + poll: -100 pending, 0 ok, 100 failed. |
| 0x004A25C0 | DCodeFetchTick | 210 | L2 | Loader tick; any failure -> cc 6. Shared with room rows. |
| 0x004A0B80 | ProfileLoaderPoll | 42 | L2 | cc 3/4 -> 0, cc 6 -> 100, else -100. |
| 0x004A1DD0 | DCodePayloadValidator | 150 | L2 | Checksum + post-load fix-ups, sets cc 3. |
| 0x004A0CF0 | ProfileLoaderReset | 93 | L2 | Blob reset + loader cc 0. |
| 0x004A0D50 | DCodeBlobReset | 105 | L2 | memset 0x6800 + defaults. |
| 0x004A1F70 | ProfileBuildDefault | 31 | L2 | Reset + cc 4. |
| 0x0049A230 | OwnProfileFetchStart | 179 | L2 | Arms the download (cc 1). |
| 0x004A2740 | (seal with save time) | 133 | L2 | Writes +0x08, the +0x0C save time, then seals. |
| 0x0040DF10 | ProfileChecksum16 | 56 | L2 | Ones'-complement sum, valid iff 0xFFFF. |
| 0x0040DEC0 | ProfileChecksumSeal | 68 | L2 | Writes stamp and checksum. |
| 0x004B8D30 | OwnProfileUploadPoll | 31 | L2 | thiscall(transfer, blob), ret 4. |
| 0x004A9D20 | NetTaskClearRequests | 138 | L2 | Clears task +0x24..+0xA8. |
| 0x004A7F90 | NetTaskDialogBusy | 27 | L2 | +0xAC or +0xB8 set. |
| 0x004A7C30 | NetTaskSwitch | 36 | L2 | thiscall(task, id), ret 4. |
| 0x00778FE0 | (file exists) | 51 | L2 | Used by the dead Save/bbdc.dat path. |
| 0x00779020 | (bbdc.dat reader) | 395 | L2 | SHA-512 + RC4, SteamID key. Reimplemented in ProfileSync. |
| 0x007791B0 | (bbdc.dat writer) | 400 | L2 | Leaks 0x6800 per call. Reimplemented in ProfileSync. |
| 0x00778D00 | (SHA-512) | 243 | L2 | CryptoAPI. |
| 0x00778E00 | (RC4 decrypt) | 222 | L2 | CryptDeriveKey(CALG_RC4) over SHA-512. |
| 0x00778EE0 | (RC4 encrypt) | 223 | L2 | Same, encrypt. |
