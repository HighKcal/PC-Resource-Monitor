# SQLite 3.54.1 amalgamation

Official download checked and archive downloaded on 2026-10-26:

- Download page: https://www.sqlite.org/download.html
- Archive: https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
- Archive SHA3-256, independently calculated with Python `hashlib.sha3_256` and matched to the official download page:
  `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`
- Public vendored files are unmodified `sqlite3.c` and `sqlite3.h`, compared byte for byte with the archive. The unused extension header and original ZIP are retained locally and excluded from Git; a fresh clone builds without either.
- Header `SQLITE_VERSION` is `3.54.1`; build statically compiles the source, without a system SQLite DLL dependency.
- Build options: `SQLITE_THREADSAFE=1`, `SQLITE_DQS=0`, `SQLITE_DEFAULT_MEMSTATUS=0`, `SQLITE_ENABLE_DBSTAT_VTAB`; vendor compilation warnings isolated from product `/W4 /WX`. DBSTAT is used only by optional `--stats` diagnostics.
- WAL-reset fixed versions: https://www.sqlite.org/wal.html section 11 states 3.51.3 and later; 3.54.1 meets this condition.
- License/public-domain statement: https://www.sqlite.org/copyright.html . SQLite source header contains the upstream blessing; it is preserved.

| File | SHA256 |
|---|---|
| sqlite3.c | `0c657666d819f261cc4007526ad57e1669de9949df7a8ea847aa284a2bf0ee52` |
| sqlite3.h | `d6c84557261893439b341d2e6c35187ae3ea24fbfd843ad94616f72d89242b8c` |

Run `python tests/review-monitor/verify_vendor.py` to verify the pinned sources offline. If the original ZIP is present, it also verifies the official SHA3-256 and archive contents. The build and verifier perform no network download.

SQLite's public-domain conditions apply to the vendored SQLite deliverables. They do not choose a license for this project's own source. This is version pinning and archive integrity verification, not a security audit.
