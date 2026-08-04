#ifndef BRIDGE_H
#define BRIDGE_H

#include <stddef.h>

// The Windows<->Linux file bridge lives at a fixed real folder
// (%LOCALAPPDATA%\vdisk\Bridge), NOT on a WinFsp RAM/VRAM disk: Windows'
// SMB server runs in a different session than the one that mounts a WinFsp
// drive, and an elevated process (needed to create the SMB share) can't see
// drives mounted by the unprivileged user session either -- both dead ends,
// confirmed empirically. A plain NTFS folder has none of those problems and
// the share only needs to be created once, ever.

// Ensures the bridge folder exists and its SMB share is set up (scoped to the
// current user only). Requires Administrator -- caller must already be
// elevated. Fills 'share_name'. Returns 1 on success.
int bridge_ensure_share(char *share_name, size_t share_name_sz);

// Returns the bridge folder's real Windows path (creating it if needed), e.g.
// "C:\Users\<user>\AppData\Local\vdisk\Bridge", so the caller can tell the
// user where to drop files on the Windows side.
int bridge_folder_path(char *out, size_t out_sz);

// Prompts (masked, no echo) on the console for the current Windows user's
// password -- used only to authenticate the guest's SMB mount for this one
// session; never written to disk or logged. Returns 1 if a non-empty
// password was entered.
int bridge_prompt_password(char *out, size_t out_sz);

// Appends shell commands to 'cmd' (must already hold a valid, null-terminated
// shell fragment) that install cifs-utils and mount //10.0.2.2/<share_name>
// at /mnt/win inside the guest, using a short-lived credentials file (deleted
// right after mounting, so the password never lingers in `ps` output or a
// permanent file). Assumes networking is already up.
void bridge_append_mount_cmd(char *cmd, size_t cmd_sz, const char *share_name,
                              const char *username, const char *password);

#endif // BRIDGE_H
