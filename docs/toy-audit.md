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

## To fix (in this order)

### Found by using it
14b. **The desktop froze once after closing Notes** (Alt+F4; the log stopped, no key answered), right as the
    built-in model finished loading. Not seen again in two runs: a stress test (open and close every app many
    times, the kernel's hang reports on) runs until it is caught and fixed.
14c. **The version still says 0.9.5** after all of this: a new version number for the next release.
14d. **Notes is one note**: a list of notes, search in them, and finding text (Ctrl+F) in a note.

### What every operating system has, and this one doesn't
15. **No date and time settings**: time zone, 12/24-hour clock, set automatically from the network.
17. **Settings is missing pages**: Date & time, Users (account, password, lock), Mouse (speed, scrolling, hand),
    Power (screen off after, lock after), Storage (what uses the disk, emptying the trash, caches), Apps
    (installed apps, removing them, default apps for files and links), Notifications (which apps may, Do not
    disturb).
18. **No way to update the system.** A release is a new image; the system should check for one, show what is
    new and apply it, keeping their things.

### Apps (Wayland) that don't fully work
19. **Subsurfaces are accepted but not drawn**: parts of some apps (video, embedded views, some menus) are missing.
20. **No viewporter or fractional scaling**: apps can't scale or crop their surfaces (video players, browsers).

### Still to look at (each surface used, not just opened)
21. Files, Notes, Terminal, Settings, the companion's window and the tray, used for real tasks in a VM
    (copy and paste between apps, drag and drop, opening a file with an app, renaming, a long file, a full disk,
    no network), and everything found added here.

### Real hardware (planned in docs/gpu.md, phase 1)
22. Printing, Bluetooth, Wi-Fi, battery and brightness for laptops: not on this list until the system boots on
    real hardware; then each is a driver and a Settings page.
