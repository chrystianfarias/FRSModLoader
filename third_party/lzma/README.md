# LZMA decoder

`LzmaDec.c`/`.h` and the headers they need, from the LZMA SDK by Igor Pavlov
(public domain), taken from the official 7-Zip repository
(https://github.com/ip7z/7zip, `C/`). Only the decoder: `GameTextures` uses it
to read `.dds` files straight out of a zip that a mod keeps in its folder,
whose entries 7-Zip compressed with LZMA (zip method 14).
