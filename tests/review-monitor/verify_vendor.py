"""Verify pinned sources offline; additionally check the archive if present."""
import hashlib
import json
from pathlib import Path
import zipfile

root = Path(__file__).resolve().parents[2]
vendor = root / 'third_party/sqlite'
expected = {
    'sqlite3.c': '0c657666d819f261cc4007526ad57e1669de9949df7a8ea847aa284a2bf0ee52',
    'sqlite3.h': 'd6c84557261893439b341d2e6c35187ae3ea24fbfd843ad94616f72d89242b8c',
}
archive_hash = '92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30'
report = {'source_sha256_matches': {}}
for name, digest in expected.items():
    report['source_sha256_matches'][name] = hashlib.sha256((vendor / name).read_bytes()).hexdigest() == digest
assert all(report['source_sha256_matches'].values()), report
archive = vendor / 'sqlite-amalgamation-3540100.zip'
if archive.exists():
    report['archive_sha3_256_matches'] = hashlib.sha3_256(archive.read_bytes()).hexdigest() == archive_hash
    assert report['archive_sha3_256_matches'], report
    with zipfile.ZipFile(archive) as z:
        report['files_equal_to_archive'] = {
            name: z.read('sqlite-amalgamation-3540100/' + name) == (vendor / name).read_bytes()
            for name in expected
        }
    assert all(report['files_equal_to_archive'].values()), report
else:
    report['archive_check'] = 'not present; pinned source files verified offline'
print(json.dumps(report, indent=2))
