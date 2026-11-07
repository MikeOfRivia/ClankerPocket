#!/usr/bin/env python3
"""Patch the vendored mcu-max FEN loader for normal pawn double steps.

Upstream mcumax_set_piece() tags every FEN-imported piece as already moved.
Chess Station uses custom variants where pawns on their original ranks
must still be allowed their ordinary two-square initial move. Other
imported pieces retain the upstream moved flag (no accidental castling).
Only the fresh generated engine copy is modified, not upstream.
"""
import pathlib
import sys

source = pathlib.Path(sys.argv[1])
code = source.read_text(encoding="utf-8")
original = "    mcumax.board[square] = piece ? (piece | MCUMAX_PIECE_MOVED) : piece;"
replacement = """    // A FEN pawns-only layout still has virgin pawns on ranks 2 and 7.
    // Leave exactly those pawns unmoved so the normal two-step rule works.
    // Back-rank pawns remain flagged as moved, and castling stays disabled.
    const bool virgin_pawn =
        (((square >> 4) == 6) &&
         (piece == (MCUMAX_PAWN_UPSTREAM | MCUMAX_BOARD_WHITE))) ||
        (((square >> 4) == 1) &&
         (piece == (MCUMAX_PAWN_DOWNSTREAM | MCUMAX_BOARD_BLACK)));
    mcumax.board[square] = piece
        ? (virgin_pawn ? piece : (piece | MCUMAX_PIECE_MOVED))
        : piece;"""
if code.count(original) != 1:
    raise RuntimeError("mcu-max FEN loader changed; refusing unsafe patch")
source.write_text(code.replace(original, replacement), encoding="utf-8")
print("Patched FEN import: home-rank pawns retain two-square opening.")
