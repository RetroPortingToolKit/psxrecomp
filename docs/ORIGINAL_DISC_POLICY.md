# Required original-disc identity

A title can require verified original content with an allowlist in `game.toml`:

```toml
[game]
required_disc_sha256 = ["<64 hexadecimal characters for an original supported dump>"]
```

A present allowlist must contain at least one valid SHA-256. Values normalize
to lowercase; empty or malformed policies reject the configuration. Older
unconfigured titles keep their previous policy. This is a title contract,
not a launcher preference or mod option.

The same content hash serves package targets and mandatory base verification.
For CUE/BIN, it hashes the resolved data-track file; for CHD, it hashes all
2352-byte decoded raw sectors. Renaming a CUE does not change the identity.
Compressed bytes and CUE text are not the disc identity. A cooked ISO does
not match a raw original hash and cannot substitute for discarded XA sectors.

Verification runs in the disc-selection badge, launcher preparation, direct
launch, and session boot/rematch. It reads the current bytes, including when
the selected filename is unchanged. A read failure or mismatch rejects boot
even with every mod disabled or no initialized mod catalog. Header, serial,
and executable-text checks alone do not prove the original assets.

Mod content applies over the verified untouched base. A runtime-generated
private mod image is not reclassified as an owner-supplied original disc.
Persistent memory cards and native saves have no policy or format change.
