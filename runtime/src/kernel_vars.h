#pragma once
#include <cstdint>

// Returns the guest address for an imported kernel/XAM variable (allocating + initialising
// it on first use). Unknown variables get a zeroed block and a warning.
uint32_t KernelVariable(const char* lib, const char* name, uint32_t ordinal);

// Address of a variable already created (0 if the game doesn't import it).
uint32_t KernelVariableAddress(const char* name);

// Object-type markers (ExEventObjectType, ...): used by ObReferenceObjectByHandle.
void KernelVarsStartTimeThread();
