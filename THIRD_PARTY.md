# Third-party components

| Component | Version / origin | License | Files |
|---|---|---|---|
| cpp-httplib | v0.61.0, yhirose/cpp-httplib, commit c419c904da4ed0d057f169c225c58cc795c07e4d | MIT | vendor/httplib.h, vendor/httplib-LICENSE |
| nlohmann/json | v3.12.0 | MIT | vendor/json.hpp, vendor/json-LICENSE |
| SQLite | 3.54.1, official 2026 amalgamation | Public domain | vendor/sqlite3.c, vendor/sqlite3.h (public-domain declaration included in file headers) |
| serial-frame | C99 core from sha11owK1te/serial-frame, GitHub commit 669a629b8ddcf6eb427a1a361d55d9a14ed9acc9 | MIT | vendor/serial-frame/ |

Dependency file SHA-256 hashes and upstream URLs are recorded in `vendor/dependencies.json`.
The SQLite source archive SHA3-256 was verified against the official download page:
`92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`.

The C++ service, UI, simulator and integration tests are application code written for this project.
No third-party component's implementation is represented as original project work.
