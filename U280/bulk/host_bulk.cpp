/**********
Copyright (c) 2018, Xilinx, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software
without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**********/


#include "xcl2.hpp"
#include <cmath>
#include <algorithm>
#include <vector>
#include <ap_int.h>
#include <cstdlib>
#include <chrono>
#include <iostream>

#include "mmio.h"
#include "sparse_helper.h"

//#define DEBUG_PRINT 1

using std::cout;
using std::endl;
using std::ifstream;
using std::string;
using std::vector;
using std::min;
using std::max;

const int NUM_CH_SPARSE = 8;
const int NUM_CH_B = 4;
const int NUM_CH_C = 8;
const int NUM_WINDOW_SIZE = 4096;

void spmm_kernel_simulation(
    const int N,
    const int NUM_PE,
    const int NUM_ROW,
    const int NUM_COLUMN,
    const int WINDOE_SIZE,
    const int num_ite,
    const vector<unsigned int, aligned_allocator<unsigned int> > & edge_list_ptr,
    double & p_time,
    const int NUM_CYC_B = 1){
    /* ----------------------------------- */
    const double frequency = 350e+6; //Hz
    const double memory_bandwidth = 900e+9 * 0.9; //B/s
    
    p_time = 0.0;
    
    double time_compute;
    double time_memory;
    
    //init C
    int init_cycle = (NUM_ROW + 63) / 64;
    p_time += (init_cycle / frequency);
    
    //main process
    for (int i = 0; i < num_ite; ++i) {
        int base_col_index = i * WINDOE_SIZE * NUM_CYC_B;
        //fetch B
        int wd_actual = ((min(WINDOE_SIZE * NUM_CYC_B, NUM_COLUMN - base_col_index) + 15) / 16) * 16;
        time_memory = 4.0 * wd_actual * 8.0 / (memory_bandwidth * 1.0); //8.0 is the share factor
        time_compute = 1.0 * wd_actual / 16 / frequency;
        p_time += max(time_memory, time_compute);
        
        for (int c = 0; c < NUM_CYC_B; ++c) {
            //computation
            int len_A = edge_list_ptr[i*NUM_CYC_B + c +1] - edge_list_ptr[i*NUM_CYC_B + c];
            time_memory = 64.0 * len_A * 8.0 / (memory_bandwidth * 1.0);
            time_compute = 1.0 * (len_A + 17) / frequency;
            p_time += max(time_memory, time_compute);
        } 
    }
    
    //transfer C
    int row_act = ((NUM_ROW + 15) / 16) * 16;
    time_memory = 4.0 * row_act * 8.0 / (memory_bandwidth * 1.0);
    time_compute = (row_act / 16 + 21) / frequency;
    p_time += max(time_memory, time_compute);
    p_time += (48.0 / frequency);
    
    //N
    p_time *= (N/8);
    
    p_time += (2000.0 / frequency);
    
}


int ceil_eightx(int x) {
    if (x <= 0) return 1;
    return ((x + 7) / 8) * 8;
}

int main(int argc, char **argv) {
    printf("start host\n");
    
    float ALPHA = 0.85;
    float BETA = -2.06;
    int N = 512;
    int rp_time = 20;

    if (argc != 8) {
	cout << "Usage: " << argv[0] << " <XCLBIN File> [matrix name] [ID] [rows] [cols] [nnz] [outputfile]" << std::endl;
        return EXIT_FAILURE;
    }

    int s_ID = atoi(argv[3]);
    int s_M = atoi(argv[4]);
    int s_K = atoi(argv[5]);
    int s_NNZ = atoi(argv[6]);

    string matrix_name = string(argv[2]);
    string filename_mtx = "./dataset/" + matrix_name + "/" + matrix_name + ".mtx";
    string filename_stx = "./dataset/" + matrix_name + "/" + matrix_name + ".stx";
    string filename_output = "./" + string(argv[7]);
    
    cout << "N = " << N <<  "\n";
    cout << "alpha = "  << ALPHA << "\n";
    cout << "beta = "  << BETA << "\n";

    int M, K, nnz;
    vector<int> CSRRowPtr;
    vector<int> CSRColIndex;
    vector<float> CSRVal;
    
    cout << "Reading sparse A matrix...";
    read_suitsparse_matrix((char*) filename_mtx.c_str(),
                           CSRRowPtr,
                           CSRColIndex,
                           CSRVal,
                           M,
                           K,
                           nnz,
                           CSR);
    cout <<  "done\n";
    
    cout << "Matrix size: \n";
    cout << "A: sparse matrix, " << M << " x " << K << ". NNZ = " << nnz <<  "\n";
    cout << "B: dense matrix, "  << K << " x " << N << "\n";
    cout << "C: dense matrix, "  << M << " x " << N << "\n";
    
    // initiate matrix B and matrix C
    vector<float> mat_B_cpu, mat_C_cpu;
    mat_B_cpu.resize(K*N, 0.0);
    mat_C_cpu.resize(M*N, 0.0);
   

    cout << "Generating dense matirx B ...";
    for (int nn = 0; nn < N; ++nn) {
        #pragma omp parallel
	for (int kk = 0; kk < K; ++kk) {
            mat_B_cpu[kk + K * nn] = (1.0 + kk) + 0.1 * (1.0 + nn); //100.0 * (kk + 1)  + 1.0 * (nn + 1);// / K / N;
        }
    }
    
    cout << "Generating dense matirx C ...";
    for (int nn = 0; nn < N; ++nn) {
        #pragma omp parallel
	for (int mm = 0; mm < M; ++mm) {
            mat_C_cpu[mm + M * nn] = 1.0 * (mm + 1) * (nn + 1);
        }
    }
     
    cout <<  "done\n";

    cout << "Preparing sparse A for FPGA ..."; 
    
    FILE * f_A = fopen(filename_stx.c_str(), "rb");
    int edge_list_ptr_size;
    //fscanf(f_A, "%d\n", &edge_list_ptr_size); 
    fread(&edge_list_ptr_size, sizeof(int), 1, f_A);
    int sparse_A_fpga_chunk_size;
    //fscanf(f_A, "%d\n", &sparse_A_fpga_chunk_size);
    fread(&sparse_A_fpga_chunk_size, sizeof(int), 1, f_A);

    int NUM_PE;
    int WINDOE_SIZE;
    vector<unsigned int, aligned_allocator<unsigned int> > edge_list_ptr_fpga;
    int edge_list_ptr_fpga_size = ((edge_list_ptr_size + 15) / 16) * 16;
    int edge_list_ptr_fpga_chunk_size = ((edge_list_ptr_fpga_size + 1023)/1024) * 1024;
    edge_list_ptr_fpga.resize(edge_list_ptr_fpga_chunk_size, 0);
    
    for (unsigned int i = 0; i < edge_list_ptr_size; ++i) {
	int tmp;
	//fscanf(f_A, "%d\n", &tmp);
	fread(&tmp, sizeof(int), 1, f_A);
	//cout << tmp << endl;
        edge_list_ptr_fpga[i] = tmp;
    }
    int sparse_A_fpga_column_size = 8 * edge_list_ptr_fpga[edge_list_ptr_size - 1];
    
    vector<vector<unsigned long, aligned_allocator<unsigned long> > > sparse_A_fpga_vec(NUM_CH_SPARSE);
    for (int ch = 0; ch < NUM_CH_SPARSE; ++ch) {
	sparse_A_fpga_vec[ch] = vector<unsigned long, aligned_allocator<unsigned long>> (sparse_A_fpga_chunk_size, 0);
	for (int i = 0; i < sparse_A_fpga_chunk_size; ++i) {
	    unsigned long tmp;
	    //fscanf(f_A, "%lu\n", &tmp);
	    fread(&tmp, sizeof(unsigned long), 1, f_A);
	    //cout << tmp << endl;
	    sparse_A_fpga_vec[ch][i] = tmp;
	}
    }
    
    cout <<  "done\n";  
    
    cout << "Preparing dense B for FPGA ...";
    
    vector<vector<float, aligned_allocator<float> > > mat_B_fpga_vec(NUM_CH_B);
    int mat_B_fpga_column_size;
    if (NUM_CH_B == 8) {
        mat_B_fpga_column_size = ((K + 16 - 1) / 16) * 16;
    }else if (NUM_CH_B == 4) {
        mat_B_fpga_column_size = ((K + 8 - 1) / 8) * 8 * 2;
    }
    int mat_B_fpga_chunk_size = ((mat_B_fpga_column_size * (N / 8) + 1023)/1024) * 1024;
 
    for (int cc = 0; cc < NUM_CH_B; ++cc) {
        mat_B_fpga_vec[cc] = vector<float, aligned_allocator<float>> (mat_B_fpga_chunk_size, 0.0);
    }

    #pragma omp parallel
    for (int nn = 0; nn < N; ++nn) {
        for (int kk = 0; kk < K; ++kk) {
            if (NUM_CH_B == 4) {
                int pos = (kk / 8) * 16 + (nn % 2) * 8 + kk % 8
                                + mat_B_fpga_column_size * (nn / 8);
                mat_B_fpga_vec[(nn/2) % 4][pos] = mat_B_cpu[kk + K * nn];
            } else if (NUM_CH_B == 8) {
                int pos = kk + mat_B_fpga_column_size * (nn / 8);
                mat_B_fpga_vec[nn % 8][pos] = mat_B_cpu[kk + K * nn];
            }
        }
    }
    
    cout << "Preparing dense C for FPGA ...";
    vector<vector<float, aligned_allocator<float> > > mat_C_fpga_in(8);
    int mat_C_fpga_in_column_size = ((M + 16 - 1) / 16) * 16;
    int mat_C_fpga_in_chunk_size = ((mat_C_fpga_in_column_size * (N / 8) + 1023)/1024) * 1024;
    
    for (int nn = 0; nn < 8; ++nn) {
        mat_C_fpga_in[nn].resize(mat_C_fpga_in_chunk_size, 0.0);
    }
    
    #pragma omp parallel
    for (int nn = 0; nn < N; ++nn) {
        for (int mm = 0; mm < M; ++mm) {
            //mat_C_cpu[mm + M * nn] = 1.0 * (mm + 1) * (nn + 1) / M / N;
            int pos = mat_C_fpga_in_column_size * (nn / 8) + mm;
            //mat_C_fpga_in[nn % 8].resize(pos+1);
            mat_C_fpga_in[nn % 8][pos] = mat_C_cpu[mm + M * nn];
        }
    }
    
    vector<vector<float, aligned_allocator<float> > > mat_C_fpga_vec(NUM_CH_C);
    //int mat_C_fpga_column_size = ((M * N + 16 * NUM_CH_C - 1) / (16 * NUM_CH_C)) * 16 * NUM_CH_C;
    int mat_C_fpga_column_size = ((M + 16 - 1) / 16) * 16;
    int mat_C_fpga_chunk_size = ((mat_C_fpga_column_size * (N / 8) + 1023)/1024) * 1024;
    
    
    for (int cc = 0; cc < NUM_CH_C; ++cc) {
        mat_C_fpga_vec[cc] = vector<float, aligned_allocator<float>> (mat_C_fpga_chunk_size, 0.0);
    }
    
    cout <<  "done\n";

    cout << "Run spmm on cpu...";
    auto start_cpu = std::chrono::steady_clock::now();
    cpu_spmm_CSR(M, N, K, nnz, ALPHA,
                 CSRRowPtr,
                 CSRColIndex,
                 CSRVal,
                 mat_B_cpu,
                 BETA,
                 mat_C_cpu);
    auto end_cpu = std::chrono::steady_clock::now();
    double time_cpu = std::chrono::duration_cast<std::chrono::nanoseconds>(end_cpu - start_cpu).count();
    time_cpu *= 1e-9;
    cout << "done (" << time_cpu*1000 << " msec)\n";
    cout <<"CPU GFLOPS: " << 2.0f*s_NNZ*N/1000000000/time_cpu << "\n";

    std::string binaryFile = argv[1];
    cl_int err;
    cl::Context context;
    cl::Kernel krnl_sextans;
    cl::CommandQueue q;

    // OPENCL HOST CODE AREA START
    auto devices = xcl::get_xil_devices();
    auto fileBuf = xcl::read_binary_file(binaryFile);
    cl::Program::Binaries bins{{fileBuf.data(), fileBuf.size()}};
    int valid_device = 0;
    printf("Num of devices : %d\n", (int)devices.size());
    for (unsigned int i = 0; i < devices.size(); i++) {
        auto device = devices[i];
        // Creating Context and Command Queue for selected Device
        OCL_CHECK(err, context = cl::Context({device}, NULL, NULL, NULL, &err));
        OCL_CHECK(err,
                q = cl::CommandQueue(
                    context, {device}, CL_QUEUE_PROFILING_ENABLE, &err));

        //if( device.getInfo<CL_DEVICE_NAME>() != "xilinx_u50_gen3x16_xdma_201920_3" ){
        if( device.getInfo<CL_DEVICE_NAME>() != "xilinx_u280_xdma_201920_3" ){
            cout  << "Skipping device : " << device.getInfo<CL_DEVICE_NAME>() << endl;
            continue;
        }
        cout << "Trying to program device[" << i
            << "]: " << device.getInfo<CL_DEVICE_NAME>() << endl;
        OCL_CHECK(err,
                cl::Program program(context, {device}, bins, NULL, &err));
        if (err != CL_SUCCESS) {
            cout << "Failed to program device[" << i
                << "] with xclbin file!\n";
        } else {
            cout << "Device[" << i << "]: program successful!\n";
            OCL_CHECK(err, krnl_sextans = cl::Kernel(program, "sextans", &err));
            valid_device++;
            break; // we break because we found a valid device
        }
    }
    if (valid_device == 0) {
        cout << "Failed to program any device found, exit!\n";
        exit(EXIT_FAILURE);
    }

    
    std::vector<cl::Buffer> buffer_A;
    std::vector<cl::Buffer> buffer_B;
    std::vector<cl::Buffer> buffer_C_in;
    std::vector<cl::Buffer> buffer_C;
    
    for (int i = 0; i < NUM_CH_SPARSE; i++) {
        OCL_CHECK(err,
              cl::Buffer currA(context,
                               CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
                               sparse_A_fpga_column_size*sizeof(unsigned long),
                               sparse_A_fpga_vec[i].data(),
                               &err);
             );
        buffer_A.push_back(std::move(currA));
    }

    for (int i = 0; i < NUM_CH_B; i++) {
        OCL_CHECK(err,
              cl::Buffer currA(context,
                               CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
                               mat_B_fpga_column_size*(N/8)*sizeof(float),
                               mat_B_fpga_vec[i].data(),
                               &err);
             );
        buffer_B.push_back(std::move(currA));
    }

    for (int i = 0; i < NUM_CH_C; i++) {
        OCL_CHECK(err,
                  cl::Buffer currA(context,
                                   CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
                                   mat_C_fpga_in_chunk_size*sizeof(float),
                                   mat_C_fpga_in[i].data(),
                                   &err);
                  );
        buffer_C_in.push_back(std::move(currA));
    }
    
    for (int i = 0; i < NUM_CH_C; i++) {
        OCL_CHECK(err,
                  cl::Buffer currA(context,
                                   CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY,
                                   mat_C_fpga_chunk_size*sizeof(float),
                                   mat_C_fpga_vec[i].data(),
                                   &err);
                  );
        buffer_C.push_back(std::move(currA));
    }
    
    OCL_CHECK(err,
              cl::Buffer buffer_edge_list_ptr(context,
                                              CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
                                              edge_list_ptr_fpga_size*sizeof(unsigned int),
                                              edge_list_ptr_fpga.data(),
                                              &err);
         );
    
    
    // set argument
    int parameter_pos = 0;
    
    OCL_CHECK(err, err = krnl_sextans.setArg(parameter_pos++, buffer_edge_list_ptr));
    
    for (int i = 0; i < NUM_CH_SPARSE; i++) {
        OCL_CHECK(err, err = krnl_sextans.setArg(parameter_pos++, buffer_A[i]));
    }
    
    for (int i = 0; i < NUM_CH_B; i++) {
        OCL_CHECK(err, err = krnl_sextans.setArg(parameter_pos++, buffer_B[i]));
    }
    
    
    for (int i = 0; i < NUM_CH_C; i++) {
        OCL_CHECK(err, err = krnl_sextans.setArg(parameter_pos++, buffer_C_in[i]));
    }
    
    
    for (int i = 0; i < NUM_CH_C; i++) {
        OCL_CHECK(err, err = krnl_sextans.setArg(parameter_pos++, buffer_C[i]));
    }
    
    int MAX_SIZE_edge_LIST_PTR = edge_list_ptr_size - 1;
    OCL_CHECK(err, err = krnl_sextans.setArg(parameter_pos++, MAX_SIZE_edge_LIST_PTR));
    
    int MAX_LEN_edge_PTR = edge_list_ptr_fpga[MAX_SIZE_edge_LIST_PTR];
    OCL_CHECK(err, err = krnl_sextans.setArg(parameter_pos++, MAX_LEN_edge_PTR));

    OCL_CHECK(err, err = krnl_sextans.setArg(parameter_pos++, M));
    OCL_CHECK(err, err = krnl_sextans.setArg(parameter_pos++, K));
    int N_parameter_pos = parameter_pos;
    int para_N = (rp_time << 16) | N;
    OCL_CHECK(err, err = krnl_sextans.setArg(parameter_pos++, para_N));
    
    unsigned int * tmpPointer_v;
    tmpPointer_v = (unsigned int*) &ALPHA;
    unsigned int alpha_int = *tmpPointer_v;
    tmpPointer_v = (unsigned int*) &BETA;
    unsigned int beta_int = *tmpPointer_v;
    OCL_CHECK(err, err = krnl_sextans.setArg(parameter_pos++, alpha_int));
    OCL_CHECK(err, err = krnl_sextans.setArg(parameter_pos++, beta_int));

    cout << "move data to HBM\n";
    for ( int i = 0; i < NUM_CH_SPARSE; i++) {
        OCL_CHECK(err, err = q.enqueueMigrateMemObjects({buffer_A[i]}, 0 /* 0 means from host*/));
    }
    for ( int i = 0; i < NUM_CH_B; i++) {
        OCL_CHECK(err, err = q.enqueueMigrateMemObjects({buffer_B[i]}, 0 /* 0 means from host*/));
    }
    for ( int i = 0; i < NUM_CH_C; i++) {
        OCL_CHECK(err, err = q.enqueueMigrateMemObjects({buffer_C_in[i]}, 0 /* 0 means from host*/));
    }
    OCL_CHECK(err, err = q.enqueueMigrateMemObjects({buffer_edge_list_ptr}, 0 /* 0 means from host*/));

    q.finish();

    printf("start kernel\nWarmup run...");
    N = 8;
    para_N = (rp_time << 16) | N;
    OCL_CHECK(err, err = krnl_sextans.setArg(N_parameter_pos, para_N));
    OCL_CHECK(err, err = q.enqueueTask(krnl_sextans));
    q.finish();
    printf("Done\n");

    FILE *fout = fopen(filename_output.c_str(), "a");

    for (N = 8; N <= 512; N = N * 2) {
        para_N = (rp_time << 16) | N;
	OCL_CHECK(err, err = krnl_sextans.setArg(N_parameter_pos, para_N));
	cout << "Running FPGA kernel N = " << N << endl;
	auto start = std::chrono::steady_clock::now();
	OCL_CHECK(err, err = q.enqueueTask(krnl_sextans));
	q.finish();
	auto end = std::chrono::steady_clock::now();
	double time_taken = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        time_taken *= 1e-9;
	printf("Kernel time is %.7e ms\n", time_taken*1000/rp_time);

	float gflops =
        (2.0f * (s_NNZ + s_M) * N)
        * rp_time
        / 1e9 
        / time_taken // total time in second
        ;
        printf("GFLOPS:%f \n", gflops);

	fprintf(fout, "%d\t%s\t%d\t%d\t%d\t%d\t%e", s_ID, matrix_name.c_str(), s_M, s_K, s_NNZ, N, 2.0 * (s_NNZ + s_M) * N);
	//gflops, energy eff, bwd efficeincy
	float enegy_eff = gflops/52 * 1e+9;
	float bdw_eff = 4.0 * (s_NNZ + N * (s_M * 2 + s_K)) / (1e+9) / (time_taken/rp_time) / 460;
	fprintf(fout, "\t%f\t%f\t%f", gflops, enegy_eff, bdw_eff);

	double sim_time = 0.0;
	int num_ite = edge_list_ptr_size - 1;
	spmm_kernel_simulation(N, 64, M, K, NUM_WINDOW_SIZE, num_ite, edge_list_ptr_fpga, sim_time, 1);
	printf("Projected kernel time is %.7e ms\n", sim_time*1000);
	gflops = 2.0f * (s_NNZ + s_M) * N / 1e+9 / sim_time;
	enegy_eff = gflops/96 * 1e+9;
	bdw_eff = 4.0 * (s_NNZ + N * (s_M * 2 + s_K)) / (1e+9) / sim_time / 900;
	fprintf(fout, "\t%f\t%f\t%f\n", gflops, enegy_eff, bdw_eff);
    }
    N = 512;

    cout << "move data to host\n";
    // Copy Result from Device Global Memory to Host Local Memory
    for (int i = 0; i < NUM_CH_C; i++) {
        OCL_CHECK(err, err = q.enqueueMigrateMemObjects({buffer_C[i]}, CL_MIGRATE_MEM_OBJECT_HOST));
    }
    q.finish();
    cout << "finish\n";

    int mismatch_cnt = 0;
        
    for (int nn = 0; nn < N; ++nn) {
        for (int mm = 0; mm < M; ++mm) {
            int pos = mat_C_fpga_column_size * (nn / 8) + mm;
            float v_cpu = mat_C_cpu[mm + nn * M];
            float v_fpga = mat_C_fpga_vec[nn % 8][pos];
                
            float dff = fabs(v_cpu - v_fpga);
            float x = min(fabs(v_cpu), fabs(v_fpga)) + 1e-4;
            if (dff/x > 1e-4) {
                mismatch_cnt++;
            }
        }
    }
        
    float diffpercent = 100.0 * mismatch_cnt / M / N;
    bool pass = diffpercent < 2.0;
        
    if(pass){
        cout << "Success!\n";
    } else{
        cout << "Failed.\n";
    }
    printf("num_mismatch = %d, percent = %.2f%%\n", mismatch_cnt, diffpercent);

    return EXIT_SUCCESS;
}