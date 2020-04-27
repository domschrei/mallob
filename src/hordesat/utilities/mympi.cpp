/*
 * mympi.cpp
 *
 *  Created on: Apr 27, 2016
 *      Author: balyo
 */

#include "mympi.h"
#include <string.h>

#ifndef USEMPI

int MPI_Allgather(void* send, int sendcnt, int , void* recv, int, int, int) {
	memcpy(recv, send, sizeof(int)*sendcnt);
	return 0;
}
int MPI_Irecv(void*, int, int, int, int, int, MPI_Request *) { return 0; }
int MPI_Test(MPI_Request *, int *, int *) { return 0; }
int MPI_Isend(void*, int, int, int, int, int, MPI_Request *) { return 0; }
int MPI_Init(int *, char ***) { return 0; }
int MPI_Finalize(void) { return 0; }
int MPI_Comm_size(int, int *size) { *size = 1; return 0; }
int MPI_Comm_rank(int, int *rank) { *rank = 0; return 0; }
int MPI_Barrier(int) { return 0; }
int MPI_Reduce(void* , void*, int, int, int, int, int) { return 0; }
int MPI_Gather(void* , int, int, void*, int, int, int, int) { return 0; }
int MPI_Bcast(void*, int, int, int, int) { return 0; }
int MPI_Sendrecv(void *, int, int, int, int, void *, int, int, int, int, int, MPI_Status *) { return 0; }
int MPI_Iprobe(int, int, int, int*, MPI_Status *) { return 0; }
int MPI_Recv(void*, int, int, int, int, int, MPI_Status *) { return 0; }

#endif
