#include "LichessPublish.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <stdarg.h>
#include <string.h>

#include "BoardState.h"
#include "MoveGen.h"
#include "TextUtils.h"

namespace {

static String pgnDateFromBuild() {
  const char *d = __DATE__;  // Mmm dd yyyy
  if (d == nullptr) {
    return String("????.??.??");
  }

  char monthAbbr[4] = {0};
  monthAbbr[0] = d[0];
  monthAbbr[1] = d[1];
  monthAbbr[2] = d[2];

  int month = 0;
  if (strncmp(monthAbbr, "Jan", 3) == 0) month = 1;
  else if (strncmp(monthAbbr, "Feb", 3) == 0) month = 2;
  else if (strncmp(monthAbbr, "Mar", 3) == 0) month = 3;
  else if (strncmp(monthAbbr, "Apr", 3) == 0) month = 4;
  else if (strncmp(monthAbbr, "May", 3) == 0) month = 5;
  else if (strncmp(monthAbbr, "Jun", 3) == 0) month = 6;
  else if (strncmp(monthAbbr, "Jul", 3) == 0) month = 7;
  else if (strncmp(monthAbbr, "Aug", 3) == 0) month = 8;
  else if (strncmp(monthAbbr, "Sep", 3) == 0) month = 9;
  else if (strncmp(monthAbbr, "Oct", 3) == 0) month = 10;
  else if (strncmp(monthAbbr, "Nov", 3) == 0) month = 11;
  else if (strncmp(monthAbbr, "Dec", 3) == 0) month = 12;

  int day = 0;
  if (d[4] == ' ') {
    day = d[5] - '0';
  } else {
    day = (d[4] - '0') * 10 + (d[5] - '0');
  }

  int year = 0;
  year = (d[7] - '0') * 1000 + (d[8] - '0') * 100 + (d[9] - '0') * 10 + (d[10] - '0');

  if (month <= 0 || day <= 0 || year <= 0) {
    return String("????.??.??");
  }

  char out[16];
  snprintf(out, sizeof(out), "%04d.%02d.%02d", year, month, day);
  return String(out);
}

static bool moveListContains(const int list[MAX_CANDIDATES], int count, int idx) {
  for (int i = 0; i < count; i++) {
    if (list[i] == idx) {
      return true;
    }
  }
  return false;
}

static bool appendToMovesBuf(LichessContext &ctx, const char *fmt, ...) {
  if (ctx.movesBuf == nullptr || ctx.movesLen == nullptr || ctx.movesBufSize <= 1 || fmt == nullptr) {
    return false;
  }

  if (*ctx.movesLen < 0) {
    *ctx.movesLen = 0;
  }

  size_t cap = (size_t)ctx.movesBufSize;
  if ((size_t)*ctx.movesLen >= cap) {
    *ctx.movesLen = (int)(cap - 1);
    ctx.movesBuf[cap - 1] = '\0';
    return false;
  }

  size_t pos = (size_t)*ctx.movesLen;
  size_t remaining = cap - pos;
  if (remaining == 0) {
    return false;
  }

  va_list args;
  va_start(args, fmt);
  int wrote = vsnprintf(ctx.movesBuf + pos, remaining, fmt, args);
  va_end(args);

  if (wrote < 0) {
    ctx.movesBuf[pos] = '\0';
    return false;
  }

  if ((size_t)wrote >= remaining) {
    ctx.movesBuf[cap - 1] = '\0';
    return false;
  }

  *ctx.movesLen += wrote;
  return true;
}

static const char *buildPgnForLichess(const LichessContext &ctx, char *outBuf, size_t outLen) {
  if (outBuf == nullptr || outLen == 0) {
    return "";
  }

  const char *moves = (ctx.movesBuf != nullptr && ctx.movesLen != nullptr && *ctx.movesLen > 0) ? ctx.movesBuf : "";
  String dateStr = pgnDateFromBuild();

  snprintf(outBuf, outLen,
           "[Event \"SmartChess Live\"]\n"
           "[Site \"https://smartchess.local\"]\n"
           "[Date \"%s\"]\n"
           "[Round \"-\"]\n"
           "[White \"SmartChess White\"]\n"
           "[Black \"SmartChess Black\"]\n"
           "[Result \"*\"]\n\n%s *",
           dateStr.c_str(),
           moves);
  return outBuf;
}

static bool publishCurrentGameToLichess(LichessContext &ctx, String &response, LichessLogFn logFn) {
  if (WiFi.status() != WL_CONNECTED) {
    response = "LICHESS_ERR:WIFI_NOT_CONNECTED";
    return false;
  }

  char pgnBuf[600];
  String pgn = buildPgnForLichess(ctx, pgnBuf, sizeof(pgnBuf));
  if (pgn.length() == 0) {
    response = "LICHESS_ERR:EMPTY_PGN";
    return false;
  }

  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  HTTPClient http;
  if (!http.begin(secureClient, "https://lichess.org/api/import")) {
    response = "LICHESS_ERR:HTTP_BEGIN";
    return false;
  }

  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "pgn=";
  body += urlEncode(pgn);

  int statusCode = http.POST(body);
  String respBody = http.getString();
  http.end();

  if (statusCode < 200 || statusCode >= 300) {
    if (ctx.lastError != nullptr) {
      *ctx.lastError = String("HTTP_") + String(statusCode);
      response = String("LICHESS_ERR:") + *ctx.lastError;
    } else {
      response = String("LICHESS_ERR:HTTP_") + String(statusCode);
    }
    if (logFn != nullptr) {
      logFn(String("[LICHESS] Upload failed status=") + String(statusCode));
    }
    return false;
  }

  int urlPos = respBody.indexOf("\"url\":\"");
  if (urlPos < 0) {
    if (ctx.lastError != nullptr) {
      *ctx.lastError = "NO_URL";
    }
    response = "LICHESS_ERR:NO_URL";
    if (logFn != nullptr) {
      logFn(String("[LICHESS] Upload response missing url"));
    }
    return false;
  }

  int start = urlPos + 7;
  int end = respBody.indexOf('"', start);
  if (end < 0) {
    if (ctx.lastError != nullptr) {
      *ctx.lastError = "PARSE_URL";
    }
    response = "LICHESS_ERR:PARSE_URL";
    return false;
  }

  String url = respBody.substring(start, end);
  if (ctx.lastUrl != nullptr) {
    *ctx.lastUrl = url;
  }
  if (ctx.lastError != nullptr) {
    *ctx.lastError = "";
  }

  response = String("LICHESS_OK:url=") + url;
  if (logFn != nullptr) {
    logFn(String("[LICHESS] Uploaded ") + url);
  }
  return true;
}

}  // namespace

void lichessResetTracking(LichessContext &ctx) {
  if (ctx.movesBuf != nullptr && ctx.movesBufSize > 0) {
    ctx.movesBuf[0] = '\0';
  }
  if (ctx.movesLen != nullptr) {
    *ctx.movesLen = 0;
  }
  if (ctx.plyCount != nullptr) {
    *ctx.plyCount = 0;
  }
  if (ctx.lastUrl != nullptr) {
    *ctx.lastUrl = "";
  }
  if (ctx.lastError != nullptr) {
    *ctx.lastError = "";
  }
  if (ctx.uploadState != nullptr) {
    *ctx.uploadState = LICHESS_IDLE;
  }
  if (ctx.republishPending != nullptr) {
    *ctx.republishPending = false;
  }
}

const char *lichessStateName(int state) {
  if (state == LICHESS_QUEUED) {
    return "QUEUED";
  }
  if (state == LICHESS_UPLOADING) {
    return "UPLOADING";
  }
  if (state == LICHESS_DONE) {
    return "DONE";
  }
  if (state == LICHESS_ERROR) {
    return "ERROR";
  }
  return "IDLE";
}

String lichessBuildSanMove(const LichessContext &ctx,
                           int fromIdx,
                           int toIdx,
                           char piece,
                           bool isCapture,
                           bool castleKingSide,
                           bool castleQueenSide,
                           bool enPassantCapture,
                           bool willPromoteToQueen) {
  int fromFile = fromIdx / 8;
  int fromRank = fromIdx % 8;

  if (castleKingSide) {
    return "O-O";
  }
  if (castleQueenSide) {
    return "O-O-O";
  }

  String toSquare = squareNameFromIdx(toIdx);
  char pieceUpper = (char)toupper(piece);

  if (pieceUpper == 'P') {
    String pawnSan = "";
    if (isCapture || enPassantCapture) {
      pawnSan += (char)('a' + fromFile);
      pawnSan += 'x';
    }
    pawnSan += toSquare;
    if (willPromoteToQueen) {
      pawnSan += "=Q";
    }
    return pawnSan;
  }

  String san = "";
  san += pieceUpper;

  if (pieceUpper != 'K') {
    bool hasOtherCandidate = false;
    bool sameFileConflict = false;
    bool sameRankConflict = false;

    bool whiteCanCastleK = !ctx.whiteKingMoved && !ctx.whiteRookHMoved;
    bool whiteCanCastleQ = !ctx.whiteKingMoved && !ctx.whiteRookAMoved;
    bool blackCanCastleK = !ctx.blackKingMoved && !ctx.blackRookHMoved;
    bool blackCanCastleQ = !ctx.blackKingMoved && !ctx.blackRookAMoved;

    for (int f = 0; f < 8; f++) {
      for (int r = 0; r < 8; r++) {
        if (f == fromFile && r == fromRank) {
          continue;
        }
        if (ctx.board[f][r] != piece) {
          continue;
        }

        int candidateListLocal[MAX_CANDIDATES];
        int candidateCountLocal = 0;
        generateCandidateSquares(ctx.board,
                                 f,
                                 r,
                                 piece,
                                 ctx.enPassantFile,
                                 ctx.enPassantRank,
                                 whiteCanCastleK,
                                 whiteCanCastleQ,
                                 blackCanCastleK,
                                 blackCanCastleQ,
                                 candidateListLocal,
                                 candidateCountLocal);

        if (!moveListContains(candidateListLocal, candidateCountLocal, toIdx)) {
          continue;
        }

        hasOtherCandidate = true;
        if (f == fromFile) {
          sameFileConflict = true;
        }
        if (r == fromRank) {
          sameRankConflict = true;
        }
      }
    }

    if (hasOtherCandidate) {
      if (!sameFileConflict) {
        san += (char)('a' + fromFile);
      } else if (!sameRankConflict) {
        san += (char)('1' + fromRank);
      } else {
        san += (char)('a' + fromFile);
        san += (char)('1' + fromRank);
      }
    }
  }

  if (isCapture) {
    san += 'x';
  }
  san += toSquare;
  return san;
}

bool lichessAppendMove(LichessContext &ctx, const String &sanMove) {
  if (sanMove.length() == 0 || ctx.movesLen == nullptr || ctx.plyCount == nullptr ||
      ctx.movesBuf == nullptr || ctx.movesBufSize <= 1) {
    return false;
  }

  char normalized[32] = {0};
  size_t len = sanMove.length();
  if (len >= sizeof(normalized)) {
    len = sizeof(normalized) - 1;
  }

  for (size_t i = 0; i < len; i++) {
    char c = sanMove[i];
    if (c == '/' || c == ' ') {
      break;
    }
    normalized[i] = c;
  }

  size_t normLen = strlen(normalized);
  while (normLen > 0 && (normalized[normLen - 1] == '\n' || normalized[normLen - 1] == '\r' || normalized[normLen - 1] == ' ')) {
    normalized[--normLen] = '\0';
  }

  if (normLen == 0) {
    return false;
  }

  int prevLen = *ctx.movesLen;
  if (prevLen < 0) {
    prevLen = 0;
  }
  int maxLen = ctx.movesBufSize - 1;
  if (prevLen > maxLen) {
    prevLen = maxLen;
  }

  bool whiteMove = ((*ctx.plyCount) % 2 == 0);
  if (whiteMove) {
    if (*ctx.movesLen > 0) {
      if (!appendToMovesBuf(ctx, " ")) {
        *ctx.movesLen = prevLen;
        ctx.movesBuf[prevLen] = '\0';
        return false;
      }
    }
    int moveNo = (*ctx.plyCount / 2) + 1;
    if (!appendToMovesBuf(ctx, "%d. %s", moveNo, normalized)) {
      *ctx.movesLen = prevLen;
      ctx.movesBuf[prevLen] = '\0';
      return false;
    }
  } else {
    if (!appendToMovesBuf(ctx, " %s", normalized)) {
      *ctx.movesLen = prevLen;
      ctx.movesBuf[prevLen] = '\0';
      return false;
    }
  }

  (*ctx.plyCount)++;
  return true;
}

const char *lichessBuildPayload(const LichessContext &ctx, const char *prefix, char *outBuf, size_t outLen) {
  if (outBuf == nullptr || outLen == 0) {
    return "";
  }

  int state = (ctx.uploadState != nullptr) ? *ctx.uploadState : LICHESS_IDLE;
  int ply = (ctx.plyCount != nullptr) ? *ctx.plyCount : 0;
  int stream = (ctx.autoPublishPerMove != nullptr && *ctx.autoPublishPerMove) ? 1 : 0;
  int pending = (ctx.republishPending != nullptr && *ctx.republishPending) ? 1 : 0;
  const char *url = (ctx.lastUrl != nullptr) ? ctx.lastUrl->c_str() : "";
  const char *err = (ctx.lastError != nullptr) ? ctx.lastError->c_str() : "";

  snprintf(outBuf,
           outLen,
           "%s|state=%s|ply=%d|stream=%d|pending=%d|url=%s|err=%s",
           prefix,
           lichessStateName(state),
           ply,
           stream,
           pending,
           url,
           err);
  return outBuf;
}

bool lichessQueuePublish(LichessContext &ctx, String &response, LichessLogFn logFn) {
  if (ctx.uploadState == nullptr || ctx.republishPending == nullptr || ctx.plyCount == nullptr ||
      ctx.movesLen == nullptr || ctx.lastError == nullptr || ctx.lastUrl == nullptr) {
    response = "LICHESS_ERR:CTX";
    return false;
  }

  char payloadBuf[192];

  if (*ctx.uploadState == LICHESS_QUEUED || *ctx.uploadState == LICHESS_UPLOADING) {
    *ctx.republishPending = true;
    response = lichessBuildPayload(ctx, "LICHESS_BUSY", payloadBuf, sizeof(payloadBuf));
    return true;
  }

  if (WiFi.status() != WL_CONNECTED) {
    *ctx.lastError = "WIFI_NOT_CONNECTED";
    response = "LICHESS_ERR:WIFI_NOT_CONNECTED";
    return false;
  }

  if (*ctx.plyCount <= 0 || *ctx.movesLen == 0) {
    *ctx.lastError = "NO_MOVES";
    response = "LICHESS_ERR:NO_MOVES";
    return false;
  }

  *ctx.lastUrl = "";
  *ctx.lastError = "";
  *ctx.republishPending = false;
  *ctx.uploadState = LICHESS_QUEUED;
  response = lichessBuildPayload(ctx, "LICHESS_QUEUED", payloadBuf, sizeof(payloadBuf));
  if (logFn != nullptr) {
    logFn(String("[LICHESS] Queued upload ply=") + String(*ctx.plyCount));
  }
  return true;
}

void lichessRequestAutoPublish(LichessContext &ctx, LichessLogFn logFn) {
  if (ctx.autoPublishPerMove == nullptr || !*ctx.autoPublishPerMove ||
      ctx.plyCount == nullptr || ctx.movesLen == nullptr) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (*ctx.plyCount <= 0 || *ctx.movesLen == 0) {
    return;
  }

  String response;
  lichessQueuePublish(ctx, response, logFn);
}

void lichessProcessUploadTick(LichessContext &ctx, LichessLogFn logFn) {
  if (ctx.uploadState == nullptr || ctx.republishPending == nullptr ||
      ctx.lastUrl == nullptr || ctx.lastError == nullptr ||
      ctx.plyCount == nullptr || ctx.movesLen == nullptr) {
    return;
  }

  if (*ctx.uploadState != LICHESS_QUEUED) {
    return;
  }

  *ctx.uploadState = LICHESS_UPLOADING;
  if (logFn != nullptr) {
    logFn(String("[LICHESS] Uploading..."));
  }

  String publishResponse;
  if (publishCurrentGameToLichess(ctx, publishResponse, logFn)) {
    *ctx.uploadState = LICHESS_DONE;
  } else {
    *ctx.uploadState = LICHESS_ERROR;
    if (ctx.lastError->length() == 0) {
      *ctx.lastError = publishResponse;
    }
  }

  if (*ctx.republishPending && WiFi.status() == WL_CONNECTED && *ctx.plyCount > 0 && *ctx.movesLen > 0) {
    *ctx.republishPending = false;
    *ctx.lastUrl = "";
    *ctx.lastError = "";
    *ctx.uploadState = LICHESS_QUEUED;
    if (logFn != nullptr) {
      logFn(String("[LICHESS] Re-queued latest moves"));
    }
  }
}
