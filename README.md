# uvm-doom

UVM port of Doom

<p align="center">
  <img src="uvm_doom.jpg" width="700" />
</p>

## Getting the code

This repo uses [UVM](https://github.com/maximecb/uvm) as a git submodule (in
`uvm/`), so clone with `--recurse-submodules`:

```bash
git clone --recurse-submodules https://github.com/maximecb/uvm-doom.git
```

If you already cloned without it, pull the submodule in:

```bash
git submodule update --init --recursive
```

To later update UVM to its latest upstream commit:

```bash
git submodule update --remote uvm
git add uvm && git commit -m "Bump uvm submodule"
```

## License

This project is licensed under the **GNU General Public License, version 2** —
see [LICENSE](LICENSE).

The Doom engine (`PureDOOM.h`) is derived from the Doom source code released by
id Software, Inc. and remains **Copyright (C) 1993-1996 id Software, Inc.**
id Software relicensed the Doom source under the GPL in 1999; the original
copyright notices are preserved as the GPL requires.

New code written for this UVM port is **Copyright (C) 2026 Maxime
Chevalier-Boisvert** and is likewise released under the GPL v2.

The bundled [UVM](https://github.com/maximecb/uvm) submodule is a separate work
under the **Apache License 2.0** and is not covered by this repository's GPL.

> Note: the `doom1.wad` shareware data file is **not** covered by the GPL. It is
> distributed under id Software's original shareware terms and may not be sold.
