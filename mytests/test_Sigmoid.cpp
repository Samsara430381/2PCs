/*
Authors: Deevashwer Rathee
Copyright:
Copyright (c) 2021 Microsoft Research
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "MyProtocol/MyProtocol.h"
#include <fstream>
#include <iostream>
#include <thread>

using namespace sci;
using namespace std;

#define MAX_THREADS 4

int party, port = 32000;
int num_threads = 4;
string address = "172.17.0.2";

// 32 = 1ULL << 5   256 = 1ULL << 8  2048 = 1ULL << 11  14
int dim = 1ULL << 14;
// int dim = exp10(n);
// int bw_x = 16;
// int bw_y = 14;
// int s_x = 12;
// int s_y = 12;     // s_y is supporsed to be large enough. s_y >= 12   if s_y < 12,Exp_N should use Faithful TR

// int bw_x = 8;
// int bw_y = 6;
// int s_x = 4;
// int s_y = 4;

int bw_x = 20;
int bw_y = 18;
int s_x = 16;
int s_y = 16; 
uint64_t mask_x = (bw_x == 64 ? -1 : ((1ULL << bw_x) - 1));
uint64_t mask_y = (bw_y == 64 ? -1 : ((1ULL << bw_y) - 1));

IOPack *iopackArr[MAX_THREADS];
OTPack *otpackArr[MAX_THREADS];

// computeULPErr(dbl_y, sig_x, s_y)
uint64_t computeULPErr(double dbl_x, double calc, double actual, int SCALE)
{
  int64_t calc_fixed = (double(calc) * (1ULL << SCALE));
  int64_t actual_fixed = (double(actual) * (1ULL << SCALE));
  uint64_t ulp_err = (calc_fixed - actual_fixed) > 0
                         ? (calc_fixed - actual_fixed)
                         : (actual_fixed - calc_fixed);
  // if (ulp_err > 5)
  //   cout << " ulp_err: " << ulp_err << " dbl_x: " << dbl_x << " calc: " << calc << " actual: " << actual << endl;
  return ulp_err;
}

void sigmoid_thread(int tid, uint64_t *x, uint64_t *y, int num_ops)
{
  MyProtocol *math;
  if (tid & 1)
  {
    math = new MyProtocol(3 - party, iopackArr[tid], otpackArr[tid]);
  }
  else
  {
    math = new MyProtocol(party, iopackArr[tid], otpackArr[tid]);
  }
  math->secSigmoid(num_ops, x, y, bw_x, bw_y, s_x, s_y);

  delete math;
}

int main(int argc, char **argv)
{
  /************* Argument Parsing  ************/
  /********************************************/
  ArgMapping amap;
  amap.arg("r", party, "Role of party: ALICE = 1; BOB = 2");
  amap.arg("p", port, "Port Number");
  amap.arg("N", dim, "Number of sigmoid operations");
  amap.arg("nt", num_threads, "Number of threads");
  amap.arg("ip", address, "IP Address of server (ALICE)");

  amap.parse(argc, argv);

  assert(num_threads <= MAX_THREADS);

  /********** Setup IO and Base OTs ***********/
  /********************************************/
  for (int i = 0; i < num_threads; i++)
  {
    iopackArr[i] = new IOPack(party, port + i, address);
    if (i & 1)
    {
      otpackArr[i] = new OTPack(iopackArr[i], 3 - party);
    }
    else
    {
      otpackArr[i] = new OTPack(iopackArr[i], party);
    }
  }
  std::cout << "All Base OTs Done" << std::endl;

  /************ Generate Test Data ************/
  /********************************************/
  PRG128 prg;

  uint64_t *x = new uint64_t[dim];
  uint64_t *y = new uint64_t[dim];

  prg.random_data(x, dim * sizeof(uint64_t));

  // if (party == ALICE)
  // {
  //   iopackArr[0]->io->send_data(x, dim * sizeof(uint64_t));
  // }
  // else
  // {
  //   uint64_t *x0 = new uint64_t[dim];
  //   iopackArr[0]->io->recv_data(x0, dim * sizeof(uint64_t));
  //   for (int i = 0; i < dim; i++)
  //   {
  //     // x is always >=0
  //     // x[i] =  (x[i] & (mask_x >> 1)) - x0[i];
  //     x[i] = 0 - x0[i];
  //   }
  //   delete[] x0;
  // }
  for (int i = 0; i < dim; i++)
  {
    x[i] &= mask_x;
    // x[i] = 0;
  }
  /************** Fork Threads ****************/
  /********************************************/
  uint64_t total_comm = 0;
  uint64_t thread_comm[num_threads];
  uint64_t thread_num_rounds[num_threads];

  for (int i = 0; i < num_threads; i++)
  {
    thread_comm[i] = iopackArr[i]->get_comm();
    thread_num_rounds[i] = iopackArr[i]->get_rounds();
  }

  auto start = clock_start();
  std::thread sig_threads[num_threads];
  int chunk_size = dim / num_threads;
  for (int i = 0; i < num_threads; ++i)
  {
    int offset = i * chunk_size;
    int lnum_ops;
    if (i == (num_threads - 1))
    {
      lnum_ops = dim - offset;
    }
    else
    {
      lnum_ops = chunk_size;
    }
    sig_threads[i] =
        std::thread(sigmoid_thread, i, x + offset, y + offset, lnum_ops);
  }
  for (int i = 0; i < num_threads; ++i)
  {
    sig_threads[i].join();
  }
  long long t = time_from(start);

  for (int i = 0; i < num_threads; i++)
  {
    thread_comm[i] = iopackArr[i]->get_comm() - thread_comm[i];
    thread_num_rounds[i] = iopackArr[i]->get_rounds() - thread_num_rounds[i];
    total_comm += thread_comm[i];
  }

  /************** Verification ****************/
  /********************************************/
  if (party == ALICE)
  {
    iopackArr[0]->io->send_data(x, dim * sizeof(uint64_t));
    iopackArr[0]->io->send_data(y, dim * sizeof(uint64_t));
  }
  else
  { // party == BOB
    uint64_t *x0 = new uint64_t[dim];
    uint64_t *y0 = new uint64_t[dim];
    iopackArr[0]->io->recv_data(x0, dim * sizeof(uint64_t));
    iopackArr[0]->io->recv_data(y0, dim * sizeof(uint64_t));

    uint64_t total_err = 0;
    uint64_t max_ULP_err = 0;
    for (int i = 0; i < dim; i++)
    {
      double dbl_x = (signed_val(x0[i] + x[i], bw_x)) / double(1LL << s_x);
      double dbl_y = (unsigned_val(y0[i] + y[i], bw_y)) / double(1ULL << s_y);
      double sig_x = 1.0 / (1.0 + exp(-1.0 * dbl_x));
      uint64_t err = computeULPErr(dbl_x, dbl_y, sig_x, s_y);
      // cout << "ULP Error: " << dbl_x << "," << dbl_y << "," << sig_x << ","
      // << err << endl;
      total_err += err;
      max_ULP_err = std::max(max_ULP_err, err);
    }

    cerr << "Average ULP error: " << total_err / dim << endl;
    cerr << "Max ULP error: " << max_ULP_err << endl;
    cerr << "Number of tests: " << dim << endl;
    cout << "Num rounds: " << thread_num_rounds[0] << endl;

    delete[] x0;
    delete[] y0;
  }

  cout << "Number of sigmoid ops/s:\t" << (double(dim) / t) * 1e6 << std::endl;
  cout << "Sigmoid Time\t" << t / (1000.0) << " ms" << endl;
  cout << "Sigmoid Bytes Sent\t" << total_comm << " bytes" << endl;

  /******************* Cleanup ****************/
  /********************************************/
  delete[] x;
  delete[] y;
  for (int i = 0; i < num_threads; i++)
  {
    delete iopackArr[i];
    delete otpackArr[i];
  }
}