/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2018 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef POSITION_H_INCLUDED
#define POSITION_H_INCLUDED

#include <cassert>
#include <deque>
#include <memory> // For std::unique_ptr
#include <string>

#include "bitboard.h"
#include "types.h"
#include "betza.h"


/// StateInfo struct stores information needed to restore a Position object to
/// its previous state when we retract a move. Whenever a move is made on the
/// board (by calling Position::do_move), a StateInfo object must be passed.

struct StateInfo {

  // Copied when making a move
  Key    pawnKey;
  Key    materialKey;
  Value  nonPawnMaterial[COLOR_NB];
  int    castlingRights;
  int    rule50;
  int    pliesFromNull;
  Score  psq;
  Square epSquare;

  // Not copied when making a move (will be recomputed anyhow)
  Key        key;
  Bitboard   checkersBB;
  Piece      capturedPiece;
  Gate       gate;
  Gate       capturedGate;
  PieceType  removedGatingType;
  StateInfo* previous;
  Bitboard   blockersForKing[COLOR_NB];
  Bitboard   pinners[COLOR_NB];
  Bitboard   checkSquares[PIECE_TYPE_NB];
};

/// A list to keep track of the position states along the setup moves (from the
/// start position to the position just before the search starts). Needed by
/// 'draw by repetition' detection. Use a std::deque because pointers to
/// elements are not invalidated upon list resizing.
typedef std::unique_ptr<std::deque<StateInfo>> StateListPtr;


/// Position class stores information regarding the board representation as
/// pieces, side to move, hash keys, castling info, etc. Important methods are
/// do_move() and undo_move(), used by the search to update node info when
/// traversing the search tree.
class Thread;

class Position {
public:
  static void init();

  Position() = default;
  Position(const Position&) = delete;
  Position& operator=(const Position&) = delete;

  // FEN string input/output
  Position& set(const std::string& fenStr, bool isChess960, StateInfo* si, Thread* th);
  Position& set(const std::string& code, Color c, StateInfo* si);
  const std::string fen() const;

  // Position representation
  Bitboard pieces() const;
  Bitboard pieces(PieceType pt) const;
  Bitboard pieces(PieceType pt1, PieceType pt2) const;
  Bitboard pieces(Color c) const;
  Bitboard pieces(Color c, PieceType pt) const;
  Bitboard pieces(Color c, PieceType pt1, PieceType pt2) const;
  Piece piece_on(Square s) const;
  Square ep_square() const;
  bool empty(Square s) const;
  template<PieceType Pt> int count(Color c) const;
  template<PieceType Pt> int count() const;
  template<PieceType Pt> const Square* squares(Color c) const;
  const Square* squares(Color c, PieceType pt) const;
  template<PieceType Pt> Square square(Color c) const;
  Bitboard gates() const;
  PieceType gating_piece(Gate gate) const;
  PieceType gating_piece(Square s) const;
  Square gating_square(Color c, Gate gate) const;

  // Castling
  int can_castle(Color c) const;
  int can_castle(CastlingRight cr) const;
  bool castling_impeded(CastlingRight cr) const;
  Square castling_rook_square(CastlingRight cr) const;

  // Checking
  Bitboard checkers() const;
  Bitboard blockers_for_king(Color c) const;
  Bitboard check_squares(PieceType pt) const;

  // Attacks to/from a given square
  Bitboard attackers_to(Square s) const;
  Bitboard attackers_to(Square s, Bitboard occupied) const;
  Bitboard attacks_from(Color c, PieceType pt, Square s) const;
  template<PieceType> Bitboard attacks_from(Color c, Square s) const;
  Bitboard attacks_from_betza(Color c, PieceType pt, Square s) const;
  Bitboard slider_blockers(Bitboard sliders, Square s, Bitboard& pinners) const;

  // Properties of moves
  bool legal(Move m) const;
  bool pseudo_legal(const Move m) const;
  bool capture(Move m) const;
  bool capture_or_promotion(Move m) const;
  bool gives_check(Move m) const;
  bool advanced_pawn_push(Move m) const;
  Piece moved_piece(Move m) const;
  Piece captured_piece() const;

  // Piece specific
  bool pawn_passed(Color c, Square s) const;
  bool opposite_bishops() const;

  // Doing and undoing moves
  void do_move(Move m, StateInfo& newSt);
  void do_move(Move m, StateInfo& newSt, bool givesCheck);
  void undo_move(Move m);
  void do_null_move(StateInfo& newSt);
  void undo_null_move();

  // Static Exchange Evaluation
  bool see_ge(Move m, Value threshold = VALUE_ZERO) const;

  // Accessing hash keys
  Key key() const;
  Key key_after(Move m) const;
  Key material_key() const;
  Key pawn_key() const;

  // Other properties of the position
  Color side_to_move() const;
  int game_ply() const;
  bool is_chess960() const;
  Thread* this_thread() const;
  bool is_draw(int ply) const;
  bool has_game_cycle(int ply) const;
  bool has_repeated() const;
  int rule50_count() const;
  Score psq_score() const;
  Value non_pawn_material(Color c) const;
  Value non_pawn_material() const;
  GamePhase game_phase() const;
  Gate gate_count() const;
  Gate setup_count(Color c) const;

  // Position consistency check, for debugging
  bool pos_is_ok() const;
  void flip();

private:
  // Initialization helpers (used while setting up a position)
  void set_castling_right(Color c, Square rfrom);
  void set_state(StateInfo* si) const;
  void set_check_info(StateInfo* si) const;

  // Other helpers
  void set_gating_type(PieceType pt);
  void unset_gating_type();
  void add_gate(Color c, Square s, Gate gate);
  void remove_gate(Color c, Square s, Gate gate);
  void put_gating_piece(Color c, Square s);
  void remove_gating_piece(Color c, Square s);
  void capture_gate(Color c, Square s);
  void uncapture_gate(Color c, Square s);
  void gate_piece(Color c, Square s);
  void ungate_piece(Color c, Square s);
  void put_piece(Piece pc, Square s);
  void remove_piece(Piece pc, Square s);
  void move_piece(Piece pc, Square from, Square to);
  template<bool Do>
  void do_castling(Color us, Square from, Square& to, Square& rfrom, Square& rto, Key& k);

  // Data members
  Piece board[SQUARE_NB];
  Gate gateBoard[SQUARE_NB];
  PieceType gatingPieces[GATE_NB];
  Square gatingSquares[COLOR_NB][GATE_NB];
  Bitboard byTypeBB[PIECE_TYPE_NB];
  Bitboard byColorBB[COLOR_NB];
  Bitboard gateBB;
  int pieceCount[PIECE_NB];
  Gate gateCount;
  Gate setupCount[COLOR_NB];
  Square pieceList[PIECE_NB][16];
  int index[SQUARE_NB];
  int castlingRightsMask[SQUARE_NB];
  Square castlingRookSquare[CASTLING_RIGHT_NB];
  Bitboard castlingPath[CASTLING_RIGHT_NB];
  int gamePly;
  Color sideToMove;
  Thread* thisThread;
  StateInfo* st;
  bool chess960;
};

extern std::ostream& operator<<(std::ostream& os, const Position& pos);

inline Color Position::side_to_move() const {
  return sideToMove;
}

inline bool Position::empty(Square s) const {
  return board[s] == NO_PIECE;
}

inline Piece Position::piece_on(Square s) const {
  return board[s];
}

inline Piece Position::moved_piece(Move m) const {
  if (type_of(m) == SET_GATING_TYPE || type_of(m) == PUT_GATING_PIECE)
      return make_piece(sideToMove, gating_type(m));
  return board[from_sq(m)];
}

inline Bitboard Position::pieces() const {
  return byTypeBB[ALL_PIECES];
}

inline Bitboard Position::pieces(PieceType pt) const {
  return byTypeBB[pt];
}

inline Bitboard Position::pieces(PieceType pt1, PieceType pt2) const {
  return byTypeBB[pt1] | byTypeBB[pt2];
}

inline Bitboard Position::pieces(Color c) const {
  return byColorBB[c];
}

inline Bitboard Position::pieces(Color c, PieceType pt) const {
  return byColorBB[c] & byTypeBB[pt];
}

inline Bitboard Position::pieces(Color c, PieceType pt1, PieceType pt2) const {
  return byColorBB[c] & (byTypeBB[pt1] | byTypeBB[pt2]);
}

template<PieceType Pt> inline int Position::count(Color c) const {
  return pieceCount[make_piece(c, Pt)];
}

template<PieceType Pt> inline int Position::count() const {
  return pieceCount[make_piece(WHITE, Pt)] + pieceCount[make_piece(BLACK, Pt)];
}

template<PieceType Pt> inline const Square* Position::squares(Color c) const {
  return pieceList[make_piece(c, Pt)];
}

inline const Square* Position::squares(Color c, PieceType pt) const {
  return pieceList[make_piece(c, pt)];
}

template<PieceType Pt> inline Square Position::square(Color c) const {
  assert(pieceCount[make_piece(c, Pt)] == 1);
  return pieceList[make_piece(c, Pt)][0];
}

inline Bitboard Position::gates() const {
  return gateBB;
}

inline PieceType Position::gating_piece(Gate gate) const {
  assert(gate >= NO_GATE && gate < GATE_NB);
  return gatingPieces[gate];
}

inline PieceType Position::gating_piece(Square s) const {
  assert(rank_of(s) == RANK_1 || rank_of(s) == RANK_8);
  assert(gateBoard[s] > NO_GATE && gateBoard[s] < GATE_NB);
  return gating_piece(gateBoard[s]);
}

inline Square Position::gating_square(Color c, Gate gate) const {
  assert(gate < GATE_NB);
  return gatingSquares[c][gate];
}

inline Square Position::ep_square() const {
  return st->epSquare;
}

inline int Position::can_castle(CastlingRight cr) const {
  return st->castlingRights & cr;
}

inline int Position::can_castle(Color c) const {
  return st->castlingRights & ((WHITE_OO | WHITE_OOO) << (2 * c));
}

inline bool Position::castling_impeded(CastlingRight cr) const {
  return byTypeBB[ALL_PIECES] & castlingPath[cr];
}

inline Square Position::castling_rook_square(CastlingRight cr) const {
  return castlingRookSquare[cr];
}

template<PieceType Pt>
inline Bitboard Position::attacks_from(Color c, Square s) const {
  return attacks_bb(c, Pt, s, byTypeBB[ALL_PIECES]);
}

inline Bitboard Position::attacks_from(Color c, PieceType pt, Square s) const {
  return attacks_bb(c, pt, s, byTypeBB[ALL_PIECES]);
}

inline Bitboard Position::attacks_from_betza(Color c, PieceType pt, Square s) const {
  return ::attacks_from_betza(c, pt, s);
}

inline Bitboard Position::attackers_to(Square s) const {
  return attackers_to(s, byTypeBB[ALL_PIECES]);
}

inline Bitboard Position::checkers() const {
  return st->checkersBB;
}

inline Bitboard Position::blockers_for_king(Color c) const {
  return st->blockersForKing[c];
}

inline Bitboard Position::check_squares(PieceType pt) const {
  return st->checkSquares[pt];
}

inline bool Position::pawn_passed(Color c, Square s) const {
  return !(pieces(~c, PAWN) & passed_pawn_mask(c, s));
}

inline bool Position::advanced_pawn_push(Move m) const {
  return   type_of(moved_piece(m)) == PAWN
        && relative_rank(sideToMove, from_sq(m)) > RANK_4;
}

inline Key Position::key() const {
  return st->key;
}

inline Key Position::pawn_key() const {
  return st->pawnKey;
}

inline Key Position::material_key() const {
  return st->materialKey;
}

inline Score Position::psq_score() const {
  return st->psq;
}

inline Value Position::non_pawn_material(Color c) const {
  return st->nonPawnMaterial[c];
}

inline Value Position::non_pawn_material() const {
  return st->nonPawnMaterial[WHITE] + st->nonPawnMaterial[BLACK];
}

inline GamePhase Position::game_phase() const {
  return  gateCount < GATE_NB - 1 ? GAMEPHASE_SELECTION
        : setupCount[sideToMove] < GATE_NB - 1 ? GAMEPHASE_PLACING
        : GAMEPHASE_PLAYING;
}

inline Gate Position::gate_count() const {
  return gateCount;
}

inline Gate Position::setup_count(Color c) const {
  return setupCount[c];
}

inline int Position::game_ply() const {
  return gamePly;
}

inline int Position::rule50_count() const {
  return st->rule50;
}

inline bool Position::opposite_bishops() const {
  return   pieceCount[make_piece(WHITE, BISHOP)] == 1
        && pieceCount[make_piece(BLACK, BISHOP)] == 1
        && opposite_colors(square<BISHOP>(WHITE), square<BISHOP>(BLACK));
}

inline bool Position::is_chess960() const {
  return chess960;
}

inline bool Position::capture_or_promotion(Move m) const {
  assert(is_ok(m));
  return type_of(m) != NORMAL ? type_of(m) != CASTLING : !empty(to_sq(m));
}

inline bool Position::capture(Move m) const {
  assert(is_ok(m));
  // Castling is encoded as "king captures rook"
  return (!empty(to_sq(m)) && type_of(m) != CASTLING) || type_of(m) == ENPASSANT;
}

inline Piece Position::captured_piece() const {
  return st->capturedPiece;
}

inline Thread* Position::this_thread() const {
  return thisThread;
}

inline void Position::set_gating_type(PieceType pt) {
  assert(gateCount < GATE_NB);
  gatingPieces[++gateCount] = pt;
}

inline void Position::unset_gating_type() {
  assert(gateCount > NO_GATE);
  gatingPieces[gateCount--] = NO_PIECE_TYPE;
}

inline void Position::add_gate(Color c, Square s, Gate gate) {
  assert(gate > NO_GATE && gate < GATE_NB);
  assert(!(gateBB & s));
  assert(gateBoard[s] == NO_GATE);
  gateBoard[s] = gate;
  gatingSquares[c][gate] = s;
  gateBB |= s;
}

inline void Position::remove_gate(Color c, Square s, Gate gate) {
  assert(gate > NO_GATE && gate < GATE_NB);
  assert(gateBB & s);
  assert(gateBoard[s] > NO_GATE);
  gateBoard[s] = NO_GATE;
  gatingSquares[c][gate] = SQ_NONE;
  gateBB ^= s;
}

inline void Position::put_gating_piece(Color c, Square s) {
  add_gate(c, s, ++setupCount[c]);
}

inline void Position::remove_gating_piece(Color c, Square s) {
  remove_gate(c, s, setupCount[c]--);
}

inline void Position::capture_gate(Color c, Square s) {
  assert(gatingPieces[gateBoard[s]] > NO_PIECE_TYPE);
  st->capturedGate = gateBoard[s];
  remove_gate(c, s, gateBoard[s]);
}

inline void Position::uncapture_gate(Color c, Square s) {
  assert(board[s] != NO_PIECE);
  add_gate(c, s, st->capturedGate);
}

inline void Position::gate_piece(Color c, Square s) {
  assert(gatingPieces[gateBoard[s]] > NO_PIECE_TYPE);
  assert(board[s] == NO_PIECE);
  st->gate = gateBoard[s];
  put_piece(make_piece(c, gatingPieces[gateBoard[s]]), s);
  remove_gate(c, s, gateBoard[s]);
}

inline void Position::ungate_piece(Color c, Square s) {
  assert(board[s] != NO_PIECE);
  assert(gatingPieces[st->gate] == type_of(board[s]));
  add_gate(c, s, st->gate);
  remove_piece(make_piece(c, gatingPieces[st->gate]), s);
}

inline void Position::put_piece(Piece pc, Square s) {

  board[s] = pc;
  byTypeBB[ALL_PIECES] |= s;
  byTypeBB[type_of(pc)] |= s;
  byColorBB[color_of(pc)] |= s;
  index[s] = pieceCount[pc]++;
  pieceList[pc][index[s]] = s;
  pieceCount[make_piece(color_of(pc), ALL_PIECES)]++;
}

inline void Position::remove_piece(Piece pc, Square s) {

  // WARNING: This is not a reversible operation. If we remove a piece in
  // do_move() and then replace it in undo_move() we will put it at the end of
  // the list and not in its original place, it means index[] and pieceList[]
  // are not invariant to a do_move() + undo_move() sequence.
  byTypeBB[ALL_PIECES] ^= s;
  byTypeBB[type_of(pc)] ^= s;
  byColorBB[color_of(pc)] ^= s;
  /* board[s] = NO_PIECE;  Not needed, overwritten by the capturing one */
  Square lastSquare = pieceList[pc][--pieceCount[pc]];
  index[lastSquare] = index[s];
  pieceList[pc][index[lastSquare]] = lastSquare;
  pieceList[pc][pieceCount[pc]] = SQ_NONE;
  pieceCount[make_piece(color_of(pc), ALL_PIECES)]--;
}

inline void Position::move_piece(Piece pc, Square from, Square to) {

  // index[from] is not updated and becomes stale. This works as long as index[]
  // is accessed just by known occupied squares.
  Bitboard fromTo = SquareBB[from] ^ SquareBB[to];
  byTypeBB[ALL_PIECES] ^= fromTo;
  byTypeBB[type_of(pc)] ^= fromTo;
  byColorBB[color_of(pc)] ^= fromTo;
  board[from] = NO_PIECE;
  board[to] = pc;
  index[to] = index[from];
  pieceList[pc][index[to]] = to;
}

inline void Position::do_move(Move m, StateInfo& newSt) {
  do_move(m, newSt, gives_check(m));
}

#endif // #ifndef POSITION_H_INCLUDED
