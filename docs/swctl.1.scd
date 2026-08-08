swctl(1)

# NAME

swctl - Control and query the swayclip daemon

# SYNOPSIS

*swctl* [OPTIONS] [COMMAND]

# OPTIONS

*-h, --help*
	Print help message.

*-v, --version*
	Print version.

# COMMANDS

All commands have a "--help" option, which provides a quick summary.

*list [--json] [--start <n>] [--number <n>]*
	List up to "--number" entries starting at "--start". If "--json" is
	provided, then the output will be a JSON array. Otherwise a format suited
	for pickers will be outputted, as described below:

	Each line is prefixed with a number representing the ID of the entry,
	separated by a Tab, then a preview of the contents. If the contents is an
	image or binary, then placeholder text displaying the mime type and size of
	the data will be shown instead.

	Most pickers consider a Tab to be a column separator, which can be
	used to only show the contents in the picker. For example with fuzzel:
	```
	swctl list | fuzzel -d --with-nth 2 | swctl set -d
	```

	Note that the JSON output may be split into several JSON individual arrays.

*len*
	Output number of entries stored in clipboard history.

*set [--clear] [--decode] [ID]*
	Set the clipboard to the entry specified by "ID". If "--clear" is provided,
	then the clipboard will be cleared, and "ID" is not necessary.

	If "--decode" is provided, then parse the output from the "list" command,
	which should be a single line.

*get <ID> <MIME TYPE>*
	Output the contents of the mime type for the entry specified by "ID". Note
	that the output is not modified/sanitized in any way.

*delete [--clear] <ID>*
	Delete the entry specified by "ID". If the clipboard was set to this entry,
	then it will be cleared (the previous entry is not used). If "--clear" is
	provided, then the entire clipboard history will be cleared (use with
	caution!).

*pin [--unpin] [--toggle] <ID>*
	Pin the entry specified by "ID". A pinned entry is not trimmed from the
	clipboard history. If "--unpin" is provided, then unpin the entry. If
	"--toggle" is provided, then toggle the pinned status of the entry.

*current*
	Output ID of entry that clipboard is set to. If the clipboard is cleared,
	then -1 is outputted. If the current entry is transient, then 0 is
	outputted, see *swayclip*(5) for more information.

*events <events>*
	Output a JSON stream of events from the daemon. "events" is a list of events
	to listen for. Each event is a JSON object that contains fields. A field
	prefixed with "?" is optional. Note that the list of events are unstable as
	of now, and may be modified at any time.

	*entry_add*:
		Emitted when a new entry is added to clipboard history.
		```
		{
			"event": "entry_add",
			"id": <ID>
		}
		```

	*entry_delete*:
		Emitted when an entry is deleted. If "id" -1, then history was cleared
		(all entries deleted).
		```
		{
			"event": "entry_add",
			"id": <ID>
		}

		```
	*entry_update*:
		Emitted when a entry is updated. Either "pinned" or "update_time" field
		will be present, but never both. "update_time" is in milliseconds since
		Unix epoch.
		```
		{
			"event": "entry_update",
			"id": <ID>,
			"?pinned": <bool>,
			"?update_time": <int>
		}
		```
	
	*entry_move*
		Emitted when an entry is moved. Currently only relevant if "dedup"
		config option is set to "all", see *swayclip*(5) for more information.
		```
		{
			"event": "entry_move",
			"id": <ID>,
			"old_pos": <int>,
			"new_pos": <int>, // Currently can only be 0
		}
		```

	*entry_state*
		Emitted when the state of an entry changes. If "state" is true, then the
		entry is set as the current entry for the clipboard, otherwise the entry
		was unset as the current entry.
		```
		{
			"event": "entry_state",
			"id": <ID>,
			"state": <bool>,
		}
		```

# SEE ALSO

*swayclip*(1)

; vim: ft=scdoc
