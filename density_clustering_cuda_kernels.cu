
#include "density_clustering_cuda_kernels.hpp"

namespace Clustering {
namespace Density {
namespace CUDA {
namespace Kernel {

  __global__ void
  population_count(unsigned int offset
                 , float* coords
                 , unsigned int n_rows
                 , unsigned int n_cols
                 , float* radii2
                 , unsigned int n_radii
                 , unsigned int* pops
                 , unsigned int i_from
                 , unsigned int i_to) {
    extern __shared__ float smem[];
    unsigned int bid = blockIdx.x;
    unsigned int tid = threadIdx.x;
    unsigned int bsize = blockDim.x;
    unsigned int gid = bid * bsize + tid + i_from;
    // load frames for comparison into shared memory
    int comp_size = min(bsize, n_rows - offset);
    if (tid < comp_size) {
      for (unsigned int j=0; j < n_cols; ++j) {
        smem[tid*n_cols+j] = coords[(tid+offset)*n_cols+j];
      }
    }
    __syncthreads();
    // count neighbors
    if (gid < i_to) {
      unsigned int ref_id = tid+bsize;
      // load reference coordinates for re-use into shared memory
      for (unsigned int j=0; j < n_cols; ++j) {
        smem[ref_id*n_cols+j] = coords[gid*n_cols+j];
      }
      for (unsigned int r=0; r < n_radii; ++r) {
        unsigned int local_pop = 0;
        float rad2 = radii2[r];
        for (unsigned int i=0; i < comp_size; ++i) {
          float dist2 = 0.0f;
          for (unsigned int j=0; j < n_cols; ++j) {
            float c = smem[ref_id*n_cols+j] - smem[i*n_cols+j];
            dist2 = fma(c, c, dist2);
          }
          if (dist2 <= rad2) {
            ++local_pop;
          }
        }
        // update frame populations (per radius)
        pops[r*n_rows+gid] += local_pop;
      }
    }
  }

  __global__ void
  nearest_neighbor_search(unsigned int offset
                        , float* coords
                        , unsigned int n_rows
                        , unsigned int n_cols
                        , float* fe
                        , float* nh_dist_ndx
                        , float* nhhd_dist_ndx
                        , unsigned int i_from
                        , unsigned int i_to) {
    extern __shared__ float smem[];
    unsigned int bid = blockIdx.x;
    unsigned int tid = threadIdx.x;
    unsigned int bsize = blockDim.x;
    unsigned int gid = bid * bsize + tid + i_from;

    float nh_mindist;
    float nh_minndx;
    float nhhd_mindist;
    float nhhd_minndx;
    float ref_fe;
    unsigned int ref_id;

    // load frames for comparison into shared memory
    int comp_size = min(bsize, n_rows - offset);
    if (tid < comp_size) {
      for (unsigned int j=0; j < n_cols; ++j) {
        smem[tid*n_cols+j] = coords[(tid+offset)*n_cols+j];
      }
    }
    __syncthreads();

    if (gid < i_to) {
      ref_id = tid+bsize;
      // load reference coordinates for re-use into shared memory
      for (unsigned int j=0; j < n_cols; ++j) {
        smem[ref_id*n_cols+j] = coords[gid*n_cols+j];
      }
      ref_fe = fe[gid];
      // load current best mindists into registers
      nh_mindist = nh_dist_ndx[gid];
      nh_minndx = nh_dist_ndx[n_rows+gid];
      nhhd_mindist = nhhd_dist_ndx[gid];
      nhhd_minndx = nhhd_dist_ndx[n_rows+gid];
      // compare squared distances of reference
      // compare squared distances of reference
      // to (other) frames in shared mem
      for (unsigned int i=0; i < comp_size; ++i) {
        float dist2=0.0f;
        for (unsigned int j=0; j < n_cols; ++j) {
          float c = smem[ref_id*n_cols+j] - smem[i*n_cols+j];
          dist2 = fma(c, c, dist2);
        }
        // frame with min distance (i.e. nearest neighbor)
        if ((nh_mindist == 0)
         || (dist2 < nh_mindist && dist2 != 0)) {
          nh_mindist = dist2;
          nh_minndx = i+offset;
        }
        // frame with min distance and lower energy
        if ((nhhd_mindist == 0 && fe[i+offset] < ref_fe)
         || (dist2 < nhhd_mindist && fe[i+offset] < ref_fe && dist2 != 0)) {
          nhhd_mindist = dist2;
          nhhd_minndx = i+offset;
        }
      }
      // write results (dist & ndx) to global buffers
      nh_dist_ndx[gid] = nh_mindist;
      nh_dist_ndx[n_rows+gid] = nh_minndx;
      nhhd_dist_ndx[gid] = nhhd_mindist;
      nhhd_dist_ndx[n_rows+gid] = nhhd_minndx;
    }
  }

  __global__ void
  screening(unsigned int offset
          , float* sorted_coords
          , unsigned int n_rows
          , unsigned int n_cols
          , float max_dist2
          , unsigned int* clustering
          , unsigned int i_from
          , unsigned int i_to) {
    // dynamic shared mem for ref coords
    extern __shared__ float smem_coords[];
    // static shared mem for temp. results
    //   schema (N == BSIZE_SCR):
    //     N rows, one for each tid
    //     N + 3 columns:
    //       one for each reference frame (1-N)
    //       one for the previous clustering result (N+1)
    //       one for the current best clustering (N+2)
    //       one for intermediate col-bests (N+3)
    // addressing is col-oriented, i.e. smem_cache[N*i_col + i_row]


//    const unsigned int col_prev = BSIZE_SCR*BSIZE_SCR;
//    const unsigned int col_result = col_prev + BSIZE_SCR;
//    const unsigned int col_inter = col_result + BSIZE_SCR;
//    __shared__ unsigned int smem_cache[BSIZE_SCR*(BSIZE_SCR+3)];


    const unsigned int col_result = 0;
    const unsigned int col_inter = BSIZE_SCR;
    __shared__ unsigned int smem_cache[2*BSIZE_SCR];


    // thread dimensions
    unsigned int bid = blockIdx.x;
    unsigned int tid = threadIdx.x;
    unsigned int bsize = blockDim.x;
    unsigned int gid = bid * bsize + tid + i_from;
    int comp_size = min(bsize, n_rows - offset);
    if (tid < comp_size) {
      // load reference coordinates to cache
      for (unsigned int j=0; j < n_cols; ++j) {
        smem_coords[tid*n_cols+j] = sorted_coords[(tid+offset)*n_cols+j];
      }
      // load reference state information to cache
      smem_cache[col_inter+tid] = clustering[tid+offset];
    }
    __syncthreads();
    if (gid < i_to) {
      for (unsigned int j=0; j < n_cols; ++j) {
        // load coordinates of current frame for re-use into shared memory
        smem_coords[(tid+bsize)*n_cols+j] = sorted_coords[gid*n_cols+j];
      }
      // load previous state information to cache
      unsigned int tmp_state = clustering[gid];
      unsigned int tmp_result = tmp_state;
//      smem_cache[col_prev+tid] = tmp_state;
      smem_cache[col_result+tid] = tmp_state;
      // compare current frame (tid) against reference block (k)
      for (unsigned int k=0; k < comp_size; ++k) {
        float dist2 = 0.0f;
        for (unsigned int j=0; j < n_cols; ++j) {
          float c = smem_coords[(tid+bsize)*n_cols+j]
                  - smem_coords[          k*n_cols+j];
          dist2 = fma(c, c, dist2);
        }
        // fill cache with intermediate results
        if (dist2 < max_dist2) {
          tmp_state = smem_cache[col_inter+k];
          tmp_result = min(tmp_state, tmp_result);
 //         smem_cache[k*BSIZE_SCR+tid] = tmp_state;
 //       } else {
 //         smem_cache[k*BSIZE_SCR+tid] = 0;
        }
      }
      smem_cache[col_result+tid] = tmp_result;
    }
    __syncthreads();
    //// following code blocks essentially perform an inner join
    //// of reference states to find min. ids & lump corresponding states
//    if (tid < comp_size) {
//      unsigned int tmp_inter = 0;
//      // tid == reference state
//      for (unsigned int i=0; i < comp_size; ++i) {
//        if (smem_cache[tid*BSIZE_SCR+i] != 0) {
//          if (tmp_inter == 0) {
//            tmp_inter = smem_cache[col_result+i];
//          } else {
//            tmp_inter = min(tmp_inter
//                          , smem_cache[col_result+i]);
//          }
//        }
//      }
//      smem_cache[col_inter+tid] = tmp_inter;
//    }
//    __syncthreads();
//    if (tid < comp_size) {
//      unsigned int tmp_result = smem_cache[col_result+tid];
//      // tid == current frame
//      for (unsigned int k=0; k < comp_size; ++k) {
//        if (smem_cache[k*BSIZE_SCR+tid] != 0) {
//          unsigned int tmp_inter = smem_cache[col_inter+k];
//          if (tmp_inter != 0) {
//            tmp_result = min(tmp_result
//                           , tmp_inter);
//          }
//        }
//      }
//      smem_cache[col_result+tid] = tmp_result;
//    }
//    __syncthreads();
//    if (tid < comp_size) {
//      unsigned int tmp_inter = smem_cache[col_inter+tid];
//      // tid == reference state
//      for (unsigned int i=0; i < comp_size; ++i) {
//        if (smem_cache[tid*BSIZE_SCR+i] != 0) {
//          if (tmp_inter == 0) {
//            tmp_inter = smem_cache[col_result+i];
//          } else {
//            tmp_inter = min(tmp_inter
//                          , smem_cache[col_result+i]);
//          }
//        }
//      }
//      smem_cache[col_inter+tid] = tmp_inter;
//    }
//    __syncthreads();
    ////////

    // update result for given frame
    if (gid < i_to) {
      clustering[gid] = smem_cache[col_result+tid];
    }
    //// update reference states (either from comparison block
    //// or previous best results) under protection against race-conditions.
    //// (since they may be updated from several, parallel blocks)


//TODO debug
//    if (tid == 0) {
//      for (unsigned int k=0; k < comp_size; ++k) {
//        unsigned int tmp_inter = smem_cache[col_inter+k];
//        // update reference
//        if (tmp_inter != 0
//         && clustering[k+offset] != tmp_inter) {
//          atomicMin(&clustering[k+offset]
//                  , tmp_inter);
//        }
//      }
//    }
//    if (tid == 1) {
//      for (unsigned int k=0; k < comp_size; ++k) {
//        unsigned int tmp_result = smem_cache[col_result+k];
//        unsigned int tmp_prev = smem_cache[col_prev+k];
//        // update prev best results
//        if (tmp_prev != 0
//         && tmp_prev != tmp_result) {
//          atomicMin(&clustering[tmp_prev-1]
//                  , tmp_result);
//        }
//      }
//    }
  }


}}}} // end Clustering::Density::CUDA::Kernel

