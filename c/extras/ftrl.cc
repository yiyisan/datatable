//------------------------------------------------------------------------------
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// © H2O.ai 2018
//------------------------------------------------------------------------------
#define dt_EXTRAS_FTRL_cc
#include "extras/ftrl.h"
#include "frame/py_frame.h"
#include "py_utils.h"
#include "python/obj.h"
#include "rowindex.h"
#include "types.h"
#include "utils/parallel.h"
#include "utils/shared_mutex.h"
#include <stdio.h>


/*
*  Reading data from Python, training model and making predictions.
*/
PyObject* ftrl(PyObject*, PyObject* args) {

  double a, b, l1, l2;
  uint64_t d;
  size_t n_epochs;
  unsigned int seed, hash_type;
  bool inter;
  PyObject* arg1;
  PyObject* arg2;

  if (!PyArg_ParseTuple(args, "OOddddLnpII:ftrl", &arg1, &arg2, &a, &b, &l1, &l2,
                        &d, &n_epochs, &inter, &hash_type, &seed)) return nullptr;

  DataTable* dt_train = py::obj(arg1).to_frame();
  DataTable* dt_test = py::obj(arg2).to_frame();

  Ftrl ft(a, b, l1, l2, d, n_epochs, inter, hash_type, seed);
  ft.train(dt_train);
  DataTable* dt_target = ft.test(dt_test).release();
  py::Frame* frame_target = py::Frame::from_datatable(dt_target);

  return frame_target;
}


/*
*  Setting up FTRL parameters and initializing weights.
*/
Ftrl::Ftrl(double a_in, double b_in, double l1_in, double l2_in,
           uint64_t d_in, size_t nepochs_in, bool inter_in, unsigned int hash_type_in,
           unsigned int seed_in) :
  a(a_in),
  b(b_in),
  l1(l1_in),
  l2(l2_in),
  d(d_in),
  n_epochs(nepochs_in),
  inter(inter_in),
  hash_type(hash_type_in),
  seed(seed_in)
{
  n = DoublePtr(new double[d]);
  z = DoublePtr(new double[d]);
  w = DoublePtr(new double[d]);

  std::memset(n.get(), 0, d * sizeof(double));
  std::memset(w.get(), 0, d * sizeof(double));
//  std::memset(z.get(), 0, d * sizeof(double));

  // Initialize weights with random numbers from [0; 1]
  srand(seed);
  for (uint64_t i = 0; i < d; ++i){
    z[i] = static_cast<double>(rand()) / RAND_MAX;
  }
}


/*
*  Train FTRL model on training data.
*/
void Ftrl::train(const DataTable* dt) {
  // This includes a bias term, and `dt->ncols - 1` columns, i.e. excluding the target column.
  n_features = static_cast<size_t>(dt->ncols);
  // Number of feature interactions.
  if (inter) {
    n_features_inter = (n_features - 1) * (n_features - 2) / 2;
  } else {
    n_features_inter = 0;
  }

  // Get the target column.
  Column* cy = dt->columns[dt->ncols - 1];
  auto cy_bool = static_cast<BoolColumn*>(cy);
  auto dy_bool = cy_bool->elements_r();

  // Do training for `n_epochs`.
  for (size_t i = 0; i < n_epochs; ++i) {
    double loss = 0;
    int32_t nth = config::nthreads;

    #pragma omp parallel num_threads(nth)
    {
      Uint64Ptr x = Uint64Ptr(new uint64_t[n_features + n_features_inter]);
      x[0] = 0; // Bias term
      int32_t ith = omp_get_thread_num();
      nth = omp_get_num_threads();

      for (size_t j = static_cast<size_t>(ith); j < dt->nrows; j+= static_cast<size_t>(nth)) {
        bool y = dy_bool[j];
        hash_row(x, dt, j);
        double p = predict(x, n_features + n_features_inter);
        double ll = logloss(p, y);

        #pragma omp atomic update
        loss += ll;

        if ((j+1) % REPORT_FREQUENCY == 0) {
          printf("Training epoch: %zu\t row: %zu\t prediction: %f\t loss: %f\t average loss: %f\n",
                  i, j+1, p, ll, loss / (j+1));
        }
        update(x, n_features + n_features_inter, p, y);
      }
    }
  }
}


/*
*  Make predictions on test data and return targets as a new datatable.
*/
dtptr Ftrl::test(const DataTable* dt) {
  // Create a target datatable.
  dtptr dt_target = nullptr;
  Column* col_target = Column::new_data_column(SType::FLOAT64, dt->nrows);
  dt_target = dtptr(new DataTable({col_target}, {"target"}));


  auto d_target = static_cast<double*>(dt_target->columns[0]->data_w());

  int32_t nth = config::nthreads;

  #pragma omp parallel num_threads(nth)
  {
    Uint64Ptr x = Uint64Ptr(new uint64_t[n_features + n_features_inter]);
    x[0] = 0; // Bias term
    int32_t ith = omp_get_thread_num();
    nth = omp_get_num_threads();

    for (size_t j = static_cast<size_t>(ith); j < dt->nrows; j+= static_cast<size_t>(nth)) {
      hash_row(x, dt, j);
      d_target[j] = predict(x, n_features + n_features_inter);
      if ((j+1) % REPORT_FREQUENCY == 0) {
        printf("Testing row: %zu\t prediction: %f\n", j+1, d_target[j]);
      }
    }

  }
  return dt_target;
}


/*
*  Make predictions for a hashed row.
*/
double Ftrl::predict(const Uint64Ptr& x, size_t x_size) {
  double wTx = 0;
  for (size_t j = 0; j < x_size; ++j) {
    size_t i = x[j];
    if (fabs(z[i]) <= l1) {
      w[i] = 0;
    } else {
      w[i] = (signum(z[i]) * l1 - z[i]) / ((b + sqrt(n[i])) / a + l2);
    }
    wTx += w[i];
  }

  return sigmoid(wTx);
}


/*
*  Sigmoid function.
*/
double Ftrl::sigmoid(double x) {
  double res = 1.0 / (1.0 + exp(-x));

  return res;
}


/*
*  Bounded sigmoid function.
*/
double Ftrl::bsigmoid(double x, double b) {
  double res = 1 / (1 + exp(-std::max(std::min(x, b), -b)));

  return res;
}


/*
*  Update weights based on prediction and the actual value.
*/
void Ftrl::update(const Uint64Ptr& x, size_t x_size, double p, bool y) {
  double g = p - y;

  for (size_t j = 0; j < x_size; ++j) {
    size_t i = x[j];
    double sigma = (sqrt(n[i] + g * g) - sqrt(n[i])) / a;
    z[i] += g - sigma * w[i];
    n[i] += g * g;
  }
}


/*
*  Hash string using the specified hash function.
*/
uint64_t Ftrl::hash_string(const char * key, size_t len) {
  uint64_t res;
  switch (hash_type) {
    case 0:  {
                std::string str(key,
                                key + len / sizeof(char));
                res = std::hash<std::string>{}(str);
                break;
             }
    case 2:  res = hash_murmur2(key, len); break;
    case 3:  {
                uint64_t h[2];
                hash_murmur3(key, len, h);
                res = h[0];
                break;
             }
    default: res = hash_murmur2(key, len);
  }
  return res;
}


/*
*  Hash each element within the datatable row.
*/
void Ftrl::hash_row(Uint64Ptr& x, const DataTable* dt, size_t row_id) {
  std::vector<std::string> c_names = dt->get_names();
  uint64_t index;

  for (size_t i = 0; i < n_features - 1; ++i) {
    std::string str;
    Column* c = dt->columns[i];
    LType ltype = info(c->stype()).ltype();

    switch (ltype) {
      case LType::BOOL:    {
                             auto c_bool = static_cast<BoolColumn*>(c);
                             auto d_bool = c_bool->elements_r();
                             index = static_cast<uint64_t>(d_bool[row_id]);
                             break;
                           }
      case LType::INT:     {
                             auto c_int = static_cast<IntColumn<int32_t>*>(c);
                             auto d_int = c_int->elements_r();
                             index = static_cast<uint64_t>(d_int[row_id]);
                             break;
                           }
      case LType::REAL:    {
                             auto c_real = static_cast<RealColumn<double>*>(c);
                             auto d_real = c_real->elements_r();
                             index = hash_double(d_real[row_id]);
                             break;
                           }
      case LType::STRING:  {
                             auto c_string = static_cast<StringColumn<uint32_t>*>(c);
                             auto d_string = c_string->offsets();
                             const char* strdata = c_string->strdata();
                             const char* c_str = strdata + (d_string[row_id - 1] & ~GETNA<uint32_t>());
                             uint32_t l = d_string[row_id] - (d_string[row_id - 1] & ~GETNA<uint32_t>());
                             index = hash_string(c_str, l * sizeof(char));
                             break;
                           }
      default:             throw ValueError() << "Datatype is not supported";
    }
    uint64_t h = hash_string(c_names[i].c_str(), c_names[i].length() * sizeof(char));
    index += h;
    x[i + 1] = index % d;
  }

  size_t count = 0;
  if (inter) {
    for (size_t i = 0; i < n_features - 1; ++i) {
      for (size_t j = i + 1; j < n_features - 1; ++j) {
        std::string s = std::to_string(x[i+1]) + std::to_string(x[j+1]);
        uint64_t h = hash_string(s.c_str(), s.length() * sizeof(char));
        x[n_features + count] = h % d;
        count++;
      }
    }
  }
}


/*
*  Calculate logloss based on a prediction and the actual value.
*/
double Ftrl::logloss(double p, bool y) {
  double epsilon = std::numeric_limits<double>::epsilon();
  p = std::max(std::min(p, 1 - epsilon), epsilon);
  if (y) {
    return -log(p);
  } else {
    return -log(1 - p);
  }
}


/*
*  Calculate signum.
*/
double Ftrl::signum(double x) {
  if (x > 0) return 1;
  if (x < 0) return -1;
  return 0;
}


/*
*  Hash `double` to `size_t` based on its bit representation.
*/
inline __attribute__((always_inline)) uint64_t Ftrl::hash_double(double x) {
  uint64_t* h = reinterpret_cast<uint64_t*>(&x);
  return *h;
}


uint64_t Ftrl::hash_murmur2 (const void * key, uint64_t len) {
  const uint64_t m = 0xc6a4a7935bd1e995;
  const int r = 47;

  uint64_t h = seed ^ (len * m);

  const uint64_t * data =static_cast<const uint64_t*>(key);
  const uint64_t * end = data + (len/8);

  while(data != end) {
    uint64_t k = *data++;

    k *= m;
    k ^= k >> r;
    k *= m;

    h ^= k;
    h *= m;
  }

  const unsigned char * data2 = reinterpret_cast<const unsigned char*>(data);

  switch (len & 7) {
    case 7: h ^= uint64_t(data2[6]) << 48; [[clang::fallthrough]];
    case 6: h ^= uint64_t(data2[5]) << 40; [[clang::fallthrough]];
    case 5: h ^= uint64_t(data2[4]) << 32; [[clang::fallthrough]];
    case 4: h ^= uint64_t(data2[3]) << 24; [[clang::fallthrough]];
    case 3: h ^= uint64_t(data2[2]) << 16; [[clang::fallthrough]];
    case 2: h ^= uint64_t(data2[1]) << 8;  [[clang::fallthrough]];
    case 1: h ^= uint64_t(data2[0]);
            h *= m;
  };

  h ^= h >> r;
  h *= m;
  h ^= h >> r;

  return h;
}

//-----------------------------------------------------------------------------
// MurmurHash3 was written by Austin Appleby, and is placed in the public
// domain. The author hereby disclaims copyright to this source code.


#define BIG_CONSTANT(x) (x##LLU)
inline uint64_t ROTL64 ( uint64_t x, int8_t r ) {
  return (x << r) | (x >> (64 - r));
}


//-----------------------------------------------------------------------------
// Block read - if your platform needs to do endian-swapping or can only
// handle aligned reads, do the conversion here

inline __attribute__((always_inline)) uint64_t getblock64 ( const uint64_t * p, int i ) {
  return p[i];
}

//-----------------------------------------------------------------------------
// Finalization mix - force all bits of a hash block to avalanche

inline __attribute__((always_inline)) uint64_t fmix64 ( uint64_t k ) {
  k ^= k >> 33;
  k *= BIG_CONSTANT(0xff51afd7ed558ccd);
  k ^= k >> 33;
  k *= BIG_CONSTANT(0xc4ceb9fe1a85ec53);
  k ^= k >> 33;

  return k;
}


void Ftrl::hash_murmur3 ( const void * key, const int len, void * out) {
  const uint8_t * data = static_cast<const uint8_t*>(key);
  const int nblocks = len / 16;

  uint64_t h1 = seed;
  uint64_t h2 = seed;

  const uint64_t c1 = BIG_CONSTANT(0x87c37b91114253d5);
  const uint64_t c2 = BIG_CONSTANT(0x4cf5ad432745937f);

  //----------
  // body

  const uint64_t * blocks = reinterpret_cast<const uint64_t *>(data);

  for(int i = 0; i < nblocks; i++) {
    uint64_t k1 = getblock64(blocks,i*2+0);
    uint64_t k2 = getblock64(blocks,i*2+1);

    k1 *= c1; k1  = ROTL64(k1,31); k1 *= c2; h1 ^= k1;
    h1 = ROTL64(h1,27); h1 += h2; h1 = h1*5+0x52dce729;
    k2 *= c2; k2  = ROTL64(k2,33); k2 *= c1; h2 ^= k2;
    h2 = ROTL64(h2,31); h2 += h1; h2 = h2*5+0x38495ab5;
  }

  //----------
  // tail

  const uint8_t * tail = static_cast<const uint8_t*>(data + nblocks*16);

  uint64_t k1 = 0;
  uint64_t k2 = 0;

  switch(len & 15) {
    case 15: k2 ^= (static_cast<uint64_t>(tail[14])) << 48; [[clang::fallthrough]];
    case 14: k2 ^= (static_cast<uint64_t>(tail[13])) << 40; [[clang::fallthrough]];
    case 13: k2 ^= (static_cast<uint64_t>(tail[12])) << 32; [[clang::fallthrough]];
    case 12: k2 ^= (static_cast<uint64_t>(tail[11])) << 24; [[clang::fallthrough]];
    case 11: k2 ^= (static_cast<uint64_t>(tail[10])) << 16; [[clang::fallthrough]];
    case 10: k2 ^= (static_cast<uint64_t>(tail[ 9])) << 8; [[clang::fallthrough]];
    case  9: k2 ^= (static_cast<uint64_t>(tail[ 8])) << 0;
             k2 *= c2; k2  = ROTL64(k2,33); k2 *= c1; h2 ^= k2; [[clang::fallthrough]];

    case  8: k1 ^= (static_cast<uint64_t>(tail[ 7])) << 56; [[clang::fallthrough]];
    case  7: k1 ^= (static_cast<uint64_t>(tail[ 6])) << 48; [[clang::fallthrough]];
    case  6: k1 ^= (static_cast<uint64_t>(tail[ 5])) << 40; [[clang::fallthrough]];
    case  5: k1 ^= (static_cast<uint64_t>(tail[ 4])) << 32; [[clang::fallthrough]];
    case  4: k1 ^= (static_cast<uint64_t>(tail[ 3])) << 24; [[clang::fallthrough]];
    case  3: k1 ^= (static_cast<uint64_t>(tail[ 2])) << 16; [[clang::fallthrough]];
    case  2: k1 ^= (static_cast<uint64_t>(tail[ 1])) << 8; [[clang::fallthrough]];
    case  1: k1 ^= (static_cast<uint64_t>(tail[ 0])) << 0;
             k1 *= c1; k1  = ROTL64(k1,31); k1 *= c2; h1 ^= k1;
  };

  //----------
  // finalization

  h1 ^= static_cast<const uint64_t>(len); h2 ^= static_cast<const uint64_t>(len);

  h1 += h2;
  h2 += h1;

  h1 = fmix64(h1);
  h2 = fmix64(h2);

  h1 += h2;
  h2 += h1;

  (static_cast<uint64_t*>(out))[0] = h1;
  (static_cast<uint64_t*>(out))[1] = h2;
}


