#ifndef PACKAGES_H
#define PACKAGES_H

// A small personal "favorites" list of apk package names for `vdisk linux -s
// <DRIVE> --tools <name>`, stored in %LOCALAPPDATA%\vdisk\packages.txt.
// Empty by default -- purely a convenience/reminder list, not required for
// --tools to work (you can always pass any package name directly).

// Prints the saved list (or a note that it's empty).
void packages_list(void);

// Adds 'name' if not already present.
void packages_add(const char *name);

// Removes 'name' if present.
void packages_del(const char *name);

// Populates the list with a small curated set of well-known, generically
// installable tools (git, curl, htop, ...); existing entries are kept, only
// missing defaults are added.
void packages_set_defaults(void);

#endif // PACKAGES_H
