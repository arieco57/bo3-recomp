#pragma once
// XMA decoder emulation (xma.cpp).
void XmaInit();                                   // after MemoryInit, before guest code runs
bool XmaHandleFault(void* addr, void* ucontext);  // SIGSEGV: guest store to the XMA registers
void XmaPollFull();                           // audio driver, after each mixing frame: release rings the game is reading
void XmaWaitIdle(int maxUs);                      // audio driver: let the decoder catch up between mixing frames
