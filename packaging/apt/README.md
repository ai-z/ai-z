# APT repository helper

This folder contains a tiny helper to create an APT repository directory from a built `.deb`.

## Generate a `.deb`

From the project root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cpack --config build/CPackConfig.cmake -G DEB
```

Compatibility note: to support Ubuntu 22.04 clients, generate the `.deb` on
Ubuntu 22.04 (or in a 22.04 container/chroot). If you build on newer distros,
the package will depend on newer `libc6`/`libstdc++6` and `apt install ai-z`
will fail on 22.04.

This will produce `ai-z_*.deb`.

## Create a repo directory

Prereqs:

```bash
sudo apt-get install -y dpkg-dev
```

Create the repository tree:

```bash
./packaging/apt/make_repo.sh ./ai-z_0.1.0_amd64.deb ./repo stable main
```

Then publish the `repo/` directory over HTTPS.

## Client install

On a client machine, add a source list entry (unsigned example):

```bash
echo "deb [trusted=yes] https://YOUR_HOST/repo stable main" | sudo tee /etc/apt/sources.list.d/ai-z.list
sudo apt update
sudo apt install ai-z
```

For real distribution, you should sign the repository metadata (Release/InRelease) and distribute the signing key.

## GitHub Pages (automated)

This repo includes GitHub Actions workflows that can build the `.deb`, generate an APT repo tree, and publish it to GitHub Pages.

- Build workflow: `.github/workflows/publish-apt-pages.yml`
- Deploy workflow: `.github/workflows/deploy-apt-pages.yml`
- Trigger: push a tag like `v0.1.0` (or run manually via **workflow_dispatch**)

Once Pages is enabled for the repository (Settings → Pages → Source: **GitHub Actions**), users can install with:

```bash
echo "deb [trusted=yes] https://OWNER.github.io/REPO stable main" | sudo tee /etc/apt/sources.list.d/ai-z.list
sudo apt update
sudo apt install ai-z
```

Replace `OWNER` and `REPO` with your GitHub org/user and repository name.

If you later switch to a custom domain, point the domain at the Pages site for this repository and keep the same `/dists/...` layout.
