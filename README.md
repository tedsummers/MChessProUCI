# MChessProUCI

**MChessProUCI** is a standalone 32-bit UCI (Universal Chess Interface) wrapper for **MChess Pro v3.5**, running the original 16-bit DOS chess engine and its embedded opening book data inside a self-contained 80186 real-mode emulator with zero external runtime dependencies.

## Credits

- **Original Program Author**: **Marty Hirsch** — creator of *MChess* and *MChess Pro*.
- All chess search, evaluation, and opening book logic (`PRBOOK.M35` / `M35KEY.000`–`M35KEY.00F`) come from MChess Pro v3.5. This project provides the 32-bit 80186/DOS/Unreal-Mode execution layer, native C ports of the piece-evaluation routines ([mcpro_eval.h](mcpro_eval.h)), and UCI protocol translation.

## Features

- Standalone 32-bit binaries for **Linux** (`MChessProUCI`) and **Windows** (`MChessProUCI.exe`)
- Embedded opening book (`PRBOOK.M35` and `M35KEY.000`–`M35KEY.00F`) with no external files required
- Unreal-Mode 16 MB extended memory support for transposition/hash tables (up to 10 MB)
- All MChess Pro v3.5 engine options exposed as standard UCI options:
  - `Hash` (`spin`, 1–10 MB, default `10`)
  - `OwnBook` (`check`, default `true`)
  - `Ponder` (`check`, default `false`)
  - `PlayingStyle` (`combo`: `Cautious`, `Normal`, `Aggressive`, default `Normal`)
  - `RandomPlay` (`combo`: `Optimal`, `SlightlyRandom`, `Randomized`, default `Optimal`)
  - `EasierPlay` (`check`, default `false`)
  - `PassiveMoves` (`check`, default `false`)
  - `PresetLevel` (`spin`, 0–9, default `2`)

## Compiling

### Linux (32-bit)

Requires `gcc` with 32-bit multilib support (`gcc-multilib` on Debian/Ubuntu, `lib32-gcc-libs` / `lib32-glibc` on Arch Linux):

```bash
make
```

Or compile directly:

```bash
gcc -m32 -O3 -Wall -Wextra -std=c11 -s -o MChessProUCI mchesspro.c -pthread
```

### Windows (32-bit)

#### Cross-compiling from Linux

Using [Zig](https://ziglang.org/):

```bash
make windows
```

Or directly with `zig cc`:

```bash
zig cc -target x86-windows-gnu -O3 -Wall -Wextra -std=c11 -s -o MChessProUCI.exe mchesspro.c
```

Or using MinGW-w64:

```bash
i686-w64-mingw32-gcc -O3 -Wall -Wextra -std=c11 -s -o MChessProUCI.exe mchesspro.c
```

#### Compiling natively on Windows (MSYS2 / MinGW-w64 32-bit)

In a **MINGW32** shell:

```bash
gcc -O3 -Wall -Wextra -std=c11 -s -o MChessProUCI.exe mchesspro.c
```
