#!/bin/sh
# Helpers for the ePass init scripts that supervise a background worker through
# a pidfile and a "ready" marker: S99epass-late and umtprd.
#
# Source this file, do not execute it:
#   . /usr/lib/epass-init-functions.sh

# True when $1 is a readable pidfile holding a live PID.
pid_is_running()
{
	pidfile="$1"

	[ -r "$pidfile" ] || return 1
	pid="$(cat "$pidfile" 2>/dev/null)"
	case "$pid" in
	""|*[!0-9]*) return 1 ;;
	esac
	kill -0 "$pid" 2>/dev/null
}

# Remove the pidfile named by $1, but only when it belongs to the calling shell.
# A worker that exits while its replacement is already starting must not delete
# the new worker's pidfile.
remove_own_pidfile()
{
	pidfile="$1"

	[ -r "$pidfile" ] || return 0
	[ "$(cat "$pidfile" 2>/dev/null)" != "$$" ] || rm -f "$pidfile"
}

# Run the calling script's "worker" verb in the background, supervised by the
# pidfile named by $1 and logging to $2.  Returns non-zero when the worker
# could not be started.
start_worker()
{
	pidfile="$1"
	logfile="$2"

	rm -f "$pidfile"
	mkdir -p "$(dirname "$logfile")"
	: > "$logfile"
	start-stop-daemon --start --background --make-pidfile \
		--pidfile "$pidfile" --output "$logfile" \
		--exec "$0" -- worker
}

# Wait for worker exit before tearing down resources it may still be creating.
stop_worker()
{
	pidfile="$1"
	if pid_is_running "$pidfile"; then
		pid="$(cat "$pidfile")"
		kill "$pid" 2>/dev/null || true
		timeout=20
		while kill -0 "$pid" 2>/dev/null && [ "$timeout" -gt 0 ]; do
			usleep 100000
			timeout=$((timeout - 1))
		done
		if kill -0 "$pid" 2>/dev/null; then
			kill -9 "$pid" 2>/dev/null || true
		fi
	fi
	rm -f "$pidfile"
}
