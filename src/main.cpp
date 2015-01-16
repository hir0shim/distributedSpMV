#include <mpi.h>
#include <ctime>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <mpi.h>
#include "sparse_matrix.h"
#include "vector.h"
#include "spmv.h"
#include "util.h"
#include "mpi_util.h"
#include "timing.h"
using namespace std;

vector<char*>   timingDetail(NUMBER_OF_TIMING, NULL);
vector<double>  timing(NUMBER_OF_TIMING);
vector<double>  timingTemp(NUMBER_OF_TIMING);

#define PERR(s)   if (rank == 0) fprintf(stderr, "%s", s);
#define POUT(s)   if (rank == 0) fprintf(stdout, "%s", s);

int main (int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <prefix of part file (i.e. 'partition/test.mtx')> [matrix file (to verify)]\n", argv[0]);
        exit(1);
    }
    char *mtxFile;
    bool verify = false;
    if (argc == 3) {
        verify = true;
        mtxFile = argv[2];
    }
    MPI_Init(&argc, &argv);


    //------------------------------
    // INIT
    //------------------------------
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if (rank == 0) fprintf(stderr, "Begin %s\n", mtxFile);
    POUT("[ Hostname ]\n");
    MPI_Barrier(MPI_COMM_WORLD);
    PrintHostName();
    MPI_Barrier(MPI_COMM_WORLD); fflush(stderr); fflush(stdout);
    string partFile = string(argv[1]) + "-" + to_string(size) + "-" + to_string(rank) + ".part"; 
    PERR("Loading sparse matrix and vector ... ");
    MPI_Barrier(MPI_COMM_WORLD); fflush(stderr); fflush(stdout);
    SparseMatrix A;
    Vector x, y;
    LoadInput(partFile, A, x);
    CreateZeroVector(y, A.localNumberOfRows);
    MPI_Barrier(MPI_COMM_WORLD); fflush(stderr); fflush(stdout);
    PERR("done\n");

    //------------------------------
    // SpMV
    //------------------------------
    PERR("Computing SpMV ... ");
    timingDetail[TIMING_TOTAL_SPMV] = "Total SpMV";
    timingDetail[TIMING_TOTAL_COMMUNICATION] = "Total Communication";
    timingDetail[TIMING_TOTAL_COMPUTATION]  = "Total Computation";
    timingDetail[TIMING_PACKING] = "Packing";
    for (int i = 0; i < NUMBER_OF_LOOP_OF_SPMV; i++) {
        MPI_Barrier(MPI_COMM_WORLD); 
        double tmp = -MPI_Wtime();
        SpMV(A, x, y);
        MPI_Barrier(MPI_COMM_WORLD); 
        tmp += MPI_Wtime();
        if (!i || timing[TIMING_TOTAL_SPMV] > tmp) {
            timing[TIMING_TOTAL_SPMV] = tmp;
            timing[TIMING_TOTAL_COMMUNICATION] = timingTemp[TIMING_TOTAL_COMMUNICATION];
            timing[TIMING_TOTAL_COMPUTATION] = timingTemp[TIMING_TOTAL_COMPUTATION];
            timing[TIMING_PACKING] = timingTemp[TIMING_PACKING];
        }
    }
    PERR("done\n");


    //------------------------------
    // DELETE
    //------------------------------
    PERR("Deleting A, x ... ");
    DeleteSparseMatrix(A);
    DeleteVector(x);
    MPI_Barrier(MPI_COMM_WORLD); fflush(stderr); fflush(stdout);
    PERR("done\n");

    //------------------------------
    // Verify
    //------------------------------
    PERR("Verifying ... ");
    if (verify) VerifySpMV(mtxFile, A, y);
    MPI_Barrier(MPI_COMM_WORLD); fflush(stderr); fflush(stdout);
    PERR("done\n");

    /*
    //------------------------------
    // Print 
    //------------------------------
    PERR("Printing result ... ");
    PrintResult(A, y);
    MPI_Barrier(MPI_COMM_WORLD); fflush(stderr); fflush(stdout);
    PERR("done\n");
    */

    /*
    //------------------------------
    // DELETE
    //------------------------------
    PERR("Deleting y ... ");
    DeleteVector(y);
    MPI_Barrier(MPI_COMM_WORLD); fflush(stderr); fflush(stdout);
    PERR("done\n");
    */




    //------------------------------
    // REPORT
    //------------------------------
    if (rank == 0) {
        puts("[ Performance ]");
        printf("%20s\t%.10lf\n", "GFLOPS", A.globalNumberOfNonzeros * 2 / timing[TIMING_TOTAL_SPMV] / 1e9);
        for (int i = 0; i < NUMBER_OF_TIMING; i++) {
            if (timingDetail[i] != NULL) {
                printf("%20s\t%.10lf\n", timingDetail[i], timing[i]);
            }
        }
    }
    PERR("Finalizing ... ");
    MPI_Finalize();
    PERR("done\n");
    PERR("Complete!!\n");
    return 0;
}
