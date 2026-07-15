/* ft_engine.h — native wrapper around the FocalTech WinBio matcher DLL.
 * Loads ftWbioEngineAdapter.dll in-process (no Wine) and exposes enroll/verify.
 * Single-device (global state); fine for one FT9201 reader. */
#ifndef FT_ENGINE_H
#define FT_ENGINE_H
#include <stddef.h>
#include <stdint.h>

/* Load + initialize the engine. dll_path = path to ftWbioEngineAdapter.dll.
 * Returns 0 on success. */
int  ft_engine_open (const char *dll_path);
void ft_engine_close (void);

/* The image geometry the engine matches at (64x80). Feed any size; it is
 * resized internally. */
void ft_engine_geometry (int *w, int *h);

/* Feed one 8-bit grayscale frame (sw x sh). purpose: 4=enroll, 1=verify.
 * Returns 0 on success (features extracted), else the engine HRESULT. */
uint32_t ft_engine_accept (const uint8_t *img, int sw, int sh, uint8_t purpose);

/* Enrollment: begin, then per accepted frame call update, then commit. */
void ft_engine_enroll_begin (void);
/* Fold the last accepted frame into the enroll template.
 * Returns: 0 = complete, 1 = need more samples, 2 = frame rejected. */
int  ft_engine_enroll_update (void);
/* Commit the enrollment. On success (*out,*outlen) is a malloc'd template blob
 * the caller must free. Returns 0 on success. */
int  ft_engine_enroll_commit (uint8_t **out, size_t *outlen);

/* Verify: load a previously committed template, then match it against the
 * most recently accepted frame. Returns 1 = match, 0 = no match. */
int  ft_engine_verify (const uint8_t *tmpl, size_t tmpllen);

#endif
