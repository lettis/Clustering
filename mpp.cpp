/*
Copyright (c) 2015, Florian Sittel (www.lettis.net)
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "tools.hpp"
#include "mpp.hpp"
#include "logger.hpp"

namespace Clustering {
  namespace MPP {
    SparseMatrixF
    transition_counts(std::vector<std::size_t> trajectory,
                      std::vector<std::size_t> concat_limits,
                      std::size_t n_lag_steps,
                      std::size_t i_max) {
      if (n_lag_steps == 0) {
        std::cerr << "error: lagtime of 0 does not make any sense for"
                  << " MPP clustering" << std::endl;
        exit(EXIT_FAILURE);
      }
      std::vector<std::size_t>::iterator next_limit = concat_limits.begin();
      if (i_max == 0) {
        i_max = (*std::max_element(trajectory.begin()
                                 , trajectory.end()));
      }
      SparseMatrixF count_matrix(i_max+1, i_max+1);
      for (std::size_t i=0; i < trajectory.size() - n_lag_steps; ++i) {
        std::size_t from = trajectory[i];
        std::size_t to = trajectory[i+n_lag_steps];
        if (next_limit != concat_limits.end()) {
          // check for sub-trajectory limits
          if (i+n_lag_steps < (*next_limit)) {
            count_matrix(from, to) += 1;
          } else if (i+1 == (*next_limit)) {
            ++next_limit;
          }
        } else {
          // either last sub-trajectory or everything is
          // a single, continuous trajectory
          count_matrix(from, to) += 1;
        }
      }
      return count_matrix;
    }

    SparseMatrixF
    weighted_transition_counts(std::vector<std::size_t> trajectory
                             , std::vector<std::size_t> concat_limits
                             , std::size_t n_lag_steps) {
      // get max index (max. matrix size == max index+1)
      std::size_t i_max = (*std::max_element(trajectory.begin()
                                           , trajectory.end()));
      SparseMatrixF weighted_counts(i_max+1, i_max+1);
      std::vector<float> acc_weights(i_max+1);
      std::size_t lower_lim = 0;
      for (std::size_t i_chunk=0; i_chunk < concat_limits.size(); ++i_chunk) {
        // compute count matrix per chunk
        std::size_t upper_lim = lower_lim + concat_limits[i_chunk];
        std::vector<std::size_t> chunk = std::vector<std::size_t>(
                                           concat_limits.begin()+lower_lim
                                         , concat_limits.begin()+upper_lim);
        SparseMatrixF counts = transition_counts(chunk
                                               , {}
                                               , n_lag_steps
                                               , i_max);
        // compute weights for this chunk
        std::vector<float> weights(i_max+1);
        for (std::size_t i=0; i < i_max+1; ++i) {
          for (std::size_t j=0; j < i_max+1; ++j) {
            weights[i] += counts(i,j);
          }
          weights[i] = sqrt(weights[i]);
          acc_weights[i] += weights[i];
        }
        // add weighted counts to end result
        for (std::size_t i=0; i < i_max+1; ++i) {
          for (std::size_t j=0; j < i_max+1; ++j) {
            weighted_counts(i,j) += weights[i]*counts(i,j);
          }
        }
        lower_lim = upper_lim;
      }
      // re-weight end result
      for (std::size_t i=0; i < i_max+1; ++i) {
        for (std::size_t j=0; j < i_max+1; ++j) {
          weighted_counts(i,j) /= acc_weights[i];
        }
      }
      return weighted_counts;
    }

    SparseMatrixF
    row_normalized_transition_probabilities(SparseMatrixF count_matrix
                                          , std::set<std::size_t> cluster_names) {
      std::size_t n_rows = count_matrix.size1();
      std::size_t n_cols = count_matrix.size2();
      SparseMatrixF transition_matrix(n_rows, n_cols);
      for (std::size_t i: cluster_names) {
        std::size_t row_sum = 0;
        for (std::size_t j=0; j < n_cols; ++j) {
          row_sum += count_matrix(i,j);
        }
        if (row_sum > 0) {
          for (std::size_t j=0; j < n_cols; ++j) {
            if (count_matrix(i,j) != 0) {
              transition_matrix(i,j) = count_matrix(i,j) / row_sum;
            }
          }
        }
      }
      return transition_matrix;
    }

    SparseMatrixF
    updated_transition_probabilities(SparseMatrixF transition_matrix
                                   , std::map<std::size_t, std::size_t> sinks) {
      std::size_t n_rows = transition_matrix.size1();
      std::size_t n_cols = transition_matrix.size2();
      SparseMatrixF updated_matrix(n_rows
                                 , n_cols);
      // macrostates == states left after lumping
      std::set<std::size_t> macrostates;
      // microstates == states before lumping
      // (with map-key being the name of the macrostate they are lumped into)
      std::map<std::size_t, std::set<std::size_t>> microstates;
      for (auto lump_from_to: sinks) {
        macrostates.insert(lump_from_to.second);
        if (microstates.count(lump_from_to.second) == 0) {
          microstates[lump_from_to.second] = {lump_from_to.first};
        } else {
          microstates[lump_from_to.second].insert(lump_from_to.first);
        }
      }
      // construct new transition matrix by summing over all transition
      // probabilities from one macrostate to another macrostate
      for (auto macro1: macrostates) {
        for (auto macro2: macrostates) {
          for (auto micro1: microstates[macro1]) {
            for (auto micro2: microstates[macro2]) {
              updated_matrix(macro1, macro2) += transition_matrix(micro1, micro2);
            }
          }
        }
      }
      return updated_matrix;
    }

    std::map<std::size_t, std::size_t>
    single_step_future_state(SparseMatrixF transition_matrix,
                             std::set<std::size_t> cluster_names,
                             float q_min,
                             std::map<std::size_t, float> min_free_energy) {
      std::map<std::size_t, std::size_t> future_state;
      for (std::size_t i: cluster_names) {
        std::vector<std::size_t> candidates;
        float max_trans_prob = 0.0f;
        if (transition_matrix(i,i) >= q_min) {
          // self-transition is greater than stability measure: stay.
          candidates = {i};
        } else {
          for (std::size_t j: cluster_names) {
            // self-transition lower than q_min:
            // choose other state as immidiate future
            // (even if it has lower probability than self-transition)
            if (i != j) {
              if (transition_matrix(i,j) > max_trans_prob) {
                max_trans_prob = transition_matrix(i,j);
                candidates = {j};
              } else if (transition_matrix(i,j) == max_trans_prob
                      && max_trans_prob > 0.0f) {
                candidates.push_back(j);
              }
            }
          }
        }
        if (candidates.size() == 0) {
          std::cerr << "error: state '"
                    << i
                    << "' has self-transition probability of "
                    << transition_matrix(i,i)
                    << " at Qmin " 
                    << q_min
                    << " and does not find any transition candidates."
                    << " please have a look at your trajectory!"
                    << std::endl;
          exit(EXIT_FAILURE);
        } else if (candidates.size() == 1) {
          future_state[i] = candidates[0];
        } else {
          // multiple candidates: choose the one with lowest Free Energy
          auto min_fe_compare = [&](std::size_t i, std::size_t j) {
            return min_free_energy[i] < min_free_energy[j];
          };
          future_state[i] = (*std::min_element(candidates.begin()
                                             , candidates.end()
                                             , min_fe_compare));
        }
      }
      return future_state;
    }

    std::map<std::size_t, std::vector<std::size_t>>
    most_probable_path(std::map<std::size_t, std::size_t> future_state,
                       std::set<std::size_t> cluster_names) {
      std::map<std::size_t, std::vector<std::size_t>> mpp;
      for (std::size_t i: cluster_names) {
        std::vector<std::size_t> path = {i};
        std::set<std::size_t> visited = {i};
        std::size_t next_state = future_state[i];
        // abort when path 'closes' in a loop, i.e.
        // when a state has been revisited
        while (visited.count(next_state) == 0) {
          path.push_back(next_state);
          visited.insert(next_state);
          next_state = future_state[next_state];
        }
        mpp[i] = path;
      }
      return mpp;
    }

    std::map<std::size_t, std::size_t>
    microstate_populations(std::vector<std::size_t> clusters,
                           std::set<std::size_t> cluster_names) {
      std::map<std::size_t, std::size_t> pops;
      for (std::size_t i: cluster_names) {
        pops[i] = std::count(clusters.begin(), clusters.end(), i);
      }
      return pops;
    }

    // assign every state the lowest free energy value
    // of all of its frames.
    std::map<std::size_t, float>
    microstate_min_free_energy(const std::vector<std::size_t>& clustering,
                               const std::vector<float>& free_energy) {
      std::map<std::size_t, float> min_fe;
      for (std::size_t i=0; i < clustering.size(); ++i) {
        std::size_t state = clustering[i];
        if (min_fe.count(state) == 0) {
          min_fe[state] = free_energy[i];
        } else {
          if (free_energy[i] < min_fe[state]) {
            min_fe[state] = free_energy[i];
          }
        }
      }
      return min_fe;
    }

    std::map<std::size_t, std::size_t>
    path_sinks(std::vector<std::size_t> clusters,
               std::map<std::size_t, std::vector<std::size_t>> mpp,
               SparseMatrixF transition_matrix,
               std::set<std::size_t> cluster_names,
               float q_min,
               std::vector<float> free_energy) {
      std::map<std::size_t, std::size_t> pops;
      pops = microstate_populations(clusters, cluster_names);
      std::map<std::size_t, float> min_free_energy;
      min_free_energy = microstate_min_free_energy(clusters, free_energy);
      std::map<std::size_t, std::size_t> sinks;
      for (std::size_t i: cluster_names) {
        std::vector<std::size_t> metastable_states;
        for (std::size_t j: mpp[i]) {
          // check: are there stable states?
          if (transition_matrix(j,j) > q_min) {
            metastable_states.push_back(j);
          }
        }
        if (metastable_states.size() == 0) {
          // no stable state: treat all states in path as 'metastable'
          metastable_states = mpp[i];
        }
        // helper function: compare states by their population
        auto pop_compare = [&](std::size_t i, std::size_t j) -> bool {
          return pops[i] < pops[j];
        };
        // helper function: compare states by their min. Free Energy
        auto fe_compare = [&](std::size_t i, std::size_t j) -> bool {
          return min_free_energy[i] < min_free_energy[j];
        };
        // find sink candidate state from lowest free energy
        auto candidate = std::min_element(metastable_states.begin()
                                        , metastable_states.end()
                                        , fe_compare);
        float min_fe = free_energy[*candidate];
        std::set<std::size_t> sink_candidates;
        while (candidate != metastable_states.end()
            && free_energy[*candidate] == min_fe) {
          // there may be several states with same (min.) free energy,
          // collect them all into one set
          sink_candidates.insert(*candidate);
          metastable_states.erase(candidate);
          candidate = std::min_element(metastable_states.begin()
                                     , metastable_states.end()
                                     , fe_compare);
        }
        // select sink by lowest free energy
        if (sink_candidates.size() == 1) {
          sinks[i] = (*sink_candidates.begin());
        } else {
          // or highest population, if equal
          sinks[i] = (*std::max_element(sink_candidates.begin()
                                      , sink_candidates.end()
                                      , pop_compare));
        }
      }
      return sinks;
    }

    // lump states based on path sinks and return new trajectory.
    // new microstates will have IDs of sinks.
    std::vector<std::size_t>
    lumped_trajectory(std::vector<std::size_t> trajectory,
                      std::map<std::size_t, std::size_t> sinks) {
      for (std::size_t& state: trajectory) {
        state = sinks[state];
      }
      return trajectory;
    }

    // run clustering for given Q_min value
    // returns: {new traj, lumping info}
    std::tuple<std::vector<std::size_t>, std::map<std::size_t, std::size_t>>
    fixed_metastability_clustering(std::vector<std::size_t> initial_trajectory,
                                   std::vector<std::size_t> concat_limits, //TODO: not needed here anymore, instead: trans_prob
                                   bool diff_size_chunks,
                                   float q_min,
                                   std::size_t lagtime,
                                   std::vector<float> free_energy) {
      std::set<std::size_t> microstate_names;
      std::vector<std::size_t> traj = initial_trajectory;
      std::map<std::size_t, std::size_t> lumping;
      const uint MAX_ITER=100;
      uint iter;
      for (iter=0; iter < MAX_ITER; ++iter) {
        // reset names in case of vanished states (due to lumping)
        microstate_names = std::set<std::size_t>(traj.begin(), traj.end());
        if (microstate_names.count(0)) {
          std::cerr << "\nwarning:\n"
                    << "  there is a state '0' in your trajectory.\n"
                    << "  are you sure you generated a proper"
                    << " trajectory of microstates\n"
                    << "  (e.g. by running a final, seeded"
                    << " density-clustering to fill up the FEL)?\n"
                    << std::endl;
        }
        logger(std::cout) << "iteration "
                          << iter+1
                          << " for q_min "
                          << Clustering::Tools::stringprintf("%0.3f", q_min)
                          << std::endl;



        //TODO: trans_prob as parameter / update after lumping

        // get transition probabilities
        logger(std::cout) << "  calculating transition probabilities" << std::endl;
        SparseMatrixF trans_prob;
        if (diff_size_chunks) {
          trans_prob = row_normalized_transition_probabilities(
                         weighted_transition_counts(traj
                                                  , concat_limits
                                                  , lagtime)
                       , microstate_names);
        } else {
          trans_prob = row_normalized_transition_probabilities(
                         transition_counts(traj
                                         , concat_limits
                                         , lagtime)
                       , microstate_names);
        }




        // get immediate future
        logger(std::cout) << "  calculating future states" << std::endl;
        std::map<std::size_t, std::size_t> future_state;
        future_state = single_step_future_state(trans_prob
                                              , microstate_names
                                              , q_min
                                              , microstate_min_free_energy(
                                                  traj
                                                , free_energy));
        // compute MPP
        logger(std::cout) << "  calculating most probable path" << std::endl;
        std::map<std::size_t, std::vector<std::size_t>> mpp;
        mpp = most_probable_path(future_state, microstate_names);
        // compute sinks (i.e. states with lowest Free Energy per path)
        logger(std::cout) << "  calculating path sinks" << std::endl;
        std::map<std::size_t, std::size_t> sinks = path_sinks(traj
                                                            , mpp
                                                            , trans_prob
                                                            , microstate_names
                                                            , q_min
                                                            , free_energy);
        // lump trajectory into sinks
        std::vector<std::size_t> traj_old = traj;
        logger(std::cout) << "  lumping trajectory" << std::endl;
        traj = lumped_trajectory(traj, sinks);
        for (auto from_to: sinks) {
          std::size_t from = from_to.first;
          std::size_t to = from_to.second;
          if (from != to) {
            lumping[from] = to;
          }
        }


        //TODO: update transition matrix here



        // check convergence
        if (traj_old == traj) {
          break;
        }
      }
      if (iter == MAX_ITER) {
        throw std::runtime_error(Clustering::Tools::stringprintf(
                                   "reached max. no. of iterations"
                                   " for Q_min convergence: %d"
                                 , iter));
      } else {

        //TODO: also return updated transition matrix

        return std::make_tuple(traj, lumping);
      }
    }

    void
    main(boost::program_options::variables_map args) {
      // import IO functions
      using Clustering::Tools::stringprintf;
      using Clustering::Tools::read_clustered_trajectory;
      using Clustering::Tools::read_free_energies;
      using Clustering::Tools::read_single_column;
      using Clustering::Tools::write_single_column;
      using Clustering::Tools::write_map;
      // load initial trajectory, free energies, etc
      std::string basename = args["basename"].as<std::string>();
      std::map<std::size_t, std::pair<std::size_t, float>> transitions;
      std::map<std::size_t, std::size_t> max_pop;
      std::map<std::size_t, float> max_qmin;
      Clustering::logger(std::cout) << "loading microstates" << std::endl;
      std::vector<std::size_t> traj;
      traj = read_clustered_trajectory(args["input"].as<std::string>());
      Clustering::logger(std::cout) << "loading free energies" << std::endl;
      std::string fname_fe_in = args["free-energy-input"].as<std::string>();
      std::vector<float> free_energy = read_free_energies(fname_fe_in);
      float q_min_from = args["qmin-from"].as<float>();
      float q_min_to = args["qmin-to"].as<float>();
      float q_min_step = args["qmin-step"].as<float>();
      int lagtime = args["lagtime"].as<int>();
      std::vector<std::size_t> concat_limits;
      bool diff_sized_chunks = args.count("concat_limits");
      if (diff_sized_chunks) {
        std::string fname_climits = args["concat-limits"].as<std::string>();
        concat_limits = read_single_column<std::size_t>(fname_climits);
      } else if (args.count("concat-nframes")) {
        std::size_t n_frames_per_subtraj;
        n_frames_per_subtraj = args["concat-nframes"].as<std::size_t>();
        for (std::size_t i=n_frames_per_subtraj
           ; i < traj.size()
           ; i += n_frames_per_subtraj) {
          concat_limits.push_back(i);
        }
      }
      Clustering::logger(std::cout) << "beginning q_min loop" << std::endl;
      for (float q_min=q_min_from; q_min <= q_min_to; q_min += q_min_step) {
        auto traj_sinks = fixed_metastability_clustering(traj
                                                       , concat_limits
                                                       , diff_sized_chunks
                                                       , q_min
                                                       , lagtime
                                                       , free_energy);
        // write trajectory at current Qmin level to file
        traj = std::get<0>(traj_sinks);
        write_single_column(stringprintf("%s_traj_%0.3f.dat"
                                       , basename.c_str()
                                       , q_min)
                          , traj);
        // save transitions (i.e. lumping of states)
        std::map<std::size_t, std::size_t> sinks = std::get<1>(traj_sinks);
        for (auto from_to: sinks) {
          transitions[from_to.first] = {from_to.second, q_min};
        }
        // write microstate populations to file
        std::map<std::size_t, std::size_t> pops;
        pops = Clustering::Tools::microstate_populations(traj);
        write_map<std::size_t, std::size_t>(stringprintf("%s_pop_%0.3f.dat"
                                                       , basename.c_str()
                                                       , q_min)
                                          , pops);
        // collect max. pops + max. q_min per microstate
        for (std::size_t id: std::set<std::size_t>(traj.begin(), traj.end())) {
          max_pop[id] = pops[id];
          max_qmin[id] = q_min;
        }
      }
      // write transitions to file
      {
        std::ofstream ofs(basename + "_transitions.dat");
        for (auto trans: transitions) {
          ofs << trans.first
              << " "
              << trans.second.first
              << " "
              << trans.second.second
              << "\n";
        }
      }
      write_map<std::size_t, std::size_t>(basename + "_max_pop.dat", max_pop);
      write_map<std::size_t, float>(basename + "_max_qmin.dat", max_qmin);
    }
  } // end namespace MPP
} // end namespace Clustering

