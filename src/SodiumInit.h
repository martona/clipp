#pragma once

// Initializes libsodium. Lives in its own TU (not Cli.cpp) so the Windows GUI
// binary -- which compiles no CLI (clipp.com owns that) -- still has exactly one
// definition, shared with the command-line path. Call once on whichever path
// runs; sodium_init() is itself idempotent.
bool InitializeSodium();
