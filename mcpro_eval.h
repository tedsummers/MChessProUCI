#ifndef MCPRO_EVAL_H
#define MCPRO_EVAL_H

#include <stdint.h>

/*
 * Native C implementation of MChess Pro v3.5's piece-mobility / PST /
 * king-safety / outpost evaluation functions:
 *   - mcpro_eval_white_pieces() <-> 0fcb:c98c
 *   - mcpro_eval_black_pieces() <-> 0fcb:d048
 *
 * Operates directly on MChess Pro's DGROUP memory layout so every move,
 * score, and node count remains 100% bit-exact with the original 80186 code.
 */

static inline uint16_t mcpro_eval_black_pieces(const uint8_t *dg) {
    if (dg[0xcc09] == 0) return 0;

    const uint8_t *board    = dg + 0xd574;
    const uint8_t *wp_atk   = dg + 0xdad6; /* -0x252a */
    const uint8_t *bp_atk1  = dg + 0x2dee;
    const uint8_t *bp_atk2  = dg + 0x2df8;
    const uint8_t *kzone    = dg + 0xd71c; /* -0x28e4 */
    const uint8_t *plist_ix = dg + 0xd90a; /* -0x26f6 */
    const uint8_t *fmask    = dg + 0x019a;

    int32_t score = 0;

    for (int si = 6; si >= 0; si--) {
        int di = dg[0xd6c8 + si];
        uint8_t pc = board[di];
        if (pc <= 8) continue;

        if (pc == 9) { /* Black Knight */
            const uint8_t *pst = dg + 0xdf6c;
            score += pst[di];

            static const int8_t fwd_d[4] = { 8, 4, 7, 2 };
            for (int k = 0; k < 4; k++) {
                di += fwd_d[k];
                uint8_t sq = board[di];
                if (sq < 7 && (sq != 0 || wp_atk[di] == 0)) {
                    score += pst[di];
                }
            }
            di -= 0x2a;
            static const int8_t bwd_d[4] = { 0, 2, 7, 4 };
            for (int k = 0; k < 4; k++) {
                di += bwd_d[k];
                uint8_t sq = board[di];
                if (sq != 7 && sq != 8 && (sq != 0 || wp_atk[di] == 0)) {
                    score += pst[di];
                }
            }
        } else if (pc == 10) { /* Black Bishop */
            const uint8_t *pst = dg + 0xd9f6;
            int cx = di;

            /* +9 ray */
            while (1) {
                score += pst[di];
                di += 9;
                uint8_t sq = board[di];
                if (sq != 0) {
                    if (sq < 7) {
                        score += pst[di];
                        if (board[di] < 2) {
                            if (wp_atk[di] == 0) score += 0x80;
                        } else if (board[di] == 2) {
                            if (wp_atk[di] == 0) score += 0x60;
                            while (1) {
                                di += 9;
                                uint8_t sq2 = board[di];
                                if (sq2 == 0) {
                                    score += pst[di];
                                    continue;
                                }
                                if (sq2 < 7 && sq2 >= 4) {
                                    score += 0x80 + pst[di];
                                    continue;
                                }
                                break;
                            }
                        }
                    }
                    break;
                }
            }

            /* -9 ray */
            di = cx;
            while (1) {
                di -= 9;
                uint8_t sq = board[di];
                if (sq == 0) {
                    score += pst[di];
                    continue;
                }
                if (sq == 2) {
                    if (wp_atk[di] == 0) {
                        score -= 0x20;
                        score += 0x80;
                    }
                    while (1) {
                        score += pst[di];
                        di -= 9;
                        uint8_t sq2 = board[di];
                        if (sq2 == 0) continue;
                        if (sq2 < 7 && sq2 >= 4) {
                            score += 0x80;
                            continue;
                        }
                        break;
                    }
                    break;
                } else if (sq < 2) {
                    int8_t d = (int8_t)wp_atk[di] - (int8_t)bp_atk1[di];
                    if (d < 1) {
                        if (kzone[di] == 1) score += 0x60;
                        score += pst[di];
                    } else if (d > 1) {
                        score -= 0x48;
                    }
                    break;
                } else if (sq <= 7) {
                    if (sq != 7) score += pst[di];
                    break;
                } else if (sq < 9) {
                    if (di <= 0x30) {
                        if (di <= 0x26) {
                            if (kzone[di] == 1) score += 0x60;
                            score += pst[di];
                        } else if (board[di - 10] != 1) {
                            score += pst[di];
                        }
                    } else if (di > 0x44 && board[di - 10] != 0) {
                        score -= 0x80;
                    }
                    break;
                } else if (sq == 12) {
                    score += pst[di];
                    continue;
                } else if (sq > 12) {
                    score -= 0x48;
                    break;
                } else {
                    if (kzone[di] == 1) score += 0x60;
                    score += pst[di];
                    break;
                }
            }

            /* +11 ray */
            di = cx;
            while (1) {
                di += 11;
                uint8_t sq = board[di];
                if (sq == 0) {
                    score += pst[di];
                    continue;
                }
                if (sq < 7) {
                    score += pst[di];
                    if (board[di] < 2) {
                        if (wp_atk[di] == 0) score += 0x80;
                    } else if (board[di] == 2) {
                        if (wp_atk[di] == 0) score += 0x60;
                        while (1) {
                            di += 11;
                            uint8_t sq2 = board[di];
                            if (sq2 == 0) {
                                score += pst[di];
                                continue;
                            }
                            if (sq2 < 7 && sq2 >= 4) {
                                score += 0x80 + pst[di];
                                continue;
                            }
                            break;
                        }
                    }
                }
                break;
            }

            /* -11 ray */
            di = cx;
            while (1) {
                di -= 11;
                uint8_t sq = board[di];
                if (sq == 0) {
                    score += pst[di];
                    continue;
                }
                if (sq == 2) {
                    if (wp_atk[di] == 0) {
                        score -= 0x20;
                        score += 0x80;
                    }
                    while (1) {
                        score += pst[di];
                        di -= 11;
                        uint8_t sq2 = board[di];
                        if (sq2 == 0) continue;
                        if (sq2 < 7 && sq2 >= 4) {
                            score += 0x80;
                            continue;
                        }
                        break;
                    }
                    break;
                } else if (sq < 2) {
                    int8_t d = (int8_t)wp_atk[di] - (int8_t)bp_atk1[di];
                    if (d < 1) {
                        if (kzone[di] == 2) score += 0x60;
                        score += pst[di];
                    } else if (d > 1) {
                        score -= 0x48;
                    }
                    break;
                } else if (sq <= 7) {
                    if (sq == 7) goto next_piece_b;
                    score += pst[di];
                    goto next_piece_b;
                } else if (sq < 9) {
                    if (di <= 0x30) {
                        if (di <= 0x26) {
                            if (kzone[di] == 2) score += 0x60;
                            score += pst[di];
                        } else if (board[di - 10] != 1) {
                            score += pst[di];
                        }
                    } else if (di > 0x44 && board[di - 10] != 0) {
                        score -= 0x80;
                    }
                    break;
                } else if (sq == 12) {
                    score += pst[di];
                    continue;
                } else if (sq > 12) {
                    score -= 0x48;
                    break;
                } else {
                    if (kzone[di] == 2) score += 0x60;
                    score += pst[di];
                    break;
                }
            }
        } else if (pc == 11) { /* Black Rook */
            const uint8_t *pst = dg + 0xe566;
            int cx = di;

            /* +1 and -1 rays */
            for (int dir = 0; dir < 2; dir++) {
                int step = (dir == 0) ? 1 : -1;
                di = cx;
                while (1) {
                    di += step;
                    uint8_t sq = board[di];
                    if (sq == 0) {
                        score += pst[di];
                        continue;
                    }
                    if (sq == 11) {
                        score += 0x10 + pst[di];
                        continue;
                    }
                    if (sq == 12) {
                        score += pst[di];
                        continue;
                    }
                    if (sq >= 7) break;
                    if (sq <= 1) {
                        if (wp_atk[di] == 0) {
                            score += 0x30 + pst[di];
                        }
                        break;
                    }
                    if (sq > 3) {
                        score += pst[di];
                        break;
                    }
                    if (bp_atk1[di] == 0 && bp_atk2[di] == 0) {
                        if (wp_atk[di] == 1) {
                            score += pst[di];
                            break;
                        }
                        if (wp_atk[di] > 1) break;
                    }
                    score += 0x30;
                    while (1) {
                        score += pst[di];
                        di += step;
                        uint8_t sq2 = board[di];
                        if (sq2 == 0) continue;
                        if (sq2 >= 2 && sq2 <= 6) {
                            if (sq2 < 5 && bp_atk1[di] == 0 && wp_atk[di] != 0) break;
                            score += 0xc8;
                        }
                        break;
                    }
                    break;
                }
            }

            /* +10 ray */
            di = cx;
            while (1) {
                di += 10;
                uint8_t sq = board[di];
                if (sq == 0) {
                    score += pst[di];
                    continue;
                }
                if (sq == 7) break;
                if (sq > 7) {
                    if (sq == 11 || sq == 12) {
                        score += 0x20 + pst[di];
                        continue;
                    }
                    break;
                }
                if (sq <= 1) {
                    uint8_t bl = plist_ix[di];
                    if (dg[0xdc77] & fmask[bl]) {
                        score += 0x60;
                        if (wp_atk[di] == 0) {
                            score += 0x40;
                            if (di >= 0x3d) score += 0xa0;
                            score += pst[di];
                        }
                    } else if (wp_atk[di] == 0) {
                        score += 0x50 + pst[di];
                    }
                    break;
                }
                if (sq > 3) {
                    score += pst[di];
                    break;
                }
                if (bp_atk1[di] == 0 && bp_atk2[di] == 0) {
                    if (wp_atk[di] == 1) {
                        score += pst[di];
                        break;
                    }
                    if (wp_atk[di] > 1) break;
                }
                score += 0x30;
                while (1) {
                    score += pst[di];
                    di += 10;
                    uint8_t sq2 = board[di];
                    if (sq2 == 0) continue;
                    if (sq2 >= 2 && sq2 <= 6) {
                        if (sq2 < 5 && bp_atk1[di] == 0 && wp_atk[di] != 0) break;
                        score += 0xc8;
                    }
                    break;
                }
                break;
            }

            /* -10 ray */
            di = cx;
            while (1) {
                di -= 10;
                uint8_t sq = board[di];
                if (sq == 0) {
                    score += pst[di];
                    continue;
                }
                if (sq == 7 || sq == 13) break;
                if (sq < 7) {
                    if (sq <= 1) {
                        if (wp_atk[di] == 0) score += 0x60 + pst[di];
                        break;
                    }
                    if (sq > 3) {
                        score += pst[di];
                        break;
                    }
                    if (bp_atk1[di] == 0 && bp_atk2[di] == 0) {
                        if (wp_atk[di] == 1) {
                            score += pst[di];
                            break;
                        }
                        if (wp_atk[di] > 1) break;
                    }
                    score += 0x30;
                    while (1) {
                        score += pst[di];
                        di -= 10;
                        uint8_t sq2 = board[di];
                        if (sq2 == 0) continue;
                        if (sq2 >= 2 && sq2 <= 6) {
                            if (sq2 < 5 && bp_atk1[di] == 0 && wp_atk[di] != 0) break;
                            if (sq2 == 4) {
                                while (1) {
                                    di -= 10;
                                    uint8_t sq3 = board[di];
                                    if (sq3 == 0) continue;
                                    if (sq3 != 4 && sq3 != 5) score += 0xc8;
                                    break;
                                }
                            } else {
                                score += 0xc8;
                            }
                        }
                        break;
                    }
                    break;
                }
                if (sq >= 11) {
                    score += pst[di];
                    continue;
                }
                if (sq > 8) {
                    score += pst[di];
                    break;
                }
                if (di > 0x44) break;
                if (di > 0x26) {
                    uint8_t bl = plist_ix[di];
                    if (dg[0xd478] & fmask[bl]) {
                        score += 0xa0;
                        if (di > 0x3a) score += 0x50;
                        if (board[di - 10] == 0) score += 0x50;
                    }
                    break;
                }
                score += 0xf0;
                if (board[di - 10] == 0) score += 0x50;
                score += pst[di];
                break;
            }
        } else if (pc == 12) { /* Black Queen */
            const uint8_t *pst = dg + 0xe35c;
            int cx = di;

            /* +1 ray */
            while (1) {
                score += pst[di];
                di += 1;
                if (board[di] != 0) break;
            }
            if (kzone[di] == 7) {
                if (board[di] == 8) {
                    if (board[di - 10] == 0) score += 0x60;
                } else if (board[di] > 8) {
                    score += 0x60;
                } else {
                    if (board[di] != 1) score += 0x60;
                    if (bp_atk1[di] <= 0 && wp_atk[di] <= 0) score += 0x60;
                    else if (bp_atk1[di] > 0) score += 0x60;
                }
            }

            /* -1 ray */
            di = cx;
            while (1) {
                di -= 1;
                if (board[di] != 0) break;
                score += pst[di];
            }
            if (kzone[di] == 5) {
                if (board[di] == 8) {
                    if (board[di - 10] == 0) score += 0x60;
                } else if (board[di] > 8) {
                    score += 0x60;
                } else {
                    if (board[di] != 1) score += 0x60;
                    if (bp_atk1[di] <= 0 && wp_atk[di] <= 0) score += 0x60;
                    else if (bp_atk1[di] > 0) score += 0x60;
                }
            }

            /* +10 ray */
            di = cx;
            while (1) {
                di += 10;
                if (board[di] != 0) break;
                score += pst[di];
            }

            /* -10 ray */
            di = cx;
            while (1) {
                di -= 10;
                uint8_t sq = board[di];
                if (sq == 0) {
                    if (wp_atk[di] == 0) score += pst[di];
                    continue;
                }
                if (sq == 7) break;
                if (sq == 11) {
                    score += pst[di];
                    continue;
                }
                if (sq != 8) {
                    if (kzone[di] == 6) {
                        score += 0x60;
                        if ((int8_t)wp_atk[di] <= 0) score += 0x60;
                    }
                }
                score += pst[di];
                break;
            }

            /* +9 ray */
            di = cx;
            while (1) {
                di += 9;
                if (board[di] != 0) break;
                score += pst[di];
            }

            /* -9 ray */
            di = cx;
            while (1) {
                di -= 9;
                uint8_t sq = board[di];
                if (sq == 0) {
                    if (wp_atk[di] == 0) score += pst[di];
                    continue;
                }
                if (sq == 7) break;
                if (sq == 10) {
                    score += pst[di];
                    continue;
                }
                if (kzone[di] == 1) {
                    if (sq == 1) {
                        if ((int8_t)wp_atk[di] <= 0) score += 0x60;
                    } else if (sq != 8 || board[di - 10] != 1) {
                        score += 0x60;
                        if ((int8_t)wp_atk[di] <= 0) score += 0x60;
                    }
                }
                score += pst[di];
                break;
            }

            /* +11 ray */
            di = cx;
            while (1) {
                di += 11;
                if (board[di] != 0) break;
                score += pst[di];
            }

            /* -11 ray */
            di = cx;
            while (1) {
                di -= 11;
                uint8_t sq = board[di];
                if (sq == 0) {
                    if (wp_atk[di] == 0) score += pst[di];
                    continue;
                }
                if (sq == 7) break;
                if (sq == 10) {
                    score += pst[di];
                    continue;
                }
                if (kzone[di] == 2) {
                    if (sq == 1) {
                        if ((int8_t)wp_atk[di] <= 0) score += 0x60;
                    } else if (sq != 8 || board[di - 10] != 1) {
                        score += 0x60;
                        if ((int8_t)wp_atk[di] <= 0) score += 0x60;
                    }
                }
                score += pst[di];
                break;
            }
        }
    next_piece_b:;
    }

    int16_t res = (int16_t)score >> 2;
    int16_t mat = (int16_t)(dg[0xe3d6] | (dg[0xe3d7] << 8));
    if (dg[0xdc78] == 0 && mat <= 0x0e10) {
        res >>= 1;
    }
    return (uint16_t)res;
}

#endif /* MCPRO_EVAL_H */
