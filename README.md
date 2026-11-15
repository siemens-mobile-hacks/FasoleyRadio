# Fasoley Radio

[English](README.en.md)

Онлайн-радио (MP3-потоки по HTTP/HTTPS) эльфом для BenQ-Siemens EL71 sw45 и Siemens S65 sw58 / SK65 sw50.
Интернет — любой: [bt-pan](https://github.com/siemens-mobile-hacks/bt-pan) по Bluetooth или GPRS.

- `FasoleyRadio_ELKA.elf` — EL71, `FasoleyRadio.elf` — S65 и SK65.
- Библиотека функций — та же, что для bt-pan.
- S65: патч `patches/s65/0_pcm_all_2024_10931_S65v58.vkp`. SK65: `patches/sk65/0_…` и `0b_…`. EL71 патчей не нужно.

Сборка: SDK в `../sdk`, `cd source && cmake -B build -S . && cmake --build build --target radio_SG radio_ELKA`.
