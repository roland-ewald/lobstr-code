/*
Copyright (C) 2011 Melissa Gymrek <mgymrek@mit.edu>

This file is part of lobSTR.

lobSTR is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

lobSTR is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with lobSTR.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <cmath>
#include <list>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "src/common.h"
#include "src/NoiseModel.h"
#include "src/runtime_parameters.h"
#include "src/TextFileReader.h"
#include "src/TextFileWriter.h"

const int MIN_PERIOD = 2;
const int MAX_PERIOD = 6;
const float NONUNIT_PENALTY = 0.000001;

using namespace std;

static float invLogit(float x) {
  float expx = exp(x);
  return expx/(1+expx);
}

static float ppois(int step, float mean) {
  if (step < 0) return 0;
  float p = exp(-1*mean);
  for (int i = 0; i < step; i++) {
    p = p*mean;
    p = p/(i+1);
  }
  return p;
}

NoiseModel::NoiseModel(RInside* _r) {
  R = _r;
}

void NoiseModel::Train(ReadContainer* read_container) {
  // Populate training data
  if (debug) {
    cerr << "Populating training data..." << endl;
  }
  for (map<pair<string, int>, list<AlignedRead> >::iterator
         it = read_container->aligned_str_map_.begin();
       it != read_container->aligned_str_map_.end(); it++) {
    // check if haploid
    const list<AlignedRead>& aligned_reads = it->second;
    bool is_haploid = ((aligned_reads.front().chrom == "chrX" ||
                        aligned_reads.front().chrom == "chrY"));
    if (!is_haploid) continue;
    // check if has unique mode
    float unique_mode;
    if (HasUniqueMode(aligned_reads, &unique_mode)) {
      // add to training data (mode, period, copynum, mutated)
      int period = aligned_reads.front().period;
      for (list<AlignedRead>::const_iterator it2 =
             aligned_reads.begin(); it2 != aligned_reads.end(); it2++) {
        training_data.mode.push_back(unique_mode);
        training_data.period.push_back(period);
        training_data.copynum.push_back((*it2).diffFromRef);
        training_data.mutated.push_back((*it2).diffFromRef
                                        != unique_mode);
      }
    }
  }

  if (debug) {
    cerr << "Converint to R format" << endl;
  }
  // Convert training data vectors to R vectors
  int td_length = training_data.mode.size();
  r_training_data.mode = Rcpp::NumericVector(td_length);
  r_training_data.period = Rcpp::NumericVector(td_length);
  r_training_data.copynum = Rcpp::NumericVector(td_length);
  r_training_data.mutated = Rcpp::NumericVector(td_length);
  for (int i = 0; i < td_length; i++) {
    r_training_data.mode[i] = training_data.mode.at(i);
    r_training_data.period[i] = training_data.period.at(i);
    r_training_data.copynum[i] = training_data.copynum.at(i);
    r_training_data.mutated[i] = training_data.mutated.at(i);
  }
  (*R)["mode"] = r_training_data.mode;
  (*R)["period"] = r_training_data.period;
  (*R)["copynum"] = r_training_data.copynum;
  (*R)["mutated"] = r_training_data.mutated;

  if (debug) {
    cerr << "Fitting models..." << endl;
  }
  // Fit model
  FitMutationProb();
  FitStepProb();
}

bool NoiseModel::HasUniqueMode(const list<AlignedRead>&
                               aligned_reads,
                               float* mode) {
  if (aligned_reads.size() == 1) {
    *mode = aligned_reads.front().diffFromRef;
    return true;
  }
  int top_copy_count;
  float top_copy;
  int second_copy_count;
  float second_copy;
  map<float, int> copy_to_count;
  for (list<AlignedRead>::const_iterator it = aligned_reads.begin();
       it != aligned_reads.end(); it++) {
    float copy = (*it).diffFromRef;
    copy_to_count[copy]++;
  }
  for (map<float, int>::const_iterator it = copy_to_count.begin();
       it != copy_to_count.end(); it++) {
    int count = it->second;
    float copy = it->first;
    if (count > top_copy_count) {
      second_copy_count = top_copy_count;
      second_copy = top_copy;
      top_copy_count = count;
      top_copy = copy;
    } else if (count > second_copy_count) {
      second_copy = copy;
      second_copy_count = count;
    }
  }
  if (top_copy_count > second_copy_count) {
    *mode = top_copy;
    return true;
  }
  return false;
}

void NoiseModel::FitMutationProb() {
  if (debug) {
    cerr << "Fitting mutation prob..." << endl;
  }
  Rcpp::NumericVector periods(MAX_PERIOD-MIN_PERIOD+1);
  for (int i = 0; i < MAX_PERIOD-MIN_PERIOD+1; i++) {
    periods[i] = i+MIN_PERIOD;
  }
  (*R)["all_periods"] = periods;

  // Do logistic regression
  string evalString = "mut_model = glm(mutated~period, " \
    "family='binomial'); \n" \
    "mutIntercept = mut_model$coefficients[[1]]; \n" \
    "mutSlope = mut_model$coefficients[[2]]; \n" \
    "c(mutIntercept, mutSlope)";
  if (debug) {
    cerr << evalString << endl;
  }
  SEXP ans = (*R).parseEval(evalString);  // return intercept, slope
  Rcpp::NumericVector v(ans);
  mutIntercept = v[0];
  mutSlope = v[1];
}

void NoiseModel::FitStepProb() {
  if (debug) {
    cerr << "Fitting step prob..." << endl;
  }
  string evalString = "data = data.frame(mutated=mutated, " \
    "period = period, copynum = copynum, mode = mode); \n"  \
    "mutated_data = data[data$mutated,];\n" \
    "mutated_data$step = abs(mutated_data$copynum - mutated_data$mode); \n" \
    "step_model = glm(mutated_data$step ~ " \
    "mutated_data$period, family='poisson'); \n"      \
    "poisIntercept = step_model$coefficients[[1]];\n" \
    "poisSlope = step_model$coefficients[[2]];\n" \
    "if (is.na(poisSlope)) {poisSlope = 0};\n" \
    "c(poisIntercept, poisSlope)";
  if (debug) {
    cerr << evalString << endl;
  }
  SEXP ans = (*R).parseEval(evalString);  // return intercept, slope
  Rcpp::NumericVector v(ans);
  poisIntercept = v[0];
  poisSlope = v[1];
}

float NoiseModel::GetTransitionProb(int a,
                                    int b, int period) {
  float mutProb = 1-invLogit(mutIntercept + mutSlope*period);
  float poisMean = exp(poisIntercept + poisSlope*period);
  if (debug) {
    cerr << "[GetTransitionProb]: " << a << " " << b << " "
         << period << " " << mutProb << " " << poisMean << endl;
  }
  float score;
  if (a == b) {
    score = (1-mutProb);
  } else {
    int diff = abs(b-a);
    score = (mutProb)*ppois(diff-1, poisMean);
    if (diff%period != 0) score = score*NONUNIT_PENALTY;
  }
  if (debug) {
    cerr << "[GetTransitionProb]: " << score << endl;
  }
  return score;
}

bool NoiseModel::ReadNoiseModelFromFile(string filename) {
  TextFileReader nFile(filename);
  string line;
  bool mI = false;
  bool mS = false;
  bool pI = false;
  bool pS = false;
  while (nFile.GetNextLine(&line)) {
    vector<string> items;
    split(line, '=', items);
    if (debug) {
      cerr << "reading line " << line << " " << items.size() << endl;
    }
    if (items.size() == 0) break;
    if (items.size() != 2) return false;
    if (items.at(0) == "mutIntercept") {
      mutIntercept = atof(items.at(1).c_str());
      mI = true;
    }
    if (items.at(0) == "mutSlope") {
      mutSlope = atof(items.at(1).c_str());
      mS = true;
    }
    if (items.at(0) == "poisIntercept") {
      poisIntercept = atof(items.at(1).c_str());
      pI = true;
    }
    if (items.at(0) == "poisSlope") {
      poisSlope = atof(items.at(1).c_str());
      pS = true;
    }
  }
  if (!(mI && mS && pI && pS)) return false;
  return true;
}

bool NoiseModel::WriteNoiseModelToFile(string filename) {
  TextFileWriter nWriter(filename);
  stringstream ss;
  ss << "mutIntercept=" << mutIntercept << endl
     << "mutSlope=" << mutSlope << endl
     << "poisIntercept=" << poisIntercept << endl
     << "poisSlope=" << poisSlope << endl;
  nWriter.Write(ss.str());
  return true;
}

NoiseModel::~NoiseModel() {}

