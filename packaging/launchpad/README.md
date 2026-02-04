# Launchpad PPA (Jammy + Noble)

This project can be uploaded to a Launchpad PPA via GitHub Actions without committing any Debian changelog changes back to the repository.

## How it works

- The workflow builds a *source package* twice (once for `jammy`, once for `noble`).
- For each series it runs `dch --newversion ...` locally (in CI only) so the Debian version matches the Git tag.
- Uploads with `dput ppa:OWNER/PPA`.

The binary version printed by `ai-z --version` is forced from the Debian changelog upstream version via `-DAI_Z_VERSION_OVERRIDE` in `debian/rules`.

## Required secrets

Set these GitHub repository secrets:

- `LAUNCHPAD_PPA`: `owner/ppa` (example: `mylaunchpadid/ai-z`)
- `LAUNCHPAD_GPG_PRIVATE_KEY`: ASCII-armored private key used to sign `.changes`
- `LAUNCHPAD_GPG_FINGERPRINT`: full fingerprint for that key
- `LAUNCHPAD_GPG_PASSPHRASE`: passphrase (optional if the key is unencrypted)
- `LAUNCHPAD_SSH_PRIVATE_KEY`: SSH private key registered with your Launchpad account (used by `dput`)

Optional secrets used for changelog identity:

- `DEBFULLNAME`
- `DEBEMAIL`

## Trigger

- Push a tag like `v0.1.23` to run automatically, or use **workflow_dispatch**.

## Notes

- Launchpad must have your public GPG key associated with your account.
- The SSH public key must be added to Launchpad as well (the private key is stored in GitHub Secrets).
- If you upload the same version again, Launchpad will reject it; bump the Debian revision or re-tag.
