# Linux libc compatibility

GPUI 0.2.2 transitively uses xattr 0.2.3 via its archive support. Modern libc
no longer exports the nonstandard Linux ENOATTR alias. This vendored copy uses
ENODATA on Linux/Android and preserves ENOATTR on BSD/macOS. All other source
is unchanged from the published xattr 0.2.3 crate, including its licenses.

Upstream: https://github.com/Stebalien/xattr
