#include <mpi.h>
#include <string>
#include <iostream>
#include <fstream>
#include <map>
#include <vector>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cmath>
#include "mpi_util.h"
#include "sparse_matrix.h"
#include "vector.h"
#include "util.h"
#ifdef GPU
#include <cuda_runtime_api.h>
#include <cusparse_v2.h>
#include <helper_cuda.h>
#endif
using namespace std;



void PrintHostName () {
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    char hostname[256];
    char *buf = new char[size * 256];
    int namelen;
    MPI_Get_processor_name(hostname, &namelen);
    MPI_Gather(hostname, 256, MPI_CHAR, buf, 256, MPI_CHAR, 0, MPI_COMM_WORLD);
    if (rank == 0) {
        for (int i = 0; i < size; i++) {
            fprintf(stdout, "%d/%d %s\n", i, size, buf + 256*i);
        }
        fflush(stdout);
    }
    delete [] buf;
}

void LoadInput (const string &partFile, SparseMatrix &A, Vector &x) {
    ifstream ifs(partFile);
    if (ifs.fail()) {
        std::cerr << "File not found : " + partFile<< std::endl;
        exit(1);
    }
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    string comment;

    //--------------------------------------------------------------------------------
    // Matrix
    //--------------------------------------------------------------------------------
    ifs >> comment; assert(comment == "#Matrix");
    int nCol;
    string matrixName;
    int nProc;
    ifs >> A.globalNumberOfRows >> nCol >> A.globalNumberOfNonzeros >> nProc >> matrixName;
    assert(nProc == size);
    assert(A.globalNumberOfRows == nCol);

    //--------------------------------------------------------------------------------
    // Partitioning
    //--------------------------------------------------------------------------------
    ifs >> comment; assert(comment == "#Partitioning");
    A.assign = new int[A.globalNumberOfRows];
    for (int i = 0; i < A.globalNumberOfRows; i++) ifs >> A.assign[i];

    //--------------------------------------------------------------------------------
    // Local <-> global map
    //--------------------------------------------------------------------------------
    ifs >> comment; assert(comment == "#LocalToGlobalTable");
    ifs >> A.totalNumberOfUsedCols;
    A.local2global = new int[A.totalNumberOfUsedCols];
    for (int i = 0; i < A.totalNumberOfUsedCols; i++) {
        ifs >> A.local2global[i];
        A.global2local[A.local2global[i]] = i;
    }
    //--------------------------------------------------------------------------------
    // SubMatrix
    //--------------------------------------------------------------------------------
    ifs >> comment; assert(comment == "#SubMatrix");
    int numInternalNnz, numExternalNnz;
    ifs >> A.localNumberOfRows >> numInternalNnz >> numExternalNnz;

    A.internalPtr = new int[A.localNumberOfRows + 1];
    A.internalIdx = new int[numInternalNnz];
    A.internalVal = new double[numInternalNnz];
    {
        int ip = 0;
        for (int i = 0; i < numInternalNnz; i++) {
            int row, col;
            double val;
            ifs >> row >> col >> val;
            row = A.global2local[row];
            col = A.global2local[col];
            A.internalIdx[i] = col;
            A.internalVal[i] = val;
            while (ip <= row) A.internalPtr[ip++] = i;
        }
        while (ip <= A.localNumberOfRows) A.internalPtr[ip++] = numInternalNnz;
    }
    A.externalPtr = new int[A.localNumberOfRows + 1];
    A.externalIdx = new int[numExternalNnz];
    A.externalVal = new double[numExternalNnz];
    {
        int ep = 0;
        for (int i = 0; i < numExternalNnz; i++) {
            int row, col;
            double val;
            ifs >> row >> col >> val;
            row = A.global2local[row];
            col = A.global2local[col];
            A.externalIdx[i] = col;
            A.externalVal[i] = val;
            while (ep <= row) A.externalPtr[ep++] = i;
        }
        while (ep <= A.localNumberOfRows) A.externalPtr[ep++] = numExternalNnz;
    }
    A.localNumberOfNonzeros = numInternalNnz + numExternalNnz;

    //--------------------------------------------------------------------------------
    // Communication
    //--------------------------------------------------------------------------------
    ifs >> comment; assert(comment == "#Communication");

    ifs >> comment; assert(comment == "#Send");
    ifs >> A.numberOfSendNeighbors >> A.totalNumberOfSend;
    A.sendLength = new int[A.numberOfSendNeighbors];
    A.sendNeighbors = new int[A.numberOfSendNeighbors];
    A.sendBuffer = new double[A.totalNumberOfSend];
    A.localIndexOfSend = new int[A.totalNumberOfSend];
    int sendOffset = 0;
    for (int i = 0; i < A.numberOfSendNeighbors; i++) {
        ifs >> A.sendNeighbors[i] >> A.sendLength[i];
        for (int j = 0; j < A.sendLength[i]; j++) {
            ifs >> A.localIndexOfSend[sendOffset + j];
        }
        sendOffset += A.sendLength[i];
    }
    assert(sendOffset == A.totalNumberOfSend);

    ifs >> comment; assert(comment == "#Recv");
    ifs >> A.numberOfRecvNeighbors >> A.totalNumberOfRecv;
    A.recvLength = new int[A.numberOfRecvNeighbors];
    A.recvNeighbors = new int[A.numberOfRecvNeighbors];
    A.localIndexOfRecv = new int[A.totalNumberOfRecv];
    int recvOffset = 0;
    for (int i = 0; i < A.numberOfRecvNeighbors; i++) {
        ifs >> A.recvNeighbors[i] >> A.recvLength[i];
        for (int j = 0; j < A.recvLength[i]; j++) {
            ifs >> A.localIndexOfRecv[recvOffset + j];
        }
        recvOffset += A.recvLength[i];
    }
    assert(recvOffset == A.totalNumberOfRecv);
    x.values = new double[A.totalNumberOfUsedCols];
    for (int i = 0; i < A.localNumberOfRows; i++) {
        x.values[i] = A.local2global[i] + 1;
    }
    //fill(x.values, x.values + A.totalNumberOfUsedCols, 1);
#ifdef GPU
    int ip = A.internalPtr[A.localNumberOfRows];
    int ep = A.externalPtr[A.localNumberOfRows];
    checkCudaErrors(cudaMalloc((void**)&A.cuda_internalPtr, (A.localNumberOfRows + 1) * sizeof(int)));
    checkCudaErrors(cudaMalloc((void**)&A.cuda_internalIdx, ip * sizeof(int)));
    checkCudaErrors(cudaMalloc((void**)&A.cuda_internalVal, ip * sizeof(double)));

    checkCudaErrors(cudaMalloc((void**)&A.cuda_externalPtr, (A.localNumberOfRows + 1) * sizeof(int)));
    checkCudaErrors(cudaMalloc((void**)&A.cuda_externalIdx, ep * sizeof(int)));
    checkCudaErrors(cudaMalloc((void**)&A.cuda_externalVal, ep * sizeof(double)));

    checkCudaErrors(cudaMemcpy((void *)A.cuda_internalPtr, A.internalPtr, (A.localNumberOfRows + 1) * sizeof(int), cudaMemcpyHostToDevice));
    checkCudaErrors(cudaMemcpy((void *)A.cuda_internalIdx, A.internalIdx, ip * sizeof(int), cudaMemcpyHostToDevice));
    checkCudaErrors(cudaMemcpy((void *)A.cuda_internalVal, A.internalVal, ip * sizeof(double), cudaMemcpyHostToDevice));
    checkCudaErrors(cudaMemcpy((void *)A.cuda_externalPtr, A.externalPtr, (A.localNumberOfRows + 1) * sizeof(int), cudaMemcpyHostToDevice));
    checkCudaErrors(cudaMemcpy((void *)A.cuda_externalIdx, A.externalIdx, ep * sizeof(int), cudaMemcpyHostToDevice));
    checkCudaErrors(cudaMemcpy((void *)A.cuda_externalVal, A.externalVal, ep * sizeof(double), cudaMemcpyHostToDevice));

    checkCudaErrors(cudaMalloc((void**)&A.cuda_x_values, (A.localNumberOfRows + A.totalNumberOfRecv) * sizeof(double)));
    checkCudaErrors(cudaMalloc((void**)&A.cuda_y_values, A.localNumberOfRows * sizeof(double)));
#endif
}

void CreateZeroVector (Vector &v, int length) {
    v.values = new double[length];
    fill(v.values, v.values + length, 0);
}

void PrintResult (SparseMatrix &A, Vector &y) {
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    MPI_Barrier(MPI_COMM_WORLD);
    for (int i = 0; i < A.globalNumberOfRows; i++) {
        MPI_Barrier(MPI_COMM_WORLD);
        if (A.assign[i] == rank) {
            cerr << i << " " << rank << " " <<  A.global2local[i] << " " << y.values[A.global2local[i]] << endl;
        }
        cerr.flush();
        MPI_Barrier(MPI_COMM_WORLD);
    }
}


bool VerifySpMV (const string &mtxFile, const SparseMatrix &A, const Vector &y) {
    bool res = true;
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    double *unorderedResult = new double[A.globalNumberOfRows];
    double *localReorderedResult = new double[A.localNumberOfRows];
    vector<int> localRows;
    for (int i = 0; i < A.globalNumberOfRows; i++) {
        if (rank == A.assign[i]) localRows.push_back(i);
    }
    for (int i = 0; i < A.localNumberOfRows; i++) {
        int idx = lower_bound(localRows.begin(), localRows.end(), A.local2global[i]) - localRows.begin();
        localReorderedResult[idx] = y.values[i];
    }
    int *recvCount = new int[size];
    memset(recvCount, 0, size * sizeof(int));
    for (int i = 0; i < A.globalNumberOfRows; i++) recvCount[A.assign[i]]++;
    int *displs = new int[size+1];
    displs[0] = 0;
    for (int i = 0; i < size; i++) displs[i+1] = displs[i] + recvCount[i];
    MPI_Allgatherv(localReorderedResult, A.localNumberOfRows, MPI_DOUBLE, unorderedResult, recvCount, displs, MPI_DOUBLE, MPI_COMM_WORLD);
    if (rank != 0) return true;
    double *result = new double[A.globalNumberOfRows];
    int *index = new int[size];
    memset(index, 0, size * sizeof(int));
    for (int i = 0; i < A.globalNumberOfRows; i++) {
        int p = A.assign[i];
        result[i] = unorderedResult[displs[p] + index[p]];
        index[p]++;
    }
    int nRow, nCol, nNnz;
    vector<Element> elements = GetElementsFromFile(mtxFile, nRow, nCol, nNnz);

    int *ptr = new int[nRow+1];
    int *idx = new int[nNnz];
    double *val = new double[nNnz];
    int p = 0;
    for (int i = 0; i < nNnz; i++) {
        int r = elements[i].row;
        idx[i] = elements[i].col;
        val[i] = elements[i].val;
        //if (rank == 0) printf("%d %d %lf\n", r, idx[i], val[i]);
        while (p <= r) ptr[p++] = i;
    }
    while (p <= nRow) ptr[p++] = nNnz;

    for (int i = 0; i < nRow; i++) {
        double sum = 0;
        for (int j = ptr[i]; j < ptr[i+1]; j++) {
            // TODO
            sum += val[j] *(idx[j] + 1);
            //sum += val[j] * 1;
        }
        double relative_error = abs(abs(result[i] - sum) / result[i]);
        const double EPS = 1e-8;
        if (relative_error > EPS)  {
            if (rank == 0) cerr << "Result is wrong at " << i << " expected value: " << sum << " returned value: " << result[i] << " relative error: " << relative_error << " absolute error: " << abs(result[i]-sum) << endl;
            res = false;
        }
    }
    return res;
}

// Create new buffer for internal idx to increase cache hit rate
void CreateDenseInternalIdx (SparseMatrix &A, Vector &x) {
    set<int> usedCols;
    for (int i = 0; i < A.internalPtr[A.localNumberOfRows]; i++) {
        usedCols.insert(A.internalIdx[i]);
    }
    x.denseInternalValues = new double[usedCols.size()];
    int p = 0;
    map<int, int> internalToUsed;
    map<int, int> usedToInternal;
    for (auto c : usedCols) {
        internalToUsed[c] = p;
        usedToInternal[p] = c;
        p++;
    }
    
    for (int i = 0; i < p; i++) {
        x.denseInternalValues[i] = x.values[usedToInternal[i]];
    }
    A.denseInternalIdx = new int[A.localNumberOfNonzeros];
    int idx = 0;
    for (int i = 0; i < A.internalPtr[A.localNumberOfRows]; i++) {
        int c = A.internalIdx[i];
        A.denseInternalIdx[i] = internalToUsed[c];
    }
#ifdef GPU
    int ip = A.internalPtr[A.localNumberOfRows];
    checkCudaErrors(cudaMalloc((void**)&A.cuda_denseInternalIdx, ip * sizeof(int)));
    checkCudaErrors(cudaMemcpy((void *)A.cuda_denseInternalIdx, A.denseInternalIdx, ip * sizeof(int), cudaMemcpyHostToDevice));
#endif
}


void SelectDevice () {
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
#ifdef GPU
    checkCudaErrors(cudaSetDevice(rank%GPU_PER_NODE));
#endif
}


double GetSynchronizedTime () {
    MPI_Barrier(MPI_COMM_WORLD);
    double t = MPI_Wtime();
    MPI_Bcast(&t, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);
    return t;
}

void DeleteSparseMatrix (SparseMatrix & A) {
}
void DeleteVector (Vector & x) {
}


void PrintOption () {
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if (rank == 0) {
        printf("%25s\t", "Option");
#ifdef CPU
        printf("+CPU");
#endif
#ifdef MIC
        printf("+MIC");
#endif
#ifdef GPU
        printf("+GPU");
#endif
#ifdef USE_DENSE_INTERNAL_INDEX
        printf("+USE_DENSE_INTERNAL_INDEX");
#endif
#ifdef PRINT_HOSTNAME
        printf("+PRINT_HOSTNAME");
#endif
#ifdef PRINT_PERFORMANCE
        printf("+PRINT_PERFORMANCE");
#endif
#ifdef PRINT_REAL_PERFORMANCE
        printf("+PRINT_REAL_PERFORMANCE");
#endif
#ifdef PRINT_NUMABIND
        printf("+PRINT_NUMABIND");
#endif
        printf("\n");
    }
}
