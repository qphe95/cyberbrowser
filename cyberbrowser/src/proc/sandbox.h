#ifndef SANDBOX_H
#define SANDBOX_H

/* The coordinator's side of the sandboxes (SANDBOX_DESIGN.md section 6):
 * the token, AppContainer, mitigations and job each kind of child starts
 * with.
 *
 *   tab    LPAC, an AppContainer profile of its own (made for it, deleted
 *          when it ends), no capabilities; win32k disabled, dynamic code
 *          prohibited; a job of one process, with a memory limit and UI
 *          restrictions.
 *   store  LPAC, no capabilities, with access to browser_data\sites only
 *          (sandbox_grant_folder); win32k disabled.
 *   net    LPAC with the internetClient and privateNetworkClientServer
 *          capabilities, and read access to the replays when they are used
 *          (sandbox_grant_folder); win32k disabled.
 *   loop   the loopback relay: a restricted token at low integrity, which
 *          can read the system's files but not the user's (an AppContainer
 *          can't reach loopback addresses); win32k disabled.
 *   ui     an AppContainer with win32k, for its windows.
 *
 * Every child's process and thread objects carry a medium integrity label,
 * so no sandboxed process (all run at low integrity) can open another. */

#include <stdbool.h>
#include "spawn.h"

typedef enum SandboxKind { SB_TAB, SB_STORE, SB_NET, SB_UI, SB_LOOP } SandboxKind;

/* Set up; false if the sandbox is off (CYBER_NO_SANDBOX=1) or couldn't be. */
bool sandbox_init(void);
bool sandbox_enabled(void);

/* Fill *sb for a child of this kind (a tab: its slot).  False on failure;
 * the caller mustn't start the child then.  Release it after the start. */
bool sandbox_for(SandboxKind kind, int slot, SpawnSandbox *sb);
void sandbox_release(SpawnSandbox *sb);

/* The tab process in `slot` has ended (or never started): delete its
 * profile.  sandbox_end: all of them, when the browser exits. */
void sandbox_tab_ended(int slot);
void sandbox_end(void);

/* Give the storage service (SB_STORE) or the network process (SB_NET)
 * access to `dir` and what it holds: full control if write, else reading. */
bool sandbox_grant_folder(const wchar_t *dir, SandboxKind kind, bool write);

/* Delete every AppContainer profile the browser registered ("cyberbrowser.*":
 * registry entries and folders under %LOCALAPPDATA%\Packages).  Returns how
 * many. */
int sandbox_remove_profiles(void);

#endif
