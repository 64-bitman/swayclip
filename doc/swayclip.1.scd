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

swayclip is a clipboard manager that provides persistent history and supports
multiple Wayland seats. The running Wayland compositor must support the
*ext-data-control-v1* protocol, which you can check if available using the
*wayland-info* command line utility, usually provided with libwayland.

A common problem with the Wayland protocol is that the compositor does not store
the clipboard contents, instead the source client does. This results in the
clipboard/selection being cleared when the source client exits. swayclip solves
this problem by storing the contents of the clipboard, and only becomes the
source client when the existing source client exits, or if the clipboard is
cleared directly.

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
