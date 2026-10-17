# Publish this repository

Suggested repository name: **air-radar**.

The prepared desktop folder is a Git repository with a clean initial snapshot. The ZIP
contains source files without Git history. The project contains
generic firmware, source and documentation, with no configured-device backup.
Its initial commit uses a generic project identity so a private email address is
not added to the public history. Configure your own Git name and GitHub no-reply
email for future commits if desired.

## GitHub Desktop

1. Sign in to GitHub Desktop with your own GitHub account.
2. Choose **File → Add local repository** and select this `air-radar` folder.
3. Choose **Publish repository**, use the name `air-radar` and select the visibility
   you want. Uncheck **Keep this code private** if you want everyone to see it.

## If you extracted the source ZIP

Before pushing with Git, initialize the extracted folder if it has no `.git` directory:

```sh
git init -b main
git add .
git commit -m "Initial Air Radar 3.0.0 release"
```

Use your configured Git identity, preferably a GitHub no-reply email for public history.
Alternatively, use the website upload instructions below without Git.

## GitHub website and Git

1. Create a repository named `air-radar` on GitHub. Leave it empty: do not add a
   README, gitignore or license there, because this local repository has its own files.
2. Copy its HTTPS Git URL from the repository page.
3. In a terminal opened in this folder, run:

```sh
git remote add origin https://github.com/YOUR-USERNAME/air-radar.git
git push -u origin main
```

Replace the example URL with your repository's actual URL. Sign in through Git's
normal credential prompt if required; do not paste account tokens into source files.

If you use **Upload files** on GitHub instead, extract the provided ZIP first and
upload its contents with the folder structure intact. Uploading only the ZIP as a
repository file will not expose the source tree or enable the checks workflow.

No original-code license has been selected. Third-party notices are included;
choose a project license before advertising this as an open-source project.

Do not enable GitHub Pages for the credential form. The ESP32 or the PC companion
must serve that page locally so it can configure the device.
