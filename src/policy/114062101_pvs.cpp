#include <utility>
#include <algorithm>
#include "114062101_state.hpp"
#include "114062101_pvs.hpp"

// Transposition Table flags and entry structure
enum TTFlag { TT_EXACT, TT_ALPHA, TT_BETA };
struct TTEntry {
    uint64_t hash;
    int depth;
    int score;
    TTFlag flag;
};

// 8MB Transposition Table 
const int TT_SIZE = 8388593;
TTEntry TT[TT_SIZE];

// Killer moves table for move ordering
Move killer_moves[100][2];

/*============================================================
 * MiniMax — eval_ctx (PVS + TT + LMR + Killer Heuristics)
 *============================================================*/
int MiniMax::eval_ctx(State *state, int depth, GameHistory& history, int ply, SearchContext& ctx, const MMParams& p, int alpha, int beta) {
    ctx.nodes++;
    if (ply > ctx.seldepth) ctx.seldepth = ply;
    if (ctx.stop) return 0;

    if (state->legal_actions.empty() && state->game_state == UNKNOWN) {
        state->get_legal_actions();
    }
    
    if (state->game_state == WIN) return P_MAX - ply;
    if (state->game_state == DRAW) return -10;

    int rep_score;
    if (state->check_repetition(history, rep_score)) {
        return rep_score == 0 ? -10 : rep_score; 
    }

    // Transposition Table (TT) Probe
    int original_alpha = alpha;
    uint64_t hash = state->hash();
    TTEntry& tt_entry = TT[hash % TT_SIZE];
    
    if (tt_entry.hash == hash && tt_entry.depth >= depth) {
        if (tt_entry.flag == TT_EXACT) return tt_entry.score;
        if (tt_entry.flag == TT_ALPHA && tt_entry.score <= alpha) return tt_entry.score;
        if (tt_entry.flag == TT_BETA && tt_entry.score >= beta) return tt_entry.score;
    }
    
    history.push(hash);

    // Quiescence Search (QS) to resolve active tactical captures
    if (depth <= 0) {
        int stand_pat = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
        if (stand_pat >= beta) {
            history.pop(hash);
            return stand_pat;
        }
        if (alpha < stand_pat) alpha = stand_pat;

        int opp = 1 - state->player;
        std::vector<Move> captures;
        const int qs_piece_val[7] = {0, 2, 6, 7, 8, 20, 100};

        for (auto& m : state->legal_actions) {
            int r2 = m.second.first, c2 = m.second.second;
            int attacker = state->board.board[state->player][m.first.first][m.first.second];
            int victim = state->board.board[opp][r2][c2];
            if (victim > 0 || (attacker == 1 && (r2 == 0 || r2 == BOARD_H - 1))) {
                captures.push_back(m);
            }
        }

        if (!captures.empty()) {
            // MVV-LVA for tactical moves
            auto eval_qs = [&](const Move& m) {
                int r2 = m.second.first, c2 = m.second.second;
                int attacker = state->board.board[state->player][m.first.first][m.first.second];
                int victim = state->board.board[opp][r2][c2];
                int score = 0;
                if (victim > 0) score += 10000 + (qs_piece_val[victim] - qs_piece_val[attacker]);
                if (attacker == 1 && (r2 == 0 || r2 == BOARD_H - 1)) score += 500;
                return score;
            };

            std::stable_sort(captures.begin(), captures.end(), [&](const Move& a, const Move& b) {
                return eval_qs(a) > eval_qs(b);
            });

            int best_score = stand_pat;
            for (auto& action : captures) {
                State* next = state->next_state(action);
                bool same = next->same_player_as_parent();
                int raw_score = eval_ctx(next, depth - 1, history, ply + 1, ctx, p, same ? alpha : -beta, same ? beta : -alpha);
                int score = same ? raw_score : -raw_score;
                delete next;

                if (score > best_score) best_score = score;
                if (score > alpha) alpha = score;
                if (alpha >= beta) break;
            }
            history.pop(hash);
            return best_score;
        }
        history.pop(hash);
        return stand_pat;
    }

    // Main Search Move Ordering (MVV-LVA + Killer Moves)
    int opp = 1 - state->player;
    auto sorted_actions = state->legal_actions;
    const int main_piece_val[7] = {0, 20, 60, 70, 80, 200, 1000};

    auto eval_main = [&](const Move& m) {
        int r2 = m.second.first, c2 = m.second.second;
        int attacker = state->board.board[state->player][m.first.first][m.first.second];
        int victim = state->board.board[opp][r2][c2];
        int score = 0;
        
        if (victim > 0) score += 10000 + (main_piece_val[victim] - main_piece_val[attacker]);
        if (attacker == 1 && (r2 == 0 || r2 == BOARD_H - 1)) score += 500;
        if (victim == 0 && ply < 100 && (m == killer_moves[ply][0] || m == killer_moves[ply][1])) score += 50;
        
        return score;
    };

    std::stable_sort(sorted_actions.begin(), sorted_actions.end(), [&](const Move& m1, const Move& m2) {
        return eval_main(m1) > eval_main(m2);
    });

    int best_score = M_MAX;
    bool first_move = true;
    int move_count = 0;

    // Principal Variation Search (PVS) with Null Window
    for (auto& action : sorted_actions) {
        move_count++;
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();
        int next_depth = same ? depth : depth - 1;
        int raw_score, score;

        bool is_capture = state->board.board[opp][action.second.first][action.second.second] != 0;
        bool is_killer = (ply < 100) && (action == killer_moves[ply][0] || action == killer_moves[ply][1]);

        if (first_move) {
            raw_score = eval_ctx(next, next_depth, history, ply + 1, ctx, p, same ? alpha : -beta, same ? beta : -alpha);
            score = same ? raw_score : -raw_score;
            first_move = false;
        } else {
            // Late Move Reductions (LMR) - adjust conditions if necessary to avoid pruning critical pawn pushes
            int reduced_depth = next_depth;
            if (next_depth >= 3 && move_count >= 4 && !is_capture && !is_killer) {
                reduced_depth -= 1; 
            }

            raw_score = eval_ctx(next, reduced_depth, history, ply + 1, ctx, p, same ? alpha : -(alpha + 1), same ? alpha + 1 : -alpha);
            score = same ? raw_score : -raw_score;

            if (score > alpha && reduced_depth < next_depth) {
                raw_score = eval_ctx(next, next_depth, history, ply + 1, ctx, p, same ? alpha : -(alpha + 1), same ? alpha + 1 : -alpha);
                score = same ? raw_score : -raw_score;
            }
            
            if (score > alpha && score < beta) {
                raw_score = eval_ctx(next, next_depth, history, ply + 1, ctx, p, same ? alpha : -beta, same ? beta : -alpha);
                score = same ? raw_score : -raw_score;
            }
        }
        delete next;

        if (score > best_score) best_score = score;
        if (score > alpha) alpha = score;
        
        if (alpha >= beta) {
            if (!is_capture && ply < 100) {
                if (killer_moves[ply][0] != action) {
                    killer_moves[ply][1] = killer_moves[ply][0];
                    killer_moves[ply][0] = action;
                }
            }
            break;
        }
        if (ctx.stop) break;
    }

    // Transposition Table (TT) Store
    tt_entry.hash = hash;
    tt_entry.depth = depth;
    tt_entry.score = best_score;
    if (best_score <= original_alpha) tt_entry.flag = TT_ALPHA;
    else if (best_score >= beta) tt_entry.flag = TT_BETA;
    else tt_entry.flag = TT_EXACT;

    history.pop(hash);
    return best_score;
}

/*============================================================
 * MiniMax — search (Root Node)
 *============================================================*/
SearchResult MiniMax::search(State *state, int depth, GameHistory& history, SearchContext& ctx) {
    ctx.reset();
    MMParams p = MMParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    if (!state->legal_actions.size()) {
        state->get_legal_actions();
    }

    int alpha = M_MAX;
    int beta = P_MAX;
    int best_score = M_MAX - 10;
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();

    for (auto& action : state->legal_actions) {
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();
        
        int next_depth = same ? depth : depth - 1;
        int raw_score = eval_ctx(next, next_depth, history, 1, ctx, p, same ? alpha : -beta, same ? beta  : -alpha);
        int score = same ? raw_score : -raw_score;
        
        delete next;

        if (score > best_score) {
            best_score = score;
            result.best_move = action;
            result.pv = {action};

            if (p.report_partial && ctx.on_root_update) {
               ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
            }
        }  
        if (score > alpha) {
            alpha = score;
        }
        move_index++;
    }

    result.score = best_score;
    result.nodes = ctx.nodes;
    result.seldepth = ctx.seldepth;
    
    return result;
}

/*============================================================
 * MiniMax — default_params / param_defs
 *============================================================*/
ParamMap MiniMax::default_params() {
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
    };
}

std::vector<ParamDef> MiniMax::param_defs() {
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
    };
}