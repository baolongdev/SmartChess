#pragma once

#include <Arduino.h>

enum LichessUploadState {
  LICHESS_IDLE = 0,
  LICHESS_QUEUED = 1,
  LICHESS_UPLOADING = 2,
  LICHESS_DONE = 3,
  LICHESS_ERROR = 4,
};

struct LichessContext {
  char *movesBuf;
  int movesBufSize;
  int *movesLen;
  int *plyCount;
  String *lastUrl;
  String *lastError;
  bool *republishPending;
  bool *autoPublishPerMove;
  int *uploadState;
  bool whiteKingMoved;
  bool blackKingMoved;
  bool whiteRookAMoved;
  bool whiteRookHMoved;
  bool blackRookAMoved;
  bool blackRookHMoved;
  const char (*board)[8];
  int enPassantFile;
  int enPassantRank;
};

using LichessLogFn = void (*)(const String &line);

void lichessResetTracking(LichessContext &ctx);
const char *lichessStateName(int state);
String lichessBuildSanMove(const LichessContext &ctx,
                           int fromIdx,
                           int toIdx,
                           char piece,
                           bool isCapture,
                           bool castleKingSide,
                           bool castleQueenSide,
                           bool enPassantCapture,
                           bool willPromoteToQueen);
bool lichessAppendMove(LichessContext &ctx, const String &sanMove);
const char *lichessBuildPayload(const LichessContext &ctx, const char *prefix, char *outBuf, size_t outLen);
bool lichessQueuePublish(LichessContext &ctx, String &response, LichessLogFn logFn);
void lichessRequestAutoPublish(LichessContext &ctx, LichessLogFn logFn);
void lichessProcessUploadTick(LichessContext &ctx, LichessLogFn logFn);
