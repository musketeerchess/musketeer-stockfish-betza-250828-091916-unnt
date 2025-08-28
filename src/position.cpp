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

#include <algorithm>
#include <cassert>
#include <cstddef> // For offsetof()
#include <cstring> // For std::memset, std::memcmp
#include <iomanip>
#include <sstream>

#include "bitboard.h"
#include "misc.h"
#include "movegen.h"
#include "position.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"
#include "syzygy/tbprobe.h"

using std::string;

namespace PSQT {
  extern Score psq[PIECE_NB][SQUARE_NB];
  extern Score psq_gate[PIECE_NB][FILE_NB];
}

namespace Zobrist {

  Key psq[PIECE_NB][SQUARE_NB];
  Key psq_gate[PIECE_NB][FILE_NB];
  Key inhand[PIECE_TYPE_NB][GATE_NB];
  Key enpassant[FILE_NB];
  Key castling[CASTLING_RIGHT_NB];
  Key side, noPawns;
}

namespace {

// min_attacker() is a helper function used by see_ge() to locate the least
// valuable attacker for the side to move, remove the attacker we just found
// from the bitboards and scan for new X-ray attacks behind it.

template<int Pt>
PieceType min_attacker(const Bitboard* byTypeBB, Square to, Bitboard stmAttackers,
                       Bitboard& occupied, Bitboard& attackers) {

  Bitboard b = stmAttackers & byTypeBB[Pt];
  if (!b)
      return min_attacker<Pt + 1>(byTypeBB, to, stmAttackers, occupied, attackers);

  occupied ^= lsb(b); // Remove the attacker from occupied

  // Add any X-ray attack behind the just removed piece. For instance with
  // rooks in a8 and a7 attacking a1, after removing a7 we add rook in a8.
  // Note that new added attackers can be of any color.
  if (Pt == PAWN || Pt == BISHOP || Pt == QUEEN)
      attackers |= attacks_bb<BISHOP>(to, occupied) & (byTypeBB[BISHOP] | byTypeBB[QUEEN]);

  if (Pt == ROOK || Pt == QUEEN)
      attackers |= attacks_bb<ROOK>(to, occupied) & (byTypeBB[ROOK] | byTypeBB[QUEEN]);

  // X-ray may add already processed pieces because byTypeBB[] is constant: in
  // the rook example, now attackers contains _again_ rook in a7, so remove it.
  attackers &= occupied;
  return (PieceType)Pt;
}

template<>
PieceType min_attacker<KING>(const Bitboard*, Square, Bitboard, Bitboard&, Bitboard&) {
  return KING; // No need to update bitboards: it is the last cycle
}

} // namespace


/// operator<<(Position) returns an ASCII representation of the position

std::ostream& operator<<(std::ostream& os, const Position& pos) {

  os << "\n +---+---+---+---+---+---+---+---+\n";

  for (Rank r = RANK_8; r >= RANK_1; --r)
  {
      for (File f = FILE_A; f <= FILE_H; ++f)
          os << " | " << PieceToChar[pos.piece_on(make_square(f, r))];

      os << " |\n +---+---+---+---+---+---+---+---+\n";
  }

  os << "\nFen: " << pos.fen() << "\nKey: " << std::hex << std::uppercase
     << std::setfill('0') << std::setw(16) << pos.key()
     << std::setfill(' ') << std::dec << "\nCheckers: ";

  for (Bitboard b = pos.checkers(); b; )
      os << UCI::square(pop_lsb(&b)) << " ";

  if (    int(Tablebases::MaxCardinality) >= popcount(pos.pieces())
      && !pos.can_castle(ANY_CASTLING))
  {
      StateInfo st;
      Position p;
      p.set(pos.fen(), pos.is_chess960(), &st, pos.this_thread());
      Tablebases::ProbeState s1, s2;
      Tablebases::WDLScore wdl = Tablebases::probe_wdl(p, &s1);
      int dtz = Tablebases::probe_dtz(p, &s2);
      os << "\nTablebases WDL: " << std::setw(4) << wdl << " (" << s1 << ")"
         << "\nTablebases DTZ: " << std::setw(4) << dtz << " (" << s2 << ")";
  }

  return os;
}


// Marcel van Kervinck's cuckoo algorithm for fast detection of "upcoming repetition"
// situations. Description of the algorithm in the following paper:
// https://marcelk.net/2013-04-06/paper/upcoming-rep-v2.pdf

// First and second hash functions for indexing the cuckoo tables
inline int H1(Key h) { return h & 0x1fff; }
inline int H2(Key h) { return (h >> 16) & 0x1fff; }

// Cuckoo tables with Zobrist hashes of valid reversible moves, and the moves themselves
Key cuckoo[8192];
Move cuckooMove[8192];


/// Position::init() initializes at startup the various arrays used to compute
/// hash keys.

void Position::init() {

  PRNG rng(1070372);

  for (Color c = WHITE; c <= BLACK; ++c)
      for (PieceType pt = PAWN; pt <= KING; ++pt)
          for (Square s = SQ_A1; s <= SQ_H8; ++s)
              Zobrist::psq[make_piece(c, pt)][s] = rng.rand<Key>();

  for (Color c = WHITE; c <= BLACK; ++c)
      for (PieceType pt = PAWN; pt <= KING; ++pt)
          for (File f = FILE_A; f <= FILE_H; ++f)
              Zobrist::psq_gate[make_piece(c, pt)][f] = rng.rand<Key>();

  for (PieceType pt = PAWN; pt <= KING; ++pt)
      for (Gate g = NO_GATE; g < GATE_NB; ++g)
          Zobrist::inhand[pt][g] = rng.rand<Key>();

  for (File f = FILE_A; f <= FILE_H; ++f)
      Zobrist::enpassant[f] = rng.rand<Key>();

  for (int cr = NO_CASTLING; cr <= ANY_CASTLING; ++cr)
  {
      Zobrist::castling[cr] = 0;
      Bitboard b = cr;
      while (b)
      {
          Key k = Zobrist::castling[1ULL << pop_lsb(&b)];
          Zobrist::castling[cr] ^= k ? k : rng.rand<Key>();
      }
  }

  Zobrist::side = rng.rand<Key>();
  Zobrist::noPawns = rng.rand<Key>();

  // Prepare the cuckoo tables
  int count = 0;
  for (Color c = WHITE; c <= BLACK; ++c)
      for (PieceType pt = KNIGHT; pt <= QUEEN || pt == KING; pt != QUEEN ? ++pt: pt = KING)
      {
      Piece pc = make_piece(c, pt);
      for (Square s1 = SQ_A1; s1 <= SQ_H8; ++s1)
          for (Square s2 = Square(s1 + 1); s2 <= SQ_H8; ++s2)
              if (PseudoAttacks[WHITE][type_of(pc)][s1] & s2)
              {
                  Move move = make_move(s1, s2);
                  Key key = Zobrist::psq[pc][s1] ^ Zobrist::psq[pc][s2] ^ Zobrist::side;
                  int i = H1(key);
                  while (true)
                  {
                      std::swap(cuckoo[i], key);
                      std::swap(cuckooMove[i], move);
                      if (move == 0)   // Arrived at empty slot ?
                          break;
                      i = (i == H1(key)) ? H2(key) : H1(key); // Push victim to alternative slot
                  }
                  count++;
             }
      }
  assert(count == 3668);
}


/// Position::set() initializes the position object with the given FEN string.
/// This function is not very robust - make sure that input FENs are correct,
/// this is assumed to be the responsibility of the GUI.

Position& Position::set(const string& fenStr, bool isChess960, StateInfo* si, Thread* th) {
/*
   A FEN string defines a particular position using only the ASCII character set.

   A FEN string contains six fields separated by a space. The fields are:

   1) Piece placement (from white's perspective). Each rank is described, starting
      with rank 8 and ending with rank 1. Within each rank, the contents of each
      square are described from file A through file H. Following the Standard
      Algebraic Notation (SAN), each piece is identified by a single letter taken
      from the standard English names. White pieces are designated using upper-case
      letters ("PNBRQK") whilst Black uses lowercase ("pnbrqk"). Blank squares are
      noted using digits 1 through 8 (the number of blank squares), and "/"
      separates ranks.

   2) Active color. "w" means white moves next, "b" means black.

   3) Castling availability. If neither side can castle, this is "-". Otherwise,
      this has one or more letters: "K" (White can castle kingside), "Q" (White
      can castle queenside), "k" (Black can castle kingside), and/or "q" (Black
      can castle queenside).

   4) En passant target square (in algebraic notation). If there's no en passant
      target square, this is "-". If a pawn has just made a 2-square move, this
      is the position "behind" the pawn. This is recorded only if there is a pawn
      in position to make an en passant capture, and if there really is a pawn
      that might have advanced two squares.

   5) Halfmove clock. This is the number of halfmoves since the last pawn advance
      or capture. This is used to determine if a draw can be claimed under the
      fifty-move rule.

   6) Fullmove number. The number of the full move. It starts at 1, and is
      incremented after Black's move.
*/

  unsigned char col, row, token;
  size_t idx;
  Square sq = SQ_A8;
  std::istringstream ss(fenStr);

  std::memset(this, 0, sizeof(Position));
  std::memset(si, 0, sizeof(StateInfo));
  std::fill_n(&pieceList[0][0], sizeof(pieceList) / sizeof(Square), SQ_NONE);
  st = si;

  ss >> std::noskipws;

  // Detect FEN format
  bool xboard = ss.str().find('*') != std::string::npos;

  // 1. Piece placement
  // Black gating pieces in XBoard format
  if (xboard)
  {
      for (Square s = SQ_A8; (ss >> token) && token != '/' && s <= SQ_H8; s++)
      {
          if ((idx = PieceToChar.find(token)) != string::npos)
          {
              set_gating_type(type_of(Piece(idx)));
              put_gating_piece(BLACK, s);
          }
      }
  }

  while ((ss >> token) && !isspace(token) && token != '[')
  {
      if (isdigit(token))
          sq += (token - '0') * EAST; // Advance the given number of files

      else if (token == '/')
      {
          sq += 2 * SOUTH;
          if (sq < SQ_A1)
              break;
      }

      else if ((idx = PieceToChar.find(token)) != string::npos)
      {
          put_piece(Piece(idx), sq);
          ++sq;
      }
  }

  // White gating pieces in XBoard format
  if (xboard)
  {
      Square s1 = SQ_A1, s2 = SQ_A1;
      for (Square s = SQ_A1; (ss >> token) && !isspace(token) && s <= SQ_H1; s++)
      {
          if ((idx = PieceToChar.find(token)) != string::npos)
          {
              if (type_of(Piece(idx)) == gating_piece(GATE_1))
                  s1 = s;
              else
                  s2 = s;
          }
      }
      put_gating_piece(WHITE, s1);
      put_gating_piece(WHITE, s2);
  }
  // Gating pieces
  else if (token == '[')
      while ((ss >> token) && !isspace(token))
      {
          // Allow various formats here. Rank and slash are optional.
          if (token == ']' || token == '/')
              continue;
          else if ((idx = PieceToChar.find(token)) != string::npos)
          {
              Piece pc = Piece(idx);
              Color c = color_of(pc);
              PieceType pt = type_of(pc);

              if (c == WHITE)
                  set_gating_type(pt);
              ss >> token;
              if (token >= 'a' && token <= 'h')
              {
                  put_gating_piece(c, make_square(File(token - 'a'), c == WHITE ? RANK_1 : RANK_8));
                  // Consume next character in case of optionally specified rank
                  if (ss.peek() != EOF && isdigit(ss.peek()))
                      ss >> token;
              }
              else if (token == '-')
                  gatingSquares[c][++setupCount[c]] = SQ_NONE;
              else if (token == '?')
                  continue;
          }
      }

  // 2. Active color
  ss >> token;
  sideToMove = (token == 'w' ? WHITE : BLACK);
  ss >> token;

  // 3. Castling availability. Compatible with 3 standards: Normal FEN standard,
  // Shredder-FEN that uses the letters of the columns on which the rooks began
  // the game instead of KQkq and also X-FEN standard that, in case of Chess960,
  // if an inner rook is associated with the castling right, the castling tag is
  // replaced by the file letter of the involved rook, as for the Shredder-FEN.
  while ((ss >> token) && !isspace(token))
  {
      Square rsq;
      Color c = islower(token) ? BLACK : WHITE;
      Piece rook = make_piece(c, ROOK);

      token = char(toupper(token));

      if (token == 'K')
          for (rsq = relative_square(c, SQ_H1); piece_on(rsq) != rook; --rsq) {}

      else if (token == 'Q')
          for (rsq = relative_square(c, SQ_A1); piece_on(rsq) != rook; ++rsq) {}

      else if (isChess960 && token >= 'A' && token <= 'H')
          rsq = make_square(File(token - 'A'), relative_rank(c, RANK_1));

      else
          continue;

      set_castling_right(c, rsq);
  }

  // 4. En passant square. Ignore if no pawn capture is possible
  if (   ((ss >> col) && (col >= 'a' && col <= 'h'))
      && ((ss >> row) && (row == '3' || row == '6')))
  {
      st->epSquare = make_square(File(col - 'a'), Rank(row - '1'));

      if (   !(attackers_to(st->epSquare) & pieces(sideToMove, PAWN))
          || !(pieces(~sideToMove, PAWN) & (st->epSquare + pawn_push(~sideToMove))))
          st->epSquare = SQ_NONE;
  }
  else
      st->epSquare = SQ_NONE;

  // 5-6. Halfmove clock and fullmove number
  ss >> std::skipws >> st->rule50 >> gamePly;

  // Convert from fullmove starting from 1 to gamePly starting from 0,
  // handle also common incorrect FEN with fullmove = 0.
  gamePly = std::max(2 * (gamePly - 1), 0) + (sideToMove == BLACK);

  chess960 = isChess960;
  thisThread = th;
  set_state(st);

  assert(pos_is_ok());

  return *this;
}


/// Position::set_castling_right() is a helper function used to set castling
/// rights given the corresponding color and the rook starting square.

void Position::set_castling_right(Color c, Square rfrom) {

  Square kfrom = square<KING>(c);
  CastlingSide cs = kfrom < rfrom ? KING_SIDE : QUEEN_SIDE;
  CastlingRight cr = (c | cs);

  st->castlingRights |= cr;
  castlingRightsMask[kfrom] |= cr;
  castlingRightsMask[rfrom] |= cr;
  castlingRookSquare[cr] = rfrom;

  Square kto = relative_square(c, cs == KING_SIDE ? SQ_G1 : SQ_C1);
  Square rto = relative_square(c, cs == KING_SIDE ? SQ_F1 : SQ_D1);

  for (Square s = std::min(rfrom, rto); s <= std::max(rfrom, rto); ++s)
      if (s != kfrom && s != rfrom)
          castlingPath[cr] |= s;

  for (Square s = std::min(kfrom, kto); s <= std::max(kfrom, kto); ++s)
      if (s != kfrom && s != rfrom)
          castlingPath[cr] |= s;
}


/// Position::set_check_info() sets king attacks to detect if a move gives check

void Position::set_check_info(StateInfo* si) const {

  si->blockersForKing[WHITE] = slider_blockers(pieces(BLACK), square<KING>(WHITE), si->pinners[BLACK]);
  si->blockersForKing[BLACK] = slider_blockers(pieces(WHITE), square<KING>(BLACK), si->pinners[WHITE]);

  Square ksq = square<KING>(~sideToMove);

  for (PieceType pt = PAWN; pt < KING; ++pt)
      si->checkSquares[pt] = attacks_from(~sideToMove, pt, ksq);
  si->checkSquares[KING]   = 0;
}


/// Position::set_state() computes the hash keys of the position, and other
/// data that once computed is updated incrementally as moves are made.
/// The function is only used when a new position is set up, and to verify
/// the correctness of the StateInfo data when running in debug mode.

void Position::set_state(StateInfo* si) const {

  si->key = si->materialKey = 0;
  si->pawnKey = Zobrist::noPawns;
  si->nonPawnMaterial[WHITE] = si->nonPawnMaterial[BLACK] = VALUE_ZERO;
  si->psq = SCORE_ZERO;
  si->checkersBB = attackers_to(square<KING>(sideToMove)) & pieces(~sideToMove);

  set_check_info(si);

  for (Bitboard b = pieces(); b; )
  {
      Square s = pop_lsb(&b);
      Piece pc = piece_on(s);
      si->key ^= Zobrist::psq[pc][s];
      si->psq += PSQT::psq[pc][s];
  }

  for (Gate g = GATE_1; g <= gate_count(); g++)
      si->key ^= Zobrist::inhand[gating_piece(g)][g];

  for (Color c = WHITE; c <= BLACK; ++c)
      for (Gate g = GATE_1; g <= setup_count(c); g++)
      {
          Square s = gating_square(c, g);
          if (s != SQ_NONE)
          {
              Piece pc = make_piece(c, gating_piece(g));
              si->key ^= Zobrist::psq_gate[pc][file_of(s)];
              si->psq += PSQT::psq_gate[pc][file_of(s)];
          }
      }

  if (si->epSquare != SQ_NONE)
      si->key ^= Zobrist::enpassant[file_of(si->epSquare)];

  if (sideToMove == BLACK)
      si->key ^= Zobrist::side;

  si->key ^= Zobrist::castling[si->castlingRights];

  for (Bitboard b = pieces(PAWN); b; )
  {
      Square s = pop_lsb(&b);
      si->pawnKey ^= Zobrist::psq[piece_on(s)][s];
  }

  for (Color c = WHITE; c <= BLACK; ++c)
      for (PieceType pt = PAWN; pt <= KING; ++pt)
      {
          Piece pc = make_piece(c, pt);
          if (pt != PAWN && pt != KING)
              si->nonPawnMaterial[c] += pieceCount[pc] * PieceValue[MG][pc];

          for (int cnt = 0; cnt < pieceCount[pc]; ++cnt)
              si->materialKey ^= Zobrist::psq[pc][cnt];
      }
}


/// Position::set() is an overload to initialize the position object with
/// the given endgame code string like "KBPKN". It is mainly a helper to
/// get the material key out of an endgame code.

Position& Position::set(const string& code, Color c, StateInfo* si) {

  assert(code.length() > 0 && code.length() < 8);
  assert(code[0] == 'K');

  string sides[] = { code.substr(code.find('K', 1)),      // Weak
                     code.substr(0, code.find('K', 1)) }; // Strong

  std::transform(sides[c].begin(), sides[c].end(), sides[c].begin(), tolower);

  string fenStr = "8/" + sides[0] + char(8 - sides[0].length() + '0') + "/8/8/8/8/"
                       + sides[1] + char(8 - sides[1].length() + '0') + "/8 w - - 0 10";

  return set(fenStr, false, si, nullptr);
}


/// Position::fen() returns a FEN representation of the position. In case of
/// Chess960 the Shredder-FEN notation is used. This is mainly a debugging function.

const string Position::fen() const {

  int emptyCnt;
  std::ostringstream ss;

  for (Rank r = RANK_8; r >= RANK_1; --r)
  {
      for (File f = FILE_A; f <= FILE_H; ++f)
      {
          for (emptyCnt = 0; f <= FILE_H && empty(make_square(f, r)); ++f)
              ++emptyCnt;

          if (emptyCnt)
              ss << emptyCnt;

          if (f <= FILE_H)
              ss << PieceToChar[piece_on(make_square(f, r))];
      }

      if (r > RANK_1)
          ss << '/';
  }

  // List all gating pieces in the format [LaCblgch]
  if (gateCount > NO_GATE)
  {
      ss << '[';
      for (Color c = WHITE; c <= BLACK; ++c)
          for (Gate i = GATE_1; i <= gateCount; i++)
              ss << std::string{PieceToChar[make_piece(c, gating_piece(i))],
                                  setupCount[c] < i              ? '?'
                                : gating_square(c, i) != SQ_NONE ? char('a' + file_of(gating_square(c, i)))
                                                                 : '-'};
      ss << ']';
  }

  ss << (sideToMove == WHITE ? " w " : " b ");

  if (can_castle(WHITE_OO))
      ss << (chess960 ? char('A' + file_of(castling_rook_square(WHITE |  KING_SIDE))) : 'K');

  if (can_castle(WHITE_OOO))
      ss << (chess960 ? char('A' + file_of(castling_rook_square(WHITE | QUEEN_SIDE))) : 'Q');

  if (can_castle(BLACK_OO))
      ss << (chess960 ? char('a' + file_of(castling_rook_square(BLACK |  KING_SIDE))) : 'k');

  if (can_castle(BLACK_OOO))
      ss << (chess960 ? char('a' + file_of(castling_rook_square(BLACK | QUEEN_SIDE))) : 'q');

  if (!can_castle(WHITE) && !can_castle(BLACK))
      ss << '-';

  ss << (ep_square() == SQ_NONE ? " - " : " " + UCI::square(ep_square()) + " ")
     << st->rule50 << " " << 1 + (gamePly - (sideToMove == BLACK)) / 2;

  return ss.str();
}


/// Position::slider_blockers() returns a bitboard of all the pieces (both colors)
/// that are blocking attacks on the square 's' from 'sliders'. A piece blocks a
/// slider if removing that piece from the board would result in a position where
/// square 's' is attacked. For example, a king-attack blocking piece can be either
/// a pinned or a discovered check piece, according if its color is the opposite
/// or the same of the color of the slider.

Bitboard Position::slider_blockers(Bitboard sliders, Square s, Bitboard& pinners) const {

  Bitboard blockers = 0;
  pinners = 0;

  // Snipers are sliders that attack 's' when a piece is removed
  Bitboard snipers =   sliders
                    &  attackers_to(s, 0)
                    & ~attackers_to(s);

  while (snipers)
  {
    Square sniperSq = pop_lsb(&snipers);
    Bitboard b = between_bb(s, sniperSq) & pieces();

    if (b && !more_than_one(b))
    {
        blockers |= b;
        if (b & pieces(color_of(piece_on(s))))
            pinners |= sniperSq;
    }
  }
  return blockers;
}


/// Position::attackers_to() computes a bitboard of all pieces which attack a
/// given square. Slider attacks use the occupied bitboard to indicate occupancy.

Bitboard Position::attackers_to(Square s, Bitboard occupied) const {

  Bitboard b = 0;
  for (Color c = WHITE; c <= BLACK; ++c)
      for (PieceType pt = PAWN; pt <= KING; ++pt)
          b |= attacks_bb(~c, pt, s, occupied) & pieces(c, pt);
  return b;
}


/// Position::legal() tests whether a pseudo-legal move is legal

bool Position::legal(Move m) const {

  assert(is_ok(m));

  // Pseudo-legal moves during setup phase are legal
  if (type_of(m) == SET_GATING_TYPE || type_of(m) == PUT_GATING_PIECE)
  {
      assert(gating_type(m) != NO_PIECE_TYPE);
      return true;
  }

  Color us = sideToMove;
  Square from = from_sq(m);
  Square to = to_sq(m);
  Square ksq = square<KING>(us);

  assert(color_of(moved_piece(m)) == us);
  assert(piece_on(square<KING>(us)) == make_piece(us, KING));

  // En passant captures are a tricky special case. Because they are rather
  // uncommon, we do it simply by testing whether the king is attacked after
  // the move is made.
  if (type_of(m) == ENPASSANT)
  {
      Square capsq = to - pawn_push(us);
      Bitboard occupied = (pieces() ^ from ^ capsq) | to;

      assert(to == ep_square());
      assert(moved_piece(m) == make_piece(us, PAWN));
      assert(piece_on(capsq) == make_piece(~us, PAWN));
      assert(piece_on(to) == NO_PIECE);

      return !(attackers_to(ksq, occupied) & pieces(~us) & occupied);
  }

  // If the moving piece is a king, check whether the destination
  // square is attacked by the opponent. Castling moves are checked
  // for legality during move generation.
  if (type_of(piece_on(from)) == KING)
      return type_of(m) == CASTLING || !(attackers_to(to_sq(m)) & pieces(~us));

  // A non-king move is legal if and only if it is not pinned or it
  // does not expose the king to attackers.
  return   !(blockers_for_king(us) & from)
        || !(attackers_to(ksq, (pieces() ^ from) | to) & pieces(~us) & ~SquareBB[to]);
}


/// Position::pseudo_legal() takes a random move and tests whether the move is
/// pseudo legal. It is used to validate moves from TT that can be corrupted
/// due to SMP concurrent access or hash position key aliasing.

bool Position::pseudo_legal(const Move m) const {

  Color us = sideToMove;
  Square from = from_sq(m);
  Square to = to_sq(m);
  Piece pc = moved_piece(m);

  // Use a slower but simpler function for uncommon cases
  if (type_of(m) != NORMAL)
      return MoveList<LEGAL>(*this).contains(m);

  // A normal move during setup phase can not be legal
  if (game_phase() != GAMEPHASE_PLAYING)
      return false;

  // Is not a promotion, so promotion piece must be empty
  if (promotion_type(m) != NO_PIECE_TYPE)
      return false;

  // If the 'from' square is not occupied by a piece belonging to the side to
  // move, the move is obviously not legal.
  if (pc == NO_PIECE || color_of(pc) != us)
      return false;

  // The destination square cannot be occupied by a friendly piece
  if (pieces(us) & to)
      return false;

  // Handle the special case of a pawn move
  if (type_of(pc) == PAWN)
  {
      // We have already handled promotion moves, so destination
      // cannot be on the 8th/1st rank.
      if (rank_of(to) == relative_rank(us, RANK_8))
          return false;

      if (   !(attacks_from<PAWN>(us, from) & pieces(~us) & to) // Not a capture
          && !((from + pawn_push(us) == to) && empty(to))       // Not a single push
          && !(   (from + 2 * pawn_push(us) == to)              // Not a double push
               && (rank_of(from) == relative_rank(us, RANK_2))
               && empty(to)
               && empty(to - pawn_push(us))))
          return false;
  }
  else if (!(attacks_from(us, type_of(pc), from) & to))
      return false;

  // Evasions generator already takes care to avoid some kind of illegal moves
  // and legal() relies on this. We therefore have to take care that the same
  // kind of moves are filtered out here.
  if (checkers())
  {
      if (type_of(pc) != KING)
      {
          // Double check? In this case a king move is required
          if (more_than_one(checkers()))
              return false;

          // Our move must be a blocking evasion or a capture of the checking piece
          Square checksq = lsb(checkers());
          if (  !((between_bb(checksq, square<KING>(us)) | checkers()) & to)
              || (LeaperAttacks[~us][type_of(piece_on(checksq))][checksq] & square<KING>(us)))
              return false;
      }
      // In case of king moves under check we have to remove king so as to catch
      // invalid moves like b1a1 when opposite queen is on c1.
      else if (attackers_to(to, pieces() ^ from) & pieces(~us))
          return false;
  }

  return true;
}


/// Position::gives_check() tests whether a pseudo-legal move gives a check

bool Position::gives_check(Move m) const {

  assert(is_ok(m));
  assert(color_of(moved_piece(m)) == sideToMove);

  // There are no checks during setup phase
  if (game_phase() != GAMEPHASE_PLAYING)
      return false;

  Square from = from_sq(m);
  Square to = to_sq(m);

  // Is there a direct check?
  if (st->checkSquares[type_of(piece_on(from))] & to)
      return true;

  // Is there a discovered check?
  // We have to consider here that a piece could leap over a slider.
  if (   (st->blockersForKing[~sideToMove] & from)
      && (  !aligned(from, to, square<KING>(~sideToMove))
          || (attackers_to(square<KING>(~sideToMove), pieces() ^ from ^ to) & pieces(sideToMove))))
      return true;

  // Is there a check by a gated piece?
  if ((gateBB & from) && (attacks_bb(sideToMove, gating_piece(from), from, pieces() ^ to) & square<KING>(~sideToMove)))
      return true;

  switch (type_of(m))
  {
  case NORMAL:
      return false;

  case PROMOTION:
      return attacks_bb(sideToMove, promotion_type(m), to, pieces() ^ from) & square<KING>(~sideToMove);

  // En passant capture with check? We have already handled the case
  // of direct checks and ordinary discovered check, so the only case we
  // need to handle is the unusual case of a discovered check through
  // the captured pawn.
  case ENPASSANT:
  {
      Square capsq = make_square(file_of(to), rank_of(from));
      Bitboard b = (pieces() ^ from ^ capsq) | to;

      return attackers_to(square<KING>(~sideToMove), b) & pieces(sideToMove) & b;
  }
  case CASTLING:
  {
      Square kfrom = from;
      Square rfrom = to; // Castling is encoded as 'King captures the rook'
      Square kto = relative_square(sideToMove, rfrom > kfrom ? SQ_G1 : SQ_C1);
      Square rto = relative_square(sideToMove, rfrom > kfrom ? SQ_F1 : SQ_D1);

      // Is there a check by a gated piece on rook square?
      if (   (gateBB & rfrom)
          && (  attacks_bb(sideToMove, gating_piece(rfrom), rfrom, pieces() ^ kfrom ^ kto ^ rto)
              & square<KING>(~sideToMove)))
          return true;
      return   (PseudoAttacks[sideToMove][ROOK][rto] & square<KING>(~sideToMove))
            && (attacks_bb<ROOK>(rto, (pieces() ^ kfrom ^ rfrom) | rto | kto) & square<KING>(~sideToMove));
  }
  default:
      assert(false);
      return false;
  }
}


/// Position::do_move() makes a move, and saves all information necessary
/// to a StateInfo object. The move is assumed to be legal. Pseudo-legal
/// moves should be filtered out before this function is called.

void Position::do_move(Move m, StateInfo& newSt, bool givesCheck) {

  assert(is_ok(m));
  assert(&newSt != st);

  thisThread->nodes.fetch_add(1, std::memory_order_relaxed);
  Key k = st->key ^ Zobrist::side;

  // Copy some fields of the old state to our new StateInfo object except the
  // ones which are going to be recalculated from scratch anyway and then switch
  // our state pointer to point to the new (ready to be updated) state.
  std::memcpy(&newSt, st, offsetof(StateInfo, key));
  newSt.previous = st;
  st = &newSt;

  Color us = sideToMove;
  Color them = ~us;
  switch (type_of(m))
  {
  case SET_GATING_TYPE:
      if (gateCount == NO_GATE || gating_type(m) != gatingPieces[gateCount])
      {
          set_gating_type(gating_type(m));
          st->removedGatingType = NO_PIECE_TYPE;
          k ^= Zobrist::inhand[gating_type(m)][gateCount];
      }
      else
      {
          k ^=  Zobrist::inhand[gating_type(m)][gateCount]
              ^ Zobrist::inhand[CANNON][gateCount] ^ Zobrist::inhand[LEOPARD][gateCount + 1];
          unset_gating_type();
          set_gating_type(CANNON);
          set_gating_type(LEOPARD);
          st->removedGatingType = gating_type(m);
      }
      break;
  case PUT_GATING_PIECE:
      assert(gating_type(m) == gatingPieces[setupCount[us] + 1]);
      put_gating_piece(us, to_sq(m));
      st->psq += PSQT::psq_gate[make_piece(us, gating_type(m))][file_of(to_sq(m))];
      k ^= Zobrist::psq_gate[make_piece(us, gating_type(m))][file_of(to_sq(m))];
      break;
  default:
      // Increment ply counters. In particular, rule50 will be reset to zero later on
      // in case of a capture or a pawn move.
      ++gamePly;
      ++st->rule50;
      ++st->pliesFromNull;

      Square from = from_sq(m);
      Square to = to_sq(m);
      Piece pc = piece_on(from);
      Piece captured = type_of(m) == ENPASSANT ? make_piece(them, PAWN) : piece_on(to);

      assert(color_of(pc) == us);
      assert(captured == NO_PIECE || color_of(captured) == (type_of(m) != CASTLING ? them : us));
      assert(type_of(captured) != KING);

      if (type_of(m) == CASTLING)
      {
          assert(pc == make_piece(us, KING));
          assert(captured == make_piece(us, ROOK));

          Square rfrom, rto;
          do_castling<true>(us, from, to, rfrom, rto, k);

          st->psq += PSQT::psq[captured][rto] - PSQT::psq[captured][rfrom];
          k ^= Zobrist::psq[captured][rfrom] ^ Zobrist::psq[captured][rto];
          captured = NO_PIECE;
      }

      if (captured)
      {
          Square capsq = to;

          // If the captured piece is a pawn, update pawn hash key, otherwise
          // update non-pawn material.
          if (type_of(captured) == PAWN)
          {
              if (type_of(m) == ENPASSANT)
              {
                  capsq -= pawn_push(us);

                  assert(pc == make_piece(us, PAWN));
                  assert(to == st->epSquare);
                  assert(relative_rank(us, to) == RANK_6);
                  assert(piece_on(to) == NO_PIECE);
                  assert(piece_on(capsq) == make_piece(them, PAWN));

                  board[capsq] = NO_PIECE; // Not done by remove_piece()
              }

              st->pawnKey ^= Zobrist::psq[captured][capsq];
          }
          else
              st->nonPawnMaterial[them] -= PieceValue[MG][captured];

          // Update board and piece lists
          remove_piece(captured, capsq);
          if (gateBB & capsq)
          {
              st->psq -= PSQT::psq_gate[make_piece(~us, gating_piece(capsq))][file_of(capsq)];
              k ^= Zobrist::psq_gate[make_piece(~us, gating_piece(capsq))][file_of(capsq)];
              capture_gate(them, capsq);
          }
          else
              st->capturedGate = NO_GATE;

          // Update material hash key and prefetch access to materialTable
          k ^= Zobrist::psq[captured][capsq];
          st->materialKey ^= Zobrist::psq[captured][pieceCount[captured]];
          prefetch(thisThread->materialTable[st->materialKey]);

          // Update incremental scores
          st->psq -= PSQT::psq[captured][capsq];

          // Reset rule 50 counter
          st->rule50 = 0;
      }

      // Update hash key
      k ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];

      // Reset en passant square
      if (st->epSquare != SQ_NONE)
      {
          k ^= Zobrist::enpassant[file_of(st->epSquare)];
          st->epSquare = SQ_NONE;
      }

      // Update castling rights if needed
      if (st->castlingRights && (castlingRightsMask[from] | castlingRightsMask[to]))
      {
          int cr = castlingRightsMask[from] | castlingRightsMask[to];
          k ^= Zobrist::castling[st->castlingRights & cr];
          st->castlingRights &= ~cr;
      }

      // Move the piece. The tricky Chess960 castling is handled earlier
      if (type_of(m) != CASTLING)
      {
          move_piece(pc, from, to);
          if (gateBB & from)
          {
              Piece gated_piece = make_piece(us, gating_piece(from));
              st->psq += PSQT::psq[gated_piece][from] - PSQT::psq_gate[gated_piece][file_of(from)];
              k ^= Zobrist::psq[gated_piece][from] ^ Zobrist::psq_gate[gated_piece][file_of(from)];
              gate_piece(us, from);
          }
      }

      // If the moving piece is a pawn do some special extra work
      if (type_of(pc) == PAWN)
      {
          // Set en-passant square if the moved pawn can be captured
          if (   (int(to) ^ int(from)) == 16
              && (attacks_from<PAWN>(us, to - pawn_push(us)) & pieces(them, PAWN)))
          {
              st->epSquare = to - pawn_push(us);
              k ^= Zobrist::enpassant[file_of(st->epSquare)];
          }

          else if (type_of(m) == PROMOTION)
          {
              Piece promotion = make_piece(us, promotion_type(m));

              assert(relative_rank(us, to) == RANK_8);
              assert(type_of(promotion) >= KNIGHT && type_of(promotion) < KING);

              remove_piece(pc, to);
              put_piece(promotion, to);

              // Update hash keys
              k ^= Zobrist::psq[pc][to] ^ Zobrist::psq[promotion][to];
              st->pawnKey ^= Zobrist::psq[pc][to];
              st->materialKey ^=  Zobrist::psq[promotion][pieceCount[promotion]-1]
                                  ^ Zobrist::psq[pc][pieceCount[pc]];

              // Update incremental score
              st->psq += PSQT::psq[promotion][to] - PSQT::psq[pc][to];

              // Update material
              st->nonPawnMaterial[us] += PieceValue[MG][promotion];
          }

          // Update pawn hash key and prefetch access to pawnsTable
          st->pawnKey ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];
          prefetch2(thisThread->pawnsTable[st->pawnKey]);

          // Reset rule 50 draw counter
          st->rule50 = 0;
      }

      // Update incremental scores
      st->psq += PSQT::psq[pc][to] - PSQT::psq[pc][from];

      // Set capture piece
      st->capturedPiece = captured;
  }

  // Calculate checkers bitboard (if move gives check)
  st->checkersBB = givesCheck ? attackers_to(square<KING>(them)) & pieces(us) : 0;

  // Update the key with the final value
  st->key = k;

  sideToMove = ~sideToMove;

  // Update king attacks used for fast check detection
  set_check_info(st);

  assert(pos_is_ok());
}


/// Position::undo_move() unmakes a move. When it returns, the position should
/// be restored to exactly the same state as before the move was made.

void Position::undo_move(Move m) {

  assert(is_ok(m));

  sideToMove = ~sideToMove;

  Color us = sideToMove;
  switch (type_of(m))
  {
  case SET_GATING_TYPE:
      unset_gating_type();
      if (st->removedGatingType)
      {
          unset_gating_type();
          set_gating_type(st->removedGatingType);
      }
      break;
  case PUT_GATING_PIECE:
      assert(gating_type(m) == gatingPieces[setupCount[us]]);
      remove_gating_piece(us, to_sq(m));
      break;
  default:
      Square from = from_sq(m);
      Square to = to_sq(m);
      Piece pc = piece_on(to);

      assert(empty(from) || type_of(m) == CASTLING || color_of(pc) == us);
      assert(type_of(st->capturedPiece) != KING);

      if (type_of(m) == PROMOTION)
      {
          assert(relative_rank(us, to) == RANK_8);
          assert(type_of(pc) == promotion_type(m));
          assert(type_of(pc) >= KNIGHT && type_of(pc) < KING);

          remove_piece(pc, to);
          pc = make_piece(us, PAWN);
          put_piece(pc, to);
      }

      if (type_of(m) == CASTLING)
      {
          Square rfrom, rto;
          do_castling<false>(us, from, to, rfrom, rto, st->key);
      }
      else
      {
          if (pieces() & from)
              ungate_piece(us, from);
          move_piece(pc, to, from); // Put the piece back at the source square

          if (st->capturedPiece)
          {
              Square capsq = to;

              if (type_of(m) == ENPASSANT)
              {
                  capsq -= pawn_push(us);

                  assert(type_of(pc) == PAWN);
                  assert(to == st->previous->epSquare);
                  assert(relative_rank(us, to) == RANK_6);
                  assert(piece_on(capsq) == NO_PIECE);
                  assert(st->capturedPiece == make_piece(~us, PAWN));
              }

              put_piece(st->capturedPiece, capsq); // Restore the captured piece
              if (st->capturedGate > NO_GATE)
                  uncapture_gate(~us, capsq);
          }
      }

      --gamePly;
  }

  // Finally point our state pointer back to the previous state
  st = st->previous;

  assert(pos_is_ok());
}


/// Position::do_castling() is a helper used to do/undo a castling move. This
/// is a bit tricky in Chess960 where from/to squares can overlap.
template<bool Do>
void Position::do_castling(Color us, Square from, Square& to, Square& rfrom, Square& rto, Key& k) {

  bool kingSide = to > from;
  rfrom = to; // Castling is encoded as "king captures friendly rook"
  rto = relative_square(us, kingSide ? SQ_F1 : SQ_D1);
  to = relative_square(us, kingSide ? SQ_G1 : SQ_C1);

  // Ungate piece
  if (!Do && (pieces() & (SquareBB[from] | rfrom)))
  {
      Square s = pieces() & from ? from : rfrom;
      if (s != to && s != rto)
          ungate_piece(us, s);
      else if (st->capturedGate > NO_GATE)
          uncapture_gate(us, s);
  }

  // Remove both pieces first since squares could overlap in Chess960
  remove_piece(make_piece(us, KING), Do ? from : to);
  remove_piece(make_piece(us, ROOK), Do ? rfrom : rto);
  board[Do ? from : to] = board[Do ? rfrom : rto] = NO_PIECE; // Since remove_piece doesn't do it for us
  put_piece(make_piece(us, KING), Do ? to : from);
  put_piece(make_piece(us, ROOK), Do ? rto : rfrom);

  // Gate piece
  if (Do && (gateBB & (SquareBB[from] | rfrom)))
  {
    Square s = gateBB & from ? from : rfrom;
    // In Chess960 only gate if square will be free, else remove gate
    if (s != to && s != rto)
    {
        Piece gated_piece = make_piece(us, gating_piece(s));
        st->psq += PSQT::psq[gated_piece][s] - PSQT::psq_gate[gated_piece][file_of(s)];
        k ^= Zobrist::psq[gated_piece][s] ^ Zobrist::psq_gate[gated_piece][file_of(s)];
        gate_piece(us, s);
    }
    else
    {
        Piece gated_piece = make_piece(us, gating_piece(s));
        st->psq -= PSQT::psq_gate[gated_piece][file_of(s)];
        k ^= Zobrist::psq_gate[gated_piece][file_of(s)];
        capture_gate(us, s);
    }
  }
  else
      st->capturedGate = NO_GATE;
}


/// Position::do(undo)_null_move() is used to do(undo) a "null move": It flips
/// the side to move without executing any move on the board.

void Position::do_null_move(StateInfo& newSt) {

  assert(!checkers());
  assert(&newSt != st);

  std::memcpy(&newSt, st, sizeof(StateInfo));
  newSt.previous = st;
  st = &newSt;

  if (st->epSquare != SQ_NONE)
  {
      st->key ^= Zobrist::enpassant[file_of(st->epSquare)];
      st->epSquare = SQ_NONE;
  }

  st->key ^= Zobrist::side;
  prefetch(TT.first_entry(st->key));

  ++st->rule50;
  st->pliesFromNull = 0;

  sideToMove = ~sideToMove;

  set_check_info(st);

  assert(pos_is_ok());
}

void Position::undo_null_move() {

  assert(!checkers());

  st = st->previous;
  sideToMove = ~sideToMove;
}


/// Position::key_after() computes the new hash key after the given move. Needed
/// for speculative prefetch. It doesn't recognize special moves like castling,
/// en-passant and promotions.

Key Position::key_after(Move m) const {

  Square from = from_sq(m);
  Square to = to_sq(m);
  Piece pc = piece_on(from);
  Piece captured = piece_on(to);
  Key k = st->key ^ Zobrist::side;

  if (captured)
      k ^= Zobrist::psq[captured][to];

  return k ^ Zobrist::psq[pc][to] ^ Zobrist::psq[pc][from];
}


/// Position::see_ge (Static Exchange Evaluation Greater or Equal) tests if the
/// SEE value of move is greater or equal to the given threshold. We'll use an
/// algorithm similar to alpha-beta pruning with a null window.

bool Position::see_ge(Move m, Value threshold) const {

  assert(is_ok(m));

  // Only deal with normal moves, assume others pass a simple see
  if (type_of(m) != NORMAL)
      return VALUE_ZERO >= threshold;

  Bitboard stmAttackers;
  Square from = from_sq(m), to = to_sq(m);
  PieceType nextVictim = type_of(piece_on(from));
  Color us = color_of(piece_on(from));
  Color stm = ~us; // First consider opponent's move
  Value balance;   // Values of the pieces taken by us minus opponent's ones

  // The opponent may be able to recapture so this is the best result
  // we can hope for.
  balance = PieceValue[MG][piece_on(to)] - threshold;

  // Consider captured gates
  if (gateBB & to)
      balance += PieceValue[MG][gating_piece(to)];

  if (balance < VALUE_ZERO)
      return false;

  // Now assume the worst possible result: that the opponent can
  // capture our piece for free.
  balance -= PieceValue[MG][nextVictim];

  // If it is enough (like in PxQ) then return immediately. Note that
  // in case nextVictim == KING we always return here, this is ok
  // if the given move is legal.
  if (balance >= VALUE_ZERO)
      return true;

  // Find all attackers to the destination square, with the moving piece
  // removed, but possibly an X-ray attacker added behind it.
  Bitboard occupied = pieces() ^ from ^ to;
  Bitboard attackers = attackers_to(to, occupied) & occupied;

  while (true)
  {
      stmAttackers = attackers & pieces(stm);

      // Don't allow pinned pieces to attack (except the king) as long as
      // all pinners are on their original square.
      if (!(st->pinners[~stm] & ~occupied))
          stmAttackers &= ~st->blockersForKing[stm];

      // If stm has no more attackers then give up: stm loses
      if (!stmAttackers)
          break;

      // Locate and remove the next least valuable attacker, and add to
      // the bitboard 'attackers' the possibly X-ray attackers behind it.
      nextVictim = min_attacker<PAWN>(byTypeBB, to, stmAttackers, occupied, attackers);

      stm = ~stm; // Switch side to move

      // Negamax the balance with alpha = balance, beta = balance+1 and
      // add nextVictim's value.
      //
      //      (balance, balance+1) -> (-balance-1, -balance)
      //
      assert(balance < VALUE_ZERO);

      balance = -balance - 1 - PieceValue[MG][nextVictim];

      // If balance is still non-negative after giving away nextVictim then we
      // win. The only thing to be careful about it is that we should revert
      // stm if we captured with the king when the opponent still has attackers.
      if (balance >= VALUE_ZERO)
      {
          if (nextVictim == KING && (attackers & pieces(stm)))
              stm = ~stm;
          break;
      }
      assert(nextVictim != KING);
  }
  return us != stm; // We break the above loop when stm loses
}


/// Position::is_draw() tests whether the position is drawn by 50-move rule
/// or by repetition. It does not detect stalemates.

bool Position::is_draw(int ply) const {

  if (st->rule50 > 99 && (!checkers() || MoveList<LEGAL>(*this).size()))
      return true;

  int end = std::min(st->rule50, st->pliesFromNull);

  if (end < 4)
    return false;

  StateInfo* stp = st->previous->previous;
  int cnt = 0;

  for (int i = 4; i <= end; i += 2)
  {
      stp = stp->previous->previous;

      // Return a draw score if a position repeats once earlier but strictly
      // after the root, or repeats twice before or at the root.
      if (   stp->key == st->key
          && ++cnt + (ply > i) == 2)
          return true;
  }

  return false;
}


// Position::has_repeated() tests whether there has been at least one repetition
// of positions since the last capture or pawn move.

bool Position::has_repeated() const {

    StateInfo* stc = st;
    while (true)
    {
        int i = 4, end = std::min(stc->rule50, stc->pliesFromNull);

        if (end < i)
            return false;

        StateInfo* stp = stc->previous->previous;

        do {
            stp = stp->previous->previous;

            if (stp->key == stc->key)
                return true;

            i += 2;
        } while (i <= end);

        stc = stc->previous;
    }
}


/// Position::has_game_cycle() tests if the position has a move which draws by repetition,
/// or an earlier position has a move that directly reaches the current position.

bool Position::has_game_cycle(int ply) const {

  int j;

  int end = std::min(st->rule50, st->pliesFromNull);

  if (end < 3)
    return false;

  Key originalKey = st->key;
  StateInfo* stp = st->previous;

  for (int i = 3; i <= end; i += 2)
  {
      stp = stp->previous->previous;

      Key moveKey = originalKey ^ stp->key;
      if (   (j = H1(moveKey), cuckoo[j] == moveKey)
          || (j = H2(moveKey), cuckoo[j] == moveKey))
      {
          Move move = cuckooMove[j];
          Square s1 = from_sq(move);
          Square s2 = to_sq(move);

          if (!(between_bb(s1, s2) & pieces()))
          {
              // In the cuckoo table, both moves Rc1c5 and Rc5c1 are stored in the same
              // location. We select the legal one by reversing the move variable if necessary.
              if (empty(s1))
                  move = make_move(s2, s1);

              if (ply > i)
                  return true;

              // For repetitions before or at the root, require one more
              StateInfo* next_stp = stp;
              for (int k = i + 2; k <= end; k += 2)
              {
                  next_stp = next_stp->previous->previous;
                  if (next_stp->key == stp->key)
                     return true;
              }
          }
      }
  }
  return false;
}


/// Position::flip() flips position with the white and black sides reversed. This
/// is only useful for debugging e.g. for finding evaluation symmetry bugs.

void Position::flip() {

  string f, token;
  std::stringstream ss(fen());

  for (Rank r = RANK_8; r >= RANK_1; --r) // Piece placement
  {
      std::getline(ss, token, r > RANK_1 ? '/' : ' ');
      f.insert(0, token + (f.empty() ? " " : "/"));
  }

  ss >> token; // Active color
  f += (token == "w" ? "B " : "W "); // Will be lowercased later

  ss >> token; // Castling availability
  f += token + " ";

  std::transform(f.begin(), f.end(), f.begin(),
                 [](char c) { return char(islower(c) ? toupper(c) : tolower(c)); });

  ss >> token; // En passant square
  f += (token == "-" ? token : token.replace(1, 1, token[1] == '3' ? "6" : "3"));

  std::getline(ss, token); // Half and full moves
  f += token;

  set(f, is_chess960(), st, this_thread());

  assert(pos_is_ok());
}


/// Position::pos_is_ok() performs some consistency checks for the
/// position object and raises an asserts if something wrong is detected.
/// This is meant to be helpful when debugging.

bool Position::pos_is_ok() const {

  constexpr bool Fast = true; // Quick (default) or full check?

  if (   (sideToMove != WHITE && sideToMove != BLACK)
      || piece_on(square<KING>(WHITE)) != make_piece(WHITE, KING)
      || piece_on(square<KING>(BLACK)) != make_piece(BLACK, KING)
      || (   ep_square() != SQ_NONE
          && relative_rank(sideToMove, ep_square()) != RANK_6))
      assert(0 && "pos_is_ok: Default");

  if (Fast)
      return true;

  if (   pieceCount[make_piece(WHITE, KING)] != 1
      || pieceCount[make_piece(BLACK, KING)] != 1
      || attackers_to(square<KING>(~sideToMove)) & pieces(sideToMove))
      assert(0 && "pos_is_ok: Kings");

  if (   (pieces(PAWN) & (Rank1BB | Rank8BB))
      || pieceCount[make_piece(WHITE, PAWN)] > 8
      || pieceCount[make_piece(BLACK, PAWN)] > 8)
      assert(0 && "pos_is_ok: Pawns");

  if (   (pieces(WHITE) & pieces(BLACK))
      || (pieces(WHITE) | pieces(BLACK)) != pieces()
      || popcount(pieces(WHITE)) > 16
      || popcount(pieces(BLACK)) > 16)
      assert(0 && "pos_is_ok: Bitboards");

  for (PieceType p1 = PAWN; p1 <= KING; ++p1)
      for (PieceType p2 = PAWN; p2 <= KING; ++p2)
          if (p1 != p2 && (pieces(p1) & pieces(p2)))
              assert(0 && "pos_is_ok: Bitboards");

  StateInfo si = *st;
  set_state(&si);
  if (std::memcmp(&si, st, sizeof(StateInfo)))
      assert(0 && "pos_is_ok: State");

  for (Color c = WHITE; c <= BLACK; ++c)
      for (PieceType pt = PAWN; pt <= KING; ++pt)
      {
          Piece pc = make_piece(c, pt);
          if (   pieceCount[pc] != popcount(pieces(c, pt))
              || pieceCount[pc] != std::count(board, board + SQUARE_NB, pc))
              assert(0 && "pos_is_ok: Pieces");

          for (int i = 0; i < pieceCount[pc]; ++i)
              if (board[pieceList[pc][i]] != pc || index[pieceList[pc][i]] != i)
                  assert(0 && "pos_is_ok: Index");
      }

  for (Color c = WHITE; c <= BLACK; ++c)
      for (CastlingSide s = KING_SIDE; s <= QUEEN_SIDE; s = CastlingSide(s + 1))
      {
          if (!can_castle(c | s))
              continue;

          if (   piece_on(castlingRookSquare[c | s]) != make_piece(c, ROOK)
              || castlingRightsMask[castlingRookSquare[c | s]] != (c | s)
              || (castlingRightsMask[square<KING>(c)] & (c | s)) != (c | s))
              assert(0 && "pos_is_ok: Castling");
      }

  return true;
}
