/*
 * EgisTec EH577 (LighTuning 1c7a:0577) USB fingerprint reader driver for libfprint
 *
 * Copyright (C) 2026 Grant Garrison
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Match-on-host via the vendor engine (EgisTouchFPEngine0577.dll), loaded
 * in-process by eg_engine.c. Transport is the EGIS/SIGE bulk protocol on
 * EP 0x01 (OUT) / 0x82 (IN). Frames are 3990 bytes = 70x57 8bpp.
 *
 * DEVICE SAFETY: this sensor is the chassis power button and CANNOT be
 * unplugged; USB reset() and set_configuration() both WEDGE it (proven), so
 * this driver NEVER calls either — it only claims the interface and does clean
 * bulk request/response pairs, fully draining every IN response (a large frame
 * response left undrained desyncs the next command — THE key device lesson).
 *
 * Init overview:
 *   - 99-command deterministic init (EH577_INIT[], from the vendor capture).
 *     Each: bulk-OUT command on 0x01, then bulk-IN drain on 0x82.
 *   - Per-frame capture wrapper: ARM(61 2d 13) / STATUS(60 00 13) /
 *     GRAB(64 0f 96 -> 3990B frame) / DISARM(61 2d 20).
 *   - Finger presence detected by frame mean (baseline ~82, finger ~200-240);
 *     mean > EH577_FINGER_MEAN => finger present.
 */

#define FP_COMPONENT "eh577"
#include "fpi-log.h"
#include "drivers_api.h"

#include "eh577_init.h"
#include "eg_engine.h"
#include <math.h>

#define CTRL_TIMEOUT   5000
#define BULK_TIMEOUT   5000
#define DRAIN_LEN      4096   /* >= max response (3990) so any IN drains to its short packet */

/* ------------------------------------------------------------------ */
/* Device struct                                                        */
/* ------------------------------------------------------------------ */

struct _FpiDeviceEh577
{
  FpDevice  parent;

  FpiSsm   *loop_ssm;
  guint     init_idx;          /* current index into EH577_INIT[] */

  guint8   *image_buf;         /* last captured frame (EH577_FRAME_LEN) */
  guint     last_mean;         /* mean of last grabbed frame */
  guint     grab_count;        /* diagnostic: total frames grabbed */

  guint     enroll_done;       /* enrollment stages completed (UI, capped at nr) */
  guint     enroll_frames;     /* total frames folded/attempted this enrollment */
  FpPrint  *cur_print;         /* FpPrint being enrolled / verified against */
  gboolean  is_verify;
  gboolean  scan_ok;           /* set by the grab eval: awaited event happened */
  gboolean  await_release;     /* enroll: waiting for the finger to LIFT between stages */

  gboolean  engine_ok;         /* vendor engine loaded */
  guint8   *tmpl_blob;         /* committed engine template (verify) */
  gsize     tmpl_len;
};

G_DECLARE_FINAL_TYPE (FpiDeviceEh577, fpi_device_eh577,
                      FPI, DEVICE_EH577, FpDevice)
G_DEFINE_TYPE (FpiDeviceEh577, fpi_device_eh577, FP_TYPE_DEVICE)

/* ================================================================== */
/* Bulk helpers (EGIS/SIGE)                                            */
/* ================================================================== */

/* generic bulk-OUT completion: just advance */
static void
bulk_out_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  if (err) fpi_ssm_mark_failed (t->ssm, err);
  else     fpi_ssm_next_state (t->ssm);
}

/* generic bulk-IN drain completion: discard the response, advance */
static void
bulk_drain_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  if (err) fpi_ssm_mark_failed (t->ssm, err);
  else     fpi_ssm_next_state (t->ssm);
}

/* submit a bulk-OUT of `len` bytes copied from `data` (freed after transfer) */
static void
bulk_out (FpiSsm *ssm, FpDevice *dev, const guint8 *data, gsize len)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);
  guint8 *buf = g_malloc (len);
  memcpy (buf, data, len);
  t->ssm = ssm;
  fpi_usb_transfer_fill_bulk_full (t, EH577_EP_OUT, buf, len, g_free);
  fpi_usb_transfer_submit (t, BULK_TIMEOUT,
    fpi_device_get_cancellable (dev), bulk_out_cb, NULL);
}

/* submit a bulk-IN of up to `len` bytes; completes on the response's short
 * packet, so a 7-byte SIGE and a 3990-byte frame both drain in one transfer */
static void
bulk_in (FpiSsm *ssm, FpDevice *dev, gsize len, FpiUsbTransferCallback cb)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);
  t->ssm = ssm;
  fpi_usb_transfer_fill_bulk (t, EH577_EP_IN, len);
  fpi_usb_transfer_submit (t, BULK_TIMEOUT,
    fpi_device_get_cancellable (dev), cb, NULL);
}

/* ================================================================== */
/* Init state machine — drive EH577_INIT[99] as OUT/drain pairs        */
/* ================================================================== */

enum {
  INIT_OUT,     /* bulk-OUT EH577_INIT[init_idx] */
  INIT_IN,      /* bulk-IN drain; loop until all 99 sent */
  INIT_NUM,
};

static void
init_in_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  FpiDeviceEh577 *self = FPI_DEVICE_EH577 (dev);
  if (err) { fpi_ssm_mark_failed (t->ssm, err); return; }

  self->init_idx++;
  if (self->init_idx < G_N_ELEMENTS (EH577_INIT))
    fpi_ssm_jump_to_state (t->ssm, INIT_OUT);
  else
    fpi_ssm_mark_completed (t->ssm);
}

static void
init_ssm_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceEh577 *self = FPI_DEVICE_EH577 (dev);

  if (g_cancellable_is_cancelled (fpi_device_get_cancellable (dev)))
    {
      fpi_ssm_mark_completed (ssm);
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case INIT_OUT:
      {
        const Eh577Cmd *c = &EH577_INIT[self->init_idx];
        bulk_out (ssm, dev, c->cmd, c->len);
      }
      break;

    case INIT_IN:
      bulk_in (ssm, dev, DRAIN_LEN, init_in_cb);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
init_ssm_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceEh577 *self = FPI_DEVICE_EH577 (dev);

  if (!error)
    {
      self->image_buf = g_malloc0 (EH577_FRAME_LEN);

      const char *dll = g_getenv ("EH577_ENGINE_DLL");
      if (!dll)
        dll = "/usr/lib/libfprint-2/EgisTouchFPEngine0577.dll";
      self->engine_ok = (ft_engine_open (dll) == 0);
      if (!self->engine_ok)
        fp_warn ("eh577: vendor engine failed to load from %s "
                 "(set EH577_ENGINE_DLL to override)", dll);
      else
        fp_info ("eh577: vendor matching engine loaded");
    }
  fpi_device_open_complete (dev, error);
}

/* ================================================================== */
/* Scan state machine — poll the capture wrapper until a finger frame  */
/* ================================================================== */

enum {
  SCAN_POLL_DELAY,   /* inter-poll delay; loops here until a finger is present */
  SCAN_ARM_OUT, SCAN_ARM_IN,
  SCAN_STAT_OUT, SCAN_STAT_IN,
  SCAN_EXPOSE,       /* let the sensor integrate the finger before grabbing */
  SCAN_GRAB_OUT, SCAN_GRAB_IN,   /* GRAB_IN reads the 3990B frame + computes mean */
  SCAN_DISARM_OUT, SCAN_DISARM_IN,
  SCAN_EVAL,         /* finger present? complete : poll again */
  SCAN_NUM,
};

static void
grab_in_cb (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  FpiDeviceEh577 *self = FPI_DEVICE_EH577 (dev);
  if (err) { fpi_ssm_mark_failed (t->ssm, err); return; }

  if (t->actual_length >= EH577_FRAME_LEN)
    {
      memcpy (self->image_buf, t->buffer, EH577_FRAME_LEN);
      guint64 sum = 0;
      for (gsize i = 0; i < EH577_FRAME_LEN; i++)
        sum += self->image_buf[i];
      self->last_mean = (guint) (sum / EH577_FRAME_LEN);
    }
  else
    {
      fp_warn ("eh577: short frame read: %zd < %d",
               (ssize_t) t->actual_length, EH577_FRAME_LEN);
      self->last_mean = 0;   /* treat as no-finger */
    }
  self->grab_count++;
  if ((self->grab_count % 15) == 0)
    fp_dbg ("eh577: grab #%u len=%zd mean=%u", self->grab_count,
            (ssize_t) t->actual_length, self->last_mean);
  fpi_ssm_next_state (t->ssm);
}

static void
scan_ssm_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceEh577 *self = FPI_DEVICE_EH577 (dev);

  if (g_cancellable_is_cancelled (fpi_device_get_cancellable (dev)))
    {
      fpi_ssm_mark_completed (ssm);
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SCAN_POLL_DELAY:
      /* GENTLE cadence: the analog capture-arm degrades under rapid arm/grab/disarm
       * cycling (~hundreds of cycles burns it). Poll slowly while waiting for a
       * finger so an empty wait doesn't burn the arm before the user presses.
       * ~700ms => ~1.4 cycles/s; a 15s wait is ~20 cycles, a full enroll ~35 total,
       * well under the burn threshold. */
      fpi_ssm_next_state_delayed (ssm, 700);
      break;

    case SCAN_ARM_OUT:    bulk_out (ssm, dev, EH577_ARM, sizeof EH577_ARM); break;
    case SCAN_ARM_IN:     bulk_in (ssm, dev, DRAIN_LEN, bulk_drain_cb);      break;
    case SCAN_STAT_OUT:   bulk_out (ssm, dev, EH577_STATUS, sizeof EH577_STATUS); break;
    case SCAN_STAT_IN:    bulk_in (ssm, dev, DRAIN_LEN, bulk_drain_cb);      break;
    case SCAN_EXPOSE:     fpi_ssm_next_state_delayed (ssm, 100); break;
    case SCAN_GRAB_OUT:   bulk_out (ssm, dev, EH577_GRAB, sizeof EH577_GRAB); break;
    case SCAN_GRAB_IN:    bulk_in (ssm, dev, EH577_FRAME_LEN, grab_in_cb);   break;
    case SCAN_DISARM_OUT: bulk_out (ssm, dev, EH577_DISARM, sizeof EH577_DISARM); break;
    case SCAN_DISARM_IN:  bulk_in (ssm, dev, DRAIN_LEN, bulk_drain_cb);      break;

    case SCAN_EVAL:
      {
        gboolean present = self->last_mean > EH577_FINGER_MEAN;
        /* Awaiting release => complete when the finger LIFTS; otherwise when it PRESSES. */
        gboolean done = self->await_release ? !present : present;
        if (done)
          {
            self->scan_ok = TRUE;
            fp_dbg ("eh577: %s (mean=%u)",
                    self->await_release ? "finger lifted" : "finger frame captured",
                    self->last_mean);
            fpi_ssm_mark_completed (ssm);
          }
        else
          fpi_ssm_jump_to_state (ssm, SCAN_POLL_DELAY);   /* keep polling gently */
      }
      break;

    default:
      g_assert_not_reached ();
    }
}

/* ================================================================== */
/* Scan completion → enroll / verify via the vendor engine             */
/* ================================================================== */

static void start_scan (FpDevice *dev);

static void
scan_ssm_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceEh577 *self = FPI_DEVICE_EH577 (dev);
  self->loop_ssm = NULL;

  if (error)
    {
      if (self->is_verify)
        fpi_device_verify_complete (dev, error);
      else
        fpi_device_enroll_complete (dev, NULL, error);
      return;
    }

  if (!self->scan_ok)
    {
      /* No finger — for verify, silently re-scan; for enroll, ask to center. */
      if (self->is_verify) { start_scan (dev); return; }
      GError *retry = fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER);
      fpi_device_enroll_progress (dev, self->enroll_done, NULL, g_steal_pointer (&retry));
      start_scan (dev);
      return;
    }

  /* Between enroll stages we wait for the finger to LIFT (a natural press-lift-
   * press rhythm gives distinct sub-images and avoids duplicate frames). It just
   * lifted — go wait for the next fresh press. */
  if (self->await_release)
    {
      self->await_release = FALSE;
      start_scan (dev);
      return;
    }

  if (!self->engine_ok)
    {
      GError *e = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                            "eh577: vendor engine not loaded");
      if (self->is_verify)
        fpi_device_verify_complete (dev, e);
      else
        fpi_device_enroll_complete (dev, NULL, e);
      return;
    }

  /* Feed the raw 70x57 frame to the vendor engine. IMPORTANT: unlike FT9201, the
   * EGIS engine's AcceptSampleData returns a SOFT code (0x80098008 = duplicate /
   * low-quality / need-more) even on perfectly good frames during enrollment — it
   * is NOT a hard reject. Bailing to CENTER_FINGER on nonzero (as FT9201 does)
   * loops forever on good prints. So proceed regardless and let enroll_update /
   * verify drive the outcome — this mirrors the proven offline enroll path. */
  uint32_t ahr = ft_engine_accept (self->image_buf,
                                    EH577_FRAME_W, EH577_FRAME_H,
                                    self->is_verify ? 1 : 4);
  /* The soft accept code (0x80098008) is normal for both enroll and verify — the
   * engine still extracts a usable feature set. Proceed regardless; the earlier
   * verify SIGSEGV was the too-small storage vtable (fixed in eg_engine.c), NOT an
   * incomplete extraction, so no verify-side re-scan gate is needed. */
  if (ahr != 0)
    fp_dbg ("eh577: accept hr=0x%08x (soft, proceeding)", ahr);

  if (self->is_verify)
    {
      int match = ft_engine_verify (self->tmpl_blob, self->tmpl_len);
      fp_info ("eh577: engine verify -> %s", match ? "MATCH" : "no match");
      fpi_device_verify_report (dev,
                                match ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                                NULL, NULL);
      fpi_device_verify_complete (dev, NULL);
      return;
    }

  /* Enrollment: fold this frame. enroll_update: 0 = engine COMPLETE, 1 = need more,
   * 2 = frame rejected (duplicate/low-quality). Commit ONLY when the engine reports
   * complete — committing at a fixed UI stage count fails, because the engine has
   * its own minimum-coverage requirement (needs ~12 folded frames from a continuous
   * hold before it will build a template). Safety-cap so the loop can't run forever. */
  int st = ft_engine_enroll_update ();
  self->enroll_frames++;
  guint nr = FP_DEVICE_GET_CLASS (dev)->nr_enroll_stages;

  if (st == 2)
    {
      /* duplicate / low-quality — don't advance; ask for a fresh press */
      if (self->enroll_frames < 80)
        {
          GError *retry = fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER);
          fpi_device_enroll_progress (dev, self->enroll_done, NULL, g_steal_pointer (&retry));
          self->await_release = TRUE;
          start_scan (dev);
          return;
        }
    }
  else
    {
      /* a frame was folded — advance UI progress (capped at nr stages) */
      if (self->enroll_done < nr) self->enroll_done++;
      fpi_device_enroll_progress (dev, self->enroll_done, NULL, NULL);
      /* keep folding until the ENGINE reports complete (st==0). Do NOT force a lift
       * here: the engine finalizes from CONTINUOUS coverage of a held finger (the
       * offline path completes at ~12 such frames), so a sustained hold feeds it to
       * completion. A lift/reposition is only requested when the engine says the
       * current position is saturated (st==2, handled above). */
      if (st != 0 && self->enroll_frames < 80)
        {
          start_scan (dev);
          return;
        }
    }

  /* Enough coverage (or engine reports complete) — commit the template. */
  uint8_t *blob = NULL;
  size_t   blen = 0;
  if (ft_engine_enroll_commit (&blob, &blen) != 0)
    {
      fpi_device_enroll_complete (dev, NULL,
        fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                  "eh577: engine enroll commit failed"));
      return;
    }

  GVariant *arr = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, blob, blen, 1);
  GVariant *data = g_variant_new ("(@ay)", arr);
  fpi_print_set_type (self->cur_print, FPI_PRINT_RAW);
  g_object_set (self->cur_print, "fpi-data", data, NULL);
  fp_info ("eh577: enrollment committed, template %zu bytes", blen);
  free (blob);

  fpi_device_enroll_complete (dev, g_object_ref (self->cur_print), NULL);
}

/* ================================================================== */
/* FpDevice vfuncs                                                     */
/* ================================================================== */

static void
start_scan (FpDevice *dev)
{
  FpiDeviceEh577 *self = FPI_DEVICE_EH577 (dev);
  self->scan_ok = FALSE;
  self->loop_ssm = fpi_ssm_new (dev, scan_ssm_run_state, SCAN_NUM);
  fpi_ssm_start (self->loop_ssm, scan_ssm_complete);
}

static void
dev_cancel (FpDevice *dev)
{
  /* USB transfers hold fpi_device_get_cancellable(); they abort automatically. */
}

static void
dev_open (FpDevice *dev)
{
  FpiDeviceEh577 *self = FPI_DEVICE_EH577 (dev);
  GError *error = NULL;
  FpiSsm *ssm;

  /* Claim only — NEVER reset or set_configuration (both wedge this device). */
  g_usb_device_claim_interface (fpi_device_get_usb_device (dev), 0, 0, &error);
  if (error) { fpi_device_open_complete (dev, error); return; }

  self->init_idx = 0;
  ssm = fpi_ssm_new (dev, init_ssm_run_state, INIT_NUM);
  fpi_ssm_start (ssm, init_ssm_complete);
}

static void
dev_close (FpDevice *dev)
{
  FpiDeviceEh577 *self = FPI_DEVICE_EH577 (dev);
  GError *error = NULL;

  g_clear_pointer (&self->image_buf, g_free);
  g_clear_pointer (&self->tmpl_blob, g_free);
  /* ft_engine_close() is a no-op: the vendor DLL maps once per process at a
   * fixed base and must survive device close/reopen. */
  if (self->engine_ok)
    ft_engine_close ();
  /* release only — NO reset */
  g_usb_device_release_interface (fpi_device_get_usb_device (dev), 0, 0, &error);
  fpi_device_close_complete (dev, error);
}

static void
dev_enroll (FpDevice *dev)
{
  FpiDeviceEh577 *self = FPI_DEVICE_EH577 (dev);

  self->is_verify    = FALSE;
  self->enroll_done  = 0;
  self->enroll_frames = 0;
  self->await_release = FALSE;

  if (self->engine_ok)
    ft_engine_enroll_begin ();

  fpi_device_get_enroll_data (dev, &self->cur_print);
  start_scan (dev);
}

static void
dev_verify (FpDevice *dev)
{
  FpiDeviceEh577 *self = FPI_DEVICE_EH577 (dev);

  fpi_device_get_verify_data (dev, &self->cur_print);

  GVariant *data = NULL;
  g_object_get (self->cur_print, "fpi-data", &data, NULL);
  if (!data || !g_variant_check_format_string (data, "(@ay)", FALSE))
    {
      if (data) g_variant_unref (data);
      fpi_device_verify_complete (dev,
        fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                  "eh577: stored template invalid"));
      return;
    }

  GVariant *arr = NULL;
  g_variant_get (data, "(@ay)", &arr);
  gsize stored_len = 0;
  const guint8 *stored = g_variant_get_fixed_array (arr, &stored_len, 1);

  g_clear_pointer (&self->tmpl_blob, g_free);
  self->tmpl_blob = g_malloc (stored_len);
  memcpy (self->tmpl_blob, stored, stored_len);
  self->tmpl_len = stored_len;
  g_variant_unref (arr);
  g_variant_unref (data);

  self->is_verify = TRUE;
  self->await_release = FALSE;
  start_scan (dev);
}

/* ================================================================== */
/* Driver registration                                                 */
/* ================================================================== */

static const FpIdEntry id_table[] = {
  { .vid = 0x1c7a, .pid = 0x0577 },
  { .vid = 0,      .pid = 0      },
};

static void
fpi_device_eh577_init (FpiDeviceEh577 *self)
{
}

static void
fpi_device_eh577_class_init (FpiDeviceEh577Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id               = "eh577";
  dev_class->full_name        = "EgisTec EH577 Fingerprint Reader";
  dev_class->type             = FP_DEVICE_TYPE_USB;
  dev_class->id_table         = id_table;
  dev_class->nr_enroll_stages = 8;
  dev_class->scan_type        = FP_SCAN_TYPE_PRESS;
  dev_class->temp_hot_seconds = -1;

  dev_class->open    = dev_open;
  dev_class->close   = dev_close;
  dev_class->enroll  = dev_enroll;
  dev_class->verify  = dev_verify;
  dev_class->cancel  = dev_cancel;

  fpi_device_class_auto_initialize_features (dev_class);
}
