# Fasoley Radio

[Русский](README.md)

Online radio (MP3 streams over HTTP/HTTPS) as an elf for BenQ-Siemens EL71 sw45 and Siemens S65 sw58 / SK65 sw50.
Any internet works: [bt-pan](https://github.com/siemens-mobile-hacks/bt-pan) over Bluetooth, or GPRS.

- `FasoleyRadio_ELKA.elf` — EL71, `FasoleyRadio.elf` — S65 and SK65.
- Function library — the same as for bt-pan.
- S65: patch `patches/s65/0_pcm_all_2024_10931_S65v58.vkp`. SK65: `patches/sk65/0_…` and `0b_…`. EL71 needs no patches.

Build: SDK in `../sdk`, `cd source && cmake -B build -S . && cmake --build build --target radio_SG radio_ELKA`.
