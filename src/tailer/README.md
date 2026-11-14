# Tailer

This directory contains the functionality for monitoring
[remote files](https://docs.lnav.org/en/latest/usage.html#remote-files).  The
name "tailer" refers to the binary that is transferred to the remote host that
takes care of tailing files and sending the contents back to the host that is
running the main lnav binary.  To ease integration with lnav's existing
functionality, the remote files are mirrored locally.  The tailer also
supports interactive use by providing previews of file contents and
TAB-completion possibilities.

## Files

The important files in this directory are:

- [tailer.main.c](tailer.main.c) - The main() implementation for the tailer.
- [tailer.looper.hh](tailer.looper.hh) - The service in the main lnav binary
  that transfers tailers to hosts and communicates with them.
- [tailer.h](tailer.h) and [tailerpp.hh](tailerpp.hh) - Utility libraries for
  the tailer protocol.
- tailer.ape - The [αcτµαlly pδrταblε εxεcµταblε](https://justine.lol/ape.html)
  build of the tailer.  This binary is produced by a GitHub Action and checked
  in so the build process doesn't need to be supported on lots of platforms.
- [sdefl.h](sdefl.h) - The deflate encoder used to compress file blocks,
  vendored from [vurtun/lib](https://github.com/vurtun/lib) at commit
  5a3f3aba052e.  The only change is that the standard includes are skipped
  when building with cosmopolitan.

## Flow

When a remote-path is passed to lnav, the
[file_collection.hh](../file_collection.hh) logic forwards the request to the
`tailer::looper` service.  This service makes a single connection to the
remote host using the `ssh` command, so that the user's custom configurations
will be used, or `kubectl exec` for paths in a Kubernetes container.  The
connection runs a small bootstrap script that prints which tailer
implementation it wants (Python if `python3` is available, otherwise
"tailer.ape"), reads the size of the implementation as a line on stdin, writes
that many bytes to a file, and then starts the tailer with the same
stdin/stdout.  lnav waits for the tailer's `TPT_ANNOUNCE` packet before sending
anything else, so nothing is consumed by the bootstrap script.  From then on,
stdin/stdout are used for a binary protocol and stderr for logging.  The
tailer then waits for requests to open files, preview files, and get possible
paths for TAB-completions.  File contents are sent as `TPPT_DEFLATED_BITS`
payloads when compressing them makes them smaller.
