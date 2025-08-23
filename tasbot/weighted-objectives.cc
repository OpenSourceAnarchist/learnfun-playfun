
#include "weighted-objectives.h"

#include <algorithm>
#include <set>
#include <string>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>
#include <cmath>
#include <math.h>

#include "tasbot.h"
#include "../cc-lib/arcfour.h"
#include "../cc-lib/textsvg.h"
#include "util.h"

using namespace std;

// Encoding for objective components in on-disk format (and learnfun generation):
// Use high bits as flags, low bits as memory index (0..2047 is enough for NES RAM).
static const int OBJ_SIGNED_FLAG = (1 << 29);   // treat byte as signed
static const int OBJ_DEC_FLAG    = (1 << 30);   // decreasing is good
static const int OBJ_INDEX_MASK  = ((1 << 29) - 1);

static inline int ObjIndexFromToken(int tok) { return tok & OBJ_INDEX_MASK; }
static inline bool ObjSignedFromToken(int tok) { return (tok & OBJ_SIGNED_FLAG) != 0; }
static inline bool ObjDecreasingFromToken(int tok) { return (tok & OBJ_DEC_FLAG) != 0; }
static inline int ObjTokenFromParts(int index, bool is_signed, bool is_dec) {
  return (index & OBJ_INDEX_MASK) | (is_signed ? OBJ_SIGNED_FLAG : 0) | (is_dec ? OBJ_DEC_FLAG : 0);
}

struct WeightedObjectives::Info {
  explicit Info(double w) : weight(w), is_sorted(true) {}
  double weight;

  // Per-part definition for this objective.
  vector<int> indices;           // memory indices
  vector<uint8> is_signed;       // 1 if signed compare
  vector<uint8> is_decreasing;   // 1 if decreasing is good

  // Sorted, using transformed keys (MapKey) lex-ascending.
  mutable bool is_sorted;  
  mutable vector< vector<uint8> > observations;

  const vector< vector<uint8> > &GetObservations() const {
    if (!is_sorted) {
      std::sort(observations.begin(), observations.end());
      // Observation thinning: keep only unique transformed keys.
      observations.erase(std::unique(observations.begin(), observations.end()), observations.end());
      is_sorted = true;
    }
    return observations;
  }
};

WeightedObjectives::WeightedObjectives() {}

WeightedObjectives::WeightedObjectives(const vector< vector<int> > &objs) {
  for (int i = 0; i < objs.size(); i++) {
    Info *info = new Info(1.0);
    const vector<int> &tok = objs[i];
    info->indices.reserve(tok.size());
    info->is_signed.reserve(tok.size());
    info->is_decreasing.reserve(tok.size());
    for (int j = 0; j < tok.size(); j++) {
      info->indices.push_back(ObjIndexFromToken(tok[j]));
      info->is_signed.push_back(ObjSignedFromToken(tok[j]) ? 1 : 0);
      info->is_decreasing.push_back(ObjDecreasingFromToken(tok[j]) ? 1 : 0);
    }
    // Use the tokens (with flags) as the key to keep distinct variants separate.
    weighted[tok] = info;
  }
}

static string ObjectiveToString(const vector<int> &obj) {
  string s;
  for (int i = 0; i < obj.size(); i++) {
    char d[64] = {0};
    sprintf(d, "%s%d", (i ? " " : ""), obj[i]);
    s += d;
  }
  return s;
}

WeightedObjectives *
WeightedObjectives::LoadFromFile(const string &filename) {
  WeightedObjectives *wo = new WeightedObjectives;
  vector<string> lines = Util::ReadFileToLines(filename);
  for (int i = 0; i < lines.size(); i++) {
    string line = lines[i];
    Util::losewhitel(line);
    if (!line.empty() && !Util::startswith(line, "#")) {
      stringstream ss(line, stringstream::in);
      double d;
      ss >> d;
      vector<int> locs;
      while (!ss.eof()) {
        int i;
        ss >> i;
        if (!ss.fail()) locs.push_back(i);
      }
      Info *info = new Info(d);
      info->indices.reserve(locs.size());
      info->is_signed.reserve(locs.size());
      info->is_decreasing.reserve(locs.size());
      for (int j = 0; j < (int)locs.size(); j++) {
        info->indices.push_back(ObjIndexFromToken(locs[j]));
        info->is_signed.push_back(ObjSignedFromToken(locs[j]) ? 1 : 0);
        info->is_decreasing.push_back(ObjDecreasingFromToken(locs[j]) ? 1 : 0);
      }
      wo->weighted.insert(make_pair(locs, info));
    }
  }

  return wo;
}

vector< pair<const vector<int> *, double> > WeightedObjectives::GetAll() const {
  vector< pair<const vector<int> *, double> > v;
  for (Weighted::const_iterator it = weighted.begin(); 
       it != weighted.end(); ++it) {
    v.push_back(make_pair(&it->first, it->second->weight));
  }
  return v;
}
  
void WeightedObjectives::SaveToFile(const string &filename) const {
  string out;
  for (Weighted::const_iterator it = weighted.begin(); it != weighted.end(); ++it) {
    const Info &info = *it->second;
    if (info.weight <= 0) continue;
    // Reconstruct tokens from Info (so flags reflect any learning decisions).
    vector<int> tokens; tokens.reserve(info.indices.size());
    for (int k = 0; k < info.indices.size(); k++) {
      tokens.push_back(ObjTokenFromParts(info.indices[k], info.is_signed[k] != 0, info.is_decreasing[k] != 0));
    }
    // Build line
    string line = StringPrintf("%f", info.weight);
    for (int k = 0; k < tokens.size(); k++) line += StringPrintf(" %d", tokens[k]);
    line += "\n";
    out += line;
  }
  Util::WriteFile(filename, out);
  printf("Saved weighted objectives to %s\n", filename.c_str());
}

size_t WeightedObjectives::Size() const {
  return weighted.size();
}

// Map a byte to a rank key [0..255] depending on flags.
static inline uint8 MapKey(uint8 b, bool is_signed, bool is_dec) {
  int v = is_signed ? (int)(int8_t)b + 128 : (int)b;
  int key = is_dec ? (255 - v) : v;
  if (key < 0) key = 0; if (key > 255) key = 255;
  return (uint8)key;
}

void WeightedObjectives::Observe(const vector<uint8> &memory) {
  // PERF Currently, we just keep a sorted vector for each objective's
  // value at each observation. This is not very efficient. Worse, it
  // may have the undesirable effect that a particular state's value
  // can change (arbitrarily) with future observations, even just by
  // observing states we've already seen again (changes mass
  // distribution).
  for (Weighted::iterator it = weighted.begin();
       it != weighted.end(); ++it) {
    Info *info = it->second;
    info->observations.resize(info->observations.size() + 1);
    vector<uint8> *cur = &info->observations.back();
    cur->reserve(info->indices.size());

    for (int i = 0; i < info->indices.size(); i++) {
      int idx = info->indices[i];
      uint8 b = memory[idx];
      cur->push_back(MapKey(b, info->is_signed[i] != 0, info->is_decreasing[i] != 0));
    }

    info->is_sorted = false;

    // Maybe should just keep the unique values? Otherwise
    // lower_bound is doing something kind of funny when there
    // are lots of the same value...
  }
}

// Removed free helpers that referenced private Info; comparisons are inlined below.

double WeightedObjectives::WeightedLess(const vector<uint8> &mem1,
					const vector<uint8> &mem2) const {
  double score = 0.0;
  for (Weighted::const_iterator it = weighted.begin();
       it != weighted.end(); ++it) {
    const Info *info = it->second;
    const double weight = info->weight;
    bool less = false;
    bool decided = false;
    for (int i = 0; i < (int)info->indices.size(); i++) {
      uint8 a = MapKey(mem1[info->indices[i]], info->is_signed[i] != 0, info->is_decreasing[i] != 0);
      uint8 b = MapKey(mem2[info->indices[i]], info->is_signed[i] != 0, info->is_decreasing[i] != 0);
      if (a < b) { less = true; decided = true; break; }
      if (a > b) { less = false; decided = true; break; }
    }
    if (decided && less) score += weight;
  }
  CHECK(score >= 0);
  return score;
}

double WeightedObjectives::Evaluate(const vector<uint8> &mem1,
				    const vector<uint8> &mem2) const {
  double score = 0.0;
  for (Weighted::const_iterator it = weighted.begin();
       it != weighted.end(); ++it) {
    const Info *info = it->second;
    const double weight = info->weight;
    int order = 0;
    for (int i = 0; i < (int)info->indices.size(); i++) {
      uint8 a = MapKey(mem1[info->indices[i]], info->is_signed[i] != 0, info->is_decreasing[i] != 0);
      uint8 b = MapKey(mem2[info->indices[i]], info->is_signed[i] != 0, info->is_decreasing[i] != 0);
      if (a < b) { order = 1; break; }
      if (a > b) { order = -1; break; }
    }
    if (order == 1) score += weight; else if (order == -1) score -= weight;
  }
  return score;
}

// Helper: inline f = rank fraction for a transformed key vector
static inline double RankFracInline(const vector< vector<uint8> > &obs,
                                    const vector<uint8> &key) {
  if (obs.empty()) return 0.0;
  int idx = lower_bound(obs.begin(), obs.end(), key) - obs.begin();
  double frac = (double)idx / (double)obs.size();
  if (frac < 0.0) frac = 0.0; if (frac > 1.0) frac = 1.0;
  return frac;
}

double WeightedObjectives::EvaluateMagnitude(const vector<uint8> &mem1,
                                             const vector<uint8> &mem2) const {
  double score = 0.0;
  for (Weighted::const_iterator it = weighted.begin(); it != weighted.end(); ++it) {
    const Info *info = it->second;
    const double weight = info->weight;
    vector<uint8> k1; k1.reserve(info->indices.size());
    vector<uint8> k2; k2.reserve(info->indices.size());
    for (int i = 0; i < (int)info->indices.size(); i++) {
      int idx = info->indices[i];
      k1.push_back(MapKey(mem1[idx], info->is_signed[i] != 0, info->is_decreasing[i] != 0));
      k2.push_back(MapKey(mem2[idx], info->is_signed[i] != 0, info->is_decreasing[i] != 0));
    }
    const vector< vector<uint8> > &obs = info->GetObservations();
    double f1 = RankFracInline(obs, k1);
    double f2 = RankFracInline(obs, k2);
    score += weight * (f2 - f1);
  }
  return score;
}

void WeightedObjectives::DeltaMagnitude(const vector<uint8> &mem1,
                                        const vector<uint8> &mem2,
                                        double *pos_out,
                                        double *neg_out) const {
  double pos = 0.0, neg = 0.0;
  for (Weighted::const_iterator it = weighted.begin(); it != weighted.end(); ++it) {
    const Info *info = it->second;
    const double weight = info->weight;
    vector<uint8> k1; k1.reserve(info->indices.size());
    vector<uint8> k2; k2.reserve(info->indices.size());
    for (int i = 0; i < (int)info->indices.size(); i++) {
      int idx = info->indices[i];
      k1.push_back(MapKey(mem1[idx], info->is_signed[i] != 0, info->is_decreasing[i] != 0));
      k2.push_back(MapKey(mem2[idx], info->is_signed[i] != 0, info->is_decreasing[i] != 0));
    }
    const vector< vector<uint8> > &obs = info->GetObservations();
    double f1 = RankFracInline(obs, k1);
    double f2 = RankFracInline(obs, k2);
    double d = weight * (f2 - f1);
    if (d >= 0) pos += d; else neg += d;
  }
  if (pos_out) *pos_out = pos;
  if (neg_out) *neg_out = neg;
}

#if 0
// XXX can probably simplify this, but should probably just remove it.
double WeightedObjectives::BuggyEvaluate(const vector<uint8> &mem1,
					 const vector<uint8> &mem2) const {
  double score = 0.0;
  for (Weighted::const_iterator it = weighted.begin();
       it != weighted.end(); ++it) {
    const vector<int> &objective = it->first;
    const double weight = it->second->weight;
    switch (Order(mem1, mem2, objective)) {
      // XXX bug!!
    case -1: score -= weight; // FALLTHROUGH
      case 1: score += weight;
      case 0:
      default:;
    }
  }
  return score;
}
#endif

// Removed GetValuesX/GetUniqueValuesX; call sites inline transforms.

// Find the index of the vector now within the values
// array, which is sorted and unique.
static inline int GetValueIndex(const vector< vector<uint8> > &values,
                                const vector<uint8> &now) {
  return lower_bound(values.begin(), values.end(), now) - values.begin();
}

static inline double GetValueFrac(const vector< vector<uint8> > &values,
                                  const vector<uint8> &now) {
  int idx = GetValueIndex(values, now);
  // -1, since it can never be the size itself?
  // and what should the value be if values is empty or singleton?
  return (double)idx / values.size();
}

double WeightedObjectives::GetNormalizedValue(const vector<uint8> &mem) {
  double sum = 0.0;

  for (Weighted::iterator it = weighted.begin(); it != weighted.end(); ++it) {
    Info *info = &*it->second;
    vector<uint8> cur; cur.reserve(info->indices.size());
    for (int i = 0; i < (int)info->indices.size(); i++) {
      int idx = info->indices[i];
      cur.push_back(MapKey(mem[idx], info->is_signed[i] != 0, info->is_decreasing[i] != 0));
    }
    sum += GetValueFrac(info->GetObservations(), cur);
  }

  sum /= (double)weighted.size();
  return sum;
}

vector<double> WeightedObjectives::
GetNormalizedValues(const vector<uint8> &mem) {
  vector<double> out;
  for (Weighted::iterator it = weighted.begin(); it != weighted.end(); ++it) {
    Info *info = &*it->second;
    vector<uint8> cur; cur.reserve(info->indices.size());
    for (int i = 0; i < (int)info->indices.size(); i++) {
      int idx = info->indices[i];
      cur.push_back(MapKey(mem[idx], info->is_signed[i] != 0, info->is_decreasing[i] != 0));
    }
    out.push_back(GetValueFrac(info->GetObservations(), cur));
  }

  return out;
}

void WeightedObjectives::WeightByExamples(const vector< vector<uint8> >
					  &memories) {
  CHECK(!memories.empty());
  // Build a new weighted map by splitting any objective whose parts
  // prefer different signed/polarity flags into separate sub-objectives.
  Weighted newweighted;

  auto compute_scalar_delta = [&](int index, bool sflag, bool dflag) -> double {
    // Unique value set for rank fractions (as 1-D vectors).
    set< vector<uint8> > values_set;
    values_set.clear();
    for (int i = 0; i < (int)memories.size(); i++) {
      uint8 k = MapKey(memories[i][index], sflag, dflag);
      values_set.insert(vector<uint8>(1, k));
    }
    vector< vector<uint8> > values(values_set.begin(), values_set.end());
    // Integral (sum of step deltas), normalized by number of steps.
    double sum = 0.0;
    for (int i = 0; i < (int)memories.size() - 1; i++) {
      vector<uint8> a(1, MapKey(memories[i][index], sflag, dflag));
      vector<uint8> b(1, MapKey(memories[i + 1][index], sflag, dflag));
      sum += GetValueFrac(values, b) - GetValueFrac(values, a);
    }
    if (memories.size() > 1) sum /= (double)(memories.size() - 1);
    return sum;
  };

  auto group_weight_delta = [&](const vector<int> &indices,
                                bool sflag, bool dflag) -> double {
    if (indices.empty()) return 0.0;
    // Build transformed key vectors per memory for these indices.
    set< vector<uint8> > values_set;
    for (int i = 0; i < (int)memories.size(); i++) {
      vector<uint8> key; key.reserve(indices.size());
      for (int j = 0; j < (int)indices.size(); j++) {
        key.push_back(MapKey(memories[i][indices[j]], sflag, dflag));
      }
      values_set.insert(key);
    }
    vector< vector<uint8> > values(values_set.begin(), values_set.end());
    // Integral (sum of step deltas), normalized by number of steps.
    double sum = 0.0;
    for (int i = 0; i < (int)memories.size() - 1; i++) {
      vector<uint8> a; a.reserve(indices.size());
      vector<uint8> b; b.reserve(indices.size());
      for (int j = 0; j < (int)indices.size(); j++) {
        a.push_back(MapKey(memories[i][indices[j]], sflag, dflag));
        b.push_back(MapKey(memories[i + 1][indices[j]], sflag, dflag));
      }
      sum += GetValueFrac(values, b) - GetValueFrac(values, a);
    }
    if (memories.size() > 1) sum /= (double)(memories.size() - 1);
    return sum;
  };

  for (Weighted::iterator it = weighted.begin(); it != weighted.end(); ++it) {
    Info *info = it->second;
    // Per-index best flags.
    struct Part { int idx; bool s; bool d; };
    vector<Part> parts;
    parts.reserve(info->indices.size());
    for (int k = 0; k < (int)info->indices.size(); k++) {
      int idx = info->indices[k];
      double bestv = 0.0; bool bests = false, bestd = false;
      for (int si = 0; si < 2; si++) for (int di = 0; di < 2; di++) {
        bool s = (si != 0), d = (di != 0);
        double v = compute_scalar_delta(idx, s, d);
  if (k == 0 || fabs(v) > fabs(bestv)) { bestv = v; bests = s; bestd = d; }
      }
      parts.push_back({idx, bests, bestd});
    }

    // Group indices by (s,d).
    vector<int> grp_ss, grp_sd, grp_ds, grp_dd; // naming: s/d true/false combos
    for (const Part &p : parts) {
      if (p.s && p.d) grp_ss.push_back(p.idx);
      else if (p.s && !p.d) grp_sd.push_back(p.idx);
      else if (!p.s && p.d) grp_ds.push_back(p.idx);
      else grp_dd.push_back(p.idx);
    }

    auto emit_group = [&](const vector<int> &grp, bool s, bool d) {
      if (grp.empty()) return;
      // Compute group weight (absolute delta) and construct Info and key.
      double delta = group_weight_delta(grp, s, d);
  double w = fabs(delta);
      // Heuristic: penalize trivial/constant groups. If there is only one
      // unique transformed value across memories, or weight ~ 0, skip.
      set< vector<uint8> > uniq;
      for (int i = 0; i < (int)memories.size(); i++) {
        vector<uint8> key; key.reserve(grp.size());
        for (int j = 0; j < (int)grp.size(); j++) key.push_back(MapKey(memories[i][grp[j]], s, d));
        uniq.insert(key);
      }
      if (uniq.size() <= 1) return; // constant objective; ignore
      if (w < 1e-9) return; // numerically trivial
      // If weight is zero, still include it (consumers will ignore <=0 when saving).
      Info *ni = new Info(w);
      ni->indices = grp;
      ni->is_signed.assign(grp.size(), s ? 1 : 0);
      ni->is_decreasing.assign(grp.size(), d ? 1 : 0);
      // Key uses flagged tokens, preserving index order.
      vector<int> tokens; tokens.reserve(grp.size());
      for (int idx : grp) tokens.push_back(ObjTokenFromParts(idx, s, d));
      newweighted.insert(make_pair(tokens, ni));
    };

    emit_group(grp_dd, false, false);
    emit_group(grp_ds, false, true);
    emit_group(grp_sd, true, false);
    emit_group(grp_ss, true, true);
  }

  // Delete old infos and replace map.
  for (Weighted::iterator it = weighted.begin(); it != weighted.end(); ++it) {
    delete it->second;
  }
  weighted.swap(newweighted);
}

void WeightedObjectives::SaveSVG(const vector< vector<uint8> > &memories,
				 const string &filename) const {
  static const int WIDTH = 2048;
  static const int HEIGHT = 1204;

  string out = TextSVG::Header(WIDTH, HEIGHT);

  ArcFour rc("Zmake colors");

  uint64 skipped = 0;
  int howmany = 500;
  for (Weighted::const_iterator it = weighted.begin();
       howmany-- && it != weighted.end(); ++it) {
    const Info *info = it->second;
    // All the distinct values this objective takes on, in order (transformed).
    set< vector<uint8> > values_set;
    for (int i = 0; i < (int)memories.size(); i++) {
      vector<uint8> key; key.reserve(info->indices.size());
      for (int j = 0; j < (int)info->indices.size(); j++)
        key.push_back(MapKey(memories[i][info->indices[j]], info->is_signed[j] != 0, info->is_decreasing[j] != 0));
      values_set.insert(key);
    }
    vector< vector<uint8> > values(values_set.begin(), values_set.end());
    // printf("%lld distinct values for %s\n", values.size(),
    // ObjectiveToString(obj).c_str());

    const string color = RandomColor(&rc);
    const string startpolyline =
      StringPrintf("  <polyline fill=\"none\" "
		   "opacity=\"0.5\" "
		   "stroke=\"%s\""
		   " stroke-width=\"%d\" points=\"", 
		   // (info.weight <= 0) ? "#f00" : "#0f0",
		   color.c_str(),
		   1
		   // (info.weight <= 0) ? 3 : 1
		   );
    const string endpolyline = "\" />\n";
    out += "<g>\n";
    out += startpolyline;

    static const int MAXLEN = 256;
    int numleft = MAXLEN;
    // Fill in points as space separated x,y coords
    int lastvalueindex = -1;
    for (int i = 0; i < memories.size(); i++) {
      vector<uint8> now; now.reserve(info->indices.size());
      for (int j = 0; j < (int)info->indices.size(); j++)
        now.push_back(MapKey(memories[i][info->indices[j]], info->is_signed[j] != 0, info->is_decreasing[j] != 0));
      int valueindex = GetValueIndex(values, now);

      // Allow drawing horizontal lines without interstitial points.
      if (valueindex == lastvalueindex) {
	while (i < memories.size() - 1) {
    vector<uint8> next; next.reserve(info->indices.size());
    for (int j = 0; j < (int)info->indices.size(); j++)
      next.push_back(MapKey(memories[i + 1][info->indices[j]], info->is_signed[j] != 0, info->is_decreasing[j] != 0));
	  int nextvalueindex = GetValueIndex(values, next);
	  if (nextvalueindex != valueindex)
	    break;
	  i++;
	  skipped++;
	}
      }
      lastvalueindex = valueindex;

	// Fraction in [0, 1]
      double yf = (double)valueindex / (double)values.size();
      double xf = (double)i / (double)memories.size();
      out += Coords(WIDTH * xf, HEIGHT * (1.0 - yf)) + " ";
      if (numleft-- == 0) {
	out += endpolyline;
	out += startpolyline;
	out += Coords(WIDTH * xf, HEIGHT * (1.0 - yf)) + " ";
	numleft = MAXLEN;
      }

    }

    out += endpolyline;
    out += "</g>\n";
  }

  out += SVGTickmarks(WIDTH, memories.size(), 50.0, 20.0, 12.0);

  out += TextSVG::Footer();
  Util::WriteFile(filename, out);

  printf("Wrote %lld objectives, skipping %lld points, to %s\n", 
	 weighted.size(), skipped, filename.c_str());
}

// My apologies. I don't know lua and it's the night before a demo!
void WeightedObjectives::SaveLua(int n, const string &filename) const {
  static const char *colors[] = {
    "#ff4444",
    "#44ff44",
    "#4444ff",
    "#ffff44",
    "#ff44ff",
    "#44ffff",
  };
  static const int num_colors = sizeof(colors) / sizeof(char *);

  string out =
    "-- generated file! do not edit.\n\n"
    "while (true) do\n"
    "  local YSTART = 0;\n"
    "  local XSTART = 2;\n"
    "  ypos = YSTART;\n"
    "  xpos = XSTART;\n"
    "  color = \"#FFFFFF\"\n"
    "    local function wb(loc)\n"
    "      local byte = memory.readbyte(loc);\n"
    "      local hex = string.format(\"%2x\", byte);\n"
    "      gui.text(xpos, ypos, hex, color);\n"
    "      xpos = xpos + 12;\n"
    "      if xpos > 250 then\n"
    "        xpos = XSTART;\n"
    "        ypos = ypos + 8;\n"
    "      end;\n"
    "    end;\n"
    "\n";

  // Get them all.
  map<double, const vector<int> *> sorted;
  for (Weighted::const_iterator it = weighted.begin();
       it != weighted.end(); ++it) {
    sorted.insert(make_pair(it->second->weight, &it->first));
  }

  int actual = 0;
  {
    int i = 0;
    for (map<double, const vector<int> *>::const_iterator it = sorted.begin();
	 i < n && it != sorted.end(); ++it, i++) {
      out += StringPrintf("\n"
			  "  -- score %f\n", it->first);
      out +=
	"  xpos = XSTART;\n"
	"  ypos = ypos + 10;\n"
	"  color = \"" + (string)colors[i % num_colors] + "\"\n";
      const vector<int> &v = *it->second;
  for(int j = 0; j < v.size(); j++) {
    out += StringPrintf("  wb(%d);\n", ObjIndexFromToken(v[j]));
  }
      actual++;
    }
  }

  out += "  FCEU.frameadvance();\n";
  out += "end;\n";
  Util::WriteFile(filename, out);

  printf("Wrote %d objectives to %s\n", 
	 actual, filename.c_str());
}
