swayclip(5)

# NAME

Configuration options for swayclip

# DESCRIPTION

The configuration file for swayclip is sourced on startup, using the TOML
format. To see where to write the config file, see *swayclip*(1).

# TERMINOLOGY

*source client*
	The current Wayland client that "owns" the clipboard. "own" is this case
	means that it provides the contents of the clipboard to other clients.

*selection event*
	The event when another client becomes the source client, in other words when
	the user copies something.

*entry*
	An entry contains metadata and the associated contents of a selection event.
	A new one may be created when the user copies something to the clipboard.

*wayland seat*
	A Wayland seat represents a collection of input devices. It has its own
	clipboard state separate from other seats. Most users will only use one
	seat, so if you still do not know what this means, it likely does not matter
	for you.

# OPTIONS

The following options are supported under the *"daemon"* table:

*max_entries = <integer>* 
	Maximum number of entries to store in the database. If this limit is
	exceeded, the oldest entries that are not pinned are purged from the
	clipboard history. Set to 100 entries by default.
	
*max_size = <integer>*
	Maximum number of bytes allowed for the contents of a mime type. If this
	number if exceeded, the selection event is aborted, and no entry is added to
	the database. Defaults to 20 MB.

*persist = <boolean>*
	If database should be written on disk in the filesystem. If true, then
	clipboard history is preserved across restarting of the daemon. If false,
	then the database is stored completely in memory, and will be lost when the
	daemon exits. Defaults to true.

*regular = <boolean>*
	Default value for if the regular selection should be synced with the daemon
	clipboard. Defaults to true.

*primary = <boolean>*
	Default value for if the primary selection should be synced with the daemon
	clipboard. Defaults to true

*set_on_startup = <boolean>*
	If the most recent entry in the database should be set as the current entry
	on startup, or when a new seat is added (for that seat). This is only
	relevant if "persist" is true.

*dedup = <string>*
	Configures how duplicated entries should be handled, defaulting to "all". An
	entry is considered the same as another entry, if they have the same mime
	types containing the same data. The following values are supported:

	*none*
		Duplicate entries are left as is and not removed.

	*prev*
		If the entry from a selection event is the same as the current entry, then
		the selection event will be ignored. If there is a duplicate entry, but it
		is not the current entry, then the entry will still be saved.

	*all*
		If there is a duplicate entry in all of the clipboard history, then the
		selection event will be ignored and instead of the existing entry will be
		moved to the front of clipboard history.

The following options are supported under the *"mime_types"* table, which is
under the *"daemon"* table:

*allowed = [array of regular expressions]*
	Each string is a POSIX regular expression, that is matched with the mime
	types in the selection event. If there is a match, then the daemon will
	store the data for this mime type (also referred to as a clipboard format.

*blocked = [array of regular expressions]*
	Same semantics as the "allowed" option, but if there is a match, then the
	selection event is ignored, and no entry will be created. This is useful if
	you do not want the clipboard of sensitive applications from being stored.


The following options are supported under the *"seats"* table, which is
under the *"daemon"* table:

	*<seat name>*
	Name of the Wayland seat that should have its clipboard synced with the
	daemon. It is a table which supports the following options
		
	*regular = <boolean>*
		if the regular selection of this seat should be synced with the daemon
		clipboard. Defaults to the value of "daemon.regular".

	*primary = <boolean>*
		if the primary selection of this seat should be synced with the daemon
		clipboard. Defaults to to the value of "daemon.primary".

Note that if the "seats" table is not specified, then all seats will be synced.
It is recommended for most users to not specifiy this table, unless they use
multiple seats and want more control.

# EXAMPLE

```
[daemon]
max_entries = 1000
persist = true
regular = true
primary = false

[daemon.mime_types]
allowed = ["text/.+", "image/.+"]

# Some password managers set this mime type
blocked = ["x-kde-passwordManagerHint"]

[daemon.seats."seat0"]
regular = true
primary = false
```

# SEE ALSO

*swayclip*(1)

; vim: ft=scdoc
