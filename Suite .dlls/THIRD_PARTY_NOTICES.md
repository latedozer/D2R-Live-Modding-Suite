# Third-party notices

Three DLLs of this folder build in and statically link **CascLib**, which reads
the game's own data: `d2rl-live-cube-recipes.dll`, `d2rl-mod-book.dll` (the
game's button picture) and `d2rl-live-export.dll` (tables the mod has no copy
of). The source is in the repository at `Live Cube Recipes Mod/third_party/CascLib/`. CascLib is kept unmodified. Only `src/`,
`CMakeLists.txt`, `LICENSE` and `README.txt` are included, and
`LICENSES/` holds the license texts of code that CascLib bundles. Every source
file keeps its original copyright header.

| Component | Files | Copyright | License |
|---|---|---|---|
| CascLib, commit 38a3466 (https://github.com/ladislav-zezula/CascLib) | `CascLib/` | (c) 2014 Ladislav Zezula | MIT, `LICENSES/CascLib-MIT.txt` |
| zlib 1.2.11 | `src/zlib/` | (c) 1995-2017 Jean-loup Gailly and Mark Adler | zlib, `LICENSES/zlib.txt` |
| AES (from OpenSSL) | `src/overwatch/aes*` | (c) 2002-2016 The OpenSSL Project Authors | OpenSSL / SSLeay, `LICENSES/OpenSSL-SSLeay.txt` |
| SHA-1 | `src/hashes/sha1.*` | (c) 2003-2005 Jouni Malinen | BSD (chosen from GPL-2.0/BSD), `LICENSES/sha1-BSD.txt` |
| MD5 | `src/hashes/md5.*` | Alexander Peslyak, 2001 | Public domain / permissive, terms in the file header |
| lookup3 hash | `src/jenkins/lookup3.c` | Bob Jenkins, 2006 | Public domain |

This product includes software developed by the OpenSSL Project for use in
the OpenSSL Toolkit (http://www.openssl.org/).

This product includes cryptographic software written by Eric Young
(eay@cryptsoft.com).

## Binary releases

Those three DLLs contain all of the code above. The license texts are in
`LICENSES/` beside this file: `CascLib-MIT.txt` (CascLib's `LICENSE`),
`zlib.txt`, `OpenSSL-SSLeay.txt`, `sha1-BSD.txt`.

Diablo II: Resurrected and its data are the property of Blizzard
Entertainment. None of the game's files are included in this repository.
