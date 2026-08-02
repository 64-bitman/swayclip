# swayclip - Clipboard manager for Wayland compositors

- Supports persistent history
- Supports multiple seats
- Lightweight and fast
- Controllable via IPC
- Supports multimedia and any clipboard format

# Build & Install

Build dependencies:
```
meson
sqlite
json-c
libwayland
scdoc (optional, for man pages)
```

Install:
```
meson setup -C build
meson compile -C build
sudo meson install -C build
```

# Usage

See `swayclip --help` and the man pages for more details.
```
man 1 swayclip
man 5 swayclip
man 1 swctl
```

# FAQ

<details>
<summary><strong>Why is it named swayclip?</strong></summary>

Because I use sway and I think it sounds cool.

</details>

<details>
<summary><strong>What makes it different from other clipboard managers?</strong></summary>

A problem I noted with existing Wayland clipboard managers, is that they either
shell out the Wayland protocol handling to an existing tool such as
`wl-clipboard`, or are native but with a shortcoming.

In both cases, this means that when you copy to the clipboard, the clipboard
manager immediately takes ownership of the clipboard. This may strip certain
useful mime types from the clipboard that the original source client had.

Swayclip avoids this by instead only taking ownership of the clipboard when it
is cleared (either manually or when the source client exits). This is what KDE
Klipper does I believe, according to my memory when I used to use KDE...

</details>

# License

[GPLv3 License](LICENSE)
