//
// Copyright(C) 2020 by Ryan Krafnick
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	DSDA Key Frame
//

#include "doomstat.h"
#include "s_advsound.h"
#include "s_sound.h"
#include "p_saveg.h"
#include "p_map.h"
#include "r_draw.h"
#include "r_fps.h"
#include "r_main.h"
#include "g_game.h"
#include "e6y.h"

#include "dsda/demo.h"
#include "dsda/settings.h"
#include "key_frame.h"

// Hook into the save & demo ecosystem
extern const byte* demo_p;
extern byte* savebuffer;
extern size_t savegamesize;
extern dboolean setsizeneeded;
extern dboolean BorderNeedRefresh;
struct MapEntry *G_LookupMapinfo(int gameepisode, int gamemap);
void RecalculateDrawnSubsectors(void);

static byte* dsda_quick_key_frame_buffer;
static int dsda_key_frame_restored;

typedef struct {
  byte* buffer;
  int index;
} dsda_key_frame_t;

static dsda_key_frame_t* dsda_auto_key_frames;
static int dsda_last_auto_key_frame;
static int dsda_auto_key_frames_size;

void dsda_InitKeyFrame(void) {
  dsda_auto_key_frames_size = dsda_AutoKeyFrameDepth();

  if (dsda_auto_key_frames_size == 0) return;

  if (dsda_auto_key_frames != NULL) free(dsda_auto_key_frames);

  dsda_auto_key_frames =
    calloc(dsda_auto_key_frames_size, sizeof(dsda_key_frame_t));
  dsda_last_auto_key_frame = -1;
}

// Stripped down version of G_DoSaveGame
void dsda_StoreKeyFrame(byte** buffer, int log) {
  int demo_write_buffer_offset, i;
  demo_write_buffer_offset = dsda_DemoBufferOffset();

  save_p = savebuffer = malloc(savegamesize);

  CheckSaveGame(5 + MIN_MAXPLAYERS);
  *save_p++ = compatibility_level;
  *save_p++ = gameskill;
  *save_p++ = gameepisode;
  *save_p++ = gamemap;

  for (i = 0; i < MAXPLAYERS; i++)
    *save_p++ = playeringame[i];

  for (; i < MIN_MAXPLAYERS; i++)
    *save_p++ = 0;

  *save_p++ = idmusnum;

  CheckSaveGame(GAME_OPTION_SIZE);
  save_p = G_WriteOptions(save_p);

  // Store progress bar for demo playback
  CheckSaveGame(sizeof(demo_curr_tic));
  memcpy(save_p, &demo_curr_tic, sizeof(demo_curr_tic));
  save_p += sizeof(demo_curr_tic);

  // Store location in demo playback buffer
  CheckSaveGame(sizeof(demo_p));
  memcpy(save_p, &demo_p, sizeof(demo_p));
  save_p += sizeof(demo_p);

  // Store location in demo recording buffer
  CheckSaveGame(sizeof(demo_write_buffer_offset));
  memcpy(save_p, &demo_write_buffer_offset, sizeof(demo_write_buffer_offset));
  save_p += sizeof(demo_write_buffer_offset);

  CheckSaveGame(sizeof(leveltime));
  memcpy(save_p, &leveltime, sizeof(leveltime));
  save_p += sizeof(leveltime);

  CheckSaveGame(sizeof(totalleveltimes));
  memcpy(save_p, &totalleveltimes, sizeof(totalleveltimes));
  save_p += sizeof(totalleveltimes);

  CheckSaveGame(1);
  *save_p++ = (gametic - basetic) & 255;

  Z_CheckHeap();
  P_ArchivePlayers();
  Z_CheckHeap();
  P_ThinkerToIndex();
  P_ArchiveWorld();
  Z_CheckHeap();
  P_TrueArchiveThinkers();
  P_IndexToThinker();
  Z_CheckHeap();
  P_ArchiveRNG();
  Z_CheckHeap();
  P_ArchiveMap();
  Z_CheckHeap();

  if (*buffer != NULL) free(*buffer);

  *buffer = savebuffer;
  savebuffer = save_p = NULL;

  if (log) doom_printf("Stored key frame");
}

// Stripped down version of G_DoLoadGame
// save_p is coopted to use the save logic
void dsda_RestoreKeyFrame(byte* buffer) {
  int demo_write_buffer_offset, i;

  if (buffer == NULL) {
    doom_printf("No key frame found");
    return;
  }

  save_p = buffer;

  compatibility_level = *save_p++;
  gameskill = *save_p++;
  gameepisode = *save_p++;
  gamemap = *save_p++;
  gamemapinfo = G_LookupMapinfo(gameepisode, gamemap);

  for (i = 0; i < MAXPLAYERS; i++)
    playeringame[i] = *save_p++;
  save_p += MIN_MAXPLAYERS - MAXPLAYERS;

  idmusnum = *save_p++;
  if (idmusnum == 255) idmusnum = -1;

  save_p += (G_ReadOptions(save_p) - save_p);

  // Restore progress bar for demo playback
  memcpy(&demo_curr_tic, save_p, sizeof(demo_curr_tic));
  save_p += sizeof(demo_curr_tic);

  // Restore location in demo playback buffer
  memcpy(&demo_p, save_p, sizeof(demo_p));
  save_p += sizeof(demo_p);

  // Restore location in demo recording buffer
  memcpy(&demo_write_buffer_offset, save_p, sizeof(demo_write_buffer_offset));
  save_p += sizeof(demo_write_buffer_offset);

  dsda_SetDemoBufferOffset(demo_write_buffer_offset);

  G_InitNew(gameskill, gameepisode, gamemap);

  memcpy(&leveltime, save_p, sizeof(leveltime));
  save_p += sizeof(leveltime);

  memcpy(&totalleveltimes, save_p, sizeof(totalleveltimes));
  save_p += sizeof(totalleveltimes);

  basetic = gametic - *save_p++;

  P_MapStart();
  P_UnArchivePlayers();
  P_UnArchiveWorld();
  P_TrueUnArchiveThinkers();
  P_UnArchiveRNG();
  P_UnArchiveMap();
  P_MapEnd();
  R_ActivateSectorInterpolations();
  R_SmoothPlaying_Reset(NULL);

  if (musinfo.current_item != -1)
    S_ChangeMusInfoMusic(musinfo.current_item, true);

  RecalculateDrawnSubsectors();

  if (setsizeneeded) R_ExecuteSetViewSize();

  R_FillBackScreen();

  BorderNeedRefresh = true;

  dsda_key_frame_restored = 1;

  doom_printf("Restored key frame");
}

int dsda_KeyFrameRestored(void) {
  if (!dsda_key_frame_restored) return 0;

  dsda_key_frame_restored = 0;
  return 1;
}

void dsda_StoreQuickKeyFrame(void) {
  dsda_StoreKeyFrame(&dsda_quick_key_frame_buffer, true);
}

void dsda_RestoreQuickKeyFrame(void) {
  dsda_RestoreKeyFrame(dsda_quick_key_frame_buffer);
}

void dsda_RewindAutoKeyFrame(void) {
  int current_time;
  int interval_tics;
  int key_frame_index;
  int history_index;

  if (dsda_auto_key_frames_size == 0) {
    doom_printf("No key frame found");
    return;
  }

  current_time = totalleveltimes + leveltime;
  interval_tics = 35 * dsda_AutoKeyFrameInterval();

  key_frame_index = current_time / interval_tics - 1;

  history_index = dsda_last_auto_key_frame - 1;
  if (history_index < 0) history_index = dsda_auto_key_frames_size - 1;

  if (dsda_auto_key_frames[history_index].index <= key_frame_index) {
    dsda_last_auto_key_frame = history_index;
    dsda_SkipNextWipe();
    dsda_RestoreKeyFrame(dsda_auto_key_frames[history_index].buffer);
  }
  else doom_printf("No key frame found"); // rewind past the depth limit
}

void dsda_UpdateAutoKeyFrames(void) {
  int key_frame_index;
  int current_time;
  int interval_tics;
  dsda_key_frame_t* current_key_frame;

  if (
    dsda_auto_key_frames_size == 0 ||
    gamestate != GS_LEVEL ||
    gameaction != ga_nothing
  ) return;

  current_time = totalleveltimes + leveltime;
  interval_tics = 35 * dsda_AutoKeyFrameInterval();

  // Automatically save a key frame each interval
  if (current_time % interval_tics == 0) {
    key_frame_index = current_time / interval_tics;

    // Don't duplicate (e.g., because we rewound to this index)
    if (
      dsda_last_auto_key_frame >= 0 &&
      dsda_auto_key_frames[dsda_last_auto_key_frame].index == key_frame_index
    ) return;

    dsda_last_auto_key_frame += 1;
    if (dsda_last_auto_key_frame >= dsda_auto_key_frames_size)
      dsda_last_auto_key_frame = 0;

    current_key_frame = &dsda_auto_key_frames[dsda_last_auto_key_frame];
    current_key_frame->index = key_frame_index;

    dsda_StoreKeyFrame(&current_key_frame->buffer, false);
  }
}
