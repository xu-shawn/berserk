#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "eval.h"
#include "move.h"
#include "movegen.h"
#include "search.h"
#include "see.h"
#include "transposition.h"
#include "types.h"
#include "util.h"

const int CHECKMATE = 32767;
const int MATE_BOUND = 30000;

const int FUTILITY_MARGINS[] = {0, 90, 180, 280, 380, 490, 600};

void Search(Board* board, SearchParams* params, SearchData* data) {
  data->nodes = 0;
  data->seldepth = 1;

  ttClear();
  memset(board->killers, 0, sizeof(board->killers));
  memset(board->counters, 0, sizeof(board->counters));

  int alpha = -CHECKMATE;
  int beta = CHECKMATE;

  int score = negamax(alpha, beta, 1, 0, 1, board, params, data);

  for (int depth = 2; depth <= params->depth && !params->stopped; depth++) {
    int delta = depth >= 5 && abs(score) < 1500 ? 50 : CHECKMATE;
    alpha = max(score - delta, -CHECKMATE);
    beta = min(score + delta, CHECKMATE);

    while (!params->stopped) {
      score = negamax(alpha, beta, depth, 0, 1, board, params, data);

      if (score <= alpha && score > -MATE_BOUND) {
        alpha = max(alpha - delta, -CHECKMATE);
        delta *= 2;
      } else if (score >= beta && score < MATE_BOUND) {
        beta = min(beta + delta, CHECKMATE);
        delta *= 2;
      } else {
        break;
      }
    }
  }

  printf("bestmove %s\n", moveStr(ttMove(ttProbe(board->zobrist))));
}

void printPv(Move move, Board* board) {
  int depth = 0;
  Board temp[1];
  memcpy(temp, board, sizeof(Board));

  do {
    printf("%s ", moveStr(move));

    makeMove(move, temp);
    TTValue ttValue = ttProbe(temp->zobrist);
    move = ttMove(ttValue);
  } while (++depth < 64 && move);

  printf("\n");
}

int negamax(int alpha, int beta, int depth, int ply, int canNull, Board* board, SearchParams* params,
            SearchData* data) {
  // Repetition detection
  if (ply && isRepetition(board))
    return 0;

  // Mate distance pruning
  alpha = max(alpha, -CHECKMATE + ply);
  beta = min(beta, CHECKMATE - ply - 1);
  if (alpha >= beta)
    return alpha;

  // Check extension
  int currInCheck = inCheck(board);
  if (currInCheck)
    depth++;

  if (depth == 0)
    return quiesce(alpha, beta, ply, board, params, data);

  if ((data->nodes & 2047) == 0)
    communicate(params);

  TTValue ttValue = ttProbe(board->zobrist);
  if (ttValue) {
    if (ttDepth(ttValue) >= depth) {
      int score = ttScore(ttValue, ply);
      int flag = ttFlag(ttValue);

      if (flag == TT_EXACT)
        return score;
      if (flag == TT_LOWER && score >= beta)
        return score;
      if (flag == TT_UPPER && score <= alpha)
        return score;
    }
  }

  data->nodes++;

  int isPV = beta - alpha != 1 ? 1 : 0;
  int bestScore = -CHECKMATE, a0 = alpha;
  Move bestMove = 0;

  int staticEval = Evaluate(board);
  if (!isPV && !currInCheck) {
    // Reverse Futility Pruning
    if (depth <= 6 && staticEval - FUTILITY_MARGINS[depth] >= beta && staticEval < MATE_BOUND)
      return staticEval;

    // Null move pruning
    if (depth > 3 && canNull) {
      int R = 3;
      if (depth >= 6)
        R++;

      nullMove(board);
      int score = -negamax(-beta, -beta + 1, depth - R, ply + 1, 0, board, params, data);
      undoNullMove(board);

      if (params->stopped)
        return 0;

      if (score >= beta)
        return beta;
    }
  }

  MoveList moveList[1];
  generateMoves(moveList, board, ply);

  int numMoves = 0;
  for (int i = 0; i < moveList->count; i++) {
    bubbleTopMove(moveList, i);
    Move move = moveList->moves[i];

    int score = alpha + 1;
    int newDepth = depth - 1;

    if (!isPV && bestScore > -MATE_BOUND &&
        (board->pieces[KNIGHT[board->side]] | board->pieces[BISHOP[board->side]] | board->pieces[ROOK[board->side]] |
         board->pieces[QUEEN[board->side]])) {

      int lmrDepth = numMoves <= 6 ? newDepth - 1 : newDepth - depth / 2;
      lmrDepth = max(0, lmrDepth);

      int seeCutoff = moveCapture(move) ? (-80 * depth) : (-25 * lmrDepth * lmrDepth);

      // SEE pruning
      if (see(board, move) < seeCutoff)
        continue;
    }

    numMoves++;
    makeMove(move, board);

    // Start LMR
    int doZws = (!isPV || numMoves > 1) ? 1 : 0;

    int givesCheck = inCheck(board);
    if (depth >= 3 && numMoves > (!ply ? 3 : 1) && !moveCapture(move) && !movePromo(move) && !givesCheck &&
        !currInCheck) {
      // Senpai logic
      int R = numMoves <= 6 ? 1 : depth / 2;
      R = min(newDepth, R);

      score = -negamax(-alpha - 1, -alpha, newDepth - R, ply + 1, 1, board, params, data);
      doZws = (score > alpha) ? 1 : 0;
    }

    if (doZws)
      score = -negamax(-alpha - 1, -alpha, newDepth, ply + 1, 1, board, params, data);

    if (isPV && (numMoves == 1 || (score > alpha && (!ply || score < beta))))
      score = -negamax(-beta, -alpha, newDepth, ply + 1, 1, board, params, data);

    undoMove(move, board);

    if (params->stopped)
      return 0;

    if (score > bestScore) {
      bestScore = score;
      bestMove = move;

      if (score > alpha)
        alpha = score;

      if (alpha >= beta) {
        if (!moveCapture(move) && !movePromo(move)) {
          addKiller(board, move, ply);
          addCounter(board, move, board->gameMoves[board->moveNo - 1]);
        }

        break;
      }
    }
  }

  // Checkmate detection
  if (!moveList->count)
    return inCheck(board) ? -CHECKMATE + ply : 0;

  if (!ply && !params->stopped) {
    if (bestScore > MATE_BOUND) {
      int movesToMate = (CHECKMATE - bestScore) / 2 + ((CHECKMATE - bestScore) & 1);

      printf("info depth %d seldepth %d nodes %d time %ld score mate %d pv ", depth, data->seldepth, data->nodes,
             getTimeMs() - params->startTime, movesToMate);
    } else if (bestScore < -MATE_BOUND) {
      int movesToMate = (CHECKMATE + bestScore) / 2 - ((CHECKMATE - bestScore) & 1);

      printf("info depth %d seldepth %d  nodes %d time %ld score mate -%d pv ", depth, data->seldepth, data->nodes,
             getTimeMs() - params->startTime, movesToMate);
    } else {
      printf("info depth %d seldepth %d nodes %d time %ld score cp %d pv ", depth, data->seldepth, data->nodes,
             getTimeMs() - params->startTime, bestScore);
    }
    printPv(bestMove, board);
  }

  int ttFlag = bestScore >= beta ? TT_LOWER : bestScore <= a0 ? TT_UPPER : TT_EXACT;
  ttPut(board->zobrist, depth, bestScore, ttFlag, bestMove, ply);

  return bestScore;
}

int quiesce(int alpha, int beta, int ply, Board* board, SearchParams* params, SearchData* data) {
  if ((data->nodes & 2047) == 0)
    communicate(params);

  TTValue ttValue = ttProbe(board->zobrist);
  if (ttValue) {
    int score = ttScore(ttValue, ply);
    int flag = ttFlag(ttValue);

    if (flag == TT_EXACT)
      return score;
    if (flag == TT_LOWER && score >= beta)
      return score;
    if (flag == TT_UPPER && score <= alpha)
      return score;
  }

  data->nodes++;
  if (ply > data->seldepth)
    data->seldepth = ply;

  int eval = Evaluate(board);

  if (eval >= beta)
    return beta;

  if (eval > alpha)
    alpha = eval;

  MoveList moveList[1];
  generateQuiesceMoves(moveList, board);

  for (int i = 0; i < moveList->count; i++) {
    bubbleTopMove(moveList, i);
    Move move = moveList->moves[i];

    if (movePromo(move) && movePromo(move) < QUEEN[WHITE])
      continue;

    int captured = capturedPiece(move, board);
    if (eval + 200 + scoreMG(materialValues[captured]) < alpha)
      continue;

    makeMove(move, board);
    int score = -quiesce(-beta, -alpha, ply + 1, board, params, data);
    undoMove(move, board);

    if (params->stopped)
      return 0;

    if (score >= beta)
      return beta;
    if (score > alpha)
      alpha = score;
  }

  return alpha;
}