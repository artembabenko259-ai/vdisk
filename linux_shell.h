#ifndef LINUX_SHELL_H
#define LINUX_SHELL_H

// Launches an interactive BusyBox (Unix-like) shell rooted on the given vdisk
// drive: BusyBox's "/" maps to the drive root, so the whole environment lives
// on the RAM/VRAM disk. Downloads BusyBox once if needed. Blocks until the user
// exits the shell. Returns 1 on success, 0 on failure.
int run_linux_shell(char drive_letter);

#endif // LINUX_SHELL_H
