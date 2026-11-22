# Third-party notices

The Live Cube Recipes plugin builds in and statically links **CascLib**
(`third_party/CascLib/`), which reads the game's own button picture when the
mod has no copy of it. CascLib is kept unmodified. Only `src/`,
`CMakeLists.txt`, `LICENSE` and `README.txt` are included, and
`LICENSES/` holds the license texts of code that CascLib bundles. Every source
file keeps its original copyright header.

| Component | Files | Copyright | License |
|---|---|---|---|
| CascLib, commit 38a3466 (https://github.com/ladislav-zezula/CascLib) | `third_party/CascLib/` | (c) 2014 Ladislav Zezula | MIT, `third_party/CascLib/LICENSE` |
| zlib 1.2.11 | `src/zlib/` | (c) 1995-2017 Jean-loup Gailly and Mark Adler | zlib, `third_party/CascLib/LICENSES/zlib.txt` |
| AES (from OpenSSL) | `src/overwatch/aes*` | (c) 2002-2016 The OpenSSL Project Authors | OpenSSL / SSLeay, `third_party/CascLib/LICENSES/OpenSSL-SSLeay.txt` |
| SHA-1 | `src/hashes/sha1.*` | (c) 2003-2005 Jouni Malinen | BSD (chosen from GPL-2.0/BSD), `third_party/CascLib/LICENSES/sha1-BSD.txt` |
| MD5 | `src/hashes/md5.*` | Alexander Peslyak, 2001 | Public domain / permissive, terms in the file header |
| lookup3 hash | `src/jenkins/lookup3.c` | Bob Jenkins, 2006 | Public domain |

This product includes software developed by the OpenSSL Project for use in
the OpenSSL Toolkit (http://www.openssl.org/).

This product includes cryptographic software written by Eric Young
(eay@cryptsoft.com).

## Binary releases

`d2rl-live-cube-recipes.dll` contains all of the code above. Any release
folder with the DLL must also include this file and the license texts:
`third_party/CascLib/LICENSE` and `third_party/CascLib/LICENSES/`.

Diablo II: Resurrected and its data are the property of Blizzard
Entertainment. None of the game's files are included in this repository.
