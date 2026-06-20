#include <iostream>
#include <sstream>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <cmath>

#define USE_BITBOARD

#include "./state.hpp"
#include "config.hpp"
#include "../../policy/game_history.hpp"

// Material values for evaluation
static const int eval_material[7] = {0, 200, 600, 700, 800, 2000, 1000000};

// Piece-Square Tables (PST) for positional evaluation
static const int eval_pst[6][BOARD_H][BOARD_W] = {
    {
        { -8,  -4,  -2,  -4,  -8 },
        {  6,   8,  10,   8,   6 },
        { 10,  14,  16,  14,  10 },
        { 18,  22,  24,  22,  18 },
        { 32,  36,  40,  36,  32 },
        {  0,   0,   0,   0,   0 },
    },
    {
        { -16,  -8,  -4,  -8, -16 },
        {  -8,   8,  14,   8,  -8 },
        {  -4,  14,  22,  14,  -4 },
        {  -4,  14,  22,  14,  -4 },
        {  -8,   8,  14,   8,  -8 },
        { -16,  -8,  -4,  -8, -16 },
    },
    {
        { -12,  -4,  -2,  -4, -12 },
        {  -4,   8,  10,   8,  -4 },
        {  -2,  12,  14,  12,  -2 },
        {  -2,  12,  14,  12,  -2 },
        {  -4,   8,  10,   8,  -4 },
        { -12,  -4,  -2,  -4, -12 },
    },
    {
        { -6,  -2,   0,  -2,  -6 },
        {  0,   2,   4,   2,   0 },
        {  2,   4,   6,   4,   2 },
        {  2,   4,   6,   4,   2 },
        {  4,   6,   8,   6,   4 },
        { 10,  10,  12,  10,  10 },
    },
    {
        { -8,  -4,  -2,  -4,  -8 },
        { -2,   4,   6,   4,  -2 },
        {  0,   8,  10,   8,   0 },
        {  0,   8,  10,   8,   0 },
        { -2,   4,   6,   4,  -2 },
        { -8,  -4,  -2,  -4,  -8 },
    },
    {
        {  30,  36,  40,  36,  30 },
        {  18,  24,  28,  24,  18 },
        {   6,  10,  14,  10,   6 },
        {  -8,  -4,   0,  -4,  -8 },
        { -22, -18, -12, -18, -22 },
        { -36, -30, -24, -30, -36 },
    },
};

static inline int mobility_bonus(int legal_count) {
    return legal_count * 6;
}

// Mirror PST for black player
static inline int pst_score_for(int piece_type, int player, int row, int col) {
    if (piece_type <= 0 || piece_type > 6) return 0;
    if (player == 0) return eval_pst[piece_type - 1][row][col];
    return eval_pst[piece_type - 1][BOARD_H - 1 - row][BOARD_W - 1 - col];
}

// Check for passed pawns
static bool is_passed_pawn(const State* state, int player, int row, int col) {
    int opp = 1 - player;
    if (state->board.board[player][row][col] != 1) return false;
    
    if (player == 0) {
        for (int r = 0; r < row; ++r) {
            for (int dc = -1; dc <= 1; ++dc) {
                int c = col + dc;
                if (c >= 0 && c < BOARD_W && state->board.board[opp][r][c] == 1) return false;
            }
        }
    } else {
        for (int r = row + 1; r < BOARD_H; ++r) {
            for (int dc = -1; dc <= 1; ++dc) {
                int c = col + dc;
                if (c >= 0 && c < BOARD_W && state->board.board[opp][r][c] == 1) return false;
            }
        }
    }
    return true;
}

static inline int pawn_distance_to_promotion(int player, int row) {
    return player == 0 ? row : (BOARD_H - 1 - row);
}

// Main evaluation function
int State::evaluate(bool use_kp_eval, bool use_mobility, const GameHistory* history) {
    (void)history; 

    if (this->game_state == WIN) {
        return P_MAX;
    }

    int score = 0;
    int my_king_r = -1, my_king_c = -1;
    int op_king_r = -1, op_king_c = -1;
    int material_diff = 0;

    for (int r = 0; r < BOARD_H; ++r) {
        for (int c = 0; c < BOARD_W; ++c) {
            int my_piece = this->board.board[this->player][r][c];
            int op_piece = this->board.board[1 - this->player][r][c];

            if (my_piece) {
                score += eval_material[my_piece];
                material_diff += eval_material[my_piece];
                score += pst_score_for(my_piece, this->player, r, c);
                if (my_piece == 6) {
                    my_king_r = r;
                    my_king_c = c;
                }
            }

            if (op_piece) {
                score -= eval_material[op_piece];
                material_diff -= eval_material[op_piece];
                score -= pst_score_for(op_piece, 1 - this->player, r, c);
                if (op_piece == 6) {
                    op_king_r = r;
                    op_king_c = c;
                }
            }
        }
    }

    // Light king tropism bonus
    if (my_king_r != -1 && op_king_r != -1) {
        int king_dist = std::max(std::abs(my_king_r - op_king_r), std::abs(my_king_c - op_king_c));
        score += (3 - std::min(3, king_dist)) * 12;
    }

    if (use_mobility) {
        score += mobility_bonus(static_cast<int>(this->legal_actions.size()));
    }

    // Passed pawn evaluation
    int minor_material = 0;
    for (int r = 0; r < BOARD_H; ++r) {
        for (int c = 0; c < BOARD_W; ++c) {
            int my_piece = this->board.board[this->player][r][c];
            int opp_piece = this->board.board[1 - this->player][r][c];
            
            if (my_piece > 1 && my_piece != 6) minor_material += 1;
            if (opp_piece > 1 && opp_piece != 6) minor_material += 1;

            if (my_piece == 1 && is_passed_pawn(this, this->player, r, c)) {
                int dist = pawn_distance_to_promotion(this->player, r);
                score += 180 * (BOARD_H - dist);
            }
            
            if (opp_piece == 1 && is_passed_pawn(this, 1 - this->player, r, c)) {
                int dist = pawn_distance_to_promotion(1 - this->player, r);
                int penalty = 0;
                if (dist <= 1) penalty = 1800;
                else if (dist == 2) penalty = 900;
                
                if (this->player == 1) {
                    penalty = (penalty * 3) / 2; 
                }
                score -= penalty;
            }
        }
    }

    // King safety for black
    if (this->player == 1 && my_king_r != -1 && my_king_c != -1) {
        int danger = 0;
        for (int r = 0; r < BOARD_H; ++r) {
            for (int c = 0; c < BOARD_W; ++c) {
                int opp_piece = this->board.board[0][r][c];
                if (opp_piece) {
                    int dist = std::abs(r - my_king_r) + std::abs(c - my_king_c);
                    if (dist <= 2) {
                        int weight = 0;
                        switch (opp_piece) {
                            case 1: weight = 1; break; 
                            case 2: weight = 4; break; 
                            case 3: weight = 3; break; 
                            case 4: weight = 3; break; 
                            case 5: weight = 6; break; 
                            default: weight = 2; break;
                        }
                        danger += (3 - dist) * weight;
                    }
                }
            }
        }
        score -= danger * 60;
    }

    // Endgame mating logic (force king to edges/corners)
    if (minor_material <= 4 && my_king_r != -1 && op_king_r != -1) {
        int king_manhattan = std::abs(my_king_r - op_king_r) + std::abs(my_king_c - op_king_c);
        int bonus_scale = 18 + std::max(-8, std::min(material_diff / 100, 18));
        score += (12 - king_manhattan) * bonus_scale;

        constexpr int center_r = BOARD_H / 2;
        constexpr int center_c = BOARD_W / 2;
        int opp_center_dist = std::abs(op_king_r - center_r) + std::abs(op_king_c - center_c);
        score += opp_center_dist * (26 + std::max(-6, std::min(material_diff / 160, 12)));
    }

    score -= this->step;
    (void)use_kp_eval;

    return score;
}

// Zobrist Hashing implementation
static uint64_t zobrist_piece[2][7][BOARD_H][BOARD_W];
static uint64_t zobrist_side;
static bool zobrist_ready = false;

static void init_zobrist() {
    uint64_t s = 0x7A35C9D1E4F02B68ULL;
    auto rand64 = [&s]() -> uint64_t {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s;
    };
    for (int p = 0; p < 2; p++) {
        for (int t = 0; t < 7; t++) {
            for (int r = 0; r < BOARD_H; r++) {
                for (int c = 0; c < BOARD_W; c++) {
                    zobrist_piece[p][t][r][c] = rand64();
                }
            }
        }
    }
    zobrist_side = rand64();
    zobrist_ready = true;
}

uint64_t State::compute_hash_full() const {
    if (!zobrist_ready) {
        init_zobrist();
    }
    uint64_t h = 0;
    for (int p = 0; p < 2; p++) {
        for (int r = 0; r < BOARD_H; r++) {
            for (int c = 0; c < BOARD_W; c++) {
                int piece = this->board.board[p][r][c];
                if (piece) {
                    h ^= zobrist_piece[p][piece][r][c];
                }
            }
        }
    }
    if (this->player) {
        h ^= zobrist_side;
    }
    return h;
}

State* State::next_state(const Move& move) {
    if (!zobrist_ready) { init_zobrist(); }

    Board next = this->board;
    Point from = move.first, to = move.second;
    int p = this->player;
    int opp = 1 - p;

    int8_t orig_piece = next.board[p][from.first][from.second];
    int8_t moved = orig_piece;
    
    // Pawn promotion
    if (moved == 1 && (to.first == BOARD_H - 1 || to.first == 0)) {
        moved = 5;
    }

    // Incremental Zobrist hash update
    uint64_t h = this->hash();
    h ^= zobrist_side;

    h ^= zobrist_piece[p][orig_piece][from.first][from.second];

    int8_t captured = next.board[opp][to.first][to.second];
    if (captured) {
        h ^= zobrist_piece[opp][captured][to.first][to.second];
        next.board[opp][to.first][to.second] = 0;
    }

    h ^= zobrist_piece[p][moved][to.first][to.second];

    next.board[p][from.first][from.second] = 0;
    next.board[p][to.first][to.second] = moved;

    State* ns = new State(next, opp);
    ns->step = this->step + 1;
    ns->zobrist_hash = h;
    ns->zobrist_valid = true;
    return ns;
}

// Fallback naive move generation
static const int move_table_rook_bishop[8][7][2] = {
    {{0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 5}, {0, 6}, {0, 7}},
    {{0, -1}, {0, -2}, {0, -3}, {0, -4}, {0, -5}, {0, -6}, {0, -7}},
    {{1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}, {6, 0}, {7, 0}},
    {{-1, 0}, {-2, 0}, {-3, 0}, {-4, 0}, {-5, 0}, {-6, 0}, {-7, 0}},
    {{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6}, {7, 7}},
    {{1, -1}, {2, -2}, {3, -3}, {4, -4}, {5, -5}, {6, -6}, {7, -7}},
    {{-1, 1}, {-2, 2}, {-3, 3}, {-4, 4}, {-5, 5}, {-6, 6}, {-7, 7}},
    {{-1, -1}, {-2, -2}, {-3, -3}, {-4, -4}, {-5, -5}, {-6, -6}, {-7, -7}},
};

static const int move_table_knight[8][2] = {
    {2, 1}, {2, -1}, {-2, 1}, {-2, -1},
    {1, 2}, {1, -2}, {-1, 2}, {-1, -2}
};

static const int move_table_king[8][2] = {
    {1, 0}, {0, 1}, {-1, 0}, {0, -1}, 
    {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
};

void State::get_legal_actions_naive() {
    this->game_state = NONE;
    std::vector<Move> all_actions;
    all_actions.reserve(64);
    auto self_board = this->board.board[this->player];
    auto oppn_board = this->board.board[1 - this->player];

    int now_piece, oppn_piece;
    for (int i = 0; i < BOARD_H; i++) {
        for (int j = 0; j < BOARD_W; j++) {
            if ((now_piece = self_board[i][j])) {
                switch (now_piece) {
                    case 1:
                        if (this->player && i < BOARD_H - 1) {
                            if (!oppn_board[i+1][j] && !self_board[i+1][j]) {
                                all_actions.push_back(Move(Point(i, j), Point(i+1, j)));
                            }
                            if (j < BOARD_W - 1 && (oppn_piece = oppn_board[i+1][j+1]) > 0) {
                                all_actions.push_back(Move(Point(i, j), Point(i+1, j+1)));
                                if (oppn_piece == 6) {
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                            if (j > 0 && (oppn_piece = oppn_board[i+1][j-1]) > 0) {
                                all_actions.push_back(Move(Point(i, j), Point(i+1, j-1)));
                                if (oppn_piece == 6) {
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                        } else if (!this->player && i > 0) {
                            if (!oppn_board[i-1][j] && !self_board[i-1][j]) {
                                all_actions.push_back(Move(Point(i, j), Point(i-1, j)));
                            }
                            if (j < BOARD_W - 1 && (oppn_piece = oppn_board[i-1][j+1]) > 0) {
                                all_actions.push_back(Move(Point(i, j), Point(i-1, j+1)));
                                if (oppn_piece == 6) {
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                            if (j > 0 && (oppn_piece = oppn_board[i-1][j-1]) > 0) {
                                all_actions.push_back(Move(Point(i, j), Point(i-1, j-1)));
                                if (oppn_piece == 6) {
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                        }
                        break;

                    case 2:
                    case 4:
                    case 5:
                        int st, end;
                        switch (now_piece) {
                            case 2: st = 0; end = 4; break;
                            case 4: st = 4; end = 8; break;
                            case 5: st = 0; end = 8; break;
                            default: st = 0; end = -1;
                        }
                        for (int part = st; part < end; part++) {
                            auto move_list = move_table_rook_bishop[part];
                            for (int k = 0; k < std::max(BOARD_H, BOARD_W); k++) {
                                int p[2] = {move_list[k][0] + i, move_list[k][1] + j};

                                if (p[0] >= BOARD_H || p[0] < 0 || p[1] >= BOARD_W || p[1] < 0) break;
                                
                                now_piece = self_board[p[0]][p[1]];
                                if (now_piece) break;

                                all_actions.push_back(Move(Point(i, j), Point(p[0], p[1])));

                                oppn_piece = oppn_board[p[0]][p[1]];
                                if (oppn_piece) {
                                    if (oppn_piece == 6) {
                                        this->game_state = WIN;
                                        this->legal_actions = all_actions;
                                        return;
                                    } else {
                                        break;
                                    }
                                }
                            }
                        }
                        break;

                    case 3:
                        for (auto move : move_table_knight) {
                            int p[2] = {move[0] + i, move[1] + j};
                            if (p[0] >= BOARD_H || p[0] < 0 || p[1] >= BOARD_W || p[1] < 0) continue;
                            
                            now_piece = self_board[p[0]][p[1]];
                            if (now_piece) continue;

                            all_actions.push_back(Move(Point(i, j), Point(p[0], p[1])));

                            oppn_piece = oppn_board[p[0]][p[1]];
                            if (oppn_piece == 6) {
                                this->game_state = WIN;
                                this->legal_actions = all_actions;
                                return;
                            }
                        }
                        break;

                    case 6:
                        for (auto move : move_table_king) {
                            int p[2] = {move[0] + i, move[1] + j};
                            if (p[0] >= BOARD_H || p[0] < 0 || p[1] >= BOARD_W || p[1] < 0) continue;
                            
                            now_piece = self_board[p[0]][p[1]];
                            if (now_piece) continue;

                            all_actions.push_back(Move(Point(i, j), Point(p[0], p[1])));

                            oppn_piece = oppn_board[p[0]][p[1]];
                            if (oppn_piece == 6) {
                                this->game_state = WIN;
                                this->legal_actions = all_actions;
                                return;
                            }
                        }
                        break;
                }
            }
        }
    }
    this->legal_actions = all_actions;
}

// Bitboard definitions for high-speed move generation
#define BB_SQ(r, c)  ((r) * BOARD_W + (c))
#define BB_ROW(sq)   ((sq) / BOARD_W)
#define BB_COL(sq)   ((sq) % BOARD_W)

static uint32_t bb_knight[30];      
static uint32_t bb_king[30];        
static uint32_t bb_pawn_push[2][30];
static uint32_t bb_pawn_cap[2][30]; 
static bool bb_ready = false;

static const int bb_dr[8] = {0, 0, 1, -1, 1, 1, -1, -1};
static const int bb_dc[8] = {1, -1, 0, 0, 1, -1, 1, -1};

static void bb_init() {
    static const int kn_dr[8] = {1, 1, -1, -1, 2, 2, -2, -2};
    static const int kn_dc[8] = {2, -2, 2, -2, 1, -1, 1, -1};
    static const int ki_dr[8] = {1, 0, -1, 0, 1, 1, -1, -1};
    static const int ki_dc[8] = {0, 1, 0, -1, 1, -1, 1, -1};

    for (int r = 0; r < BOARD_H; r++) {
        for (int c = 0; c < BOARD_W; c++) {
            int sq = BB_SQ(r, c);

            bb_knight[sq] = 0;
            for (int d = 0; d < 8; d++) {
                int nr = r + kn_dr[d], nc = c + kn_dc[d];
                if (nr >= 0 && nr < BOARD_H && nc >= 0 && nc < BOARD_W) {
                    bb_knight[sq] |= 1u << BB_SQ(nr, nc);
                }
            }

            bb_king[sq] = 0;
            for (int d = 0; d < 8; d++) {
                int nr = r + ki_dr[d], nc = c + ki_dc[d];
                if (nr >= 0 && nr < BOARD_H && nc >= 0 && nc < BOARD_W) {
                    bb_king[sq] |= 1u << BB_SQ(nr, nc);
                }
            }

            bb_pawn_push[0][sq] = 0;
            bb_pawn_cap[0][sq] = 0;
            if (r > 0) {
                bb_pawn_push[0][sq] = 1u << BB_SQ(r-1, c);
                if (c > 0) bb_pawn_cap[0][sq] |= 1u << BB_SQ(r-1, c-1);
                if (c < BOARD_W-1) bb_pawn_cap[0][sq] |= 1u << BB_SQ(r-1, c+1);
            }

            bb_pawn_push[1][sq] = 0;
            bb_pawn_cap[1][sq] = 0;
            if (r < BOARD_H-1) {
                bb_pawn_push[1][sq] = 1u << BB_SQ(r+1, c);
                if (c > 0) bb_pawn_cap[1][sq] |= 1u << BB_SQ(r+1, c-1);
                if (c < BOARD_W-1) bb_pawn_cap[1][sq] |= 1u << BB_SQ(r+1, c+1);
            }
        }
    }
    bb_ready = true;
}

// Bitboard-accelerated legal move generation
void State::get_legal_actions_bitboard() {
    if (!bb_ready) bb_init();

    this->game_state = NONE;
    this->legal_actions.clear();
    this->legal_actions.reserve(64);

    int self = this->player;
    int oppn = 1 - self;

    uint32_t self_occ = 0, oppn_occ = 0;
    int self_pt[30] = {};  
    int oppn_pt[30] = {};  

    for (int r = 0; r < BOARD_H; r++) {
        for (int c = 0; c < BOARD_W; c++) {
            int sq = BB_SQ(r, c);
            if (this->board.board[self][r][c]) {
                self_occ |= 1u << sq;
                self_pt[sq] = this->board.board[self][r][c];
            }
            if (this->board.board[oppn][r][c]) {
                oppn_occ |= 1u << sq;
                oppn_pt[sq] = this->board.board[oppn][r][c];
            }
        }
    }

    uint32_t all_occ = self_occ | oppn_occ;
    uint32_t pieces = self_occ;

    while (pieces) {
        int sq = __builtin_ctz(pieces);
        pieces &= pieces - 1;
        int r = BB_ROW(sq), c = BB_COL(sq);
        int piece = self_pt[sq];
        uint32_t targets = 0;

        switch (piece) {
            case 1: { 
                uint32_t push = bb_pawn_push[self][sq] & ~all_occ;
                uint32_t cap = bb_pawn_cap[self][sq] & oppn_occ;
                uint32_t cap_scan = cap;
                while (cap_scan) {
                    int to = __builtin_ctz(cap_scan);
                    cap_scan &= cap_scan - 1;
                    if (oppn_pt[to] == 6) {
                        this->game_state = WIN;
                        this->legal_actions.push_back(Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
                        return;
                    }
                }
                targets = push | cap;
                break;
            }

            case 3: { 
                targets = bb_knight[sq] & ~self_occ;
                uint32_t opp_targets = targets & oppn_occ;
                while (opp_targets) {
                    int to = __builtin_ctz(opp_targets);
                    opp_targets &= opp_targets - 1;
                    if (oppn_pt[to] == 6) {
                        this->game_state = WIN;
                        this->legal_actions.push_back(Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
                        return;
                    }
                }
                break;
            }

            case 6: { 
                targets = bb_king[sq] & ~self_occ;
                uint32_t opp_targets = targets & oppn_occ;
                while (opp_targets) {
                    int to = __builtin_ctz(opp_targets);
                    opp_targets &= opp_targets - 1;
                    if (oppn_pt[to] == 6) {
                        this->game_state = WIN;
                        this->legal_actions.push_back(Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
                        return;
                    }
                }
                break;
            }

            case 2: 
            case 4: 
            case 5: { 
                int d_start = (piece == 4) ? 4 : 0;
                int d_end   = (piece == 2) ? 4 : 8;
                for (int d = d_start; d < d_end; d++) {
                    int cr = r + bb_dr[d], cc = c + bb_dc[d];
                    while (cr >= 0 && cr < BOARD_H && cc >= 0 && cc < BOARD_W) {
                        int to = BB_SQ(cr, cc);
                        uint32_t to_bit = 1u << to;
                        if (self_occ & to_bit) {
                            break; 
                        }

                        if ((oppn_occ & to_bit) && oppn_pt[to] == 6) {
                            this->game_state = WIN;
                            this->legal_actions.push_back(Move(Point(r, c), Point(cr, cc)));
                            return;
                        }

                        targets |= to_bit;
                        if (oppn_occ & to_bit) {
                            break; 
                        }
                        cr += bb_dr[d]; cc += bb_dc[d];
                    }
                }
                break;
            }
        }

        while (targets) {
            int to = __builtin_ctz(targets);
            targets &= targets - 1;
            this->legal_actions.push_back(Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
        }
    }
}

void State::get_legal_actions() {
    #ifdef USE_BITBOARD
    get_legal_actions_bitboard();
    #else
    get_legal_actions_naive();
    #endif
}

const char piece_table[2][7][5] = {
  {" ", "♙", "♖", "♘", "♗", "♕", "♔"},
  {" ", "♟", "♜", "♞", "♝", "♛", "♚"}
};

std::string State::encode_output() const {
    std::stringstream ss;
    int now_piece;
    for (int i = 0; i < BOARD_H; i++) {
        for (int j = 0; j < BOARD_W; j++) {
            if ((now_piece = this->board.board[0][i][j])) {
                ss << std::string(piece_table[0][now_piece]);
            } else if ((now_piece = this->board.board[1][i][j])) {
                ss << std::string(piece_table[1][now_piece]);
            } else {
                ss << " ";
            }
            ss << " ";
        }
        ss << "\n";
    }
    return ss.str();
}

std::string State::encode_state() {
    std::stringstream ss;
    ss << this->player;
    ss << "\n";
    for (int pl = 0; pl < 2; pl++) {
        for (int i = 0; i < BOARD_H; i++) {
            for (int j = 0; j < BOARD_W; j++) {
                ss << int(this->board.board[pl][i][j]);
                ss << " ";
            }
            ss << "\n";
        }
        ss << "\n";
    }
    return ss.str();
}

BaseState* State::create_null_state() const {
    State* s = new State(this->board, 1 - this->player);
    s->step = this->step + 1;
    s->zobrist_valid = false;
    s->game_state = this->game_state;
    s->get_legal_actions();
    return s;
}

static const char* piece_chars = ".PRNBQK";
static const char* piece_chars_lower = ".prnbqk";

std::string State::encode_board() const {
    std::string s;
    for (int r = 0; r < BOARD_H; r++) {
        if (r > 0) {
            s += '/';
        }
        for (int c = 0; c < BOARD_W; c++) {
            int w = board.board[0][r][c];
            int b = board.board[1][r][c];
            if (w > 0 && w <= 6) {
                s += piece_chars[w];
            } else if (b > 0 && b <= 6) {
                s += piece_chars_lower[b];
            } else {
                s += '.';
            }
        }
    }
    return s;
}

void State::decode_board(const std::string& s, int side_to_move) {
    player = side_to_move;
    game_state = UNKNOWN;
    zobrist_valid = false;
    board = Board{};
    int r = 0, c = 0;
    
    for (char ch : s) {
        if (ch == '/') {
            r++;
            c = 0;
            continue;
        }
        if (r >= BOARD_H || c >= BOARD_W) {
            break;
        }
        if (ch >= 'A' && ch <= 'Z') {
            for (int p = 1; p <= 6; p++) {
                if (piece_chars[p] == ch) {
                    board.board[0][r][c] = p;
                    break;
                }
            }
        } else if (ch >= 'a' && ch <= 'z') {
            for (int p = 1; p <= 6; p++) {
                if (piece_chars_lower[p] == ch) {
                    board.board[1][r][c] = p;
                    break;
                }
            }
        }
        c++;
    }
    get_legal_actions();
}

std::string State::cell_display(int row, int col) const {
    int w = static_cast<int>(board.board[0][row][col]);
    int b = static_cast<int>(board.board[1][row][col]);
    if (w) {
        const char* names = ".PRNBQK";
        return std::string(" ") + names[w] + " ";
    } else if (b) {
        const char* names = ".prnbqk";
        return std::string(" ") + names[b] + " ";
    } else {
        return " . ";
    }
}

bool State::check_repetition(const GameHistory& history, int& out_score) const {
    if (history.count(hash()) >= 2) {
        out_score = 0;
        return true;
    }
    return false;
}