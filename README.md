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

See `swayclip --help` and the man pages for more details, or see below.
```
man 1 swayclip
man 5 swayclip
man 1 swctl
```

# Quick start

## Example Config

See swayclip(5) for more details.
```TOML
# ~/.config/swayclip/config.toml
[daemon]
max_entries = 1000
persist = true
regular = true
primary = false

[daemon.mime_types]
allowed = ["text/.+", "image/.+"]
blocked = ["x-kde-passwordManagerHint"]
```

## Run

Just run the swayclip daemon, e.g. from your Wayland compositor autostart
mechanism. For example for sway:
```
exec swayclip
```

## Use with other tools

The output emitted by "swctl list" is the same used by
[cliphist](https://github.com/sentriz/cliphist). To integrate it with various
pickers:
```sh
# dmenu
swctl list | dmenu | swctl set -d

# fzf
swctl list | fzf --no-sort --with-nth 2 | swctl set -d

# rofi
swctl list | rofi -dmenu -display-columns 2 | swctl set -d

# fuzzel
swctl list | fuzzel -d --with-nth 2 | swctl set -d
```

If you see numbers, then that is the ID of the clipboard entry. Most pickers use
Tab as a column separator, meaning you can show the content only, as shown
above.

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

In Wayland, the compositor does not store the contents of the clipboard
directly. Instead the source client does, which is the Wayland client that
currently "owns" the clipboard. When another client requests some data from the
clipboard, the source clients sends the data. 

Swayclip avoids this by instead only taking ownership of the clipboard when it
is cleared (either manually or when the source client exits). This is what KDE
Klipper does I believe, according to my memory when I used to use KDE...

</details>

# License

[GPLv3 License](LICENSE)
