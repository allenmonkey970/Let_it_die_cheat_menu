#pragma once
// In-process memory scanner, controlled by a file-RPC channel so the agent can
// operate the reverse-engineering while the user just plays the game.
void Scanner_Start();
// Number of currently frozen addresses (for the overlay to display).
int  Scanner_FreezeCount();
// Last status line (for the overlay to display).
const char* Scanner_LastStatus();
