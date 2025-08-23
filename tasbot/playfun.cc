/* Tries playing a game (deliberately not customized to any particular
   ROM) using an objective function learned by learnfun.

   This is the third iteration. It attempts to fix a problem where
   playfun-futures would get stuck in local maxima, like the overhang
   in Mario's world 1-2.
*/

#include <vector>
#include <string>
#include <set>
#include <cmath>
#include <stdint.h>

#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>

#include "tasbot.h"

#include "fceu/driver.h"
#include "fceu/drivers/common/args.h"
#include "fceu/state.h"
#include "basis-util.h"
#include "emulator.h"
#include "fceu/fceu.h"
#include "fceu/types.h"
#include "simplefm2.h"
#include "weighted-objectives.h"
#include "motifs.h"
#include "../cc-lib/arcfour.h"
#include "util.h"
#include "../cc-lib/textsvg.h"

#if MARIONET
#include "SDL.h"
#include "SDL_net.h"
#include "marionet.pb.h"
#include "netutil.h"
using ::google::protobuf::Message;
#endif

// deprecated
#define FASTFORWARD 0

// XXX learnfun should write this to a file called game.base64.
#define BASE64 "base64:jjYwGG411HcjG/j9UOVM3Q=="

// This is the factor that determines how quickly a motif changes
// weight. When a motif is chosen because it yields the best future,
// we check its immediate effect on the state (normalized); if an
// increase, then we divide its weight by alpha. If a decrease, then
// we multiply. Should be a value in (0, 1] but usually around 0.8.
#define MOTIF_ALPHA 0.8
// Largest fraction of the total weight that any motif is allowed to
// have when being reweighted up. We don't reweight down to the cap,
// but prevent it from going over. Also, this can be violated if one
// motif is at the max and another has its weight reduced, but still
// keeps motifs from getting weighted out of control.
#define MOTIF_MAX_FRAC 0.1
// Minimum fraction allowed when reweighting down. We don't decrease
// below this, but don't increase to meet the fraction, either.
#define MOTIF_MIN_FRAC 0.00001

// XXX cheats -- should be 0xFF
// no support any more
// #define INPUTMASK (~(INPUT_T | INPUT_S))

struct Scoredist {
  Scoredist() : startframe(0), chosen_idx() {}
  explicit Scoredist(int startframe) : startframe(startframe),
				       chosen_idx(0) {}
  int startframe;
  vector<double> immediates;
  vector<double> positives;
  vector<double> negatives;
  vector<double> norms;
  int chosen_idx;
};

static void SaveDistributionSVG(const vector<Scoredist> &dists,
				const string &filename) {
  static const double WIDTH = 1024.0;
  static const double HEIGHT = 1024.0;

  // Add slop for radii.
  string out = TextSVG::Header(WIDTH + 12, HEIGHT + 12);

  // immediates, positives, negatives all are in same value space
  double maxval = 0.0;
  for (int i = 0; i < dists.size(); i++) {
    const Scoredist &dist = dists[i];
    maxval =
      VectorMax(VectorMax(VectorMax(maxval, dist.negatives),
			  dist.positives),
		dist.immediates);
  }

  int totalframes = dists.back().startframe;

  for (int i = 0; i < dists.size(); i++) {
    const Scoredist &dist = dists[i];
    double xf = dist.startframe / (double)totalframes;
    out += DrawDots(WIDTH, HEIGHT,
		    "#33A", xf, dist.immediates, maxval, dist.chosen_idx);
    out += DrawDots(WIDTH, HEIGHT,
		    "#090", xf, dist.positives, maxval, dist.chosen_idx);
    out += DrawDots(WIDTH, HEIGHT,
		    "#A33", xf, dist.negatives, maxval, dist.chosen_idx);
    // out += DrawDots("#000", xf, dist.norms, 1.0, dist.chosen_idx);
  }

  // XXX args?
  out += SVGTickmarks(WIDTH, totalframes, 50.0, 20.0, 12.0);

  out += TextSVG::Footer();
  Util::WriteFile(filename, out);
  printf("Wrote distributions to %s.\n", filename.c_str());
}

namespace {
struct Future {
  vector<uint8> inputs;
  bool weighted;
  int desired_length;
  // TODO
  int rounds_survived;
  bool is_mutant;
  Future() : weighted(true), desired_length(0), rounds_survived(0),
	     is_mutant(false) {}
  Future(bool w, int d) : weighted(w),
			  desired_length(d),
			  rounds_survived(0),
			  is_mutant(false) {}
};

// For backtracking.
struct Replacement {
  vector<uint8> inputs;
  double score;
  string method;
};
}  // namespace

static void SaveFuturesHTML(const vector<Future> &futures,
			    const string &filename) {
  string out;
  for (int i = 0; i < futures.size(); i++) {
    out += StringPrintf("<div>%d. len %d/%d. %s %s\n", i,
			futures[i].inputs.size(),
			futures[i].desired_length,
			futures[i].is_mutant ? "mutant" : "fresh",
			futures[i].weighted ? "weighted" : "random");
    for (int j = 0; j < futures[i].inputs.size(); j++) {
      out += SimpleFM2::InputToColorString(futures[i].inputs[j]);
    }
    out += "</div>\n";
  }
  Util::WriteFile(filename, out);
  printf("Wrote futures to %s\n", filename.c_str());
}

struct PlayFun {
  PlayFun() : watermark(0), log(NULL), rc("playfun"), nfutures_(NFUTURES) {
    map<string, string> config = Util::ReadFileToMap("config.txt");
    if (config.empty()) {
      fprintf(stderr, "You need a file called config.txt; please "
	      "take a look at the README.\n");
      abort();
    }

    game = config["game"];
    const string moviename = config["movie"];
    CHECK(!game.empty());
    CHECK(!moviename.empty());

    Emulator::Initialize(game + ".nes");
    objectives = WeightedObjectives::LoadFromFile(game + ".objectives");
    CHECK(objectives);
    fprintf(stderr, "Loaded %d objective functions\n", objectives->Size());

    motifs = Motifs::LoadFromFile(game + ".motifs");
    CHECK(motifs);

    // XXX configure this via config.txt
    // For 64-bit machines with loads of ram
    // Emulator::ResetCache(100000, 10000);
    // For modest systems
    Emulator::ResetCache(100000, 10000);

  motifvec = motifs->AllMotifs();

    // Initialize dynamic futures target within bounds.
    if (nfutures_ < MIN_FUTURES) nfutures_ = MIN_FUTURES;
    if (nfutures_ > MAX_FUTURES) nfutures_ = MAX_FUTURES;

    // Attempt to resume from a saved state file.
    bool resumed = LoadStateFile();

    // PERF basis?

    if (!resumed) {
      solution = SimpleFM2::ReadInputs(moviename);

      size_t start = 0;
      bool saw_input = false;
      while (start < solution.size()) {
        Commit(solution[start], "warmup");
        watermark++;
        saw_input = saw_input || solution[start] != 0;
        if (start > FASTFORWARD && saw_input) break;
        start++;
      }

      CHECK(start > 0 && "Currently, there needs to be at least "
        "one observation to score.");

      printf("Skipped %ld frames until first keypress/ffwd.\n", start);
    } else {
      printf("Resumed from checkpoint state file.\n");
    }
  }

  // PERF. Shouldn't really save every memory, but
  // we're using it for drawing SVG for now. This saves one
  // in OBSERVE_EVERY memories, and isn't truncated when we
  // backtrack.
  vector< vector<uint8> > memories;

  // Contains the movie we record (partial solution).
  vector<uint8> movie;

  // Keeps savestates.
  struct Checkpoint {
    vector<uint8> save;
    // such that truncating movie to length movenum
    // produces the savestate.
    int movenum;
    Checkpoint(const vector<uint8> save, int movenum)
      : save(save), movenum(movenum) {}
    // For putting in containers.
    Checkpoint() : movenum(0) {}
  };
  vector<Checkpoint> checkpoints;

  // Index below which we should not backtrack (because it
  // contains pre-game menu stuff, for example).
  int watermark;

  // Number of real futures to push forward.
  // XXX the more the merrier! Made this small to test backtracking.
  static const int NFUTURES = 200;

  // Number of futures that should be generated from weighted
  // motifs as opposed to totally random.
  static const int NWEIGHTEDFUTURES = 175;

  // Drop this many of the worst futures and replace them with
  // totally new futures.
  static const int DROPFUTURES = 25;
  // Drop this many of the worst futures and replace them with
  // variants on the best future.
  static const int MUTATEFUTURES = 35;

  // Number of inputs in each future.
  static const int MINFUTURELENGTH = 60;
  static const int MAXFUTURELENGTH = 1200;
  // Limit how many candidate nexts we evaluate per round. We prioritize
  // nexts derived from futures' heads and sample the rest from backfill.
  static const int MAX_NEXTS = 100;
  // Maintain at least this many nexts per round after subsampling.
  static const int MIN_NEXTS = 40;
  // Bounds for dynamic number of futures maintained.
  static const int MIN_FUTURES = 80;
  static const int MAX_FUTURES = 400;

  static const bool TRY_BACKTRACK = true;
  // Make a checkpoint this often (number of inputs).
  static const int CHECKPOINT_EVERY = 20;
  // In rounds, not inputs.
  static const int TRY_BACKTRACK_EVERY = 36;
  // In inputs.
  static const int MIN_BACKTRACK_DISTANCE = 120;

  // Observe the memory (for calibrating objectives and drawing
  // SVG) this often (number of inputs).
  static const int OBSERVE_EVERY = 10;

  // Should always be the same length as movie.
  vector<string> subtitles;

  void Commit(uint8 input, const string &message) {
    Emulator::CachingStep(input);
    movie.push_back(input);
    subtitles.push_back(message);
    if (movie.size() % CHECKPOINT_EVERY == 0) {
      vector<uint8> savestate;
      Emulator::SaveUncompressed(&savestate);
      checkpoints.push_back(Checkpoint(savestate, movie.size()));
      // Persist full state on every checkpoint.
      SaveStateFile();
    }

    // PERF: This is very slow...
    if (movie.size() % OBSERVE_EVERY == 0) {
      vector<uint8> mem;
      Emulator::GetMemory(&mem);
      memories.push_back(mem);
      objectives->Observe(mem);
    }
  }

  void Rewind(int movenum) {
    // Is it possible / meaningful to rewind stuff like objectives
    // observations?
    CHECK(movenum >= 0);
    CHECK(movenum < movie.size());
    CHECK(movie.size() == subtitles.size());
    movie.resize(movenum);
    subtitles.resize(movenum);
    // Pop any checkpoints since movenum.
    while (!checkpoints.empty() &&
	   checkpoints.back().movenum > movenum) {
      checkpoints.resize(checkpoints.size() - 1);
    }
  }

  // DESTROYS THE STATE
  void ScoreByFuture(const Future &future,
		     const vector<uint8> &base_memory,
		     vector<uint8> *base_state,
		     double *positive_scores,
		     double *negative_scores,
		     double *integral_score) {
    vector<uint8> future_memory;
    double integral = ScoreIntegral(base_state, future.inputs, &future_memory);

    *integral_score = integral / future.inputs.size();
    double pos = 0.0, neg = 0.0;
    objectives->DeltaMagnitude(base_memory, future_memory, &pos, &neg);
    *positive_scores = pos;
    *negative_scores = neg;
  }

  #if MARIONET
  static void ReadBytesFromProto(const string &pf, vector<uint8> *bytes) {
    // PERF iterators.
    for (int i = 0; i < pf.size(); i++) {
      bytes->push_back(pf[i]);
    }
  }

  void Helper(int port) {
    SingleServer server(port);

    fprintf(stderr, "[%d] " ANSI_CYAN " Ready." ANSI_RESET "\n",
	    port);

    // Cache the last few request/responses, so that we don't
    // recompute if there are connection problems. The master
    // prefers to ask the same helper again on failure.
    RequestCache cache(8);

    InPlaceTerminal term(1);
    int connections = 0;
    for (;;) {
      server.Listen();

      connections++;
      string line = StringPrintf("[%d] Connection #%d from %s",
				 port,
				 connections,
				 server.PeerString().c_str());
      term.Output(line + "\n");

      HelperRequest hreq;
      if (server.ReadProto(&hreq)) {

	if (const Message *res = cache.Lookup(hreq)) {
	  line += ", " ANSI_GREEN "cached!" ANSI_RESET;
	  term.Output(line + "\n");
	  if (!server.WriteProto(*res)) {
	    term.Advance();
	    fprintf(stderr, "Failed to send cached result...\n");
	    // keep going...
	  }

	} else if (hreq.has_playfun()) {
	  line += ", " ANSI_YELLOW "playfun" ANSI_RESET;
	  term.Output(line + "\n");
	  const PlayFunRequest &req = hreq.playfun();
	  vector<uint8> next, current_state;
	  ReadBytesFromProto(req.current_state(), &current_state);
	  ReadBytesFromProto(req.next(), &next);
	  vector<Future> futures;
	  for (int i = 0; i < req.futures_size(); i++) {
	    Future f;
	    ReadBytesFromProto(req.futures(i).inputs(), &f.inputs);
	    futures.push_back(f);
	  }

	  double immediate_score, best_future_score, worst_future_score,
	    futures_score;
	  vector<double> futurescores(futures.size(), 0.0);

	  // Do the work.
	  InnerLoop(next, futures, &current_state,
		    &immediate_score, &best_future_score,
		    &worst_future_score, &futures_score,
		    &futurescores);

	  PlayFunResponse res;
	  res.set_immediate_score(immediate_score);
	  res.set_best_future_score(best_future_score);
	  res.set_worst_future_score(worst_future_score);
	  res.set_futures_score(futures_score);
	  for (int i = 0; i < futurescores.size(); i++) {
	    res.add_futurescores(futurescores[i]);
	  }

	  // fprintf(stderr, "Result: %s\n", res.DebugString().c_str());
	  cache.Save(hreq, res);
	  if (!server.WriteProto(res)) {
	    term.Advance();
	    fprintf(stderr, "Failed to send playfun result...\n");
	    // But just keep going.
	  }
	} else if (hreq.has_tryimprove()) {
	  const TryImproveRequest &req = hreq.tryimprove();
	  line += ", " ANSI_PURPLE "tryimprove " +
	    TryImproveRequest::Approach_Name(req.approach()) +
	    ANSI_RESET;
	  term.Output(line + "\n");

	  // This thing prints.
	  term.Advance();	
	  TryImproveResponse res;
	  DoTryImprove(req, &res);

	  cache.Save(hreq, res);
	  if (!server.WriteProto(res)) {
	    term.Advance();
	    fprintf(stderr, "Failed to send tryimprove result...\n");
	    // Keep going...
	  }
	} else {
	  term.Advance();
	  fprintf(stderr, ".. unknown request??\n");
	}
      } else {
	term.Advance();
	fprintf(stderr, "\nFailed to read request...\n");
      }
      server.Hangup();
    }
  }

  template<class F, class S>
  struct CompareByFirstDesc {
    bool operator ()(const pair<F, S> &a,
		     const pair<F, S> &b) {
      return b.first < a.first;
    }
  };

  void DoTryImprove(const TryImproveRequest &req,
		    TryImproveResponse *res) {
    vector<uint8> start_state, end_state;
    ReadBytesFromProto(req.start_state(), &start_state);
    ReadBytesFromProto(req.end_state(), &end_state);
    const double end_integral = req.end_integral();

    vector<uint8> improveme;
    ReadBytesFromProto(req.improveme(), &improveme);

    // Get the memories so that we can score.
    vector<uint8> start_memory, end_memory;
    Emulator::LoadUncompressed(&end_state);
    Emulator::GetMemory(&end_memory);

    Emulator::LoadUncompressed(&start_state);
    Emulator::GetMemory(&start_memory);

    InPlaceTerminal term(1);

    vector< pair< double, vector<uint8> > > repls;

    ArcFour rc(req.seed());
    if (req.approach() == TryImproveRequest::RANDOM) {
      for (int i = 0; i < req.iters(); i++) {
	// Get a random sequence of inputs.
	vector<uint8> inputs = GetRandomInputs(&rc, improveme.size());

	// Now execute it.
	double score = 0.0;
	if (IsImprovement(&term, (double)i / req.iters(),
			  &start_state,
			  start_memory,
			  inputs,
			  end_memory, end_integral, &score)) {
	  term.Advance();
	  fprintf(stderr, "Improved! %f\n", score);
	  repls.push_back(make_pair(score, inputs));
	}
      }
    } else if (req.approach() == TryImproveRequest::OPPOSITES) {
      vector<uint8> inputs = improveme;

      TryDualizeAndReverse(&term, 0,
			   &start_state, start_memory,
			   &inputs, 0, inputs.size(),
			   end_memory, end_integral, &repls,
			   false);

      TryDualizeAndReverse(&term, 0,
			   &start_state, start_memory,
			   &inputs, 0, inputs.size() / 2,
			   end_memory, end_integral, &repls,
			   false);

      for (int i = 0; i < req.iters(); i++) {
	int start, len;
	GetRandomSpan(inputs, 1.0, &rc, &start, &len);
	if (len == 0 && start != inputs.size()) len = 1;
	bool keepreversed = rc.Byte() & 1;

	// XXX Note, does nothing when len = 0.
	TryDualizeAndReverse(&term, (double)i / req.iters(),
			     &start_state, start_memory,
			     &inputs, start, len,
			     end_memory, end_integral, &repls,
			     keepreversed);
      }

    } else if (req.approach() == TryImproveRequest::ABLATION) {
      for (int i = 0; i < req.iters(); i++) {
	vector<uint8> inputs = improveme;
	uint8 mask;
	// No sense in getting a mask that keeps everything.
	do { mask = rc.Byte(); } while (mask == 255);
	uint32 cutoff = RandomInt32(&rc);
	for (int j = 0; j < inputs.size(); j++) {
	  if (RandomInt32(&rc) < cutoff) {
	    inputs[j] &= mask;
	  }
	}

	// Might have chosen a mask on e.g. SELECT, which is
	// never in the input.
	double score = 0.0;
	if (inputs != improveme &&
	    IsImprovement(&term, (double)i / req.iters(),
			  &start_state,
			  start_memory,
			  inputs,
			  end_memory, end_integral, &score)) {
	  term.Advance();
	  fprintf(stderr, "Improved (abl %d)! %f\n", mask, score);
	  repls.push_back(make_pair(score, inputs));
	}
      }
    } else if (req.approach() == TryImproveRequest::CHOP) {
      set< vector<uint8> > tried;

      for (int i = 0; i < req.iters(); i++) {
	vector<uint8> inputs = improveme;

	// We allow using iterations to chop more from the thing
	// we just chopped, if it was an improvement.
	int depth = 0;
	for (; i < req.iters(); i++, depth++) {
	  int start, len;
	  // Use exponent of 2 (prefer smaller spans) because
	  // otherwise chopping is quite blunt.
	  GetRandomSpan(inputs, 2.0, &rc, &start, &len);
	  if (len == 0 && start != inputs.size()) len = 1;

	  ChopOut(&inputs, start, len);
	  double score = 0.0;
	  if (inputs != improveme &&
	      IsImprovement(&term, (double) i / req.iters(),
			    &start_state, start_memory,
			    inputs,
			    end_memory, end_integral,
			    &score)) {
	    term.Advance();
	    fprintf(stderr, "Improved (chop %d for %d depth %d)! %f\n",
		    start, len, depth, score);
	    repls.push_back(make_pair(score, inputs));

	    // If we already tried this one, don't do it again.
	    if (tried.find(inputs) == tried.end()) {
	      tried.insert(inputs);
	    } else {
	      // Don't keep chopping.
	      break;
	    }
	  } else {
	    tried.insert(inputs);
	    // Don't keep chopping.
	    break;
	  }
        }
      }
    }

    const int nimproved = repls.size();

    if (repls.size() > req.maxbest()) {
      std::sort(repls.begin(), repls.end(),
		CompareByFirstDesc< double, vector<uint8> >());
      repls.resize(req.maxbest());
    }

    for (int i = 0; i < repls.size(); i++) {
      res->add_inputs(&repls[i].second[0], repls[i].second.size());
      res->add_score(repls[i].first);
    }

    // XXX I think that some can produce more than iters outputs,
    // so better could be greater than 100%. 
    res->set_iters_tried(req.iters());
    res->set_iters_better(nimproved);

    term.Advance();
    fprintf(stderr, "In %d iters (%s), %d were improvements (%.1f%%)\n",
	    req.iters(),
	    TryImproveRequest::Approach_Name(req.approach()).c_str(),
	    nimproved, (100.0 * nimproved) / req.iters());
  }

  // Exponent controls the length of the span. Large exponents
  // yield smaller spans. Note that this will return empty spans.
  void GetRandomSpan(const vector<uint8> &inputs, double exponent,
		     ArcFour *rc, int *start, int *len) {
    *start = RandomDouble(rc) * inputs.size();
    if (*start < 0) *start = 0;
    if (*start >= inputs.size()) *start = inputs.size() - 1;
    int maxlen = inputs.size() - *start;
    double d = pow(RandomDouble(rc), exponent);
    *len = d * maxlen;
    if (*len < 0) *len = 0;
    if (*len >= maxlen) *len = maxlen;
  }

  void ChopOut(vector<uint8> *inputs, int start, int len) {
    inputs->erase(inputs->begin() + start, inputs->begin() + start + len);
  }

  void TryDualizeAndReverse(InPlaceTerminal *term, double frac,
			    vector<uint8> *start_state,
			    const vector<uint8> &start_memory,
			    vector<uint8> *inputs, int startidx, int len,
			    const vector<uint8> &end_memory,
			    double end_integral,
			    vector< pair< double, vector<uint8> > > *repls,
			    bool keepreversed) {

    Dualize(inputs, startidx, len);
    double score = 0.0;
    if (IsImprovement(term, frac,
		      start_state,
		      start_memory,
		      *inputs,
		      end_memory, end_integral,
		      &score)) {
      term->Advance();
      fprintf(stderr, "Improved! %f\n", score);
      repls->push_back(make_pair(score, *inputs));
    }

    ReverseRange(inputs, startidx, len);

    if (IsImprovement(term, frac,
		      start_state,
		      start_memory,
		      *inputs,
		      end_memory, end_integral,
		      &score)) {
      term->Advance();
      fprintf(stderr, "Improved (rev)! %f\n", score);
      repls->push_back(make_pair(score, *inputs));
    }

    if (!keepreversed) {
      ReverseRange(inputs, startidx, len);
    }
  }

  static void ReverseRange(vector<uint8> *v, int start, int len) {
    CHECK(start >= 0);
    CHECK((start + len) <= v->size());
    int i = start, j = start + len - 1;
    while (i < j) {
      uint8 tmp = (*v)[i];
      (*v)[i] = (*v)[j];
      (*v)[j] = tmp;
      i++; j--;
    }
  }

  static void Dualize(vector<uint8> *v, int start, int len) {
    CHECK(start >= 0);
    CHECK((start + len) <= v->size());
    for (int i = 0; i < len; i++) {
      uint8 input = (*v)[start + i];
      uint8 r = !!(input & INPUT_R);
      uint8 l = !!(input & INPUT_L);
      uint8 d = !!(input & INPUT_D);
      uint8 u = !!(input & INPUT_U);
      uint8 t = !!(input & INPUT_T);
      uint8 s = !!(input & INPUT_S);
      uint8 b = !!(input & INPUT_B);
      uint8 a = !!(input & INPUT_A);

      uint8 newinput = 0;
      if (r) newinput |= INPUT_L;
      if (l) newinput |= INPUT_R;
      if (d) newinput |= INPUT_U;
      if (u) newinput |= INPUT_D;
      if (t) newinput |= INPUT_S;
      if (s) newinput |= INPUT_T;
      if (b) newinput |= INPUT_A;
      if (a) newinput |= INPUT_B;

      (*v)[start + i] = newinput;
    }
  }

  // Computes the score as the sum of the scores of each step over the
  // input. You might want to normalize the score by the input length,
  // if comparing inputs of different length. Also swaps in the
  // final memory if non-NULL.
  double ScoreIntegral(vector<uint8> *start_state,
		       const vector<uint8> &inputs,
		       vector<uint8> *final_memory) {
    Emulator::LoadUncompressed(start_state);
    vector<uint8> previous_memory;
    Emulator::GetMemory(&previous_memory);
    double sum = 0.0;
    for (int i = 0; i < inputs.size(); i++) {
      Emulator::CachingStep(inputs[i]);
      vector<uint8> new_memory;
      Emulator::GetMemory(&new_memory);
  sum += objectives->EvaluateMagnitude(previous_memory, new_memory);
      previous_memory.swap(new_memory);
    }
    if (final_memory != NULL) {
      final_memory->swap(previous_memory);
    }
    return sum;
  }

  // Note that this does NOT normalize the integral scores by their
  // length, with the idea that we only make the input shorter, so
  // they tend to be disadvantaged (unless the paths are net negative,
  // in which case a trend towards shorter is desirable). If we had
  // an approach that increased the length of sequences, we would need
  // to be careful with this function.
  bool IsImprovement(InPlaceTerminal *term, double frac,
		     vector<uint8> *start_state,
		     const vector<uint8> &start_memory,
		     const vector<uint8> &inputs,
		     const vector<uint8> &end_memory,
		     double end_integral,
		     double *score) {
    vector<uint8> new_memory;
    double new_integral = ScoreIntegral(start_state, inputs, &new_memory);

    //           end_integral
    //                     ....----> end
    //         ....----````           |
    //    start                       |  n_minus_e
    //         ````----....           v
    //                     ````----> new
    //           new_integral
    //
    // The _integral scores are comparing the path integrals from start
    // to end or new. We have intermediate states for these so we can
    // compute integrals with the thought that those are more accurate.
    // n_minus_e is comparing end and new directly; we don't know a path
    // between those memories so this is the only option.

  double n_minus_e = objectives->EvaluateMagnitude(end_memory, new_memory);

    if (term != NULL) {
      string msg =
	StringPrintf("%2.f%%  Send %f  Snew %f  n-e %f\n",
		     100.0 * frac,
		     end_integral, new_integral, n_minus_e);
      term->Output(msg);
    }

    // Path integral is worse.
    if (end_integral > new_integral) return false;

    // Not actually an improvement over start (note that
    // end was even worse, though. maybe should consider
    // taking this anyway).
    if (new_integral <= 0) return false;

    // End is a better state from our perspective.
    if (n_minus_e <= 0) return false;

    *score = (new_integral - end_integral) + n_minus_e;
    return true;
  }

  // Old version, no integration.
  bool IsImprovementTriangle(InPlaceTerminal *term, double frac,
			     vector<uint8> *start_state,
			     const vector<uint8> &start_memory,
			     const vector<uint8> &inputs,
			     const vector<uint8> &end_memory,
			     double *score) {
    Emulator::LoadUncompressed(start_state);
    for (int i = 0; i < inputs.size(); i++) {
      Emulator::CachingStep(inputs[i]);
    }

    vector<uint8> new_memory;
    Emulator::GetMemory(&new_memory);

    //               e_minus_s
    //                     ....----> end
    //         ....----````           |
    //    start                       |  n_minus_e
    //         ````----....           v
    //                     ````----> new
    //                n_minus_s
    //
    // Success if the new memory is an improvement over the
    // start state, an improvement over the end state, and
    // a bigger improvement over the start state than the end
    // state is.
  double e_minus_s = objectives->EvaluateMagnitude(start_memory, end_memory);
  double n_minus_s = objectives->EvaluateMagnitude(start_memory, new_memory);
  double n_minus_e = objectives->EvaluateMagnitude(end_memory, new_memory);

    if (term != NULL) {
      string msg =
	StringPrintf("%2.f%%  e-s %f  n-s %f  n-e %f\n",
		     100.0 * frac,
		     e_minus_s, n_minus_s, n_minus_e);
      term->Output(msg);
    }

    // Old way was better improvement than new way.
    if (e_minus_s >= n_minus_s) return false;
    // Not actually an improvement over start (note that
    // end was even worse, though...)
    if (n_minus_s <= 0) return false;
    // End is a better state from our perspective.
    if (n_minus_e <= 0) return false;

    // All scores have the same e_minus_s component, so ignore that.
    *score = n_minus_e + n_minus_s;
    return true;
  }

  vector<uint8> GetRandomInputs(ArcFour *rc, int len) {
    vector<uint8> inputs;
    while(inputs.size() < len) {
      const vector<uint8> &m =
	motifs->RandomWeightedMotifWith(rc);

      for (int x = 0; x < m.size(); x++) {
	inputs.push_back(m[x]);
	if (inputs.size() == len) {
	  break;
	}
      }
    }
    return inputs;
  }
  #endif


  void InnerLoop(const vector<uint8> &next,
		 const vector<Future> &futures_orig,
		 vector<uint8> *current_state,
		 double *immediate_score,
		 double *best_future_score,
		 double *worst_future_score,
		 double *futures_score,
		 vector<double> *futurescores) {

    // Make copy so we can make fake futures.
    vector<Future> futures = futures_orig;

    Emulator::LoadUncompressed(current_state);

    vector<uint8> current_memory;
    Emulator::GetMemory(&current_memory);

    // Take steps.
    for (int j = 0; j < next.size(); j++)
      Emulator::CachingStep(next[j]);

    vector<uint8> new_memory;
    Emulator::GetMemory(&new_memory);

    vector<uint8> new_state;
    Emulator::SaveUncompressed(&new_state);

    // Used to be BuggyEvaluate = WeightedLess? XXX
  *immediate_score = objectives->EvaluateMagnitude(current_memory, new_memory);

    // PERF unused except for drawing
    // XXX probably shouldn't do this since it depends on local
    // storage.
    // double norm_score = objectives->GetNormalizedValue(new_memory);

    *best_future_score = -1e80;
    *worst_future_score = 1e80;


    // XXX reconsider whether this is really useful
    {
      // Synthetic future where we keep holding the last
      // button pressed.
      // static const int NUM_FAKE_FUTURES = 1;
      int total_future_length = 0;
      for (int i = 0; i < futures.size(); i++) {
	total_future_length += futures[i].inputs.size();
      }

      const int average_future_length = (int)((double)total_future_length /
					      (double)futures.size());
      
      Future fakefuture_hold;
      for (int z = 0; z < average_future_length; z++) {
	fakefuture_hold.inputs.push_back(next.back());
      }
      futures.push_back(fakefuture_hold);
    }

    *futures_score = 0.0;
    for (int f = 0; f < futures.size(); f++) {
      if (f != 0) Emulator::LoadUncompressed(&new_state);
      double positive_scores, negative_scores, integral_score;
      ScoreByFuture(futures[f], new_memory, &new_state,
		    &positive_scores, &negative_scores,
		    &integral_score);
      CHECK(positive_scores >= 0);
      CHECK(negative_scores <= 0);

      // For scoring the futures themselves (pruning and duplicating),
      // we want to disprefer futures that kill the player or get
      // stuck or whatever. So count both the positive and negative
      // components, plus the normalized integral.
      if (f < futures_orig.size()) {
	(*futurescores)[f] += integral_score +
	  positive_scores + negative_scores;
      }

      // TODO: I think maybe a better idea is to use the max over
      // all futures? It's appropriate to be optimistic, but we
      // shouldn't ignore components of the objective that go down,
      // because we have to take them together. But integral_score
      // probably dominates, here, so maybe positive_scores is a
      // good tie-breaker?

  // Include both positive and negative components, and the full
  // integral (even when negative). This aligns selection with the
  // pruning metric used for futures themselves.
  double future_score = positive_scores + negative_scores + integral_score;

      *futures_score += future_score;

      // Unused except for diagnostics.
      if (future_score > *best_future_score)
	*best_future_score = future_score;
      if (future_score < *worst_future_score)
	*worst_future_score = future_score;
    }

    // Discards the copy.
    // futures.resize(futures.size() - NUM_FAKE_FUTURES);
  }

  // The parallel step. We either run it in serial locally
  // (without MARIONET) or as jobs on helpers, via TCP.
  void ParallelStep(const vector< vector<uint8> > &nexts,
		    const vector<Future> &futures,
		    // morally const
		    vector<uint8> *current_state,
		    const vector<uint8> &current_memory,
		    vector<double> *futuretotals,
        int *best_next_idx,
        double *out_best_score) {
    uint64 start_time = time(NULL);
    fprintf(stderr, "Parallel step with %d nexts, %d futures.\n",
	    nexts.size(), futures.size());
    CHECK(nexts.size() > 0);
    *best_next_idx = 0;

  double best_score = 0.0;
    Scoredist distribution(movie.size());

#if MARIONET
    // One piece of work per request.
    vector<HelperRequest> requests;
    requests.resize(nexts.size());
    for (int i = 0; i < nexts.size(); i++) {
      PlayFunRequest *req = requests[i].mutable_playfun();
      req->set_current_state(&((*current_state)[0]), current_state->size());
      req->set_next(&nexts[i][0], nexts[i].size());
      for (int f = 0; f < futures.size(); f++) {
	FutureProto *fp = req->add_futures();
	fp->set_inputs(&futures[f].inputs[0],
		       futures[f].inputs.size());
      }
      // if (!i) fprintf(stderr, "REQ: %s\n", req->DebugString().c_str());
    }

    GetAnswers<HelperRequest, PlayFunResponse> getanswers(ports_, requests);
    getanswers.Loop();

    const vector<GetAnswers<HelperRequest, PlayFunResponse>::Work> &work =
      getanswers.GetWork();

    for (int i = 0; i < work.size(); i++) {
      const PlayFunResponse &res = work[i].res;
      for (int f = 0; f < res.futurescores_size(); f++) {
    CHECK(f < futuretotals->size());
    (*futuretotals)[f] += res.futurescores(f);
      }

      const double score = res.immediate_score() + res.futures_score();

      distribution.immediates.push_back(res.immediate_score());
      distribution.positives.push_back(res.futures_score());
      distribution.negatives.push_back(res.worst_future_score());
      // XXX norm score is disabled because it can't be
      // computed in a distributed fashion.
      distribution.norms.push_back(0);

      if (score > best_score) {
	best_score = score;
	*best_next_idx = i;
      }
    }

#else
    // Local version: parallelize across nexts when available.
    struct LocalRes { double immed, futsum, worst; vector<double> futscores; };
    vector<LocalRes> local(nexts.size());

    #pragma omp parallel for schedule(dynamic) if(nexts.size() > 1)
    for (int i = 0; i < (int)nexts.size(); i++) {
      double immediate_score = 0.0, best_future_score = 0.0, worst_future_score = 0.0,
             futures_score = 0.0;
      vector<double> futurescores(futures.size(), 0.0);
      // Use a thread-local copy of current_state; InnerLoop loads it as needed.
      vector<uint8> tl_state = *current_state;
      InnerLoop(nexts[i], futures, &tl_state,
                &immediate_score, &best_future_score, &worst_future_score,
                &futures_score, &futurescores);
      local[i].immed = immediate_score;
      local[i].futsum = futures_score;
      local[i].worst = worst_future_score;
      local[i].futscores.swap(futurescores);
    }

    for (int i = 0; i < (int)local.size(); i++) {
      for (int f = 0; f < (int)local[i].futscores.size(); f++) {
        (*futuretotals)[f] += local[i].futscores[f];
      }
      double score = local[i].immed + local[i].futsum;
      distribution.immediates.push_back(local[i].immed);
      distribution.positives.push_back(local[i].futsum);
      distribution.negatives.push_back(local[i].worst);
      distribution.norms.push_back(0);
      if (score > best_score) { best_score = score; *best_next_idx = i; }
    }
#endif
    distribution.chosen_idx = *best_next_idx;
    distributions.push_back(distribution);

  if (out_best_score) *out_best_score = best_score;
  uint64 end_time = time(NULL);
    fprintf(stderr, "Parallel step took %d seconds.\n",
	    (int)(end_time - start_time));
  }

  void PopulateFutures(vector<Future> *futures) {
    int num_currently_weighted = 0;
    for (int i = 0; i < futures->size(); i++) {
      if ((*futures)[i].weighted) {
	num_currently_weighted++;
      }
    }

    int num_to_weight = max(NWEIGHTEDFUTURES - num_currently_weighted, 0);
    #ifdef DEBUGFUTURES
    fprintf(stderr, "there are %d futures, %d cur weighted, %d need\n",
	    futures->size(), num_currently_weighted, num_to_weight);
    #endif
  while ((int)futures->size() < nfutures_) {
      // Keep the desired length around so that we only
      // resize the future if we drop it. Randomize between
      // MIN and MAX future lengths.
      int flength = MINFUTURELENGTH +
	(int)
	((double)(MAXFUTURELENGTH - MINFUTURELENGTH) *
	 RandomDouble(&rc));

      if (num_to_weight > 0) {
	futures->push_back(Future(true, flength));
	num_to_weight--;
      } else {
	futures->push_back(Future(false, flength));
      }
    }

    // Make sure we have enough futures with enough data in.
    // PERF: Should avoid creating exact duplicate futures.
  for (int i = 0; i < nfutures_ && i < futures->size(); i++) {
      while ((*futures)[i].inputs.size() <
	     (*futures)[i].desired_length) {
	const vector<uint8> &m =
	  (*futures)[i].weighted ?
	  motifs->RandomWeightedMotif() :
	  motifs->RandomMotif();
	for (int x = 0; x < m.size(); x++) {
	  (*futures)[i].inputs.push_back(m[x]);
	  if ((*futures)[i].inputs.size() ==
	      (*futures)[i].desired_length) {
	    break;
	  }
	}
      }
    }

    #ifdef DEBUGFUTURES
    for (int f = 0; f < futures->size(); f++) {
      fprintf(stderr, "%d. %s %d/%d: ...\n",
	      f, (*futures)[f].weighted ? "weighted" : "random",
	      (*futures)[f].inputs.size(),
	      (*futures)[f].desired_length);
    }
    #endif
  }

  Future MutateFuture(const Future &input) {
    Future out;
    out.is_mutant = true;
    out.weighted = input.weighted;
    if ((rc.Byte() & 7) == 0) out.weighted = !out.weighted;
    out.inputs = input.inputs;

    out.desired_length = input.desired_length;

    // Replace tail with something random.
    out.inputs.resize(max(MINFUTURELENGTH, input.desired_length / 2));

    // Occasionally, try something very different.
    if ((rc.Byte() & 7) == 0) {
      Dualize(&out.inputs, 0, out.inputs.size());
    }
    // TODO: More interesting mutations here (chop, ablate, reverse..)

    return out;
  }

  // Consider every possible next step along with every possible
  // future. Commit to the step that has the best score among
  // those futures. Remove the futures that didn't perform well
  // overall, and replace them. Reweight motifs according... XXX
  void TakeBestAmong(const vector< vector<uint8> > &nexts,
		     const vector<string> &nextplanations,
		     vector<Future> *futures,
       bool chopfutures,
       double *out_best_score) {
    vector<uint8> current_state;
    vector<uint8> current_memory;

    if ((int)futures->size() != nfutures_) {
      fprintf(stderr, "Note: futures target %d but current %d.\n", nfutures_, (int)futures->size());
    }

    // Save our current state so we can try many different branches.
    Emulator::SaveUncompressed(&current_state);
    Emulator::GetMemory(&current_memory);

    // Total score across all motifs for each future.
    vector<double> futuretotals(futures->size(), 0.0);

    // Most of the computation happens here.
    int best_next_idx = -1;
    ParallelStep(nexts, *futures,
		 &current_state, current_memory,
		 &futuretotals,
		 &best_next_idx,
		 out_best_score);
    CHECK(best_next_idx >= 0);
    CHECK(best_next_idx < nexts.size());

    // Adapt future lengths: when a future performs well overall,
    // increase its desired length; otherwise, shorten it. This lets
    // us have more, shorter futures when things look bad, and fewer,
    // longer futures when the outlook is good.
    for (int i = 0; i < futures->size(); i++) {
      int cur = (*futures)[i].desired_length;
      // Scale step ~10% of current length, min 1.
      int step = cur / 10; if (step < 1) step = 1;
      if (futuretotals[i] > 0) cur = min(MAXFUTURELENGTH, cur + step);
      else cur = max(MINFUTURELENGTH, cur - step);
      (*futures)[i].desired_length = cur;
    }

    // Adjust number of futures based on fraction of positive totals.
    int positives = 0;
    for (int i = 0; i < futuretotals.size(); i++) if (futuretotals[i] > 0) positives++;
    double frac_pos = futuretotals.empty() ? 0.0 : (double)positives / (double)futuretotals.size();
    int delta = std::max(1, nfutures_ / 20); // 5% step
    if (frac_pos < 0.4 && nfutures_ < MAX_FUTURES) nfutures_ = std::min(MAX_FUTURES, nfutures_ + delta);
    else if (frac_pos > 0.6 && nfutures_ > MIN_FUTURES) nfutures_ = std::max(MIN_FUTURES, nfutures_ - delta);

    if (chopfutures) {
      // fprintf(stderr, "Chop futures.\n");
      // Chop the head off each future.
      const int choplength = nexts[best_next_idx].size();
      for (int i = 0; i < futures->size(); i++) {
	vector<uint8> newf;
	for (int j = choplength; j < (*futures)[i].inputs.size(); j++) {
	  newf.push_back((*futures)[i].inputs[j]);
	}
	(*futures)[i].inputs.swap(newf);
      }
    }

    // XXX: Don't drop the future if it was the one we got the
    // max() score for. Right? It might have had very poor scores
    // otherwise, but we might be relying on it existing.
    // TODO: Consider duplicating the future that we got the max()
    // score from.

    // Discard the futures with the worst total.
    // They'll be replaced the next time around the loop.
    // PERF don't really need to make DROPFUTURES passes,
    // but there are not many futures and not many dropfutures.
    static const int TOTAL_TO_DROP = DROPFUTURES + MUTATEFUTURES;
    for (int t = 0; t < TOTAL_TO_DROP; t++) {
      // fprintf(stderr, "Drop futures (%d/%d).\n", t, DROPFUTURES);
      CHECK(!futures->empty());
      CHECK(futures->size() <= futuretotals.size());
      double worst_total = futuretotals[0];
      int worst_idx = 0;
      for (int i = 1; i < futures->size(); i++) {
  if (futuretotals[i] < worst_total) {
    worst_total = futuretotals[i];
    worst_idx = i;
  }
      }

      // Delete it by swapping.
      if (worst_idx != futures->size() - 1) {
	(*futures)[worst_idx] = (*futures)[futures->size() - 1];
	// Also swap in the futuretotals so the scores match.
	// This was a bug before -- it always dropped the lowest
	// scoring one and then the tail of the array (because this
	// slot would still have the lowest score).
	futuretotals[worst_idx] = futuretotals[futures->size() - 1];
      }
      futures->resize(futures->size() - 1);
    }

    // Now get the future with the best score.
    CHECK(!futures->empty());
    int best_future_idx = 0;
    double best_future_score = futuretotals[0];
    for (int i = 1; i < futures->size(); i++) {
      if (futuretotals[i] > best_future_score) {
	best_future_score = futuretotals[i];
	best_future_idx = i;
      }
    }

    for (int t = 0; t < MUTATEFUTURES; t++) {
      futures->push_back(MutateFuture((*futures)[best_future_idx]));
    }

    if ((int)futures->size() > nfutures_) {
      futures->resize(nfutures_);
    }

    // If in single mode, this is probably cached, but with
    // MARIONET this is usually a full replay.
    // fprintf(stderr, "Replay %d moves\n", nexts[best_next_idx].size());
    Emulator::LoadUncompressed(&current_state);
    for (int j = 0; j < nexts[best_next_idx].size(); j++) {
      Commit(nexts[best_next_idx][j], nextplanations[best_next_idx]);
    }

    // Now, if the motif we used was a local improvement to the
    // score, reweight it.
    // This should be a motif in the normal case where we're trying
    // each motif, but when we use this to implement the best
    // backtrack plan, it usually won't be.
    if (motifs->IsMotif(nexts[best_next_idx])) {
      double total = motifs->GetTotalWeight();
      motifs->Pick(nexts[best_next_idx]);
      vector<uint8> new_memory;
      Emulator::GetMemory(&new_memory);
      double oldval = objectives->GetNormalizedValue(current_memory);
      double newval = objectives->GetNormalizedValue(new_memory);
      double *weight = motifs->GetWeightPtr(nexts[best_next_idx]);
      // Already checked it's a motif.
      CHECK(weight != NULL);
      if (newval > oldval) {
	// Increases its weight.
	double d = *weight / MOTIF_ALPHA;
	if (d / total < MOTIF_MAX_FRAC) {
	  *weight = d;
	} else {
	  fprintf(stderr, "motif is already at max frac: %.2f\n", d);
	}
      } else {
	// Decreases its weight.
	double d = *weight * MOTIF_ALPHA;
	if (d / total > MOTIF_MIN_FRAC) {
	  *weight = d;
	} else {
	  fprintf(stderr, "motif is already at min frac: %f\n", d);
	}
      }
    }

    PopulateFutures(futures);
  }

  // Main loop for the master, or when compiled without MARIONET support.
  // Helpers is an array of helper ports, which is ignored unless MARIONET
  // is active.
  void Master(const vector<int> &helpers) {
    // XXX
    ports_ = helpers;

    string logname = StringPrintf("%s-log.html", game.c_str());
    log = fopen(logname.c_str(), "w");
    CHECK(log != NULL);
    fprintf(log,
	    "<!DOCTYPE html>\n"
	    "<link rel=\"stylesheet\" href=\"log.css\" />\n"
	    "<h1>%s started at %s %s.</h1>\n",
	    game.c_str(),
	    DateString(time(NULL)).c_str(),
	    TimeString(time(NULL)).c_str());
    fflush(log);

#if 0
    vector< vector<uint8> > nexts = motifvec;
    vector<string> nextplanations;
    for (int i = 0; i < nexts.size(); i++) {
      nextplanations.push_back(StringPrintf("motif %d:%d",
					     i, nexts[i].size()));
    }
    // XXX...
    for (int i = 0; i < nexts.size(); i++) {
      for (int j = 0; j < nexts[i].size(); j++) {
	nexts[i][j] &= INPUTMASK;
      }
    }
#endif

    fprintf(stderr, "[MASTER] Beginning " 
	    ANSI_YELLOW "%s" ANSI_RESET ".\n", game.c_str());

    // This version of the algorithm looks like this. At some point in
    // time, we have the set of motifs we might play next. We'll
    // evaluate all of those. We also have a series of possible
    // futures that we're considering. At each step we play our
    // candidate motif (ignoring that many steps as in the future --
    // but note that for each future, there should be some motif that
    // matches its head). Then we play all the futures. The motif with the
    // best overall score is chosen; we chop the head off each future,
    // and add a random motif to its end.
    // (XXX docs are inaccurate now)
    // XXX recycling futures...
    vector<Future> futures;

    int rounds_until_backtrack = TRY_BACKTRACK_EVERY;
    int64 iters = 0;

  PopulateFutures(&futures);
  int low_rounds = 0;  // consecutive low-score rounds
    for (;; iters++) {

      // XXX TODO this probably gets confused by backtracking.
      motifs->Checkpoint(movie.size());

      vector< vector<uint8> > nexts;
      vector<string> nextplanations;
      MakeNexts(futures, &nexts, &nextplanations);
      // Subsample nexts to keep evaluation tractable. Target based on nfutures_.
      int before = nexts.size();
      int target_nexts = std::min(MAX_NEXTS, std::max(MIN_NEXTS, nfutures_));
      SubsampleNexts(&nexts, &nextplanations, target_nexts);
      if (nexts.size() != before) {
        fprintf(stderr, "Subsampled nexts %d -> %d (target %d)\n", before, (int)nexts.size(), target_nexts);
      }

  double round_best = 0.0;
  TakeBestAmong(nexts, nextplanations, &futures, true, &round_best);
  // Log dynamic futures and average desired length.
  double avg_len = 0.0;
  for (int i = 0; i < futures.size(); i++) avg_len += futures[i].desired_length;
  if (!futures.empty()) avg_len /= futures.size();
  fprintf(stderr, "Futures: %d  Avg desired_length: %.1f\n", (int)futures.size(), avg_len);

      fprintf(stderr, "%d rounds, "
	      ANSI_WHITE "%d inputs" ANSI_RESET ". backtrack in %d. "
	      "Cxpoints at ",
	      iters, movie.size(), rounds_until_backtrack);

      for (int i = 0, j = checkpoints.size() - 1; i < 3 && j >= 0; i++) {
	fprintf(stderr, "%d, ", checkpoints[j].movenum);
	j--;
      }
      fprintf(stderr, "...\n");

      // Early backtrack if we seem stuck (no good options) for a while.
      if (round_best < 0.0) low_rounds++; else low_rounds = 0;
      int stuck_threshold_rounds = max(1, TRY_BACKTRACK_EVERY / 2);
      if (low_rounds >= stuck_threshold_rounds) {
        fprintf(stderr, ANSI_YELLOW "Stuck detection: forcing backtrack consideration." ANSI_RESET "\n");
        rounds_until_backtrack = 0;
        low_rounds = 0;
      }

      MaybeBacktrack(iters, &rounds_until_backtrack, &futures);

      if (iters % 10 == 0) {
	SaveMovie();
	SaveQuickDiagnostics(futures);
	if (iters % 50 == 0) {
	  SaveDiagnostics(futures);
	}
      }
    }
  }

  // Make the nexts that we should try for this round.
  void MakeNexts(const vector<Future> &futures,
		 vector< vector<uint8> > *nexts,
		 vector<string> *nextplanations) {

    // Note that backfill motifs are not necessarily this length.
    static const int INPUTS_PER_NEXT = 10;

    map< vector<uint8>, string > todo;
    for (int i = 0; i < futures.size(); i++) {
      if (futures[i].inputs.size() >= INPUTS_PER_NEXT) {
	vector<uint8> nf(futures[i].inputs.begin(),
			 futures[i].inputs.begin() + INPUTS_PER_NEXT);
	if (todo.find(nf) == todo.end()) {
	  todo.insert(make_pair(nf, StringPrintf("ftr-%d", i)));
	}
      }
    }

    // There may be duplicates (typical, in fact). Insert motifs
    // as long as we can.
  while ((int)todo.size() < std::max(nfutures_, MAX_NEXTS)) {
      const vector<uint8> *motif = motifs->RandomWeightedMotifNotIn(todo);
      if (motif == NULL) {
	fprintf(stderr, "No more motifs (have %d todo).\n", todo.size());
	break;
      }
	
      todo.insert(make_pair(*motif, "backfill"));
    }

    // Now populate nexts and explanations.
    nexts->clear();
    nextplanations->clear();
    for (map< vector<uint8>, string >::const_iterator it = todo.begin();
	 it != todo.end(); ++it) {
      nexts->push_back(it->first);
      nextplanations->push_back(it->second);
    }
  }

  // Prefer futures-derived nexts, then sample remainder from backfill.
  void SubsampleNexts(vector< vector<uint8> > *nexts,
                      vector<string> *nextplanations,
                      int target) {
    if ((int)nexts->size() <= target) return;
    vector<int> fut_idx, back_idx;
    fut_idx.reserve(nexts->size());
    back_idx.reserve(nexts->size());
    for (int i = 0; i < nextplanations->size(); i++) {
      if ((*nextplanations)[i].size() >= 4 &&
          (*nextplanations)[i].substr(0, 4) == "ftr-") fut_idx.push_back(i);
      else back_idx.push_back(i);
    }
    // Shuffle indices using rc for randomness (Fisher-Yates via Swap loop).
    auto shuffle_idx = [this](vector<int> *v) {
      for (int i = 0; i < v->size(); i++) {
        int j = (int)(RandomDouble(&rc) * v->size());
        if (j < 0) j = 0;
        if (j >= v->size()) j = v->size() - 1;
        if (i != j) std::swap((*v)[i], (*v)[j]);
      }
    };
    shuffle_idx(&fut_idx);
    shuffle_idx(&back_idx);

  int take_fut = std::min((int)fut_idx.size(), (target + 1) / 2);
    vector<int> chosen;
    chosen.reserve(target);
    for (int i = 0; i < take_fut; i++) chosen.push_back(fut_idx[i]);
    int remaining = target - chosen.size();
    for (int i = 0; i < back_idx.size() && remaining > 0; i++, remaining--) {
      chosen.push_back(back_idx[i]);
    }
    // If still not enough (e.g., few backfills), pull more futures.
    for (int i = take_fut; remaining > 0 && i < fut_idx.size(); i++, remaining--) {
      chosen.push_back(fut_idx[i]);
    }
    // Rebuild vectors in chosen order.
    vector< vector<uint8> > nn;
    vector<string> np;
    nn.reserve(chosen.size()); np.reserve(chosen.size());
    for (int idx : chosen) { nn.push_back((*nexts)[idx]); np.push_back((*nextplanations)[idx]); }
    nexts->swap(nn);
    nextplanations->swap(np);
  }

  void TryImprove(Checkpoint *start,
		  const vector<uint8> &improveme,
		  const vector<uint8> &current_state,
		  vector<Replacement> *replacements,
		  double *improvability) {

    uint64 start_time = time(NULL);
    fprintf(stderr, "TryImprove step on %d inputs.\n",
	    improveme.size());
    CHECK(replacements);
    replacements->clear();

    const double current_integral =
      ScoreIntegral(&start->save, improveme, NULL);

    fprintf(log, "<li>Trying to improve frames %d&ndash;%d, %f</li>\n",
      start->movenum, (int)movie.size(), current_integral);

    static const int MAXBEST = 10;

    // For random, we could compute the right number of
    // tasks based on the number of helpers...
    static const int NUM_IMPROVE_RANDOM = 10;
    static const int RANDOM_ITERS = 200;

    static const int NUM_ABLATION = 10;
    static const int ABLATION_ITERS = 200;

    static const int NUM_CHOP = 10;
    static const int CHOP_ITERS = 200;

    // Note that some of these have a fixed number
    // of iterations that are tried, independent of
    // the iters field. So try_opposites = true and
    // opposites_ites = 0 does make sense.
    static const bool TRY_OPPOSITES = true;
    static const int OPPOSITES_ITERS = 200;


    #ifdef MARIONET

    // One piece of work per request.
    vector<HelperRequest> requests;

    // Every request shares this stuff.
    TryImproveRequest base_req;
    base_req.set_start_state(&start->save[0], start->save.size());
    base_req.set_improveme(&improveme[0], improveme.size());
    base_req.set_end_state(&current_state[0], current_state.size());
    base_req.set_end_integral(current_integral);
    base_req.set_maxbest(MAXBEST);

    if (TRY_OPPOSITES) {
      TryImproveRequest req = base_req;
      req.set_approach(TryImproveRequest::OPPOSITES);
      req.set_iters(OPPOSITES_ITERS);
      req.set_seed(StringPrintf("opp%d", start->movenum));

      HelperRequest hreq;
      hreq.mutable_tryimprove()->MergeFrom(req);
      requests.push_back(hreq);
    }

    for (int i = 0; i < NUM_ABLATION; i++) {
      TryImproveRequest req = base_req;
      req.set_iters(ABLATION_ITERS);
      req.set_seed(StringPrintf("abl%d.%d", start->movenum, i));
      req.set_approach(TryImproveRequest::ABLATION);

      HelperRequest hreq;
      hreq.mutable_tryimprove()->MergeFrom(req);
      requests.push_back(hreq);
    }

    for (int i = 0; i < NUM_CHOP; i++) {
      TryImproveRequest req = base_req;
      req.set_iters(CHOP_ITERS);
      req.set_seed(StringPrintf("chop%d.%d", start->movenum, i));
      req.set_approach(TryImproveRequest::CHOP);

      HelperRequest hreq;
      hreq.mutable_tryimprove()->MergeFrom(req);
      requests.push_back(hreq);
    }

    for (int i = 0; i < NUM_IMPROVE_RANDOM; i++) {
      TryImproveRequest req = base_req;
      req.set_iters(RANDOM_ITERS);
      req.set_seed(StringPrintf("seed%d.%d", start->movenum, i));
      req.set_approach(TryImproveRequest::RANDOM);

      HelperRequest hreq;
      hreq.mutable_tryimprove()->MergeFrom(req);
      requests.push_back(hreq);
    }

    GetAnswers<HelperRequest, TryImproveResponse>
      getanswers(ports_, requests);
    getanswers.Loop();

    const vector<GetAnswers<HelperRequest,
			    TryImproveResponse>::Work> &work =
      getanswers.GetWork();

    fprintf(log, "<li>Attempts at improving:\n<ul>");
    int numer = 0, denom = 0;
    for (int i = 0; i < work.size(); i++) {
      const TryImproveRequest &req = work[i].req->tryimprove();
      const TryImproveResponse &res = work[i].res;
      CHECK(res.score_size() == res.inputs_size());
      for (int j = 0; j < res.inputs_size(); j++) {
	Replacement r;
	r.method =
	  StringPrintf("%s-%d-%s",
		       TryImproveRequest::Approach_Name(req.approach()).c_str(),
		       req.iters(),
		       req.seed().c_str());
	ReadBytesFromProto(res.inputs(j), &r.inputs);
	r.score = res.score(j);
	replacements->push_back(r);
      }
      
      fprintf(log, "<li>%s: %d/%d</li>\n",
	      TryImproveRequest::Approach_Name(req.approach()).c_str(),
	      res.iters_better(),
	      res.iters_tried());

      numer += res.iters_better();
      denom += res.iters_tried();
    }
    fprintf(log, "</ul></li><li> ... (total %d/%d = %.1f%%)</li>\n",
	    numer, denom, (100.0 * numer) / denom);
    *improvability = (double)numer / denom;

    #else
    // This is optional, so if there's no MARIONET, skip for now.
    fprintf(stderr, "TryImprove requires MARIONET...\n");
    #endif

    uint64 end_time = time(NULL);
    fprintf(stderr, "TryImprove took %d seconds.\n",
	    (int)(end_time - start_time));
  }

  // Get a checkpoint that is at least MIN_BACKTRACK_DISTANCE inputs
  // in the past, or return NULL.
  Checkpoint *GetRecentCheckpoint() {
    for (int i = checkpoints.size() - 1; i >= 0; i--) {
      if ((movie.size() - checkpoints[i].movenum) > MIN_BACKTRACK_DISTANCE &&
	  checkpoints[i].movenum > watermark) {
	return &checkpoints[i];
      }
    }
    return NULL;
  }


  void MaybeBacktrack(int iters,
		      int *rounds_until_backtrack,
		      vector<Future> *futures) {
    if (!TRY_BACKTRACK)
      return;

    // Now consider backtracking.
    // TODO: We could trigger a backtrack step whenever we feel
    // like we aren't making significant progress, like when
    // there's very little difference between the futures we're
    // looking at, or when we haven't made much progress since
    // the checkpoint, or whatever. That would probably help
    // since part of the difficulty here is going to be deciding
    // whether the current state or some backtracked-to state is
    // actually better, and if we know the current state is bad,
    // then we have less opportunity to get it wrong.
    --*rounds_until_backtrack;
    if (*rounds_until_backtrack == 0) {
      *rounds_until_backtrack = TRY_BACKTRACK_EVERY;
      fprintf(stderr, " ** backtrack time. **\n");
      uint64 start_time = time(NULL);

      fprintf(log,
	      "<h2>Backtrack at iter %d, end frame %d, %s.</h2>\n",
	      iters,
	      
	      movie.size(),
	      TimeString(start_time).c_str());
      fflush(log);

      // Backtracking is like this. Call the last checkpoint "start"
      // (technically it could be any checkpoint, so think about
      // principled ways of finding a good starting point.) and
      // the current point "now". There are N inputs between
      // start and now.
      //
      // The goal is, given what we know, to see if we can find a
      // different N inputs that yield a better outcome than what
      // we have now. The purpose is twofold:
      //  - We may have just gotten ourselves into a local maximum
      //    by bad luck. If the checkpoint is before that bad
      //    choice, we have some chance of not making it (but
      //    that's basically random).
      //  - We now know more about what's possible, which should
      //    help us choose better. For examples, we can try
      //    variations on the sequence of N moves between start
      //    and now.

      // Morally const, but need to load state from it.
      Checkpoint *start_ptr = GetRecentCheckpoint();
      if (start_ptr == NULL) {
	fprintf(stderr, "No checkpoint to try backtracking.\n");
	return;
      }
      // Copy, because stuff we do in here can resize the
      // checkpoints array and cause disappointment.
      Checkpoint start = *start_ptr;

      const int nmoves = movie.size() - start.movenum;
      CHECK(nmoves > 0);

      // Inputs to be improved.
      vector<uint8> improveme;
      for (int i = start.movenum; i < movie.size(); i++) {
	improveme.push_back(movie[i]);
      }

      vector<uint8> current_state;
      Emulator::SaveUncompressed(&current_state);
      vector<Replacement> replacements;
      double improvability = 0.0;
      TryImprove(&start, improveme, current_state,
		 &replacements, &improvability);
      if (replacements.empty()) {
	fprintf(stderr,
		ANSI_GREEN "There were no superior replacements."
		ANSI_RESET "\n");
	return;
      } else if (improvability < 0.05) {
	fprintf(stderr,
		"Improvability only " ANSI_GREEN "%.2f%% :)" ANSI_RESET "\n",
		100.0 * improvability);
      } else if (improvability > 0.30) {
	fprintf(stderr,
		"Improvability high at " ANSI_RED "%.2f%% :(" ANSI_RESET "\n",
		100.0 * improvability);
      } else {
	fprintf(stderr, "Improvability is " ANSI_CYAN "%.2f%%" ANSI_RESET "\n",
		100.0 * improvability);
      }

      // Rather than trying to find the best immediate one (we might
      // be hovering above a pit about to die, so we do need to look
      // into the future), use the standard TakeBestAmong to score all
      // the potential improvements, as well as the current best.
      fprintf(stderr,
	      "There are %d+1 possible replacements for last %d moves...\n",
	      replacements.size(),
	      nmoves);

      for (int i = 0; i < replacements.size(); i++) {
	fprintf(log,
		"<li>%d inputs via %s, %.2f</li>\n",
		replacements[i].inputs.size(),
		replacements[i].method.c_str(),
		replacements[i].score);
      }
      fflush(log);

      SimpleFM2::WriteInputsWithSubtitles(
	  StringPrintf("%s-playfun-backtrack-%d-replaced.fm2", 
		       game.c_str(), iters),
	  game + ".nes",
	  BASE64,
	  movie,
	  subtitles);
      Rewind(start.movenum);
      Emulator::LoadUncompressed(&start.save);

      set< vector<uint8> > tryme;
      vector< vector<uint8> > tryvec;
      vector<string> trysplanations;
      // Allow the existing sequence to be chosen if it's
      // still better despite seeing these alternatives.
      tryme.insert(improveme);
      tryvec.push_back(improveme);
      // XXX better to keep whatever annotations were already there!
      trysplanations.push_back("original");

      for (int i = 0; i < replacements.size(); i++) {
	// Currently ignores scores and methods. Make TakeBestAmong
	// take annotated nexts so it can tell you which one it
	// preferred. (Consider weights too..?)
	if (tryme.find(replacements[i].inputs) == tryme.end()) {
	  tryme.insert(replacements[i].inputs);
	  tryvec.push_back(replacements[i].inputs);
	  trysplanations.push_back(replacements[i].method);
	}
      }

      // vector< vector<uint8> > tryvec(tryme.begin(), tryme.end());
      if (tryvec.size() != replacements.size() + 1) {
	fprintf(stderr, "... but there were %d duplicates (removed).\n",
		(replacements.size() + 1) - tryvec.size());
	fprintf(log, "<li><b>%d total but there were %d duplicates (removed)."
		"</b></li>\n",
		replacements.size() + 1,
		(replacements.size() + 1) - tryvec.size());
	fflush(log);
      }

      // PERF could be passing along the end state for these, to
      // avoid the initial replay. If they happen to go back to the
      // same helper that computed it in the first place, it'd be
      // cached, at least.
      TakeBestAmong(tryvec, trysplanations, futures, false);

      fprintf(stderr, "Write replacement movie.\n");
      SimpleFM2::WriteInputsWithSubtitles(
	  StringPrintf("%s-playfun-backtrack-%d-replacement.fm2", 
		       game.c_str(), iters),
	  game + ".nes",
	  BASE64,
	  movie,
	  subtitles);

      // What to do about futures? This is simplest, I guess...
      uint64 end_time = time(NULL);
      fprintf(stderr,
	      "Backtracking took %d seconds in total. "
	      "Back to normal search...\n",
	      end_time - start_time);
      fprintf(log,
	      "<li>Backtracking took %d seconds in total.</li>\n",
	      end_time - start_time);
      fflush(log);
    }
  }

  void SaveMovie() {
    printf("                     - writing movie -\n");
    SimpleFM2::WriteInputsWithSubtitles(
        game + "-playfun-futures-progress.fm2",
	game + ".nes",
	BASE64,
	movie,
	subtitles);
    Emulator::PrintCacheStats();
  }

  void SaveQuickDiagnostics(const vector<Future> &futures) {
    printf("                     - quick diagnostics -\n");
    SaveFuturesHTML(futures, game + "-playfun-futures.html");
  }

  void SaveDiagnostics(const vector<Future> &futures) {
    printf("                     - slow diagnostics -\n");
    // This is now too expensive because the futures aren't cached
    // in this process.
    #if 0
    for (int i = 0; i < futures.size(); i++) {
      vector<uint8> fmovie = movie;
      for (int j = 0; j < futures[i].inputs.size(); j++) {
	fmovie.push_back(futures[i].inputs[j]);
	SimpleFM2::WriteInputs(StringPrintf("%s-playfun-future-%d.fm2",
					    game.c_str(),
					    i),
			       game + ".nes",
			       BASE64,
			       fmovie);
      }
    }
    printf("Wrote %d movie(s).\n", futures.size() + 1);
    #endif
    SaveDistributionSVG(distributions, game + "-playfun-scores.svg");
    objectives->SaveSVG(memories, game + "-playfun-futures.svg");
    motifs->SaveHTML(game + "-playfun-motifs.html");
    printf("                     (wrote)\n");
  }

  // Ports for the helpers.
  vector<int> ports_;

  // For making SVG.
  vector<Scoredist> distributions;

  // Used to ffwd to gameplay.
  vector<uint8> solution;

  FILE *log;
  ArcFour rc;
  WeightedObjectives *objectives;
  Motifs *motifs;
  vector< vector<uint8> > motifvec;
  string game;

 private:
  // Dynamic futures count.
  int nfutures_;
  // ---- Persistent state snapshotting ----
  static inline void AppendU32(vector<uint8> *out, uint32_t v) {
    out->push_back(v & 0xFF);
    out->push_back((v >> 8) & 0xFF);
    out->push_back((v >> 16) & 0xFF);
    out->push_back((v >> 24) & 0xFF);
  }
  static inline void AppendI32(vector<uint8> *out, int32_t v) {
    AppendU32(out, (uint32_t)v);
  }
  static inline bool ReadU32(const vector<uint8> &in, size_t *p, uint32_t *v) {
    if (*p + 4 > in.size()) return false;
    *v = (uint32_t)in[*p] |
         ((uint32_t)in[*p + 1] << 8) |
         ((uint32_t)in[*p + 2] << 16) |
         ((uint32_t)in[*p + 3] << 24);
    *p += 4; return true;
  }
  static inline bool ReadI32(const vector<uint8> &in, size_t *p, int32_t *v) {
    uint32_t u; if (!ReadU32(in, p, &u)) return false; *v = (int32_t)u; return true;
  }
  static inline void AppendBytes(vector<uint8> *out, const vector<uint8> &b) {
    AppendU32(out, (uint32_t)b.size());
    out->insert(out->end(), b.begin(), b.end());
  }
  static inline bool ReadBytes(const vector<uint8> &in, size_t *p, vector<uint8> *b) {
    uint32_t n; if (!ReadU32(in, p, &n)) return false;
    if (*p + n > in.size()) return false;
    b->assign(in.begin() + *p, in.begin() + *p + n);
    *p += n; return true;
  }
  static inline void AppendString(vector<uint8> *out, const string &s) {
    AppendU32(out, (uint32_t)s.size());
    out->insert(out->end(), s.begin(), s.end());
  }
  static inline bool ReadString(const vector<uint8> &in, size_t *p, string *s) {
    uint32_t n; if (!ReadU32(in, p, &n)) return false;
    if (*p + n > in.size()) return false;
    s->assign((const char*)&in[*p], n);
    *p += n; return true;
  }

  void SaveStateFile() {
    if (checkpoints.empty()) return;
    const Checkpoint &cp = checkpoints.back();
    vector<uint8> out;
    // Magic
    out.push_back('P'); out.push_back('F'); out.push_back('S'); out.push_back('T');
    AppendU32(&out, 1); // version
    AppendString(&out, game);
    AppendI32(&out, watermark);
    // Movie and subtitles
    AppendBytes(&out, movie);
    AppendU32(&out, (uint32_t)subtitles.size());
    for (int i = 0; i < subtitles.size(); i++) AppendString(&out, subtitles[i]);
    // Memories
    AppendU32(&out, (uint32_t)memories.size());
    for (int i = 0; i < memories.size(); i++) AppendBytes(&out, memories[i]);
    // Latest checkpoint
    AppendI32(&out, cp.movenum);
    AppendBytes(&out, cp.save);
    // Motif weights
    vector< vector<uint8> > all = motifs->AllMotifs();
    AppendU32(&out, (uint32_t)all.size());
    for (int i = 0; i < all.size(); i++) {
      double *w = motifs->GetWeightPtr(all[i]);
      double ww = (w != NULL) ? *w : 0.0;
      // Serialize double as 8 bytes
      union { double d; uint8 b[8]; } u; u.d = ww;
      for (int k = 0; k < 8; k++) out.push_back(u.b[k]);
      AppendBytes(&out, all[i]);
    }
    // RNG state
    vector<uint8> rng;
    rc.GetState(&rng);
    AppendBytes(&out, rng);

    const string fname = game + ".pfstate";
    Util::WriteFileBytes(fname, out);
    fprintf(stderr, "Saved checkpoint state to %s (%zu bytes)\n", fname.c_str(), out.size());
  }

  bool LoadStateFile() {
    const string fname = game + ".pfstate";
    if (!Util::ExistsFile(fname)) return false;
    vector<uint8> in = Util::ReadFileBytes(fname);
    size_t p = 0;
    if (in.size() < 8) return false;
    if (!(in[0]=='P' && in[1]=='F' && in[2]=='S' && in[3]=='T')) return false;
    p = 4;
    uint32_t ver = 0; if (!ReadU32(in, &p, &ver)) return false;
    if (ver != 1) { fprintf(stderr, "Unknown pfstate version %u\n", ver); return false; }
    string g; if (!ReadString(in, &p, &g)) return false;
    if (g != game) { fprintf(stderr, "State file game mismatch: %s vs %s\n", g.c_str(), game.c_str()); return false; }
    int32_t wm = 0; if (!ReadI32(in, &p, &wm)) return false; watermark = wm;
    if (!ReadBytes(in, &p, &movie)) return false;
    uint32_t nsubs = 0; if (!ReadU32(in, &p, &nsubs)) return false;
    subtitles.clear(); subtitles.reserve(nsubs);
    for (uint32_t i = 0; i < nsubs; i++) { string s; if (!ReadString(in, &p, &s)) return false; subtitles.push_back(s); }
    uint32_t nmem = 0; if (!ReadU32(in, &p, &nmem)) return false;
    memories.clear(); memories.resize(nmem);
    for (uint32_t i = 0; i < nmem; i++) if (!ReadBytes(in, &p, &memories[i])) return false;
    int32_t movenum = 0; if (!ReadI32(in, &p, &movenum)) return false;
    vector<uint8> save; if (!ReadBytes(in, &p, &save)) return false;
    checkpoints.clear(); checkpoints.push_back(Checkpoint(save, movenum));
    // Restore emulator to that checkpoint.
    Emulator::LoadUncompressed(&checkpoints.back().save);
    // Rebuild objective observations from saved memories.
    for (int i = 0; i < memories.size(); i++) objectives->Observe(memories[i]);
    // Restore motif weights
    uint32_t nmot = 0; if (!ReadU32(in, &p, &nmot)) return false;
    for (uint32_t i = 0; i < nmot; i++) {
      if (p + 8 > in.size()) return false; union { double d; uint8 b[8]; } u; for (int k=0;k<8;k++) u.b[k]=in[p++];
      vector<uint8> inputs; if (!ReadBytes(in, &p, &inputs)) return false;
      double *w = motifs->GetWeightPtr(inputs);
      if (w) *w = u.d;
    }
    // RNG
    vector<uint8> rng; if (!ReadBytes(in, &p, &rng)) return false;
    (void)rc.SetState(rng);
    return true;
  }
};

/**
 * The main loop for the SDL.
 */
int main(int argc, char *argv[]) {
  #if MARIONET
  fprintf(stderr, "Init SDL\n");

  /* Initialize SDL and network, if we're using it. */
  CHECK(SDL_Init(0) >= 0);
  CHECK(SDLNet_Init() >= 0);
  fprintf(stderr, "SDL initialized OK.\n");
  #endif

  PlayFun pf;

  #if MARIONET
  if (argc >= 2) {
    if (0 == strcmp(argv[1], "--helper")) {
      if (argc < 3) {
	fprintf(stderr, "Need one port number after --helper.\n");
	abort();
      }
      int port = atoi(argv[2]);
      fprintf(stderr, "Starting helper on port %d...\n", port);
      pf.Helper(port);
      fprintf(stderr, "helper returned?\n");
    } else if (0 == strcmp(argv[1], "--master")) {
      vector<int> helpers;
      for (int i = 2; i < argc; i++) {
	int hp = atoi(argv[i]);
	if (!hp) {
	  fprintf(stderr,
		  "Expected a series of helper ports after --master.\n");
	  abort();
	}
	helpers.push_back(hp);
      }
      pf.Master(helpers);
      fprintf(stderr, "master returned?\n");
    }
  } else {
    vector<int> empty;
    pf.Master(empty);
  }
  #else
  vector<int> nobody;
  pf.Master(nobody);
  #endif

  Emulator::Shutdown();

  // exit the infrastructure
  FCEUI_Kill();

  #if MARIONET
  SDLNet_Quit();
  SDL_Quit();
  #endif
  return 0;
}
