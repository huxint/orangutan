# Filesystem Runtime

`oran-io` owns filesystem effects and blocking-operation cancellation. Tool policy
is evaluated above it; authorized directory/file handles carry the effect into
this boundary. Diagnostic path strings are never authority.

## Authority

`DirectoryAuthority` pins a root directory. Relative traversal rejects escapes
and unintended symlinks. File read/write/edit operations retain
appropriate handles across asynchronous work. Mutation requests pin their target
and compare identity/version before changing it, so approval does not authorize
a replacement inode introduced while the caller waits.

Workspace roots and extra read/write roots are explicit. Reads use read authority;
mutations require write authority. An outside-root override must pass dispatch
approval and keep its selected authority throughout the operation.

`PrivateDirectory` creates/opens an owned private directory, rejects unsafe file
reads, writes mode-0600 files atomically with fsync, and provides exclusive locks.
Hosts can use it for state ownership. It does not encrypt credentials.

## File Reads

Text reads share one descriptor implementation. Workspace callers supply a
`ReadOnlyFile` opened through their directory authority. The trusted-host path
overload opens the file once on the supplied worker through
`ReadOnlyFile::open_trusted`; it follows symlinks and requires a regular file.
Both paths hold that descriptor through the complete read. A later pathname
replacement cannot redirect an already-open read.

Every invocation reads current file bytes. Reads enforce byte/range bounds and
align truncated or byte-range results to UTF-8 code-point boundaries. The ranged
API returns contents, fingerprint, line span, returned-byte count and truncation;
`read_text_file` rejects truncation with `invalid_argument`.
Line ranges scan from the start using a fixed-size buffer.

File fingerprints and version tokens support conflict detection. Size or mtime
drift during a read returns `conflict`; whole files smaller than 64 KiB retry
once before returning that error. Fingerprints describe metadata, not a content
hash. Separate calls observe rewrites even when size and mtime are unchanged.

Blocking work runs on the supplied worker executor through `run_blocking`.
Queued cancellation stops the operation before execution. Work already in
progress retains its captures until it returns; the caller resumes after safe
resource cleanup. Each read owns its resources and cancellation independently.

## Verification

Tests exercise symlink/path escape rejection, target replacement, private-file
checks, UTF-8/range limits, content freshness and independent caller cancellation.
