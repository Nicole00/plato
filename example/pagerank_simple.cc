/*
  Tencent is pleased to support the open source community by making
  Plato available.
  Copyright (C) 2019 THL A29 Limited, a Tencent company.
  All rights reserved.

  Licensed under the BSD 3-Clause License (the "License"); you may
  not use this file except in compliance with the License. You may
  obtain a copy of the License at

  https://opensource.org/licenses/BSD-3-Clause

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" basis,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
  implied. See the License for the specific language governing
  permissions and limitations under the License.

  See the AUTHORS file for names of contributors.
*/

#include <cstdint>
#include <cstdlib>

#include "glog/logging.h"
#include "gflags/gflags.h"

#include "boost/format.hpp"
#include "boost/iostreams/stream.hpp"
#include "boost/iostreams/filter/gzip.hpp"
#include "boost/iostreams/filtering_stream.hpp"

#include "plato/graph/graph.hpp"
#include "plato/algo/pagerank/pagerank.hpp"

DEFINE_string(input,       "",      "input file, in csv format, without edge data");
DEFINE_string(output,      "",      "output directory");
DEFINE_bool(is_directed,   false,   "is graph directed or not");
DEFINE_bool(part_by_in,    false,   "partition by in-degree");
DEFINE_int32(alpha,        -1,      "alpha value used in sequence balance partition");
DEFINE_uint64(iterations,  100,     "number of iterations");
DEFINE_double(damping,     0.85,    "the damping factor");

DEFINE_double(
  eps,
  0.001,
  "the calculation will be considered complete if the sum of "
  "the difference of the 'rank' value between iterations changes "
  "less than 'eps'. if 'eps' equals to 0, pagerank will be "
  "force to execute 'iteration' epochs."
);

bool string_not_empty(const char*, const std::string& value) {
  if (0 == value.length()) { return false; }
  return true;
}

DEFINE_validator(input,  &string_not_empty);
DEFINE_validator(output, &string_not_empty);

void init(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::LogToStderr();
}

int main(int argc, char** argv) {
  plato::stop_watch_t watch;
  auto& cluster_info = plato::cluster_info_t::get_instance();

  init(argc, argv);
  cluster_info.initialize(&argc, &argv);

  watch.mark("t0");

  plato::graph_info_t graph_info(FLAGS_is_directed);
  auto pdcsc = plato::create_dcsc_seqs_from_path<plato::empty_t, plato::vid_t, plato::edge_file_cache_t>(
    &graph_info, FLAGS_input, plato::edge_format_t::CSV,
    plato::dummy_decoder<plato::empty_t>, FLAGS_alpha, FLAGS_part_by_in
  );

  using graph_spec_t = std::remove_reference<decltype(*pdcsc)>::type;

  plato::algo::pagerank_opts_t opts;
  opts.iteration_ = FLAGS_iterations;
  opts.damping_   = FLAGS_damping;
  opts.eps_       = FLAGS_eps;

  auto ranks = plato::algo::pagerank<graph_spec_t>(*pdcsc, graph_info, opts);

  if (0 == cluster_info.partition_id_) {
    LOG(INFO) << "pagerank calculation done: " << watch.show("t0") / 1000.0 << "s";
  }

  watch.mark("t0");
  {  // save result to hdfs
    std::vector<std::unique_ptr<plato::hdfs_t::fstream>> fs_v(cluster_info.threads_);
    std::vector<std::unique_ptr<boost::iostreams::filtering_stream<boost::iostreams::output>>>
      fs_output_v(cluster_info.threads_);

    for (int i = 0; i < cluster_info.threads_; ++i) {
      fs_v[i].reset(new plato::hdfs_t::fstream(plato::hdfs_t::get_hdfs(FLAGS_output),
          (boost::format("%s/%04d_%04d.csv.gz") % FLAGS_output.c_str() % cluster_info.partition_id_ % i).str(), true));

      fs_output_v[i].reset(new boost::iostreams::filtering_stream<boost::iostreams::output>());
      fs_output_v[i]->push(boost::iostreams::gzip_compressor());
      fs_output_v[i]->push(*fs_v[i]);
    }

    ranks.foreach<int> (
      [&](plato::vid_t v_i, double* pval) {
        static thread_local auto& fs_output = fs_output_v[omp_get_thread_num()];
        *fs_output << v_i << "," << *pval << "\n";
        return 0;
      }
    );
  }
  if (0 == cluster_info.partition_id_) {
    LOG(INFO) << "save result cost: " << watch.show("t1") / 1000.0 << "s";
  }

  return 0;
}
