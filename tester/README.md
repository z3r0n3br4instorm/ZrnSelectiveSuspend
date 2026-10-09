# Testing ZrnSelectiveSuspend on your computer

Thank you for helping. ZSS switches graphics cards off, moves running programs
between them, and lends them to virtual machines, without restarting anything.
It has been proven on one laptop so far; every other machine teaches us
something.

## What you run

```sh
./zss-report-gui      # a window: press Collect, read the result, press Send report
./zss-report          # the same in a terminal; --mail opens your mail program at the end
```

It takes a few minutes. It needs Python 3; the window also needs PySide6 or
PyQt6 (`pacman -S pyside6`, `apt install python3-pyside6`, `dnf install
python3-pyside6`). Without them, use the terminal version.

## What it does

1. **Collects facts:** the graphics cards and their drivers, how their power can
   be switched, the IOMMU, what Vulkan and OpenGL see, the display server, and
   graphics-related kernel messages.
2. **Runs the moving tests:** test programs are moved between your graphics
   cards and back, and what they draw is compared. A few test windows appear for
   a moment. This uses a test daemon of its own in dry-run mode: **no card is
   switched off and no driver is touched.**
3. **Saves the report** as a folder and a `.tar.gz` file in your home folder.

**It changes nothing on your computer, installs nothing, and sends nothing by
itself.** You read the report and send it yourself.

Before anything is saved, your computer's name, your user name and home folder,
disk identifiers, serial numbers, network addresses and email addresses are
replaced by placeholders. The report does not include your files, browser data
or anything outside the graphics stack.

## Sending it

Press **Send report** (or run `./zss-report --mail`). Your mail program opens
with the address `omethabeyrathne3@gmail.com` and a summary filled in. Attach the
`.tar.gz` file (its path is copied for you) and send.

## The full test (optional)

**Run full test** goes further and does change your computer: it builds and
installs ZSS with its kernel module (your graphics driver is not patched), runs
the whole test suite, and switches your discrete graphics card off and on once.
It asks for your password and shows every command first. Save your work before
running it. `packaging/uninstall.sh` removes everything again.
