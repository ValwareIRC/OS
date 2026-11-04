# What still makes it feel like a toy: the audit (2026-11-02)

The standard: a person using it as their main computer, every day. Anything that pretends, says something
untrue or cute, hides a missing piece, or is missing what every real operating system has, is on this list.
Each item says what is wrong and what is done about it. Fixed items say how they were checked.

Method: every string the desktop can show was read (build/audit/ui-strings.txt, 782 of them, extracted by
build/diskpeek/ui_strings.py); every surface opened in a VM and looked at (tests/surfaces_vm_win.py); the code
searched for placeholders, stubs, "for now", artificial waits and half-done features; the features of a real
desktop OS (Windows, macOS, GNOME) checked one by one against what exists.

## Fixed

1. **Apps renamed by designs** (Notes as "Dreams" or "Recycle Bin", Files as "Keepsakes"): apps keep their real
   names in every design; the design language no longer offers renaming. Checked: starlit and Luna in a VM. (25f68de)
2. **System buttons with themed words** ("Goodnight", "red pill", "NEW GAME+"): they say what they do. (25f68de)
3. **Slogans as greetings** ("the signal's steady", "quiet terminal, uptime forever"): the greeting and the line
   under it are the system's (a greeting by the hour with their name; the date). (25f68de)
4. **The boot**: kernel text scrolling by, then a red "Operating System" splash with a fixed 3.2 s wait. Now a
   calm splash, the steps behind Esc, their design's colours, a fade into the desktop; no wait. (9ec1286)
5. **A crash didn't end the whole program** (the other threads ran on, even exiting "successfully"). (32bbdc6)
6. **Flathub apps didn't open** once the desktop ran as the person. (dbbd476)
7. **The AI "thinking" forever**, crashing on a 5 GB model in 12 GB, refusing over a 4096-token context. (0384d24)

8. **Shutting down had three names** ("Power off", "Power Off", "Shut down"): "Shut down" everywhere.
9. **Onboarding spoke in riddles** ("something almost asleep", "That one feels like home", "Which of these sounds
   like you?"): plain questions and answers.
10. **About showed design trivia** ("shaped for Valerie by Luna", palette, type): it shows the system and the
    computer (processor, memory, graphics, storage, display, network, kernel, uptime, licences); the design's notes
    are in Settings > Appearance. Checked in a VM.
11. **Installed apps showed a coloured letter**: their own icons (Chromium's, checked in a VM); apps with no
    picture of their own (ones the companion made) get the symbol of what they are (a list for Todo, a clock for
    Stopwatch); a letter only when nothing fits.
12. **"Recent" showed system programs** (`du` from /bin) and files as folders: only their own files, each with its
    own icon. Checked in a VM.
13. **Suggestions showed "Files" with no icon** (it was drawn at zero opacity). Checked in a VM.
14a. **The Terminal opened on a note for developers** ("The usual tools are here; 'busybox' lists them") and a
    bare `~ $`: it opens on a prompt that says who and where (`valerie@computer:~$`).
14. **No lock screen, no password**: an optional password (Settings > Users; kept by init as SHA-512 crypt, root
    only, waits after wrong guesses), a lock screen (Super+L, or after a while away: Settings > Users > Lock after)
    with the time, the date, who it is, and Restart / Shut down; with a password it asks for it at start.
    tests/lock_vm_win.py 8/8. (Locking on waking comes with sleep, which the system doesn't have yet.)
16. **The home folder was /root, the user "user"**: the account is named after them (valerie), its home
    /home/valerie, in /etc/passwd and /etc/group, the computer `<name>-pc`; a disk from before moves over.
    tests/account_vm_win.py 6/6.
14e. **A design could rename them**: starter designs carried a sample name ("Sam") and their own companion
    names, and applying one could make the desktop call them Sam. A design never sets their name now (the one
    they gave is the only one), the companion's only while it has none; the samples are gone from every design.
    tests/names_vm_win.py.
14c. **The version said 0.9.5** after all of this: 0.10.0.
14d. **Notes was one note**: the list of their notes in Documents beside the page (newest first, with the
    start of each), searched by title and by what they say, Ctrl+F in a note (every match shown, "2 of 5",
    Enter and Shift+Enter, the arrows), a note to Trash from the list; narrow windows show the list on asking.
    tests/notes_list_vm_win.py 7/7.
15. **No date and time**: the clock is set from the network (SNTP in init, at the start and every six hours;
    the kernel gained clock_settime, for root of the first user namespace only); the time zone is found from
    where the computer is on the internet, or chosen in Settings > Date & time (searched by city, country or
    zone, with each zone's time); the tz database is in the system; a 12- or 24-hour clock that every clock
    follows (the desktop's, the lock screen, Files, Notes, the tray). tests/time_vm_win.py 6/6.
17. **Settings was missing pages**: Mouse (speed, acceleration, natural scrolling, left-handed), Power (the
    screen off after a while away, lock after, shut down and restart), Storage (what uses the disk: their files,
    apps, AI models, Trash, caches; Empty Trash, clear caches), Apps (what they installed, removed with a second
    press; which app opens web links, email, pictures, music, videos, PDFs, text: mimeapps.list, which the portal
    now follows), Notifications (Do not disturb by hand or at set hours, a switch for each app that has sent one).
    tests/settings_pages_vm_win.py 7/7 (the screen really goes off after five minutes and comes back on a key).
14f. **The companion talked like a bad novel and made up memories** ("I still remember the way you left the
    screen on overnight... The city hums on"): its character had been written by "a novelist", with habits of
    theirs it had invented, and the welcome back asked it to recall something. Now it talks plainly, mentions
    the past only when it is written down, never quotes their portrait back, and a character never holds what
    they didn't say; characters written the old way are rewritten once. tests/plain_talk_vm_win.py (a copy of
    their disk, the built-in model: "You're back, and it's nearly one."). An 8B model still slips now and then.
14g. **A picture the companion opened went to Notes** ("no app here for mita-pc.png"), and it never knew: it
    tried again and again while only the person saw the error. Pictures open in the viewer; [[see: FILE]] lets
    it look at a picture itself (and a model that can't see is told so); a picture now goes with the very request
    that asks about it (each one used to arrive a step late, the screen's too); what the screen says while its
    tags run is told to it, and an error gets one answer at once. tests/see_picture_vm_win.py 6/6.
14h. **`mkdir -p` failed in a folder they can't write** (their home, under /home): the kernel said "permission
    denied" for a folder that was already there, where Linux says "already there". Fixed for mkdir, symlink and
    link, with a self-test.
14i. **Firefox took 20 minutes to install**: the desktop downloaded its 1.1 GB reflex model and copied the 5 GB
    built-in model onto the disk at the same moment. The desktop's own downloads and copies now wait while an app
    installs (and go on from where they were). From a fresh disk, in the desktop: 157-165 s
    (tests/browser_vm_win.py, which fails past 5 minutes; tests/flatpak_install_win.py: 147 s alone).
19. **Subsurfaces were accepted but not drawn**: Firefox's window was black (its toolbar and page are in one).
    Composited now: places, order, synchronized and desynchronized frames, alpha. tests/wl-sub-test.sh, and
    tests/browser_vm_win.py checks the window shows its page.
14j. **Apps couldn't see files change**: no inotify, so GLib had no file monitor (folders didn't refresh, settings
    files weren't reread). The kernel has inotify now; `gio monitor` sees files made, changed, renamed and
    deleted. 18 self-tests.
14k. **Their files lived in memory**: the home was held in RAM and written whole into the store at every change,
    128 MB at most, no file over 32 MB kept (a 50 MB download was gone after a restart). It lives on the disk
    now, in an area with ext3's journal (the kernel writes it, e2fsck replays it), so a power cut loses nothing
    already saved. tests/journal_crash_win.py pulls the power at random moments while files are written (3 of 3
    rounds: every file there byte for byte); tests/home_disk_vm_win.py moves a home from before onto the disk
    and keeps a 50 MB file through a power cut (9 of 9).
14l. **The clock could run at half speed for a whole session**: the timer was measured at boot against one 10 ms
    window of the PIT, and in a VM one slow port read stretched it. One boot measured 2.15 times too high, and
    every second, timeout and animation then took 2.15 seconds (Firefox's start took 14 minutes; the clock was
    122 s behind when the network set it). Now measured over 50 ms with both ends pinned, again if not sure to
    0.1%; the boot log says how sure it was. Even unstalled the old way was 3.4% off.
14m. **A program could have only 256 files and sockets open**, and the kernel panicked when one ran out: Firefox
    needs more, stalled, and its next socket() freed the socket twice. The table grows now, to Linux's limits
    (1024, raised by the program up to 65536); a full table refuses with EMFILE. 17 self-tests.
14n. **The built-in model was read at every boot**, 5 GB from the disc into memory (about 3 minutes on a slow
    boot), whatever mind the companion used. It stays on the disc now unless it is the mind (chosen in
    onboarding, behind the waiting screen there); when the mind is elsewhere and can't be reached, the person
    is asked whether it may stand in, and it goes back to their mind when that answers. tests/standin_vm_win.py
    (15 checks). Not yet on a computer started with UEFI from a USB stick: there the boot loader still brings it,
    as the kernel can't read USB yet (docs/gpu.md, phase 1).
14o. **Out of memory ended whoever needed the next page**, the desktop as often as not. The kernel ends the
    process holding the most memory now (as Linux; oom_score_adj is real, -1000 for the desktop) and the one
    that faulted goes on. 7 self-tests (a program that takes all the memory is ended, the system goes on).

## To fix (in this order)

### Found by using it
14b. **The desktop froze once after closing Notes** (Alt+F4; the log stopped, no key answered), right as the
    built-in model finished loading. Not caught yet: tests/freeze_stress_vm_win.py opens and closes Notes, Files,
    Terminal and Settings over and over with the built-in model loading and after, the kernel's hang reports on;
    61 closes passed (24 of them Notes). Still open until it is caught or it holds over many more runs.

### What every operating system has, and this one doesn't
18. **No way to update the system.** A release is a new image; the system should check for one, show what is
    new and apply it, keeping their things. It needs an installed system first: today it runs from the image
    (a USB stick or a virtual machine's disc), with nothing on the disk to replace. Installing to the disk comes
    with real hardware (docs/gpu.md); updates are built on that, with a way back to the version before.

### Apps (Wayland) that don't fully work
20. **No viewporter or fractional scaling**: apps can't scale or crop their surfaces (video players, browsers).

### Still to look at (each surface used, not just opened)
21. Files, Notes, Terminal, Settings, the companion's window and the tray, used for real tasks in a VM
    (copy and paste between apps, drag and drop, opening a file with an app, renaming, a long file, a full disk,
    no network), and everything found added here.

### Real hardware (planned in docs/gpu.md, phase 1)
22. Printing, Bluetooth, Wi-Fi, battery and brightness for laptops: not on this list until the system boots on
    real hardware; then each is a driver and a Settings page.
