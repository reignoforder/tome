/* SPDX-License-Identifier: MIT-0 */
/* Linked into every mod built with tome_add_mod under MinGW.
 *
 * MinGW's CRT calls _fpreset (fninit) from a TLS callback in every thread
 * started after the DLL loads, which would switch the game's new threads to
 * 64-bit x87 precision; Windows starts them at 53-bit, as they'd run without
 * mods. Defining it here keeps the CRT's version out. Tome does the same
 * for tome.mix (loader/src/dllmain.cpp). */
#ifdef __cplusplus
extern "C"
#endif
void _fpreset(void) {}
