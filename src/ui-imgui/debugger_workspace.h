#ifndef MZ800EMU_DEBUGGER_WORKSPACE_H
#define MZ800EMU_DEBUGGER_WORKSPACE_H

/* SDL frontend: one native debugger host, independently resized from game. */
void debugger_workspace_begin_frame();
const char* debugger_workspace_panel(const char* name);
void debugger_workspace_end_frame();

#endif
