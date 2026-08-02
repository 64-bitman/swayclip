swayclip(5)

# NAME

Configuration options for swayclip

# DESCRIPTION

swayclip is a clipboard manager: it keeps a history of things you've copied so
you can paste them again later. When it starts up, it reads a settings file
written in the TOML format. To find out where this file should go, see
*swayclip*(1).

# TERMINOLOGY

A few words you'll see used in a specific way in this document:

*source client*
	Whichever app currently "owns" the clipboard, meaning it's the one
	supplying whatever you'd paste right now.

*selection event*
	What happens when you copy something. A different app becomes the one "in
	charge" of the clipboard.

*entry*
	One item in your clipboard history, essentially one thing you copied, along
	with some info about it. A new entry gets created each time you copy
	something.

*wayland seat*
	Think of a "seat" as one user's set of keyboard/mouse/etc, with its own
	clipboard. Almost everyone only has one seat on their computer, so unless
	you know you have more than one, you can disregard this term entirely, it
	won't affect you.

# OPTIONS

These options go under a section called *"daemon"* in the settings file.

*max_entries = <integer>* 
	How many items to keep in your clipboard history. Once you go over this
	number, the oldest items get deleted to make room, but only ones you haven't
	"pinned" (marked to keep). Default: 100 items.
	
*max_size = <integer>*
	The largest size (in bytes) that a single copied item is allowed to be. If
	you copy something bigger than this, it will not be saved. Default: 20 MB.

*persist = <boolean>*
	Whether your clipboard history is saved to disk. If turned on (true), your
	history persists even after you restart swayclip. If turned off (false),
	everything is kept only in memory and disappears when swayclip closes.
	Default: true (saved to disk).

*regular = <boolean>*
	Whether normal copy/paste (the kind you get from Ctrl+C) should be tracked
	by swayclip. Default: true (tracked).

*primary = <boolean>*
	Whether "primary selection" should be tracked. It is the clipboard you get
	usually from highlighting text, which some apps let you paste with a
	middle-click.
	Default: true (tracked).

*set_on_startup = <boolean>*
	Whether swayclip should automatically restore your most recent clipboard
	item when it starts up (or when a new "seat" is added). This only matters if
	you have "persist" turned on above.

*dedup = <string>*
	What to do about repeated (duplicate) items. Two entries count as duplicates
	if they contain exactly the same content. Default: "all". Possible values:

	*none*
		Keep every duplicate, don't remove or merge anything.

	*prev*
		If you copy something and it's identical to the very last thing you
		copied, ignore it (don't add a new entry). But if it matches an older
		entry further back in your history, still save it as new.

	*all*
		If you copy something that matches any previous entry anywhere in your
		history, don't create a new entry, instead just move that older matching
		entry back up to the top of the list.

*logfile = <path string>*
	Path to file to log messages to. Note that the "--logfile" command argument
	takes precedence.

*db = <path string>*
	Path to file to store database at. Note that the "--db" command argument
	takes precedence.

These options go under a *"mime_types"* section, which itself lives inside the
*"daemon"* section. They let you control which kinds of content get saved.

*allowed = [array of regular expressions]*
	A list of patterns (POSIX regular expressions) describing which content
	types are okay to save — for example, plain text or images. If something
	copied matches one of these patterns, swayclip will store it.

*blocked = [array of regular expressions]*
	Works the same way as "allowed" above, but in reverse: if a copied item
	matches one of these patterns, it's deliberately skipped and never saved.
	Useful for things like passwords, which some apps mark in a special way so
	you can exclude them.

These options go under a *"seats"* section, which also lives inside the
*"daemon"* section. Most users can skip this entirely, it's only useful if you
have more than one "seat" (see the Terminology section above) and want different
clipboard behavior for each one.

	*<seat name>*
	Replace this with the actual name of the seat you want to configure.
	Under it, you can set:
		
	*regular = <boolean>*
		Same as the "regular" option above, but just for this one seat.
		If not set, it uses whatever "daemon.regular" is set to.

	*primary = <boolean>*
		Same as the "primary" option above, but just for this one seat.
		If not set, it uses whatever "daemon.primary" is set to.

If you don't include a "seats" section at all, swayclip will just track the
clipboard for every seat automatically, which is what most users want.

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
