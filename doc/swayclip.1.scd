swayclip(1)

# NAME

swayclip - Clipboard manager for Wayland compositors

# SYNOPSIS

*swayclip* [OPTIONS]

# OPTIONS

*-l, --logfile*
	File to write log messages. If not provided then stderr is used.

*-c, --config* <PATH>
	Use a custom config file.

*-s, --db* <PATH>
	Use a custom location to store database at.

*-r, --ready*
	Print the line "Ready" when fully initialized. Used for testing.

*-d, --debug*
	Enable debug log messages.

*-h, --help*
	Print help message.

*-v, --version*
	Print version.

# DESCRIPTION

swayclip is a clipboard manager, it remembers things you've copied so you
can access them later, and it works across multiple Wayland seats (see
*swayclip*(5) for what that means). For it to work, your Wayland compositor
needs to support the *ext-data-control-v1* protocol. You can check whether yours
does by running the *wayland-info* command, which should come bundled with
libwayland.

In Wayland, whatever app you copied something from is the one actually
holding onto that clipboard content, the compositor itself doesn't store it.
This means if you close that app, whatever you copied disappears too. swayclip
fixes this by keeping its own copy of your clipboard history, and only steps in
to "hold" the clipboard once the original app closes or exits.

# CONFIGURATION

swayclip follows the XDG Base Directory Specification. If $XDG_CONFIG_HOME is
set in the environment, then it will look for a config file at
*"$XDG_CONFIG_HOME/swayclip/config.toml"*. If not, then it defaults to
*"~/.config/swayclip/config.toml"*.

# IPC CONTROL

swayclip can be controlled via IPC commands. The IPC protocol used currently is
unstable as of now and may be subject to change. If you want to control the
daemon from the command line, there is the *swctl*(1) utility.

# AUTHOR

Written and maintained by Foxe Chen.

# SEE ALSO

*swayclip*(5) *swctl*(1) *swayclip-ipc*(7)

; vim: ft=scdoc
