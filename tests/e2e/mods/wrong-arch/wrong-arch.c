/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Built as a 64-bit DLL by tools/e2e-test.sh to check that Tome rejects it clearly. */
__declspec(dllexport) int tome_mod_load(void* api, void* mod) { (void)api; (void)mod; return 0; }
