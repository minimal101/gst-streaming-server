/* GStreamer Streaming Server
 * Copyright (C) 2013 Rdio Inc <ingestions@rd.io>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
/*
 *
 * http://msdn.microsoft.com/en-us/library/ff469518.aspx
 *
 */

#include "config.h"

#include <gst/gst.h>
#include <gst/base/gstbytereader.h>
#include <glib/gstdio.h>

#include "gss-adaptive.h"
#include "gss-server.h"
#include "gss-html.h"
#include "gss-session.h"
#include "gss-soup.h"
#include "gss-content.h"
#include "gss-isom.h"
#include "gss-playready.h"
#include "gss-utils.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <openssl/aes.h>

#define AUDIO_TRACK_ID 1
#define VIDEO_TRACK_ID 2

#define GSS_ISM_SECOND 10000000

static void gss_adaptive_resource_get_manifest (GssTransaction * t,
    GssAdaptive * adaptive);
static void gss_adaptive_resource_get_content (GssTransaction * t,
    GssAdaptive * adaptive);
static void load_file (GssAdaptive * adaptive, const char *filename,
    int video_bitrate, int audio_bitrate);

static GssDrmType global_drm = GSS_DRM_CLEAR;


static guint8 *
gss_adaptive_assemble_chunk (GssTransaction * t, GssAdaptive * adaptive,
    GssAdaptiveLevel * level, GssIsomFragment * fragment)
{
  guint8 *mdat_data;
  off_t ret;
  int fd;
  ssize_t n;
  int i;
  int offset;

  fd = open (level->filename, O_RDONLY);
  if (fd < 0) {
    GST_WARNING ("file not found %s", level->filename);
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return NULL;
  }

  mdat_data = g_malloc (fragment->mdat_size);

  GST_WRITE_UINT32_BE (mdat_data, fragment->mdat_size);
  GST_WRITE_UINT32_LE (mdat_data + 4, GST_MAKE_FOURCC ('m', 'd', 'a', 't'));
  offset = 8;
  for (i = 0; i < fragment->n_mdat_chunks; i++) {
    GST_DEBUG ("chunk %d: %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT,
        i, fragment->chunks[i].offset, fragment->chunks[i].size);
    ret = lseek (fd, fragment->chunks[i].offset, SEEK_SET);
    if (ret < 0) {
      GST_WARNING ("failed to seek");
      soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
      close (fd);
      return NULL;
    }

    n = read (fd, mdat_data + offset, fragment->chunks[i].size);
    if (n < fragment->chunks[i].size) {
      GST_WARNING ("read failed");
      g_free (mdat_data);
      soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
      close (fd);
      return NULL;
    }
    offset += fragment->chunks[i].size;
  }
  close (fd);

  return mdat_data;
}


static void
gss_adaptive_send_chunk (GssTransaction * t, GssAdaptive * adaptive,
    GssAdaptiveLevel * level, GssIsomFragment * fragment, guint8 * mdat_data)
{
  soup_message_set_status (t->msg, SOUP_STATUS_OK);
  /* strip off mdat header at end of moof_data */
  soup_message_body_append (t->msg->response_body, SOUP_MEMORY_TAKE,
      fragment->moof_data, fragment->moof_size - 8);
  soup_message_body_append (t->msg->response_body, SOUP_MEMORY_TAKE, mdat_data,
      fragment->mdat_size);
}

static char *
get_codec_string (guint8 * codec_data, int len)
{
  char *s;
  int i;

  if (codec_data == NULL)
    return g_strdup ("");

  s = g_malloc (len * 2 + 1);
  for (i = 0; i < len; i++) {
    sprintf (s + i * 2, "%02x", codec_data[i]);
  }
  return s;
}

static void
gss_adaptive_resource_get_manifest (GssTransaction * t, GssAdaptive * adaptive)
{
  GString *s = g_string_new ("");
  int i;

  t->s = s;

  GSS_A ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");

  GSS_P
      ("<SmoothStreamingMedia MajorVersion=\"2\" MinorVersion=\"1\" Duration=\"%"
      G_GUINT64_FORMAT "\">\n", adaptive->duration);
  GSS_P
      ("  <StreamIndex Type=\"video\" Name=\"video\" Chunks=\"%d\" QualityLevels=\"%d\" MaxWidth=\"%d\" MaxHeight=\"%d\" "
      "DisplayWidth=\"%d\" DisplayHeight=\"%d\" "
      "Url=\"content?stream=video&amp;bitrate={bitrate}&amp;start_time={start time}\">\n",
      adaptive->video_levels[0].n_fragments, adaptive->n_video_levels,
      adaptive->max_width, adaptive->max_height, adaptive->max_width,
      adaptive->max_height);
  /* also IsLive, LookaheadCount, DVRWindowLength */

  for (i = 0; i < adaptive->n_video_levels; i++) {
    GssAdaptiveLevel *level = &adaptive->video_levels[i];

    GSS_P ("    <QualityLevel Index=\"%d\" Bitrate=\"%d\" "
        "FourCC=\"H264\" MaxWidth=\"%d\" MaxHeight=\"%d\" "
        "CodecPrivateData=\"%s\" />\n", i, level->bitrate, level->video_width,
        level->video_height, level->codec_data);
  }
  {
    GssAdaptiveLevel *level = &adaptive->video_levels[0];

    for (i = 0; i < level->n_fragments; i++) {
      GssIsomFragment *fragment;
      fragment = gss_isom_track_get_fragment (level->track, i);
      GSS_P ("    <c d=\"%" G_GUINT64_FORMAT "\" />\n",
          (guint64) fragment->duration);
    }
  }
  GSS_A ("  </StreamIndex>\n");

  GSS_P ("  <StreamIndex Type=\"audio\" Index=\"0\" Name=\"audio\" "
      "Chunks=\"%d\" QualityLevels=\"%d\" "
      "Url=\"content?stream=audio&amp;bitrate={bitrate}&amp;start_time={start time}\">\n",
      adaptive->audio_levels[0].n_fragments, adaptive->n_audio_levels);
  for (i = 0; i < adaptive->n_audio_levels; i++) {
    GssAdaptiveLevel *level = &adaptive->audio_levels[i];

    GSS_P ("    <QualityLevel FourCC=\"AACL\" Bitrate=\"%d\" "
        "SamplingRate=\"%d\" Channels=\"2\" BitsPerSample=\"16\" "
        "PacketSize=\"4\" AudioTag=\"255\" CodecPrivateData=\"%s\" />\n",
        level->bitrate, level->audio_rate, level->codec_data);
    break;
  }
  {
    GssAdaptiveLevel *level = &adaptive->audio_levels[0];

    for (i = 0; i < level->n_fragments; i++) {
      GssIsomFragment *fragment;
      fragment = gss_isom_track_get_fragment (level->track, i);
      GSS_P ("    <c d=\"%" G_GUINT64_FORMAT "\" />\n",
          (guint64) fragment->duration);
    }
  }

  GSS_A ("  </StreamIndex>\n");
  if (global_drm == GSS_DRM_PLAYREADY) {
    char *prot_header_base64;

    GSS_A ("<Protection>\n");
    GSS_A ("  <ProtectionHeader "
        "SystemID=\"9a04f079-9840-4286-ab92-e65be0885f95\">");

    prot_header_base64 = gss_playready_get_protection_header_base64 (adaptive,
        t->server->playready->license_url);
    GSS_P ("%s", prot_header_base64);
    g_free (prot_header_base64);

    GSS_A ("</ProtectionHeader>\n");
    GSS_A ("</Protection>\n");
  }
  GSS_A ("</SmoothStreamingMedia>\n");

}

static void
append_content_protection (GssTransaction * t, GssAdaptive * adaptive)
{
  GString *s = t->s;

  if (global_drm == GSS_DRM_PLAYREADY) {
    char *prot_header_base64;
    GSS_A ("      <ContentProtection "
        "schemeIdUri=\"urn:uuid:9a04f079-9840-4286-ab92-e65be0885f95\">\n");
    prot_header_base64 =
        gss_playready_get_protection_header_base64 (adaptive,
        t->server->playready->license_url);
    GSS_P ("        <mspr:pro>%s</mspr:pro>\n", prot_header_base64);
    g_free (prot_header_base64);
    GSS_A ("      </ContentProtection>\n");
  }

}

static void
gss_adaptive_resource_get_dash_range_mpd (GssTransaction * t,
    GssAdaptive * adaptive)
{
  GString *s = g_string_new ("");
  int i;

  t->s = s;

  soup_message_headers_replace (t->msg->response_headers, "Content-Type",
      "application/octet-stream");
  soup_message_headers_replace (t->msg->response_headers,
      "Access-Control-Allow-Origin", "*");

  GSS_A ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  GSS_A ("<MPD xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
      "  xmlns=\"urn:mpeg:dash:schema:mpd:2011\"\n");
  if (global_drm == GSS_DRM_PLAYREADY) {
    GSS_A ("  xmlns:mspr=\"urn:microsoft:playready\"\n");
  }
  GSS_P ("  xsi:schemaLocation=\"urn:mpeg:dash:schema:mpd:2011 DASH-MPD.xsd\"\n"
      "  type=\"static\"\n"
      "  mediaPresentationDuration=\"PT%dS\"\n"
      "  minBufferTime=\"PT2S\"\n"
      "  profiles=\"urn:mpeg:dash:profile:isoff-on-demand:2011\">\n",
      (int) (adaptive->duration / GSS_ISM_SECOND));
  GSS_P ("  <Period>\n");

  GSS_A ("    <AdaptationSet mimeType=\"audio/mp4\" "
      "lang=\"en\" "
      "subsegmentAlignment=\"true\" " "subsegmentStartsWithSAP=\"1\">\n");
  append_content_protection (t, adaptive);
  for (i = 0; i < adaptive->n_audio_levels; i++) {
    GssAdaptiveLevel *level = &adaptive->audio_levels[i];
    GssIsomTrack *track = level->track;

    GSS_P ("      <Representation id=\"a%d\" codecs=\"%s\" bandwidth=\"%d\">\n",
        i, level->codec, level->bitrate);
    GSS_P ("        <BaseURL>content-range/a%d</BaseURL>\n", i);
    GSS_P ("        <SegmentBase indexRange=\"%" G_GSIZE_FORMAT "-%"
        G_GSIZE_FORMAT "\">" "<Initialization range=\"%" G_GSIZE_FORMAT "-%"
        G_GSIZE_FORMAT "\" /></SegmentBase>\n", track->dash_header_size,
        track->dash_header_and_sidx_size - 1, (gsize) 0,
        track->dash_header_size - 1);
    GSS_A ("      </Representation>\n");
    break;
  }
  GSS_A ("    </AdaptationSet>\n");

  GSS_A ("    <AdaptationSet mimeType=\"video/mp4\" "
      "subsegmentAlignment=\"true\" " "subsegmentStartsWithSAP=\"1\">\n");
  append_content_protection (t, adaptive);
  for (i = 0; i < adaptive->n_video_levels; i++) {
    GssAdaptiveLevel *level = &adaptive->video_levels[i];
    GssIsomTrack *track = level->track;

    GSS_P ("      <Representation id=\"v%d\" bandwidth=\"%d\" "
        "codecs=\"%s\" width=\"%d\" height=\"%d\">\n",
        i, level->bitrate, level->codec,
        level->video_width, level->video_height);
    GSS_P ("        <BaseURL>content-range/v%d</BaseURL>\n", i);
    GSS_P ("        <SegmentBase indexRange=\"%" G_GSIZE_FORMAT "-%"
        G_GSIZE_FORMAT "\">" "<Initialization range=\"%" G_GSIZE_FORMAT "-%"
        G_GSIZE_FORMAT "\" /></SegmentBase>\n", track->dash_header_size,
        track->dash_header_and_sidx_size - 1, (gsize) 0,
        track->dash_header_size - 1);
    GSS_A ("      </Representation>\n");
    break;
  }
  GSS_A ("    </AdaptationSet>\n");

  GSS_A ("  </Period>\n");
  GSS_A ("</MPD>\n");
  GSS_A ("\n");

}

static gboolean
ranges_overlap (guint64 start1, guint64 len1, guint64 start2, guint64 len2)
{
  if (start1 + len1 <= start2 || start2 + len2 <= start1)
    return FALSE;
  return TRUE;
}

#if 0
static guint64
overlap_size (guint64 start1, guint64 size1, guint64 start2, guint64 size2)
{
  guint64 start;
  guint64 end;

  start = MAX (start1, start2);
  end = MIN (start1 + size1, start2 + size2);
  if (start < end) {
    return end - start;
  }
  return 0;
}
#endif

static void
gss_soup_message_body_append_clipped (SoupMessageBody * body,
    SoupMemoryUse use, guint8 * data, guint64 start1, guint64 size1,
    guint64 start2, guint64 size2)
{
  guint64 start;
  guint64 end;
  guint64 offset;

  start = MAX (start1, start2);
  end = MIN (start1 + size1, start2 + size2);
  if (start >= end)
    return;

  offset = start - start2;
  soup_message_body_append (body, use, data + offset, end - start);
}

static void
gss_adaptive_resource_get_dash_range_fragment (GssTransaction * t,
    GssAdaptive * adaptive, const char *path)
{
  gboolean have_range;
  SoupRange *ranges;
  int n_ranges;
  int index;
  GssAdaptiveLevel *level;
  gsize start, end;
  int i;
  guint64 offset;
  guint64 n_bytes;
  guint64 header_size;

  soup_message_headers_replace (t->msg->response_headers,
      "Access-Control-Allow-Origin", "*");

  /* skip over content-range/ */
  path += 14;

  if (path[0] != 'a' && path[0] != 'v') {
    GST_ERROR ("bad path: %s", path);
    return;
  }
  index = strtoul (path + 1, NULL, 10);

  level = NULL;
  if (path[0] == 'a') {
    if (index < adaptive->n_audio_levels) {
      level = &adaptive->audio_levels[index];
    }
  } else {
    if (index < adaptive->n_video_levels) {
      level = &adaptive->video_levels[index];
    }
  }

  if (level == NULL) {
    GST_ERROR ("bad level: %c%d from path %s", path[0], index, path);
    return;
  }

  if (t->msg->method == SOUP_METHOD_HEAD) {
    GST_DEBUG ("%s: HEAD", path);
    soup_message_headers_set_content_length (t->msg->response_headers,
        level->track->dash_size);
    return;
  }

  have_range = soup_message_headers_get_ranges (t->msg->request_headers,
      level->track->dash_size, &ranges, &n_ranges);

  if (have_range) {
    if (n_ranges != 1) {
      GST_ERROR ("too many ranges");
    }
    start = ranges[0].start;
    end = ranges[0].end + 1;
  } else {
    start = 0;
    end = level->track->dash_size;
  }
  GST_DEBUG ("%s: range: %ld-%ld", path, start, end);

  offset = start;
  n_bytes = end - start;

  if (ranges_overlap (offset, n_bytes, 0,
          level->track->dash_header_and_sidx_size)) {
    gss_soup_message_body_append_clipped (t->msg->response_body,
        SOUP_MEMORY_COPY, level->track->dash_header_data,
        offset, n_bytes, 0, level->track->dash_header_and_sidx_size);
  }
  header_size = level->track->dash_header_and_sidx_size;

  for (i = 0; i < level->track->n_fragments; i++) {
    GssIsomFragment *fragment = level->track->fragments[i];
    guint8 *mdat_data;

    if (offset + n_bytes <= fragment->offset)
      break;

    if (ranges_overlap (offset, n_bytes, header_size + fragment->offset,
            fragment->moof_size)) {
      gss_soup_message_body_append_clipped (t->msg->response_body,
          SOUP_MEMORY_COPY, fragment->moof_data,
          offset, n_bytes, header_size + fragment->offset, fragment->moof_size);
    }

    if (ranges_overlap (offset, n_bytes, header_size + fragment->offset +
            fragment->moof_size, fragment->mdat_size)) {
      mdat_data = gss_adaptive_assemble_chunk (t, adaptive, level, fragment);
      if (global_drm != GSS_DRM_CLEAR) {
        gss_playready_encrypt_samples (fragment, mdat_data,
            adaptive->content_key);
      }

      gss_soup_message_body_append_clipped (t->msg->response_body,
          SOUP_MEMORY_COPY, mdat_data + 8,
          offset, n_bytes, header_size + fragment->offset + fragment->moof_size,
          fragment->mdat_size - 8);
      g_free (mdat_data);
    }
  }

  soup_message_body_complete (t->msg->response_body);

  if (have_range) {
    soup_message_headers_set_content_range (t->msg->response_headers,
        ranges[0].start, ranges[0].end, level->track->dash_size);

    soup_message_set_status (t->msg, SOUP_STATUS_PARTIAL_CONTENT);

    soup_message_headers_free_ranges (t->msg->response_headers, ranges);
  } else {
    soup_message_set_status (t->msg, SOUP_STATUS_OK);
  }

  soup_message_headers_replace (t->msg->response_headers, "Content-Type",
      (path[0] == 'v') ? "video/mp4" : "audio/mp4");
}

static void
gss_adaptive_resource_get_dash_live_mpd (GssTransaction * t,
    GssAdaptive * adaptive)
{
  GString *s = g_string_new ("");
  int i;

  t->s = s;

  soup_message_headers_replace (t->msg->response_headers, "Content-Type",
      "application/octet-stream");
  soup_message_headers_replace (t->msg->response_headers,
      "Access-Control-Allow-Origin", "*");

  GSS_P ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
  GSS_A ("<MPD xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
      "  xmlns=\"urn:mpeg:dash:schema:mpd:2011\"\n");
  if (global_drm == GSS_DRM_PLAYREADY) {
    GSS_A ("  xmlns:mspr=\"urn:microsoft:playready\"\n");
  }
  GSS_P ("  xsi:schemaLocation=\"urn:mpeg:dash:schema:mpd:2011 DASH-MPD.xsd\"\n"
      "  type=\"static\"\n"
      "  mediaPresentationDuration=\"PT%dS\"\n"
      "  minBufferTime=\"PT4S\"\n"
      "  profiles=\"urn:mpeg:dash:profile:isoff-live:2011\">\n",
      (int) (adaptive->duration / GSS_ISM_SECOND));
  GSS_P ("  <Period>\n");

  GSS_A ("    <AdaptationSet " "id=\"1\" "
      "profiles=\"ccff\" "
      "bitstreamSwitching=\"true\" "
      "segmentAlignment=\"true\" "
      "contentType=\"audio\" " "mimeType=\"audio/mp4\" " "lang=\"en\">\n");
  append_content_protection (t, adaptive);
  GSS_A ("    <SegmentTemplate timescale=\"10000000\" "
      "media=\"content?stream=audio&amp;bitrate=$Bandwidth$&amp;start_time=$Time$\" "
      "initialization=\"content?stream=audio&amp;bitrate=$Bandwidth$&amp;start_time=init\">\n");
  GSS_A ("      <SegmentTimeline>\n");
  {
    GssAdaptiveLevel *level = &adaptive->audio_levels[0];

    for (i = 0; i < level->n_fragments; i++) {
      GssIsomFragment *fragment;
      fragment = gss_isom_track_get_fragment (level->track, i);
      GSS_P ("        <S d=\"%" G_GUINT64_FORMAT "\" />\n",
          (guint64) fragment->duration);
    }
  }
  GSS_A ("      </SegmentTimeline>\n");
  GSS_A ("    </SegmentTemplate>\n");
  for (i = 0; i < adaptive->n_audio_levels; i++) {
    GssAdaptiveLevel *level = &adaptive->audio_levels[i];

    GSS_P ("      <Representation id=\"a%d\" codecs=\"%s\" "
        "bandwidth=\"%d\" audioSamplingRate=\"%d\"/>\n",
        i, level->codec, level->bitrate, level->audio_rate);
  }
  GSS_A ("    </AdaptationSet>\n");

  GSS_P ("    <AdaptationSet " "id=\"2\" "
      "profiles=\"ccff\" "
      "bitstreamSwitching=\"true\" "
      "segmentAlignment=\"true\" "
      "contentType=\"video\" "
      "mimeType=\"video/mp4\" "
      "maxWidth=\"1920\" " "maxHeight=\"1080\" " "startWithSAP=\"1\">\n");
  append_content_protection (t, adaptive);

  GSS_A ("    <SegmentTemplate timescale=\"10000000\" "
      "media=\"content?stream=video&amp;bitrate=$Bandwidth$&amp;start_time=$Time$\" "
      "initialization=\"content?stream=video&amp;bitrate=$Bandwidth$&amp;start_time=init\">\n");
  GSS_A ("      <SegmentTimeline>\n");
  {
    GssAdaptiveLevel *level = &adaptive->video_levels[0];

    for (i = 0; i < level->n_fragments; i++) {
      GssIsomFragment *fragment;
      fragment = gss_isom_track_get_fragment (level->track, i);
      GSS_P ("        <S d=\"%" G_GUINT64_FORMAT "\" />\n",
          (guint64) fragment->duration);
    }
  }
  GSS_A ("      </SegmentTimeline>\n");
  GSS_A ("    </SegmentTemplate>\n");
  for (i = 0; i < adaptive->n_video_levels; i++) {
    GssAdaptiveLevel *level = &adaptive->video_levels[i];

    GSS_P ("      <Representation id=\"v%d\" bandwidth=\"%d\" "
        "codecs=\"%s\" width=\"%d\" height=\"%d\"/>\n",
        i, level->bitrate, level->codec,
        level->video_width, level->video_height);
  }
  GSS_A ("    </AdaptationSet>\n");

  GSS_A ("  </Period>\n");
  GSS_A ("</MPD>\n");
  GSS_A ("\n");

}

static gboolean
parse_guint64 (const char *s, guint64 * value)
{
  char *end;

  if (s == NULL)
    return FALSE;

  if (s[0] == '\0')
    return FALSE;

  *value = g_ascii_strtoull (s, &end, 0);

  if (end[0] != '\0')
    return FALSE;
  return TRUE;
}

static void
gss_adaptive_resource_get_content (GssTransaction * t, GssAdaptive * adaptive)
{
  const char *stream;
  const char *start_time_str;
  const char *bitrate_str;
  guint64 start_time;
  guint64 bitrate;
  gboolean is_init;
  GssAdaptiveLevel *level;
  GssIsomFragment *fragment;
  gboolean ret;

  //GST_ERROR ("content request");

  if (t->query == NULL) {
    GST_ERROR ("no query");
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  stream = g_hash_table_lookup (t->query, "stream");
  start_time_str = g_hash_table_lookup (t->query, "start_time");
  bitrate_str = g_hash_table_lookup (t->query, "bitrate");

  if (stream == NULL || start_time_str == NULL || bitrate_str == NULL) {
    GST_ERROR ("missing parameter");
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  ret = parse_guint64 (bitrate_str, &bitrate);
  if (!ret) {
    GST_ERROR ("bad bitrate %s", bitrate_str);
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  if (strcmp (start_time_str, "init") == 0) {
    is_init = TRUE;
    start_time = 0;
  } else {
    is_init = FALSE;
    ret = parse_guint64 (start_time_str, &start_time);
    if (!ret) {
      GST_ERROR ("bad start_time %s", start_time_str);
      soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
      return;
    }
  }

  if (strcmp (stream, "audio") != 0 && strcmp (stream, "video") != 0) {
    GST_ERROR ("bad stream %s", stream);
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  level = gss_adaptive_get_level (adaptive, (stream[0] == 'v'), bitrate);
  if (level == NULL) {
    GST_ERROR ("no level for %s, %" G_GUINT64_FORMAT, stream, bitrate);
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  soup_message_headers_replace (t->msg->response_headers,
      "Access-Control-Allow-Origin", "*");

  if (is_init) {
    soup_message_body_append (t->msg->response_body, SOUP_MEMORY_COPY,
        level->track->ccff_header_data, level->track->ccff_header_size);
  } else {
    fragment = gss_isom_track_get_fragment_by_timestamp (level->track,
        start_time);
    if (fragment == NULL) {
      GST_ERROR ("no fragment for %" G_GUINT64_FORMAT, start_time);
      soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
      return;
    }
    //GST_ERROR ("frag %s %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT,
    //    level->filename, fragment->offset, fragment->size);

    {
      guint8 *mdat_data;

      mdat_data = gss_adaptive_assemble_chunk (t, adaptive, level, fragment);
      if (global_drm != GSS_DRM_CLEAR) {
        gss_playready_encrypt_samples (fragment, mdat_data,
            adaptive->content_key);
      }
      gss_adaptive_send_chunk (t, adaptive, level, fragment, mdat_data);
    }
  }
  soup_message_headers_replace (t->msg->response_headers, "Content-Type",
      (stream[0] == 'v') ? "video/mp4" : "audio/mp4");
}

GssAdaptive *
gss_adaptive_new (void)
{
  GssAdaptive *adaptive;

  adaptive = g_malloc0 (sizeof (GssAdaptive));

  return adaptive;

}

void
gss_adaptive_free (GssAdaptive * adaptive)
{
  int i;

  for (i = 0; i < adaptive->n_parsers; i++) {
    gss_isom_parser_free (adaptive->parsers[i]);
  }

  for (i = 0; i < adaptive->n_audio_levels; i++) {
    adaptive->audio_levels[i].track = NULL;
    g_free (adaptive->audio_levels[i].codec_data);
    g_free (adaptive->audio_levels[i].filename);
    g_free (adaptive->audio_levels[i].codec);
  }
  for (i = 0; i < adaptive->n_video_levels; i++) {
    adaptive->video_levels[i].track = NULL;
    g_free (adaptive->video_levels[i].codec_data);
    g_free (adaptive->video_levels[i].filename);
    g_free (adaptive->video_levels[i].codec);
  }
  g_free (adaptive->audio_levels);
  g_free (adaptive->video_levels);
  g_free (adaptive->content_id);
  g_free (adaptive->kid);
  g_free (adaptive);
}

GssAdaptiveLevel *
gss_adaptive_get_level (GssAdaptive * adaptive, gboolean video, guint64 bitrate)
{
  int i;
  if (video) {
    for (i = 0; i < adaptive->n_video_levels; i++) {
      if (adaptive->video_levels[i].bitrate == bitrate) {
        return &adaptive->video_levels[i];
      }
    }
  } else {
    for (i = 0; i < adaptive->n_audio_levels; i++) {
      if (adaptive->audio_levels[i].bitrate == bitrate) {
        return &adaptive->audio_levels[i];
      }
    }
  }
  return NULL;
}

static gboolean
split (const char *line, char **filename, int *video_bitrate,
    int *audio_bitrate)
{
  const char *s = line;
  char *end;
  int i;

  while (g_ascii_isspace (s[0]))
    s++;
  if (s[0] == '#' || s[0] == 0)
    return FALSE;

  for (i = 0; s[i]; i++) {
    if (g_ascii_isspace (s[i]))
      break;
  }

  *filename = g_strndup (s, i);
  *video_bitrate = 0;
  *audio_bitrate = 0;

  s += i;
  while (g_ascii_isspace (s[0]))
    s++;

  *video_bitrate = strtoul (s, &end, 10);
  s = end;

  *audio_bitrate = strtoul (s, &end, 10);
  s = end;

  return TRUE;
}

static guint8 *
create_key_id (const char *key_string)
{
  GChecksum *checksum;
  guint8 *bytes;
  gsize size;

  checksum = g_checksum_new (G_CHECKSUM_SHA1);
  bytes = g_malloc (20);
  g_checksum_update (checksum, (const guint8 *) key_string, -1);
  g_checksum_update (checksum,
      (const guint8 *) "KThMK9Tibb+X9qRuTvwOchPRwH+4hV05yZXnx7C", -1);
  size = 20;
  g_checksum_get_digest (checksum, bytes, &size);
  g_checksum_free (checksum);

  return bytes;
}

GssAdaptive *
gss_adaptive_load (GssServer * server, const char *key)
{
  GssAdaptive *adaptive;
  int i;
  char *filename;
  char *contents;
  gsize length;
  char **lines;
  gboolean ret;
  GError *error = NULL;

  GST_DEBUG ("looking for %s", key);

  filename = g_strdup_printf ("ism-vod/%s/gss-manifest", key);
  ret = g_file_get_contents (filename, &contents, &length, &error);
  if (!ret) {
    GST_ERROR ("failed to open %s", filename);
    g_free (filename);
    g_error_free (error);
    return NULL;
  }
  g_free (filename);

  GST_DEBUG ("loading %s", key);

  adaptive = gss_adaptive_new ();

  adaptive->server = server;

  adaptive->content_id = g_strdup (key);
  adaptive->kid = create_key_id (key);
  adaptive->kid_len = 16;

  gss_playready_generate_key (server->playready, adaptive->content_key,
      adaptive->kid, adaptive->kid_len);

  lines = g_strsplit (contents, "\n", 0);
  for (i = 0; lines[i]; i++) {
    char *fn = NULL;
    int video_bitrate = 0;
    int audio_bitrate = 0;
    char *full_fn;

    ret = split (lines[i], &fn, &video_bitrate, &audio_bitrate);
    if (!ret)
      continue;

    GST_DEBUG ("fn %s video_bitrate %d audio_bitrate %d",
        fn, video_bitrate, audio_bitrate);

    full_fn = g_strdup_printf ("ism-vod/%s/%s", key, fn);

    load_file (adaptive, full_fn, video_bitrate, audio_bitrate);
    g_free (full_fn);
    g_free (fn);
  }

  g_strfreev (lines);
  g_free (contents);

  GST_DEBUG ("loading done");

  return adaptive;
}

static void
generate_iv (GssAdaptiveLevel * level, const char *filename, int track_id)
{
  GChecksum *csum;
  char *s;
  gsize size;
  guint8 bytes[20];

  s = g_strdup_printf ("%s:%d", filename, track_id);

  csum = g_checksum_new (G_CHECKSUM_SHA1);
  g_checksum_update (csum, (guchar *) s, -1);
  g_free (s);

  size = 20;
  g_checksum_get_digest (csum, (guchar *) bytes, &size);
  memcpy (&level->iv, bytes, sizeof (level->iv));

  g_checksum_free (csum);
}

static void
gss_level_from_track (GssAdaptive * adaptive, GssIsomTrack * track,
    GssIsomMovie * movie, const char *filename, int bitrate, gboolean is_video)
{
  GssAdaptiveLevel *level;
  int i;

  if (is_video) {
    adaptive->video_levels = g_realloc (adaptive->video_levels,
        (adaptive->n_video_levels + 1) * sizeof (GssAdaptiveLevel));
    level = adaptive->video_levels + adaptive->n_video_levels;
    adaptive->n_video_levels++;
  } else {
    adaptive->audio_levels = g_realloc (adaptive->audio_levels,
        (adaptive->n_audio_levels + 1) * sizeof (GssAdaptiveLevel));
    level = adaptive->audio_levels + adaptive->n_audio_levels;
    adaptive->n_audio_levels++;
  }
  memset (level, 0, sizeof (GssAdaptiveLevel));
  level->track = track;

  if (is_video) {
    adaptive->max_width = MAX (adaptive->max_width, track->mp4v.width);
    adaptive->max_height = MAX (adaptive->max_height, track->mp4v.height);
  }

  generate_iv (level, filename, track->tkhd.track_id);

  for (i = 0; i < track->n_fragments; i++) {
    GssIsomFragment *fragment = track->fragments[i];
    gss_playready_setup_iv (adaptive->server->playready, adaptive, level,
        fragment);
  }
  gss_isom_track_prepare_streaming (movie, track);

  level->track_id = track->tkhd.track_id;
  level->n_fragments = track->n_fragments;
  level->filename = g_strdup (filename);
  level->bitrate = bitrate;
  level->video_width = track->mp4v.width;
  level->video_height = track->mp4v.height;
  //level->file = file;

  level->codec_data = get_codec_string (track->esds.codec_data,
      track->esds.codec_data_len);
  if (is_video) {
    level->codec = g_strdup_printf ("avc1.%02x%02x%02x",
        track->esds.codec_data[1],
        track->esds.codec_data[2], track->esds.codec_data[3]);
  } else {
    /* FIXME hard-coded AAC LC */
    level->codec = g_strdup ("mp4a.40.2");
  }
}

static void
load_file (GssAdaptive * adaptive, const char *filename, int video_bitrate,
    int audio_bitrate)
{
  GssIsomParser *file;
  GssIsomTrack *video_track;
  GssIsomTrack *audio_track;

  file = gss_isom_parser_new ();
  adaptive->parsers[adaptive->n_parsers] = file;
  adaptive->n_parsers++;
  gss_isom_parser_parse_file (file, filename);

  if (file->movie->tracks[0]->n_fragments == 0) {
    gss_isom_parser_fragmentize (file);
  }

  if (adaptive->duration == 0) {
    adaptive->duration = gss_isom_movie_get_duration (file->movie);
  }

  video_track = gss_isom_movie_get_video_track (file->movie);
  if (video_track) {
    gss_level_from_track (adaptive, video_track, file->movie, filename,
        video_bitrate, TRUE);
  }

  audio_track = gss_isom_movie_get_audio_track (file->movie);
  if (audio_track) {
    gss_level_from_track (adaptive, audio_track, file->movie, filename,
        audio_bitrate, FALSE);
#if 0
    GssAdaptiveLevel *level;
    int i;

    adaptive->audio_levels = g_realloc (adaptive->audio_levels,
        (adaptive->n_audio_levels + 1) * sizeof (GssAdaptiveLevel));
    level = adaptive->audio_levels + adaptive->n_audio_levels;
    adaptive->n_audio_levels++;
    memset (level, 0, sizeof (GssAdaptiveLevel));

    generate_iv (level, filename, video_track->tkhd.track_id);

    for (i = 0; i < audio_track->n_fragments; i++) {
      GssIsomFragment *fragment = audio_track->fragments[i];
      gss_playready_setup_iv (adaptive->server->playready, adaptive, level,
          fragment);
    }
    gss_isom_track_prepare_streaming (file->movie, audio_track);

    level->track_id = audio_track->tkhd.track_id;
    level->track = audio_track;
    level->n_fragments =
        gss_isom_parser_get_n_fragments (file, level->track_id);
    level->file = file;
    level->filename = g_strdup (filename);
    level->bitrate = audio_bitrate;
    level->codec_data = get_codec_string (audio_track->esds.codec_data,
        audio_track->esds.codec_data_len);
    level->audio_rate = audio_track->mp4a.sample_rate >> 16;
    /* FIXME hard-coded AAC LC */
    level->codec = g_strdup ("mp4a.40.2");
#endif
  }

}

void
gss_adaptive_get_resource (GssTransaction * t, GssAdaptive * adaptive,
    GssDrmType drm_type, GssAdaptiveStream stream_type, const char *subpath)
{
  if (strcmp (subpath, "Manifest") == 0) {
    gss_adaptive_resource_get_manifest (t, adaptive);
  } else if (strcmp (subpath, "content") == 0) {
    gss_adaptive_resource_get_content (t, adaptive);
  } else if (strcmp (subpath, "manifest-range.mpd") == 0) {
    gss_adaptive_resource_get_dash_range_mpd (t, adaptive);
  } else if (strncmp (subpath, "content-range/", 14) == 0) {
    gss_adaptive_resource_get_dash_range_fragment (t, adaptive, subpath);
  } else if (strcmp (subpath, "manifest-live.mpd") == 0) {
    gss_adaptive_resource_get_dash_live_mpd (t, adaptive);
  } else {
    GST_ERROR ("not found: %s, %s", t->path, subpath);
    soup_message_set_status (t->msg, SOUP_STATUS_NOT_FOUND);
  }
}

GssAdaptiveStream
gss_adaptive_get_stream_type (const char *s)
{
  if (strcmp (s, "ism") == 0)
    return GSS_ADAPTIVE_STREAM_ISM;
  if (strcmp (s, "dash-live") == 0)
    return GSS_ADAPTIVE_STREAM_DASH_LIVE;
  if (strcmp (s, "dash-ondemand") == 0)
    return GSS_ADAPTIVE_STREAM_DASH_ONDEMAND;
  return GSS_ADAPTIVE_STREAM_UNKNOWN;
}
