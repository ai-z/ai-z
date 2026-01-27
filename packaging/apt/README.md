# APT repository helper

This folder contains a tiny helper to create an APT repository directory from a built `.deb`.

## Generate a `.deb`

From the project root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cpack --config build/CPackConfig.cmake -G DEB
```

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

This repo includes a GitHub Actions workflow that can build the `.deb`, generate an APT repo tree, and publish it to GitHub Pages.

- Workflow: `.github/workflows/publish-apt-pages.yml`
- Trigger: push a tag like `v0.1.0` (or run it manually via **workflow_dispatch**)

Once Pages is enabled for the repository (Settings → Pages → Source: **GitHub Actions**), users can install with:

```bash
echo "deb [trusted=yes] https://OWNER.github.io/REPO stable main" | sudo tee /etc/apt/sources.list.d/ai-z.list
sudo apt update
sudo apt install ai-z
```

Replace `OWNER` and `REPO` with your GitHub org/user and repository name.
