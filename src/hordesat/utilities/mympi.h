/*
 * mympi.h
 *
 *  Created on: Apr 26, 2016
 *      Author: balyo
 */

#ifndef MYMPI_H_
#define MYMPI_H_

/**
 * The purpose of this file is to enable hordesat to be compiled
 * without MPI for single node usage. To build the mpi-free version
 * comment #define USEMPI below and update the makefile by changing
 * CXX = mpic++ to CXX = g++
 *
 * The mpi-free version only works with the default AllToAll clasue sharing.
 */

#define USEMPI

#ifdef USEMPI

#include <mpi.h>

#else

typedef void* MPI_Request;
struct MPI_Status {
	int MPI_SOURCE;

};
const int MPI_INT = 0;
const int MPI_UNSIGNED_LONG = 0;
const int MPI_MAX = 0;
const int MPI_DOUBLE = 0;
const int MPI_SUM = 0;
const int MPI_COMM_WORLD = 0;
const int MPI_ANY_SOURCE = 0;
const int MPI_STATUS_IGNORE = 0;
const int MPI_REQUEST_NULL = 0;
//typedef int MPI_Datatype

int MPI_Allgather(void* , int, int, void*, int, int, int);
int MPI_Irecv(void*, int, int, int, int, int, MPI_Request *);
int MPI_Test(MPI_Request *, int *, int *);
int MPI_Isend(void*, int, int, int, int, int, MPI_Request *);
int MPI_Init(int *, char ***);
int MPI_Finalize(void);
int MPI_Comm_size(int, int *);
int MPI_Comm_rank(int, int *);
int MPI_Barrier(int);
int MPI_Reduce(void* , void*, int, int, int, int, int);
int MPI_Gather(void* , int, int, void*, int, int, int, int);
int MPI_Bcast(void*, int, int, int, int);
int MPI_Sendrecv(void *, int, int, int, int, void *, int, int, int, int, int, MPI_Status *);
int MPI_Iprobe(int, int, int, int*, MPI_Status *);
int MPI_Recv(void*, int, int, int, int, int, MPI_Status *);


#endif





#endif /* MYMPI_H_ */
